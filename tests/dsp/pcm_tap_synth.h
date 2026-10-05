// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Seeded FM audio the way an SDR program writes its output for the PCM noise squelch's tests (issue #628,
 * tools/pcm_noise_squelch_model.py's "sdr" sources): complex noise and a carrier through a Hamming-windowed sinc
 * channel filter, the phase step per sample, optional de-emphasis and audio low-pass, a volume and int16 rounding, at
 * 48 kHz.
 */

#ifndef DSD_NEO_TESTS_DSP_PCM_TAP_SYNTH_H_
#define DSD_NEO_TESTS_DSP_PCM_TAP_SYNTH_H_

#include <dsd-neo/core/safe_api.h>
#include <math.h>
#include <stdint.h>

#include "analog_tone_synth.h"

enum {
    PCM_TAP_RATE = 48000,
    PCM_TAP_CH_TAPS = 127,
    PCM_TAP_LPF_TAPS = 255,
};

/* PCM_TAP_SYLLABIC: the tone with a 3 Hz syllabic envelope, so its voice band is never steady over a window run, as
   speech's is not. PCM_TAP_RELAY: a carrier relaying noise, as a repeater does a weak user's: Gaussian noise through a
   300-3000 Hz audio band (a second-order high-pass and low-pass), relay_rms of it deviating the carrier by the tone's
   deviation. */
typedef enum { PCM_TAP_NOISE, PCM_TAP_DEAD, PCM_TAP_TONE, PCM_TAP_ZERO, PCM_TAP_SYLLABIC, PCM_TAP_RELAY } pcm_tap_kind;

/* An SDR program's FM output: noise plus a carrier through a Hamming-windowed sinc channel filter, the phase step per
   sample, optional de-emphasis and audio low-pass, a gain and int16 rounding. */
typedef struct {
    synth_rng rng;
    double width_hz;
    double dev_hz;
    double taps[PCM_TAP_CH_TAPS];
    double sum_h2;
    double ring_re[PCM_TAP_CH_TAPS];
    double ring_im[PCM_TAP_CH_TAPS];
    int pos;
    double prev_re;
    double prev_im;
    double pole; /* de-emphasis, 0: none */
    double deemph;
    double lpf[PCM_TAP_LPF_TAPS];
    int lpf_on;
    double lpf_ring[PCM_TAP_LPF_TAPS];
    int lpf_pos;
    double carrier_phase;
    double tone_phase;
    double syllable_phase;
    double offset_hz;   /* the carrier's offset from the channel's centre */
    double tone_dev_hz; /* the tone's deviation; 0 is the width's rated deviation */
    double scale;       /* noise alone at -20 dBFS RMS */
    /* PCM_TAP_RELAY's audio band: high-pass and low-pass biquads (b0, b1, b2, a1, a2), their states, and the scale that
       makes unit Gaussian noise through them unit RMS. */
    double relay_hp[5];
    double relay_lp[5];
    double relay_state[4];
    double relay_scale;
    double relay_rms; /* 0.35 unless a test sets it */
} pcm_tap;

/* A Butterworth biquad (b0, b1, b2, a1, a2) at @p f0_hz: a high-pass when @p high, else a low-pass. */
static inline void
pcm_tap_biquad(double* c, double f0_hz, int high) {
    const double w0 = 2.0 * M_PI * f0_hz / (double)PCM_TAP_RATE;
    const double alpha = sin(w0) / (2.0 * 0.70710678118654752);
    const double cw = cos(w0);
    const double a0 = 1.0 + alpha;
    const double b = high ? (1.0 + cw) / 2.0 : (1.0 - cw) / 2.0;
    c[0] = b / a0;
    c[1] = (high ? -2.0 * b : 2.0 * b) / a0;
    c[2] = b / a0;
    c[3] = (-2.0 * cw) / a0;
    c[4] = (1.0 - alpha) / a0;
}

/* One sample through a biquad @p c with state @p st (transposed direct form II). */
static inline double
pcm_tap_biquad_run(const double* c, double* st, double x) {
    const double y = (c[0] * x) + st[0];
    st[0] = (c[1] * x) - (c[3] * y) + st[1];
    st[1] = (c[2] * x) - (c[4] * y);
    return y;
}

/* One sample of PCM_TAP_RELAY's audio, unscaled. */
static inline double
pcm_tap_relay_audio(pcm_tap* s) {
    const double x = synth_gauss(&s->rng);
    return pcm_tap_biquad_run(s->relay_lp, &s->relay_state[2], pcm_tap_biquad_run(s->relay_hp, &s->relay_state[0], x));
}

