// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The auto squelch's classifier, floor and gate (issue #518 follow-up), in sample time on seeded signals through the
 * repository's own channel taps: the plan constants against the design model (tools/squelch_model.py), noise that
 * never opens the gate at any margin, carriers that open and close it, a carrier that is never learned as floor,
 * noise level steps, seeding, the per-channel cache, and flags that do not depend on how the samples are cut into
 * blocks. Every bound is in samples of the channel rate.
 */

#include <assert.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "analog_tone_synth.h"

enum { MAX_TAPS = 512, SRC_NONE = 0, SRC_FM = 1, SRC_AM = 2 };

static int
near_rel(double a, double b, double rel) {
    return fabs(a - b) <= rel * fmax(fabs(a), fabs(b));
}

static double
db(double ratio) {
    return 10.0 * log10(ratio);
}

/* ------------------------------------------------------------------------------------------------- channel plans */

typedef struct {
    int rate;
    int width; /* 0: no channel filter */
    float taps[MAX_TAPS];
    int taps_len;
    double sum_h;
    double sum_h2;
    dsd_squelch_floor_plan plan;
} channel;

/* The channel taps full_demod() designs for @p width at @p rate, and the classifier plan for them with the half-band
   stage @p hb (NULL: none). */
static void
channel_make(channel* ch, int rate, int width, const float* hb, int hb_len) {
    DSD_MEMSET(ch, 0, sizeof(*ch));
    ch->rate = rate;
    ch->width = width;
    if (width > 0) {
        ch->taps_len = dsd_channel_lpf_design_analog(rate, width, ch->taps, MAX_TAPS);
        assert(ch->taps_len > 0);
    } else {
        ch->taps[0] = 1.0f;
        ch->taps_len = 1;
    }
    for (int i = 0; i < ch->taps_len; i++) {
        ch->sum_h += (double)ch->taps[i];
        ch->sum_h2 += (double)ch->taps[i] * (double)ch->taps[i];
    }
    const int rc = dsd_squelch_floor_plan_design(&ch->plan, width > 0 ? ch->taps : NULL, width > 0 ? ch->taps_len : 0,
                                                 hb, hb_len, rate);
    assert(rc == 0 && ch->plan.valid);
}

/* ------------------------------------------------------------------------------------------------ signal source */

/* White complex noise plus an optional carrier, through the channel taps. */
typedef struct {
    synth_rng rng;
    const channel* ch;
    double re[2 * MAX_TAPS];
    double im[2 * MAX_TAPS];
    int pos;
    double noise_var; /* complex variance at the filter input */
    int kind;
    double amp;
    double carrier_phase;
    double carrier_step;
    double mod_phase;
    double mod_step;
    double fm_index; /* peak phase deviation, rad */
    double am_depth;
} source;

static void
source_init(source* s, const channel* ch, uint64_t seed, double noise_var) {
    DSD_MEMSET(s, 0, sizeof(*s));
    synth_rng_seed(&s->rng, seed);
    s->ch = ch;
    s->noise_var = noise_var;
}

/* Noise power at the channel output. */
static double
source_noise_out(const source* s) {
    return s->noise_var * s->ch->sum_h2;
}

static void
source_noise_only(source* s) {
    s->kind = SRC_NONE;
}

/* An FM carrier @p cnr_db above the channel-output noise, offset @p offset_hz, a @p tone_hz tone at @p dev_hz peak
   deviation (0: unmodulated). */
static void
source_fm(source* s, double cnr_db, double offset_hz, double tone_hz, double dev_hz) {
    s->kind = SRC_FM;
    s->amp = sqrt(source_noise_out(s) * pow(10.0, cnr_db / 10.0)) / s->ch->sum_h;
    s->carrier_step = 2.0 * M_PI * offset_hz / (double)s->ch->rate;
    s->mod_step = 2.0 * M_PI * tone_hz / (double)s->ch->rate;
    s->fm_index = tone_hz > 0.0 ? dev_hz / tone_hz : 0.0;
}

/* An AM carrier (its unmodulated power @p cnr_db above the noise) with a @p tone_hz tone at @p depth. */
static void
source_am(source* s, double cnr_db, double tone_hz, double depth) {
    s->kind = SRC_AM;
    s->amp = sqrt(source_noise_out(s) * pow(10.0, cnr_db / 10.0)) / s->ch->sum_h;
    s->carrier_step = 0.0;
    s->mod_step = 2.0 * M_PI * tone_hz / (double)s->ch->rate;
    s->am_depth = depth;
}

static void
source_run(source* s, float* iq, int count) {
    const double sd = sqrt(s->noise_var / 2.0);
    const int len = s->ch->taps_len;
    for (int n = 0; n < count; n++) {
        double xr = sd * synth_gauss(&s->rng);
        double xi = sd * synth_gauss(&s->rng);
        if (s->kind == SRC_FM) {
            const double ph = s->carrier_phase + (s->fm_index * sin(s->mod_phase));
            xr += s->amp * cos(ph);
            xi += s->amp * sin(ph);
        } else if (s->kind == SRC_AM) {
            const double a = s->amp * (1.0 + (s->am_depth * cos(s->mod_phase)));
            xr += a * cos(s->carrier_phase);
            xi += a * sin(s->carrier_phase);
        }
        s->carrier_phase = fmod(s->carrier_phase + s->carrier_step, 2.0 * M_PI);
        s->mod_phase = fmod(s->mod_phase + s->mod_step, 2.0 * M_PI);
        /* Newest at pos + len, so x[n - k] sits at pos + len - k. */
        s->pos = (s->pos + 1) % len;
        s->re[s->pos] = s->re[s->pos + len] = xr;
        s->im[s->pos] = s->im[s->pos + len] = xi;
        double yr = 0.0;
        double yi = 0.0;
        const double* hr = &s->re[s->pos + len];
        const double* hi = &s->im[s->pos + len];
        for (int k = 0; k < len; k++) {
            yr += (double)s->ch->taps[k] * hr[-k];
            yi += (double)s->ch->taps[k] * hi[-k];
        }
        iq[(size_t)n * 2U] = (float)yr;
        iq[((size_t)n * 2U) + 1U] = (float)yi;
    }
}

/* ------------------------------------------------------------------------------------------------------- runner */

enum { MAX_TRACKERS = 3, MAX_CHUNK = 96 };

/* Up to three trackers (one per margin) fed the same samples a millisecond at a time. */
typedef struct {
    dsd_squelch_floor t[MAX_TRACKERS];
    int count;
    uint64_t samples;
    uint64_t opens[MAX_TRACKERS];       /* samples flagged open */
    uint64_t known_at[MAX_TRACKERS];    /* windows when first KNOWN (0: never) */
    int ever_learning[MAX_TRACKERS];    /* LEARNING seen after KNOWN */
    int64_t first_open[MAX_TRACKERS];   /* sample index of the first open flag since runner_mark() (-1: none) */
    int64_t first_closed[MAX_TRACKERS]; /* the same for a closed flag */
    uint64_t mark;
} runner;

static void
runner_init(runner* r, const channel* ch, const int* margins, int count) {
    DSD_MEMSET(r, 0, sizeof(*r));
    r->count = count;
    for (int i = 0; i < count; i++) {
        dsd_squelch_floor_set_margin(&r->t[i], margins[i]);
        dsd_squelch_floor_set_plan(&r->t[i], &ch->plan);
        r->first_open[i] = -1;
        r->first_closed[i] = -1;
    }
}

