// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The PCM noise squelch (issue #628), in sample time on seeded audio the way an SDR program writes its FM output:
 * complex noise and a carrier through a FIR channel filter, a discriminator, optional de-emphasis and audio low-pass,
 * a volume and int16 rounding (tools/pcm_noise_squelch_model.py's sources). The band rule, the band-passes' response,
 * learning with the gate closed, noise that never opens it, carriers that open it and confirm the reference, quieting
 * that follows the CNR and not the level, gain steps, starting on a carrier, a source that low-passed its audio (no
 * band), rates with no room, digital silence, the per-passband cache, stale quieting, the threshold, and flags that do
 * not depend on how the samples are cut into blocks. --sweep runs strong modulation that must never close the gate.
 */

#include <assert.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/pcm_noise_squelch.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analog_tone_synth.h"

enum {
    RATE = 48000,
    CH_TAPS = 127,
    LPF_TAPS = 255,
    BLOCK = 960, /* 20 ms */
    MAX_BLOCKS = 2000,
};

/* The same double, bit for bit. */
static int
same_double(double a, double b) {
    uint64_t ua = 0U;
    uint64_t ub = 0U;
    DSD_MEMCPY(&ua, &a, sizeof ua);
    DSD_MEMCPY(&ub, &b, sizeof ub);
    return ua == ub;
}

/* --------------------------------------------------------------------------------------------- the source */

typedef enum { SIG_NOISE, SIG_DEAD, SIG_TONE, SIG_ZERO } sig_kind;

/* An SDR program's FM output: noise plus a carrier through a Hamming-windowed sinc channel filter, the phase step per
   sample, optional de-emphasis and audio low-pass, a gain and int16 rounding. */
typedef struct {
    synth_rng rng;
    double width_hz;
    double dev_hz;
    double taps[CH_TAPS];
    double sum_h2;
    double ring_re[CH_TAPS];
    double ring_im[CH_TAPS];
    int pos;
    double prev_re;
    double prev_im;
    double pole; /* de-emphasis, 0: none */
    double deemph;
    double lpf[LPF_TAPS];
    int lpf_on;
    double lpf_ring[LPF_TAPS];
    int lpf_pos;
    double carrier_phase;
    double tone_phase;
    double offset_hz; /* the carrier's offset from the channel's centre */
    double scale;     /* noise alone at -20 dBFS RMS */
} source;

static void
sinc_taps(double* h, int n, double cutoff_hz) {
    const double fc = cutoff_hz / (double)RATE;
    const double mid = 0.5 * (double)(n - 1);
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        const double m = (double)i - mid;
        const double s = fabs(m) < 1e-9 ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
        h[i] = s * (0.54 - (0.46 * cos(2.0 * M_PI * (double)i / (double)(n - 1))));
        sum += h[i];
    }
    for (int i = 0; i < n; i++) {
        h[i] /= sum;
    }
}

static double source_raw(source* s, sig_kind kind, double cnr_db, double tone_hz);

static void
source_init(source* s, uint64_t seed, double width_hz, double deemph_us, double lpf_hz) {
    DSD_MEMSET(s, 0, sizeof(*s));
    synth_rng_seed(&s->rng, seed);
    s->width_hz = width_hz;
    {
        const double a = (width_hz / 2.0) - 3000.0;
        const double b = width_hz / 8.0;
        const double dev = a > b ? a : b;
        s->dev_hz = dev < 5000.0 ? dev : 5000.0;
    }
    sinc_taps(s->taps, CH_TAPS, width_hz / 2.0);
    for (int i = 0; i < CH_TAPS; i++) {
        s->sum_h2 += s->taps[i] * s->taps[i];
    }
    s->prev_re = 1.0;
    s->pole = deemph_us > 0.0 ? exp(-1.0 / ((double)RATE * deemph_us * 1e-6)) : 0.0;
    if (lpf_hz > 0.0) {
        sinc_taps(s->lpf, LPF_TAPS, lpf_hz);
        s->lpf_on = 1;
    }
    /* Calibrate on a copy: noise alone at -20 dBFS RMS. */
    source probe = *s;
    double e = 0.0;
    const int n = RATE / 2;
    const int settle = n / 4;
    const int measured = n - settle;
    for (int i = 0; i < n; i++) {
        const double x = source_raw(&probe, SIG_NOISE, 0.0, 0.0);
        if (i >= settle) {
            e += x * x;
        }
    }
    s->scale = pow(10.0, -20.0 / 20.0) * 32768.0 / sqrt(e / (double)measured);
}

