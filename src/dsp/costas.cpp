// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright 2006,2010-2012 Free Software Foundation, Inc.
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Costas loop carrier recovery (GNU Radio port).
 *
 * Port of GNU Radio's costas_loop_cc implementation. The default loop bandwidth
 * is reduced (2*pi/800 rad/sample) to match the narrower DSP bandwidth run by
 * dsd-neo compared to typical GNU Radio flows.
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>

#include <cmath>
#include <complex>
#include <cstdint>

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kQ15ToRad = kTwoPi / 32768.0f;
constexpr float kRadToQ15 = 32768.0f / kTwoPi;

/* LUT copied from GNU Radio blocks/control_loop.h */
static const float kTanhLut[256] = {
    -0.96402758, -0.96290241, -0.96174273, -0.96054753, -0.95931576, -0.95804636, -0.95673822, -0.95539023, -0.95400122,
    -0.95257001, -0.95109539, -0.9495761,  -0.94801087, -0.94639839, -0.94473732, -0.94302627, -0.94126385, -0.93944862,
    -0.93757908, -0.93565374, -0.93367104, -0.93162941, -0.92952723, -0.92736284, -0.92513456, -0.92284066, -0.92047938,
    -0.91804891, -0.91554743, -0.91297305, -0.91032388, -0.90759795, -0.9047933,  -0.90190789, -0.89893968, -0.89588656,
    -0.89274642, -0.88951709, -0.88619637, -0.88278203, -0.87927182, -0.87566342, -0.87195453, -0.86814278, -0.86422579,
    -0.86020115, -0.85606642, -0.85181914, -0.84745683, -0.84297699, -0.83837709, -0.83365461, -0.82880699, -0.82383167,
    -0.81872609, -0.81348767, -0.80811385, -0.80260204, -0.7969497,  -0.79115425, -0.78521317, -0.77912392, -0.772884,
    -0.76649093, -0.75994227, -0.75323562, -0.74636859, -0.73933889, -0.73214422, -0.7247824,  -0.71725127, -0.70954876,
    -0.70167287, -0.6936217,  -0.68539341, -0.67698629, -0.66839871, -0.65962916, -0.65067625, -0.64153871, -0.6322154,
    -0.62270534, -0.61300768, -0.60312171, -0.59304692, -0.58278295, -0.57232959, -0.56168685, -0.55085493, -0.53983419,
    -0.52862523, -0.51722883, -0.50564601, -0.49387799, -0.48192623, -0.46979241, -0.45747844, -0.44498647, -0.4323189,
    -0.41947836, -0.40646773, -0.39329014, -0.37994896, -0.36644782, -0.35279057, -0.33898135, -0.32502449, -0.31092459,
    -0.2966865,  -0.28231527, -0.26781621, -0.25319481, -0.23845682, -0.22360817, -0.208655,   -0.19360362, -0.17846056,
    -0.16323249, -0.14792623, -0.13254879, -0.11710727, -0.10160892, -0.08606109, -0.07047123, -0.05484686, -0.0391956,
    -0.02352507, -0.00784298, 0.00784298,  0.02352507,  0.0391956,   0.05484686,  0.07047123,  0.08606109,  0.10160892,
    0.11710727,  0.13254879,  0.14792623,  0.16323249,  0.17846056,  0.19360362,  0.208655,    0.22360817,  0.23845682,
    0.25319481,  0.26781621,  0.28231527,  0.2966865,   0.31092459,  0.32502449,  0.33898135,  0.35279057,  0.36644782,
    0.37994896,  0.39329014,  0.40646773,  0.41947836,  0.4323189,   0.44498647,  0.45747844,  0.46979241,  0.48192623,
    0.49387799,  0.50564601,  0.51722883,  0.52862523,  0.53983419,  0.55085493,  0.56168685,  0.57232959,  0.58278295,
    0.59304692,  0.60312171,  0.61300768,  0.62270534,  0.6322154,   0.64153871,  0.65067625,  0.65962916,  0.66839871,
    0.67698629,  0.68539341,  0.6936217,   0.70167287,  0.70954876,  0.71725127,  0.7247824,   0.73214422,  0.73933889,
    0.74636859,  0.75323562,  0.75994227,  0.76649093,  0.772884,    0.77912392,  0.78521317,  0.79115425,  0.7969497,
    0.80260204,  0.80811385,  0.81348767,  0.81872609,  0.82383167,  0.82880699,  0.83365461,  0.83837709,  0.84297699,
    0.84745683,  0.85181914,  0.85606642,  0.86020115,  0.86422579,  0.86814278,  0.87195453,  0.87566342,  0.87927182,
    0.88278203,  0.88619637,  0.88951709,  0.89274642,  0.89588656,  0.89893968,  0.90190789,  0.9047933,   0.90759795,
    0.91032388,  0.91297305,  0.91554743,  0.91804891,  0.92047938,  0.92284066,  0.92513456,  0.92736284,  0.92952723,
    0.93162941,  0.93367104,  0.93565374,  0.93757908,  0.93944862,  0.94126385,  0.94302627,  0.94473732,  0.94639839,
    0.94801087,  0.9495761,   0.95109539,  0.95257001,  0.95400122,  0.95539023,  0.95673822,  0.95804636,  0.95931576,
    0.96054753,  0.96174273,  0.96290241,  0.96402758};