static void
runner_mark(runner* r) {
    r->mark = r->samples;
    for (int i = 0; i < r->count; i++) {
        r->first_open[i] = -1;
        r->first_closed[i] = -1;
        r->opens[i] = 0U;
    }
}

static void
runner_run(runner* r, source* s, double seconds) {
    const int rate = s->ch->rate;
    const int chunk = rate / 1000;
    assert(chunk > 0 && chunk <= MAX_CHUNK);
    float iq[2 * MAX_CHUNK];
    uint8_t flags[MAX_CHUNK];
    const long chunks = lround(seconds * 1000.0);
    for (long c = 0; c < chunks; c++) {
        source_run(s, iq, chunk);
        for (int i = 0; i < r->count; i++) {
            dsd_squelch_floor_process(&r->t[i], iq, chunk, flags);
            for (int k = 0; k < chunk; k++) {
                const int64_t at = (int64_t)(r->samples - r->mark) + k;
                if ((flags[k] & DSD_SQUELCH_FLAG_CLOSED) == 0U) {
                    r->opens[i]++;
                    if (r->first_open[i] < 0) {
                        r->first_open[i] = at;
                    }
                } else if (r->first_closed[i] < 0) {
                    r->first_closed[i] = at;
                }
            }
            dsd_squelch_floor_status st;
            dsd_squelch_floor_get_status(&r->t[i], &st);
            if (st.state == DSD_SQUELCH_FLOOR_KNOWN && r->known_at[i] == 0U) {
                r->known_at[i] = st.windows;
            } else if (st.state == DSD_SQUELCH_FLOOR_LEARNING && r->known_at[i] != 0U) {
                r->ever_learning[i] = 1;
            }
        }
        r->samples += (uint64_t)chunk;
    }
}

static dsd_squelch_floor_status
status_of(const dsd_squelch_floor* t) {
    dsd_squelch_floor_status st;
    dsd_squelch_floor_get_status(t, &st);
    return st;
}

static int
sub_len(int rate) {
    return (rate + 49) / 50;
}

static int
window_len(int rate) {
    return (rate + 24) / 25;
}

/* --------------------------------------------------------------------------------------------- plan constants */

typedef struct {
    const char* name;
    int rate;
    int width;
    int hb15;
    int taps;
    int lag;
    double rho;
    double neff40;
    double beta;
} plan_ref;

/* build/squelch_model/report.json (tools/squelch_model.py at f9d2440f5): lag, rho_n(L), 2 x the 20 ms N_eff and beta
   for plans through the last half-band stage (hb15) and without one (replay at the channel rate). */
static const plan_ref k_plan_refs[] = {
    {"NFM 12500 @ 48 kHz, hb15", 48000, 12500, 1, 135, 4, -0.020926346001035386, 535.0147517798636,
     -0.016436413527103313},
    {"NFM 12500 @ 48 kHz", 48000, 12500, 0, 135, 4, -0.09689519845191712, 545.6382042607455, -0.07619093824808792},
    {"NFM 16000 @ 24 kHz, hb15", 24000, 16000, 1, 67, 3, 0.09971073995883625, 662.3346324386125, 0.07841032207787794},
    {"NFM 25000 @ 48 kHz, hb15", 48000, 25000, 1, 135, 2, 0.06207872934723177, 1006.9514226390868,
     0.048780041069727574},
    {"AM 6000 @ 12 kHz, hb15", 12000, 6000, 1, 33, 4, 0.09248317869331897, 272.3365260673836, 0.0727140272362788},
    {"AM 20000 @ 24 kHz, hb15", 24000, 20000, 1, 67, 4, -0.07237604456034484, 816.8432255964372, -0.056881306477338835},
    {"no channel filter @ 24 kHz, hb15", 24000, 0, 1, 1, 1, 0.10660703099456009, 926.5735274345914, 0.0838484243955978},
    {"no channel filter @ 48 kHz", 48000, 0, 0, 1, 1, 0.0, 1920.0, 0.0},
};

static void
test_plan_constants(void) {
    for (size_t i = 0; i < sizeof k_plan_refs / sizeof k_plan_refs[0]; i++) {
        const plan_ref* ref = &k_plan_refs[i];
        channel ch;
        channel_make(&ch, ref->rate, ref->width, ref->hb15 ? hb_q15_taps : NULL, ref->hb15 ? HB_TAPS : 0);
        const dsd_squelch_floor_plan* p = &ch.plan;
        const int ok = ch.taps_len == ref->taps && p->lag == ref->lag && p->rate_hz == ref->rate
                       && fabs(p->rho_lag - ref->rho) <= 1e-9 && near_rel(p->neff, ref->neff40, 1e-9)
                       && fabs(p->beta - ref->beta) <= 1e-9 && near_rel(p->sqrt_neff * p->sqrt_neff, p->neff, 1e-12);
        if (!ok) {
            DSD_FPRINTF(stderr, "%s: taps %d lag %d rho %.12f neff %.9f beta %.12f\n", ref->name, ch.taps_len, p->lag,
                        p->rho_lag, p->neff, p->beta);
            assert(0);
        }
        if (!ref->hb15) {
            /* Without a half-band stage white noise comes out at sum h^2. */
            assert(near_rel(p->noise_gain, ch.sum_h2, 1e-12));
        }
    }

    dsd_squelch_floor_plan p;
    assert(dsd_squelch_floor_plan_design(&p, NULL, 0, NULL, 0, 99) == -1 && !p.valid);
    assert(dsd_squelch_floor_plan_design(&p, NULL, 0, NULL, 0, 100) == 0 && p.valid && p.lag == 1);
    assert(dsd_squelch_floor_plan_design(NULL, NULL, 0, NULL, 0, 48000) == -1);
    /* A 64-tap boxcar stays correlated to lag 58: past the longest lag a plan may use. */
    static float boxcar[64];
    for (int i = 0; i < 64; i++) {
        boxcar[i] = 1.0f / 64.0f;
    }
    assert(dsd_squelch_floor_plan_design(&p, boxcar, 64, NULL, 0, 48000) == -1 && !p.valid);
    static const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    assert(dsd_squelch_floor_plan_design(&p, zeros, 4, NULL, 0, 48000) == -1 && !p.valid);

    channel a;
    channel b;
    channel_make(&a, 48000, 12500, NULL, 0);
    channel_make(&b, 48000, 12500, NULL, 0);
    assert(dsd_squelch_floor_plan_equal(&a.plan, &b.plan));
    channel_make(&b, 48000, 12500, hb_q15_taps, HB_TAPS);
    assert(!dsd_squelch_floor_plan_equal(&a.plan, &b.plan));
    channel_make(&b, 24000, 12500, NULL, 0);
    assert(!dsd_squelch_floor_plan_equal(&a.plan, &b.plan));
    dsd_squelch_floor_plan none_a;
    dsd_squelch_floor_plan none_b;
    DSD_MEMSET(&none_a, 0, sizeof none_a);
    DSD_MEMSET(&none_b, 0, sizeof none_b);
    assert(dsd_squelch_floor_plan_equal(&none_a, &none_b));
    assert(!dsd_squelch_floor_plan_equal(&none_a, &a.plan));
    assert(!dsd_squelch_floor_plan_equal(NULL, &a.plan));
}

