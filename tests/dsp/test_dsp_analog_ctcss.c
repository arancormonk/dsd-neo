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
#include <dsd-neo/runtime/analog_tones.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "analog_rx_internal.h"
#include "analog_tone_synth.h"

/* The bounds under test (docs/cli.md "Received tone"). */
enum {
    LOCK_BOUND_MS = 400,
    LOSS_BOUND_MS = 350,
    BURST_LOSS_BOUND_MS = 150,
};

static const int k_rates[] = {8000, 44100, 48000, 78125};
#define RATE_COUNT ((int)(sizeof(k_rates) / sizeof(k_rates[0])))

/* One observation of the core after a block. */
typedef struct {
    int state;
    int tenths;
    int carrier;
} observation;

static observation
observe(const dsd_analog_rx_core* core) {
    dsd_analog_rx_publication pub;
    dsd_analog_rx_core_publish(core, &pub);
    observation o = {pub.tone_state, pub.ctcss_tenths_hz, pub.carrier_open};
    if (pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED) {
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
    int64_t tone_on;     /**< first sample carrying the tone */
    int64_t tone_off;    /**< first sample without it (INT64_MAX = never) */
    int64_t flip_at;     /**< reverse burst: the tone's phase flips by pi here (INT64_MAX = never) */
    double scale;        /**< input scale: 1, 1/pi, 32768 */
    synth_speech speech; /**< optional voice */
    synth_voice_hpf voice_hpf;
    double voice_gain;  /**< 0 = no voice */
    int voice_filtered; /**< 1 = through the transmitter's 300 Hz high-pass */
};

static float
signal_next(signal_src* src, int64_t n) {
    double v = src->noise_sigma > 0.0 ? src->noise_sigma * synth_gauss(&src->rng) : 0.0;
    if (n == src->flip_at) {
        src->tone.phase += M_PI;
    }
    if (n >= src->tone_on && n < src->tone_off) {
        v += synth_tone_next(&src->tone);
    }
    if (src->voice_gain > 0.0) {
        float voice = synth_speech_next(&src->speech);
        if (src->voice_filtered) {
            voice = synth_voice_hpf_next(&src->voice_hpf, voice);
        }
        v += src->voice_gain * voice;
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
    src->scale = 1.0;
}

/* What a run saw: when each thing first happened, in samples from the start. -1 = never. */
typedef struct {
    int64_t first_lock;     /**< first block end at which the expected tone was locked */
    int64_t first_wrong;    /**< first block end at which any other tone was locked */
    int64_t first_unlocked; /**< first block end, after first_lock, without the lock */
    int64_t locks;          /**< transitions into LOCKED */
    int final_state;
} run_result;

/*
 * Feed @p total samples in blocks of @p block, observing after each. Small blocks give the
 * lock time to within one block; the core itself only changes its verdict on 50 ms hops, so
 * block size never changes when that happens (checked separately).
 */
static run_result
run_signal(dsd_analog_rx_core* core, signal_src* src, int64_t total, int block, int expect_tenths) {
    run_result r = {-1, -1, -1, 0, 0};
    float buf[4096];
    assert(block > 0 && block <= (int)(sizeof(buf) / sizeof(buf[0])));
    int prev_locked = 0;
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
        if (locked && o.tenths == expect_tenths && r.first_lock < 0) {
            r.first_lock = n + m;
        }
        if (locked && o.tenths != expect_tenths && r.first_wrong < 0) {
            r.first_wrong = n + m;
        }
        if (!locked && r.first_lock >= 0 && r.first_unlocked < 0) {
            r.first_unlocked = n + m;
        }
        prev_locked = locked;
        r.final_state = o.state;
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

/* Lock time of one tone, measured from its onset, with the onset landing anywhere inside a
   hop. Returns -1 if it never locked; asserts it never locked anything else. */
static double
lock_time_ms(int fs, double hz, double snr_db, uint64_t seed, int onset_offset_ms) {
    dsd_analog_rx_core_init(&g_core);
    signal_src src;
    signal_init(&src, fs, seed, hz, snr_db);
    src.tone_on = ms_to_samples(fs, 300.0 + onset_offset_ms);
    const int block = fs / 1000 > 0 ? fs / 1000 : 1;
    const int64_t total = src.tone_on + ms_to_samples(fs, LOCK_BOUND_MS + 150.0);
    const run_result r = run_signal(&g_core, &src, total, block, (int)lround(hz * 10.0));
    if (r.first_wrong >= 0) {
        DSD_FPRINTF(stderr, "wrong lock: fs=%d hz=%.1f snr=%.0f seed=%llu\n", fs, hz, snr_db, (unsigned long long)seed);
    }
    assert(r.first_wrong < 0);
    if (r.first_lock < 0) {
        return -1.0;
    }
    return samples_to_ms(fs, r.first_lock - src.tone_on);
}

static int
compare_doubles(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

/* p50/p95/worst of @p count timings, for the PR evidence. Sorts in place. */
static void
report_timings(const char* what, double* times, int count) {
    qsort(times, (size_t)count, sizeof(times[0]), compare_doubles);
    printf("%s: p50 %.0f ms, p95 %.0f ms, worst %.0f ms (%d cases)\n", what, times[count / 2],
           times[(count * 95) / 100], times[count - 1], count);
    (void)fflush(stdout);
}

/* All 50 tones, every rate, at +10 and 0 dB in-band tone-to-noise: the right value within
   400 ms of sample time. Prints the p95 and worst lock times for the PR evidence. */
static void
test_every_tone_locks_within_bound(void) {
    static const double snrs[] = {10.0, 0.0};
    static double times[DSD_CTCSS_TONE_COUNT * RATE_COUNT];
    for (int si = 0; si < 2; si++) {
        int count = 0;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
                const double hz = (double)dsd_ctcss_tone_tenths(k) / 10.0;
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
        report_timings(what, times, count);
    }
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

/* 150.0 Hz (1.4 Hz from 151.4) and 68.2 Hz (between 67.0 and 69.3) are not supported
   tones: seconds of either, clean or at +10 dB, never lock anything. */
static void
test_unsupported_frequencies_never_lock(void) {
    static const double freqs[] = {150.0, 68.2};
    static const double snrs[] = {60.0, 10.0};
    for (int f = 0; f < 2; f++) {
        for (int s = 0; s < 2; s++) {
            for (int ri = 0; ri < RATE_COUNT; ri++) {
                dsd_analog_rx_core_init(&g_core);
                signal_src src;
                signal_init(&src, k_rates[ri], 777ULL + (uint64_t)(f * 10 + s), freqs[f], snrs[s]);
                src.tone_on = 0;
                const run_result r = run_signal(&g_core, &src, ms_to_samples(k_rates[ri], 3000.0), k_rates[ri] / 50, 0);
                assert(r.locks == 0);
                /* And they are positively rejected, not left pending. */
                assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
            }
        }
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

    dsd_analog_rx_core_init(&g_core);
    signal_init(&src, 48000, 2718282ULL, 100.0, 0.0);
    src.noise_sigma = 0.0;
    src.voice_gain = 1.0;
    src.voice_filtered = 1;
    synth_speech_init(&src.speech, 48000, 3ULL, 0.05);
    synth_voice_hpf_init(&src.voice_hpf, 48000);
    r = run_signal(&g_core, &src, ms_to_samples(48000, 60000.0), 960, 0);
    assert(r.locks == 0);

    dsd_analog_rx_core_init(&g_core);
    signal_init(&src, 48000, 1618033ULL, 100.0, 0.0);
    src.noise_sigma = 0.05;
    r = run_signal(&g_core, &src, ms_to_samples(48000, 60000.0), 960, 0);
    assert(r.locks == 0);
    /* Noise is positively "no tone", not "still deciding". */
    assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
}

/* A tone under a transmitter's voice (speech through a 300 Hz high-pass, 10 dB above the
   tone): it still locks on the right value, and the voice never reads as another tone. */
static void
test_tone_under_voice_locks(void) {
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k += 7) {
        const double hz = (double)dsd_ctcss_tone_tenths(k) / 10.0;
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
        const run_result r = run_signal(&g_core, &src, ms_to_samples(48000, 3000.0), 48, (int)lround(hz * 10.0));
        assert(r.first_wrong < 0);
        assert(r.first_lock >= 0 && samples_to_ms(48000, r.first_lock - src.tone_on) <= 1500.0);
        /* Once locked, the voice does not knock it out. */
        assert(r.final_state == DSD_ANALOG_TONE_STATE_LOCKED);
    }
}

/* The tone stops while the carrier (noise) carries on: the lock is gone within 350 ms. */
static void
test_tone_loss_within_bound(void) {
    double times[RATE_COUNT * DSD_CTCSS_TONE_COUNT];
    int count = 0;
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        for (int k = 3; k < DSD_CTCSS_TONE_COUNT; k += 11) {
            const int fs = k_rates[ri];
            const double hz = (double)dsd_ctcss_tone_tenths(k) / 10.0;
            dsd_analog_rx_core_init(&g_core);
            signal_src src;
            signal_init(&src, fs, 90210ULL + (uint64_t)(k * 31 + ri), hz, 10.0);
            src.tone_on = 0;
            src.tone_off = ms_to_samples(fs, 1000.0 + (double)((k * 11) % 50));
            const run_result r =
                run_signal(&g_core, &src, src.tone_off + ms_to_samples(fs, 800.0), fs / 1000, (int)lround(hz * 10.0));
            assert(r.first_lock >= 0 && r.first_lock < src.tone_off);
            assert(r.first_unlocked > src.tone_off);
            const double loss_ms = samples_to_ms(fs, r.first_unlocked - src.tone_off);
            if (loss_ms > (double)LOSS_BOUND_MS) {
                DSD_FPRINTF(stderr, "slow loss: fs=%d hz=%.1f -> %.0f ms\n", fs, hz, loss_ms);
            }
            assert(loss_ms <= (double)LOSS_BOUND_MS);
            /* Carrier still up, no tone: "none", positively. */
            assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
            times[count++] = loss_ms;
        }
    }
    report_timings("CTCSS loss after the tone stops", times, count);
}

/* A reverse burst -- the transmitter flipping its tone's phase before it unkeys -- ends the
   lock within 150 ms, without waiting for the tone to stop. */
static void
test_reverse_burst_drops_fast(void) {
    double times[RATE_COUNT * DSD_CTCSS_TONE_COUNT];
    int count = 0;
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        for (int k = 5; k < DSD_CTCSS_TONE_COUNT; k += 9) {
            const int fs = k_rates[ri];
            const double hz = (double)dsd_ctcss_tone_tenths(k) / 10.0;
            dsd_analog_rx_core_init(&g_core);
            signal_src src;
            signal_init(&src, fs, 8675309ULL + (uint64_t)(k * 13 + ri), hz, 20.0);
            src.tone_on = 0;
            src.flip_at = ms_to_samples(fs, 1000.0 + (double)((k * 17) % 50));
            const run_result r =
                run_signal(&g_core, &src, src.flip_at + ms_to_samples(fs, 180.0), fs / 1000, (int)lround(hz * 10.0));
            assert(r.first_lock >= 0 && r.first_lock < src.flip_at);
            assert(r.first_unlocked > src.flip_at);
            const double loss_ms = samples_to_ms(fs, r.first_unlocked - src.flip_at);
            if (loss_ms > (double)BURST_LOSS_BOUND_MS) {
                DSD_FPRINTF(stderr, "slow burst: fs=%d hz=%.1f -> %.0f ms\n", fs, hz, loss_ms);
            }
            assert(loss_ms <= (double)BURST_LOSS_BOUND_MS);
            times[count++] = loss_ms;
        }
    }
    report_timings("CTCSS loss on a reverse burst", times, count);
}

/* Every metric is a ratio, so the RTL live scale (about 1/pi), the unscaled replay and
   int16-scale PCM all lock the same tone on the same hop. */
static void
test_scale_invariance(void) {
    static const double scales[] = {1.0, 1.0 / M_PI, 32768.0};
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k += 5) {
        const double hz = (double)dsd_ctcss_tone_tenths(k) / 10.0;
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

/* Below 2.4 kHz there is no sub-audible band to keep: detection says so and stays out. */
static void
test_unusable_rate(void) {
    float block[100];
    DSD_MEMSET(block, 0, sizeof(block));
    block[0] = 1.0f;
    dsd_analog_rx_core_init(&g_core);
    assert(dsd_analog_rx_core_process(&g_core, block, 100, 2000, 1) == 0);
    const observation o = observe(&g_core);
    assert(o.state == DSD_ANALOG_TONE_STATE_INACTIVE);
    /* A usable rate afterwards redesigns and runs. */
    assert(dsd_analog_rx_core_process(&g_core, block, 100, 8000, 1) == 1);
    assert(g_core.fe.active == 1 && g_core.fe.decim == 3);
}

int
main(void) {
    test_unusable_rate();
    test_carrier_hangover();
    test_block_size_does_not_move_the_verdict();
    test_scale_invariance();
    test_adjacent_low_tones_are_distinguished();
    test_unsupported_frequencies_never_lock();
    test_tone_loss_within_bound();
    test_reverse_burst_drops_fast();
    test_tone_under_voice_locks();
    test_every_tone_locks_within_bound();
    test_speech_and_noise_never_lock();
    return 0;
}
