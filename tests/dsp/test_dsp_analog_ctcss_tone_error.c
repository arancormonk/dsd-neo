// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * CTCSS transmitter tone error (issue #643), through the pure analog receive core in sample time.
 *
 * A transmitter's encoder is specified to 0.5 % of its tone, so a radio 0.4 % high on 150.0 Hz sends 150.6 Hz -- the
 * reporter's radios did -- which the detector's on-value rules (estimates within 0.5 Hz) never confirmed. These tests
 * pin what the detector now does with such a tone, and that it does it only where the on-value rules find nothing:
 * every signal on which they act reads as it did before (the review cases below are the ones where an earlier design
 * did not). Every signal comes from a seeded generator, so a failure reproduces exactly.
 */

#include <assert.h>
#include <dsd-neo/core/analog_tone.h>
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
#include "analog_tone_policy.h"
#include "analog_tone_synth.h"

static const int k_rates[] = {8000, 44100, 48000, 78125};
#define RATE_COUNT ((int)(sizeof(k_rates) / sizeof(k_rates[0])))

enum {
    HOP_MS = DSD_ANALOG_CTCSS_SUBBLOCK_MS,
    PARTS_MAX = 4,
    CHANGES_MAX = 32,
};

static dsd_analog_rx_core g_core;

/* One sinusoid of a test signal: from on_ms, at hz (then hz2 from move_ms, phase-continuously), amplitude amp (then
   amp2 from step_ms, ramped over ramp_ms), with a phase offset of excursion_rad between excursion_from_ms and
   excursion_to_ms. */
typedef struct {
    double hz;
    double amp;
    double phase;
    double on_ms;
    double hz2;
    double move_ms;
    double amp2;
    double step_ms;
    double ramp_ms;
    double excursion_rad;
    double excursion_from_ms;
    double excursion_to_ms;
    /** Instead of amp2: the amplitude steps to sched_amp[i] at sched_ms[i] (ascending), sched of them. */
    int sched;
    double sched_ms[4];
    double sched_amp[4];
} part;

typedef struct {
    double fs;
    part parts[PARTS_MAX];
    int count;
    double noise_sigma;
    int f2_noise; /**< 1 = the noise is differentiated: an FM discriminator's f^2 shape */
    double prev_noise;
    synth_rng rng;
    double acc[PARTS_MAX];
} multi_src;

static void
multi_init(multi_src* src, double fs, uint64_t seed) {
    DSD_MEMSET(src, 0, sizeof(*src));
    src->fs = fs;
    synth_rng_seed(&src->rng, seed);
}

static part*
multi_add(multi_src* src, double hz, double amp) {
    assert(src->count < PARTS_MAX);
    part* p = &src->parts[src->count++];
    DSD_MEMSET(p, 0, sizeof(*p));
    p->hz = hz;
    p->hz2 = hz;
    p->amp = amp;
    p->amp2 = amp;
    p->move_ms = 1e12;
    p->step_ms = 1e12;
    p->excursion_from_ms = 1e12;
    p->excursion_to_ms = 1e12;
    return p;
}

static float
multi_next(multi_src* src, int64_t n) {
    const double t_ms = 1000.0 * (double)n / src->fs;
    double v = 0.0;
    for (int i = 0; i < src->count; i++) {
        const part* p = &src->parts[i];
        const double hz = t_ms >= p->move_ms ? p->hz2 : p->hz;
        src->acc[i] += 2.0 * M_PI * hz / src->fs;
        if (t_ms < p->on_ms) {
            continue;
        }
        double amp = p->amp;
        if (t_ms >= p->step_ms) {
            const double k = p->ramp_ms > 0.0 ? fmin(1.0, (t_ms - p->step_ms) / p->ramp_ms) : 1.0;
            amp = p->amp + ((p->amp2 - p->amp) * k);
        }
        for (int j = 0; j < p->sched; j++) {
            if (t_ms >= p->sched_ms[j]) {
                amp = p->sched_amp[j];
            }
        }
        const double ex = (t_ms >= p->excursion_from_ms && t_ms < p->excursion_to_ms) ? p->excursion_rad : 0.0;
        v += amp * cos(src->acc[i] + p->phase + ex);
    }
    if (src->noise_sigma > 0.0) {
        const double w = src->noise_sigma * synth_gauss(&src->rng);
        if (src->f2_noise) {
            /* A first difference has gain 2 sin(pi f / fs), f^2 in power at sub-audible frequencies. */
            v += w - src->prev_noise;
            src->prev_noise = w;
        } else {
            v += w;
        }
    }
    return (float)v;
}

/* What a run published, change by change: the time each published label took over. */
typedef struct {
    int count;
    double at_ms[CHANGES_MAX];
    int tenths[CHANGES_MAX];    /**< the locked tone, 0 = none locked */
    int off_value[CHANGES_MAX]; /**< dsd_analog_rx_publication::ctcss_off_value then */
    int state[CHANGES_MAX];
    int final_tenths;
    int final_state;
    double first_none_ms; /**< first block reading NONE, -1 = never */
    double first_lock_ms; /**< first block with anything locked, -1 = never */
    int first_lock_tenths;
    int locks;              /**< transitions into LOCKED */
    int off_value_mismatch; /**< 1 once an off-value flag appeared without a CTCSS lock */
} trace;

/* Run @p src for @p ms through the core in 20 ms blocks, the carrier open throughout. */
static trace
run_trace(multi_src* src, double ms) {
    trace t;
    DSD_MEMSET(&t, 0, sizeof(t));
    t.first_none_ms = -1.0;
    t.first_lock_ms = -1.0;
    int prev_tenths = -1;
    int prev_off = -1;
    int prev_locked = 0;
    const int block = (int)src->fs / 50;
    float* buf = (float*)malloc(sizeof(float) * (size_t)block);
    assert(buf != NULL);
    const int64_t total = (int64_t)llround(src->fs * ms / 1000.0);
    for (int64_t n = 0; n < total; n += block) {
        for (int i = 0; i < block; i++) {
            buf[i] = multi_next(src, n + i);
        }
        assert(dsd_analog_rx_core_process(&g_core, buf, block, (int)src->fs, 1) == 1);
        dsd_analog_rx_publication pub;
        dsd_analog_rx_core_publish(&g_core, &pub);
        const double at_ms = 1000.0 * (double)(n + block) / src->fs;
        const int locked = pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED;
        const int tenths = locked && pub.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS ? pub.ctcss_tenths_hz : 0;
        if (pub.ctcss_off_value && !tenths) {
            t.off_value_mismatch = 1;
        }
        if (locked && !prev_locked) {
            t.locks++;
        }
        if (locked && t.first_lock_ms < 0.0) {
            t.first_lock_ms = at_ms;
            t.first_lock_tenths = tenths;
        }
        if (pub.tone_state == DSD_ANALOG_TONE_STATE_NONE && t.first_none_ms < 0.0) {
            t.first_none_ms = at_ms;
        }
        if ((tenths != prev_tenths || pub.ctcss_off_value != prev_off) && t.count < CHANGES_MAX) {
            t.at_ms[t.count] = at_ms;
            t.tenths[t.count] = tenths;
            t.off_value[t.count] = pub.ctcss_off_value;
            t.state[t.count] = pub.tone_state;
            t.count++;
        }
        prev_tenths = tenths;
        prev_off = pub.ctcss_off_value;
        prev_locked = locked;
        t.final_tenths = tenths;
        t.final_state = pub.tone_state;
    }
    free(buf);
    return t;
}

