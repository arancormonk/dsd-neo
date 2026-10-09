// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * CTCSS detection (issue #522), driven through the pure analog receive core in sample time.
 *
 * Every bound below is measured in samples of the input rate, not wall-clock time, and every
 * signal comes from a seeded generator (analog_tone_synth.h), so a failure reproduces exactly.
 * Valid detection and rejection both carry positive assertions: the right tone locks within
 * its bound, and the wrong answers -- a neighbouring tone, an unsupported frequency, a voice
 * fundamental, noise -- never lock at all.
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

/* The per-row pins under test (docs/cli.md "Received tone"): what these fixed seeds measure,
   tighter than the timing contract in <dsd-neo/dsp/analog_rx.h> that every row also checks. */
enum {
    LOCK_BOUND_MS = 400,
    VOICE_LOCK_BOUND_MS = 500,
    LOSS_BOUND_MS = 350,
    BURST_LOSS_BOUND_MS = 150,
    HOP_MS = DSD_ANALOG_CTCSS_SUBBLOCK_MS,
    /* A locked tone that moves off the table is dropped once the window has filled with the
       new frequency and four more hops have failed. */
    MOVED_OFF_BOUND_MS = (DSD_ANALOG_CTCSS_WINDOW + DSD_ANALOG_CTCSS_LOSE_HOPS) * DSD_ANALOG_CTCSS_SUBBLOCK_MS,
    /* The minutes of speech read "none" for at least this share of their carrier time. */
    SPEECH_NONE_MIN_PCT = 80,
    /* The reverse burst at 0 dB: how long the flipped tone runs before the carrier drops, the
       share of bursts caught within BURST_LOSS_BOUND_MS, and the most that may be missed. */
    BURST_0DB_CARRIER_MS = 400,
    BURST_0DB_WITHIN_BOUND_MIN_PCT = 89,
    BURST_0DB_MISSED_MAX = 5,
    /* A reverse burst inside the sub-block a lock or a relock is made on, at +10 dB: the most
       of those steps whose burst may go uncaught within BURST_LOSS_BOUND_MS (these seeds miss
       none and 15). */
    BURST_AT_LOCK_MISSED_MAX = 2,
    BURST_AT_RELOCK_MISSED_MAX = 20,
};

/* A pin that holds keeps its events inside the contract's per-event ceilings too: the checks
   that assert only a pin (the adjacent low tones, the late tone, the moved-off drop) are
   covered by the ceilings through these. */
_Static_assert((int)LOCK_BOUND_MS <= (int)DSD_ANALOG_CTCSS_LOCK_CEILING_MS, "lock pin outside the lock ceiling");
_Static_assert((int)VOICE_LOCK_BOUND_MS <= (int)DSD_ANALOG_CTCSS_LOCK_CEILING_MS,
               "voice lock pin outside the lock ceiling");
_Static_assert((int)LOSS_BOUND_MS <= (int)DSD_ANALOG_CTCSS_LOSS_CEILING_MS, "loss pin outside the loss ceiling");
_Static_assert((int)BURST_LOSS_BOUND_MS <= (int)DSD_ANALOG_CTCSS_LOSS_CEILING_MS, "burst pin outside the loss ceiling");
_Static_assert((int)MOVED_OFF_BOUND_MS <= (int)DSD_ANALOG_CTCSS_LOSS_CEILING_MS,
               "moved-off pin outside the loss ceiling");

static const int k_rates[] = {8000, 44100, 48000, 78125};
#define RATE_COUNT ((int)(sizeof(k_rates) / sizeof(k_rates[0])))

/* One observation of the core after a block. A DCS lock (issue #523) reads as locked with no
   CTCSS tone, so every check below counts it as a wrong lock; only the DCS test expects one. */
typedef struct {
    int state;
    int tenths;
    int carrier;
    int dcs; /**< 1 when the lock is a DCS code's */
} observation;

static observation
observe(const dsd_analog_rx_core* core) {
    dsd_analog_rx_publication pub;
    dsd_analog_rx_core_publish(core, &pub);
    observation o = {pub.tone_state, pub.ctcss_tenths_hz, pub.carrier_open, 0};
    if (pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && pub.tone_kind == DSD_ANALOG_TONE_KIND_DCS) {
        assert(dsd_dcs_code_index(pub.dcs_code) >= 0);
        assert(pub.ctcss_tenths_hz == 0);
        o.tenths = -1;
        o.dcs = 1;
    } else if (pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED) {
        assert(pub.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS);
        assert(dsd_ctcss_tone_index(pub.ctcss_tenths_hz) >= 0);
    } else {
        assert(pub.tone_kind == DSD_ANALOG_TONE_KIND_NONE);
        assert(pub.ctcss_tenths_hz == 0);
    }
    /* Detection never gates audio: the policy field stays OFF whatever is heard. */
    assert(pub.gate == DSD_ANALOG_TONE_GATE_OFF);
    return o;
}

/* A signal source for the harness: returns the next input sample. */
typedef struct signal_src signal_src;
typedef float (*signal_next_fn)(signal_src* src, int64_t n);

struct signal_src {
    signal_next_fn next;
    double fs;
    synth_rng rng;
    synth_tone tone;
    double noise_sigma;
    int64_t tone_on;  /**< first sample carrying the tone */
    int64_t tone_off; /**< first sample without it (INT64_MAX = never) */
    int64_t flip_at;  /**< reverse burst: the tone's phase steps by flip_rad here (INT64_MAX = never) */
    double flip_rad;  /**< the reverse burst's phase step: pi, or 2 pi / 3 and 4 pi / 3 for the other variants */
    int64_t move_at;  /**< the tone moves to move_hz here, phase-continuous (INT64_MAX = never) */
    double move_hz;
    int64_t carrier_off; /**< the carrier drops here: digital silence from then on (INT64_MAX = never) */
    synth_dcs dcs;       /**< DCS signalling instead of a tone, when dcs_on */
    int dcs_on;
    double scale;        /**< input scale: 1, 1/pi, 32768 */
    synth_speech speech; /**< optional voice */
    synth_voice_hpf voice_hpf;
    double voice_gain;  /**< 0 = no voice */
    int voice_filtered; /**< 1 = through the transmitter's 300 Hz high-pass */
    synth_tone steady;  /**< optional steady voice-band tone, such as a test tone (amp 0 = none) */
    /** From here the carrier is up for only the first flicker_open samples of every
        flicker_period, digital silence between (INT64_MAX = never). */
    int64_t flicker_from;
    int64_t flicker_period;
    int64_t flicker_open;
};

static float
signal_next(signal_src* src, int64_t n) {
    if (n >= src->carrier_off) {
        return 0.0f;
    }
    double v = src->noise_sigma > 0.0 ? src->noise_sigma * synth_gauss(&src->rng) : 0.0;
    if (n == src->flip_at) {
        src->tone.phase += src->flip_rad;
    }
    if (n == src->move_at) {
        src->tone.hz = src->move_hz;
    }
    if (src->dcs_on) {
        v += synth_dcs_next(&src->dcs);
    }
    if (n >= src->tone_on && n < src->tone_off) {
        v += synth_tone_next(&src->tone);
    }
    if (src->steady.amp > 0.0) {
        v += synth_tone_next(&src->steady);
    }
    if (src->voice_gain > 0.0) {
        float voice = synth_speech_next(&src->speech);
        if (src->voice_filtered) {
            voice = synth_voice_hpf_next(&src->voice_hpf, voice);
        }
        v += src->voice_gain * voice;
    }
    /* A dropout silences the receiver, not the transmitter: every generator keeps running, so a
       tone that is still on comes back with its phase where the transmitter's is. */
    if (n >= src->flicker_from && ((n - src->flicker_from) % src->flicker_period) >= src->flicker_open) {
        return 0.0f;
    }
    return (float)(v * src->scale);
}

static void
signal_init(signal_src* src, double fs, uint64_t seed, double tone_hz, double snr_db) {
    DSD_MEMSET(src, 0, sizeof(*src));
    src->next = signal_next;
    src->fs = fs;
    synth_rng_seed(&src->rng, seed);
    src->tone.fs = fs;
    src->tone.hz = tone_hz;
    src->tone.amp = 0.1;
    src->tone.phase = synth_range(&src->rng, 0.0, 2.0 * M_PI);
    src->noise_sigma = synth_inband_noise_sigma(0.1, snr_db, fs);
    src->tone_on = INT64_MAX;
    src->tone_off = INT64_MAX;
    src->flip_at = INT64_MAX;
    src->flip_rad = M_PI;
    src->move_at = INT64_MAX;
    src->carrier_off = INT64_MAX;
    src->flicker_from = INT64_MAX;
    src->flicker_period = 1;
    src->flicker_open = 1;
    src->scale = 1.0;
}

/* What a run saw: when each thing first happened, in samples from the start. -1 = never. */
typedef struct {
    int64_t first_lock;     /**< first block end at which the expected tone was locked */
    int64_t first_wrong;    /**< first block end at which any other tone was locked */
    int64_t first_unlocked; /**< first block end, after first_lock, without the lock */
    int64_t first_none;     /**< first block end at which the verdict read NONE */
    int64_t last_acquiring; /**< last block end, before first_none, still reading ACQUIRING */
    int64_t locks;          /**< transitions into LOCKED */
    int64_t open_blocks;    /**< blocks observed with the carrier open */
    int64_t none_blocks;    /**< ... of which read NONE */
    int64_t first_closed;   /**< first block end at which the carrier read closed */
    int64_t ctcss_locks;    /**< transitions into a CTCSS lock */
    /** Transitions of the CTCSS detector itself into LOCKED, published or not: a DCS lock
        outranks a CTCSS one in the publication (issue #523), which would hide it. */
    int64_t ctcss_detector_locks;
    int final_state;
    int final_dcs; /**< 1 when the run ended on a DCS lock */
} run_result;

/*
 * Feed @p total samples in blocks of @p block, observing after each. Small blocks give the
 * lock time to within one block; the core itself only changes its verdict on 50 ms hops, so
 * block size never changes when that happens (checked separately).
 */
static run_result
run_signal(dsd_analog_rx_core* core, signal_src* src, int64_t total, int block, int expect_tenths) {
    run_result r = {-1, -1, -1, -1, -1, 0, 0, 0, -1, 0, 0, 0, 0};
    float buf[4096];
    assert(block > 0 && block <= (int)(sizeof(buf) / sizeof(buf[0])));
    int prev_locked = 0;
    int prev_dcs = 0;
    int prev_ctcss_detector_locked = 0;
    for (int64_t n = 0; n < total; n += block) {
        const int m = (total - n) < block ? (int)(total - n) : block;
        for (int i = 0; i < m; i++) {
            buf[i] = src->next(src, n + i);
        }
        assert(dsd_analog_rx_core_process(core, buf, m, (int)src->fs, 1) == 1);
        const observation o = observe(core);
        const int locked = o.state == DSD_ANALOG_TONE_STATE_LOCKED;
        if (locked && !prev_locked) {
            r.locks++;
        }
        if (locked && !o.dcs && (!prev_locked || prev_dcs)) {
            r.ctcss_locks++;
        }
        const int ctcss_detector_locked = core->ctcss.state == DSD_ANALOG_TONE_STATE_LOCKED;
        if (ctcss_detector_locked && !prev_ctcss_detector_locked) {
            r.ctcss_detector_locks++;
        }
        prev_ctcss_detector_locked = ctcss_detector_locked;
        if (locked && o.tenths == expect_tenths && r.first_lock < 0) {
            r.first_lock = n + m;
        }
        if (locked && o.tenths != expect_tenths && r.first_wrong < 0) {
            r.first_wrong = n + m;
        }
        if (!locked && r.first_lock >= 0 && r.first_unlocked < 0) {
            r.first_unlocked = n + m;
        }
        if (o.state == DSD_ANALOG_TONE_STATE_NONE && r.first_none < 0) {
            r.first_none = n + m;
        }
        if (o.state == DSD_ANALOG_TONE_STATE_ACQUIRING && r.first_none < 0) {
            r.last_acquiring = n + m;
        }
        r.open_blocks += o.carrier ? 1 : 0;
        r.none_blocks += o.carrier && o.state == DSD_ANALOG_TONE_STATE_NONE ? 1 : 0;
        if (!o.carrier && r.first_closed < 0) {
            r.first_closed = n + m;
        }
        prev_locked = locked;
        prev_dcs = o.dcs;
        r.final_state = o.state;
        r.final_dcs = o.dcs;
    }
    return r;
}

static int64_t
ms_to_samples(double fs, double ms) {
    return (int64_t)llround(fs * ms / 1000.0);
}

static double
samples_to_ms(double fs, int64_t samples) {
    return (double)samples * 1000.0 / fs;
}

static dsd_analog_rx_core g_core;

/* Lock time of a tone at @p hz that should read as the table tone @p expect_tenths, measured
   from its onset at @p onset_ms of noise, the run lasting @p watch_ms past it. Returns -1 if it
   never locked; asserts it never locked anything else. */
static double
lock_time_at_ms(int fs, double hz, int expect_tenths, double snr_db, uint64_t seed, double onset_ms, double watch_ms) {
    dsd_analog_rx_core_init(&g_core);
    signal_src src;
    signal_init(&src, fs, seed, hz, snr_db);
    src.tone_on = ms_to_samples(fs, onset_ms);
    const int block = fs / 1000 > 0 ? fs / 1000 : 1;
    const int64_t total = src.tone_on + ms_to_samples(fs, watch_ms);
    const run_result r = run_signal(&g_core, &src, total, block, expect_tenths);
    if (r.first_wrong >= 0) {
        DSD_FPRINTF(stderr, "wrong lock: fs=%d hz=%.2f snr=%.0f seed=%llu\n", fs, hz, snr_db, (unsigned long long)seed);
    }
    assert(r.first_wrong < 0);
    if (r.first_lock < 0) {
        return -1.0;
    }
    return samples_to_ms(fs, r.first_lock - src.tone_on);
}

/* The same with the onset landing anywhere inside a hop: @p onset_offset_ms after 300 ms of noise. */
static double
lock_time_as_ms(int fs, double hz, int expect_tenths, double snr_db, uint64_t seed, int onset_offset_ms,
                double watch_ms) {
    return lock_time_at_ms(fs, hz, expect_tenths, snr_db, seed, 300.0 + onset_offset_ms, watch_ms);
}

/* Lock time of a tone exactly on its table value. */
static double
lock_time_ms(int fs, double hz, double snr_db, uint64_t seed, int onset_offset_ms) {
    return lock_time_as_ms(fs, hz, (int)lround(hz * 10.0), snr_db, seed, onset_offset_ms, LOCK_BOUND_MS + 150.0);
}