/* ------------------------------------------------------------------------------------------------- classifier */

static void
test_classify(void) {
    channel ch;
    channel_make(&ch, 48000, 12500, NULL, 0);
    const dsd_squelch_floor_plan* p = &ch.plan;
    const double n = 1920.0;
    const double root = p->sqrt_neff;
    /* Constant envelope: CV^2 = 0, X = -sqrt(N_eff) (about -23). */
    assert(dsd_squelch_floor_classify(p, n, n, n, 0.0, 0.0) == DSD_SQUELCH_WINDOW_CARRIER);
    /* Noise-like: CV^2 = 1, coherence at beta. */
    assert(dsd_squelch_floor_classify(p, n, n, 2.0 * n, p->beta * n, 0.0) == DSD_SQUELCH_WINDOW_NOISE);
    assert(dsd_squelch_floor_classify(p, n, 3.0 * n, 18.0 * n, p->beta * n, 0.0) == DSD_SQUELCH_WINDOW_NOISE);
    /* A steady phase: Y = 4 (an AM carrier, whatever its envelope). */
    assert(dsd_squelch_floor_classify(p, n, n, 3.0 * n, (p->beta + 4.0 / root) * n, 0.0) == DSD_SQUELCH_WINDOW_CARRIER);
    assert(dsd_squelch_floor_classify(p, n, n, 2.0 * n, p->beta * n, (4.0 / root) * n) == DSD_SQUELCH_WINDOW_CARRIER);
    /* Between the classes: X = -5, or Y = 2.7. */
    assert(dsd_squelch_floor_classify(p, n, n, (2.0 - 5.0 / root) * n, p->beta * n, 0.0)
           == DSD_SQUELCH_WINDOW_UNDECIDED);
    assert(dsd_squelch_floor_classify(p, n, n, 2.0 * n, (p->beta + 2.7 / root) * n, 0.0)
           == DSD_SQUELCH_WINDOW_UNDECIDED);
    /* Nothing to classify. */
    assert(dsd_squelch_floor_classify(p, 1.0, 1.0, 1.0, 0.0, 0.0) == DSD_SQUELCH_WINDOW_UNDECIDED);
    assert(dsd_squelch_floor_classify(p, n, 0.0, 0.0, 0.0, 0.0) == DSD_SQUELCH_WINDOW_UNDECIDED);
    assert(dsd_squelch_floor_classify(p, n, 1e-40 * n, 1e-80 * n, 0.0, 0.0) == DSD_SQUELCH_WINDOW_UNDECIDED);
    assert(dsd_squelch_floor_classify(NULL, n, n, n, 0.0, 0.0) == DSD_SQUELCH_WINDOW_UNDECIDED);
    dsd_squelch_floor_plan none;
    DSD_MEMSET(&none, 0, sizeof none);
    assert(dsd_squelch_floor_classify(&none, n, n, n, 0.0, 0.0) == DSD_SQUELCH_WINDOW_UNDECIDED);
}

/* ------------------------------------------------------------------------------------------------------- noise */

static const int k_margins[MAX_TRACKERS] = {3, 6, 10};

/* A minute of noise: the floor is learned within 8 windows (320 ms) to within 0.5 dB, and the gate never opens. */
static void
noise_never_opens(int rate, int width, uint64_t seed) {
    static channel ch;
    static source s;
    static runner r;
    channel_make(&ch, rate, width, NULL, 0);
    source_init(&s, &ch, seed, 1e-3);
    runner_init(&r, &ch, k_margins, MAX_TRACKERS);
    runner_run(&r, &s, 60.0);
    const double expect = source_noise_out(&s);
    for (int i = 0; i < MAX_TRACKERS; i++) {
        const dsd_squelch_floor_status st = status_of(&r.t[i]);
        if (r.opens[i] != 0U || r.known_at[i] < (uint64_t)DSD_SQUELCH_FLOOR_LEARN_NEED || r.known_at[i] > 8U
            || r.ever_learning[i] || st.state != DSD_SQUELCH_FLOOR_KNOWN || fabs(db(st.floor_power / expect)) > 0.5) {
            DSD_FPRINTF(stderr,
                        "noise %d Hz / %d Hz, margin %d: opens %llu, known at window %llu, relearned %d, "
                        "floor %+.2f dB\n",
                        rate, width, k_margins[i], (unsigned long long)r.opens[i], (unsigned long long)r.known_at[i],
                        r.ever_learning[i], db(st.floor_power / expect));
            assert(0);
        }
    }
    printf("  noise %5d Hz / %5d Hz: known at window %llu, floor %+.2f dB, no opens at +3/+6/+10 dB over 60 s\n", rate,
           width, (unsigned long long)r.known_at[0], db(status_of(&r.t[0]).floor_power / expect));
}

static void
test_noise_never_opens(void) {
    noise_never_opens(48000, 12500, 0x5151000148ULL);
    noise_never_opens(48000, 25000, 0x5151000248ULL);
    noise_never_opens(12000, 8000, 0x5151000312ULL);
    noise_never_opens(12000, 5000, 0x5151000412ULL);
    noise_never_opens(24000, 0, 0x5151000524ULL);
}

/* --------------------------------------------------------------------------------------------------- carriers */

typedef struct {
    const char* name;
    int kind;
    double cnr_db;
    double offset_hz;
    double tone_hz;
    double mod; /* FM peak deviation in Hz, or AM depth */
} carrier_case;

static void
source_carrier(source* s, const carrier_case* c) {
    if (c->kind == SRC_FM) {
        source_fm(s, c->cnr_db, c->offset_hz, c->tone_hz, c->mod);
    } else {
        source_am(s, c->cnr_db, c->tone_hz, c->mod);
    }
}

/* With a floor, each carrier opens the gate within two windows of its start and holds it open throughout, and it
   closes within two sub-windows of its end; the floor does not move. */