/* Whether @p t ever published @p tenths. */
static int
trace_named(const trace* t, int tenths) {
    for (int i = 0; i < t->count; i++) {
        if (t->tenths[i] == tenths) {
            return 1;
        }
    }
    return 0;
}

/* When @p t first published @p tenths, -1 = never. */
static double
trace_first(const trace* t, int tenths) {
    for (int i = 0; i < t->count; i++) {
        if (t->tenths[i] == tenths) {
            return t->at_ms[i];
        }
    }
    return -1.0;
}

static void
trace_print(const char* what, int fs, const trace* t) {
    DSD_FPRINTF(stderr, "%s fs=%d:", what, fs);
    for (int i = 0; i < t->count; i++) {
        DSD_FPRINTF(stderr, " %.0fms:%d%s", t->at_ms[i], t->tenths[i], t->off_value[i] ? "*" : "");
    }
    DSD_FPRINTF(stderr, "\n");
}

static int
compare_doubles(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

/* The off-value lock contract (<dsd-neo/dsp/analog_rx.h>): every lock within the ceiling, 95 % within the p95. */
static void
check_off_value_lock_contract(const char* what, double* times, int count) {
    assert(count > 0);
    qsort(times, (size_t)count, sizeof(times[0]), compare_doubles);
    const double p50 = times[count / 2];
    const double p95 = times[(95 * (count - 1)) / 100];
    const double worst = times[count - 1];
    printf("%s: p50 %.0f ms, p95 %.0f ms, worst %.0f ms (%d cases; off-value p95 <= %d ms, each <= %d ms)\n", what, p50,
           p95, worst, count, DSD_ANALOG_CTCSS_OFF_VALUE_LOCK_P95_MS, DSD_ANALOG_CTCSS_OFF_VALUE_LOCK_CEILING_MS);
    (void)fflush(stdout);
    assert(p95 <= (double)DSD_ANALOG_CTCSS_OFF_VALUE_LOCK_P95_MS);
    assert(worst <= (double)DSD_ANALOG_CTCSS_OFF_VALUE_LOCK_CEILING_MS);
}

/* A tone at @p hz, amplitude 0.1, with white noise giving @p snr_db of in-band tone-to-noise (snr_db > 100: none). */
static void
tone_in_noise(multi_src* src, int fs, uint64_t seed, double hz, double snr_db) {
    multi_init(src, fs, seed);
    part* p = multi_add(src, hz, 0.1);
    p->phase = synth_range(&src->rng, 0.0, 2.0 * M_PI);
    src->noise_sigma = snr_db > 100.0 ? 0.0 : synth_inband_noise_sigma(0.1, snr_db, fs);
}

/*
 * The issue: a radio 0.4 % high on 150.0 Hz sends 150.6 Hz, which now reads as 150.0 Hz -- off its value, so flagged
 * -- at +20 and +30 dB in-band and clean, at every rate: from the carrier's start before any "none", within the
 * off-value contract, held for 3 s with one lock and never 151.4. After 300 ms of noise it locks within the off-value
 * ceiling of the onset (a "none" may come first: the no-tone verdict still lands 500 ms into the carrier). The mirror,
 * 150.8 Hz, reads 151.4. At +10 dB 150.6 Hz, which the estimate is not precise enough to name there, never reads
 * 151.4.
 */
static void
test_150_6_reads_150_0(void) {
    static const double snrs[] = {1000.0, 30.0, 20.0};
    static double times[RATE_COUNT * 3 * 6];
    int count = 0;
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        for (int si = 0; si < 3; si++) {
            for (int seed = 0; seed < 6; seed++) {
                multi_src src;
                tone_in_noise(&src, fs, 6431500ULL + (uint64_t)(ri * 100 + si * 10 + seed), 150.6, snrs[si]);
                dsd_analog_rx_core_init(&g_core);
                const trace t = run_trace(&src, 3000.0);
                assert(t.count > 0 && t.count <= CHANGES_MAX);
                if (t.first_lock_tenths != 1500 || t.locks != 1 || trace_named(&t, 1514) || t.final_tenths != 1500
                    || (t.first_none_ms >= 0.0 && t.first_none_ms < t.first_lock_ms) || !t.off_value[t.count - 1]) {
                    trace_print("150.6 Hz", fs, &t);
                }
                assert(t.first_lock_tenths == 1500 && t.locks == 1 && t.final_tenths == 1500);
                assert(!trace_named(&t, 1514));
                assert(t.first_none_ms < 0.0 || t.first_none_ms > t.first_lock_ms);
                assert(t.off_value[t.count - 1] == 1 && !t.off_value_mismatch);
                times[count++] = t.first_lock_ms;
            }
        }
    }
    check_off_value_lock_contract("CTCSS 150.6 Hz read as 150.0 Hz from the carrier's start, +20 dB and up", times,
                                  count);
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        for (int si = 1; si < 3; si++) {
            /* The onset after 300 ms of noise. */
            multi_src src;
            tone_in_noise(&src, fs, 6431600ULL + (uint64_t)(ri * 10 + si), 150.6, snrs[si]);
            src.parts[0].on_ms = 300.0;
            dsd_analog_rx_core_init(&g_core);
            const trace t = run_trace(&src, 3000.0);
            const double lock_ms = trace_first(&t, 1500);
            if (!(lock_ms >= 0.0 && lock_ms - 300.0 <= (double)DSD_ANALOG_CTCSS_OFF_VALUE_LOCK_CEILING_MS)
                || trace_named(&t, 1514)) {
                trace_print("150.6 Hz onset", fs, &t);
            }
            assert(lock_ms >= 0.0 && lock_ms - 300.0 <= (double)DSD_ANALOG_CTCSS_OFF_VALUE_LOCK_CEILING_MS);
            assert(!trace_named(&t, 1514) && t.final_tenths == 1500);
            /* The mirror: 150.8 Hz is 151.4 Hz 0.6 Hz low. */
            tone_in_noise(&src, fs, 6431700ULL + (uint64_t)(ri * 10 + si), 150.8, snrs[si]);
            dsd_analog_rx_core_init(&g_core);
            const trace m = run_trace(&src, 3000.0);
            if (m.first_lock_tenths != 1514 || trace_named(&m, 1500)) {
                trace_print("150.8 Hz", fs, &m);
            }
            assert(m.first_lock_tenths == 1514 && !trace_named(&m, 1500) && m.final_tenths == 1514);
        }
        /* +10 dB: never the other tone. */
        multi_src src;
        tone_in_noise(&src, fs, 6431800ULL + (uint64_t)ri, 150.6, 10.0);
        dsd_analog_rx_core_init(&g_core);
        const trace t = run_trace(&src, 3000.0);
        assert(!trace_named(&t, 1514));
    }
}