/* The discriminator output (after de-emphasis and the audio low-pass), unscaled. */
static double
source_raw(source* s, sig_kind kind, double cnr_db, double tone_hz) {
    double re = synth_gauss(&s->rng) * 0.70710678118654752;
    double im = synth_gauss(&s->rng) * 0.70710678118654752;
    if (kind == SIG_DEAD || kind == SIG_TONE) {
        const double amp = sqrt(s->sum_h2 * pow(10.0, cnr_db / 10.0));
        if (kind == SIG_TONE) {
            s->tone_phase += 2.0 * M_PI * tone_hz / (double)RATE;
            s->carrier_phase += 2.0 * M_PI * s->dev_hz * sin(s->tone_phase) / (double)RATE;
        }
        s->carrier_phase += 2.0 * M_PI * s->offset_hz / (double)RATE;
        re += amp * cos(s->carrier_phase);
        im += amp * sin(s->carrier_phase);
    }
    s->ring_re[s->pos] = re;
    s->ring_im[s->pos] = im;
    s->pos = (s->pos + 1) % CH_TAPS;
    double zr = 0.0;
    double zi = 0.0;
    for (int j = 0; j < CH_TAPS; j++) {
        int at = s->pos - 1 - j;
        if (at < 0) {
            at += CH_TAPS;
        }
        zr += s->ring_re[at] * s->taps[j];
        zi += s->ring_im[at] * s->taps[j];
    }
    const double d = atan2((zi * s->prev_re) - (zr * s->prev_im), (zr * s->prev_re) + (zi * s->prev_im));
    s->prev_re = zr;
    s->prev_im = zi;
    double y = d;
    if (s->pole > 0.0) {
        s->deemph = ((1.0 - s->pole) * y) + (s->pole * s->deemph);
        y = s->deemph;
    }
    if (s->lpf_on) {
        s->lpf_ring[s->lpf_pos] = y;
        s->lpf_pos = (s->lpf_pos + 1) % LPF_TAPS;
        double acc = 0.0;
        for (int j = 0; j < LPF_TAPS; j++) {
            int at = s->lpf_pos - 1 - j;
            if (at < 0) {
                at += LPF_TAPS;
            }
            acc += s->lpf_ring[at] * s->lpf[j];
        }
        y = acc;
    }
    return y;
}

/* One int16-scale sample: kind at cnr_db, gain_db over the calibrated volume. */
static float
source_next(source* s, sig_kind kind, double cnr_db, double tone_hz, double gain_db) {
    const double raw = source_raw(s, kind == SIG_ZERO ? SIG_NOISE : kind, cnr_db, tone_hz);
    if (kind == SIG_ZERO) {
        return 0.0f;
    }
    double v = floor((raw * s->scale * pow(10.0, gain_db / 20.0)) + 0.5);
    if (v > 32767.0) {
        v = 32767.0;
    } else if (v < -32768.0) {
        v = -32768.0;
    }
    return (float)v;
}

/* --------------------------------------------------------------------------------------------- runs */

typedef struct {
    sig_kind kind;
    double seconds;
    double cnr_db;
    double tone_hz;
    double gain_db;
} seg;

/* What the squelch published after each 20 ms block, and how many of the block's flags were open. */
typedef struct {
    int blocks;
    int seg_of[MAX_BLOCKS];
    dsd_pcm_noise_squelch_status st[MAX_BLOCKS];
    int open_flags[MAX_BLOCKS];
} trace;

static void
run_segments(dsd_pcm_noise_squelch* sq, source* src, const seg* segs, int nsegs, trace* tr) {
    static float pcm[BLOCK];
    static uint8_t flags[BLOCK];
    for (int k = 0; k < nsegs; k++) {
        const int blocks = (int)lround(segs[k].seconds * (double)RATE / (double)BLOCK);
        for (int b = 0; b < blocks; b++) {
            for (int i = 0; i < BLOCK; i++) {
                pcm[i] = source_next(src, segs[k].kind, segs[k].cnr_db, segs[k].tone_hz, segs[k].gain_db);
            }
            dsd_pcm_noise_squelch_process(sq, pcm, BLOCK, flags);
            if (tr && tr->blocks < MAX_BLOCKS) {
                int open = 0;
                for (int i = 0; i < BLOCK; i++) {
                    open += (flags[i] & (uint8_t)DSD_SQUELCH_FLAG_CLOSED) ? 0 : 1;
                }
                tr->seg_of[tr->blocks] = k;
                dsd_pcm_noise_squelch_get_status(sq, &tr->st[tr->blocks]);
                tr->open_flags[tr->blocks] = open;
                tr->blocks++;
            }
        }
    }
}

