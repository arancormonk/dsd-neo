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
    int64_t tone_on;  /**< first sample carrying the tone */
    int64_t tone_off; /**< first sample without it (INT64_MAX = never) */
    int64_t flip_at;  /**< reverse burst: the tone's phase flips by pi here (INT64_MAX = never) */
    int64_t move_at;  /**< the tone moves to move_hz here, phase-continuous (INT64_MAX = never) */
    double move_hz;
    synth_dcs dcs; /**< DCS signalling instead of a tone, when dcs_on */
    int dcs_on;
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
    if (n == src->move_at) {
        src->tone.hz = src->move_hz;
    }
    if (src->dcs_on) {
        v += synth_dcs_next(&src->dcs);
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
    src->move_at = INT64_MAX;
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
    int final_state;
} run_result;

/*
 * Feed @p total samples in blocks of @p block, observing after each. Small blocks give the
 * lock time to within one block; the core itself only changes its verdict on 50 ms hops, so
 * block size never changes when that happens (checked separately).
 */
static run_result
run_signal(dsd_analog_rx_core* core, signal_src* src, int64_t total, int block, int expect_tenths) {
    run_result r = {-1, -1, -1, -1, -1, 0, 0};
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
        if (o.state == DSD_ANALOG_TONE_STATE_NONE && r.first_none < 0) {
            r.first_none = n + m;
        }
        if (o.state == DSD_ANALOG_TONE_STATE_ACQUIRING && r.first_none < 0) {
            r.last_acquiring = n + m;
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

/* Lock time of a tone at @p hz that should read as the table tone @p expect_tenths, measured
   from its onset, with the onset landing anywhere inside a hop and the run lasting
   @p watch_ms past it. Returns -1 if it never locked; asserts it never locked anything else. */
static double
lock_time_as_ms(int fs, double hz, int expect_tenths, double snr_db, uint64_t seed, int onset_offset_ms,
                double watch_ms) {
    dsd_analog_rx_core_init(&g_core);
    signal_src src;
    signal_init(&src, fs, seed, hz, snr_db);
    src.tone_on = ms_to_samples(fs, 300.0 + onset_offset_ms);
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
 * pinned seeds, not the long-run rate: over 10,000 seeded starts at 0 dB, 1% of exact tones and
 * 3% of tones 0.2 Hz off take longer than 400 ms (docs/testing.md). Prints p50/p95/worst for the
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

    static double times[DSD_CTCSS_TONE_COUNT * RATE_COUNT];
    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        int count = 0;
        int within = 0;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
                const int tenths = dsd_ctcss_tone_tenths(k);
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
 * Off-table tones lock nothing over these 3 s runs, down to 0 dB in-band. 150.0 Hz sits 1.4 Hz
 * from 151.4; 68.2, 161.0 and 166.7 Hz sit 1.1-1.2 Hz from the table tone on either side, where
 * a 0 dB estimate strays past the 0.8 Hz snap gate on several percent of hops but only rarely
 * past the 0.5 Hz one a lock needs -- rarely, not never: over two hours of a 0 dB carrier,
 * 68.2 Hz reads as a neighbour for about 200 ms some ten times an hour (docs/testing.md), and
 * from 3 dB up it did not happen at all. 100.85 and 100.9 Hz sit just outside the snap gate of
 * 100.0, where only a clean signal's estimate is steady enough to tell.
 */
static void
test_unsupported_frequencies_never_lock(void) {
    static const double freqs[] = {150.0, 68.2, 161.0, 166.7};
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
 * generator's code), for 3 s at 8 kHz, never locks and is positively "no tone". The words
 * that come nearest (the most rho at a snapped table tone) do the same at every rate, clean
 * and at +10 dB, in both polarities.
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
            if (r.locks != 0) {
                DSD_FPRINTF(stderr, "DCS lock: word 0x%06X\n", (unsigned int)word);
            }
            assert(r.locks == 0 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
        }
    }
    static const uint32_t nearest[] = {0x5D5530U, 0x5559E8U};
    static const double snrs[] = {200.0, 10.0};
    for (int w = 0; w < 2; w++) {
        for (int inverted = 0; inverted < 2; inverted++) {
            const uint32_t word = inverted ? (~nearest[w] & all_ones) : nearest[w];
            for (int ri = 0; ri < RATE_COUNT; ri++) {
                for (int s = 0; s < 2; s++) {
                    const run_result r = run_dcs(k_rates[ri], word, snrs[s], 23ULL + (uint64_t)(w * 8 + ri * 2 + s));
                    assert(r.locks == 0 && r.final_state == DSD_ANALOG_TONE_STATE_NONE);
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
    double times[DSD_CTCSS_TONE_COUNT];
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
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
    check_lock_contract("CTCSS lock under voice 10 dB above the tone", times, DSD_CTCSS_TONE_COUNT);
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

    static double times[RATE_COUNT * DSD_CTCSS_TONE_COUNT];
    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        int count = 0;
        int within = 0;
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
                const int fs = k_rates[ri];
                const double hz = (double)dsd_ctcss_tone_tenths(k) / 10.0;
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
                /* Carrier still up, no tone: "none", positively. */
                assert(r.final_state == DSD_ANALOG_TONE_STATE_NONE);
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

/* A reverse burst -- the transmitter flipping its tone's phase before it unkeys -- ends the
   lock within 150 ms, without waiting for the tone to stop: every tone at every rate at
   +10 dB in-band. Nearer 0 dB a sub-block is too noisy to serve as the phase reference on
   every hop, and a burst can be caught late or missed, when the carrier drop that follows
   ends the lock instead (docs/testing.md). A caught burst ends the lock as a stop does, so the
   row checks the loss contract too. */
static void
test_reverse_burst_drops_fast(void) {
    static double times[RATE_COUNT * DSD_CTCSS_TONE_COUNT];
    int count = 0;
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
            const int fs = k_rates[ri];
            const double hz = (double)dsd_ctcss_tone_tenths(k) / 10.0;
            dsd_analog_rx_core_init(&g_core);
            signal_src src;
            signal_init(&src, fs, 8675309ULL + (uint64_t)(k * 13 + ri), hz, 10.0);
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
    check_loss_contract("CTCSS loss on a reverse burst", times, count);
}

/* A held tone at 0 dB in-band stays held: one lock per 15 s run and never lost, at every
   rate (over 4,000 s of such holds the lock never dropped; docs/testing.md). */
static void
test_lock_holds_at_0db(void) {
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        const int k = 4 + (ri * 13);
        const double hz = (double)dsd_ctcss_tone_tenths(k) / 10.0;
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
   that has seen no block yet is INACTIVE; one designed for an unusable rate is UNAVAILABLE. */
static void
test_unusable_rate(void) {
    float block[100];
    DSD_MEMSET(block, 0, sizeof(block));
    block[0] = 1.0f;
    dsd_analog_rx_core_init(&g_core);
    assert(observe(&g_core).state == DSD_ANALOG_TONE_STATE_INACTIVE);
    assert(dsd_analog_rx_core_process(&g_core, block, 100, 2000, 1) == 0);
    const observation o = observe(&g_core);
    assert(o.state == DSD_ANALOG_TONE_STATE_UNAVAILABLE && o.carrier == 0);
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
    assert(observe(&g_core).state == DSD_ANALOG_TONE_STATE_UNAVAILABLE);
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
        dsd_analog_ctcss_ops.process(&det, silence, silence, sub_len, 0);
    }
    const dsd_analog_ctcss_hop* hop = dsd_analog_ctcss_last_hop(&det);
    assert(hop->evaluated == 1);
    assert(fabs(hop->rho) < 1e-12);
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
    test_lock_follows_a_tone_off_the_table();
    test_dcs_never_locks();
    test_no_tone_verdict_then_late_tone();
    test_tone_loss_within_bound();
    test_reverse_burst_drops_fast();
    test_tone_under_voice_locks();
    test_lock_holds_at_0db();
    test_every_tone_locks_within_bound();
    test_off_nominal_tones_lock();
    test_speech_and_noise_never_lock();
    return 0;
}
