// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DCS detection (issue #523), driven through the pure analog receive core in sample time.
 *
 * Every signal is built the way a receiver hears it: the transmitter's NRZ word (bit 0 first,
 * a one as positive deviation in normal polarity), through the receiver's de-emphasis (75 us, or
 * the 750 us land-mobile option) and the demodulator's DC block (dc += (x - dc) / 2^11, as
 * demod_pipeline.cpp runs it), plus white noise at an in-band (0-290 Hz) signal-to-noise ratio,
 * all from seeded generators so a failure reproduces exactly. Valid detection and rejection
 * both carry positive assertions: the right code, under its canonical name, locks within its
 * bound, and the wrong answers -- another code, random bits, code words of no supported code,
 * every CTCSS tone, speech -- never lock at all.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "analog_rx_internal.h"
#include "analog_tone_synth.h"

/* The timing pins under test (docs/cli.md "Received code"), from the DCS timing contract in
   <dsd-neo/dsp/analog_rx.h>, and stronger than its per-event ceilings: every stop is held to
   the loss p95 target on its own, every start at 10 dB to the 10 dB lock bound, and at 3 dB
   every row through the demodulator's DC block meets the lock p95 target and every start on
   these fixed seeds locks within 700 ms. PCM input through a sound card's coupling, which the
   contract does not cover at 3 dB, has pins of its own. */
enum {
    LOCK_10DB_MS = DSD_ANALOG_DCS_LOCK_MS,
    LOCK_3DB_P95_MS = DSD_ANALOG_DCS_LOCK_P95_MS,
    LOCK_3DB_SEEDED_MS = 700,
    LOSS_MS = DSD_ANALOG_DCS_LOSS_P95_MS,
    TURNOFF_LOSS_MS = DSD_ANALOG_DCS_TURNOFF_LOSS_P95_MS,
    /* One 23-bit word at 134.4 bit/s, rounded up. */
    WORD_MS = 172,
    /* The span a lock survives without holding, DSD_ANALOG_DCS_SPAN_BITS at 134.4 bit/s, rounded
       up (477 ms). */
    SPAN_MS = ((DSD_ANALOG_DCS_SPAN_BITS * 10000) + 1343) / 1344,
    /* Two words, 46 bits at 134.4 bit/s, is 342 ms: no lock read from scratch comes sooner. */
    RESET_QUIET_MS = 330,
    /* A sound card's AC coupling on PCM input (docs/cli.md "Received code"), outside the timing
       contract at 3 dB: its own p95 pin there, above the long-run sweeps' p95 at 48 kHz with
       75 us (483 ms), and a per-start pin on the fixed seeds more than a word (172 ms) above
       their slowest (683 ms). */
    COUPLING_HZ = 10,
    LOCK_COUPLED_3DB_P95_MS = 550,
    LOCK_COUPLED_3DB_SEEDED_MS = 1000,
};

_Static_assert((int)LOCK_3DB_P95_MS <= (int)LOCK_3DB_SEEDED_MS
                   && (int)LOCK_3DB_SEEDED_MS <= (int)DSD_ANALOG_DCS_LOCK_CEILING_MS,
               "the fixed seeds sit between the 3 dB target and the ceiling");
_Static_assert((int)LOCK_10DB_MS <= (int)DSD_ANALOG_DCS_LOCK_CEILING_MS, "the ceiling covers 10 dB as well");
_Static_assert((int)LOCK_COUPLED_3DB_P95_MS <= (int)LOCK_COUPLED_3DB_SEEDED_MS,
               "the coupled p95 pin sits under its per-start pin");
_Static_assert((int)LOSS_MS <= (int)DSD_ANALOG_DCS_LOSS_CEILING_MS, "a loss target sits under its ceiling");
_Static_assert((int)TURNOFF_LOSS_MS <= (int)DSD_ANALOG_DCS_TURNOFF_LOSS_CEILING_MS,
               "a loss target sits under its ceiling");

/* The level of a DCS signal: the NRZ amplitude (a 600 Hz deviation against a 0.1-per-kHz
   scale, as the CTCSS tests use for a tone). */
#define DCS_AMP 0.1

/* De-emphasis time constants the receiver may run: the NFM default and the land-mobile one. */
static const double k_deemph_us[] = {75.0, 750.0};

static const int k_rates[] = {8000, 44100, 48000, 78125};
#define RATE_COUNT ((int)(sizeof(k_rates) / sizeof(k_rates[0])))

/* One observation of the core after a block. */
typedef struct {
    int state;
    int kind;
    int code;
    int inverted;
    int carrier;
} observation;

static observation
observe(const dsd_analog_rx_core* core) {
    dsd_analog_rx_publication pub;
    dsd_analog_rx_core_publish(core, &pub);
    observation o = {pub.tone_state, pub.tone_kind, -1, -1, pub.carrier_open};
    if (pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && pub.tone_kind == DSD_ANALOG_TONE_KIND_DCS) {
        assert(dsd_dcs_code_index(pub.dcs_code) >= 0);
        assert(pub.dcs_inverted == 0 || pub.dcs_inverted == 1);
        assert(pub.ctcss_tenths_hz == 0);
        o.code = pub.dcs_code;
        o.inverted = pub.dcs_inverted;
    } else {
        assert(pub.dcs_code == 0 && pub.dcs_inverted == 0);
    }
    /* Detection never gates audio. */
    assert(pub.gate == DSD_ANALOG_TONE_GATE_OFF);
    return o;
}

/* ------------------------------------------------------------------------------------------
 * Signal source
 * ---------------------------------------------------------------------------------------- */

/** @brief What the transmitter sends below the voice band. */
typedef enum {
    SEND_NOTHING = 0,
    SEND_WORD = 1,    /**< the word, repeated */
    SEND_TURNOFF = 2, /**< the 134.4 Hz turn-off tone */
    SEND_TONE = 3,    /**< a sine at tone_hz (a CTCSS tone) */
    SEND_RANDOM = 4,  /**< random bits at 134.4 bit/s */
} send_kind;

typedef struct {
    double fs;
    synth_rng rng;
    uint32_t word; /**< the word sent (already complemented for inverted polarity) */
    double bit_phase;
    double baud;
    int random_bit;
    synth_tone tone;
    double tone_hz;
    double amp;
    int64_t on;          /**< first sample of signalling */
    int64_t switch_at;   /**< signalling becomes then_send here (INT64_MAX = never) */
    send_kind send;      /**< from on */
    send_kind then_send; /**< from switch_at */
    int64_t carrier_off; /**< digital silence from here (INT64_MAX = never) */
    int64_t carrier_on;  /**< ... until here, when it ends (INT64_MAX = never) */
    /** From here the carrier is up for only the first flicker_open samples of every
        flicker_period, digital silence between (INT64_MAX = never). */
    int64_t flicker_from;
    int64_t flicker_period;
    int64_t flicker_open;
    /* The transmitter starts its word over at another place from rephase_at (INT64_MAX =
       never): bit_phase jumps to rephase_to, a word position in bits. */
    int64_t rephase_at;
    double rephase_to;
    /* One bit error per word: flip word bit flip_bit (flip_step moves it on each repetition),
       and a second one at flip2_bit when flip2 is set. With flip_every above 1 only the last word
       of every flip_every carries them, counting words from the first sent (words_sent). */
    int flip;
    int flip_bit;
    int flip_step;
    int flip2;
    int flip2_bit;
    int64_t flip_from;
    int flip_every;
    int64_t words_sent;
    /* Receiver model. */
    /** A DC level on the discriminator's output from @c on: a carrier off frequency (the
        transmitter's error or the receiver's) steps it in with the carrier. */
    double offset;
    double deemph_alpha; /**< one-pole de-emphasis; 1 = off */
    double deemph_y;
    double dc; /**< the DC block's state */
    /** The DC block's step, dc += (x - dc) * dc_k: the demodulator's 1/2048 by default, or a
        sound card's AC coupling on PCM input (src_couple()). */
    double dc_k;
    int dc_block;
    double noise_sigma;
    double scale;
    synth_speech speech;
    synth_voice_hpf voice_hpf;
    double voice_gain;
    int voice_filtered;
    /* A steady sine added on top of whatever is sent from hum_from until hum_to (hum.amp 0 =
       none): an interferer or a voice holding its pitch near the turn-off tone, or a CTCSS
       tone. */
    synth_tone hum;
    int64_t hum_from;
    int64_t hum_to;
} dcs_src;

static int64_t
ms_to_samples(double fs, double ms) {
    return (int64_t)llround(fs * ms / 1000.0);
}

static double
samples_to_ms(double fs, int64_t samples) {
    return (double)samples * 1000.0 / fs;
}

/* White-noise sigma that puts @p snr_db of signal-to-noise in the 0-290 Hz band against an NRZ
   signal of amplitude @p amp (power amp^2). */
static double
dcs_noise_sigma(double amp, double snr_db, double fs) {
    const double inband_noise = (amp * amp) / pow(10.0, snr_db / 10.0);
    return sqrt(inband_noise * (fs / 2.0) / 290.0);
}

