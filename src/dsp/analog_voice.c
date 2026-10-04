// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Voice band-pass and AGC for the analog monitor (issue #518). tools/design_voice_filters.py derives the elliptic
 * prototype below and prints the response figures tests/dsp/test_dsp_analog_voice.c holds the design to.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/analog_voice.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 6th-order elliptic low-pass prototype, 0.5 dB ripple, 40 dB stopband, passband edge 1 rad/s
   (tools/design_voice_filters.py --c): {wz2, b1, b0} per section, H(s) = (s^2 + wz2) / (s^2 + b1 s + b0), each scaled
   to unity at DC. */
static const double k_elliptic_proto[3][3] = {
    {1.3095142221370786, 0.77007359331317238, 0.31756455802653771},
    {1.855733055295824, 0.30575050216738592, 0.79649195194050804},
    {9.9654583660504734, 0.065031798309196864, 1.0141672538119564},
};
/* The prototype's DC gain (10^(-0.5/20) for an even order): the high-pass's passband ripples between it and 1. */
static const double k_elliptic_dc_gain = 0.94406087628592328;

static const double k_fm_hp_edge_hz = 300.0;
static const double k_am_hp_hz = 200.0;
static const double k_lp_hz = 3400.0;
/* A section whose corner is at or above this fraction of the rate is left out. */
static const double k_skip_corner_fraction = 0.45;
/* A sample whose magnitude reaches this is not a finite audio sample. */
static const double k_finite_limit = 1e30;

static void
biquad_set(dsd_voice_biquad* s, const double b[3], const double a[3]) {
    s->b0 = b[0] / a[0];
    s->b1 = b[1] / a[0];
    s->b2 = b[2] / a[0];
    s->a1 = a[1] / a[0];
    s->a2 = a[2] / a[0];
    s->s1 = 0.0;
    s->s2 = 0.0;
}

/* RBJ cookbook Butterworth section: 1 for a high-pass, 0 for a low-pass. */
static void
rbj_section(dsd_voice_biquad* s, int highpass, double corner_hz, double q, int rate_hz) {
    const double w0 = 2.0 * M_PI * corner_hz / (double)rate_hz;
    const double cw = cos(w0);
    const double alpha = sin(w0) / (2.0 * q);
    double b[3];
    if (highpass) {
        b[0] = (1.0 + cw) / 2.0;
        b[1] = -(1.0 + cw);
        b[2] = (1.0 + cw) / 2.0;
    } else {
        b[0] = (1.0 - cw) / 2.0;
        b[1] = 1.0 - cw;
        b[2] = (1.0 - cw) / 2.0;
    }
    const double a[3] = {1.0 + alpha, -2.0 * cw, 1.0 - alpha};
    biquad_set(s, b, a);
}

/* Bilinear transform s = 2 fs (1 - z^-1) / (1 + z^-1) of (A s^2 + B s + C) / (A' s^2 + B' s + C'). */
static void
bilinear_section(dsd_voice_biquad* s, const double num[3], const double den[3], int rate_hz) {
    const double k = 2.0 * (double)rate_hz;
    const double kk = k * k;
    const double b[3] = {(num[0] * kk) + (num[1] * k) + num[2], 2.0 * (num[2] - (num[0] * kk)),
                         (num[0] * kk) - (num[1] * k) + num[2]};
    const double a[3] = {(den[0] * kk) + (den[1] * k) + den[2], 2.0 * (den[2] - (den[0] * kk)),
                         (den[0] * kk) - (den[1] * k) + den[2]};
    biquad_set(s, b, a);
}

static const double k_butterworth4_q[2] = {0.54119610014619701, 1.3065629648763764};

/* The elliptic prototype as a high-pass with its passband edge prewarped to k_fm_hp_edge_hz: s -> Wc / s. */
static int
design_elliptic_highpass(dsd_voice_bandpass* bp, int rate_hz) {
    const double wc = 2.0 * (double)rate_hz * tan(M_PI * k_fm_hp_edge_hz / (double)rate_hz);
    for (int i = 0; i < 3; i++) {
        const double wz2 = k_elliptic_proto[i][0];
        const double b1 = k_elliptic_proto[i][1];
        const double b0 = k_elliptic_proto[i][2];
        const double g = (i == 0 ? k_elliptic_dc_gain : 1.0) * (b0 / wz2);
        const double num[3] = {g * wz2, 0.0, g * wc * wc};
        const double den[3] = {b0, b1 * wc, wc * wc};
        bilinear_section(&bp->sec[bp->sections++], num, den, rate_hz);
    }
    return 0;
}