static dsd_pcm_noise_squelch*
new_squelch(int native_rate, int threshold_db) {
    dsd_pcm_noise_squelch* sq = (dsd_pcm_noise_squelch*)calloc(1, sizeof(*sq));
    assert(sq);
    dsd_pcm_noise_squelch_plan plan;
    (void)dsd_pcm_noise_squelch_plan_design(&plan, RATE, native_rate);
    dsd_pcm_noise_squelch_set_plan(sq, &plan);
    dsd_pcm_noise_squelch_set_threshold(sq, threshold_db);
    dsd_pcm_noise_squelch_key key = {1U, native_rate, 1, 0};
    dsd_pcm_noise_squelch_set_key(sq, &key);
    return sq;
}

/* Blocks of segment k, from @p skip_s seconds into it. */
static int
seg_blocks(const trace* tr, int k, double skip_s, int* first) {
    int n = 0;
    int start = -1;
    const int skip = (int)lround(skip_s * (double)RATE / (double)BLOCK);
    int seen = 0;
    for (int b = 0; b < tr->blocks; b++) {
        if (tr->seg_of[b] != k) {
            continue;
        }
        if (seen++ < skip) {
            continue;
        }
        if (start < 0) {
            start = b;
        }
        n++;
    }
    *first = start;
    return n;
}

/* Seconds the gate let samples through in segment k past skip_s. */
static double
open_seconds(const trace* tr, int k, double skip_s) {
    int first = 0;
    const int n = seg_blocks(tr, k, skip_s, &first);
    double open = 0.0;
    for (int b = first; b >= 0 && b < first + n; b++) {
        open += (double)tr->open_flags[b] / (double)RATE;
    }
    return open;
}

static double
median_q(const trace* tr, int k, double skip_s) {
    int first = 0;
    const int n = seg_blocks(tr, k, skip_s, &first);
    static double v[MAX_BLOCKS];
    int m = 0;
    for (int b = first; b >= 0 && b < first + n; b++) {
        if (tr->st[b].quieting_valid) {
            v[m++] = tr->st[b].quieting_db;
        }
    }
    assert(m > 0);
    for (int i = 1; i < m; i++) {
        const double x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    return (m % 2) ? v[m / 2] : 0.5 * (v[(m / 2) - 1] + v[m / 2]);
}

/* --------------------------------------------------------------------------------------------- tests */

static void
test_band_rule(void) {
    dsd_pcm_noise_squelch_plan p;
    assert(dsd_pcm_noise_squelch_plan_design(&p, 48000, 48000) == 0);
    assert(p.valid && p.rate_hz == 48000 && p.native_rate_hz == 48000);
    assert(fabs(p.lo_hz - 3800.0) < 1e-9 && fabs(p.hi_hz - 6500.0) < 1e-9);
    assert(p.sub_bands == 13 && p.bands == 13 + (2 * 12));
    assert(fabs(p.step_hz - (2700.0 / 13.0)) < 1e-9);
    /* 16 kHz staged to 48 kHz: the native rate bounds the band (0.45 x 16 kHz = 7.2 kHz, over the 6.5 kHz top). */
    assert(dsd_pcm_noise_squelch_plan_design(&p, 48000, 16000) == 0 && fabs(p.hi_hz - 6500.0) < 1e-9);
    /* 12 kHz: 5.4 kHz. */
    assert(dsd_pcm_noise_squelch_plan_design(&p, 48000, 12000) == 0 && fabs(p.hi_hz - 5400.0) < 1e-9);
    assert(p.sub_bands == 8);
    /* 44.1 kHz runs at its own rate. */
    assert(dsd_pcm_noise_squelch_plan_design(&p, 44100, 44100) == 0 && p.rate_hz == 44100);
    /* No room: 9.6 kHz (4.32 kHz) and 8 kHz (3.6 kHz). */
    assert(dsd_pcm_noise_squelch_plan_design(&p, 48000, 9600) == -1 && !p.valid);
    assert(dsd_pcm_noise_squelch_plan_design(&p, 48000, 8000) == -1 && !p.valid);
    assert(dsd_pcm_noise_squelch_plan_design(&p, 0, 48000) == -1);
    assert(dsd_pcm_noise_squelch_plan_design(NULL, 48000, 48000) == -1);
}

/* |H(f)| of a band-pass's biquads at f. */
static double
response_db(const dsd_noise_squelch_biquad* sec, double f, double fs) {
    const double w = 2.0 * M_PI * f / fs;
    double mag = 1.0;
    for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
        /* b = gain [1, 0, -1], a = [1, a1, a2] */
        const double nr = sec[s].gain * (1.0 - cos(2.0 * w));
        const double ni = sec[s].gain * sin(2.0 * w);
        const double dr = 1.0 + (sec[s].a1 * cos(w)) + (sec[s].a2 * cos(2.0 * w));
        const double di = -(sec[s].a1 * sin(w)) - (sec[s].a2 * sin(2.0 * w));
        mag *= sqrt(((nr * nr) + (ni * ni)) / ((dr * dr) + (di * di)));
    }
    return 20.0 * log10(mag);
}