/*
 * The off-value contract row: every table tone from 125 Hz up (where 0.4 % is more than 0.5 Hz), set 0.4 % off its
 * value either way, and 150.0 and 151.4 Hz set 0.6 Hz off either way, at +20 dB in-band at every rate, lock their own
 * tone within the off-value contract and never another.
 */
static void
test_off_value_tones_lock(void) {
    static double times[DSD_CTCSS_TONE_COUNT * 2 * RATE_COUNT];
    int count = 0;
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        const int tenths = dsd_ctcss_tone_tenths(k);
        const double hz = (double)tenths / 10.0;
        const int pair = tenths == 1500 || tenths == 1514;
        if (hz < 125.0 && !pair) {
            continue;
        }
        const double off = pair ? 0.6 : 0.004 * hz;
        for (int side = 0; side < 2; side++) {
            for (int ri = 0; ri < RATE_COUNT; ri++) {
                const int fs = k_rates[ri];
                multi_src src;
                tone_in_noise(&src, fs, 6432000ULL + (uint64_t)(k * 31 + side * 7 + ri), hz + (side ? off : -off),
                              20.0);
                dsd_analog_rx_core_init(&g_core);
                const trace t = run_trace(&src, 1500.0);
                int wrong = 0;
                for (int i = 0; i < t.count; i++) {
                    wrong |= t.tenths[i] != 0 && t.tenths[i] != tenths;
                }
                if (t.first_lock_tenths != tenths || wrong) {
                    trace_print("off-value tone", fs, &t);
                    DSD_FPRINTF(stderr, "  tone %d set %+.3f Hz\n", tenths, side ? off : -off);
                }
                assert(t.first_lock_tenths == tenths && !wrong);
                times[count++] = t.first_lock_ms;
            }
        }
    }
    check_off_value_lock_contract("CTCSS lock 0.4 % (pair 0.6 Hz) off the table at +20 dB in-band", times, count);
}

/* Where the pair's gates meet: 150.70 Hz, and 150.68 and 150.72 Hz at +60 dB, name neither tone, from the carrier's
   start, at every rate. The floor on the estimate's precision (k_min_est_sd_hz) is what keeps a clean tone 0.02 Hz from
   the meeting point from being named. */
static void
test_pair_midpoint_names_neither(void) {
    static const struct {
        double hz;
        double snr_db;
    } rows[] = {{150.70, 60.0}, {150.70, 30.0}, {150.70, 20.0}, {150.70, 10.0}, {150.68, 60.0}, {150.72, 60.0}};

    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        for (int ri = 0; ri < RATE_COUNT; ri++) {
            const int fs = k_rates[ri];
            multi_src src;
            tone_in_noise(&src, fs, 6433000ULL + (uint64_t)(row * 10 + (size_t)ri), rows[row].hz, rows[row].snr_db);
            dsd_analog_rx_core_init(&g_core);
            const trace t = run_trace(&src, 3000.0);
            if (t.locks != 0) {
                trace_print("pair midpoint", fs, &t);
            }
            assert(t.locks == 0 && t.final_state == DSD_ANALOG_TONE_STATE_NONE);
        }
    }
}

/*
 * The estimate's precision (dsd_analog_ctcss_hop::est_var_hz2) is what widens the gate, so it must not claim more
 * than the estimate delivers: over tones from 100 to 254.1 Hz, on and off their value, at 0 to 60 dB in-band, in white
 * and in f^2-shaped noise, through the real front end at 48 kHz, an estimate further than 3 standard deviations from
 * the true frequency comes on at most 0.5 % of the windows that carry the tone throughout, for every window span the
 * detector uses. A window that does not -- the tone starting inside it -- is never one the gate widens on.
 */
static void
test_estimate_precision_is_calibrated(void) {
    static const double tones[] = {100.0, 123.0, 150.6, 167.9, 192.8, 210.7, 241.8, 254.1};
    static const double offsets[] = {0.0, 0.3, -0.3, 0.6, -0.6};
    static const double snrs[] = {0.0, 10.0, 20.0, 30.0, 60.0};
    static const int spans[] = {5, 8, 9, 10, 11, 12};

    enum { SPANS = (int)(sizeof(spans) / sizeof(spans[0])) };

    const int fs = 48000;
    for (int shape = 0; shape < 2; shape++) {
        int windows[SPANS] = {0};
        int outside[SPANS] = {0};
        int onset_stationary = 0;
        for (size_t ti = 0; ti < sizeof(tones) / sizeof(tones[0]); ti++) {
            for (size_t oi = 0; oi < sizeof(offsets) / sizeof(offsets[0]); oi++) {
                for (size_t si = 0; si < sizeof(snrs) / sizeof(snrs[0]); si++) {
                    const double hz = tones[ti] + offsets[oi];
                    multi_src src;
                    const uint64_t seed = 6434000ULL + ((uint64_t)shape * 1000ULL) + ((uint64_t)ti * 100ULL)
                                          + ((uint64_t)oi * 10ULL) + (uint64_t)si;
                    tone_in_noise(&src, fs, seed, hz, snrs[si]);
                    src.f2_noise = shape;
                    if (shape) {
                        /* The same in-band noise power once differentiated: 2 sin(pi f / fs) is about 2 pi f / fs. */
                        src.noise_sigma /= (2.0 * M_PI * 150.0 / (double)fs);
                    }
                    src.parts[0].on_ms = 300.0;
                    dsd_analog_rx_core_init(&g_core);
                    const int bin =
                        dsd_analog_ctcss_snap_index(tones[ti] >= 150.0 && tones[ti] < 151.0 ? 150.0 : tones[ti]);
                    assert(bin >= 0);
                    const int sub_block = fs * HOP_MS / 1000;
                    float buf[2400];
                    assert(sub_block <= (int)(sizeof(buf) / sizeof(buf[0])));
                    int64_t n = 0;
                    for (int hop = 0; hop < 40; hop++) {
                        for (int i = 0; i < sub_block; i++) {
                            buf[i] = multi_next(&src, n + i);
                        }
                        n += sub_block;
                        assert(dsd_analog_rx_core_process(&g_core, buf, sub_block, fs, 1) == 1);
                        const double t_ms = 1000.0 * (double)n / (double)fs;
                        for (int k = 0; k < SPANS; k++) {
                            dsd_analog_ctcss_hop h;
                            dsd_analog_ctcss_measure_span(&g_core.ctcss, spans[k], bin, &h);
                            if (!h.evaluated) {
                                continue;
                            }
                            /* The front end's delay is about 30 ms: a window that reaches back past the onset is one
                               that straddles it. */
                            const double window_ms = (double)(spans[k] * HOP_MS);
                            if (t_ms - window_ms < 300.0 + 40.0) {
                                if (t_ms - window_ms < 300.0 - 40.0 && t_ms > 300.0 + 80.0 && h.stationary) {
                                    onset_stationary++;
                                }
                                continue;
                            }
                            if (!h.stationary || h.rho < 0.25) {
                                continue;
                            }
                            windows[k]++;
                            const double z = fabs(h.est_hz - hz) / sqrt(h.est_var_hz2);
                            outside[k] += z > 3.0 ? 1 : 0;
                        }
                    }
                }
            }
        }
        for (int k = 0; k < SPANS; k++) {
            printf("CTCSS estimate precision, %s noise, %d ms windows: %d of %d beyond 3 sigma (%.2f%%)\n",
                   shape ? "f^2" : "white", spans[k] * HOP_MS, outside[k], windows[k],
                   windows[k] ? 100.0 * outside[k] / windows[k] : 0.0);
            assert(windows[k] > 0);
            assert(200 * outside[k] <= windows[k]);
        }
        printf("CTCSS onset windows read as carrying the tone throughout, %s noise: %d\n", shape ? "f^2" : "white",
               onset_stationary);
        (void)fflush(stdout);
    }
    /* A window with nothing in it has nothing to fit: a defined, wide variance and no stationarity. The detector is
       fed directly, past the core's carrier test, which digital silence would fail. */
    static dsd_analog_ctcss det;
    dsd_analog_ctcss_ops.configure(&det, 2400.0);
    float zeros[120] = {0};
    for (int i = 0; i < 20; i++) {
        dsd_analog_ctcss_ops.process(&det, zeros, zeros, zeros, 120, 0);
    }
    dsd_analog_ctcss_hop h;
    dsd_analog_ctcss_measure_span(&det, 5, dsd_analog_ctcss_snap_index(150.0), &h);
    /* A range rather than isfinite(), which fast-math builds may fold to true: wide, and short of any overflow. */
    assert(h.evaluated && h.est_var_hz2 >= 1.0 && h.est_var_hz2 <= 1e7 && h.stationary == 0);
}