static inline float
tanhf_lut(float x) {
    if (x > 2.0f) {
        return 1.0f;
    }
    if (x <= -2.0f) {
        return -1.0f;
    }
    int index = 128 + static_cast<int>(64.0f * x);
    return kTanhLut[index];
}

static inline float
branchless_clip(float x, float clip) {
    return 0.5f * (std::abs(x + clip) - std::abs(x - clip));
}

static inline int16_t
clamp_to_i16(float v) {
    long s = lrintf(v);
    if (s > 32767) {
        s = 32767;
    } else if (s < -32768) {
        s = -32768;
    }
    return static_cast<int16_t>(s);
}

static void
update_gains(dsd_costas_loop_state_t* c) {
    float bw = c->loop_bw;
    if (bw < 0.0f) {
        bw = 0.0f;
    }
    const float denom = 1.0f + 2.0f * c->damping * bw + bw * bw;
    c->alpha = (4.0f * c->damping * bw) / denom;
    c->beta = (4.0f * bw * bw) / denom;
}

static inline void
advance_loop(dsd_costas_loop_state_t* c, float err) {
    c->freq += c->beta * err;
    c->phase += c->freq + c->alpha * err;
}

static inline void
phase_wrap(dsd_costas_loop_state_t* c) {
    while (c->phase > kTwoPi) {
        c->phase -= kTwoPi;
    }
    while (c->phase < -kTwoPi) {
        c->phase += kTwoPi;
    }
}

static inline void
frequency_limit(dsd_costas_loop_state_t* c) {
    if (c->freq > c->max_freq) {
        c->freq = c->max_freq;
    } else if (c->freq < c->min_freq) {
        c->freq = c->min_freq;
    }
}

static inline float
phase_detector_8(const std::complex<float>& sample) {
    const float K = (std::sqrt(2.0f) - 1.0f);
    if (fabsf(sample.real()) >= fabsf(sample.imag())) {
        return ((sample.real() > 0.0f ? 1.0f : -1.0f) * sample.imag()
                - (sample.imag() > 0.0f ? 1.0f : -1.0f) * sample.real() * K);
    }
    return ((sample.real() > 0.0f ? 1.0f : -1.0f) * sample.imag() * K
            - (sample.imag() > 0.0f ? 1.0f : -1.0f) * sample.real());
}

static inline float
phase_detector_4(const std::complex<float>& sample) {
    return ((sample.real() > 0.0f ? 1.0f : -1.0f) * sample.imag()
            - (sample.imag() > 0.0f ? 1.0f : -1.0f) * sample.real());
}

static inline float
phase_detector_2(const std::complex<float>& sample) {
    return sample.real() * sample.imag();
}

static inline float
phase_detector_snr_8(const std::complex<float>& sample, float noise) {
    const float K = (std::sqrt(2.0f) - 1.0f);
    const float snr = std::norm(sample) / noise;
    if (fabsf(sample.real()) >= fabsf(sample.imag())) {
        return (tanhf_lut(snr * sample.real()) * sample.imag()) - (tanhf_lut(snr * sample.imag()) * sample.real() * K);
    }
    return (tanhf_lut(snr * sample.real()) * sample.imag() * K) - (tanhf_lut(snr * sample.imag()) * sample.real());
}