static void
test_band_pass_response(void) {
    dsd_pcm_noise_squelch_plan p;
    assert(dsd_pcm_noise_squelch_plan_design(&p, 48000, 48000) == 0);
    const double step = p.step_hz;
    for (int k = 0; k < p.bands; k++) {
        const int set = k < p.sub_bands ? 0 : 1 + ((k - p.sub_bands) / (p.sub_bands - 1));
        const int idx = k < p.sub_bands ? k : (k - p.sub_bands) % (p.sub_bands - 1);
        const double f1 = p.lo_hz + (step * ((double)idx + ((double)set / (double)DSD_PCM_NOISE_SQUELCH_SETS)));
        const double f2 = f1 + step;
        /* Unit gain at the (geometric, pre-warped) centre and the Butterworth -3 dB at the edges. */
        assert(fabs(response_db(p.section[k], f1, 48000.0) + 3.0103) < 0.05);
        assert(fabs(response_db(p.section[k], f2, 48000.0) + 3.0103) < 0.05);
        assert(response_db(p.section[k], f1 - step, 48000.0) < -20.0);
    }
    assert(fabs(response_db(p.voice, 400.0, 48000.0) + 3.0103) < 0.05);
    assert(fabs(response_db(p.voice, 2600.0, 48000.0) + 3.0103) < 0.05);
    assert(response_db(p.voice, 3800.0, 48000.0) < -15.0);
}

/* Noise alone: the gate stays closed while learning, a reference comes within 200 ms, and at N = 3 noise never
   reaches the threshold in 20 s. */
static void
test_noise_learns_and_never_opens(void) {
    dsd_pcm_noise_squelch* sq = new_squelch(48000, 3);
    static source src;
    static trace tr;
    DSD_MEMSET(&tr, 0, sizeof tr);
    source_init(&src, 11U, 12500.0, 0.0, 0.0);
    const seg s[] = {{SIG_NOISE, 20.0, 0.0, 0.0, 0.0}};
    run_segments(sq, &src, s, 1, &tr);
    int learned_block = -1;
    double q_max = -1e9;
    double open = 0.0;
    for (int b = 0; b < tr.blocks; b++) {
        if (learned_block < 0 && tr.st[b].state != DSD_PCM_NOISE_SQUELCH_LEARNING) {
            learned_block = b;
        }
        if (tr.st[b].quieting_valid && tr.st[b].quieting_db > q_max) {
            q_max = tr.st[b].quieting_db;
        }
        open += (double)tr.open_flags[b];
    }
    assert(learned_block >= 0 && (double)(learned_block + 1) * 0.02 <= 0.2 + 1e-9);
    assert(tr.st[tr.blocks - 1].state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL);
    assert(tr.st[tr.blocks - 1].available == 1);
    assert(q_max < 3.0);
    assert(open < 0.5);
    free(sq);
}

/* A carrier opens the gate within a window or so of keying, confirms the reference, and the gate closes when it
   drops. */
static void
test_carrier_opens_and_confirms(void) {
    dsd_pcm_noise_squelch* sq = new_squelch(48000, 10);
    static source src;
    static trace tr;
    DSD_MEMSET(&tr, 0, sizeof tr);
    source_init(&src, 12U, 12500.0, 0.0, 0.0);
    const seg s[] = {
        {SIG_NOISE, 2.0, 0.0, 0.0, 0.0},
        {SIG_TONE, 1.5, 20.0, 1000.0, 0.0},
        {SIG_NOISE, 1.5, 0.0, 0.0, 0.0},
    };
    run_segments(sq, &src, s, 3, &tr);
    assert(open_seconds(&tr, 0, 0.3) < 1e-9);
    assert(open_seconds(&tr, 1, 0.12) > 1.5 - 0.12 - 0.021);
    assert(open_seconds(&tr, 2, 0.1) < 1e-9);
    assert(tr.st[tr.blocks - 1].state == DSD_PCM_NOISE_SQUELCH_KNOWN);
    free(sq);
}