/* A 100 Hz tone (amplitude 0.10) beside a steady voice-like 254.65 Hz fundamental with a weak 509.3 Hz harmonic:
   zero-phase cosines, as the review that found these cases built them. */
static void
tone_beside_voice(multi_src* src, int fs, double fundamental, double harmonic, double noise_sigma, uint64_t seed) {
    multi_init(src, fs, seed);
    (void)multi_add(src, 100.0, 0.10);
    (void)multi_add(src, 254.65, fundamental);
    (void)multi_add(src, 509.3, harmonic);
    src->noise_sigma = noise_sigma;
}

/*
 * A tone the on-value rules acquire is never pre-empted by one that rests on tone error (review cases): a 100 Hz tone
 * beside a voice holding 254.65 Hz, 0.55 Hz off 254.1, with a weak harmonic. Where the voice leaves the 100 Hz tone
 * enough of the band, 100.0 Hz is confirmed when the on-value rules confirm it, before or instead of 254.1:
 *
 * - fundamental 0.13, 0.145 and 0.16, harmonic 0.052 and 0.030, clean and with noise: 100.0 at 360 / 460 / 460 ms and
 *   never 254.1;
 * - fundamental 0.175 (harmonic 0.030), where the 100 Hz tone qualifies only in the late windows: 100.0 when the
 *   on-value rules confirm it (800 ms at 48 kHz) and kept to the end -- 254.1, which the voice's dominance names
 *   first, is flagged off-value and handed over;
 * - the fundamental dropping from 0.20 to 0.145 (and to 0.13 with harmonic 0.052) at 400 ms, stepped and over 50 ms:
 *   100.0 when the on-value rules confirm it (660-760 ms), kept;
 * - the fundamental switching 0.22 / 0.145 / 0.22 / 0.145 at 300 / 650 / 850 ms at 255.15 Hz: 100.0 at 660 ms, never
 *   254.1 (the late windows' on-value rules run before tone error in either of them);
 * - a -0.12 rad phase excursion of the 0.175 fundamental from 250 to 300 ms (doubled on the harmonic), which made a run
 *   of mixed on-value and tone-error hops: any 254.1 is flagged off-value, and 100.0 ends it when the on-value rules
 *   confirm it.
 *
 * Each "when the on-value rules confirm it" is when the detector without tone error confirms 100.0 Hz on the same
 * signal, measured at each rate; where it never does, the steady voice is a talk-off the off-value flag covers.
 *
 * Every 254.1 published is flagged off-value, so the tone policy never rejects on it before its window ends.
 */