static void
src_init(dcs_src* src, double fs, uint64_t seed, int code, int inverted, double snr_db, double deemph_us) {
    DSD_MEMSET(src, 0, sizeof(*src));
    src->fs = fs;
    synth_rng_seed(&src->rng, seed);
    src->word = code >= 0 ? dsd_dcs_word(code, inverted) : 0U;
    src->bit_phase = synth_range(&src->rng, 0.0, (double)DSD_DCS_WORD_BITS);
    src->baud = SYNTH_DCS_BAUD;
    src->amp = DCS_AMP;
    src->tone.fs = fs;
    src->tone.amp = DCS_AMP;
    src->tone.phase = synth_range(&src->rng, 0.0, 2.0 * M_PI);
    src->hum.fs = fs; /* no draw from the generator: the other signals stay as they were */
    src->hum_from = INT64_MAX;
    src->hum_to = INT64_MAX;
    src->on = 0;
    src->switch_at = INT64_MAX;
    src->send = code >= 0 ? SEND_WORD : SEND_NOTHING;
    src->then_send = SEND_NOTHING;
    src->carrier_off = INT64_MAX;
    src->carrier_on = INT64_MAX;
    src->flicker_from = INT64_MAX;
    src->flicker_period = 1;
    src->flicker_open = 1;
    src->rephase_at = INT64_MAX;
    src->flip_from = INT64_MAX;
    src->deemph_alpha = deemph_us > 0.0 ? 1.0 - exp(-1.0 / (fs * deemph_us * 1e-6)) : 1.0;
    src->dc_k = 1.0 / 2048.0;
    src->dc_block = 1;
    src->noise_sigma = snr_db < 100.0 ? dcs_noise_sigma(DCS_AMP, snr_db, fs) : 0.0;
    src->scale = 1.0;
}

/* PCM input: audio a receiver demodulated and de-emphasised, through a sound card whose AC
   coupling is a one-pole high-pass at @p corner_hz, in place of the demodulator's DC block; or,
   with @p corner_hz 0, a DC-coupled input with no DC removal ahead of the front end at all. */
static void
src_couple(dcs_src* src, double corner_hz) {
    if (corner_hz <= 0.0) {
        src->dc_block = 0;
        return;
    }
    src->dc_k = 1.0 - exp(-2.0 * M_PI * corner_hz / src->fs);
}

/* The NRZ level of the word bit the transmitter is sending, then the clock moves on. */
static double
src_word_level(dcs_src* src, int64_t n) {
    const int bit = (int)src->bit_phase;
    uint32_t word = src->word;
    if (src->flip && n >= src->flip_from
        && (src->flip_every <= 1 || (src->words_sent % src->flip_every) == src->flip_every - 1)) {
        word ^= 1U << src->flip_bit;
        if (src->flip2) {
            word ^= 1U << src->flip2_bit;
        }
    }
    const double v = ((word >> bit) & 1U) ? src->amp : -src->amp;
    src->bit_phase += src->baud / src->fs;
    if (src->bit_phase >= (double)DSD_DCS_WORD_BITS) {
        src->bit_phase -= (double)DSD_DCS_WORD_BITS;
        src->words_sent++;
        if (src->flip) {
            src->flip_bit = (src->flip_bit + src->flip_step) % DSD_DCS_WORD_BITS;
        }
    }
    return v;
}

static double
src_random_level(dcs_src* src) {
    const int bit_before = (int)src->bit_phase;
    src->bit_phase += src->baud / src->fs;
    const int bit_after = (int)src->bit_phase;
    if (bit_after != bit_before) {
        src->random_bit = (int)(synth_rng_next(&src->rng) >> 63);
    }
    if (src->bit_phase >= (double)DSD_DCS_WORD_BITS) {
        src->bit_phase -= (double)DSD_DCS_WORD_BITS;
    }
    return src->random_bit ? src->amp : -src->amp;
}

static double
src_signalling(dcs_src* src, int64_t n) {
    if (n < src->on) {
        return 0.0;
    }
    const send_kind kind = n >= src->switch_at ? src->then_send : src->send;
    switch (kind) {
        case SEND_WORD: return src_word_level(src, n);
        case SEND_TURNOFF: src->tone.hz = SYNTH_DCS_BAUD; return synth_tone_next(&src->tone);
        case SEND_TONE: src->tone.hz = src->tone_hz; return synth_tone_next(&src->tone);
        case SEND_RANDOM: return src_random_level(src);
        default: return 0.0;
    }
}

static float
src_next(dcs_src* src, int64_t n) {
    if (n == src->rephase_at) {
        src->bit_phase = src->rephase_to;
    }
    double v = src_signalling(src, n);
    if (n >= src->on) {
        v += src->offset;
    }
    if (src->voice_gain > 0.0) {
        float voice = synth_speech_next(&src->speech);
        if (src->voice_filtered) {
            voice = synth_voice_hpf_next(&src->voice_hpf, voice);
        }
        v += src->voice_gain * voice;
    }
    if (src->hum.amp > 0.0 && n >= src->hum_from && n < src->hum_to) {
        v += synth_tone_next(&src->hum);
    }
    /* The receiver: de-emphasis, then the demodulator's DC block, then the noise. */
    src->deemph_y += (v - src->deemph_y) * src->deemph_alpha;
    double y = src->deemph_y;
    if (src->dc_block) {
        src->dc += (y - src->dc) * src->dc_k;
        y -= src->dc;
    }
    if (src->noise_sigma > 0.0) {
        y += src->noise_sigma * synth_gauss(&src->rng);
    }
    if (n >= src->carrier_off && n < src->carrier_on) {
        return 0.0f;
    }
    if (n >= src->flicker_from && ((n - src->flicker_from) % src->flicker_period) >= src->flicker_open) {
        return 0.0f;
    }
    return (float)(y * src->scale);
}

/* ------------------------------------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------------------------------- */

/* What a run saw, in samples from the start; -1 = never. */
typedef struct {
    int64_t first_lock;     /**< first block end at which the expected code was locked */
    int64_t first_wrong;    /**< first block end at which any other code (or a CTCSS tone) was locked */
    int64_t first_unlocked; /**< first block end after first_lock without the lock */
    int64_t first_none;     /**< first block end at which the verdict read NONE */
    int64_t first_closed;   /**< first block end at which the carrier read closed */
    int64_t dcs_locks;      /**< transitions of the DCS detector into LOCKED */
    int final_state;
} run_result;

static dsd_analog_rx_core g_core;

/* Feed @p total samples in blocks of @p block, observing after each. @p expect_code and
   @p expect_inverted name the code that may lock (-1: none may). */
static run_result
run_signal(dcs_src* src, int64_t total, int block, int expect_code, int expect_inverted) {
    run_result r = {-1, -1, -1, -1, -1, 0, 0};
    float buf[4096];
    assert(block > 0 && block <= (int)(sizeof(buf) / sizeof(buf[0])));
    int prev_dcs_locked = 0;
    for (int64_t n = 0; n < total; n += block) {
        const int m = (total - n) < block ? (int)(total - n) : block;
        for (int i = 0; i < m; i++) {
            buf[i] = src_next(src, n + i);
        }
        assert(dsd_analog_rx_core_process(&g_core, buf, m, (int)src->fs, 1) == 1);
        const observation o = observe(&g_core);
        const int dcs_locked = g_core.dcs.state == DSD_ANALOG_TONE_STATE_LOCKED;
        if (dcs_locked && !prev_dcs_locked) {
            r.dcs_locks++;
        }
        prev_dcs_locked = dcs_locked;
        const int locked = o.state == DSD_ANALOG_TONE_STATE_LOCKED;
        const int right =
            locked && o.kind == DSD_ANALOG_TONE_KIND_DCS && o.code == expect_code && o.inverted == expect_inverted;
        if (right && r.first_lock < 0) {
            r.first_lock = n + m;
        }
        if (locked && !right && r.first_wrong < 0) {
            r.first_wrong = n + m;
        }
        if (!right && r.first_lock >= 0 && r.first_unlocked < 0) {
            r.first_unlocked = n + m;
        }
        if (o.state == DSD_ANALOG_TONE_STATE_NONE && r.first_none < 0) {
            r.first_none = n + m;
        }
        if (!o.carrier && r.first_closed < 0) {
            r.first_closed = n + m;
        }
        r.final_state = o.state;
    }
    return r;
}

/* The name the detector must give @p code sent in @p inverted polarity. */
static void
expected_name(int code, int inverted, int* out_code, int* out_inverted) {
    assert(dsd_dcs_canonical(code, inverted, out_code, out_inverted) == 0);
}

/** @brief How the receiver hands the code to the detector, beyond the noise and de-emphasis. */
typedef struct {
    /** 1: PCM input through a sound card's coupling at couple_hz (0 Hz: DC-coupled) in place of
        the demodulator's DC block (src_couple()). */
    int pcm;
    double couple_hz;
    double offset; /**< DC step at the code's onset (dcs_src::offset) */
} receive_path;

/* Lock time, from the onset of the word, of @p code in @p inverted polarity, through the
   demodulator's DC block or the PCM path @p path names (NULL: the demodulator's DC block, no
   offset). Asserts nothing else ever locked; returns -1 if it never locked within @p watch_ms. */