static inline void
pcm_tap_sinc(double* h, int n, double cutoff_hz) {
    const double fc = cutoff_hz / (double)PCM_TAP_RATE;
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

static inline double pcm_tap_raw(pcm_tap* s, pcm_tap_kind kind, double cnr_db, double tone_hz);

static inline void
pcm_tap_init(pcm_tap* s, uint64_t seed, double width_hz, double deemph_us, double lpf_hz) {
    DSD_MEMSET(s, 0, sizeof(*s));
    synth_rng_seed(&s->rng, seed);
    s->width_hz = width_hz;
    {
        const double a = (width_hz / 2.0) - 3000.0;
        const double b = width_hz / 8.0;
        const double dev = a > b ? a : b;
        s->dev_hz = dev < 5000.0 ? dev : 5000.0;
    }
    pcm_tap_sinc(s->taps, PCM_TAP_CH_TAPS, width_hz / 2.0);
    for (int i = 0; i < PCM_TAP_CH_TAPS; i++) {
        s->sum_h2 += s->taps[i] * s->taps[i];
    }
    s->prev_re = 1.0;
    s->pole = deemph_us > 0.0 ? exp(-1.0 / ((double)PCM_TAP_RATE * deemph_us * 1e-6)) : 0.0;
    if (lpf_hz > 0.0) {
        pcm_tap_sinc(s->lpf, PCM_TAP_LPF_TAPS, lpf_hz);
        s->lpf_on = 1;
    }
    /* Calibrate on a copy: noise alone at -20 dBFS RMS. */
    pcm_tap probe = *s;
    double e = 0.0;
    const int n = PCM_TAP_RATE / 2;
    const int settle = n / 4;
    const int measured = n - settle;
    for (int i = 0; i < n; i++) {
        const double x = pcm_tap_raw(&probe, PCM_TAP_NOISE, 0.0, 0.0);
        if (i >= settle) {
            e += x * x;
        }
    }
    s->scale = pow(10.0, -20.0 / 20.0) * 32768.0 / sqrt(e / (double)measured);
    /* PCM_TAP_RELAY's audio band, calibrated on a copy to unit RMS. */
    pcm_tap_biquad(s->relay_hp, 300.0, 1);
    pcm_tap_biquad(s->relay_lp, 3000.0, 0);
    s->relay_rms = 0.35;
    pcm_tap band = *s;
    double r = 0.0;
    for (int i = 0; i < n; i++) {
        const double x = pcm_tap_relay_audio(&band);
        if (i >= settle) {
            r += x * x;
        }
    }
    s->relay_scale = 1.0 / sqrt(r / (double)measured);
}

/* The discriminator output (after de-emphasis and the audio low-pass), unscaled. */
static inline double
pcm_tap_raw(pcm_tap* s, pcm_tap_kind kind, double cnr_db, double tone_hz) {
    double re = synth_gauss(&s->rng) * 0.70710678118654752;
    double im = synth_gauss(&s->rng) * 0.70710678118654752;
    if (kind == PCM_TAP_DEAD || kind == PCM_TAP_TONE || kind == PCM_TAP_SYLLABIC || kind == PCM_TAP_RELAY) {
        const double amp = sqrt(s->sum_h2 * pow(10.0, cnr_db / 10.0));
        if (kind == PCM_TAP_RELAY) {
            const double dev = s->tone_dev_hz > 0.0 ? s->tone_dev_hz : s->dev_hz;
            const double au = s->relay_rms * s->relay_scale * pcm_tap_relay_audio(s);
            s->carrier_phase += 2.0 * M_PI * dev * au / (double)PCM_TAP_RATE;
        } else if (kind != PCM_TAP_DEAD) {
            s->tone_phase += 2.0 * M_PI * tone_hz / (double)PCM_TAP_RATE;
            double dev = s->tone_dev_hz > 0.0 ? s->tone_dev_hz : s->dev_hz;
            if (kind == PCM_TAP_SYLLABIC) {
                s->syllable_phase += 2.0 * M_PI * 3.0 / (double)PCM_TAP_RATE;
                dev *= 0.1 + (0.9 * fabs(sin(s->syllable_phase)));
            }
            s->carrier_phase += 2.0 * M_PI * dev * sin(s->tone_phase) / (double)PCM_TAP_RATE;
        }
        s->carrier_phase += 2.0 * M_PI * s->offset_hz / (double)PCM_TAP_RATE;
        re += amp * cos(s->carrier_phase);
        im += amp * sin(s->carrier_phase);
    }
    s->ring_re[s->pos] = re;
    s->ring_im[s->pos] = im;
    s->pos = (s->pos + 1) % PCM_TAP_CH_TAPS;
    double zr = 0.0;
    double zi = 0.0;
    for (int j = 0; j < PCM_TAP_CH_TAPS; j++) {
        int at = s->pos - 1 - j;
        if (at < 0) {
            at += PCM_TAP_CH_TAPS;
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
        s->lpf_pos = (s->lpf_pos + 1) % PCM_TAP_LPF_TAPS;
        double acc = 0.0;
        for (int j = 0; j < PCM_TAP_LPF_TAPS; j++) {
            int at = s->lpf_pos - 1 - j;
            if (at < 0) {
                at += PCM_TAP_LPF_TAPS;
            }
            acc += s->lpf_ring[at] * s->lpf[j];
        }
        y = acc;
    }
    return y;
}

/* One int16-scale sample: kind at cnr_db, gain_db over the calibrated volume. */
static inline float
pcm_tap_next(pcm_tap* s, pcm_tap_kind kind, double cnr_db, double tone_hz, double gain_db) {
    const double raw = pcm_tap_raw(s, kind == PCM_TAP_ZERO ? PCM_TAP_NOISE : kind, cnr_db, tone_hz);
    if (kind == PCM_TAP_ZERO) {
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

#endif /* DSD_NEO_TESTS_DSP_PCM_TAP_SYNTH_H_ */