/* Quieting rises with the CNR, for a modulated and a dead carrier alike. */
static void
test_quieting_follows_cnr(void) {
    static const double cnrs[] = {0.0, 6.0, 10.0, 20.0, 30.0};
    for (int kind = 0; kind < 2; kind++) {
        dsd_pcm_noise_squelch* sq = new_squelch(48000, 10);
        static source src;
        static trace tr;
        DSD_MEMSET(&tr, 0, sizeof tr);
        source_init(&src, 13U + (uint64_t)kind, 12500.0, 0.0, 0.0);
        seg s[1 + 5];
        s[0] = (seg){SIG_NOISE, 2.0, 0.0, 0.0, 0.0};
        for (int i = 0; i < 5; i++) {
            s[1 + i] = (seg){kind ? SIG_DEAD : SIG_TONE, 0.6, cnrs[i], 1000.0, 0.0};
        }
        run_segments(sq, &src, s, 6, &tr);
        double prev = -1e9;
        for (int i = 0; i < 5; i++) {
            const double q = median_q(&tr, 1 + i, 0.12);
            assert(q > prev);
            assert(fabs(q - cnrs[i]) < 4.0);
            prev = q;
        }
        free(sq);
    }
}

/* The same air at three volumes reads the same quieting. */
static void
test_level_independent(void) {
    static const double gains[] = {-15.0, 0.0, 6.0};
    double q10[3];
    double q20[3];
    for (int g = 0; g < 3; g++) {
        dsd_pcm_noise_squelch* sq = new_squelch(48000, 10);
        static source src;
        static trace tr;
        DSD_MEMSET(&tr, 0, sizeof tr);
        source_init(&src, 14U, 12500.0, 0.0, 0.0);
        const seg s[] = {
            {SIG_NOISE, 1.0, 0.0, 0.0, gains[g]},
            {SIG_TONE, 0.6, 10.0, 1000.0, gains[g]},
            {SIG_TONE, 0.6, 20.0, 1000.0, gains[g]},
        };
        run_segments(sq, &src, s, 3, &tr);
        q10[g] = median_q(&tr, 1, 0.12);
        q20[g] = median_q(&tr, 2, 0.12);
        free(sq);
    }
    for (int g = 0; g < 3; g++) {
        assert(fabs(q10[g] - q10[1]) < 0.2);
        assert(fabs(q20[g] - q20[1]) < 0.2);
    }
}

/* A volume step down on noise is a gain step: the reference follows within a fraction of a second, so noise reads
   about 0 dB again and the gate opens on it only while the step is being confirmed. */
static void
test_volume_step_down(void) {
    for (int n = 0; n < 2; n++) {
        const int threshold = n ? 3 : 10;
        dsd_pcm_noise_squelch* sq = new_squelch(48000, threshold);
        static source src;
        static trace tr;
        DSD_MEMSET(&tr, 0, sizeof tr);
        source_init(&src, 15U, 12500.0, 0.0, 0.0);
        const seg s[] = {
            {SIG_NOISE, 2.0, 0.0, 0.0, 0.0},
            {SIG_NOISE, 3.0, 0.0, 0.0, -12.0},
            {SIG_TONE, 1.0, 20.0, 1000.0, -12.0},
        };
        run_segments(sq, &src, s, 3, &tr);
        assert(open_seconds(&tr, 1, 0.0) < 0.4);
        assert(fabs(median_q(&tr, 1, 1.0)) < 1.5);
        assert(open_seconds(&tr, 2, 0.12) > 1.0 - 0.12 - 0.021);
        free(sq);
    }
}

/* A session that starts on a carrier takes it as the reference and stays shut on it; the first noise corrects that,
   and the next carrier opens the gate. */
static void
test_starting_on_a_carrier(void) {
    dsd_pcm_noise_squelch* sq = new_squelch(48000, 10);
    static source src;
    static trace tr;
    DSD_MEMSET(&tr, 0, sizeof tr);
    source_init(&src, 16U, 12500.0, 0.0, 0.0);
    const seg s[] = {
        {SIG_DEAD, 2.0, 25.0, 0.0, 0.0},
        {SIG_NOISE, 2.0, 0.0, 0.0, 0.0},
        {SIG_TONE, 1.5, 20.0, 1000.0, 0.0},
    };
    run_segments(sq, &src, s, 3, &tr);
    assert(open_seconds(&tr, 0, 0.0) < 1e-9);
    assert(open_seconds(&tr, 1, 0.1) < 1e-9);
    /* The noise itself becomes the reference as soon as its run holds, not over the 1 s tracking. */
    int first = 0;
    (void)seg_blocks(&tr, 1, 0.3, &first);
    assert(first >= 0 && tr.st[first].quieting_valid && fabs(tr.st[first].quieting_db) < 2.0);
    assert(tr.st[first].state == DSD_PCM_NOISE_SQUELCH_KNOWN);
    assert(open_seconds(&tr, 2, 0.12) > 1.5 - 0.12 - 0.021);
    free(sq);
}

/* A source that low-passed its audio at 3 kHz has nothing above voice: no band within 2 s, the gate shut on noise
   until then, and the squelch unavailable after. */