static double
lock_time_through_ms(double fs, int code, int inverted, double snr_db, double deemph_us, const receive_path* path,
                     uint64_t seed, double onset_ms, double watch_ms) {
    dsd_analog_rx_core_init(&g_core);
    dcs_src src;
    src_init(&src, fs, seed, code, inverted, snr_db, deemph_us);
    if (path && path->pcm) {
        src_couple(&src, path->couple_hz);
    }
    if (path) {
        src.offset = path->offset;
    }
    src.on = ms_to_samples(fs, onset_ms);
    int want_code = -1;
    int want_inverted = -1;
    expected_name(code, inverted, &want_code, &want_inverted);
    const int block = (int)fs / 1000 > 0 ? (int)fs / 1000 : 1;
    const run_result r = run_signal(&src, src.on + ms_to_samples(fs, watch_ms), block, want_code, want_inverted);
    if (r.first_wrong >= 0) {
        DSD_FPRINTF(stderr, "wrong lock: fs=%.0f code=%03o%c snr=%.0f seed=%llu\n", fs, (unsigned int)code,
                    inverted ? 'I' : 'N', snr_db, (unsigned long long)seed);
    }
    assert(r.first_wrong < 0);
    if (r.first_lock < 0) {
        return -1.0;
    }
    return samples_to_ms(fs, r.first_lock - src.on);
}

/* Lock time through the demodulator's DC block (lock_time_through_ms()). */
static double
lock_time_ms(double fs, int code, int inverted, double snr_db, double deemph_us, uint64_t seed, double onset_ms,
             double watch_ms) {
    return lock_time_through_ms(fs, code, inverted, snr_db, deemph_us, NULL, seed, onset_ms, watch_ms);
}

static int
compare_doubles(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

/* Print p50/p95/worst of @p count lock or loss times for the evidence, then assert every one
   within @p bound_ms and, when @p p95_ms is positive, the p95 within it. A time of -1 (never
   locked) sorts first and fails. */
static void
check_bound(const char* what, double* times, int count, int bound_ms, int p95_ms) {
    assert(count > 0);
    qsort(times, (size_t)count, sizeof(times[0]), compare_doubles);
    const double p95 = times[(count * 95) / 100];
    printf("%s: p50 %.0f ms, p95 %.0f ms, worst %.0f ms (%d cases; each <= %d ms)\n", what, times[count / 2], p95,
           times[count - 1], count, bound_ms);
    (void)fflush(stdout);
    if (times[0] < 0.0 || times[count - 1] > (double)bound_ms || (p95_ms > 0 && p95 > (double)p95_ms)) {
        DSD_FPRINTF(stderr, "lock bound broken: %s (fastest %.0f, p95 %.0f, slowest %.0f)\n", what, times[0], p95,
                    times[count - 1]);
    }
    assert(times[0] >= 0.0);
    assert(times[count - 1] <= (double)bound_ms);
    assert(p95_ms <= 0 || p95 <= (double)p95_ms);
}

/* ------------------------------------------------------------------------------------------
 * Lock
 * ---------------------------------------------------------------------------------------- */

/*
 * Every supported code in both polarities, through the demodulator's DC block with 75 and
 * 750 us de-emphasis, at 48 kHz: within 520 ms of its onset at 10 dB in-band and within 700 ms
 * at 3 dB, where each row also meets the 3 dB p95 target, under its canonical name. The onset
 * lands anywhere in a word and in a bit.
 */
static void
test_every_code_locks_within_bound(void) {
    static double times[2 * DSD_DCS_CODE_COUNT];
    static const double snrs[] = {10.0, 3.0};
    static const int bounds[] = {LOCK_10DB_MS, LOCK_3DB_SEEDED_MS};
    static const int p95s[] = {0, LOCK_3DB_P95_MS};
    for (int di = 0; di < 2; di++) {
        for (int si = 0; si < 2; si++) {
            int count = 0;
            for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
                for (int inverted = 0; inverted < 2; inverted++) {
                    const uint64_t seed = 523000ULL + (uint64_t)(i * 8 + inverted * 4 + di * 2 + si);
                    times[count++] = lock_time_ms(48000.0, dsd_dcs_code(i), inverted, snrs[si], k_deemph_us[di], seed,
                                                  300.0 + (double)(i % 7), (double)bounds[si] + 150.0);
                }
            }
            char what[96];
            DSD_SNPRINTF(what, sizeof(what), "lock at 48 kHz, %.0f dB in-band, %.0f us de-emphasis", snrs[si],
                         k_deemph_us[di]);
            check_bound(what, times, count, bounds[si], p95s[si]);
        }
    }
}

/* The other rates: 8 kHz (the DC block barely sags), 44.1 kHz and the 78.125 kHz forced rate
   (where it sags the most), at both SNRs, in both polarities: every eighth code at 10 dB, and
   every second code at 3 dB, 104 starts a row, so that the row's p95 is its sixth slowest start
   rather than its second. The de-emphasis alternates every eighth code. */
static void
test_codes_lock_at_every_rate(void) {
    static double times[DSD_DCS_CODE_COUNT];
    static const double snrs[] = {10.0, 3.0};
    static const int bounds[] = {LOCK_10DB_MS, LOCK_3DB_SEEDED_MS};
    static const int p95s[] = {0, LOCK_3DB_P95_MS};
    static const int code_steps[] = {8, 2};
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        for (int si = 0; si < 2; si++) {
            int count = 0;
            for (int i = 0; i < DSD_DCS_CODE_COUNT; i += code_steps[si]) {
                for (int inverted = 0; inverted < 2; inverted++) {
                    const uint64_t seed = 5231000ULL + (uint64_t)(ri * 1000 + i * 4 + inverted * 2 + si);
                    times[count++] =
                        lock_time_ms((double)k_rates[ri], dsd_dcs_code(i), inverted, snrs[si], k_deemph_us[(i / 8) % 2],
                                     seed, 250.0 + (double)(i % 5), (double)bounds[si] + 150.0);
                }
            }
            char what[64];
            DSD_SNPRINTF(what, sizeof(what), "lock at %d Hz, %.0f dB in-band", k_rates[ri], snrs[si]);
            check_bound(what, times, count, bounds[si], p95s[si]);
        }
    }
}

/*
 * PCM input through a sound card: audio a receiver demodulated and de-emphasised (75 us), at
 * 48 kHz, through an AC coupling with a COUPLING_HZ corner in place of the demodulator's DC
 * block. It sags a run of equal bits by 0.63 a bit, which the slicers' 0.55 hypothesis covers,
 * about three times as much per bit as the demodulator's block does at 48 kHz. Every second code
 * in both polarities locks within the 10 dB bound at 10 dB. At 3 dB the sag costs more bits
 * than the demodulator's block does, and the timing contract does not cover it: every code in
 * both polarities is held to the row's own pins, LOCK_COUPLED_3DB_P95_MS and
 * LOCK_COUPLED_3DB_SEEDED_MS.
 */
static void
test_codes_lock_through_a_sound_card_coupling(void) {
    static double times[2 * DSD_DCS_CODE_COUNT];
    static const double snrs[] = {10.0, 3.0};
    static const int bounds[] = {LOCK_10DB_MS, LOCK_COUPLED_3DB_SEEDED_MS};
    static const int p95s[] = {0, LOCK_COUPLED_3DB_P95_MS};
    static const int code_steps[] = {2, 1};
    const receive_path coupling = {1, (double)COUPLING_HZ, 0.0};
    for (int si = 0; si < 2; si++) {
        int count = 0;
        for (int i = 0; i < DSD_DCS_CODE_COUNT; i += code_steps[si]) {
            for (int inverted = 0; inverted < 2; inverted++) {
                const uint64_t seed = 524000ULL + (uint64_t)(i * 8 + inverted * 4 + si);
                times[count++] = lock_time_through_ms(48000.0, dsd_dcs_code(i), inverted, snrs[si], 75.0, &coupling,
                                                      seed, 300.0 + (double)(i % 7), (double)bounds[si] + 150.0);
            }
        }
        char what[96];
        DSD_SNPRINTF(what, sizeof(what), "lock through a %d Hz sound card coupling at 48 kHz, %.0f dB in-band",
                     (int)COUPLING_HZ, snrs[si]);
        check_bound(what, times, count, bounds[si], p95s[si]);
    }
}

/*
 * A carrier off frequency (the transmitter's error or the receiver's) reads as a DC level on the
 * discriminator's output, which steps in with the carrier. A DC-coupled PCM input keeps all of
 * it: no DC removal ahead of the front end, whose 10 Hz blocker the detector undoes, so the
 * re-poled stream carries the step for about 0.3 s per 1/e, and a step larger than the code held
 * every droop slicer on one polarity well past the 10 dB bound (at twice the code's level, 19,990
 * of 20,000 starts took longer than 520 ms, half of them longer than 617 ms). The balance slicer
 * slices each word against its own mean, so a level that holds over a word costs it nothing. At
 * 10 dB, every second code in both polarities at 48 kHz locks within the 10 dB bound through a
 * DC-coupled input with a step at its onset of 1, 2 and 4 times its own level, up or down; so
 * does the RTL path at 8 kHz, where the demodulator's DC block (0.6 Hz) lets a step linger
 * longest, with a step of 4 times. At 3 dB, every code in both polarities through a DC-coupled
 * input with a 4 times step meets the 3 dB p95 target, and every start the per-start pin of these
 * seeds.
 */