static void
test_carriers_open_and_close(void) {
    static const carrier_case cases[] = {
        {"FM 1 kHz at 2.5 kHz deviation, 20 dB", SRC_FM, 20.0, 0.0, 1000.0, 2500.0},
        {"AM 1 kHz at 80 %, 20 dB", SRC_AM, 20.0, 0.0, 1000.0, 0.8},
        {"unmodulated carrier 1 kHz off, 15 dB", SRC_FM, 15.0, 1000.0, 0.0, 0.0},
        {"FM 300 Hz at 2.5 kHz deviation, 13 dB", SRC_FM, 13.0, 0.0, 300.0, 2500.0},
    };
    static channel ch;
    static source s;
    static runner r;
    const int margin = 10;
    channel_make(&ch, 48000, 12500, NULL, 0);
    source_init(&s, &ch, 0xCA771E5ULL, 1e-3);
    runner_init(&r, &ch, &margin, 1);
    runner_run(&r, &s, 1.0);
    const double floor0 = status_of(&r.t[0]).floor_power;
    assert(status_of(&r.t[0]).state == DSD_SQUELCH_FLOOR_KNOWN && r.opens[0] == 0U);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        /* Start the carrier off any window boundary. */
        runner_run(&r, &s, 0.013);
        runner_mark(&r);
        source_carrier(&s, &cases[i]);
        runner_run(&r, &s, 1.0);
        const int64_t on = r.first_open[0];
        const uint64_t span = r.samples - r.mark;
        if (on < 0 || on > 2 * (int64_t)window_len(48000) || r.opens[0] != span - (uint64_t)on) {
            DSD_FPRINTF(stderr, "%s: opened at %lld, open %llu of %llu after it\n", cases[i].name, (long long)on,
                        (unsigned long long)r.opens[0], (unsigned long long)(span - (uint64_t)(on < 0 ? 0 : on)));
            assert(0);
        }
        runner_mark(&r);
        source_noise_only(&s);
        runner_run(&r, &s, 1.0);
        const int64_t off = r.first_closed[0];
        const int64_t reopened = r.opens[0] > (uint64_t)(off < 0 ? 0 : off) ? 1 : 0;
        if (off < 0 || off > 2 * sub_len(48000) + 1 || reopened) {
            DSD_FPRINTF(stderr, "%s: closed at %lld, open %llu samples after the end\n", cases[i].name, (long long)off,
                        (unsigned long long)r.opens[0]);
            assert(0);
        }
        printf("  %-40s opens after %4.1f ms, closes %4.1f ms after its end\n", cases[i].name, (double)on / 48.0,
               (double)off / 48.0);
    }
    const dsd_squelch_floor_status st = status_of(&r.t[0]);
    assert(st.state == DSD_SQUELCH_FLOOR_KNOWN && fabs(db(st.floor_power / floor0)) <= 0.3);
}

/* Strongly modulated FM and heavy AM at a low CNR, for 10 s each, leave the floor where it was. */
static void
test_modulation_keeps_floor(void) {
    static const carrier_case cases[] = {
        {"FM 3 kHz at 2.5 kHz deviation, 6 dB", SRC_FM, 6.0, 0.0, 3000.0, 2500.0},
        {"FM 500 Hz at 2.5 kHz deviation, 6 dB", SRC_FM, 6.0, 0.0, 500.0, 2500.0},
        {"AM 2 kHz at 100 %, 6 dB", SRC_AM, 6.0, 0.0, 2000.0, 1.0},
        {"AM 300 Hz at 100 %, 6 dB", SRC_AM, 6.0, 0.0, 300.0, 1.0},
    };
    static channel ch;
    static source s;
    static runner r;
    const int margin = 3;
    channel_make(&ch, 24000, 12500, NULL, 0);
    source_init(&s, &ch, 0x0D0D0D0DULL, 1e-3);
    runner_init(&r, &ch, &margin, 1);
    runner_run(&r, &s, 1.0);
    const double floor0 = status_of(&r.t[0]).floor_power;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        runner_mark(&r);
        source_carrier(&s, &cases[i]);
        runner_run(&r, &s, 10.0);
        const dsd_squelch_floor_status st = status_of(&r.t[0]);
        const double open_share = (double)r.opens[0] / (double)(r.samples - r.mark);
        if (st.state != DSD_SQUELCH_FLOOR_KNOWN || fabs(db(st.floor_power / floor0)) > 0.5 || open_share < 0.99) {
            DSD_FPRINTF(stderr, "%s: state %d, floor %+.2f dB, open %.4f\n", cases[i].name, st.state,
                        db(st.floor_power / floor0), open_share);
            assert(0);
        }
        source_noise_only(&s);
        runner_run(&r, &s, 0.5);
    }
}

/* A carrier from the first sample: open from the first window on, for over two minutes, and never learned as floor;
   when it ends the floor is learned from the noise. */
static void
test_landing_mid_carrier(void) {
    static const carrier_case cases[] = {
        {"unmodulated carrier, 15 dB", SRC_FM, 15.0, 400.0, 0.0, 0.0},
        {"AM 1 kHz at 90 %, 15 dB", SRC_AM, 15.0, 0.0, 1000.0, 0.9},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        static channel ch;
        static source s;
        static runner r;
        const int margin = 10;
        const double seconds = i == 0 ? 130.0 : 20.0;
        channel_make(&ch, 12000, 6000, NULL, 0);
        source_init(&s, &ch, 0x1A4D1A4DULL + i, 1e-3);
        runner_init(&r, &ch, &margin, 1);
        source_carrier(&s, &cases[i]);
        runner_run(&r, &s, seconds);
        const dsd_squelch_floor_status st = status_of(&r.t[0]);
        const uint64_t first_window = (uint64_t)(12000 / 25);
        if (st.state != DSD_SQUELCH_FLOOR_LEARNING || r.known_at[0] != 0U || r.first_open[0] != (int64_t)first_window
            || r.opens[0] != r.samples - first_window) {
            DSD_FPRINTF(stderr, "%s: state %d, opened at %lld, open %llu of %llu\n", cases[i].name, st.state,
                        (long long)r.first_open[0], (unsigned long long)r.opens[0],
                        (unsigned long long)(r.samples - first_window));
            assert(0);
        }
        runner_mark(&r);
        source_noise_only(&s);
        runner_run(&r, &s, 1.0);
        const dsd_squelch_floor_status after = status_of(&r.t[0]);
        assert(r.first_closed[0] >= 0 && r.first_closed[0] <= 2 * window_len(12000) + 1);
        assert(after.state == DSD_SQUELCH_FLOOR_KNOWN && fabs(db(after.floor_power / source_noise_out(&s))) <= 0.5);
        printf("  %-40s held open %.0f s, closed %4.1f ms after it ended\n", cases[i].name, seconds,
               (double)r.first_closed[0] / 12.0);
    }
}

/* ------------------------------------------------------------------------------------------------ level steps */

/* The noise steps: +10 dB is tracked (the floor rises 1 dB/s), +20 dB held 5 s starts LEARNING again, -10 dB falls
   fast, -25 dB starts LEARNING at once. No step opens the gate for more than one window. */