static void
test_low_passed_source_reads_no_band(void) {
    dsd_pcm_noise_squelch* sq = new_squelch(48000, 6);
    static source src;
    static trace tr;
    DSD_MEMSET(&tr, 0, sizeof tr);
    source_init(&src, 17U, 12500.0, 75.0, 3000.0);
    const seg s[] = {
        {SIG_NOISE, 3.0, 0.0, 0.0, 0.0},
        {SIG_TONE, 1.0, 20.0, 1000.0, 0.0},
        {SIG_NOISE, 1.0, 0.0, 0.0, 0.0},
    };
    run_segments(sq, &src, s, 3, &tr);
    int no_band = -1;
    for (int b = 0; b < tr.blocks; b++) {
        if (tr.st[b].state == DSD_PCM_NOISE_SQUELCH_NO_BAND) {
            no_band = b;
            break;
        }
        assert(tr.open_flags[b] == 0);
    }
    assert(no_band >= 0 && (double)(no_band + 1) * 0.02 <= 2.0);
    const dsd_pcm_noise_squelch_status* last = &tr.st[tr.blocks - 1];
    assert(last->state == DSD_PCM_NOISE_SQUELCH_NO_BAND && last->available == 0 && last->gate_open == 1);
    assert(tr.open_flags[tr.blocks - 1] == BLOCK);
    free(sq);
}

/* 8 and 9.6 kHz sources leave no room above voice: NO_ROOM, unavailable, every flag open; so does a squelch with no
   plan at all. */
static void
test_no_room(void) {
    dsd_pcm_noise_squelch* sq = new_squelch(8000, 10);
    static float pcm[BLOCK];
    static uint8_t flags[BLOCK];
    for (int i = 0; i < BLOCK; i++) {
        pcm[i] = (float)(1000.0 * sin(0.3 * (double)i));
    }
    dsd_pcm_noise_squelch_process(sq, pcm, BLOCK, flags);
    for (int i = 0; i < BLOCK; i++) {
        assert(flags[i] == 0U);
    }
    dsd_pcm_noise_squelch_status st;
    dsd_pcm_noise_squelch_get_status(sq, &st);
    assert(st.state == DSD_PCM_NOISE_SQUELCH_NO_ROOM && st.available == 0 && st.gate_open == 1);
    free(sq);
    dsd_pcm_noise_squelch* zero = (dsd_pcm_noise_squelch*)calloc(1, sizeof(*zero));
    assert(zero);
    dsd_pcm_noise_squelch_process(zero, pcm, BLOCK, flags);
    dsd_pcm_noise_squelch_get_status(zero, &st);
    assert(st.state == DSD_PCM_NOISE_SQUELCH_NO_ROOM && st.available == 0 && flags[0] == 0U);
    free(zero);
}

/* Digital silence closes the gate and teaches nothing: the reference survives it. */
static void
test_silence_closes_and_teaches_nothing(void) {
    dsd_pcm_noise_squelch* sq = new_squelch(48000, 10);
    static source src;
    static trace tr;
    DSD_MEMSET(&tr, 0, sizeof tr);
    source_init(&src, 18U, 12500.0, 0.0, 0.0);
    const seg s[] = {
        {SIG_NOISE, 2.0, 0.0, 0.0, 0.0}, {SIG_TONE, 1.0, 20.0, 1000.0, 0.0}, {SIG_ZERO, 1.0, 0.0, 0.0, 0.0},
        {SIG_NOISE, 1.0, 0.0, 0.0, 0.0}, {SIG_TONE, 1.0, 20.0, 1000.0, 0.0},
    };
    run_segments(sq, &src, s, 5, &tr);
    assert(open_seconds(&tr, 2, 0.06) < 1e-9);
    assert(open_seconds(&tr, 3, 0.1) < 1e-9);
    assert(open_seconds(&tr, 4, 0.12) > 1.0 - 0.12 - 0.021);
    assert(fabs(median_q(&tr, 3, 0.2)) < 1.5);
    free(sq);
}

/* Each passband keeps its own reference: a scan back to a passband it learned gates at once, without learning; an
   unknown passband is never kept; a new source forgets everything. */