static void
test_codes_lock_through_a_frequency_offset(void) {
    static double times[3 * DSD_DCS_CODE_COUNT];
    static const double steps[] = {1.0, 2.0, 4.0};
    int count = 0;
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < DSD_DCS_CODE_COUNT; i += 2) {
            for (int inverted = 0; inverted < 2; inverted++) {
                const double sign = ((i / 2) % 2) ? -1.0 : 1.0;
                const receive_path dc_coupled = {1, 0.0, sign * steps[k] * DCS_AMP};
                const uint64_t seed = 525000ULL + (uint64_t)(k * 1000 + i * 4 + inverted);
                times[count++] = lock_time_through_ms(48000.0, dsd_dcs_code(i), inverted, 10.0, 75.0, &dc_coupled, seed,
                                                      300.0 + (double)(i % 7), (double)LOCK_10DB_MS + 150.0);
            }
        }
    }
    check_bound("lock through a DC-coupled input, a 1-4x step at the onset, 48 kHz, 10 dB in-band", times, count,
                LOCK_10DB_MS, 0);

    count = 0;
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i += 2) {
        for (int inverted = 0; inverted < 2; inverted++) {
            const double sign = ((i / 2) % 2) ? -1.0 : 1.0;
            const receive_path rtl = {0, 0.0, sign * 4.0 * DCS_AMP};
            const uint64_t seed = 526000ULL + (uint64_t)(i * 4 + inverted);
            times[count++] = lock_time_through_ms(8000.0, dsd_dcs_code(i), inverted, 10.0, k_deemph_us[(i / 8) % 2],
                                                  &rtl, seed, 250.0 + (double)(i % 5), (double)LOCK_10DB_MS + 150.0);
        }
    }
    check_bound("lock through the demodulator's DC block, a 4x step at the onset, 8 kHz, 10 dB in-band", times, count,
                LOCK_10DB_MS, 0);

    count = 0;
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        for (int inverted = 0; inverted < 2; inverted++) {
            const double sign = (i % 2) ? -1.0 : 1.0;
            const receive_path dc_coupled = {1, 0.0, sign * 4.0 * DCS_AMP};
            const uint64_t seed = 527000ULL + (uint64_t)(i * 4 + inverted);
            times[count++] = lock_time_through_ms(48000.0, dsd_dcs_code(i), inverted, 3.0, 75.0, &dc_coupled, seed,
                                                  300.0 + (double)(i % 7), (double)LOCK_3DB_SEEDED_MS + 150.0);
        }
    }
    check_bound("lock through a DC-coupled input, a 4x step at the onset, 48 kHz, 3 dB in-band", times, count,
                LOCK_3DB_SEEDED_MS, LOCK_3DB_P95_MS);
}

/*
 * Aliases: an inverted code is named by its normal alias, the way dsd_dcs_canonical() and the
 * golden table in RUNTIME_ANALOG_TONES name it, and a normal code by itself. Sending D023I must
 * read D047N and never D023N, which a detector that ignored polarity would report.
 */
static void
test_aliases_through_the_detector(void) {
    static const int k_pins[][3] = {
        /* sent code, sent inverted, read code (always normal) */
        {0023, 0, 0023}, {0023, 1, 0047}, {0047, 1, 0023}, {0047, 0, 0047},
        {0754, 1, 0116}, {0116, 1, 0754}, {0565, 1, 0703}, {0624, 1, 0632},
    };
    for (size_t k = 0; k < sizeof(k_pins) / sizeof(k_pins[0]); k++) {
        int code = -1;
        int inverted = -1;
        expected_name(k_pins[k][0], k_pins[k][1], &code, &inverted);
        assert(code == k_pins[k][2] && inverted == 0);
        const double t = lock_time_ms(48000.0, k_pins[k][0], k_pins[k][1], 200.0, 75.0, 777ULL + k, 200.0, 700.0);
        assert(t >= 0.0 && t <= (double)LOCK_10DB_MS);
        assert(g_core.dcs.code == k_pins[k][2] && g_core.dcs.inverted == 0);
    }
}

/* The bit clock follows a transmitter a little off 134.4 bit/s: some radios send 134.3. */
static void
test_off_rate_transmitter_locks(void) {
    static const double bauds[] = {134.3, 134.5};
    for (int b = 0; b < 2; b++) {
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 48000.0, 99ULL + (uint64_t)b, 0023, 0, 10.0, 75.0);
        src.baud = bauds[b];
        src.on = ms_to_samples(48000.0, 200.0);
        const run_result r = run_signal(&src, ms_to_samples(48000.0, 8000.0), 480, 0023, 0);
        assert(r.first_wrong < 0 && r.first_lock >= 0);
        assert(samples_to_ms(48000.0, r.first_lock - src.on) <= (double)LOCK_10DB_MS);
        /* Held for the rest of the 8 s: the clock tracks the rate, no slip loses it. */
        assert(r.first_unlocked < 0);
    }
}

/* ------------------------------------------------------------------------------------------
 * Hold and loss
 * ---------------------------------------------------------------------------------------- */

/*
 * One bit error in every word holds the lock, wherever it falls: always in the same bit, or
 * moving one bit on each word (so that once in 23 words a window holds two). Nothing ever
 * locks in its place either.
 */
static void
test_one_bit_error_per_word_holds(void) {
    static const int steps[] = {0, 1, 5};
    for (int s = 0; s < 3; s++) {
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 48000.0, 424242ULL + (uint64_t)s, 0245, 0, 20.0, 75.0);
        src.bit_phase = 0.0;
        src.flip = 1;
        src.flip_bit = 3;
        src.flip_step = steps[s];
        src.flip_from = ms_to_samples(48000.0, 1000.0);
        const run_result r = run_signal(&src, ms_to_samples(48000.0, 20000.0), 480, 0245, 0);
        assert(r.first_wrong < 0 && r.first_lock >= 0);
        assert(r.first_lock < src.flip_from);
        assert(r.first_unlocked < 0);
        assert(r.dcs_locks == 1);
    }
}

/* Two bit errors in every word are no supported word: they end a lock, and nothing locks in its
   place. A window holds both errors only once it lies wholly after the damage started, up to a
   word later, so the loss bound runs from then. */
static void
test_two_bit_errors_per_word_lose(void) {
    static double times[8];
    for (int k = 0; k < 8; k++) {
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 48000.0, 31337ULL + (uint64_t)k, 0245, k % 2, 20.0, 75.0);
        src.flip = 1;
        src.flip_bit = 3 + k;
        src.flip_step = 0;
        src.flip2 = 1;
        src.flip2_bit = 17;
        src.flip_from = ms_to_samples(48000.0, 1000.0 + (double)(k * 11));
        int code = -1;
        int inverted = -1;
        expected_name(0245, k % 2, &code, &inverted);
        const run_result r = run_signal(&src, ms_to_samples(48000.0, 4000.0), 48, code, inverted);
        assert(r.first_wrong < 0 && r.first_lock >= 0 && r.first_lock < src.flip_from);
        assert(r.first_unlocked > src.flip_from && r.dcs_locks == 1);
        assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
        times[k] = samples_to_ms(48000.0, r.first_unlocked - src.flip_from);
    }
    check_bound("loss when two bits of every word go wrong", times, 8, LOSS_MS + WORD_MS, 0);
}

/* The word stops under a live carrier, with no turn-off tone: lost within the loss bound, and
   the verdict is "none" (32 bits and the front end's delay). */
static void
test_code_stop_under_carrier_loses(void) {
    static double times[16];
    int count = 0;
    for (int k = 0; k < 16; k++) {
        const double snr = (k % 2) ? 3.0 : 10.0;
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 48000.0, 606ULL + (uint64_t)k, dsd_dcs_code(k * 6), k % 3 == 0, snr, 75.0);
        src.on = 0;
        src.switch_at = ms_to_samples(48000.0, 1500.0 + (double)(k * 3));
        src.then_send = SEND_NOTHING;
        int code = -1;
        int inverted = -1;
        expected_name(dsd_dcs_code(k * 6), k % 3 == 0, &code, &inverted);
        const run_result r = run_signal(&src, ms_to_samples(48000.0, 2600.0), 48, code, inverted);
        assert(r.first_wrong < 0 && r.first_lock >= 0 && r.first_lock < src.switch_at);
        assert(r.first_unlocked > src.switch_at);
        assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
        times[count++] = samples_to_ms(48000.0, r.first_unlocked - src.switch_at);
    }
    check_bound("loss when the code stops under a live carrier", times, count, LOSS_MS, 0);
}

/* The 134.4 Hz turn-off tone ends a lock at once: within the turn-off bound of its start, well
   before the 32-bit loss, and nothing locks while it lasts. */
static void
test_turnoff_tone_loses_fast(void) {
    static double times[16];
    int count = 0;
    for (int k = 0; k < 16; k++) {
        const double snr = (k % 2) ? 3.0 : 10.0;
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 48000.0, 707ULL + (uint64_t)k, dsd_dcs_code(k * 6 + 1), k % 3 == 1, snr,
                 k % 4 == 0 ? 750.0 : 75.0);
        src.switch_at = ms_to_samples(48000.0, 1500.0 + (double)(k * 5));
        src.then_send = SEND_TURNOFF;
        int code = -1;
        int inverted = -1;
        expected_name(dsd_dcs_code(k * 6 + 1), k % 3 == 1, &code, &inverted);
        const run_result r = run_signal(&src, ms_to_samples(48000.0, 1900.0), 48, code, inverted);
        assert(r.first_wrong < 0 && r.first_lock >= 0 && r.first_lock < src.switch_at);
        assert(r.first_unlocked > src.switch_at);
        assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
        times[count++] = samples_to_ms(48000.0, r.first_unlocked - src.switch_at);
    }
    check_bound("loss on the turn-off tone", times, count, TURNOFF_LOSS_MS, 0);
}