static int
compare_doubles(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

/*
 * The timing contract (<dsd-neo/dsp/analog_rx.h>) on one row of fixed seeds: prints p50/p95/worst
 * of @p count timings for the PR evidence, then asserts the row's p95 is within @p p95_target_ms
 * and every single event within @p ceiling_ms. The p95 is the event at index 95% of @p count (the
 * 191st fastest of 200), so a row of 200 allows at most nine beyond the target. Sorts in place.
 */
static void
check_timing_contract(const char* what, double* times, int count, int p95_target_ms, int ceiling_ms) {
    assert(count > 0);
    qsort(times, (size_t)count, sizeof(times[0]), compare_doubles);
    const double p95 = times[(count * 95) / 100];
    const double worst = times[count - 1];
    printf("%s: p50 %.0f ms, p95 %.0f ms, worst %.0f ms (%d cases; contract p95 <= %d ms, each <= %d ms)\n", what,
           times[count / 2], p95, worst, count, p95_target_ms, ceiling_ms);
    (void)fflush(stdout);
    if (p95 > (double)p95_target_ms || worst > (double)ceiling_ms) {
        DSD_FPRINTF(stderr, "timing contract broken: %s\n", what);
    }
    assert(p95 <= (double)p95_target_ms);
    assert(worst <= (double)ceiling_ms);
}

/* Lock: from the tone's onset to the first hop that reports it. */
static void
check_lock_contract(const char* what, double* times, int count) {
    check_timing_contract(what, times, count, DSD_ANALOG_CTCSS_LOCK_P95_MS, DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
}

/* Loss: from the tone's stop (or a caught reverse burst) under a live carrier to the first hop
   without it. */
static void
check_loss_contract(const char* what, double* times, int count) {
    check_timing_contract(what, times, count, DSD_ANALOG_CTCSS_LOSS_P95_MS, DSD_ANALOG_CTCSS_LOSS_CEILING_MS);
}

/* All 50 tones, every rate, at +10 and 0 dB in-band tone-to-noise: the right value within
   400 ms of sample time. Checks the lock contract and prints the p95 and worst lock times for the
   PR evidence. */
static void
test_every_tone_locks_within_bound(void) {
    static const double snrs[] = {10.0, 0.0};
    static double times[SYNTH_LEGACY_CTCSS_COUNT * RATE_COUNT];
    for (int si = 0; si < 2; si++) {
        int count = 0;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            for (int k = 0; k < SYNTH_LEGACY_CTCSS_COUNT; k++) {
                const double hz = (double)synth_legacy_ctcss_tenths(k) / 10.0;
                const uint64_t seed = 1000003ULL * (uint64_t)(k + 1) + (uint64_t)ri * 7919ULL + (uint64_t)si;
                const double t = lock_time_ms(k_rates[ri], hz, snrs[si], seed, (k * 7) % 50);
                if (t < 0.0 || t > (double)LOCK_BOUND_MS) {
                    DSD_FPRINTF(stderr, "slow lock: fs=%d hz=%.1f snr=%.0f -> %.0f ms\n", k_rates[ri], hz, snrs[si], t);
                }
                assert(t >= 0.0 && t <= (double)LOCK_BOUND_MS);
                times[count++] = t;
            }
        }
        char what[64];
        DSD_SNPRINTF(what, sizeof(what), "CTCSS lock at %+.0f dB in-band", snrs[si]);
        check_lock_contract(what, times, count);
    }
}

/*
 * Transmitter encoder error: a tone a little off its table value still locks on that value.
 * Every tone at every rate, alternately above and below the table value, with its own seeds
 * (not the ones test_every_tone_locks_within_bound uses). A lock needs estimates within
 * 0.5 Hz of the table value (k_acquire_snap_hz in analog_ctcss.c: the frequency hysteresis
 * that keeps 68.2 Hz off 69.3), so the further off a tone sits the more of the 0 dB estimate
 * scatter (about 0.19 Hz RMS) pushes a hop past that gate and the later it locks. Each row is
 * a floor on the share of these 200 starts that lock within the 400 ms bound and a ceiling on
 * the slowest, set at what these seeds do (the printed shares and times); the last row is the
 * exact tone at 0 dB on a second seed set. Every row also checks the lock contract. These are
 * pinned seeds, not the long-run rate: over 10,000 seeded starts at 0 dB, 0.3% of exact tones and
 * 1.6% of tones 0.2 Hz off take longer than 400 ms (docs/testing.md). Prints p50/p95/worst for the
 * PR evidence.
 */
static void
test_off_nominal_tones_lock(void) {
    static const struct {
        double offset_hz;
        double snr_db;
        int min_within_bound_pct; /**< share of starts that lock within LOCK_BOUND_MS */
        int worst_ms;             /**< every start locks within this */
    } rows[] = {
        {0.20, 10.0, 100, LOCK_BOUND_MS},
        {0.35, 10.0, 100, LOCK_BOUND_MS},
        {0.20, 0.0, 95, 500},
        {0.0, 0.0, 100, LOCK_BOUND_MS},
    };

    static double times[SYNTH_LEGACY_CTCSS_COUNT * RATE_COUNT];
    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        int count = 0;
        int within = 0;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            for (int k = 0; k < SYNTH_LEGACY_CTCSS_COUNT; k++) {
                const int tenths = synth_legacy_ctcss_tenths(k);
                const double sign = (k + ri) % 2 == 0 ? 1.0 : -1.0;
                const double hz = ((double)tenths / 10.0) + (sign * rows[row].offset_hz);
                const uint64_t seed = (3000017ULL * (uint64_t)(k + 1)) + ((uint64_t)ri * 7919ULL) + (uint64_t)row;
                const double t = lock_time_as_ms(k_rates[ri], hz, tenths, rows[row].snr_db, seed, (k * 11) % 50,
                                                 rows[row].worst_ms + 50.0);
                if (t < 0.0 || t > (double)rows[row].worst_ms) {
                    DSD_FPRINTF(stderr, "slow off-nominal lock: fs=%d hz=%.2f snr=%.0f -> %.0f ms\n", k_rates[ri], hz,
                                rows[row].snr_db, t);
                }
                assert(t >= 0.0 && t <= (double)rows[row].worst_ms);
                within += t <= (double)LOCK_BOUND_MS ? 1 : 0;
                times[count++] = t;
            }
        }
        char what[96];
        DSD_SNPRINTF(what, sizeof(what), "CTCSS lock %.2f Hz off the table at %+.0f dB in-band (%d%% within %d ms)",
                     rows[row].offset_hz, rows[row].snr_db, (100 * within) / count, LOCK_BOUND_MS);
        check_lock_contract(what, times, count);
        assert(100 * within >= rows[row].min_within_bound_pct * count);
    }
}

/*
 * 150.0 Hz (issue #518 follow-up), the tone many radios offer as a 51st, 1.4 Hz below 151.4: at
 * every rate, at +10, +3 and 0 dB in-band, on seeds of its own, it locks on 150.0 within the lock
 * contract and never reads as anything else (lock_time_as_ms() asserts that). Prints p50/p95/worst
 * for the PR evidence.
 */