static void
test_passband_cache(void) {
    dsd_pcm_noise_squelch* sq = new_squelch(48000, 10);
    static source narrow;
    static source wide;
    static trace tr;
    source_init(&narrow, 19U, 12500.0, 0.0, 0.0);
    source_init(&wide, 20U, 25000.0, 0.0, 0.0);
    wide.scale = narrow.scale; /* one program, one volume: its wide passband plays its noise louder */
    dsd_pcm_noise_squelch_key key = {1U, 48000, 1, 12500};
    dsd_pcm_noise_squelch_set_key(sq, &key);
    const seg noise = {SIG_NOISE, 1.0, 0.0, 0.0, 0.0};
    const seg carrier = {SIG_TONE, 0.5, 20.0, 1000.0, 0.0};
    DSD_MEMSET(&tr, 0, sizeof tr);
    run_segments(sq, &narrow, &noise, 1, &tr);
    key.passband_hz = 25000;
    dsd_pcm_noise_squelch_set_key(sq, &key);
    dsd_pcm_noise_squelch_status st;
    dsd_pcm_noise_squelch_get_status(sq, &st);
    assert(st.state == DSD_PCM_NOISE_SQUELCH_LEARNING);
    run_segments(sq, &wide, &noise, 1, &tr);
    key.passband_hz = 12500;
    dsd_pcm_noise_squelch_set_key(sq, &key);
    dsd_pcm_noise_squelch_get_status(sq, &st);
    assert(st.state == DSD_PCM_NOISE_SQUELCH_PROVISIONAL);
    /* Back on the narrow passband its noise reads about 0 dB (not the wide one's 4 dB more), and a carrier opens the
       gate from its first windows. */
    DSD_MEMSET(&tr, 0, sizeof tr);
    const seg back[] = {noise, carrier};
    run_segments(sq, &narrow, back, 2, &tr);
    assert(open_seconds(&tr, 0, 0.0) < 1e-9);
    assert(fabs(median_q(&tr, 0, 0.1)) < 1.5);
    assert(open_seconds(&tr, 1, 0.1) > 0.5 - 0.1 - 0.021);
    /* An unknown passband is never kept. */
    key.passband_hz = DSD_PCM_NOISE_SQUELCH_PASSBAND_UNKNOWN;
    dsd_pcm_noise_squelch_set_key(sq, &key);
    run_segments(sq, &narrow, &noise, 1, NULL);
    key.passband_hz = 6000;
    dsd_pcm_noise_squelch_set_key(sq, &key);
    key.passband_hz = DSD_PCM_NOISE_SQUELCH_PASSBAND_UNKNOWN;
    dsd_pcm_noise_squelch_set_key(sq, &key);
    dsd_pcm_noise_squelch_get_status(sq, &st);
    assert(st.state == DSD_PCM_NOISE_SQUELCH_LEARNING);
    /* A new source (or volume) forgets the narrow passband too. */
    key.passband_hz = 12500;
    key.source = 2U;
    dsd_pcm_noise_squelch_set_key(sq, &key);
    dsd_pcm_noise_squelch_get_status(sq, &st);
    assert(st.state == DSD_PCM_NOISE_SQUELCH_LEARNING);
    free(sq);
}

/* The source switches its audio low-pass on mid-session: the band drops to its stopband while the voice band holds,
   which reads as quieting. Stale quieting bounds the open gate to about 5 s, and the source then reads no band. */
static void
test_stale_quieting(void) {
    dsd_pcm_noise_squelch* sq = new_squelch(48000, 10);
    static source src;
    static trace tr;
    DSD_MEMSET(&tr, 0, sizeof tr);
    source_init(&src, 21U, 12500.0, 0.0, 0.0);
    const seg before = {SIG_NOISE, 2.0, 0.0, 0.0, 0.0};
    run_segments(sq, &src, &before, 1, &tr);
    sinc_taps(src.lpf, LPF_TAPS, 3000.0);
    src.lpf_on = 1;
    const seg after = {SIG_NOISE, 9.0, 0.0, 0.0, 0.0};
    run_segments(sq, &src, &after, 1, &tr);
    double opened = 0.0;
    for (int b = 100; b < tr.blocks; b++) {
        if (tr.st[b].state != DSD_PCM_NOISE_SQUELCH_NO_BAND) {
            opened += (double)tr.open_flags[b] / (double)RATE;
        }
    }
    assert(opened < 5.3);
    assert(tr.st[tr.blocks - 1].state == DSD_PCM_NOISE_SQUELCH_NO_BAND);
    free(sq);
}

/* The threshold: clamped to 3..30, closing 3 dB lower but never under 1.5 dB. */
static void
test_threshold(void) {
    dsd_pcm_noise_squelch t;
    DSD_MEMSET(&t, 0, sizeof t);
    dsd_pcm_noise_squelch_set_threshold(&t, 1);
    assert(t.threshold_db == 3 && fabs(t.open_db - 3.0) < 1e-12 && fabs(t.close_db - 1.5) < 1e-12);
    dsd_pcm_noise_squelch_set_threshold(&t, 40);
    assert(t.threshold_db == 30 && fabs(t.close_db - 27.0) < 1e-12);
    dsd_pcm_noise_squelch_set_threshold(&t, 10);
    assert(fabs(t.open_db - 10.0) < 1e-12 && fabs(t.close_db - 7.0) < 1e-12);
}