/*
 * A steady component near the turn-off tone under a code the slicers still read -- an
 * interferer, or a voice holding its pitch -- leaves the lock alone: 130, 134.4 and 140 Hz at
 * the code's power and 3 dB above it, from 2 s into a 6 s transmission. The turn-off tone
 * detector sees it dominate the band, but a lock ends on it only once the code has gone too.
 */
static void
test_steady_component_keeps_the_lock(void) {
    static const double hums[] = {130.0, 134.4, 140.0};
    static const double amps[] = {0.1414, 0.2};
    for (int h = 0; h < 3; h++) {
        for (int a = 0; a < 2; a++) {
            const int code = dsd_dcs_code(h * 30 + a * 7 + 5);
            const int sent_inverted = (h + a) % 2;
            dsd_analog_rx_core_init(&g_core);
            dcs_src src;
            src_init(&src, 48000.0, 1340ULL + (uint64_t)(h * 2 + a), code, sent_inverted, 20.0, 75.0);
            src.hum.hz = hums[h];
            src.hum.amp = amps[a];
            src.hum_from = ms_to_samples(48000.0, 2000.0);
            int want_code = -1;
            int want_inverted = -1;
            expected_name(code, sent_inverted, &want_code, &want_inverted);
            const run_result r = run_signal(&src, ms_to_samples(48000.0, 6000.0), 480, want_code, want_inverted);
            assert(r.first_wrong < 0 && r.first_lock >= 0 && r.first_lock < src.hum_from);
            assert(r.first_unlocked < 0 && r.dcs_locks == 1);
        }
    }
}

/*
 * A code outranks a tone: a CTCSS tone that locks under a held code (a voice's talk-off on a
 * coded channel) never replaces the code, and a code that locks under a held tone replaces the
 * tone at once; once the code is lost, a tone still locked shows. Both detectors are really
 * locked here, so the rule, not a missed detection, decides the display.
 */
static void
test_a_code_outranks_a_tone(void) {
    for (int dcs_first = 0; dcs_first < 2; dcs_first++) {
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 48000.0, 2500ULL + (uint64_t)dcs_first, 0754, 0, 20.0, 75.0);
        src.hum.hz = 250.3;
        src.hum.amp = 0.1;
        if (dcs_first) {
            /* The code throughout until 5 s, the tone from 2 s on. */
            src.hum_from = ms_to_samples(48000.0, 2000.0);
            src.switch_at = ms_to_samples(48000.0, 5000.0);
            src.then_send = SEND_NOTHING;
        } else {
            /* The tone until 5 s, the code from 2 s on. */
            src.hum_from = 0;
            src.hum_to = ms_to_samples(48000.0, 5000.0);
            src.on = ms_to_samples(48000.0, 2000.0);
        }
        const int64_t second_starts = ms_to_samples(48000.0, 2000.0);
        const int64_t first_stops = ms_to_samples(48000.0, 5000.0);
        int64_t tone_published = -1;
        int64_t code_published = -1;
        int both_locked = 0;
        int tone_after_code = 0;
        float buf[480];
        for (int64_t n = 0; n < ms_to_samples(48000.0, 7000.0); n += 480) {
            for (int i = 0; i < 480; i++) {
                buf[i] = src_next(&src, n + i);
            }
            assert(dsd_analog_rx_core_process(&g_core, buf, 480, 48000, 1) == 1);
            const int64_t end = n + 480;
            dsd_analog_rx_publication pub;
            dsd_analog_rx_core_publish(&g_core, &pub);
            const int dcs_shown = pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                                  && pub.tone_kind == DSD_ANALOG_TONE_KIND_DCS && pub.dcs_code == 0754;
            const int tone_shown = pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                                   && pub.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS && pub.ctcss_tenths_hz == 2503;
            assert(pub.tone_state != DSD_ANALOG_TONE_STATE_LOCKED || dcs_shown || tone_shown);
            const int code_locked = g_core.dcs.state == DSD_ANALOG_TONE_STATE_LOCKED;
            /* Whenever the code is locked, it is what shows. */
            assert(!code_locked || dcs_shown);
            if (code_locked && g_core.ctcss.state == DSD_ANALOG_TONE_STATE_LOCKED) {
                both_locked = 1;
            }
            if (tone_shown && tone_published < 0) {
                tone_published = end;
            }
            if (dcs_shown && code_published < 0) {
                code_published = end;
            }
            if (tone_shown && code_published >= 0) {
                tone_after_code = 1;
            }
        }
        assert(both_locked);
        if (dcs_first) {
            /* The code from its lock until it stops, then the tone. */
            assert(code_published >= 0 && code_published < second_starts);
            assert(tone_published > first_stops);
        } else {
            /* The tone until the code locks, within the 10 dB bound of its start, and never again
               while the code holds: the code outlasts the tone. */
            assert(tone_published >= 0 && tone_published < second_starts);
            assert(code_published > second_starts
                   && samples_to_ms(48000.0, code_published - second_starts) <= (double)LOCK_10DB_MS);
            assert(!tone_after_code);
        }
    }
}

/*
 * A code's own waveform in noise can read as a CTCSS tone before the code locks: it happened
 * twice in the 7,000,000 starts at 10 dB of an earlier long-run sweep (docs/testing.md), D274N
 * read as 67.0 Hz at 8 kHz and D122N as 77.0 Hz at 78.125 kHz, both with 75 us de-emphasis. The
 * code outranks the tone the moment it locks, so each still shows within the 10 dB bound of its
 * onset; the rule that let the first lock keep the publication showed D122N only after 535 ms,
 * when the tone was lost. Those two starts, replayed exactly (seed, code and onset); both still
 * read the tone first.
 */
static void
test_a_code_read_as_a_tone_first_still_locks_in_time(void) {
    static const struct {
        double fs;
        int code;
        uint64_t seed;
        double onset_ms;
        int tone_tenths;
    } k_starts[] = {
        {8000.0, 0274, 6000125295ULL, 264.407, 670},
        {78125.0, 0122, 6400372048ULL, 164.561, 770},
    };

    for (size_t k = 0; k < sizeof(k_starts) / sizeof(k_starts[0]); k++) {
        const double fs = k_starts[k].fs;
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, fs, k_starts[k].seed, k_starts[k].code, 0, 10.0, 75.0);
        src.on = ms_to_samples(fs, k_starts[k].onset_ms);
        const int block = (int)fs / 1000;
        float buf[128];
        assert(block <= (int)(sizeof(buf) / sizeof(buf[0])));
        int64_t tone_shown = -1;
        int64_t code_shown = -1;
        for (int64_t n = 0; n < src.on + ms_to_samples(fs, (double)LOCK_10DB_MS + 150.0); n += block) {
            for (int i = 0; i < block; i++) {
                buf[i] = src_next(&src, n + i);
            }
            assert(dsd_analog_rx_core_process(&g_core, buf, block, (int)fs, 1) == 1);
            const observation o = observe(&g_core);
            if (o.state != DSD_ANALOG_TONE_STATE_LOCKED) {
                continue;
            }
            if (o.kind == DSD_ANALOG_TONE_KIND_CTCSS) {
                /* Only before the code, and only the tone the sweep saw. */
                assert(code_shown < 0);
                dsd_analog_rx_publication pub;
                dsd_analog_rx_core_publish(&g_core, &pub);
                assert(pub.ctcss_tenths_hz == k_starts[k].tone_tenths);
                tone_shown = tone_shown < 0 ? n + block : tone_shown;
            } else {
                assert(o.code == k_starts[k].code && o.inverted == 0);
                code_shown = code_shown < 0 ? n + block : code_shown;
            }
        }
        /* The tone showed first, as in the sweep: the start still exercises the rule. */
        assert(tone_shown >= 0 && code_shown > tone_shown);
        const double lock_ms = samples_to_ms(fs, code_shown - src.on);
        printf("code read as a %.1f Hz tone first at %.0f Hz: code shown %.0f ms after its onset (<= %d ms)\n",
               (double)k_starts[k].tone_tenths / 10.0, fs, lock_ms, (int)LOCK_10DB_MS);
        assert(lock_ms <= (double)LOCK_10DB_MS);
    }
}

/* The carrier drops with the code still on and no turn-off tone: the core's 200 ms hangover
   forgets it. A dropout shorter than the hangover keeps the lock. */
static void
test_carrier_drop_and_dropout(void) {
    dsd_analog_rx_core_init(&g_core);
    dcs_src src;
    src_init(&src, 48000.0, 808ULL, 0071, 0, 10.0, 75.0);
    src.carrier_off = ms_to_samples(48000.0, 1500.0);
    const run_result r = run_signal(&src, ms_to_samples(48000.0, 2000.0), 480, 0071, 0);
    assert(r.first_lock >= 0 && r.first_lock < src.carrier_off);
    assert(r.first_unlocked > src.carrier_off);
    assert(samples_to_ms(48000.0, r.first_unlocked - src.carrier_off)
           <= (double)DSD_ANALOG_CARRIER_HANGOVER_MS + 10.0 + 1.0);
    assert(observe(&g_core).carrier == 0);

    /* 150 ms of digital silence inside a transmission: the lock survives it. */
    dsd_analog_rx_core_init(&g_core);
    src_init(&src, 48000.0, 809ULL, 0071, 0, 10.0, 75.0);
    const int64_t gap_from = ms_to_samples(48000.0, 1500.0);
    const int64_t gap_to = ms_to_samples(48000.0, 1650.0);
    int held = 1;
    int locked_before = 0;
    float buf[480];
    for (int64_t n = 0; n < ms_to_samples(48000.0, 3000.0); n += 480) {
        for (int i = 0; i < 480; i++) {
            const float v = src_next(&src, n + i);
            buf[i] = (n + i >= gap_from && n + i < gap_to) ? 0.0f : v;
        }
        assert(dsd_analog_rx_core_process(&g_core, buf, 480, 48000, 1) == 1);
        const observation o = observe(&g_core);
        const int locked = o.state == DSD_ANALOG_TONE_STATE_LOCKED && o.code == 0071;
        if (n + 480 <= gap_from) {
            locked_before = locked;
        } else if (!locked) {
            held = 0;
        }
    }
    assert(locked_before == 1);
    assert(held == 1);
}