static void
test_on_value_tone_is_never_pre_empted(void) {
    static const double fundamentals[] = {0.13, 0.145, 0.16};
    static const double main_ms[] = {360.0, 460.0, 460.0};
    static const double harmonics[] = {0.052, 0.030};
    static const double noises[] = {0.0, 0.02};
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        for (int f = 0; f < 3; f++) {
            for (int h = 0; h < 2; h++) {
                for (int nz = 0; nz < 2; nz++) {
                    multi_src src;
                    tone_beside_voice(&src, fs, fundamentals[f], harmonics[h], noises[nz], 6435000ULL + (uint64_t)ri);
                    dsd_analog_rx_core_init(&g_core);
                    const trace t = run_trace(&src, 3000.0);
                    const double at = trace_first(&t, 1000);
                    if (!(at >= 0.0 && at <= main_ms[f] + HOP_MS) || trace_named(&t, 2541)) {
                        trace_print("tone beside voice", fs, &t);
                    }
                    assert(at >= 0.0 && at <= main_ms[f] + HOP_MS);
                    assert(!trace_named(&t, 2541) && t.final_tenths == 1000);
                }
            }
        }

        /* by_ms: when the on-value rules alone confirm 100.0 Hz, at 8, 44.1, 48 and 78.125 kHz (-1: they never do,
           which leaves only the off-value flag to check). */
        static const struct {
            double fundamental;
            double fundamental2;
            double step_ms;
            double ramp_ms;
            double harmonic;
            double by_ms[RATE_COUNT];
        } changing[] = {
            {0.175, 0.175, 1e12, 0.0, 0.030, {1100.0, -1.0, 800.0, 1160.0}},
            {0.20, 0.145, 400.0, 0.0, 0.030, {700.0, 720.0, 700.0, 700.0}},
            {0.20, 0.145, 400.0, 50.0, 0.030, {760.0, 760.0, 760.0, 760.0}},
            {0.20, 0.13, 400.0, 0.0, 0.052, {660.0, 660.0, 660.0, 660.0}},
        };

        for (size_t c = 0; c < sizeof(changing) / sizeof(changing[0]); c++) {
            multi_src src;
            tone_beside_voice(&src, fs, changing[c].fundamental, changing[c].harmonic, 0.0, 6435100ULL);
            src.parts[1].amp2 = changing[c].fundamental2;
            src.parts[1].step_ms = changing[c].step_ms;
            src.parts[1].ramp_ms = changing[c].ramp_ms;
            dsd_analog_rx_core_init(&g_core);
            const trace t = run_trace(&src, 3000.0);
            const double at = trace_first(&t, 1000);
            int unflagged = 0;
            for (int i = 0; i < t.count; i++) {
                unflagged |= t.tenths[i] == 2541 && !t.off_value[i];
            }
            const double by_ms = changing[c].by_ms[ri];
            if ((by_ms >= 0.0 && !(at >= 0.0 && at <= by_ms + HOP_MS && t.final_tenths == 1000)) || unflagged) {
                trace_print("changing voice", fs, &t);
            }
            assert(!unflagged);
            if (by_ms >= 0.0) {
                assert(at >= 0.0 && at <= by_ms + HOP_MS && t.final_tenths == 1000);
                /* Once confirmed, 100.0 is kept: never handed back. */
                for (int i = 0; i < t.count; i++) {
                    assert(!(t.at_ms[i] > at && t.tenths[i] != 1000));
                }
            }
        }
        {
            multi_src src;
            tone_beside_voice(&src, fs, 0.22, 0.030, 0.0, 6435200ULL);
            src.parts[1].hz = 255.15;
            src.parts[1].hz2 = 255.15;
            src.parts[2].hz = 510.3;
            src.parts[2].hz2 = 510.3;
            static const double steps_ms[] = {300.0, 650.0, 850.0};
            static const double steps_amp[] = {0.145, 0.22, 0.145};
            src.parts[1].sched = 3;
            for (int j = 0; j < 3; j++) {
                src.parts[1].sched_ms[j] = steps_ms[j];
                src.parts[1].sched_amp[j] = steps_amp[j];
            }
            dsd_analog_rx_core_init(&g_core);
            const trace t = run_trace(&src, 3000.0);
            const double at = trace_first(&t, 1000);
            if (!(at >= 0.0 && at <= 660.0 + HOP_MS) || trace_named(&t, 2541)) {
                trace_print("switching voice", fs, &t);
            }
            assert(at >= 0.0 && at <= 660.0 + HOP_MS && !trace_named(&t, 2541));
        }
        {
            multi_src src;
            tone_beside_voice(&src, fs, 0.175, 0.030, 0.0, 6435300ULL);
            src.parts[1].excursion_rad = -0.12;
            src.parts[1].excursion_from_ms = 250.0;
            src.parts[1].excursion_to_ms = 300.0;
            src.parts[2].excursion_rad = -0.24;
            src.parts[2].excursion_from_ms = 250.0;
            src.parts[2].excursion_to_ms = 300.0;
            dsd_analog_rx_core_init(&g_core);
            const trace t = run_trace(&src, 4000.0);
            int unflagged = 0;
            for (int i = 0; i < t.count; i++) {
                unflagged |= t.tenths[i] == 2541 && !t.off_value[i];
            }
            /* The on-value rules confirm 100.0 Hz at 1100, -, 800 and 600 ms. */
            static const double by_ms[RATE_COUNT] = {1100.0, -1.0, 800.0, 600.0};
            const double at = trace_first(&t, 1000);
            if (unflagged || (by_ms[ri] >= 0.0 && !(at >= 0.0 && at <= by_ms[ri] + HOP_MS && t.final_tenths == 1000))) {
                trace_print("phase excursion", fs, &t);
            }
            assert(!unflagged);
            if (by_ms[ri] >= 0.0) {
                assert(at >= 0.0 && at <= by_ms[ri] + HOP_MS && t.final_tenths == 1000);
            }
        }
    }
}

/*
 * A held tone that moves within its tolerance keeps its lock (review cases): 254.1 Hz moving phase-continuously to
 * 255.2 Hz at +10 and +20 dB, and to 255.1 Hz at +30 dB, under a live carrier -- a second radio keyed inside the
 * hangover -- holds with one lock and no drop, and an allow list of 254.1 Hz stays ALLOWED throughout. The on-value
 * rules dropped every one of them. When the moved tone's lock then rests on tone error alone and a tone the on-value
 * rules confirm joins (100 Hz at 2 s, after 254.1 at amplitude 0.145 with a 0.030 harmonic moved to 255.1 at 1 s),
 * the lock is handed to it as they would hand it (by 2.46 s); with no such tone the moved one is kept. Two on-value
 * rivals of a held tone that alternate (100 and 123 Hz, opposite +/-5 % steps every 50 ms) end it as the on-value
 * rules end it, by 1.5 s, and only they are named after.
 */