int
dsd_voice_bandpass_design(dsd_voice_bandpass* bp, dsd_voice_band_kind kind, int rate_hz) {
    if (!bp || (kind != DSD_VOICE_BAND_FM && kind != DSD_VOICE_BAND_AM)) {
        return -1;
    }
    DSD_MEMSET(bp, 0, sizeof *bp);
    bp->kind = (int)kind;
    bp->rate_hz = rate_hz;
    if (rate_hz <= 0) {
        return 0;
    }
    const double limit = k_skip_corner_fraction * (double)rate_hz;
    if (kind == DSD_VOICE_BAND_FM) {
        if (k_fm_hp_edge_hz < limit) {
            (void)design_elliptic_highpass(bp, rate_hz);
        }
    } else if (k_am_hp_hz < limit) {
        for (int i = 0; i < 2; i++) {
            rbj_section(&bp->sec[bp->sections++], 1, k_am_hp_hz, k_butterworth4_q[i], rate_hz);
        }
    }
    if (k_lp_hz < limit) {
        for (int i = 0; i < 2; i++) {
            rbj_section(&bp->sec[bp->sections++], 0, k_lp_hz, k_butterworth4_q[i], rate_hz);
        }
    }
    return 0;
}

void
dsd_voice_bandpass_reset(dsd_voice_bandpass* bp) {
    if (!bp) {
        return;
    }
    for (int i = 0; i < bp->sections; i++) {
        bp->sec[i].s1 = 0.0;
        bp->sec[i].s2 = 0.0;
    }
}

void
dsd_voice_bandpass_process(dsd_voice_bandpass* bp, float* buf, size_t n) {
    if (!bp || !buf) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        double x = (double)buf[i];
        if (!(fabs(x) < k_finite_limit)) {
            buf[i] = 0.0f;
            continue;
        }
        for (int k = 0; k < bp->sections; k++) {
            dsd_voice_biquad* s = &bp->sec[k];
            const double y = (s->b0 * x) + s->s1;
            s->s1 = (s->b1 * x) - (s->a1 * y) + s->s2;
            s->s2 = (s->b2 * x) - (s->a2 * y);
            x = y;
        }
        buf[i] = (float)x;
    }
}

double
dsd_voice_bandpass_gain_db(const dsd_voice_bandpass* bp, double hz) {
    if (!bp || bp->sections <= 0 || bp->rate_hz <= 0) {
        return 0.0;
    }
    const double w = 2.0 * M_PI * hz / (double)bp->rate_hz;
    const double c1 = cos(w);
    const double s1 = -sin(w);
    const double c2 = cos(2.0 * w);
    const double s2 = -sin(2.0 * w);
    double mag2 = 1.0;
    for (int k = 0; k < bp->sections; k++) {
        const dsd_voice_biquad* s = &bp->sec[k];
        const double nr = s->b0 + (s->b1 * c1) + (s->b2 * c2);
        const double ni = (s->b1 * s1) + (s->b2 * s2);
        const double dr = 1.0 + (s->a1 * c1) + (s->a2 * c2);
        const double di = (s->a1 * s1) + (s->a2 * s2);
        const double den = (dr * dr) + (di * di);
        if (!(den > 0.0)) {
            return -300.0;
        }
        mag2 *= ((nr * nr) + (ni * ni)) / den;
    }
    return mag2 > 1e-30 ? 10.0 * log10(mag2) : -300.0;
}

static double
db_per_second_to_factor(double db_per_s, int rate_hz) {
    return pow(10.0, -db_per_s / (20.0 * (double)rate_hz));
}

int
dsd_voice_agc_init(dsd_voice_agc* agc, int rate_hz, double ref_gain) {
    if (!agc || rate_hz <= 0 || !(ref_gain > 0.0 && ref_gain < k_finite_limit)) {
        return -1;
    }
    DSD_MEMSET(agc, 0, sizeof *agc);
    agc->rate_hz = rate_hz;
    agc->ref_gain = ref_gain;
    agc->max_gain = ref_gain * pow(10.0, DSD_VOICE_AGC_MAX_BOOST_DB / 20.0);
    /* The reference input is the one the reference gain takes to the fixed-gain reference peak: -12 dBFS. */
    const double ref_in = 8231.0 / ref_gain;
    agc->floor_in = ref_in * pow(10.0, DSD_VOICE_AGC_FLOOR_DB / 20.0);
    agc->env_min = (double)DSD_VOICE_AGC_TARGET_PEAK / agc->max_gain;
    agc->release_fast = db_per_second_to_factor(DSD_VOICE_AGC_RELEASE_FAST_DB_PER_S, rate_hz);
    agc->release_slow = db_per_second_to_factor(DSD_VOICE_AGC_RELEASE_SLOW_DB_PER_S, rate_hz);
    agc->near_ratio = pow(10.0, -DSD_VOICE_AGC_NEAR_DB / 20.0);
    agc->level_decay = exp(-1000.0 / ((double)rate_hz * (double)DSD_VOICE_AGC_LEVEL_MS));
    agc->hold_samples = (uint32_t)(((int64_t)rate_hz * DSD_VOICE_AGC_HOLD_MS) / 1000);
    agc->checkpoint_samples = (uint32_t)(((int64_t)rate_hz * DSD_VOICE_AGC_CHECKPOINT_MS) / 1000);
    if (agc->checkpoint_samples == 0U) {
        agc->checkpoint_samples = 1U;
    }
    dsd_voice_agc_reset(agc);
    return 0;
}