/*
 * A reset leaves nothing of a held code behind: the carrier hangover running out (a carrier
 * down for 300 ms), and dsd_analog_rx_core_reset() under a running code, which is how the tap
 * starts every new reception (a retune, a stream pause, a new input rate). The detector is no
 * longer locked the moment the reset runs. A carrier that comes back with no code never locks,
 * one that comes back with another code shows that code and never the old one, and the same
 * code running on through the reset shows again only once it has been read twice from scratch
 * (46 bits, 342 ms), not within RESET_QUIET_MS of the reset.
 */
static void
test_a_reset_forgets_the_lock(void) {
    enum { AFTER_NOTHING = 0, AFTER_OTHER_CODE = 1, AFTER_CORE_RESET = 2 };

    const double fs = 48000.0;
    const int64_t hangover = ms_to_samples(fs, (double)DSD_ANALOG_CARRIER_HANGOVER_MS);
    for (int k = 0; k < 3; k++) {
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, fs, 8100ULL + (uint64_t)k, 0071, 0, 10.0, 75.0);
        /* Block-aligned (480 samples): the reset runs between two blocks. */
        const int64_t boundary = ms_to_samples(fs, 1500.0);
        int64_t reset_at = boundary + hangover;
        int64_t fresh_from = boundary;
        if (k != AFTER_CORE_RESET) {
            src.carrier_off = boundary;
            src.carrier_on = boundary + ms_to_samples(fs, 300.0);
            fresh_from = src.carrier_on;
            src.switch_at = boundary;
            src.then_send = (k == AFTER_NOTHING) ? SEND_NOTHING : SEND_WORD;
        } else {
            reset_at = boundary;
        }
        int locked_before = 0;
        int64_t new_lock = -1;
        float buf[480];
        for (int64_t n = 0; n < ms_to_samples(fs, 3000.0); n += 480) {
            if (k == AFTER_OTHER_CODE && n == boundary) {
                src.word = dsd_dcs_word(0754, 0);
            }
            if (k == AFTER_CORE_RESET && n == boundary) {
                dsd_analog_rx_core_reset(&g_core);
                assert(g_core.dcs.state != DSD_ANALOG_TONE_STATE_LOCKED && g_core.dcs.code < 0);
            }
            for (int i = 0; i < 480; i++) {
                buf[i] = src_next(&src, n + i);
            }
            assert(dsd_analog_rx_core_process(&g_core, buf, 480, 48000, 1) == 1);
            const int64_t end = n + 480;
            const observation o = observe(&g_core);
            const int locked = o.state == DSD_ANALOG_TONE_STATE_LOCKED;
            if (end <= boundary) {
                locked_before = locked && o.code == 0071;
                continue;
            }
            if (end < reset_at) {
                continue; /* inside the hangover the held lock may still show */
            }
            if (locked && new_lock < 0) {
                /* A lock read from scratch, of the code now sent. */
                new_lock = end;
                assert(k != AFTER_NOTHING);
                assert(end > fresh_from + ms_to_samples(fs, (double)RESET_QUIET_MS));
                assert(o.code == (k == AFTER_OTHER_CODE ? 0754 : 0071) && o.inverted == 0);
            }
            if (new_lock < 0) {
                /* Nothing of the old lock is left in the detector, whatever the publication. */
                assert(!locked && g_core.dcs.state != DSD_ANALOG_TONE_STATE_LOCKED);
            }
            if (k == AFTER_OTHER_CODE) {
                assert(o.code != 0071);
            }
        }
        assert(locked_before);
        if (k == AFTER_NOTHING) {
            assert(new_lock < 0 && observe(&g_core).state == DSD_ANALOG_TONE_STATE_NONE);
        } else {
            assert(new_lock >= 0 && samples_to_ms(fs, new_lock - fresh_from) <= (double)LOCK_10DB_MS);
        }
    }
}

/*
 * A carrier that keeps dropping out, each time for less than the hangover, never expires, so
 * only the detector can end a lock under it. The code stops, and from then on the carrier is
 * up for 20 ms of every 40-180 ms, digital silence between: no window is ever read wholly with
 * the carrier open again, so the 32-bit loss never runs, and the lock ends on the span since
 * it last held (64 bits), whatever the rate and block size. A window can still hold the code
 * up to a word after it stops, so every stop is lost within the span and a word of it (the
 * loss contract's p95 target and ceiling are for a live carrier). Nothing locks again and the
 * carrier stays open throughout. A span that ran only on bits read with the carrier open would
 * stretch with every dropout (0.9 s on one of these, 1.8 s under random flicker). Prints
 * p50/p95/worst.
 */
static void
test_code_stop_under_a_flickering_carrier(void) {
    static const int periods_ms[] = {40, 60, 100, 140, 180};

    enum { PERIOD_COUNT = (int)(sizeof(periods_ms) / sizeof(periods_ms[0])), OFFSETS = 5 };

    static double times[RATE_COUNT * PERIOD_COUNT * OFFSETS];
    int count = 0;
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        for (int pi = 0; pi < PERIOD_COUNT; pi++) {
            for (int oi = 0; oi < OFFSETS; oi++) {
                const double fs = (double)k_rates[ri];
                const int code = dsd_dcs_code(((ri * 17) + (pi * 7) + (oi * 3)) % DSD_DCS_CODE_COUNT);
                const int sent_inverted = (ri + pi + oi) % 2;
                /* 1 ms blocks, and the 20 ms blocks an RTL stream delivers, alternately. */
                const int block = (oi % 2 == 0) ? k_rates[ri] / 1000 : k_rates[ri] / 50;
                dsd_analog_rx_core_init(&g_core);
                dcs_src src;
                src_init(&src, fs, 5231001ULL + (uint64_t)((ri * 1009) + (pi * 101) + oi), code, sent_inverted, 10.0,
                         (oi == 4) ? 750.0 : 75.0);
                src.switch_at = ms_to_samples(fs, 1500.0 + (double)(oi * 10));
                src.then_send = SEND_NOTHING;
                src.flicker_from = src.switch_at;
                src.flicker_period = ms_to_samples(fs, (double)periods_ms[pi]);
                src.flicker_open = ms_to_samples(fs, 20.0);
                int want_code = -1;
                int want_inverted = -1;
                expected_name(code, sent_inverted, &want_code, &want_inverted);
                const run_result r =
                    run_signal(&src, src.switch_at + ms_to_samples(fs, 1500.0), block, want_code, want_inverted);
                assert(r.first_wrong < 0);
                assert(r.first_lock >= 0 && r.first_lock < src.switch_at);
                /* Every dropout is shorter than the hangover: the carrier never read closed. */
                assert(r.first_closed < 0);
                if (r.first_unlocked < 0) {
                    DSD_FPRINTF(stderr, "stopped code held under a flickering carrier: fs=%d code=%03o period=%d ms\n",
                                k_rates[ri], (unsigned int)code, periods_ms[pi]);
                }
                assert(r.first_unlocked > src.switch_at);
                const double loss_ms = samples_to_ms(fs, r.first_unlocked - src.switch_at);
                if (loss_ms > (double)(SPAN_MS + WORD_MS)) {
                    DSD_FPRINTF(stderr,
                                "slow loss under a flickering carrier: fs=%d code=%03o period=%d ms -> %.0f ms\n",
                                k_rates[ri], (unsigned int)code, periods_ms[pi], loss_ms);
                }
                assert(loss_ms <= (double)(SPAN_MS + WORD_MS));
                assert(r.dcs_locks == 1 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
                times[count++] = loss_ms;
            }
        }
    }
    qsort(times, (size_t)count, sizeof(times[0]), compare_doubles);
    printf("loss under a flickering carrier: p50 %.0f ms, p95 %.0f ms, worst %.0f ms (%d cases; each <= %d ms)\n",
           times[count / 2], times[(count * 95) / 100], times[count - 1], count, (int)(SPAN_MS + WORD_MS));
    (void)fflush(stdout);
}

/*
 * The same code at another word phase keeps the lock: a radio that re-keys inside the carrier
 * hangover (a simplex radio with no turn-off tone), or a repeater whose carrier stays up while
 * another transmitter sends the same code, starts the word over somewhere else. Every shift of
 * 1 to 22 bits, the bit timing moved by a share of a bit as well, on a continuous carrier and
 * after gaps of 120 and 190 ms (the hangover is 200 ms): the code stays shown throughout, as
 * one lock, and never reads "none". After a gap the timing moves by close to half a bit, where
 * the bit clock takes longest to settle, and the windows read across the gap hold bits of it
 * for a word, so the new place is read only after the gap, a word and the settling: 57 of the
 * 64-bit span at 190 ms.
 */