/* The same samples cut into different blocks give the same flags and the same readings, bit for bit. */
static void
test_block_cuts(void) {
    static float pcm[6 * RATE];
    static uint8_t flags[3][6 * RATE];
    static source src;
    source_init(&src, 22U, 12500.0, 75.0, 0.0);
    const int n = 6 * RATE;
    for (int i = 0; i < n; i++) {
        const double t = (double)i / (double)RATE;
        sig_kind kind = SIG_NOISE;
        if (t >= 2.0 && t < 3.5) {
            kind = SIG_TONE;
        } else if (t >= 4.5 && t < 4.7) {
            kind = SIG_ZERO;
        }
        pcm[i] = source_next(&src, kind, 15.0, 800.0, t >= 5.0 ? -10.0 : 0.0);
    }
    static const int cuts[3] = {1, 613, 4800};
    dsd_pcm_noise_squelch_status st[3];
    for (int c = 0; c < 3; c++) {
        dsd_pcm_noise_squelch* sq = new_squelch(48000, 10);
        for (int at = 0; at < n; at += cuts[c]) {
            const int len = (n - at) < cuts[c] ? (n - at) : cuts[c];
            dsd_pcm_noise_squelch_process(sq, pcm + at, len, flags[c] + at);
        }
        dsd_pcm_noise_squelch_get_status(sq, &st[c]);
        free(sq);
    }
    for (int c = 1; c < 3; c++) {
        assert(memcmp(flags[0], flags[c], (size_t)n) == 0);
        assert(st[c].state == st[0].state && st[c].gate_open == st[0].gate_open && st[c].windows == st[0].windows);
        assert(same_double(st[c].quieting_db, st[0].quieting_db));
    }
}

/* --------------------------------------------------------------------------------------------- the sweep */

/* Strong modulation on 12.5 and 25 kHz sources: a tone sweep at the rated deviation, centred and at the offsets its
   Carson bandwidth leaves inside the channel (tools/pcm_noise_squelch_model.py's wanted modulation), never closes a
   gate opened at N = 30. */
static void
test_strong_modulation_never_closes(void) {
    static const double widths[] = {12500.0, 25000.0};
    for (int w = 0; w < 2; w++) {
        double worst = 1e9;
        double worst_f = 0.0;
        double worst_off = 0.0;
        for (int f = 300; f <= 3000; f += 25) {
            static source probe;
            source_init(&probe, 1U, widths[w], 0.0, 0.0);
            const double room = (widths[w] / 2.0) - probe.dev_hz - (double)f;
            const double offsets[3] = {0.0, room, -room};
            for (int o = 0; o < (room > 1.0 ? 3 : 1); o++) {
                dsd_pcm_noise_squelch* sq = new_squelch(48000, 30);
                static source src;
                static trace tr;
                DSD_MEMSET(&tr, 0, sizeof tr);
                source_init(&src, 100U + (uint64_t)f + (1000U * (uint64_t)o), widths[w], 0.0, 0.0);
                const seg s[] = {
                    {SIG_NOISE, 0.5, 0.0, 0.0, 0.0},
                    {SIG_TONE, 0.3, 80.0, (double)f, 0.0},
                };
                run_segments(sq, &src, s, 1, &tr);
                src.offset_hz = offsets[o];
                const int keyed = tr.blocks;
                run_segments(sq, &src, &s[1], 1, &tr);
                /* From 120 ms into the carrier: the first windows straddle the keying. */
                for (int b = keyed + 6; b < tr.blocks; b++) {
                    assert(tr.open_flags[b] == BLOCK);
                    if (tr.st[b].quieting_db < worst) {
                        worst = tr.st[b].quieting_db;
                        worst_f = (double)f;
                        worst_off = offsets[o];
                    }
                }
                free(sq);
            }
        }
        printf("  sweep %.0f Hz: lowest quieting %.1f dB (%.0f Hz at %+.0f Hz)\n", widths[w], worst, worst_f,
               worst_off);
        assert(worst >= 33.0);
    }
}

int
main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--sweep") == 0) {
        test_strong_modulation_never_closes();
        printf("DSP_PCM_NOISE_SQUELCH_SWEEP: OK\n");
        return 0;
    }
    test_band_rule();
    test_band_pass_response();
    test_threshold();
    test_no_room();
    test_noise_learns_and_never_opens();
    test_carrier_opens_and_confirms();
    test_quieting_follows_cnr();
    test_level_independent();
    test_volume_step_down();
    test_starting_on_a_carrier();
    test_low_passed_source_reads_no_band();
    test_silence_closes_and_teaches_nothing();
    test_passband_cache();
    test_stale_quieting();
    test_block_cuts();
    printf("DSP_PCM_NOISE_SQUELCH: OK\n");
    return 0;
}