static void
test_moved_tone_is_kept_and_yields(void) {
    static const struct {
        double to_hz;
        double snr_db;
    } moves[] = {{255.2, 10.0}, {255.2, 20.0}, {255.1, 30.0}};

    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        for (size_t m = 0; m < sizeof(moves) / sizeof(moves[0]); m++) {
            multi_src src;
            tone_in_noise(&src, fs, 25412551ULL + (uint64_t)(ri * 10 + (int)m), 254.1, moves[m].snr_db);
            src.parts[0].hz2 = moves[m].to_hz;
            src.parts[0].move_ms = 1000.0;
            dsd_analog_rx_core_init(&g_core);
            dsd_analog_tone_policy policy;
            dsd_analog_tone_policy_init(&policy);
            dsd_tone_set set;
            DSD_MEMSET(&set, 0, sizeof(set));
            assert(dsd_tone_set_parse("254.1", &set, NULL, 0) == 0);
            (void)dsd_analog_tone_policy_configure(&policy, DSD_TONE_FILTER_ALLOW, &set);
            const int block = fs / 50;
            float* buf = (float*)malloc(sizeof(float) * (size_t)block);
            assert(buf != NULL);
            int locks = 0;
            int prev = 0;
            int lost_after_allowed = 0;
            int allowed = 0;
            for (int64_t n = 0; n < (int64_t)fs * 4; n += block) {
                for (int i = 0; i < block; i++) {
                    buf[i] = multi_next(&src, n + i);
                }
                assert(dsd_analog_rx_core_process(&g_core, buf, block, fs, 1) == 1);
                dsd_analog_rx_publication pub;
                dsd_analog_rx_core_publish(&g_core, &pub);
                const int locked = pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && pub.ctcss_tenths_hz == 2541;
                locks += locked && !prev;
                prev = locked;
                const int gate = dsd_analog_tone_policy_step(&policy, &pub, (unsigned int)block, fs);
                allowed |= gate == DSD_ANALOG_TONE_GATE_ALLOWED;
                lost_after_allowed |= allowed && gate != DSD_ANALOG_TONE_GATE_ALLOWED;
            }
            free(buf);
            if (locks != 1 || !prev || lost_after_allowed) {
                DSD_FPRINTF(stderr, "moved tone fs=%d -> %.1f at %.0f dB: locks=%d held=%d gate lost=%d\n", fs,
                            moves[m].to_hz, moves[m].snr_db, locks, prev, lost_after_allowed);
            }
            assert(locks == 1 && prev && allowed && !lost_after_allowed);
        }
        for (int with_tone = 0; with_tone < 2; with_tone++) {
            multi_src src;
            multi_init(&src, fs, 6436000ULL);
            part* p = multi_add(&src, 254.1, 0.145);
            p->hz2 = 255.1;
            p->move_ms = 1000.0;
            part* h = multi_add(&src, 508.2, 0.030);
            h->hz2 = 510.2;
            h->move_ms = 1000.0;
            if (with_tone) {
                part* q = multi_add(&src, 100.0, 0.10);
                q->on_ms = 2000.0;
            }
            dsd_analog_rx_core_init(&g_core);
            const trace t = run_trace(&src, 4000.0);
            if (with_tone ? !(trace_first(&t, 1000) >= 0.0 && trace_first(&t, 1000) <= 2460.0 + HOP_MS)
                          : (t.locks != 1 || t.final_tenths != 2541)) {
                trace_print(with_tone ? "moved tone then 100 Hz" : "moved tone alone", fs, &t);
            }
            if (with_tone) {
                assert(trace_first(&t, 1000) >= 0.0 && trace_first(&t, 1000) <= 2460.0 + HOP_MS);
                assert(t.final_tenths == 1000);
            } else {
                assert(t.locks == 1 && t.final_tenths == 2541);
            }
        }
        {
            dsd_analog_rx_core_init(&g_core);
            const int block = fs / 50;
            float* buf = (float*)malloc(sizeof(float) * (size_t)block);
            assert(buf != NULL);
            double dropped = -1.0;
            int others = 0;
            int rivals = 0;
            for (int64_t n = 0; n < (int64_t)fs * 4; n += block) {
                for (int i = 0; i < block; i++) {
                    const double t = (double)(n + i) / (double)fs;
                    double x = 0.10 * cos(2.0 * M_PI * 254.1 * t);
                    if (t >= 1.0) {
                        const int slot_a = (int)floor((t - 1.0) / 0.05) % 2;
                        const int slot_b = (int)floor((t - 1.0 - 0.02) / 0.05) % 2;
                        const double ma = slot_a ? 1.05 : 0.95;
                        const double mb = slot_b ? 0.95 : 1.05;
                        x += (0.14 * ma * cos(2.0 * M_PI * 100.0 * t))
                             + (0.14 * mb * cos((2.0 * M_PI * 123.0 * t) + 1.0));
                    }
                    buf[i] = (float)x;
                }
                assert(dsd_analog_rx_core_process(&g_core, buf, block, fs, 1) == 1);
                dsd_analog_rx_publication pub;
                dsd_analog_rx_core_publish(&g_core, &pub);
                const int tenths = pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED ? pub.ctcss_tenths_hz : 0;
                const double at = 1000.0 * (double)(n + block) / (double)fs;
                if (dropped < 0.0 && at > 1000.0 && tenths != 2541) {
                    dropped = at;
                }
                if (dropped >= 0.0 && tenths != 0) {
                    others |= tenths != 1000 && tenths != 1230;
                    rivals |= 1;
                }
            }
            free(buf);
            if (!(dropped >= 0.0 && dropped <= 1500.0) || others || !rivals) {
                DSD_FPRINTF(stderr, "alternating rivals fs=%d: dropped %.0f ms, others=%d rivals=%d\n", fs, dropped,
                            others, rivals);
            }
            assert(dropped >= 0.0 && dropped <= 1500.0 && !others && rivals);
        }
    }
}

/*
 * A 150.6 Hz tone starting after 300 ms of noise at 0 and +10 dB in-band, where its windows straddle the onset for a
 * while, at every rate and on 25 seeds each: tone error never names 151.4 -- no 151.4 Hz published off-value -- and at
 * +10 dB nothing does. At 0 dB the on-value rules themselves read a tone 0.6 Hz toward the other of the pair as that
 * tone now and then, exactly as they did before tone error was allowed for (the same runs, measured against the
 * detector without it).
 */
static void
test_onset_never_names_the_other_tone(void) {
    static const double snrs[] = {0.0, 10.0};
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        for (int si = 0; si < 2; si++) {
            for (int seed = 0; seed < 25; seed++) {
                multi_src src;
                tone_in_noise(&src, k_rates[ri], 6437000ULL + (uint64_t)(ri * 1000 + si * 100 + seed), 150.6, snrs[si]);
                src.parts[0].on_ms = 300.0;
                dsd_analog_rx_core_init(&g_core);
                const trace t = run_trace(&src, 2000.0);
                int off_value_other = 0;
                for (int i = 0; i < t.count; i++) {
                    off_value_other |= t.tenths[i] == 1514 && t.off_value[i];
                }
                if (off_value_other || (snrs[si] > 5.0 && trace_named(&t, 1514))) {
                    trace_print("150.6 Hz onset", k_rates[ri], &t);
                }
                assert(!off_value_other);
                assert(snrs[si] < 5.0 || !trace_named(&t, 1514));
            }
        }
    }
}

/*
 * A DCS code still being read keeps the policy's extended window (DSD_ANALOG_TONE_WINDOW_DCS_MS) under an off-value
 * CTCSS lock, as it would with nothing locked: the merged publication keeps the DCS detector's candidate there, and
 * only there -- an on-value lock hides it, as before. The candidate is set on the DCS detector directly, as the first
 * half of a code's word would set it.
 */
static void
test_off_value_lock_keeps_the_dcs_candidate(void) {
    static const struct {
        double hz;
        int off_value;
    } k_cases[] = {{150.6, 1}, {150.0, 0}};

    for (size_t k = 0; k < sizeof(k_cases) / sizeof(k_cases[0]); k++) {
        dsd_analog_rx_core_init(&g_core);
        multi_src src;
        tone_in_noise(&src, 48000, 6430000ULL + k, k_cases[k].hz, 200.0);
        const trace t = run_trace(&src, 1000.0);
        assert(t.final_state == DSD_ANALOG_TONE_STATE_LOCKED && t.final_tenths == 1500);
        dsd_analog_rx_publication pub;
        dsd_analog_rx_core_publish(&g_core, &pub);
        assert(pub.ctcss_off_value == k_cases[k].off_value && pub.dcs_candidate == 0);

        g_core.dcs.candidate_age = 0;
        dsd_analog_rx_core_publish(&g_core, &pub);
        assert(pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && pub.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS);
        assert(pub.ctcss_tenths_hz == 1500 && pub.ctcss_off_value == k_cases[k].off_value);
        assert(pub.dcs_candidate == k_cases[k].off_value);

        g_core.dcs.candidate_age = DSD_ANALOG_DCS_SPAN_BITS;
        dsd_analog_rx_core_publish(&g_core, &pub);
        assert(pub.dcs_candidate == 0);
    }
}