static void
test_same_code_at_another_word_phase_holds(void) {
    static const double gaps_ms[] = {0.0, 120.0, 190.0};
    const double fs = 48000.0;
    for (int gap = 0; gap < 3; gap++) {
        for (int k = 1; k < DSD_DCS_WORD_BITS; k++) {
            const int code = dsd_dcs_code(((k * 5) + (gap * 3)) % DSD_DCS_CODE_COUNT);
            const int sent_inverted = (k + gap) % 2;
            dsd_analog_rx_core_init(&g_core);
            dcs_src src;
            src_init(&src, fs, 5230ULL + (uint64_t)((gap * 100) + k), code, sent_inverted, (k % 2) ? 10.0 : 20.0, 75.0);
            src.bit_phase = 0.0;
            const int64_t resume = ms_to_samples(fs, 1500.0);
            const int64_t gap_from = resume - ms_to_samples(fs, gaps_ms[gap]);
            if (gap) {
                src.carrier_off = gap_from;
                src.carrier_on = resume;
            }
            /* Where the word would have been at resume, moved on k bits and a share of one. */
            const double at = fmod((double)resume * src.baud / fs, (double)DSD_DCS_WORD_BITS);
            src.rephase_at = resume;
            const double share = gap ? 0.45 + (0.01 * (double)(k % 10)) : 0.1 * (double)((k * 3) % 10);
            src.rephase_to = fmod(at + (double)k + share, (double)DSD_DCS_WORD_BITS);
            int want_code = -1;
            int want_inverted = -1;
            expected_name(code, sent_inverted, &want_code, &want_inverted);
            const run_result r = run_signal(&src, ms_to_samples(fs, 2600.0), 480, want_code, want_inverted);
            if (r.first_unlocked >= 0 || r.dcs_locks != 1) {
                DSD_FPRINTF(stderr,
                            "lost at another word phase: gap=%.0f ms shift=%d bits, unlocked at %.0f ms, %lld locks\n",
                            gaps_ms[gap], k, samples_to_ms(fs, r.first_unlocked), (long long)r.dcs_locks);
            }
            assert(r.first_wrong < 0 && r.first_lock >= 0 && r.first_lock < gap_from);
            assert(r.first_unlocked < 0 && r.first_none < 0 && r.first_closed < 0);
            assert(r.dcs_locks == 1);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * Rejection
 * ---------------------------------------------------------------------------------------- */

/* A carrier with no code reads "detecting", then "none" once 500 ms of it have been evaluated;
   a code that starts after that still locks within the bound of its own start. */
static void
test_no_code_verdict_then_late_code(void) {
    dsd_analog_rx_core_init(&g_core);
    dcs_src src;
    src_init(&src, 48000.0, 919ULL, 0265, 1, 10.0, 75.0);
    src.on = ms_to_samples(48000.0, 1200.0);
    int code = -1;
    int inverted = -1;
    expected_name(0265, 1, &code, &inverted);
    const run_result r = run_signal(&src, ms_to_samples(48000.0, 1900.0), 48, code, inverted);
    assert(r.first_none >= 0);
    assert(samples_to_ms(48000.0, r.first_none) >= 500.0 && samples_to_ms(48000.0, r.first_none) <= 560.0);
    assert(g_core.dcs.bits_decided > 0);
    assert(r.first_lock > src.on && samples_to_ms(48000.0, r.first_lock - src.on) <= (double)LOCK_10DB_MS);
}

/*
 * Acquisition reads a supported word twice in a row, exactly in one 23-bit window and within one
 * bit in the other (analog_dcs.c, step 5), pinned at both edges on a code whose every other word
 * is damaged, so no two windows 23 bits apart are ever identical. Random bits first settle the
 * bit clock and the slicers' levels without locking anything, and the code then starts at the
 * top of a clean word, so no window reads the random bits as part of it. One bit off locks every
 * code in both polarities, the damaged bit spread over the word, within the 10 dB bound of the
 * code's start. Bits 3 and 12 off never lock. Later in the word a droop slicer whose hypothesis
 * does not fit can read a damaged bit back as the code's own value, so a pair there now and then
 * locks the right code; bits 3 and 12 every slicer reads as sent. Both edges fail on a rule one
 * bit off: with both windows exact the one-bit case locks only where a slicer reads the damaged
 * bit back, and with two bits of slack the two-bit case locks every code.
 */
static void
test_acquisition_allows_one_bit_between_readings(void) {
    const double fs = 8000.0;
    const int64_t settle = ms_to_samples(fs, 500.0);
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        for (int inverted = 0; inverted < 2; inverted++) {
            const int code = dsd_dcs_code(i);
            int want_code = -1;
            int want_inverted = -1;
            expected_name(code, inverted, &want_code, &want_inverted);
            for (int damage = 1; damage <= 2; damage++) {
                dsd_analog_rx_core_init(&g_core);
                dcs_src src;
                src_init(&src, fs, 5230ULL + (uint64_t)((i * 4) + (inverted * 2) + damage), code, inverted, 200.0,
                         75.0);
                src.send = SEND_RANDOM;
                src.switch_at = settle;
                src.then_send = SEND_WORD;
                src.rephase_at = settle;
                src.rephase_to = 0.0;
                src.flip = 1;
                src.flip_every = 2;
                src.flip_from = settle;
                if (damage == 1) {
                    src.flip_bit = (i + (11 * inverted)) % DSD_DCS_WORD_BITS;
                } else {
                    src.flip_bit = 3;
                    src.flip2 = 1;
                    src.flip2_bit = 12;
                }
                const run_result r = run_signal(&src, settle + ms_to_samples(fs, 2000.0), 80, want_code, want_inverted);
                if (damage == 1) {
                    assert(r.dcs_locks == 1 && r.first_lock > settle);
                    assert(samples_to_ms(fs, r.first_lock - settle) <= (double)LOCK_10DB_MS);
                } else {
                    assert(r.dcs_locks == 0 && r.first_lock < 0);
                }
            }
        }
    }
}

/* Random bits at the DCS rate never lock, however long they run: a lock needs a supported word
   read twice in a row, once exactly and once within a bit. */
static void
test_random_bits_never_lock(void) {
    dsd_analog_rx_core_init(&g_core);
    dcs_src src;
    src_init(&src, 8000.0, 1234567ULL, -1, 0, 200.0, 75.0);
    src.send = SEND_RANDOM;
    const run_result r = run_signal(&src, ms_to_samples(8000.0, 600000.0), 400, -1, -1);
    printf("random bits: %lld bits read over 10 minutes, %lld locks\n", (long long)g_core.dcs.bits_decided,
           (long long)r.dcs_locks);
    assert(r.dcs_locks == 0 && r.first_wrong < 0);
    assert(g_core.dcs.bits_decided > 80000);
}

/* Fewest bits in which some rotation of @p word differs from a supported code's word, in either
   polarity. */
static int
distance_to_supported(uint32_t word) {
    int best = DSD_DCS_WORD_BITS;
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        for (int inverted = 0; inverted < 2; inverted++) {
            const uint32_t target = dsd_dcs_word(dsd_dcs_code(i), inverted);
            for (int k = 0; k < DSD_DCS_WORD_BITS; k++) {
                uint32_t diff = synth_dcs_rotate(word, k) ^ target;
                int bits = 0;
                while (diff != 0U) {
                    diff &= diff - 1U;
                    bits++;
                }
                best = bits < best ? bits : best;
            }
        }
    }
    return best;
}

/*
 * The Golay (23,12) code's rotation classes that carry no supported code, sent clean in both
 * polarities: none locks. They are code words too, so each is at least 7 bits from every
 * supported word. And every supported word sent bit-reversed (the reciprocal generator's code,
 * a transmitter with the wrong bit order): none locks unless it is one bit from a supported
 * word, which it may then read as, the way a DCS decoder tolerates a bit error.
 */
static void
test_other_code_words_never_lock(void) {
    int tried = 0;
    for (uint32_t m = 0; m < 4096U; m++) {
        const uint32_t word = synth_golay23_word(m);
        if (synth_dcs_canonical(word) != word || word == 0U || word == (1U << DSD_DCS_WORD_BITS) - 1U) {
            continue; /* one representative per class; constants are DC */
        }
        if (dsd_dcs_match(word, NULL, NULL)) {
            continue;
        }
        assert(distance_to_supported(word) >= 7);
        for (int inverted = 0; inverted < 2; inverted++) {
            dsd_analog_rx_core_init(&g_core);
            dcs_src src;
            src_init(&src, 8000.0, 1000ULL + m, 0023, 0, 200.0, 75.0);
            src.word = inverted ? (~word & ((1U << DSD_DCS_WORD_BITS) - 1U)) : word;
            const run_result r = run_signal(&src, ms_to_samples(8000.0, 1500.0), 400, -1, -1);
            assert(r.dcs_locks == 0 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
        }
        tried++;
    }
    /* 178 non-constant classes, 104 of them a supported code's. */
    assert(tried == 178 - DSD_DCS_CODE_COUNT);
    int far = 0;
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        const uint32_t reversed = synth_dcs_reverse(dsd_dcs_word(dsd_dcs_code(i), 0));
        if (distance_to_supported(reversed) <= 1) {
            continue;
        }
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 8000.0, 2000ULL + (uint64_t)i, 0023, 0, 200.0, 75.0);
        src.word = reversed;
        const run_result r = run_signal(&src, ms_to_samples(8000.0, 1500.0), 400, -1, -1);
        assert(r.dcs_locks == 0);
        far++;
    }
    assert(far > DSD_DCS_CODE_COUNT / 2);
}