static void
test_tone_150_locks_within_bound(void) {
    static const double snrs[] = {10.0, 3.0, 0.0};

    enum { SEEDS = 20 };

    static double times[RATE_COUNT * SEEDS];
    for (int si = 0; si < 3; si++) {
        int count = 0;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            for (int n = 0; n < SEEDS; n++) {
                const uint64_t seed = (15000001ULL * (uint64_t)(n + 1)) + ((uint64_t)ri * 7919ULL) + (uint64_t)si;
                const double t = lock_time_as_ms(k_rates[ri], 150.0, 1500, snrs[si], seed, (n * 7) % 50,
                                                 (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS + 50.0);
                if (t < 0.0 || t > (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS) {
                    DSD_FPRINTF(stderr, "slow 150.0 Hz lock: fs=%d snr=%.0f -> %.0f ms\n", k_rates[ri], snrs[si], t);
                }
                assert(t >= 0.0 && t <= (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
                times[count++] = t;
            }
        }
        char what[64];
        DSD_SNPRINTF(what, sizeof(what), "CTCSS 150.0 Hz lock at %+.0f dB in-band", snrs[si]);
        check_lock_contract(what, times, count);
    }
}

/*
 * 150.0 and 151.4 Hz, the closest pair in the table: each, on its value and 0.2 or 0.35 Hz either
 * side (an encoder's error, toward the other tone included), at +10, +3 and 0 dB in-band, at 8 and
 * 48 kHz, over 3 s, locks nothing but itself on these seeds, and from 3 dB up, or within 0.2 Hz of
 * its value, it does lock. Each tone's gate is 0.7 Hz, half the distance between them
 * (ctcss_gate_hz()). Not never: near 0 dB, with a tone set toward the other, the other can still
 * be named for a few hops, rarely now that a candidate leaning toward the other tone needs a
 * third agreeing hop (issue #623; the rows below); docs/testing.md has the long-run rates.
 */
static void
test_150_and_151_4_never_cross(void) {
    static const int tones[] = {1500, 1514};
    static const double offsets[] = {0.0, 0.2, -0.2, 0.35, -0.35};
    static const double snrs[] = {10.0, 3.0, 0.0};
    static const int rates[] = {8000, 48000};
    for (int ti = 0; ti < 2; ti++) {
        for (int oi = 0; oi < 5; oi++) {
            for (int si = 0; si < 3; si++) {
                for (int ri = 0; ri < 2; ri++) {
                    for (int n = 0; n < 2; n++) {
                        const int fs = rates[ri];
                        const double hz = ((double)tones[ti] / 10.0) + offsets[oi];
                        dsd_analog_rx_core_init(&g_core);
                        signal_src src;
                        signal_init(&src, fs, 1514001ULL + (uint64_t)(((((ti * 5) + oi) * 3 + si) * 2 + ri) * 2 + n),
                                    hz, snrs[si]);
                        src.tone_on = 0;
                        const run_result r = run_signal(&g_core, &src, ms_to_samples(fs, 3000.0), fs / 50, tones[ti]);
                        if (r.first_wrong >= 0 || r.first_lock < 0) {
                            DSD_FPRINTF(stderr, "150.0/151.4: fs=%d hz=%.2f snr=%.0f wrong=%lld lock=%lld\n", fs, hz,
                                        snrs[si], (long long)r.first_wrong, (long long)r.first_lock);
                        }
                        assert(r.first_wrong < 0);
                        if (snrs[si] >= 3.0 || fabs(offsets[oi]) <= 0.2 + 1e-9) {
                            assert(r.first_lock >= 0);
                        }
                    }
                }
            }
        }
    }
}

/* Issue #623: a tone of the 150.0/151.4 Hz pair @p toward_hz toward the other tone of the pair (negative: away). */
static double
pair_tone_hz(int tenths, double toward_hz) {
    return ((double)tenths / 10.0) + (tenths == 1500 ? toward_hz : -toward_hz);
}

/* Onset of a start rebuilt from the long-run sweeps (docs/testing.md): 300 ms of noise and @p onset_us more. */
static double
sweep_onset_ms(int onset_us) {
    return 300.0 + ((double)onset_us / 1000.0);
}

/*
 * Issue #623: a candidate for 150.0 or 151.4 Hz whose newest estimate leans more than 0.3 Hz
 * toward the other tone of the pair waits for a third agreeing hop before it locks. On a clean
 * carrier (+30 dB in-band) from the reception's start, at every rate and on the same seed, each
 * tone set 0.4 Hz toward the other locks exactly one hop after the same tone set 0.4 Hz away;
 * 146.2 Hz, whose nearest neighbour is 3.8 Hz away, locks on the same hop either way.
 */
static void
test_pair_lean_waits_one_hop(void) {
    static const int tones[] = {1500, 1514, 1462};
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        for (int ti = 0; ti < 3; ti++) {
            const int tenths = tones[ti];
            /* Toward the nearer neighbour: up for 150.0 and 146.2 Hz, down for 151.4 Hz. */
            const double sign = tenths == 1514 ? -1.0 : 1.0;
            const uint64_t seed = 623001ULL + (uint64_t)(ri * 3 + ti);
            double t[2];
            for (int side = 0; side < 2; side++) {
                const double hz = ((double)tenths / 10.0) + ((side == 0 ? 0.4 : -0.4) * sign);
                t[side] = lock_time_at_ms(fs, hz, tenths, 30.0, seed, 0.0, 600.0);
                assert(t[side] >= 0.0);
            }
            /* One hop of the core at this rate, in input samples: a sub-block of decimated samples. */
            const double hop_ms = samples_to_ms(fs, (int64_t)g_core.ctcss.sub_len * (int64_t)g_core.fe.decim);
            const double extra = t[0] - t[1];
            const double want = tenths == 1462 ? 0.0 : hop_ms;
            if (fabs(extra - want) > 1.5) {
                DSD_FPRINTF(stderr, "lean hop: fs=%d tone=%d toward %.0f away %.0f (hop %.1f ms)\n", fs, tenths, t[0],
                            t[1], hop_ms);
            }
            assert(fabs(extra - want) <= 1.5);
        }
    }
}

/*
 * Issue #623: starts of 150.0 and 151.4 Hz that named the other tone of the pair before the
 * third-hop rule, rebuilt from the seeds of the offline sweep the rule was tuned on (10,000 starts
 * per condition, the onset anywhere in a hop after 300 ms of noise): the 73 of its 80 such starts
 * in the contract's conditions (on the table value and 0.2 Hz either side at 0, +3 and +10 dB,
 * 0.35 Hz either side at +10 dB: 220,000 starts) that the rule stops, and 20 of the 281 it stops
 * set 0.35 Hz toward the other at 0 and +3 dB. Each now locks its own tone, never the other, within
 * the lock ceiling. Prints p50/worst.
 */
static void
test_pair_starts_that_named_the_other_tone(void) {
    static const struct {
        int tenths;
        double toward_hz;
        double snr_db;
        uint64_t seed;
        int fs;
        int onset_us;
    } starts[] = {{1500, 0.00, 3.0, 516390005311ULL, 48000, 8849},   {1500, 0.00, 3.0, 516390005597ULL, 8000, 33747},
                  {1500, 0.00, 3.0, 516390009911ULL, 44100, 19558},  {1500, 0.20, 0.0, 983170001832ULL, 44100, 34872},
                  {1500, 0.20, 0.0, 983170001887ULL, 48000, 427},    {1500, 0.20, 0.0, 983170001928ULL, 48000, 18041},
                  {1500, 0.20, 0.0, 983170002296ULL, 8000, 22645},   {1500, 0.20, 0.0, 983170002686ULL, 8000, 47248},
                  {1500, 0.20, 0.0, 983170004044ULL, 8000, 8503},    {1500, 0.20, 0.0, 983170004483ULL, 8000, 10909},
                  {1500, 0.20, 0.0, 983170005516ULL, 44100, 12004},  {1500, 0.20, 0.0, 983170006911ULL, 48000, 5776},
                  {1500, 0.20, 0.0, 983170007415ULL, 44100, 32731},  {1500, 0.20, 0.0, 983170008495ULL, 8000, 30481},
                  {1500, 0.20, 0.0, 983170008657ULL, 44100, 44888},  {1500, 0.20, 3.0, 416720001040ULL, 8000, 43910},
                  {1500, 0.20, 3.0, 416720001638ULL, 44100, 43489},  {1500, 0.20, 3.0, 416720004896ULL, 44100, 43891},
                  {1500, 0.20, 3.0, 416720005503ULL, 44100, 35678},  {1500, 0.20, 3.0, 416720007508ULL, 8000, 43477},
                  {1500, 0.35, 10.0, 1400008081ULL, 48000, 9230},    {1514, 0.00, 0.0, 629020002312ULL, 78125, 41852},
                  {1514, 0.00, 0.0, 629020005522ULL, 48000, 38415},  {1514, 0.00, 0.0, 629020005596ULL, 48000, 7900},
                  {1514, 0.00, 0.0, 629020006255ULL, 48000, 3917},   {1514, 0.00, 0.0, 629020008457ULL, 44100, 32757},
                  {1514, 0.00, 0.0, 629020009943ULL, 48000, 30244},  {1514, 0.20, 0.0, 234010000163ULL, 78125, 8056},
                  {1514, 0.20, 0.0, 234010001757ULL, 78125, 40957},  {1514, 0.20, 0.0, 234010002023ULL, 8000, 42123},
                  {1514, 0.20, 0.0, 234010002050ULL, 8000, 4533},    {1514, 0.20, 0.0, 234010002442ULL, 78125, 49610},
                  {1514, 0.20, 0.0, 234010002932ULL, 44100, 45671},  {1514, 0.20, 0.0, 234010003411ULL, 48000, 10520},
                  {1514, 0.20, 0.0, 234010003769ULL, 44100, 29509},  {1514, 0.20, 0.0, 234010004181ULL, 8000, 19714},
                  {1514, 0.20, 0.0, 234010004898ULL, 78125, 48091},  {1514, 0.20, 0.0, 234010006060ULL, 78125, 41013},
                  {1514, 0.20, 0.0, 234010008211ULL, 8000, 47224},   {1514, 0.20, 0.0, 234010009250ULL, 48000, 35080},
                  {1514, 0.20, 0.0, 234010009564ULL, 8000, 6806},    {1514, 0.20, 0.0, 234010009811ULL, 78125, 13615},
                  {1514, 0.20, 3.0, 718200000027ULL, 8000, 26556},   {1514, 0.20, 3.0, 718200000785ULL, 78125, 22751},
                  {1514, 0.20, 3.0, 718200000995ULL, 44100, 48512},  {1514, 0.20, 3.0, 718200001631ULL, 48000, 34226},
                  {1514, 0.20, 3.0, 718200003415ULL, 8000, 38816},   {1514, 0.20, 3.0, 718200004281ULL, 8000, 2316},
                  {1514, 0.20, 3.0, 718200004576ULL, 8000, 39957},   {1514, 0.20, 3.0, 718200005197ULL, 78125, 7843},
                  {1514, 0.20, 3.0, 718200005724ULL, 8000, 896},     {1514, 0.20, 3.0, 718200005859ULL, 44100, 588},
                  {1514, 0.20, 3.0, 718200007479ULL, 78125, 21740},  {1514, 0.20, 3.0, 718200007568ULL, 44100, 17719},
                  {1514, 0.20, 3.0, 718200008862ULL, 48000, 15058},  {1514, 0.20, 3.0, 718200009014ULL, 8000, 23263},
                  {1514, 0.20, 3.0, 718200009105ULL, 44100, 46597},  {1514, 0.20, 3.0, 718200009337ULL, 8000, 11784},
                  {1514, 0.20, 3.0, 718200009385ULL, 48000, 43967},  {1514, 0.20, 3.0, 718200009858ULL, 78125, 29022},
                  {1514, 0.35, 10.0, 686100000543ULL, 48000, 43960}, {1514, 0.35, 10.0, 686100001079ULL, 48000, 11578},
                  {1514, 0.35, 10.0, 686100001334ULL, 78125, 43453}, {1514, 0.35, 10.0, 686100003293ULL, 48000, 47965},
                  {1514, 0.35, 10.0, 686100003631ULL, 8000, 5521},   {1514, 0.35, 10.0, 686100003945ULL, 78125, 37914},
                  {1514, 0.35, 10.0, 686100004138ULL, 78125, 17196}, {1514, 0.35, 10.0, 686100006790ULL, 78125, 11687},
                  {1514, 0.35, 10.0, 686100007108ULL, 78125, 32103}, {1514, 0.35, 10.0, 686100007584ULL, 44100, 12652},
                  {1514, 0.35, 10.0, 686100008222ULL, 44100, 47778}, {1514, 0.35, 10.0, 686100008469ULL, 78125, 1940},
                  {1514, 0.35, 10.0, 686100008998ULL, 48000, 46713}, {1500, 0.35, 0.0, 656640002623ULL, 78125, 13729},
                  {1500, 0.35, 0.0, 656640003120ULL, 8000, 29682},   {1500, 0.35, 0.0, 656640003346ULL, 48000, 39030},
                  {1500, 0.35, 0.0, 656640003986ULL, 48000, 23118},  {1500, 0.35, 0.0, 656640004068ULL, 78125, 15942},
                  {1500, 0.35, 0.0, 656640005487ULL, 48000, 2862},   {1500, 0.35, 0.0, 656640006014ULL, 44100, 31917},
                  {1500, 0.35, 0.0, 656640006088ULL, 8000, 16454},   {1500, 0.35, 3.0, 471410003951ULL, 48000, 46581},
                  {1514, 0.35, 0.0, 122280000727ULL, 44100, 10240},  {1514, 0.35, 0.0, 122280003065ULL, 8000, 44647},
                  {1514, 0.35, 0.0, 122280004887ULL, 48000, 11947},  {1514, 0.35, 0.0, 122280005805ULL, 78125, 27772},
                  {1514, 0.35, 0.0, 122280006569ULL, 44100, 45832},  {1514, 0.35, 0.0, 122280006951ULL, 8000, 37360},
                  {1514, 0.35, 0.0, 122280008682ULL, 44100, 33674},  {1514, 0.35, 0.0, 122280008687ULL, 8000, 46194},
                  {1514, 0.35, 0.0, 122280008865ULL, 78125, 25496},  {1514, 0.35, 0.0, 122280008929ULL, 44100, 11031},
                  {1514, 0.35, 3.0, 443210009967ULL, 8000, 16235}};

    enum { START_COUNT = (int)(sizeof(starts) / sizeof(starts[0])) };

    static double times[START_COUNT];
    for (int i = 0; i < START_COUNT; i++) {
        const double hz = pair_tone_hz(starts[i].tenths, starts[i].toward_hz);
        times[i] = lock_time_at_ms(starts[i].fs, hz, starts[i].tenths, starts[i].snr_db, starts[i].seed,
                                   sweep_onset_ms(starts[i].onset_us), 1000.0);
        if (times[i] < 0.0 || times[i] > (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS) {
            DSD_FPRINTF(stderr, "pair start: fs=%d hz=%.2f snr=%.0f -> %.0f ms\n", starts[i].fs, hz, starts[i].snr_db,
                        times[i]);
        }
        assert(times[i] >= 0.0 && times[i] <= (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
    }
    qsort(times, (size_t)START_COUNT, sizeof(times[0]), compare_doubles);
    printf(
        "CTCSS 150.0/151.4 Hz starts that named the other tone before the third-hop rule: p50 %.0f ms, worst %.0f ms "
        "(%d cases; each its own tone, <= %d ms)\n",
        times[START_COUNT / 2], times[START_COUNT - 1], START_COUNT, DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
    (void)fflush(stdout);
}

/*
 * Issue #623: 150.0 and 151.4 Hz starting in noise, the onset anywhere in a hop, at every rate,
 * on seeds of their own: on the table value and 0.2 Hz toward the other tone at 0 dB in-band, and
 * 0.35 Hz toward it at +10 dB. Each row locks only its own tone (lock_time_as_ms() asserts it)
 * and checks the lock contract. Prints p50/p95/worst for the PR evidence.
 */
static void
test_150_and_151_4_onset_in_noise(void) {
    static const struct {
        double toward_hz;
        double snr_db;
    } rows[] = {{0.0, 0.0}, {0.2, 0.0}, {0.35, 10.0}};

    enum { SEEDS = 25 };

    static double times[2 * RATE_COUNT * SEEDS];
    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        int count = 0;
        for (int ti = 0; ti < 2; ti++) {
            const int tenths = ti == 0 ? 1500 : 1514;
            for (int ri = 0; ri < RATE_COUNT; ri++) {
                for (int n = 0; n < SEEDS; n++) {
                    const uint64_t seed = (6230017ULL * (uint64_t)(n + 1)) + ((uint64_t)ri * 7919ULL)
                                          + ((uint64_t)row * 104729ULL) + (uint64_t)ti;
                    const double hz = pair_tone_hz(tenths, rows[row].toward_hz);
                    const double t = lock_time_as_ms(k_rates[ri], hz, tenths, rows[row].snr_db, seed,
                                                     ((n * 7) + (ti * 3) + (int)row) % 50,
                                                     (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS + 50.0);
                    if (t < 0.0) {
                        DSD_FPRINTF(stderr, "pair onset never locked: fs=%d hz=%.2f snr=%.0f\n", k_rates[ri], hz,
                                    rows[row].snr_db);
                    }
                    assert(t >= 0.0);
                    times[count++] = t;
                }
            }
        }
        char what[96];
        DSD_SNPRINTF(what, sizeof(what), "CTCSS 150.0/151.4 Hz %.2f Hz toward the other at %+.0f dB in-band",
                     rows[row].toward_hz, rows[row].snr_db);
        check_lock_contract(what, times, count);
    }
}

/*
 * Issue #623: a locked tone of the pair that moves, phase-continuously, to the other tone of the
 * pair or between 151.4 and 146.2 Hz, under a live carrier at +10 and 0 dB, every rate: the old
 * tone is dropped within MOVED_OFF_BOUND_MS of the move, the new one reads within the lock ceiling
 * of it, and no third tone is ever named. A lock handed between the pair's tones waits for the
 * third agreeing hop too while the new tone's estimate still leans toward the old one.
 */
static void
test_150_and_151_4_handover(void) {
    static const double moves[][2] = {{151.4, 150.0}, {150.0, 151.4}, {151.4, 146.2}, {146.2, 151.4}};
    static const double snrs[] = {10.0, 0.0};

    enum { SEEDS = 3 };

    double worst_old = 0.0;
    double worst_new = 0.0;
    for (size_t mi = 0; mi < sizeof(moves) / sizeof(moves[0]); mi++) {
        const int from_t = (int)lround(moves[mi][0] * 10.0);
        const int to_t = (int)lround(moves[mi][1] * 10.0);
        for (int si = 0; si < 2; si++) {
            for (int ri = 0; ri < RATE_COUNT; ri++) {
                for (int n = 0; n < SEEDS; n++) {
                    const int fs = k_rates[ri];
                    dsd_analog_rx_core_init(&g_core);
                    signal_src src;
                    const uint64_t seed = 6231001ULL + (uint64_t)(((((int)mi * 2) + si) * RATE_COUNT + ri) * SEEDS + n);
                    signal_init(&src, fs, seed, moves[mi][0], snrs[si]);
                    src.tone_on = 0;
                    src.move_at = ms_to_samples(fs, 2000.0 + (double)((n * 17 + ri * 5) % 50));
                    src.move_hz = moves[mi][1];
                    const int block = fs / 1000;
                    const int64_t total = src.move_at + ms_to_samples(fs, 1500.0);
                    float buf[128];
                    int64_t old_last = -1;
                    int64_t new_first = -1;
                    int locked_before = 0;
                    for (int64_t s = 0; s < total; s += block) {
                        const int m = (total - s) < block ? (int)(total - s) : block;
                        for (int i = 0; i < m; i++) {
                            buf[i] = src.next(&src, s + i);
                        }
                        assert(dsd_analog_rx_core_process(&g_core, buf, m, fs, 1) == 1);
                        const observation o = observe(&g_core);
                        const int locked = o.state == DSD_ANALOG_TONE_STATE_LOCKED;
                        if (locked) {
                            /* Never a third tone, a DCS code included. */
                            assert(!o.dcs && (o.tenths == from_t || o.tenths == to_t));
                        }
                        if (s + m <= src.move_at) {
                            locked_before = locked && o.tenths == from_t;
                            continue;
                        }
                        if (locked && o.tenths == from_t) {
                            old_last = s + m;
                        }
                        if (locked && o.tenths == to_t && new_first < 0) {
                            new_first = s + m;
                        }
                    }
                    /* The run exercises a handover: the old tone was locked when it moved. */
                    assert(locked_before);
                    assert(new_first >= 0);
                    const double old_ms = old_last < 0 ? 0.0 : samples_to_ms(fs, old_last - src.move_at);
                    const double new_ms = samples_to_ms(fs, new_first - src.move_at);
                    if (old_ms > (double)MOVED_OFF_BOUND_MS || new_ms > (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS) {
                        DSD_FPRINTF(stderr, "pair handover: %.1f -> %.1f Hz fs=%d snr=%.0f old %.0f new %.0f ms\n",
                                    moves[mi][0], moves[mi][1], fs, snrs[si], old_ms, new_ms);
                    }
                    assert(old_ms <= (double)MOVED_OFF_BOUND_MS);
                    assert(new_ms <= (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
                    worst_old = fmax(worst_old, old_ms);
                    worst_new = fmax(worst_new, new_ms);
                }
            }
        }
    }
    printf("CTCSS 150.0/151.4 Hz handover: old tone dropped within %.0f ms (<= %d), new tone read within %.0f ms "
           "(<= %d)\n",
           worst_old, MOVED_OFF_BOUND_MS, worst_new, DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
    (void)fflush(stdout);
}

/*
 * The slowest starts of 150.0 and 151.4 Hz in the long-run sweeps at 0 dB in-band with the
 * third-hop rule (docs/testing.md; 600,000 starts on the table value and 0.2 Hz either side),
 * rebuilt from the sweep's own seeds, the late acquisition windows' locks among them: each locks
 * its own tone within the 700 ms lock ceiling.
 */
static void
test_pair_slow_starts_lock_within_ceiling(void) {
    static const struct {
        int tenths;
        double toward_hz;
        uint64_t seed;
        int fs;
        int onset_us;
    } starts[] = {{1500, 0.20, 2252782983759959ULL, 78125, 36609}, {1514, 0.00, 2252428833716371ULL, 8000, 20987},
                  {1514, 0.20, 2252033823692821ULL, 48000, 39445}, {1514, 0.00, 2252428833774930ULL, 48000, 44267},
                  {1514, 0.20, 2252033823767090ULL, 78125, 4723},  {1514, 0.20, 2252033823781709ULL, 44100, 16947},
                  {1514, 0.20, 2252033823765836ULL, 48000, 15187}, {1500, 0.20, 2252782983735594ULL, 48000, 27692},
                  {1514, 0.20, 2252033823753512ULL, 78125, 33680}, {1514, 0.20, 2252033823705614ULL, 48000, 35082},
                  {1500, 0.20, 2252782983715539ULL, 44100, 39693}, {1514, 0.20, 2252033823782711ULL, 8000, 34652},
                  {1514, 0.20, 2252033823685696ULL, 44100, 40717}, {1500, 0.20, 2252782983778793ULL, 44100, 47403},
                  {1500, 0.20, 2252782983716368ULL, 48000, 44432}, {1514, 0.20, 2252033823720172ULL, 78125, 44274},
                  {1514, 0.20, 2252033823743453ULL, 78125, 6718},  {1514, 0.20, 2252033823748638ULL, 48000, 8572},
                  {1500, 0.20, 2252782983686933ULL, 78125, 8268},  {1514, 0.20, 2252033823700477ULL, 78125, 10417},
                  {1500, 0.20, 2252782983777525ULL, 78125, 13905}, {1514, -0.20, 2252595513781536ULL, 48000, 14487}};

    enum { START_COUNT = (int)(sizeof(starts) / sizeof(starts[0])) };

    double worst = 0.0;
    for (int i = 0; i < START_COUNT; i++) {
        const double hz = pair_tone_hz(starts[i].tenths, starts[i].toward_hz);
        const double t = lock_time_at_ms(starts[i].fs, hz, starts[i].tenths, 0.0, starts[i].seed,
                                         sweep_onset_ms(starts[i].onset_us), 1000.0);
        if (t < 0.0 || t > (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS) {
            DSD_FPRINTF(stderr, "slow pair start: fs=%d hz=%.2f -> %.0f ms\n", starts[i].fs, hz, t);
        }
        assert(t >= 0.0 && t <= (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
        worst = fmax(worst, t);
    }
    printf("CTCSS 150.0/151.4 Hz slowest long-run starts at +0 dB in-band: worst %.0f ms (%d cases; each <= %d ms)\n",
           worst, START_COUNT, DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
    (void)fflush(stdout);
}

/*
 * The slowest onsets of the long-run lock sweeps at 0 dB in-band (docs/testing.md, "Ceiling
 * tail"): 32 of 2,000,000 that the 250 ms window alone locked only after the 700 ms lock ceiling
 * -- 8 exact tones, the slowest after 1,128 ms, and 24 tones 0.2 Hz off, the slowest after
 * 843 ms -- because their noise kept every pair of hops from qualifying the tone well past half a
 * second. The late acquisition windows lock each of them within the ceiling, on its table value.
 * Each onset is rebuilt exactly as the sweep made it: set @c s, the tone at every rate, seed and
 * onset derived from them.
 */
static void
test_late_onsets_lock_within_ceiling(void) {
    static const struct {
        int set;
        int fs;
        int tenths;
        double offset_hz;
    } onsets[] = {
        {2546, 78125, 670, 0.0},  {2103, 44100, 1148, 0.0}, {1540, 48000, 670, 0.0},  {3220, 48000, 2291, 0.0},
        {3572, 44100, 1598, 0.0}, {3443, 48000, 1598, 0.0}, {3589, 44100, 2181, 0.0}, {2384, 44100, 948, 0.0},
        {4345, 78125, 1413, 0.2}, {2284, 78125, 2181, 0.2}, {3522, 48000, 2541, 0.2}, {1363, 8000, 670, 0.2},
        {1011, 78125, 1598, 0.2}, {4544, 78125, 1928, 0.2}, {1812, 48000, 1598, 0.2}, {3339, 8000, 915, 0.2},
        {402, 48000, 693, 0.2},   {3528, 44100, 1862, 0.2}, {116, 44100, 2181, 0.2},  {1592, 78125, 885, 0.2},
        {863, 8000, 2541, 0.2},   {1640, 8000, 670, 0.2},   {1473, 78125, 719, 0.2},  {1191, 44100, 2065, 0.2},
        {3220, 48000, 2291, 0.2}, {2023, 8000, 854, 0.2},   {1899, 78125, 1230, 0.2}, {4813, 8000, 1230, 0.2},
        {3220, 78125, 1462, 0.2}, {1575, 8000, 2418, 0.2},  {803, 78125, 2503, 0.2},  {4873, 8000, 1598, 0.2},
    };

    enum { ONSET_COUNT = (int)(sizeof(onsets) / sizeof(onsets[0])) };

    static double times[ONSET_COUNT];
    for (int i = 0; i < ONSET_COUNT; i++) {
        int ri = 0;
        while (k_rates[ri] != onsets[i].fs) {
            ri++;
        }
        const int k = synth_legacy_ctcss_index(onsets[i].tenths);
        assert(k >= 0);
        const int s = onsets[i].set;
        const double sign = (k + ri + s) % 2 == 0 ? 1.0 : -1.0;
        const double hz = ((double)onsets[i].tenths / 10.0) + (sign * onsets[i].offset_hz);
        const uint64_t seed = (3000017ULL * (uint64_t)(k + 1)) + ((uint64_t)ri * 7919ULL) + ((uint64_t)s * 104729ULL);
        times[i] = lock_time_as_ms(onsets[i].fs, hz, onsets[i].tenths, 0.0, seed, (k * 11 + s * 7) % 50,
                                   (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS + 50.0);
        if (times[i] < 0.0 || times[i] > (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS) {
            DSD_FPRINTF(stderr, "late onset past the lock ceiling: set=%d fs=%d hz=%.2f -> %.0f ms\n", s, onsets[i].fs,
                        hz, times[i]);
        }
        assert(times[i] >= 0.0 && times[i] <= (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
    }
    qsort(times, (size_t)ONSET_COUNT, sizeof(times[0]), compare_doubles);
    printf("CTCSS lock of the long-run sweeps' slowest onsets at +0 dB in-band: p50 %.0f ms, worst %.0f ms (%d cases; "
           "each <= %d ms)\n",
           times[ONSET_COUNT / 2], times[ONSET_COUNT - 1], ONSET_COUNT, DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
    (void)fflush(stdout);
}

/* 67.0, 69.3 and 71.9 Hz sit 2.3-2.6 Hz apart; each must read as itself. */
static void
test_adjacent_low_tones_are_distinguished(void) {
    static const double tones[] = {67.0, 69.3, 71.9};
    for (int i = 0; i < 3; i++) {
        for (uint64_t seed = 1; seed <= 4; seed++) {
            const double t = lock_time_ms(48000, tones[i], seed <= 2 ? 10.0 : 0.0, 424242ULL * seed + (uint64_t)i, 13);
            assert(t >= 0.0 && t <= (double)LOCK_BOUND_MS);
        }
    }
}

/*
 * Off-table tones lock nothing over these 3 s runs, down to 0 dB in-band. 152.6 Hz sits 1.2 Hz
 * above 151.4, whose gate is 0.7 Hz now that 150.0 Hz sits 1.4 Hz below it; 68.2, 161.0 and
 * 166.7 Hz sit 1.1-1.2 Hz from the table tone on either side, where
 * a 0 dB estimate strays past the 0.8 Hz snap gate on several percent of hops but only rarely
 * past the 0.5 Hz one a lock needs -- rarely, not never: over two hours of a 0 dB carrier,
 * 68.2 Hz reads as a neighbour for about 200 ms some ten times an hour (docs/testing.md), and
 * from 3 dB up it did not happen at all. 100.85 and 100.9 Hz sit just outside the snap gate of
 * 100.0, where only a clean signal's estimate is steady enough to tell.
 */
static void
test_unsupported_frequencies_never_lock(void) {
    static const double freqs[] = {152.6, 68.2, 161.0, 166.7};
    static const double snrs[] = {60.0, 10.0, 3.0, 0.0};
    for (int f = 0; f < 4; f++) {
        for (int s = 0; s < 4; s++) {
            for (int ri = 0; ri < RATE_COUNT; ri++) {
                dsd_analog_rx_core_init(&g_core);
                signal_src src;
                signal_init(&src, k_rates[ri], 777ULL + (uint64_t)(f * 10 + s), freqs[f], snrs[s]);
                src.tone_on = 0;
                const run_result r = run_signal(&g_core, &src, ms_to_samples(k_rates[ri], 3000.0), k_rates[ri] / 50, 0);
                if (r.locks != 0) {
                    DSD_FPRINTF(stderr, "off-table lock: fs=%d hz=%.1f snr=%.0f\n", k_rates[ri], freqs[f], snrs[s]);
                }
                assert(r.locks == 0);
                /* And they are positively rejected, not left pending. */
                assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
            }
        }
    }
    static const double edges[] = {100.85, 100.9};
    static const double edge_snrs[] = {20.0, 10.0};
    for (int f = 0; f < 2; f++) {
        for (int s = 0; s < 2; s++) {
            for (int ri = 0; ri < RATE_COUNT; ri++) {
                dsd_analog_rx_core_init(&g_core);
                signal_src src;
                signal_init(&src, k_rates[ri], 8080ULL + (uint64_t)(f * 10 + s), edges[f], edge_snrs[s]);
                src.tone_on = 0;
                const run_result r = run_signal(&g_core, &src, ms_to_samples(k_rates[ri], 3000.0), k_rates[ri] / 50, 0);
                assert(r.locks == 0 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
            }
        }
    }
}

/* The decimated rate the front end runs at for an input at @p fs, from its own design. */
static double
decimated_rate(int fs) {
    static dsd_analog_subaudible_fe fe;
    assert(dsd_analog_subaudible_fe_configure(&fe, fs) == 1);
    return fe.out_rate_hz;
}

/* 1.2 s of a steady voice-band tone at @p hz, alone on the carrier: nothing may lock, and the
   carrier reads NONE. */
static void
check_voice_band_tone_rejected(int fs, double hz) {
    dsd_analog_rx_core_init(&g_core);
    signal_src src;
    signal_init(&src, fs, 1ULL, 100.0, 0.0);
    src.noise_sigma = 0.0;
    src.steady.fs = fs;
    src.steady.hz = hz;
    src.steady.amp = 0.3;
    const run_result r = run_signal(&g_core, &src, ms_to_samples(fs, 1200.0), fs / 50, 0);
    if (r.locks != 0 || r.final_state != DSD_ANALOG_TONE_STATE_NONE) {
        DSD_FPRINTF(stderr, "voice-band tone read as CTCSS: fs=%d hz=%.1f\n", fs, hz);
    }
    assert(r.locks == 0 && r.first_wrong < 0);
    assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE && r.open_blocks > 0);
}

/*
 * A steady tone in the voice band -- a 2300 Hz test tone, a data or signalling tone -- with
 * nothing else on the carrier. Decimating to the ~2.4 kHz rate folds a residue of it, 58 dB
 * or more down, onto k * rate +/- f, and with nothing else below 290 Hz that residue is the
 * whole band: without the full-band share check a 2300 Hz tone at 48 kHz read as 100.0 Hz and
 * 2333 Hz as 67.0 Hz. Every tone at every rate is placed so its residue lands exactly on a
 * table tone, from each side of the first multiple of the decimated rate, and a spread of
 * tones from the higher multiples up to half the input rate; 6 kHz is the rate class whose
 * stage 1 folds the most back (a 2-to-1 decimation).
 */
static void
test_voice_band_tones_never_lock(void) {
    static const int rates[] = {6000, 8000, 44100, 48000, 78125};
    for (size_t ri = 0; ri < sizeof(rates) / sizeof(rates[0]); ri++) {
        const int fs = rates[ri];
        const double out = decimated_rate(fs);
        for (int k = 1; k <= 4; k++) {
            /* The legacy table, then 150.0 Hz (t == SYNTH_LEGACY_CTCSS_COUNT). */
            for (int t = 0; t <= SYNTH_LEGACY_CTCSS_COUNT; t += (k == 1) ? 1 : 5) {
                const double tone = t < SYNTH_LEGACY_CTCSS_COUNT ? (double)synth_legacy_ctcss_tenths(t) / 10.0 : 150.0;
                for (int side = -1; side <= 1; side += 2) {
                    const double hz = ((double)k * out) + ((double)side * tone);
                    if (hz > 300.0 && hz < ((double)fs / 2.0) - 10.0) {
                        check_voice_band_tone_rejected(fs, hz);
                    }
                }
            }
        }
    }
    /* The reported cases, at 48 kHz: 2300 and 2500 Hz on 100.0, 2333 and 2467 Hz on 67.0,
       2200 Hz on 199.5 (as 200 Hz) and 2562.2 Hz on 162.2. */
    static const double reported[] = {2300.0, 2500.0, 2333.0, 2467.0, 2200.0, 2562.2};
    for (size_t i = 0; i < sizeof(reported) / sizeof(reported[0]); i++) {
        check_voice_band_tone_rejected(48000, reported[i]);
    }
}

/*
 * The other side of that check: a real tone beside a voice-band tone 30 dB louder, whose
 * residue lands on the tone's own correlator bin, still locks as itself within the bound. The
 * tone then carries about -30 dB of the full band, 20 dB above the share a lock needs.
 */
static void
test_tone_beside_a_loud_voice_band_tone_locks(void) {
    static const double tones[] = {67.0, 100.0, 162.2, 254.1};
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        for (int t = 0; t < 4; t++) {
            dsd_analog_rx_core_init(&g_core);
            signal_src src;
            signal_init(&src, fs, 5150ULL + (uint64_t)(ri * 4 + t), tones[t], 10.0);
            src.tone_on = ms_to_samples(fs, 300.0);
            src.steady.fs = fs;
            src.steady.hz = decimated_rate(fs) - tones[t];
            src.steady.amp = src.tone.amp * pow(10.0, 30.0 / 20.0);
            const int tenths = (int)lround(tones[t] * 10.0);
            const run_result r = run_signal(&g_core, &src, src.tone_on + ms_to_samples(fs, 1000.0), fs / 1000, tenths);
            assert(r.first_wrong < 0);
            assert(r.first_lock >= 0);
            const double t_ms = samples_to_ms(fs, r.first_lock - src.tone_on);
            if (t_ms > (double)LOCK_BOUND_MS) {
                DSD_FPRINTF(stderr, "slow lock beside a voice-band tone: fs=%d hz=%.1f -> %.0f ms\n", fs, tones[t],
                            t_ms);
            }
            assert(t_ms <= (double)LOCK_BOUND_MS);
            assert(r.first_unlocked < 0);
        }
    }
}

/*
 * And a lock ends with its tone even while a voice-band tone's residue sits on the same bin:
 * 100.0 Hz beside a steady 2300 Hz tone, on a clean carrier, stops after a second. The residue
 * alone is a perfectly steady 100 Hz in an otherwise empty band, so only the full-band share
 * on held hops lets the lock go -- within the loss bound, and nothing locks again.
 */
static void
test_tone_stop_beside_a_voice_band_tone_drops(void) {
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        dsd_analog_rx_core_init(&g_core);
        signal_src src;
        signal_init(&src, fs, 6160ULL + (uint64_t)ri, 100.0, 0.0);
        src.noise_sigma = 0.0;
        src.tone_on = 0;
        src.tone_off = ms_to_samples(fs, 1000.0);
        src.steady.fs = fs;
        src.steady.hz = decimated_rate(fs) - 100.0;
        src.steady.amp = 0.3;
        const run_result r = run_signal(&g_core, &src, ms_to_samples(fs, 2500.0), fs / 1000, 1000);
        assert(r.first_wrong < 0);
        assert(r.first_lock >= 0 && r.first_lock < src.tone_off);
        assert(r.first_unlocked > src.tone_off);
        const double lost_ms = samples_to_ms(fs, r.first_unlocked - src.tone_off);
        if (lost_ms > (double)LOSS_BOUND_MS) {
            DSD_FPRINTF(stderr, "residue held the lock: fs=%d -> %.0f ms\n", fs, lost_ms);
        }
        assert(lost_ms <= (double)LOSS_BOUND_MS);
        assert(r.locks == 1 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
    }
}

/*
 * A lock is only as good as its latest frequency: a locked table tone that moves off the
 * table -- to the midpoint of its neighbours, phase-continuous and just as strong -- is
 * dropped once the window has filled with the new frequency and four hops have failed, and
 * nothing else locks in its place. Without the frequency check on held hops the old value
 * stays on screen for as long as the new tone lasts.
 */
static void
test_lock_follows_a_tone_off_the_table(void) {
    static const double moves[][2] = {{69.3, 68.2}, {67.0, 68.2}, {162.2, 161.0}, {167.9, 166.7}};
    static const double snrs[] = {60.0, 10.0};
    for (int m = 0; m < 4; m++) {
        for (int s = 0; s < 2; s++) {
            for (int ri = 0; ri < RATE_COUNT; ri++) {
                const int fs = k_rates[ri];
                dsd_analog_rx_core_init(&g_core);
                signal_src src;
                signal_init(&src, fs, 4242ULL + (uint64_t)(m * 7 + s * 3 + ri), moves[m][0], snrs[s]);
                src.tone_on = 0;
                src.move_at = ms_to_samples(fs, 1000.0 + (double)(13 * m));
                src.move_hz = moves[m][1];
                const run_result r = run_signal(&g_core, &src, src.move_at + ms_to_samples(fs, 2000.0), fs / 1000,
                                                (int)lround(moves[m][0] * 10.0));
                assert(r.first_lock >= 0 && r.first_lock < src.move_at);
                assert(r.first_unlocked > src.move_at);
                const double lost_ms = samples_to_ms(fs, r.first_unlocked - src.move_at);
                if (lost_ms > (double)MOVED_OFF_BOUND_MS) {
                    DSD_FPRINTF(stderr, "held off-table: fs=%d %.1f->%.1f -> %.0f ms\n", fs, moves[m][0], moves[m][1],
                                lost_ms);
                }
                assert(lost_ms <= (double)MOVED_OFF_BOUND_MS);
                /* The one lock of the run was the real tone; the off-table one never locks. */
                assert(r.locks == 1 && r.first_wrong < 0);
                assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
            }
        }
    }
}

/*
 * The snap: the nearest tone whose gate holds the estimate, the gate's edge included. 150.0 and
 * 151.4 Hz get 0.7 Hz, half the distance between them, and an estimate midway, where their gates
 * meet, snaps to neither however its distances round; every other tone keeps 0.8 Hz, or 0.5 % of
 * itself from 160 Hz up.
 */
static void
test_snap_gates(void) {
    const int i1000 = dsd_ctcss_tone_index(1000);
    const int i1500 = dsd_ctcss_tone_index(1500);
    const int i1514 = dsd_ctcss_tone_index(1514);
    assert(i1000 >= 0 && i1500 >= 0 && i1514 == i1500 + 1);
    assert(dsd_analog_ctcss_snap_index(150.0) == i1500);
    assert(dsd_analog_ctcss_snap_index(150.69) == i1500);
    assert(dsd_analog_ctcss_snap_index(150.7) == -1);
    assert(dsd_analog_ctcss_snap_index(150.71) == i1514);
    assert(dsd_analog_ctcss_snap_index(149.3) == i1500);
    assert(dsd_analog_ctcss_snap_index(149.29) == -1);
    assert(dsd_analog_ctcss_snap_index(152.1) == i1514);
    assert(dsd_analog_ctcss_snap_index(152.11) == -1);
    assert(dsd_analog_ctcss_snap_index(100.8) == i1000);
    assert(dsd_analog_ctcss_snap_index(99.2) == i1000);
    assert(dsd_analog_ctcss_snap_index(100.81) == -1);
    /* From 160 Hz up the gate is 0.5 % of the tone (issue #643): 1.2705 Hz at 254.1, 0.811 Hz at 162.2. */
    const int i2541 = dsd_ctcss_tone_index(2541);
    const int i1622 = dsd_ctcss_tone_index(1622);
    assert(i2541 >= 0 && i1622 >= 0);
    assert(dsd_analog_ctcss_snap_index(254.1 + 1.27) == i2541);
    assert(dsd_analog_ctcss_snap_index(254.1 + 1.28) == -1);
    assert(dsd_analog_ctcss_snap_index(162.2 + 0.81) == i1622);
    assert(dsd_analog_ctcss_snap_index(162.2 + 0.82) == -1);
}

/*
 * 150.0 Hz stops (the loss and burst sweeps run the 50 tones they were pinned on): at every rate,
 * at +10 and 0 dB in-band on seeds of its own, it is dropped within the loss contract once it stops
 * under a live carrier and nothing locks again, and a 180 degree reverse burst at +10 dB ends the
 * lock within BURST_LOSS_BOUND_MS.
 */
static void
test_tone_150_loss_and_burst(void) {
    enum { SEEDS = 10 };

    static const double snrs[] = {10.0, 0.0};
    static double times[RATE_COUNT * SEEDS];
    for (int si = 0; si < 2; si++) {
        int count = 0;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            for (int n = 0; n < SEEDS; n++) {
                const int fs = k_rates[ri];
                dsd_analog_rx_core_init(&g_core);
                signal_src src;
                signal_init(&src, fs, 1500771ULL + (uint64_t)(n * 31 + ri) + (uint64_t)si * 104729ULL, 150.0, snrs[si]);
                src.tone_on = 0;
                src.tone_off = ms_to_samples(fs, 1000.0 + (double)((n * 11) % 50));
                const run_result r =
                    run_signal(&g_core, &src, src.tone_off + ms_to_samples(fs, 800.0), fs / 1000, 1500);
                assert(r.first_lock >= 0 && r.first_lock < src.tone_off);
                assert(r.first_unlocked > src.tone_off);
                assert(r.locks == 1 && r.first_wrong < 0 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
                times[count++] = samples_to_ms(fs, r.first_unlocked - src.tone_off);
            }
        }
        char what[64];
        DSD_SNPRINTF(what, sizeof(what), "CTCSS 150.0 Hz loss after it stops at %+.0f dB in-band", snrs[si]);
        check_loss_contract(what, times, count);
    }
    int count = 0;
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        for (int n = 0; n < SEEDS; n++) {
            const int fs = k_rates[ri];
            dsd_analog_rx_core_init(&g_core);
            signal_src src;
            signal_init(&src, fs, 1500881ULL + (uint64_t)(n * 13 + ri), 150.0, 10.0);
            src.tone_on = 0;
            src.flip_at = ms_to_samples(fs, 1000.0 + (double)((n * 17) % 50));
            src.flip_rad = M_PI;
            const run_result r = run_signal(&g_core, &src, src.flip_at + ms_to_samples(fs, 180.0), fs / 1000, 1500);
            assert(r.first_lock >= 0 && r.first_lock < src.flip_at);
            assert(r.first_unlocked > src.flip_at);
            const double loss_ms = samples_to_ms(fs, r.first_unlocked - src.flip_at);
            assert(loss_ms <= (double)BURST_LOSS_BOUND_MS);
            times[count++] = loss_ms;
        }
    }
    check_loss_contract("CTCSS 150.0 Hz loss on a 180 degree reverse burst", times, count);
}

/* What a run that moves a locked tone published, block by block: when the tone it started on locked and was dropped,
   when another tone was first named after the drop, and whether the first tone came back or a third tone was named. */
typedef struct {
    int64_t first_lock;  /**< first block end with the start tone locked */
    int64_t drop;        /**< first block end, after first_lock, without the start tone locked */
    int64_t named_other; /**< first block end, after the drop, with @p other_tenths locked */
    int64_t back;        /**< first block end, after the drop, with the start tone locked again */
    int64_t third;       /**< first block end with any tone but those two locked */
    int final_state;
} move_trace;

static move_trace
run_move(signal_src* src, int64_t total, int block, int start_tenths, int other_tenths) {
    move_trace t = {-1, -1, -1, -1, -1, 0};
    float buf[4096];
    assert(block > 0 && block <= (int)(sizeof(buf) / sizeof(buf[0])));
    for (int64_t n = 0; n < total; n += block) {
        const int m = (total - n) < block ? (int)(total - n) : block;
        for (int i = 0; i < m; i++) {
            buf[i] = src->next(src, n + i);
        }
        assert(dsd_analog_rx_core_process(&g_core, buf, m, (int)src->fs, 1) == 1);
        const observation o = observe(&g_core);
        const int locked = o.state == DSD_ANALOG_TONE_STATE_LOCKED ? o.tenths : 0;
        if (locked == start_tenths && t.first_lock < 0) {
            t.first_lock = n + m;
        }
        if (t.first_lock >= 0 && t.drop < 0 && locked != start_tenths) {
            t.drop = n + m;
        }
        if (t.drop >= 0 && locked == other_tenths && t.named_other < 0) {
            t.named_other = n + m;
        }
        if (t.drop >= 0 && locked == start_tenths && t.back < 0) {
            t.back = n + m;
        }
        if (locked != 0 && locked != start_tenths && locked != other_tenths && t.third < 0) {
            t.third = n + m;
        }
        t.final_state = o.state;
    }
    return t;
}

/*
 * 150.0 and 151.4 Hz hold within 0.7 Hz of their value, half the distance between them, not the
 * 0.8 Hz every other tone gets (dsd_analog_ctcss_tables::gate_hz): a locked one that moves to within
 * 0.8 Hz of it but nearer the other tone -- 150.76 and 150.64 Hz -- is dropped as a tone moved off
 * the table is. The other tone is then the nearest, 0.64 Hz off its value, which a transmitter's tone
 * error explains (issue #643): it is named once the late windows have refilled since the drop, at
 * +60 dB within the lock ceiling of the drop, and at +20 dB, where 0.64 Hz sits at the edge of what
 * the estimate's precision allows, named or left at "none" as these seeds have it. Never the first
 * tone again, and never a third. Beside them: 150.80 and 150.60 Hz, 0.6 Hz off the other tone, are
 * named at +30 dB; 150.72 and 150.68 Hz, 0.02 Hz from where the gates meet, are named as neither even
 * at +60 dB.
 */
static void
test_150_and_151_4_hold_only_their_own_side(void) {
    static const struct {
        double from_hz;
        double to_hz;
        double snr_db;
        int named; /**< 1 = the other tone must be named, 0 = nothing may be, -1 = either, as the seeds have it */
    } rows[] = {
        {150.0, 150.76, 60.0, 1}, {151.4, 150.64, 60.0, 1}, {150.0, 150.76, 20.0, -1}, {151.4, 150.64, 20.0, -1},
        {150.0, 150.80, 30.0, 1}, {151.4, 150.60, 30.0, 1}, {150.0, 150.72, 60.0, 0},  {151.4, 150.68, 60.0, 0},
    };

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        /* The original four rows keep the seeds they were pinned on. */
        const int m = rows[row].from_hz < 151.0 ? 0 : 1;
        const int s = rows[row].snr_db > 50.0 ? 0 : 1;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            const int fs = k_rates[ri];
            dsd_analog_rx_core_init(&g_core);
            signal_src src;
            signal_init(&src, fs, 7272ULL + (uint64_t)(m * 7 + s * 3 + ri) + (row >= 4 ? 1000ULL * row : 0ULL),
                        rows[row].from_hz, rows[row].snr_db);
            src.tone_on = 0;
            src.move_at = ms_to_samples(fs, 1000.0 + (double)(13 * m));
            src.move_hz = rows[row].to_hz;
            const int from = (int)lround(rows[row].from_hz * 10.0);
            const int to = from == 1500 ? 1514 : 1500;
            const move_trace t = run_move(&src, src.move_at + ms_to_samples(fs, 2000.0), fs / 1000, from, to);
            assert(t.first_lock >= 0 && t.first_lock < src.move_at);
            assert(t.drop > src.move_at);
            const double lost_ms = samples_to_ms(fs, t.drop - src.move_at);
            const double named_ms = t.named_other >= 0 ? samples_to_ms(fs, t.named_other - t.drop) : -1.0;
            if (lost_ms > (double)MOVED_OFF_BOUND_MS || t.back >= 0 || t.third >= 0) {
                DSD_FPRINTF(stderr, "held past the gate: fs=%d %.2f->%.2f -> %.0f ms, back=%lld third=%lld\n", fs,
                            rows[row].from_hz, rows[row].to_hz, lost_ms, (long long)t.back, (long long)t.third);
            }
            assert(lost_ms <= (double)MOVED_OFF_BOUND_MS);
            assert(t.back < 0 && t.third < 0);
            if (rows[row].named == 1) {
                if (!(named_ms >= 0.0 && named_ms <= (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS)) {
                    DSD_FPRINTF(stderr, "other tone not named: fs=%d %.2f->%.2f snr=%.0f -> %.0f ms\n", fs,
                                rows[row].from_hz, rows[row].to_hz, rows[row].snr_db, named_ms);
                }
                assert(named_ms >= 0.0 && named_ms <= (double)DSD_ANALOG_CTCSS_LOCK_CEILING_MS);
            } else if (rows[row].named == 0) {
                assert(t.named_other < 0 && t.final_state == DSD_ANALOG_TONE_STATE_NONE);
            } else {
                assert(t.named_other >= 0 || t.final_state == DSD_ANALOG_TONE_STATE_NONE);
            }
        }
    }
}

/* Fewest bits in which some rotation of @p word differs from @p code's word in either polarity. */
static int
dcs_distance_to_code(uint32_t word, int code) {
    int best = SYNTH_DCS_BITS;
    for (int inverted = 0; inverted < 2; inverted++) {
        const uint32_t target = dsd_dcs_word(code, inverted);
        for (int k = 0; k < SYNTH_DCS_BITS; k++) {
            uint32_t diff = synth_dcs_rotate(word, k) ^ target;
            int bits = 0;
            while (diff != 0U) {
                diff &= diff - 1U;
                bits++;
            }
            best = bits < best ? bits : best;
        }
    }
    return best;
}

/* Fewest bits in which some rotation of @p word differs from any supported code's word. */
static int
dcs_distance_to_supported(uint32_t word) {
    int best = SYNTH_DCS_BITS;
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        const int d = dcs_distance_to_code(word, dsd_dcs_code(i));
        best = d < best ? d : best;
    }
    return best;
}

/* The Golay (23,12) code's words, one per rotation class: every periodic waveform a DCS
   transmitter can send, whatever its code (issue #523 decodes them). */
enum { DCS_CLASS_MAX = 256 };

static int
dcs_rotation_classes(uint32_t* classes, int cap) {
    int count = 0;
    for (uint32_t m = 0; m < 4096U; m++) {
        const uint32_t c = synth_dcs_canonical(synth_golay23_word(m));
        int seen = 0;
        for (int i = 0; i < count; i++) {
            if (classes[i] == c) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            assert(count < cap);
            classes[count++] = c;
        }
    }
    return count;
}

static run_result
run_dcs(int fs, uint32_t word, double snr_db, uint64_t seed) {
    dsd_analog_rx_core_init(&g_core);
    signal_src src;
    signal_init(&src, fs, seed, 100.0, snr_db);
    if (snr_db > 100.0) {
        src.noise_sigma = 0.0;
    }
    src.dcs_on = 1;
    src.dcs.fs = fs;
    src.dcs.word = word;
    src.dcs.amp = 0.1;
    src.dcs.bit_phase = 0.0;
    return run_signal(&g_core, &src, ms_to_samples(fs, 3000.0), fs / 50, 0);
}

/*
 * DCS is never a CTCSS tone. A repeating 23-bit word at 134.4 bit/s is a line spectrum every
 * 5.84 Hz, and several lines land within the snap gate of a table tone; a run of alternating
 * bits is a 67.2 Hz square wave, 0.2 Hz from 67.0. Every rotation class of the code -- all
 * 178 non-constant periodic waveforms, which covers every DCS code in both polarities, since
 * a word's complement is a code word too -- sent forward and bit-reversed (the reciprocal
 * generator's code), for 3 s at 8 kHz, never locks a CTCSS tone: not in the publication, and not
 * in the CTCSS detector's own state either, where the DCS lock, which outranks it in the
 * publication, would hide one. Each ends as the DCS detector (issue #523) names it, with no lock
 * that comes and goes on the way: locked once on its code when it is a supported code's signal,
 * locked once on a supported code one bit away, the way a DCS decoder tolerates a bit error, or
 * never locked and positively "no tone". The words that come nearest (the most rho at a snapped
 * table tone), which are at least three bits from every supported code's word, never lock at
 * all at every rate, clean, at +10 dB and at 0 dB in-band, in both polarities.
 */
static void
test_dcs_never_locks(void) {
    static uint32_t classes[DCS_CLASS_MAX];
    const uint32_t all_ones = (1U << SYNTH_DCS_BITS) - 1U;
    const int count = dcs_rotation_classes(classes, DCS_CLASS_MAX);
    /* 23 is prime: every non-constant word rotates through 23 distinct words. */
    assert(count == 2 + ((4096 - 2) / SYNTH_DCS_BITS));
    for (int i = 0; i < count; i++) {
        if (classes[i] == 0U || classes[i] == all_ones) {
            continue; /* a constant level: DC, which the front end removes */
        }
        for (int reversed = 0; reversed < 2; reversed++) {
            const uint32_t word = reversed ? synth_dcs_reverse(classes[i]) : classes[i];
            const run_result r = run_dcs(8000, word, 200.0, 1ULL);
            if (r.ctcss_locks != 0 || r.ctcss_detector_locks != 0) {
                DSD_FPRINTF(stderr, "CTCSS lock on DCS: word 0x%06X\n", (unsigned int)word);
            }
            /* Neither published nor hidden behind the DCS lock that outranks it. */
            assert(r.ctcss_locks == 0 && r.ctcss_detector_locks == 0);
            int code = -1;
            int inverted = -1;
            if (dsd_dcs_match(word, &code, &inverted)) {
                assert(r.locks == 1 && r.final_state == DSD_ANALOG_TONE_STATE_LOCKED && r.final_dcs == 1);
                assert(g_core.dcs.code == code && g_core.dcs.inverted == inverted);
            } else if (r.final_dcs) {
                /* A word one bit from a supported code's may read as that code, the way a DCS
                   decoder tolerates a bit error; nothing further away does. */
                assert(r.locks == 1 && r.final_state == DSD_ANALOG_TONE_STATE_LOCKED);
                assert(dcs_distance_to_code(word, g_core.dcs.code) <= 1);
            } else {
                assert(r.locks == 0 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
            }
        }
    }
    static const uint32_t nearest[] = {0x5D5530U, 0x5559E8U};
    static const double snrs[] = {200.0, 10.0, 0.0};
    for (int w = 0; w < 2; w++) {
        for (int inverted = 0; inverted < 2; inverted++) {
            const uint32_t word = inverted ? (~nearest[w] & all_ones) : nearest[w];
            assert(dcs_distance_to_supported(word) >= 3);
            for (int ri = 0; ri < RATE_COUNT; ri++) {
                for (int s = 0; s < 3; s++) {
                    const run_result r = run_dcs(k_rates[ri], word, snrs[s], 23ULL + (uint64_t)(w * 12 + ri * 3 + s));
                    assert(r.locks == 0 && r.ctcss_detector_locks == 0);
                    assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
                }
            }
        }
    }
}

/*
 * No tone, bounded: a carrier with none reads "detecting" until 500 ms of it have been
 * evaluated and "none" by the next hop, at every rate. "None" is a verdict, not a latch: a
 * tone that starts after it -- a late encoder, a repeater that adds its tone after the
 * kerchunk -- still locks on its value within the lock bound of its own start.
 */
static void
test_no_tone_verdict_then_late_tone(void) {
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        dsd_analog_rx_core_init(&g_core);
        signal_src src;
        signal_init(&src, fs, 5150ULL + (uint64_t)ri, 136.5, 10.0);
        src.tone_on = ms_to_samples(fs, 1000.0);
        const run_result r =
            run_signal(&g_core, &src, src.tone_on + ms_to_samples(fs, LOCK_BOUND_MS + 150.0), fs / 1000, 1365);
        assert(r.last_acquiring >= ms_to_samples(fs, 450.0));
        assert(r.first_none > r.last_acquiring);
        assert(samples_to_ms(fs, r.first_none) <= (double)(DSD_ANALOG_CTCSS_NO_TONE_MS + HOP_MS) + 1.0);
        assert(r.first_wrong < 0 && r.first_lock > src.tone_on);
        assert(samples_to_ms(fs, r.first_lock - src.tone_on) <= (double)LOCK_BOUND_MS);
    }
}

/*
 * A minute of hostile speech -- unfiltered, so every voiced segment puts its fundamental
 * straight into the sub-audible band -- a minute of transmitter-filtered speech, and a minute
 * of noise: nothing may lock. No 250 ms detector can promise that for every voice ever
 * spoken (a steady voice fundamental with weak harmonics near the top of the table is a tone
 * for as long as it lasts), so these are fixed seeds; docs/testing.md gives the long-run rate
 * over an hour of each.
 */
static void
test_speech_and_noise_never_lock(void) {
    dsd_analog_rx_core_init(&g_core);
    signal_src src;
    signal_init(&src, 48000, 2718281ULL, 100.0, 0.0);
    src.noise_sigma = 0.0;
    src.voice_gain = 1.0;
    synth_speech_init(&src.speech, 48000, 2ULL, 0.05);
    run_result r = run_signal(&g_core, &src, ms_to_samples(48000, 60000.0), 960, 0);
    assert(r.locks == 0);
    /* Speech is positively "no tone" too, not left deciding. Its pauses close the carrier, and
       each new stretch of carrier reads "detecting" until its own verdict, so the run need not
       end on "none"; but it reaches that verdict, and holds it for most of the carrier time
       (87% on both these seeds). */
    assert(r.first_none >= 0 && 100 * r.none_blocks >= SPEECH_NONE_MIN_PCT * r.open_blocks);

    dsd_analog_rx_core_init(&g_core);
    signal_init(&src, 48000, 2718282ULL, 100.0, 0.0);
    src.noise_sigma = 0.0;
    src.voice_gain = 1.0;
    src.voice_filtered = 1;
    synth_speech_init(&src.speech, 48000, 3ULL, 0.05);
    synth_voice_hpf_init(&src.voice_hpf, 48000);
    r = run_signal(&g_core, &src, ms_to_samples(48000, 60000.0), 960, 0);
    assert(r.locks == 0);
    assert(r.first_none >= 0 && 100 * r.none_blocks >= SPEECH_NONE_MIN_PCT * r.open_blocks);

    dsd_analog_rx_core_init(&g_core);
    signal_init(&src, 48000, 1618033ULL, 100.0, 0.0);
    src.noise_sigma = 0.05;
    r = run_signal(&g_core, &src, ms_to_samples(48000, 60000.0), 960, 0);
    assert(r.locks == 0);
    /* Noise is positively "no tone", not "still deciding". */
    assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
}

/*
 * Two minutes of seeded speech, one through a transmitter's 300 Hz high-pass and one without,
 * in which a high voice holds its pitch near a table tone (225.7 and 229.1 Hz) for most of a
 * late acquisition window and then moves on. Over 400-600 ms that pitch averages out steady
 * enough to qualify the window; what keeps it from locking is that the newest 250 ms no longer
 * carry it. Neither minute locks anything.
 */
static void
test_speech_that_moved_on_never_locks(void) {
    static const struct {
        uint64_t signal_seed;
        uint64_t speech_seed;
        int filtered;
    } minutes[] = {
        {1311789ULL, 5452909ULL, 1},
        {979190ULL, 1054290ULL, 0},
    };

    for (size_t i = 0; i < sizeof(minutes) / sizeof(minutes[0]); i++) {
        dsd_analog_rx_core_init(&g_core);
        signal_src src;
        signal_init(&src, 48000, minutes[i].signal_seed, 100.0, 0.0);
        src.noise_sigma = 0.0;
        src.voice_gain = 1.0;
        src.voice_filtered = minutes[i].filtered;
        synth_speech_init(&src.speech, 48000, minutes[i].speech_seed, 0.05);
        if (minutes[i].filtered) {
            synth_voice_hpf_init(&src.voice_hpf, 48000);
        }
        const run_result r = run_signal(&g_core, &src, ms_to_samples(48000, 60000.0), 960, 0);
        if (r.locks != 0) {
            DSD_FPRINTF(stderr, "speech locked a tone: minute %zu\n", i);
        }
        assert(r.locks == 0);
    }
}

/*
 * A tone under a transmitter's voice (speech through a 300 Hz high-pass, 10 dB above the
 * tone): every tone locks on its value within 500 ms of its start, the voice never reads as
 * another tone, and on these seeds, once locked, the voice never knocks the lock out. Slower
 * than the noise bound although the voice's long-run share of the sub-audible band is 12-14 dB
 * below the tone: a high voice's fundamental still leaks through the high-pass in bursts,
 * right where the tone is -- which is also why, over 100 minutes of this speech, the two
 * highest table tones lost their lock briefly four times (docs/testing.md). Checks the lock
 * contract and prints the p50/p95/worst lock times for the PR evidence.
 */
static void
test_tone_under_voice_locks(void) {
    double times[SYNTH_LEGACY_CTCSS_COUNT + 1];
    /* The legacy table, then 150.0 Hz (k == SYNTH_LEGACY_CTCSS_COUNT). */
    for (int k = 0; k <= SYNTH_LEGACY_CTCSS_COUNT; k++) {
        const double hz = k < SYNTH_LEGACY_CTCSS_COUNT ? (double)synth_legacy_ctcss_tenths(k) / 10.0 : 150.0;
        dsd_analog_rx_core_init(&g_core);
        signal_src src;
        signal_init(&src, 48000, 55555ULL + (uint64_t)k, hz, 30.0);
        src.tone_on = ms_to_samples(48000, 200.0);
        src.voice_gain = 1.0;
        src.voice_filtered = 1;
        /* Level 0.76 puts the filtered speech's long-run RMS at 0.22, 10 dB above the
           tone's 0.1 amplitude. */
        synth_speech_init(&src.speech, 48000, 999ULL + (uint64_t)k, 0.76);
        synth_voice_hpf_init(&src.voice_hpf, 48000);
        const run_result r = run_signal(&g_core, &src, ms_to_samples(48000, 2500.0), 48, (int)lround(hz * 10.0));
        assert(r.first_wrong < 0);
        assert(r.first_lock >= 0);
        times[k] = samples_to_ms(48000, r.first_lock - src.tone_on);
        if (times[k] > (double)VOICE_LOCK_BOUND_MS) {
            DSD_FPRINTF(stderr, "slow lock under voice: hz=%.1f -> %.0f ms\n", hz, times[k]);
        }
        assert(times[k] <= (double)VOICE_LOCK_BOUND_MS);
        /* One lock, held to the end: the voice never read as a drop. */
        assert(r.locks == 1 && r.first_unlocked < 0);
        assert(r.final_state == DSD_ANALOG_TONE_STATE_LOCKED);
    }
    check_lock_contract("CTCSS lock under voice 10 dB above the tone", times, SYNTH_LEGACY_CTCSS_COUNT + 1);
}

/*
 * The tone stops while the carrier (noise) carries on: every tone at every rate, at +10 and
 * 0 dB in-band. The lock needs four failing hops, and a hop fails once the newest 100 ms no
 * longer carry the tone, so a stop is normally dropped 250-315 ms later. What is left after
 * the stop is noise, and at the locked bin noise alone clears the hold threshold on about one
 * hop in eighty, whatever its level; such a hop restarts the count, so a few stops in a
 * hundred take longer. Each row is a floor on the share of these 200 stops dropped within
 * 350 ms and a ceiling on the slowest, set at what these seeds do, and every row checks the loss
 * contract; the long-run shares are in docs/testing.md. Prints p50/p95/worst for the PR evidence.
 */
static void
test_tone_loss_within_bound(void) {
    static const struct {
        double snr_db;
        int min_within_bound_pct; /**< share of stops dropped within LOSS_BOUND_MS */
        int worst_ms;             /**< every stop is dropped within this */
    } rows[] = {
        {10.0, 98, 400},
        {0.0, 97, 500},
    };

    static double times[RATE_COUNT * SYNTH_LEGACY_CTCSS_COUNT];
    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        int count = 0;
        int within = 0;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            for (int k = 0; k < SYNTH_LEGACY_CTCSS_COUNT; k++) {
                const int fs = k_rates[ri];
                const double hz = (double)synth_legacy_ctcss_tenths(k) / 10.0;
                dsd_analog_rx_core_init(&g_core);
                signal_src src;
                signal_init(&src, fs, 90210ULL + (uint64_t)(k * 31 + ri) + (uint64_t)row * 104729ULL, hz,
                            rows[row].snr_db);
                src.tone_on = 0;
                src.tone_off = ms_to_samples(fs, 1000.0 + (double)((k * 11) % 50));
                const run_result r = run_signal(&g_core, &src, src.tone_off + ms_to_samples(fs, 800.0), fs / 1000,
                                                (int)lround(hz * 10.0));
                assert(r.first_lock >= 0 && r.first_lock < src.tone_off);
                assert(r.first_unlocked > src.tone_off);
                const double loss_ms = samples_to_ms(fs, r.first_unlocked - src.tone_off);
                if (loss_ms > (double)rows[row].worst_ms) {
                    DSD_FPRINTF(stderr, "slow loss: fs=%d hz=%.1f snr=%.0f -> %.0f ms\n", fs, hz, rows[row].snr_db,
                                loss_ms);
                }
                assert(loss_ms <= (double)rows[row].worst_ms);
                within += loss_ms <= (double)LOSS_BOUND_MS ? 1 : 0;
                /* Carrier still up, no tone: "none", positively, and nothing locked again from what
                   the detector still held of the stopped tone. */
                assert(r.locks == 1 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
                times[count++] = loss_ms;
            }
        }
        char what[96];
        DSD_SNPRINTF(what, sizeof(what), "CTCSS loss after the tone stops at %+.0f dB in-band (%d%% within %d ms)",
                     rows[row].snr_db, (100 * within) / count, LOSS_BOUND_MS);
        check_loss_contract(what, times, count);
        assert(100 * within >= rows[row].min_within_bound_pct * count);
    }
}

/*
 * A carrier that keeps dropping out, each time for less than the hangover, never expires, so
 * only the detector can end a lock under it. The tone stops, and from then on the carrier is
 * up for 20 ms of every 40-180 ms -- a voice-band tone and noise, no sub-audible tone -- with
 * digital silence between. However those openings fall between the hops, and whatever the
 * block size, the lock ends within the loss ceiling while the carrier stays open throughout,
 * and nothing locks again. A detector that let every hop closing inside a dropout keep its
 * verdict would keep the stopped tone for good whenever the openings missed every hop's end
 * (20 ms of every 100 ms at 8 kHz here). The p95 target is for a live carrier, so the row holds
 * every stop to the ceiling only. Prints p50/p95/worst.
 */
static void
test_tone_stop_under_a_flickering_carrier(void) {
    static const int periods_ms[] = {40, 60, 100, 140, 180};

    enum { PERIOD_COUNT = (int)(sizeof(periods_ms) / sizeof(periods_ms[0])), OFFSETS = 5 };

    static double times[RATE_COUNT * PERIOD_COUNT * OFFSETS];
    int count = 0;
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        for (int pi = 0; pi < PERIOD_COUNT; pi++) {
            for (int oi = 0; oi < OFFSETS; oi++) {
                const int fs = k_rates[ri];
                const int k = (ri * 17 + pi * 7 + oi * 3) % SYNTH_LEGACY_CTCSS_COUNT;
                const double hz = (double)synth_legacy_ctcss_tenths(k) / 10.0;
                /* 1 ms blocks, and the 20 ms blocks an RTL stream delivers, alternately. */
                const int block = (oi % 2 == 0) ? fs / 1000 : fs / 50;
                dsd_analog_rx_core_init(&g_core);
                signal_src src;
                signal_init(&src, fs, 5550001ULL + (uint64_t)(ri * 1009 + pi * 101 + oi), hz, 10.0);
                src.tone_on = 0;
                src.tone_off = ms_to_samples(fs, 800.0 + (double)(oi * 10));
                src.flicker_from = src.tone_off;
                src.flicker_period = ms_to_samples(fs, (double)periods_ms[pi]);
                src.flicker_open = ms_to_samples(fs, 20.0);
                src.steady.fs = fs;
                src.steady.hz = 1000.0;
                src.steady.amp = src.tone.amp;
                const run_result r =
                    run_signal(&g_core, &src, src.tone_off + ms_to_samples(fs, 1500.0), block, (int)lround(hz * 10.0));
                assert(r.first_wrong < 0);
                assert(r.first_lock >= 0 && r.first_lock < src.tone_off);
                /* Every dropout is shorter than the hangover: the carrier never read closed. */
                assert(r.first_closed < 0);
                if (r.first_unlocked < 0) {
                    DSD_FPRINTF(stderr, "stopped tone held under a flickering carrier: fs=%d hz=%.1f period=%d ms\n",
                                fs, hz, periods_ms[pi]);
                }
                assert(r.first_unlocked > src.tone_off);
                const double loss_ms = samples_to_ms(fs, r.first_unlocked - src.tone_off);
                if (loss_ms > (double)DSD_ANALOG_CTCSS_LOSS_CEILING_MS) {
                    DSD_FPRINTF(stderr, "slow loss under a flickering carrier: fs=%d hz=%.1f period=%d ms -> %.0f ms\n",
                                fs, hz, periods_ms[pi], loss_ms);
                }
                assert(loss_ms <= (double)DSD_ANALOG_CTCSS_LOSS_CEILING_MS);
                assert(r.locks == 1 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
                times[count++] = loss_ms;
            }
        }
    }
    qsort(times, (size_t)count, sizeof(times[0]), compare_doubles);
    printf("CTCSS loss under a flickering carrier: p50 %.0f ms, p95 %.0f ms, worst %.0f ms (%d cases)\n",
           times[count / 2], times[(count * 95) / 100], times[count - 1], count);
    (void)fflush(stdout);
}

/* A reverse burst -- the transmitter stepping its tone's phase before it unkeys -- ends the
   lock within 150 ms, without waiting for the tone to stop: every tone at every rate at
   +10 dB in-band, for each variant in use: 180 degrees, and 120 and 240 degrees. The flip lands
   anywhere inside a sub-block; a 120 or 240 degree step early or late in one leaves that
   sub-block strong with part of the step in its phase, the case a burst reference taken from
   the newest estimate missed. Nearer 0 dB a sub-block is too noisy to serve as the phase
   reference on every hop, and a burst can be caught late or missed, when the carrier drop that
   follows ends the lock instead (test_reverse_burst_at_0db_ends_the_lock). A caught burst ends
   the lock as a stop does, so each row checks the loss contract too. */
static void
test_reverse_burst_drops_fast(void) {
    static const int steps_deg[] = {180, 120, 240};
    static double times[RATE_COUNT * SYNTH_LEGACY_CTCSS_COUNT];
    for (size_t v = 0; v < sizeof(steps_deg) / sizeof(steps_deg[0]); v++) {
        int count = 0;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            for (int k = 0; k < SYNTH_LEGACY_CTCSS_COUNT; k++) {
                const int fs = k_rates[ri];
                const double hz = (double)synth_legacy_ctcss_tenths(k) / 10.0;
                dsd_analog_rx_core_init(&g_core);
                signal_src src;
                signal_init(&src, fs, 8675309ULL + (uint64_t)(k * 13 + ri) + ((uint64_t)v * 7001ULL), hz, 10.0);
                src.tone_on = 0;
                src.flip_at = ms_to_samples(fs, 1000.0 + (double)((k * 17) % 50));
                src.flip_rad = (double)steps_deg[v] * M_PI / 180.0;
                const run_result r = run_signal(&g_core, &src, src.flip_at + ms_to_samples(fs, 180.0), fs / 1000,
                                                (int)lround(hz * 10.0));
                assert(r.first_lock >= 0 && r.first_lock < src.flip_at);
                assert(r.first_unlocked > src.flip_at);
                const double loss_ms = samples_to_ms(fs, r.first_unlocked - src.flip_at);
                if (loss_ms > (double)BURST_LOSS_BOUND_MS) {
                    DSD_FPRINTF(stderr, "slow burst: %d degrees fs=%d hz=%.1f -> %.0f ms\n", steps_deg[v], fs, hz,
                                loss_ms);
                }
                assert(loss_ms <= (double)BURST_LOSS_BOUND_MS);
                times[count++] = loss_ms;
            }
        }
        char what[64];
        DSD_SNPRINTF(what, sizeof(what), "CTCSS loss on a %d degree reverse burst", steps_deg[v]);
        check_loss_contract(what, times, count);
    }
}

/* The CTCSS detector on its own, fed a decimated stream directly so that a phase step lands
   exactly where a test puts it: 50 ms sub-blocks of 120 samples at 2400 Hz. */
enum { DIRECT_RATE_HZ = 2400, DIRECT_SUB_LEN = (DIRECT_RATE_HZ * DSD_ANALOG_CTCSS_SUBBLOCK_MS) / 1000 };

typedef struct {
    dsd_analog_ctcss det;
    synth_tone tone;
    synth_rng rng;
    double sigma;
} direct_run;

/* A fresh detector and a tone at @p hz in white noise giving @p snr_db of in-band tone-to-noise. */
static void
direct_start(direct_run* run, uint64_t seed, double hz, double snr_db) {
    dsd_analog_ctcss_ops.configure(&run->det, (double)DIRECT_RATE_HZ);
    synth_rng_seed(&run->rng, seed);
    run->tone.fs = (double)DIRECT_RATE_HZ;
    run->tone.hz = hz;
    run->tone.amp = 0.1;
    run->tone.phase = synth_range(&run->rng, 0.0, 2.0 * M_PI);
    run->sigma = synth_inband_noise_sigma(run->tone.amp, snr_db, (double)DIRECT_RATE_HZ);
}

/* Feed one sub-block, the tone's phase stepping by @p step_rad at sample @p step_at of it (-1 =
   none). The wide stream is the same signal and the full stream its power, as the front end
   hands them over for a signal with nothing above the band. Returns the locked tone in tenths
   of a hertz, or 0. */
static int
direct_subblock(direct_run* run, int step_at, double step_rad) {
    float band[DIRECT_SUB_LEN];
    float full[DIRECT_SUB_LEN];
    for (int i = 0; i < DIRECT_SUB_LEN; i++) {
        if (i == step_at) {
            run->tone.phase += step_rad;
        }
        const double v = (double)synth_tone_next(&run->tone) + (run->sigma * synth_gauss(&run->rng));
        band[i] = (float)v;
        full[i] = (float)(v * v);
    }
    dsd_analog_ctcss_ops.process(&run->det, band, band, full, DIRECT_SUB_LEN, 0);
    dsd_analog_rx_report report;
    dsd_analog_ctcss_ops.report(&run->det, &report);
    return report.state == DSD_ANALOG_TONE_STATE_LOCKED ? report.ctcss_tenths_hz : 0;
}

/* What reverse bursts inside the sub-block a lock is made on did. */
typedef struct {
    int tones;   /**< seeded tones run */
    int relocks; /**< ... whose lock replaced another tone's on the same hop */
    int stepped; /**< steps tried */
    int locked;  /**< ... after which the tone still locked on that sub-block's hop */
    int missed;  /**< ... and the burst then did not end the lock within BURST_LOSS_BOUND_MS */
} burst_at_lock;

/*
 * Run a tone at @p hz from @p seed -- as @p lead_hz for its first @p lead_subblocks (0 = none),
 * then phase-continuously at @p hz -- until a hop locks it as @p expect_tenths. Then replay the
 * sub-block that hop closed with the phase stepping by @p step_rad at every @p stride samples of
 * it, and follow each step that still locks the tone on that hop through three more hops.
 */
static void
run_burst_at_lock(burst_at_lock* acc, uint64_t seed, double lead_hz, int lead_subblocks, double hz, int expect_tenths,
                  double step_rad, int stride) {
    static direct_run run;
    static direct_run lock_start; /* the run as the sub-block that locks begins */
    direct_start(&run, seed, lead_subblocks > 0 ? lead_hz : hz, 10.0);
    int tenths = 0;
    int tenths_before = 0;
    for (int s = 0; s < 40 && tenths != expect_tenths; s++) {
        if (s == lead_subblocks) {
            run.tone.hz = hz;
        }
        tenths_before = tenths;
        lock_start = run;
        tenths = direct_subblock(&run, -1, 0.0);
    }
    assert(tenths == expect_tenths);
    acc->tones++;
    acc->relocks += tenths_before != 0 ? 1 : 0;
    for (int at = stride / 2; at < DIRECT_SUB_LEN; at += stride) {
        run = lock_start;
        acc->stepped++;
        if (direct_subblock(&run, at, step_rad) != expect_tenths) {
            /* The step spoiled the hop's window, so the tone did not lock on it. */
            continue;
        }
        acc->locked++;
        int ended = 0;
        for (int hop = 1; hop <= 3 && ended == 0; hop++) {
            if (direct_subblock(&run, -1, 0.0) != expect_tenths) {
                ended = hop;
            }
        }
        const double loss_ms =
            samples_to_ms(DIRECT_RATE_HZ, (int64_t)(DIRECT_SUB_LEN - at) + ((int64_t)ended * DIRECT_SUB_LEN));
        if (ended == 0 || loss_ms > (double)BURST_LOSS_BOUND_MS) {
            acc->missed++;
        }
    }
}

/*
 * A reverse burst inside the sub-block a lock is made on, as when a transmitter unkeys right
 * after its tone is confirmed. The hop that locks holds part of a 120 or 240 degree step in its
 * newest sub-block and fits it as a steeper slope, so the next hop's burst check cannot measure
 * against that hop's estimate: it takes the candidate's estimate from the hop before, whose
 * window ends a sub-block earlier. Driven through the detector alone at +10 dB in-band, every
 * tone on its table value and 0.15 Hz either side, the step at every eighth sample of the
 * sub-block: of the steps that still lock the tone on that hop, the burst ends the lock within
 * 150 ms on all but BURST_AT_LOCK_MISSED_MAX. The second row does the same for a lock that
 * replaces another tone's on the same hop (the tone moves to a new table value while locked).
 * Measured against the lock hop's own estimate, 115 of 2,882 locks and 143 of 2,852 relocks
 * here missed the burst.
 */
static void
test_reverse_burst_inside_the_lock_subblock(void) {
    static const int steps_deg[] = {120, 240};
    static const char* const rows[] = {"lock", "relock"};
    for (int row = 0; row < 2; row++) {
        burst_at_lock acc = {0, 0, 0, 0, 0};
        for (size_t v = 0; v < sizeof(steps_deg) / sizeof(steps_deg[0]); v++) {
            for (int k = 0; k < SYNTH_LEGACY_CTCSS_COUNT; k++) {
                for (int off = -1; off <= 1; off++) {
                    const int tenths = synth_legacy_ctcss_tenths(k);
                    const double hz = ((double)tenths / 10.0) + (0.15 * (double)off);
                    const double lead_hz =
                        (double)synth_legacy_ctcss_tenths((k + 25) % SYNTH_LEGACY_CTCSS_COUNT) / 10.0;
                    const uint64_t seed =
                        (1000003ULL * (uint64_t)(k + 1)) + (7919ULL * (uint64_t)(off + 3)) + ((uint64_t)v * 104729ULL);
                    run_burst_at_lock(&acc, seed, lead_hz, row == 0 ? 0 : 20, hz, tenths,
                                      (double)steps_deg[v] * M_PI / 180.0, 8);
                }
            }
        }
        printf("CTCSS reverse burst inside the sub-block of a %s at +10 dB in-band: %d of %d steps locked, %d not "
               "caught within %d ms (%d of %d tones relocked)\n",
               rows[row], acc.locked, acc.stepped, acc.missed, BURST_LOSS_BOUND_MS, acc.relocks, acc.tones);
        (void)fflush(stdout);
        /* The rows exercise what they name: most steps still lock, and on the relock row the new
           tone mostly replaces the old one on the same hop rather than after losing it. */
        assert(2 * acc.locked >= acc.stepped);
        assert(row == 0 ? acc.relocks == 0 : 4 * acc.relocks >= 3 * acc.tones);
        assert(acc.missed <= (row == 0 ? BURST_AT_LOCK_MISSED_MAX : BURST_AT_RELOCK_MISSED_MAX));
    }
}

/*
 * A reverse burst at 0 dB in-band: every tone at every rate, the flipped tone held under a live
 * carrier for 400 ms -- longer than a transmitter sends one, so that a late catch still shows --
 * and then the carrier drops. A sub-block this noisy cannot serve as the phase reference on
 * every hop, so a burst can be caught late or missed. A caught burst ends the lock as a stop
 * does, and the caught ones check the loss contract; a missed one leaves the lock to the
 * carrier drop, which ends it within the 200 ms hangover. The row is a floor on the share
 * caught within 150 ms and a ceiling on the number missed, set at what these seeds do (91%, and
 * 4 of 200); the long-run shares are in docs/testing.md. Prints p50/p95/worst of the caught ones.
 */
static void
test_reverse_burst_at_0db_ends_the_lock(void) {
    static double caught[RATE_COUNT * SYNTH_LEGACY_CTCSS_COUNT];
    int count = 0;
    int within = 0;
    int missed = 0;
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        for (int k = 0; k < SYNTH_LEGACY_CTCSS_COUNT; k++) {
            const int fs = k_rates[ri];
            const double hz = (double)synth_legacy_ctcss_tenths(k) / 10.0;
            dsd_analog_rx_core_init(&g_core);
            signal_src src;
            signal_init(&src, fs, 8675309ULL + (uint64_t)(k * 13 + ri) + 104729ULL, hz, 0.0);
            src.tone_on = 0;
            src.flip_at = ms_to_samples(fs, 1000.0 + (double)((k * 17) % 50));
            src.carrier_off = src.flip_at + ms_to_samples(fs, BURST_0DB_CARRIER_MS);
            const run_result r =
                run_signal(&g_core, &src, src.carrier_off + ms_to_samples(fs, DSD_ANALOG_CARRIER_HANGOVER_MS + 50.0),
                           fs / 1000, (int)lround(hz * 10.0));
            assert(r.first_lock >= 0 && r.first_lock < src.flip_at && r.first_wrong < 0);
            assert(r.first_unlocked > src.flip_at);
            /* Whichever ended it, the carrier drop leaves no tone behind. */
            assert(r.final_state == DSD_ANALOG_TONE_STATE_IDLE);
            if (r.first_unlocked <= src.carrier_off) {
                const double loss_ms = samples_to_ms(fs, r.first_unlocked - src.flip_at);
                within += loss_ms <= (double)BURST_LOSS_BOUND_MS ? 1 : 0;
                caught[count++] = loss_ms;
                continue;
            }
            missed++;
            /* The hangover counts closed blocks, and the block the drop falls in still carries
               signal, so the lock ends up to a block after the hangover and shows a block later. */
            const double after_drop_ms = samples_to_ms(fs, r.first_unlocked - src.carrier_off);
            const double block_ms = samples_to_ms(fs, fs / 1000);
            if (after_drop_ms > (double)DSD_ANALOG_CARRIER_HANGOVER_MS + (2.0 * block_ms)) {
                DSD_FPRINTF(stderr, "missed burst held: fs=%d hz=%.1f -> %.0f ms after the carrier drop\n", fs, hz,
                            after_drop_ms);
            }
            assert(after_drop_ms <= (double)DSD_ANALOG_CARRIER_HANGOVER_MS + (2.0 * block_ms));
        }
    }
    const int total = count + missed;
    char what[128];
    DSD_SNPRINTF(what, sizeof(what),
                 "CTCSS loss on a reverse burst at +0 dB in-band, caught (%d%% within %d ms, %d of %d missed)",
                 (100 * within) / total, BURST_LOSS_BOUND_MS, missed, total);
    check_loss_contract(what, caught, count);
    assert(100 * within >= BURST_0DB_WITHIN_BOUND_MIN_PCT * total);
    assert(missed <= BURST_0DB_MISSED_MAX);
}

/* A held tone at 0 dB in-band stays held: one lock per 15 s run and never lost, at every
   rate (over 4,000 s of such holds the lock never dropped; docs/testing.md). */
static void
test_lock_holds_at_0db(void) {
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        const int k = 4 + (ri * 13);
        const double hz = (double)synth_legacy_ctcss_tenths(k) / 10.0;
        dsd_analog_rx_core_init(&g_core);
        signal_src src;
        signal_init(&src, fs, 7700001ULL + (uint64_t)(ri * 131), hz, 0.0);
        src.tone_on = 0;
        const run_result r = run_signal(&g_core, &src, ms_to_samples(fs, 15000.0), fs / 50, (int)lround(hz * 10.0));
        assert(r.first_wrong < 0 && r.first_lock >= 0);
        assert(r.locks == 1 && r.first_unlocked < 0);
        assert(r.final_state == DSD_ANALOG_TONE_STATE_LOCKED);
    }
}

/* Every metric is a ratio, so the RTL live scale (about 1/pi), the unscaled replay and
   int16-scale PCM all lock the same tone on the same hop. */
static void
test_scale_invariance(void) {
    static const double scales[] = {1.0, 1.0 / M_PI, 32768.0};
    for (int k = 0; k < SYNTH_LEGACY_CTCSS_COUNT; k += 5) {
        const double hz = (double)synth_legacy_ctcss_tenths(k) / 10.0;
        int64_t first_lock[3];
        for (int si = 0; si < 3; si++) {
            dsd_analog_rx_core_init(&g_core);
            signal_src src;
            signal_init(&src, 48000, 314159ULL + (uint64_t)k, hz, 10.0);
            src.tone_on = ms_to_samples(48000, 250.0);
            src.scale = scales[si];
            const run_result r = run_signal(&g_core, &src, ms_to_samples(48000, 900.0), 480, (int)lround(hz * 10.0));
            assert(r.first_wrong < 0 && r.first_lock >= 0);
            first_lock[si] = r.first_lock;
        }
        assert(first_lock[0] == first_lock[1] && first_lock[0] == first_lock[2]);
    }
}

/* The verdict changes on 50 ms hops of sample time, so how the input is cut into blocks
   (20 ms RTL blocks, 960-sample PCM blocks, anything else) never moves it. Blocks are at
   least a fraction of a millisecond: carrier presence is judged per block, and a single
   sample at a zero crossing is not a block anyone delivers. */
static void
test_block_size_does_not_move_the_verdict(void) {
    static const int blocks[] = {37, 480, 960, 4096};
    int64_t hop_lock[4];
    for (int b = 0; b < 4; b++) {
        dsd_analog_rx_core_init(&g_core);
        signal_src src;
        signal_init(&src, 48000, 161803ULL, 123.0, 10.0);
        src.tone_on = ms_to_samples(48000, 310.0);
        const run_result r = run_signal(&g_core, &src, ms_to_samples(48000, 1000.0), blocks[b], 1230);
        assert(r.first_lock >= 0);
        /* Hops close every 2400 input samples at 48 kHz; a block that ends on or after one
           sees it, so flooring the observation recovers the hop for any block up to a hop. */
        hop_lock[b] = r.first_lock / 2400;
    }
    /* A 4096-sample block can only observe the lock up to one block late. */
    assert(hop_lock[0] == hop_lock[1] && hop_lock[0] == hop_lock[2]);
    assert(hop_lock[3] >= hop_lock[0] && hop_lock[3] <= hop_lock[0] + 2);

    /* The no-tone verdict too: a carrier with no tone reads "none" on the same hop. */
    int64_t hop_none[4];
    for (int b = 0; b < 4; b++) {
        dsd_analog_rx_core_init(&g_core);
        signal_src src;
        signal_init(&src, 48000, 271828ULL, 123.0, 10.0);
        const run_result r = run_signal(&g_core, &src, ms_to_samples(48000, 1000.0), blocks[b], 1230);
        assert(r.first_none >= 0 && r.locks == 0);
        hop_none[b] = r.first_none / 2400;
    }
    assert(hop_none[0] == hop_none[1] && hop_none[0] == hop_none[2]);
    assert(hop_none[3] >= hop_none[0] && hop_none[3] <= hop_none[0] + 2);
}

/* Carrier bookkeeping: a short fade keeps the tone, 200 ms without carrier forgets it. */
static void
test_carrier_hangover(void) {
    const int fs = 48000;
    float tone_block[960];
    float silent_block[960];
    DSD_MEMSET(silent_block, 0, sizeof(silent_block));
    dsd_analog_rx_core_init(&g_core);
    synth_tone tone = {fs, 100.0, 0.1, 0.0};
    for (int b = 0; b < 40; b++) {
        for (int i = 0; i < 960; i++) {
            tone_block[i] = synth_tone_next(&tone);
        }
        assert(dsd_analog_rx_core_process(&g_core, tone_block, 960, fs, 1) == 1);
    }
    observation o = observe(&g_core);
    assert(o.state == DSD_ANALOG_TONE_STATE_LOCKED && o.tenths == 1000 && o.carrier == 1);

    /* 100 ms of closed squelch (zeroed blocks), then the tone again: still locked. The tone
       keeps its phase through the fade, as a transmitter's does. */
    for (int b = 0; b < 5; b++) {
        for (int i = 0; i < 960; i++) {
            (void)synth_tone_next(&tone);
        }
        assert(dsd_analog_rx_core_process(&g_core, silent_block, 960, fs, 1) == 1);
        o = observe(&g_core);
        assert(o.state == DSD_ANALOG_TONE_STATE_LOCKED && o.carrier == 1);
    }
    for (int b = 0; b < 10; b++) {
        for (int i = 0; i < 960; i++) {
            tone_block[i] = synth_tone_next(&tone);
        }
        assert(dsd_analog_rx_core_process(&g_core, tone_block, 960, fs, 1) == 1);
    }
    o = observe(&g_core);
    assert(o.state == DSD_ANALOG_TONE_STATE_LOCKED && o.tenths == 1000);

    /* The caller's squelch reading counts as closed too; 200 ms of it forgets the tone. */
    dsd_analog_rx_publication before;
    dsd_analog_rx_core_publish(&g_core, &before);
    for (int b = 0; b < 9; b++) {
        assert(dsd_analog_rx_core_process(&g_core, tone_block, 960, fs, 0) == 1);
        o = observe(&g_core);
        assert(o.state == DSD_ANALOG_TONE_STATE_LOCKED);
    }
    assert(dsd_analog_rx_core_process(&g_core, tone_block, 960, fs, 0) == 1);
    o = observe(&g_core);
    assert(o.state == DSD_ANALOG_TONE_STATE_IDLE && o.carrier == 0 && o.tenths == 0);
    dsd_analog_rx_publication after;
    dsd_analog_rx_core_publish(&g_core, &after);
    assert(after.generation != before.generation);
}

/* Below 2.4 kHz there is no sub-audible band to keep: detection says so and stays out. A core
   that has seen no block yet is INACTIVE; one designed for an unusable rate is UNAVAILABLE. The
   carrier is kept at every rate all the same (issue #526: the scanners hold an analog row on it),
   above the level floor and through the hangover, 400 samples at 2000 Hz. */
static void
test_unusable_rate(void) {
    float block[100];
    DSD_MEMSET(block, 0, sizeof(block));
    block[0] = 1.0f;
    dsd_analog_rx_core_init(&g_core);
    assert(observe(&g_core).state == DSD_ANALOG_TONE_STATE_INACTIVE);
    assert(dsd_analog_rx_core_process(&g_core, block, 100, 2000, 1) == 0);
    const observation o = observe(&g_core);
    assert(o.state == DSD_ANALOG_TONE_STATE_UNAVAILABLE && o.carrier == 1);
    float silence[100];
    DSD_MEMSET(silence, 0, sizeof(silence));
    for (int i = 0; i < 3; i++) {
        assert(dsd_analog_rx_core_process(&g_core, block, 100, 2000, 0) == 0);
        assert(observe(&g_core).carrier == 1);
    }
    assert(dsd_analog_rx_core_process(&g_core, block, 100, 2000, 0) == 0);
    assert(observe(&g_core).carrier == 0 && observe(&g_core).state == DSD_ANALOG_TONE_STATE_UNAVAILABLE);
    /* An open squelch over a block below the floor is no carrier either. */
    assert(dsd_analog_rx_core_process(&g_core, silence, 100, 2000, 1) == 0);
    assert(observe(&g_core).carrier == 0);
    assert(dsd_analog_rx_core_process(&g_core, block, 100, 2000, 1) == 0);
    assert(observe(&g_core).carrier == 1);
    /* A reset keeps the rate design, and with it the verdict. */
    dsd_analog_rx_core_reset(&g_core);
    assert(observe(&g_core).state == DSD_ANALOG_TONE_STATE_UNAVAILABLE);
    /* A usable rate afterwards redesigns and runs. */
    assert(dsd_analog_rx_core_process(&g_core, block, 100, 8000, 1) == 1);
    assert(g_core.fe.active == 1 && g_core.fe.decim == 3);
    /* The front end's filters are sized up to DSD_ANALOG_RX_MAX_RATE_HZ, and above it the
       core says so rather than running half a design. */
    assert(dsd_analog_rx_core_process(&g_core, block, 100, DSD_ANALOG_RX_MAX_RATE_HZ, 1) == 1);
    assert(g_core.fe.active == 1 && g_core.fe.n1 > 0);
    assert(dsd_analog_rx_core_process(&g_core, block, 100, 384000, 1) == 0);
    assert(observe(&g_core).state == DSD_ANALOG_TONE_STATE_UNAVAILABLE && observe(&g_core).carrier == 1);
}

/* A window of silence fed straight to the detector leaves every correlator bin empty. The
   fit then has nothing to explain, and the hop must still come out fully defined: every
   sub-block unexplained, so the chi-square is at least what uniformly random phases give
   (pi^2 against a variance of at most pi^2 / 3 in each of the window's sub-blocks). */
static void
test_silent_window_is_defined_and_rejected(void) {
    static dsd_analog_ctcss det;
    const double rate_hz = 2400.0;
    const int sub_len = (int)lround(rate_hz * (double)DSD_ANALOG_CTCSS_SUBBLOCK_MS / 1000.0);
    float silence[120];
    assert(sub_len == 120);
    DSD_MEMSET(silence, 0, sizeof(silence));
    dsd_analog_ctcss_ops.configure(&det, rate_hz);
    for (int s = 0; s < 2 * DSD_ANALOG_CTCSS_WINDOW; s++) {
        dsd_analog_ctcss_ops.process(&det, silence, silence, silence, sub_len, 0);
    }
    const dsd_analog_ctcss_hop* hop = dsd_analog_ctcss_last_hop(&det);
    assert(hop->evaluated == 1);
    assert(fabs(hop->rho) < 1e-12);
    assert(fabs(hop->share) < 1e-12);
    assert(fabs(hop->residual - M_PI) < 1e-12);
    const double random_phase_chi2 = 3.0 * (double)DSD_ANALOG_CTCSS_WINDOW / (double)(DSD_ANALOG_CTCSS_WINDOW - 2);
    assert(isfinite(hop->chi2) && hop->chi2 >= random_phase_chi2 - 1e-9);
    dsd_analog_rx_report report;
    dsd_analog_ctcss_ops.report(&det, &report);
    assert(report.state != DSD_ANALOG_TONE_STATE_LOCKED && report.kind == DSD_ANALOG_TONE_KIND_NONE);
}

int
main(void) {
    test_silent_window_is_defined_and_rejected();
    test_unusable_rate();
    test_carrier_hangover();
    test_block_size_does_not_move_the_verdict();
    test_scale_invariance();
    test_adjacent_low_tones_are_distinguished();
    test_unsupported_frequencies_never_lock();
    test_voice_band_tones_never_lock();
    test_tone_beside_a_loud_voice_band_tone_locks();
    test_tone_stop_beside_a_voice_band_tone_drops();
    test_lock_follows_a_tone_off_the_table();
    test_dcs_never_locks();
    test_no_tone_verdict_then_late_tone();
    test_tone_loss_within_bound();
    test_tone_stop_under_a_flickering_carrier();
    test_reverse_burst_drops_fast();
    test_reverse_burst_inside_the_lock_subblock();
    test_reverse_burst_at_0db_ends_the_lock();
    test_tone_under_voice_locks();
    test_lock_holds_at_0db();
    test_every_tone_locks_within_bound();
    test_off_nominal_tones_lock();
    test_tone_150_locks_within_bound();
    test_150_and_151_4_never_cross();
    test_pair_lean_waits_one_hop();
    test_pair_starts_that_named_the_other_tone();
    test_150_and_151_4_onset_in_noise();
    test_150_and_151_4_handover();
    test_pair_slow_starts_lock_within_ceiling();
    test_150_and_151_4_hold_only_their_own_side();
    test_snap_gates();
    test_tone_150_loss_and_burst();
    test_late_onsets_lock_within_ceiling();
    test_speech_and_noise_never_lock();
    test_speech_that_moved_on_never_locks();
    return 0;
}