static inline float
phase_detector_snr_4(const std::complex<float>& sample, float noise) {
    const float snr = std::norm(sample) / noise;
    return (tanhf_lut(snr * sample.real()) * sample.imag()) - (tanhf_lut(snr * sample.imag()) * sample.real());
}

static inline float
phase_detector_snr_2(const std::complex<float>& sample, float noise) {
    const float snr = std::norm(sample) / noise;
    return tanhf_lut(snr * sample.real()) * sample.imag();
}

static float
detect_error(const std::complex<float>& sample, const dsd_costas_loop_state_t* c) {
    switch (c->order) {
        case 2: return c->use_snr ? phase_detector_snr_2(sample, c->noise) : phase_detector_2(sample);
        case 8: return c->use_snr ? phase_detector_snr_8(sample, c->noise) : phase_detector_8(sample);
        case 4:
        default: return c->use_snr ? phase_detector_snr_4(sample, c->noise) : phase_detector_4(sample);
    }
}

static void
prepare_costas(dsd_costas_loop_state_t* c, const demod_state* d) {
    if (c->loop_bw <= 0.0f) {
        c->loop_bw = dsd_neo_costas_default_loop_bw();
    }
    if (c->damping <= 0.0f) {
        c->damping = dsd_neo_costas_default_damping();
    }
    if (c->max_freq == 0.0f && c->min_freq == 0.0f) {
        c->max_freq = 1.0f;
        c->min_freq = -1.0f;
    }
    if (c->max_freq < c->min_freq) {
        float tmp = c->max_freq;
        c->max_freq = c->min_freq;
        c->min_freq = tmp;
    }
    if (c->order != 2 && c->order != 4 && c->order != 8) {
        c->order = 4;
    }
    if (c->noise <= 0.0f) {
        c->noise = 1.0f;
    }
    update_gains(c);

    if (!c->initialized && d) {
        c->phase = static_cast<float>(d->fll_phase_q15) * kQ15ToRad;
        c->freq = static_cast<float>(d->fll_freq_q15) * kQ15ToRad;
        c->initialized = 1;
    }
}

} // namespace

void
cqpsk_costas_mix_and_update(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    if (!d->cqpsk_enable) {
        return;
    }

    dsd_costas_loop_state_t* c = &d->costas_state;
    prepare_costas(c, d);

    int pairs = d->lp_len >> 1;
    int16_t* iq = d->lowpassed;
    float err_acc = 0.0f;

    for (int i = 0; i < pairs; i++) {
        std::complex<float> s(static_cast<float>(iq[(i << 1) + 0]), static_cast<float>(iq[(i << 1) + 1]));
        std::complex<float> nco = std::polar(1.0f, -c->phase);
        std::complex<float> y = s * nco;

        iq[(i << 1) + 0] = clamp_to_i16(y.real());
        iq[(i << 1) + 1] = clamp_to_i16(y.imag());

        float err = detect_error(y, c);
        err = branchless_clip(err, 1.0f);
        c->error = err;
        err_acc += fabsf(err);

        advance_loop(c, err);
        phase_wrap(c);
        frequency_limit(c);
    }

    int freq_q15 = static_cast<int>(lrintf(c->freq * kRadToQ15));
    int phase_q15 = static_cast<int>(lrintf(c->phase * kRadToQ15)) & 0x7FFF;
    d->fll_freq_q15 = freq_q15;
    d->fll_phase_q15 = phase_q15;

    if (pairs > 0) {
        float avg_err = err_acc / static_cast<float>(pairs);
        int avg_q14 = static_cast<int>(lrintf(avg_err * 16384.0f));
        if (avg_q14 < 0) {
            avg_q14 = 0;
        }
        if (avg_q14 > 32767) {
            avg_q14 = 32767;
        }
        d->costas_err_avg_q14 = avg_q14;
    } else {
        d->costas_err_avg_q14 = 0;
    }
}