/* ---- The long-run sweeps' own receptions ---------------------------------------------------------------------- */

static uint64_t
sweep_mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

typedef struct {
    double hz, amp, amp2, phase, acc;
    int64_t on, step;
} sweep_line;

/*
 * One reception of the long-run sweeps (docs/testing.md "Transmitter tone error"), rebuilt from its seed: a table tone
 * at 0.1 from an onset 300-350 ms into the carrier, at a rate the seed picks; optionally 0.2 Hz off it either way;
 * white noise at @p snr_db in-band (> 100: none); and either speech 10 dB above the tone (@p voice 1, unfiltered) or a
 * voice line near another tone (@p beside: within 0.5 % of it, 0.10-0.25 with a second harmonic, stepping to another
 * level at 0.2-1.2 s). Returns the first lock on the tone after its onset (-1: never) and, in @p final_tenths, what is
 * published as the run ends 2 s after the onset.
 */
static double
sweep_reception(uint64_t seed, double offset_hz, double snr_db, int voice, int beside, int* final_tenths) {
    const uint64_t h = sweep_mix64(seed);
    const int fs = k_rates[h % 4];
    const double onset_ms = 300.0 + (double)((h >> 8) % 50000) / 1000.0;
    const uint64_t th = sweep_mix64(seed ^ 0x7031ULL);
    const int k = (int)(th % DSD_CTCSS_TONE_COUNT);
    const int expect = dsd_ctcss_tone_tenths(k);
    const double hz = ((double)expect / 10.0) + (((th >> 20) & 1U) ? offset_hz : -offset_hz);
    synth_rng rng;
    synth_rng_seed(&rng, seed);
    sweep_line lines[3];
    int count = 0;
    DSD_MEMSET(lines, 0, sizeof(lines));
    lines[0].hz = hz;
    lines[0].amp = lines[0].amp2 = 0.1;
    lines[0].on = (int64_t)llround(fs * onset_ms / 1000.0);
    lines[0].step = INT64_MAX;
    lines[0].phase = synth_range(&rng, 0.0, 2.0 * M_PI);
    count = 1;
    const double sigma = snr_db > 100.0 ? 0.0 : synth_inband_noise_sigma(0.1, snr_db, fs);
    synth_speech speech;
    DSD_MEMSET(&speech, 0, sizeof(speech));
    if (voice) {
        synth_speech_init(&speech, fs, (seed * 7ULL) + 999ULL, 0.76);
    }
    if (beside) {
        int u = (int)(sweep_mix64(seed ^ 0xbe51deULL) % DSD_CTCSS_TONE_COUNT);
        if (dsd_ctcss_tone_tenths(u) == expect) {
            u = (u + 7) % DSD_CTCSS_TONE_COUNT;
        }
        const double uhz = ((double)dsd_ctcss_tone_tenths(u) / 10.0) * (1.0 + synth_range(&rng, -0.005, 0.005));
        sweep_line* l = &lines[count++];
        l->hz = uhz;
        l->amp = synth_range(&rng, 0.10, 0.25);
        l->phase = synth_range(&rng, 0.0, 2.0 * M_PI);
        l->amp2 = synth_range(&rng, 0.10, 0.25);
        l->step = (int64_t)llround(fs * synth_range(&rng, 0.2, 1.2));
        sweep_line* l2 = &lines[count++];
        l2->hz = 2.0 * uhz;
        l2->amp = l->amp * synth_range(&rng, 0.0, 0.4);
        (void)synth_range(&rng, 0.0, 2.0 * M_PI); /* the sweep draws a phase it then replaces */
        l2->phase = 2.0 * l->phase;
        l2->amp2 = l->amp2 * (l2->amp / l->amp);
        l2->step = l->step;
    }
    dsd_analog_rx_core_init(&g_core);
    const int block = (int)llround(fs / 1000.0);
    float buf[128];
    assert(block > 0 && block <= (int)(sizeof(buf) / sizeof(buf[0])));
    const int64_t total = lines[0].on + (int64_t)llround(fs * 2.0);
    double lock_ms = -1.0;
    *final_tenths = 0;
    for (int64_t n = 0; n < total; n += block) {
        const int m = (total - n) < block ? (int)(total - n) : block;
        for (int i = 0; i < m; i++) {
            double v = 0.0;
            for (int j = 0; j < count; j++) {
                sweep_line* l = &lines[j];
                l->acc += 2.0 * M_PI * l->hz / fs;
                if (n + i >= l->on) {
                    v += (n + i >= l->step ? l->amp2 : l->amp) * cos(l->acc + l->phase);
                }
            }
            if (voice) {
                v += synth_speech_next(&speech);
            }
            if (sigma > 0.0) {
                v += sigma * synth_gauss(&rng);
            }
            buf[i] = (float)v;
        }
        assert(dsd_analog_rx_core_process(&g_core, buf, m, fs, 1) == 1);
        dsd_analog_rx_publication pub;
        dsd_analog_rx_core_publish(&g_core, &pub);
        const double t = ((double)(n + m) * 1000.0 / fs) - onset_ms;
        const int locked =
            pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && pub.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS;
        if (locked && pub.ctcss_tenths_hz == expect && lock_ms < 0.0 && t >= 0.0) {
            lock_ms = t;
        }
        *final_tenths = locked ? pub.ctcss_tenths_hz : 0;
    }
    return lock_ms;
}

/*
 * A lock that rests on tone error alone, which the on-value rules never had, leaves everything they hold as it was
 * (issue #643): receptions of the long-run sweeps where a voice line beside the tone locked its own nearest tone off
 * its value first, and where speech moved a held tone's estimate past 0.8 Hz for one hop. Before, the line's lock
 * coming and going started the late windows over and its reverse burst held acquisition off, so the real tone locked
 * 200-600 ms later than on the on-value rules alone, or not within 2 s; and the hop the wider hold kept moved the
 * reverse-burst reference, so a burst check the on-value rules pass dropped the held tone. Each reception here locks
 * its tone at the time the on-value rules alone lock it (measured on the detector before tone error was allowed for),
 * and the held tone is still held when the run ends.
 */