/* No CTCSS tone is a DCS code: every table tone, clean and at 10 dB, at 8 and 48 kHz, for 3 s,
   never locks the DCS detector, not even for a moment (the CTCSS detector names it). */
static void
test_ctcss_tones_never_lock(void) {
    static const int rates[] = {8000, 48000};
    for (int ri = 0; ri < 2; ri++) {
        for (int t = 0; t < DSD_CTCSS_TONE_COUNT; t++) {
            for (int clean = 0; clean < 2; clean++) {
                dsd_analog_rx_core_init(&g_core);
                dcs_src src;
                src_init(&src, (double)rates[ri], 3000ULL + (uint64_t)(t * 4 + ri * 2 + clean), 0023, 0,
                         clean ? 200.0 : 10.0, 75.0);
                src.send = SEND_TONE;
                src.tone_hz = (double)dsd_ctcss_tone_tenths(t) / 10.0;
                const run_result r = run_signal(&src, ms_to_samples((double)rates[ri], 3000.0), rates[ri] / 50, -1, -1);
                assert(r.dcs_locks == 0);
                assert(g_core.dcs.state != DSD_ANALOG_TONE_STATE_LOCKED);
                assert(g_core.dcs.bits_decided > 350);
            }
        }
    }
}

/* Speech with no code never locks, unfiltered (the hostile case: a voice fundamental in the
   band) or through a transmitter's voice high-pass, over two minutes each; and a code under
   transmitter-filtered speech 10 dB above it (speech level 1.09: a long-run RMS of 0.32 against
   the code's 0.1) still locks within the 3 dB bound and, once locked, holds for 20 s. */
static void
test_speech(void) {
    for (int filtered = 0; filtered < 2; filtered++) {
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 16000.0, 4000ULL + (uint64_t)filtered, -1, 0, 200.0, 75.0);
        synth_speech_init(&src.speech, 16000.0, 4100ULL + (uint64_t)filtered, 1.09);
        synth_voice_hpf_init(&src.voice_hpf, 16000.0);
        src.voice_filtered = filtered;
        src.voice_gain = 1.0;
        const run_result r = run_signal(&src, ms_to_samples(16000.0, 120000.0), 320, -1, -1);
        assert(r.dcs_locks == 0);
    }
    static double times[24];
    for (int k = 0; k < 24; k++) {
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 48000.0, 5000ULL + (uint64_t)k, dsd_dcs_code(k * 4 + 1), k % 2, 200.0, 75.0);
        synth_speech_init(&src.speech, 48000.0, 5100ULL + (uint64_t)k, 1.09);
        synth_voice_hpf_init(&src.voice_hpf, 48000.0);
        src.voice_filtered = 1;
        src.voice_gain = 1.0;
        src.on = ms_to_samples(48000.0, 250.0);
        int code = -1;
        int inverted = -1;
        expected_name(dsd_dcs_code(k * 4 + 1), k % 2, &code, &inverted);
        const run_result r =
            run_signal(&src, src.on + ms_to_samples(48000.0, LOCK_3DB_SEEDED_MS + 150.0), 480, code, inverted);
        assert(r.first_wrong < 0);
        times[k] = r.first_lock >= 0 ? samples_to_ms(48000.0, r.first_lock - src.on) : -1.0;
    }
    check_bound("lock under transmitter-filtered speech 10 dB above the code", times, 24, LOCK_3DB_SEEDED_MS, 0);
    for (int k = 0; k < 4; k++) {
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        const int code = dsd_dcs_code(k * 25 + 3);
        src_init(&src, 48000.0, 6000ULL + (uint64_t)k, code, k % 2, 200.0, k < 2 ? 75.0 : 750.0);
        synth_speech_init(&src.speech, 48000.0, 6100ULL + (uint64_t)k, 1.09);
        synth_voice_hpf_init(&src.voice_hpf, 48000.0);
        src.voice_filtered = 1;
        src.voice_gain = 1.0;
        int want_code = -1;
        int want_inverted = -1;
        expected_name(code, k % 2, &want_code, &want_inverted);
        const run_result r = run_signal(&src, ms_to_samples(48000.0, 21000.0), 480, want_code, want_inverted);
        assert(r.first_wrong < 0 && r.first_lock >= 0);
        assert(samples_to_ms(48000.0, r.first_lock) <= (double)LOCK_3DB_SEEDED_MS);
        assert(r.first_unlocked < 0 && r.dcs_locks == 1);
    }
}

/* ------------------------------------------------------------------------------------------
 * Invariance
 * ---------------------------------------------------------------------------------------- */

/* Every threshold is a ratio: the RTL live (1/pi), replay (1) and int16 PCM (32768) scales lock
   in the same block. */
static void
test_scale_invariance(void) {
    static const double scales[] = {1.0, 1.0 / M_PI, 32768.0};
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i += 13) {
        int64_t first_lock[3];
        for (int si = 0; si < 3; si++) {
            dsd_analog_rx_core_init(&g_core);
            dcs_src src;
            src_init(&src, 48000.0, 271828ULL + (uint64_t)i, dsd_dcs_code(i), i % 2, 10.0, 75.0);
            src.scale = scales[si];
            int code = -1;
            int inverted = -1;
            expected_name(dsd_dcs_code(i), i % 2, &code, &inverted);
            const run_result r = run_signal(&src, ms_to_samples(48000.0, 900.0), 480, code, inverted);
            assert(r.first_wrong < 0 && r.first_lock >= 0);
            first_lock[si] = r.first_lock;
        }
        assert(first_lock[0] == first_lock[1] && first_lock[0] == first_lock[2]);
    }
}

/* The detector works per sample, so how the input is cut into blocks never moves a verdict:
   the lock is seen in the block that holds its sample. */
static void
test_block_size_does_not_move_the_verdict(void) {
    static const int blocks[] = {37, 480, 960, 4096};
    int64_t lock_at[4];
    for (int b = 0; b < 4; b++) {
        dsd_analog_rx_core_init(&g_core);
        dcs_src src;
        src_init(&src, 48000.0, 161803ULL, 0431, 0, 10.0, 75.0);
        const run_result r = run_signal(&src, ms_to_samples(48000.0, 1000.0), blocks[b], 0431, 0);
        assert(r.first_lock >= 0);
        lock_at[b] = r.first_lock;
    }
    /* Each block size observes the same lock sample, rounded up to its own block end. */
    for (int b = 1; b < 4; b++) {
        assert(lock_at[b] >= lock_at[0] - blocks[0] && lock_at[b] < lock_at[0] + blocks[b]);
    }
}

/* A rate the front end never delivers (every decimated rate is below 4800 Hz) leaves the
   detector inert rather than reading past its sample ring: it decides nothing and reports "no
   code", so the core never waits on it. */
static void
test_unusable_rate_is_inert(void) {
    static dsd_analog_dcs det;
    static const double rates[] = {9600.0, 48000.0};
    for (int i = 0; i < 2; i++) {
        DSD_MEMSET(&det, 0, sizeof(det));
        dsd_analog_dcs_ops.configure(&det, rates[i]);
        assert(det.rate_hz <= 0.0 && det.box_len == 1);
        float band[256];
        for (int k = 0; k < 256; k++) {
            band[k] = (k / 16) % 2 ? 0.1f : -0.1f;
        }
        dsd_analog_dcs_ops.process(&det, band, band, band, 256, 0);
        assert(det.bits_decided == 0 && det.n == 0);
        dsd_analog_rx_report report;
        dsd_analog_dcs_ops.report(&det, &report);
        assert(report.state == DSD_ANALOG_TONE_STATE_NONE && report.kind == DSD_ANALOG_TONE_KIND_NONE);
    }
    /* The highest rate it does serve: just under 4800 Hz (a 4799 Hz input, decimated by 1). */
    DSD_MEMSET(&det, 0, sizeof(det));
    dsd_analog_dcs_ops.configure(&det, 4799.0);
    assert(det.rate_hz > 4798.0 && det.box_len == 36);
}

int
main(void) {
    test_unusable_rate_is_inert();
    test_aliases_through_the_detector();
    test_no_code_verdict_then_late_code();
    test_block_size_does_not_move_the_verdict();
    test_scale_invariance();
    test_off_rate_transmitter_locks();
    test_one_bit_error_per_word_holds();
    test_two_bit_errors_per_word_lose();
    test_acquisition_allows_one_bit_between_readings();
    test_code_stop_under_carrier_loses();
    test_turnoff_tone_loses_fast();
    test_carrier_drop_and_dropout();
    test_a_reset_forgets_the_lock();
    test_code_stop_under_a_flickering_carrier();
    test_same_code_at_another_word_phase_holds();
    test_steady_component_keeps_the_lock();
    test_a_code_outranks_a_tone();
    test_a_code_read_as_a_tone_first_still_locks_in_time();
    test_other_code_words_never_lock();
    test_ctcss_tones_never_lock();
    test_every_code_locks_within_bound();
    test_codes_lock_at_every_rate();
    test_codes_lock_through_a_sound_card_coupling();
    test_codes_lock_through_a_frequency_offset();
    test_random_bits_never_lock();
    test_speech();
    return 0;
}