static void
test_level_steps(void) {
    static channel ch;
    static source s;
    static runner r;
    static const int margins[2] = {3, 10};
    const int rate = 12000;
    channel_make(&ch, rate, 8000, NULL, 0);
    source_init(&s, &ch, 0x57E95ULL, 1e-3);
    runner_init(&r, &ch, margins, 2);
    runner_run(&r, &s, 2.0);

    /* +10 dB: tracked, no relearning. */
    runner_mark(&r);
    s.noise_var = 1e-2;
    runner_run(&r, &s, 15.0);
    for (int i = 0; i < 2; i++) {
        const dsd_squelch_floor_status st = status_of(&r.t[i]);
        if (r.opens[i] > (uint64_t)window_len(rate) || r.ever_learning[i] || st.state != DSD_SQUELCH_FLOOR_KNOWN
            || fabs(db(st.floor_power / source_noise_out(&s))) > 0.5) {
            DSD_FPRINTF(stderr, "+10 dB, margin %d: open %llu, relearned %d, floor %+.2f dB\n", margins[i],
                        (unsigned long long)r.opens[i], r.ever_learning[i], db(st.floor_power / source_noise_out(&s)));
            assert(0);
        }
    }

    /* -10 dB: falls within a second. */
    runner_mark(&r);
    s.noise_var = 1e-3;
    runner_run(&r, &s, 1.0);
    for (int i = 0; i < 2; i++) {
        const dsd_squelch_floor_status st = status_of(&r.t[i]);
        assert(r.opens[i] == 0U && !r.ever_learning[i] && st.state == DSD_SQUELCH_FLOOR_KNOWN);
        assert(fabs(db(st.floor_power / source_noise_out(&s))) <= 0.5);
    }

    /* +20 dB: LEARNING again after 125 NOISE windows (5 s, and the odd UNDECIDED window between them), then a floor
       at the new level. */
    runner_mark(&r);
    s.noise_var = 1e-1;
    int relearned_ms[2] = {-1, -1};
    for (int ms = 10; ms <= 12000; ms += 10) {
        runner_run(&r, &s, 0.01);
        for (int i = 0; i < 2; i++) {
            if (relearned_ms[i] < 0 && status_of(&r.t[i]).state == DSD_SQUELCH_FLOOR_LEARNING) {
                relearned_ms[i] = ms;
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        if (relearned_ms[i] < 5000 || relearned_ms[i] > 5600) {
            DSD_FPRINTF(stderr, "+20 dB, margin %d: relearned after %d ms\n", margins[i], relearned_ms[i]);
            assert(0);
        }
        const dsd_squelch_floor_status st = status_of(&r.t[i]);
        assert(r.ever_learning[i] && st.state == DSD_SQUELCH_FLOOR_KNOWN);
        assert(r.opens[i] <= (uint64_t)window_len(rate));
        assert(fabs(db(st.floor_power / source_noise_out(&s))) <= 0.5);
    }

    /* -25 dB: LEARNING within three windows, then the new floor. */
    for (int i = 0; i < 2; i++) {
        r.ever_learning[i] = 0;
    }
    runner_mark(&r);
    s.noise_var = 1e-1 / 316.0;
    runner_run(&r, &s, 0.12);
    for (int i = 0; i < 2; i++) {
        assert(r.ever_learning[i] || status_of(&r.t[i]).state == DSD_SQUELCH_FLOOR_LEARNING);
    }
    runner_run(&r, &s, 1.0);
    for (int i = 0; i < 2; i++) {
        const dsd_squelch_floor_status st = status_of(&r.t[i]);
        assert(r.opens[i] == 0U && st.state == DSD_SQUELCH_FLOOR_KNOWN);
        assert(fabs(db(st.floor_power / source_noise_out(&s))) <= 0.5);
    }
}

/* A noise rise that reads within 3 dB of the opening level holds the gate shut; a carrier releases it at the end of
   its first window, which opens the gate there and then. */
static void
test_noise_hold_released_by_carrier(void) {
    static channel ch;
    static source s;
    static runner r;
    const int margin = 10;
    const int rate = 24000;
    channel_make(&ch, rate, 12500, NULL, 0);
    source_init(&s, &ch, 0x401DULL, 1e-3);
    runner_init(&r, &ch, &margin, 1);
    runner_run(&r, &s, 1.0);
    /* +9 dB: 7.9 x the floor, short of the 10 x opening level and past half of it. */
    runner_mark(&r);
    s.noise_var = 1e-3 * pow(10.0, 0.9);
    runner_run(&r, &s, 0.48);
    assert(r.opens[0] == 0U);
    assert(r.t[0].noise_hold);
    /* On a window boundary: the first sub-window would open the gate but for the hold. */
    runner_mark(&r);
    source_fm(&s, 20.0, 0.0, 1000.0, 2500.0);
    runner_run(&r, &s, 0.5);
    assert(r.first_open[0] == window_len(rate));
    assert(r.opens[0] == r.samples - r.mark - (uint64_t)window_len(rate));
    assert(!r.t[0].noise_hold);
}

/* At the lowest margin noise never holds the gate: a carrier starting on a window boundary opens it at the first
   sub-window, every time. */
static void
test_low_margin_opens_promptly(void) {
    static channel ch;
    static source s;
    static runner r;
    const int margin = 3;
    const int rate = 24000;
    channel_make(&ch, rate, 12500, NULL, 0);
    source_init(&s, &ch, 0x10F7ULL, 1e-3);
    runner_init(&r, &ch, &margin, 1);
    runner_run(&r, &s, 1.0);
    assert(status_of(&r.t[0]).state == DSD_SQUELCH_FLOOR_KNOWN);
    for (int k = 0; k < 12; k++) {
        runner_mark(&r);
        source_fm(&s, 10.0, 0.0, 1000.0, 2500.0);
        runner_run(&r, &s, 0.2);
        if (r.first_open[0] != sub_len(rate)) {
            DSD_FPRINTF(stderr, "onset %d at +3 dB: opened at %lld\n", k, (long long)r.first_open[0]);
            assert(0);
        }
        source_noise_only(&s);
        runner_run(&r, &s, 0.32);
    }
}

/* ----------------------------------------------------------------------------------------------------- seeding */

static void
test_seeding(void) {
    static channel ch;
    static source s;
    static runner r;
    const int margin = 10;
    const int rate = 12000;
    channel_make(&ch, rate, 8000, NULL, 0);

    /* The right floor: gates from the first sub-window and becomes KNOWN after three NOISE windows. */
    source_init(&s, &ch, 0x5EED01ULL, 1e-3);
    runner_init(&r, &ch, &margin, 1);
    dsd_squelch_floor_seed(&r.t[0], source_noise_out(&s), 0);
    assert(status_of(&r.t[0]).state == DSD_SQUELCH_FLOOR_SEEDED);
    assert(near_rel(status_of(&r.t[0]).floor_power, source_noise_out(&s), 1e-12));
    runner_run(&r, &s, 0.5);
    assert(r.opens[0] == 0U && r.known_at[0] >= 3U && r.known_at[0] <= 5U);

    /* A carrier from the first sample opens at the first sub-window, before a learning tracker could. */
    source_init(&s, &ch, 0x5EED02ULL, 1e-3);
    runner_init(&r, &ch, &margin, 1);
    dsd_squelch_floor_seed(&r.t[0], source_noise_out(&s), 1);
    assert(status_of(&r.t[0]).state == DSD_SQUELCH_FLOOR_PROVISIONAL);
    source_fm(&s, 20.0, 0.0, 1000.0, 2500.0);
    runner_run(&r, &s, 0.2);
    assert(r.first_open[0] == rate / 50 && r.opens[0] == r.samples - (uint64_t)(rate / 50));

    /* A floor 10 dB off either way starts LEARNING at the first window, then learns the right one. */
    static const double offsets_db[2] = {-10.0, 10.0};
    for (int k = 0; k < 2; k++) {
        source_init(&s, &ch, 0x5EED03ULL + (uint64_t)k, 1e-3);
        runner_init(&r, &ch, &margin, 1);
        dsd_squelch_floor_seed(&r.t[0], source_noise_out(&s) * pow(10.0, offsets_db[k] / 10.0), 1);
        const int window = rate / 25;
        runner_run(&r, &s, (double)(window + 1) / (double)rate + 0.001);
        assert(status_of(&r.t[0]).state == DSD_SQUELCH_FLOOR_LEARNING);
        runner_run(&r, &s, 0.5);
        const dsd_squelch_floor_status st = status_of(&r.t[0]);
        assert(st.state == DSD_SQUELCH_FLOOR_KNOWN && fabs(db(st.floor_power / source_noise_out(&s))) <= 0.5);
        assert(r.opens[0] <= (uint64_t)sub_len(rate));
    }

    /* 3 dB off: confirmed, then tracked to the right floor. */
    source_init(&s, &ch, 0x5EED05ULL, 1e-3);
    runner_init(&r, &ch, &margin, 1);
    dsd_squelch_floor_seed(&r.t[0], source_noise_out(&s) * 0.5, 0);
    runner_run(&r, &s, 5.0);
    assert(r.opens[0] == 0U && r.known_at[0] >= 3U && r.known_at[0] <= 5U && !r.ever_learning[0]);
    assert(fabs(db(status_of(&r.t[0]).floor_power / source_noise_out(&s))) <= 0.5);

    /* Nothing to seed. */
    dsd_squelch_floor_seed(&r.t[0], 0.0, 0);
    assert(status_of(&r.t[0]).state == DSD_SQUELCH_FLOOR_KNOWN);
    dsd_squelch_floor_seed(NULL, 1.0, 0);
}

/* A plan change keeps a known floor, rescaled by the noise gains; the same plan changes nothing. */
static void
test_set_plan(void) {
    static channel a;
    static channel b;
    static source s;
    static runner r;
    const int margin = 10;
    channel_make(&a, 24000, 12500, NULL, 0);
    channel_make(&b, 24000, 6000, NULL, 0);
    source_init(&s, &a, 0x91A4ULL, 1e-3);
    runner_init(&r, &a, &margin, 1);
    runner_run(&r, &s, 0.5);
    const double floor_a = status_of(&r.t[0]).floor_power;
    assert(status_of(&r.t[0]).state == DSD_SQUELCH_FLOOR_KNOWN);
    const uint64_t samples = r.t[0].samples;
    dsd_squelch_floor_set_plan(&r.t[0], &a.plan);
    assert(r.t[0].samples == samples);
    dsd_squelch_floor_set_plan(&r.t[0], &b.plan);
    assert(status_of(&r.t[0]).state == DSD_SQUELCH_FLOOR_KNOWN && r.t[0].samples == 0U);
    assert(near_rel(status_of(&r.t[0]).floor_power, floor_a * b.plan.noise_gain / a.plan.noise_gain, 1e-12));
    /* The rescaled floor fits the narrower channel's noise. */
    s.ch = &b;
    runner_mark(&r);
    runner_run(&r, &s, 2.0);
    assert(r.opens[0] == 0U && status_of(&r.t[0]).state == DSD_SQUELCH_FLOOR_KNOWN);
    assert(fabs(db(status_of(&r.t[0]).floor_power / source_noise_out(&s))) <= 0.5);
    /* No plan: the gate stays open and the floor starts over. */
    dsd_squelch_floor_plan none;
    DSD_MEMSET(&none, 0, sizeof none);
    dsd_squelch_floor_set_plan(&r.t[0], &none);
    const dsd_squelch_floor_status st = status_of(&r.t[0]);
    assert(st.state == DSD_SQUELCH_FLOOR_LEARNING && st.gate_open);
    float iq[16] = {0};
    uint8_t flags[8];
    DSD_MEMSET(flags, 0xFF, sizeof flags);
    dsd_squelch_floor_process(&r.t[0], iq, 8, flags);
    for (int i = 0; i < 8; i++) {
        assert(flags[i] == 0U);
    }
}

/* ------------------------------------------------------------------------------------------------------- cache */

static dsd_squelch_floor_key
key_at(int64_t freq_hz) {
    dsd_squelch_floor_key k;
    DSD_MEMSET(&k, 0, sizeof k);
    k.freq_hz = freq_hz;
    k.gain = 400;
    k.tuner_agc = 0;
    k.bias = 1;
    k.device = 0xABCDU;
    k.rate_hz = 48000;
    k.chain = 1;
    return k;
}

static int
cache_used(const dsd_squelch_floor_cache* c) {
    int used = 0;
    for (int i = 0; i < DSD_SQUELCH_FLOOR_CACHE_SIZE; i++) {
        used += c->entries[i].used != 0U;
    }
    return used;
}

static void
test_cache(void) {
    static dsd_squelch_floor_cache c;
    DSD_MEMSET(&c, 0, sizeof c);
    const dsd_squelch_floor_key a = key_at(162475000);
    double d = -1.0;
    assert(dsd_squelch_floor_cache_find(&c, &a, &d) == 0);
    dsd_squelch_floor_cache_store(&c, &a, 2.5e-3);
    assert(dsd_squelch_floor_cache_find(&c, &a, &d) == 1 && near_rel(d, 2.5e-3, 1e-15));
    /* Neighbours within 5 MHz, the nearest first. */
    dsd_squelch_floor_key k = key_at(162475000 + 25000);
    assert(dsd_squelch_floor_cache_find(&c, &k, &d) == 2 && near_rel(d, 2.5e-3, 1e-15));
    k = key_at(162475000 - 5000000);
    assert(dsd_squelch_floor_cache_find(&c, &k, &d) == 2);
    k = key_at(162475000 + 5000001);
    assert(dsd_squelch_floor_cache_find(&c, &k, &d) == 0);
    const dsd_squelch_floor_key a2 = key_at(163475000);
    dsd_squelch_floor_cache_store(&c, &a2, 7e-3);
    k = key_at(163375000);
    assert(dsd_squelch_floor_cache_find(&c, &k, &d) == 2 && near_rel(d, 7e-3, 1e-15));
    /* Everything but the frequency must match, even for a neighbour. */
    const dsd_squelch_floor_key base = key_at(162475000);
    for (int field = 0; field < 6; field++) {
        k = base;
        switch (field) {
            case 0: k.gain = 300; break;
            case 1: k.tuner_agc = 1; break;
            case 2: k.bias = 0; break;
            case 3: k.device = 0x1234U; break;
            case 4: k.rate_hz = 24000; break;
            default: k.chain = 2; break;
        }
        assert(dsd_squelch_floor_cache_find(&c, &k, &d) == 0 && !dsd_squelch_floor_key_equal(&k, &base));
    }
    /* Storing again replaces. */
    dsd_squelch_floor_cache_store(&c, &a, 3e-3);
    assert(dsd_squelch_floor_cache_find(&c, &a, &d) == 1 && near_rel(d, 3e-3, 1e-15) && cache_used(&c) == 2);
    dsd_squelch_floor_cache_store(&c, &a, 0.0);
    dsd_squelch_floor_cache_store(&c, &a, -1.0);
    assert(dsd_squelch_floor_cache_find(&c, &a, &d) == 1 && near_rel(d, 3e-3, 1e-15));
    /* Stale after 30 minutes of sample time. */
    dsd_squelch_floor_cache_advance(&c, 1800.0);
    assert(dsd_squelch_floor_cache_find(&c, &a, NULL) == 1);
    dsd_squelch_floor_cache_advance(&c, 0.5);
    assert(dsd_squelch_floor_cache_find(&c, &a, NULL) == 0 && dsd_squelch_floor_cache_find(&c, &a2, NULL) == 0);
    dsd_squelch_floor_cache_advance(&c, -5.0);
    dsd_squelch_floor_cache_store(&c, &a, 4e-3);
    assert(dsd_squelch_floor_cache_find(&c, &a, &d) == 1 && near_rel(d, 4e-3, 1e-15));
    /* Least recently used out. */
    DSD_MEMSET(&c, 0, sizeof c);
    for (int i = 0; i < DSD_SQUELCH_FLOOR_CACHE_SIZE; i++) {
        k = key_at(100000000 + ((int64_t)i * 10000000));
        dsd_squelch_floor_cache_store(&c, &k, 1e-3 * (i + 1));
    }
    k = key_at(100000000);
    assert(dsd_squelch_floor_cache_find(&c, &k, NULL) == 1);
    const dsd_squelch_floor_key extra = key_at(100000000 + ((int64_t)DSD_SQUELCH_FLOOR_CACHE_SIZE * 10000000));
    dsd_squelch_floor_cache_store(&c, &extra, 1.0);
    assert(dsd_squelch_floor_cache_find(&c, &k, NULL) == 1);
    k = key_at(110000000);
    assert(dsd_squelch_floor_cache_find(&c, &k, NULL) == 0);
    assert(dsd_squelch_floor_cache_find(&c, &extra, NULL) == 1 && cache_used(&c) == DSD_SQUELCH_FLOOR_CACHE_SIZE);
    /* NULLs. */
    dsd_squelch_floor_cache_store(NULL, &a, 1.0);
    dsd_squelch_floor_cache_store(&c, NULL, 1.0);
    assert(dsd_squelch_floor_cache_find(NULL, &a, &d) == 0 && dsd_squelch_floor_cache_find(&c, NULL, &d) == 0);
    assert(!dsd_squelch_floor_key_equal(NULL, &a) && !dsd_squelch_floor_key_equal(&a, NULL));
    dsd_squelch_floor_cache_advance(NULL, 1.0);
}

/* Moving between channels: a known floor is remembered and comes back on a revisit at the same gain; a neighbour
   seeds provisionally; another gain learns afresh. */
static void
test_change_context(void) {
    static channel ch;
    static source s;
    static runner r;
    static dsd_squelch_floor_cache c;
    const int margin = 10;
    const int rate = 12000;
    channel_make(&ch, rate, 8000, NULL, 0);
    DSD_MEMSET(&c, 0, sizeof c);
    source_init(&s, &ch, 0xC0C0ULL, 1e-3);
    runner_init(&r, &ch, &margin, 1);
    dsd_squelch_floor* t = &r.t[0];
    const dsd_squelch_floor_key a = key_at(162475000);
    const dsd_squelch_floor_key b = key_at(172475000);
    dsd_squelch_floor_change_context(t, &c, NULL, &a);
    assert(status_of(t).state == DSD_SQUELCH_FLOOR_LEARNING);
    runner_run(&r, &s, 1.0);
    const double floor_a = status_of(t).floor_power;
    assert(status_of(t).state == DSD_SQUELCH_FLOOR_KNOWN);

    /* The same context: nothing happens. */
    const uint64_t samples = t->samples;
    dsd_squelch_floor_change_context(t, &c, &a, &a);
    assert(t->samples == samples && status_of(t).state == DSD_SQUELCH_FLOOR_KNOWN);

    /* A to B (10 MHz away, unknown): A remembered, B learned. */
    dsd_squelch_floor_change_context(t, &c, &a, &b);
    double d = 0.0;
    assert(dsd_squelch_floor_cache_find(&c, &a, &d) == 1 && near_rel(d, floor_a / ch.plan.noise_gain, 1e-12));
    assert(status_of(t).state == DSD_SQUELCH_FLOOR_LEARNING);
    s.noise_var = 4e-3;
    runner_run(&r, &s, 1.0);
    assert(status_of(t).state == DSD_SQUELCH_FLOOR_KNOWN);

    /* Back to A: seeded with A's floor, confirmed by A's noise. */
    dsd_squelch_floor_change_context(t, &c, &b, &a);
    assert(dsd_squelch_floor_cache_find(&c, &b, NULL) == 1);
    assert(status_of(t).state == DSD_SQUELCH_FLOOR_SEEDED && near_rel(status_of(t).floor_power, floor_a, 1e-12));
    s.noise_var = 1e-3;
    runner_mark(&r);
    runner_run(&r, &s, 0.3);
    assert(r.opens[0] == 0U && status_of(t).state == DSD_SQUELCH_FLOOR_KNOWN);

    /* A neighbour of A: provisional. A seeded floor is not remembered on the way out. */
    const dsd_squelch_floor_key near_a = key_at(164475000);
    dsd_squelch_floor_change_context(t, &c, &a, &near_a);
    assert(status_of(t).state == DSD_SQUELCH_FLOOR_PROVISIONAL);
    assert(dsd_squelch_floor_cache_find(&c, &a, &d) == 1);
    const double remembered = d;
    dsd_squelch_floor_seed(t, floor_a * 3.0, 0);
    dsd_squelch_floor_change_context(t, &c, &near_a, &a);
    assert(dsd_squelch_floor_cache_find(&c, &near_a, NULL) == 2);
    assert(dsd_squelch_floor_cache_find(&c, &a, &d) == 1 && near_rel(d, remembered, 1e-15));

    /* Another gain on A: nothing remembered for it. */
    dsd_squelch_floor_key a_low = a;
    a_low.gain = 200;
    dsd_squelch_floor_change_context(t, &c, &a, &a_low);
    assert(status_of(t).state == DSD_SQUELCH_FLOOR_LEARNING);
    /* No context at all. */
    dsd_squelch_floor_change_context(t, &c, &a_low, NULL);
    assert(status_of(t).state == DSD_SQUELCH_FLOOR_LEARNING);
    dsd_squelch_floor_change_context(t, NULL, NULL, &a);
    assert(status_of(t).state == DSD_SQUELCH_FLOOR_LEARNING);
    dsd_squelch_floor_change_context(NULL, &c, &a, &b);
}

/* ------------------------------------------------------------------------------------------- block partitions */

static int
same_double(double a, double b) {
    uint64_t ua = 0U;
    uint64_t ub = 0U;
    DSD_MEMCPY(&ua, &a, sizeof ua);
    DSD_MEMCPY(&ub, &b, sizeof ub);
    return ua == ub;
}

static int
same_tracker(const dsd_squelch_floor* a, const dsd_squelch_floor* b) {
    return a->state == b->state && a->gate_open == b->gate_open && a->noise_hold == b->noise_hold
           && a->windows == b->windows && a->samples == b->samples && a->agree == b->agree
           && a->shift_windows == b->shift_windows && a->learn_count == b->learn_count
           && same_double(a->floor_power, b->floor_power) && same_double(a->window_power, b->window_power)
           && same_double(a->w_sa, b->w_sa) && same_double(a->w_sa2, b->w_sa2) && same_double(a->w_su_re, b->w_su_re)
           && same_double(a->w_su_im, b->w_su_im) && same_double(a->s_sa, b->s_sa);
}

/* The same samples cut into blocks any way give the same flags and the same tracker, bit for bit. */
static void
test_block_partitions(void) {
    static channel ch;
    static source s;

    enum { RATE = 24000, TOTAL = RATE * 4 };

    static float iq[2 * TOTAL];
    static uint8_t ref_flags[TOTAL];
    static uint8_t flags[TOTAL];
    channel_make(&ch, RATE, 12500, NULL, 0);
    source_init(&s, &ch, 0xB10C5ULL, 1e-3);
    int at = 0;
    const int seg = RATE / 2;
    source_run(&s, iq, seg);
    at += seg;
    source_fm(&s, 20.0, 0.0, 1000.0, 2500.0);
    source_run(&s, iq + ((size_t)at * 2U), seg);
    at += seg;
    source_noise_only(&s);
    source_run(&s, iq + ((size_t)at * 2U), seg);
    at += seg;
    source_am(&s, 12.0, 700.0, 0.9);
    source_run(&s, iq + ((size_t)at * 2U), seg);
    at += seg;
    source_noise_only(&s);
    s.noise_var = 2e-2;
    source_run(&s, iq + ((size_t)at * 2U), TOTAL - at);

    static dsd_squelch_floor ref;
    DSD_MEMSET(&ref, 0, sizeof ref);
    dsd_squelch_floor_set_margin(&ref, 6);
    dsd_squelch_floor_set_plan(&ref, &ch.plan);
    dsd_squelch_floor_process(&ref, iq, TOTAL, ref_flags);
    int transitions = 0;
    for (int i = 1; i < TOTAL; i++) {
        transitions += ref_flags[i] != ref_flags[i - 1];
    }
    assert(transitions >= 4);

    /* Reset is a fresh start, the lag history included: the same samples give the same tracker and flags. */
    static uint8_t again[TOTAL];
    static dsd_squelch_floor fresh;
    DSD_MEMSET(&fresh, 0, sizeof fresh);
    dsd_squelch_floor_set_margin(&fresh, 6);
    dsd_squelch_floor_set_plan(&fresh, &ch.plan);
    dsd_squelch_floor_reset(&ref);
    dsd_squelch_floor_process(&fresh, iq, 100, NULL);
    dsd_squelch_floor_process(&ref, iq, 100, again);
    assert(same_tracker(&fresh, &ref));
    dsd_squelch_floor_process(&ref, iq + 200, TOTAL - 100, again + 100);
    assert(memcmp(again, ref_flags, sizeof again) == 0);

    for (int mode = 0; mode < 3; mode++) {
        static dsd_squelch_floor t;
        DSD_MEMSET(&t, 0, sizeof t);
        dsd_squelch_floor_set_margin(&t, 6);
        dsd_squelch_floor_set_plan(&t, &ch.plan);
        DSD_MEMSET(flags, 0xEE, sizeof flags);
        synth_rng cuts;
        synth_rng_seed(&cuts, 0xC075ULL + (uint64_t)mode);
        int pos = 0;
        while (pos < TOTAL) {
            int n = 1;
            if (mode == 1) {
                n = 1 + (int)(synth_rng_next(&cuts) % 5000U);
            } else if (mode == 2) {
                /* Cut one sample either side of every window boundary. */
                const int next = (int)((((uint64_t)(pos / (RATE / 25)) + 1U) * RATE) / 25U);
                n = (next - pos > 2) ? next - pos - 1 : 2;
            }
            if (n > TOTAL - pos) {
                n = TOTAL - pos;
            }
            dsd_squelch_floor_process(&t, iq + ((size_t)pos * 2U), n, flags + pos);
            pos += n;
        }
        assert(memcmp(flags, ref_flags, sizeof flags) == 0);
        assert(same_tracker(&t, &ref));
    }
}

/* ------------------------------------------------------------------------------------------------ odd corners */

static void
test_corners(void) {
    /* A zeroed tracker has no plan: open, and every flag open. */
    static dsd_squelch_floor t;
    DSD_MEMSET(&t, 0, sizeof t);
    dsd_squelch_floor_status st = status_of(&t);
    assert(st.gate_open && st.state == DSD_SQUELCH_FLOOR_LEARNING && st.windows == 0U);
    float iq[64];
    uint8_t flags[32];
    for (int i = 0; i < 64; i++) {
        iq[i] = (float)(i % 7) * 0.01f;
    }
    DSD_MEMSET(flags, 0xFF, sizeof flags);
    dsd_squelch_floor_process(&t, iq, 32, flags);
    for (int i = 0; i < 32; i++) {
        assert(flags[i] == 0U);
    }
    dsd_squelch_floor_process(&t, iq, 32, NULL);
    dsd_squelch_floor_process(&t, NULL, 32, flags);
    dsd_squelch_floor_process(&t, iq, 0, flags);
    dsd_squelch_floor_process(NULL, iq, 32, flags);
    dsd_squelch_floor_reset(NULL);
    dsd_squelch_floor_set_plan(NULL, NULL);
    dsd_squelch_floor_set_margin(NULL, 6);
    dsd_squelch_floor_get_status(&t, NULL);

    /* Margins clamp to 3..30 dB. */
    dsd_squelch_floor_set_margin(&t, 1);
    assert(t.margin_db == 3 && near_rel(t.margin_lin, pow(10.0, 0.3), 1e-12));
    dsd_squelch_floor_set_margin(&t, 99);
    assert(t.margin_db == 30 && near_rel(t.margin_lin, 1000.0, 1e-12));

    /* A known floor survives exact zeros (nothing to classify), with the gate shut; reset starts over. */
    static channel ch;
    static source s;
    static runner r;
    const int margin = 6;
    channel_make(&ch, 12000, 8000, NULL, 0);
    source_init(&s, &ch, 0x2E205ULL, 1e-3);
    runner_init(&r, &ch, &margin, 1);
    runner_run(&r, &s, 0.5);
    const double floor0 = status_of(&r.t[0]).floor_power;
    static float zeros[2 * 12000];
    static uint8_t zflags[12000];
    dsd_squelch_floor_process(&r.t[0], zeros, 12000, zflags);
    st = status_of(&r.t[0]);
    assert(st.state == DSD_SQUELCH_FLOOR_KNOWN && same_double(st.floor_power, floor0) && !st.gate_open);
    assert(st.last_class == DSD_SQUELCH_WINDOW_UNDECIDED);
    dsd_squelch_floor_reset(&r.t[0]);
    st = status_of(&r.t[0]);
    assert(st.state == DSD_SQUELCH_FLOOR_LEARNING && !st.gate_open && st.windows == 0U);
    assert(near_rel(st.floor_power, 0.0, 0.0) && r.t[0].margin_db == 6 && r.t[0].plan.valid);
    /* Learning on zeros learns nothing and stays shut. */
    dsd_squelch_floor_process(&r.t[0], zeros, 12000, zflags);
    st = status_of(&r.t[0]);
    assert(st.state == DSD_SQUELCH_FLOOR_LEARNING && !st.gate_open);
    for (int i = 0; i < 12000; i++) {
        assert(zflags[i] == DSD_SQUELCH_FLAG_CLOSED);
    }
}

int
main(void) {
    test_plan_constants();
    test_classify();
    test_noise_never_opens();
    test_carriers_open_and_close();
    test_modulation_keeps_floor();
    test_landing_mid_carrier();
    test_level_steps();
    test_noise_hold_released_by_carrier();
    test_low_margin_opens_promptly();
    test_seeding();
    test_set_plan();
    test_cache();
    test_change_context();
    test_block_partitions();
    test_corners();
    printf("DSP_SQUELCH_FLOOR: OK\n");
    return 0;
}