void
dsd_voice_agc_reset(dsd_voice_agc* agc) {
    if (!agc || !(agc->ref_gain > 0.0)) {
        return;
    }
    agc->env = (double)DSD_VOICE_AGC_TARGET_PEAK / agc->ref_gain;
    agc->level = 0.0;
    agc->hold = 0U;
    agc->playing = 0;
    agc->since_checkpoint = 0U;
    agc->points_head = 0;
    agc->points_count = 0;
}

static void
agc_push_point(dsd_voice_agc* agc) {
    agc->points[agc->points_head].env = agc->env;
    agc->points[agc->points_head].hold = agc->hold;
    agc->points_head = (agc->points_head + 1) % DSD_VOICE_AGC_CHECKPOINTS;
    if (agc->points_count < DSD_VOICE_AGC_CHECKPOINTS) {
        agc->points_count++;
    }
}

/* The gate closed: back to the oldest checkpoint, DSD_VOICE_AGC_ROLLBACK_MS ago once the ring is full, or the state
   the gate opened on when it was open for less. */
static void
agc_roll_back(dsd_voice_agc* agc) {
    if (agc->points_count > 0) {
        const int oldest =
            (agc->points_head - agc->points_count + DSD_VOICE_AGC_CHECKPOINTS) % DSD_VOICE_AGC_CHECKPOINTS;
        agc->env = agc->points[oldest].env;
        agc->hold = agc->points[oldest].hold;
    }
    agc->points_head = 0;
    agc->points_count = 0;
    agc->since_checkpoint = 0U;
}

static void
agc_set_playing(dsd_voice_agc* agc, int playing) {
    if (playing && !agc->playing) {
        agc->points_head = 0;
        agc->points_count = 0;
        agc->since_checkpoint = 0U;
        agc_push_point(agc);
    } else if (!playing && agc->playing) {
        agc_roll_back(agc);
    }
    agc->playing = playing;
}

/* One playing sample's envelope update. The attack follows the sample itself, so the gain always limits it; the hold
   and the release follow the input level, which a waveform's zero crossings do not pull down. */
static void
agc_track(dsd_voice_agc* agc, double a) {
    const double decayed = agc->level * agc->level_decay;
    agc->level = a > decayed ? a : decayed;
    if (a >= agc->env) {
        agc->env = a;
        agc->hold = agc->hold_samples;
    } else if (agc->level >= agc->floor_in) {
        if (agc->hold > 0U) {
            agc->hold--;
        } else {
            agc->env *= (agc->level >= agc->env * agc->near_ratio) ? agc->release_fast : agc->release_slow;
            if (agc->env < agc->env_min) {
                agc->env = agc->env_min;
            }
        }
    }
    if (++agc->since_checkpoint >= agc->checkpoint_samples) {
        agc->since_checkpoint = 0U;
        agc_push_point(agc);
    }
}

void
dsd_voice_agc_process(dsd_voice_agc* agc, float* buf, size_t n, int playing) {
    if (!agc || !buf || !(agc->ref_gain > 0.0)) {
        return;
    }
    if (n == 0U) {
        return;
    }
    agc_set_playing(agc, playing ? 1 : 0);
    const double target = (double)DSD_VOICE_AGC_TARGET_PEAK;
    for (size_t i = 0; i < n; i++) {
        const double x = (double)buf[i];
        const double a = fabs(x);
        if (!(a < k_finite_limit)) {
            buf[i] = 0.0f;
            continue;
        }
        if (agc->playing) {
            agc_track(agc, a);
        }
        double g = target / agc->env;
        if (a * g > target) {
            /* Only while not playing, when the envelope does not follow the input. */
            g = target / a;
        }
        buf[i] = (float)(x * g);
    }
}

double
dsd_voice_agc_gain(const dsd_voice_agc* agc) {
    if (!agc || !(agc->env > 0.0)) {
        return 0.0;
    }
    return (double)DSD_VOICE_AGC_TARGET_PEAK / agc->env;
}