static void
test_off_value_lock_leaves_the_on_value_rules_alone(void) {
    static const struct {
        uint64_t seed;
        double offset_hz;
        double snr_db;
        int voice;
        int beside;
        double main_lock_ms;
        int main_final;
    } k_cases[] = {
        {9212300006157ULL, 0.0, 200.0, 0, 1, 1141.22, 1679}, {9212300094839ULL, 0.0, 200.0, 0, 1, 1082.36, 1413},
        {1581250011998ULL, 0.0, 30.0, 0, 1, 465.54, 0},      {2062660032304ULL, 0.2, 20.0, 0, 1, 1110.86, 1773},
        {1330270021877ULL, 0.0, 30.0, 1, 0, 303.39, 2503},
    };

    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        int final_tenths = -1;
        const double lock_ms = sweep_reception(k_cases[i].seed, k_cases[i].offset_hz, k_cases[i].snr_db,
                                               k_cases[i].voice, k_cases[i].beside, &final_tenths);
        DSD_FPRINTF(stderr, "sweep reception %llu: lock %.2f ms (on-value rules %.2f), final %d (%d)\n",
                    (unsigned long long)k_cases[i].seed, lock_ms, k_cases[i].main_lock_ms, final_tenths,
                    k_cases[i].main_final);
        assert(fabs(lock_ms - k_cases[i].main_lock_ms) < 0.01);
        if (k_cases[i].main_final != 0) {
            assert(final_tenths == k_cases[i].main_final);
        }
    }
}

/*
 * An off-value lock leaves the on-value rules' late run where it was (issue #643): 250.72 Hz (0.17 % high on 250.3) at
 * 0.1 beside 242.32 Hz at 0.041, two noiseless zero-phase cosines. The tone locks off its value first; the on-value
 * rules, whose late windows were already agreeing on 250.3 Hz, confirm it at the time they always did -- 500 ms, 520 ms
 * at 44.1 kHz -- not one hop later as they did when the off-value lock cleared their run.
 */
static void
test_off_value_lock_keeps_the_on_value_late_run(void) {
    static const double k_main_confirm_ms[RATE_COUNT] = {500.0, 520.0, 500.0, 499.8};
    for (int ri = 0; ri < RATE_COUNT; ri++) {
        const int fs = k_rates[ri];
        const int block = fs / 50;
        float* buf = (float*)malloc(sizeof(float) * (size_t)block);
        assert(buf != NULL);
        dsd_analog_rx_core_init(&g_core);
        double confirmed_ms = -1.0;
        int locked_other = 0;
        int final_tenths = 0;
        for (int64_t n = 0; n < (int64_t)fs * 2; n += block) {
            for (int i = 0; i < block; i++) {
                const double t = (double)(n + i) / fs;
                buf[i] = (float)((0.1 * cos(2.0 * M_PI * 250.72 * t)) + (0.041 * cos(2.0 * M_PI * 242.32 * t)));
            }
            assert(dsd_analog_rx_core_process(&g_core, buf, block, fs, 1) == 1);
            dsd_analog_rx_publication pub;
            dsd_analog_rx_core_publish(&g_core, &pub);
            const int locked = pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED;
            locked_other |= locked && pub.ctcss_tenths_hz != 2503;
            if (locked && pub.ctcss_tenths_hz == 2503 && !pub.ctcss_off_value && confirmed_ms < 0.0) {
                confirmed_ms = 1000.0 * (double)(n + block) / fs;
            }
            final_tenths = locked ? pub.ctcss_tenths_hz : 0;
        }
        free(buf);
        DSD_FPRINTF(stderr, "250.72 Hz beside 242.32 Hz fs=%d: confirmed on its value at %.1f ms (%.1f before)\n", fs,
                    confirmed_ms, k_main_confirm_ms[ri]);
        assert(fabs(confirmed_ms - k_main_confirm_ms[ri]) < 0.05);
        assert(!locked_other && final_tenths == 2503);
    }
}

/*
 * The loss of a lock that rested on tone error alone returns to the on-value rules' own verdict (issue #643), here
 * NONE: under a carrier that opens 1 ms in every 10 (squelch chatter the 200 ms hangover bridges, so carrier time
 * counts slowly), 100.0 Hz locks and is lost before 500 ms of carrier have been read, which leaves those rules at NONE;
 * then 251.4 Hz (0.44 % above 250.3) locks off its value and stops. Its loss reads NONE, not the ACQUIRING the
 * detector held before any loss. The noise floor that keeps the carrier is a fixed hash, the same everywhere.
 */
static void
test_off_value_loss_keeps_the_on_value_verdict(void) {
    const int fs = 48000;
    const int block = fs / 1000;
    float buf[48];
    assert(block <= (int)(sizeof(buf) / sizeof(buf[0])));
    dsd_analog_rx_core_init(&g_core);
    int none_before = 0;
    int off_value_lock = 0;
    int acquiring_after_none = 0;
    for (int64_t n = 0; n < (int64_t)fs * 2; n += block) {
        const int ms = (int)(n / block);
        for (int i = 0; i < block; i++) {
            const double t = (double)(n + i) / fs;
            uint32_t h = (uint32_t)((uint64_t)(n + i) * 2654435761ULL);
            h ^= h >> 13;
            h *= 0x5bd1e995U;
            h ^= h >> 15;
            double v = 0.002 * (((double)(h & 0xffffU) / 32768.0) - 1.0);
            if (t < 0.350) {
                v += 0.1 * cos(2.0 * M_PI * 100.0 * t);
            }
            if (t >= 0.450 && t < 1.300) {
                v += 0.1 * cos(2.0 * M_PI * 251.4 * t);
            }
            buf[i] = (float)v;
        }
        assert(dsd_analog_rx_core_process(&g_core, buf, block, fs, ms % 10 == 0) == 1);
        dsd_analog_rx_publication pub;
        dsd_analog_rx_core_publish(&g_core, &pub);
        if (g_core.ctcss.state == DSD_ANALOG_TONE_STATE_NONE && ms < 700) {
            none_before = 1;
        }
        off_value_lock |=
            pub.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && pub.ctcss_tenths_hz == 2503 && pub.ctcss_off_value;
        acquiring_after_none |= none_before && g_core.ctcss.state == DSD_ANALOG_TONE_STATE_ACQUIRING;
    }
    /* The carrier held throughout: carrier time stayed short of the 500 ms no-tone verdict. */
    assert(g_core.ctcss.open_samples < (int64_t)fs / 2);
    assert(none_before && off_value_lock && !acquiring_after_none);
    assert(g_core.ctcss.state == DSD_ANALOG_TONE_STATE_NONE);
}

int
main(void) {
    test_150_6_reads_150_0();
    test_off_value_tones_lock();
    test_pair_midpoint_names_neither();
    test_estimate_precision_is_calibrated();
    test_on_value_tone_is_never_pre_empted();
    test_moved_tone_is_kept_and_yields();
    test_onset_never_names_the_other_tone();
    test_off_value_lock_keeps_the_dcs_candidate();
    test_off_value_lock_leaves_the_on_value_rules_alone();
    test_off_value_lock_keeps_the_on_value_late_run();
    test_off_value_loss_keeps_the_on_value_verdict();
    printf("DSP_ANALOG_CTCSS_TONE_ERROR: OK\n");
    return 0;
}
