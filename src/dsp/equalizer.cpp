// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * This implements a GNU Radio-style blind CMA linear equalizer for the
 * CQPSK/H-DQPSK path. It follows the same constant-modulus error convention as
 * GNU Radio's gr::digital::adaptive_algorithm_cma:
 *
 *   error = y * (|y|^2 - modulus)
 *   taps += -step * error * conj(input_vector)
 *
 * GNU Radio attribution:
 *   Copyright 2020 Free Software Foundation, Inc.
 *   SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The surrounding streaming/history code is local to dsd-neo and is deliberately
 * simpler than GNU Radio's generic Linear Equalizer block because dsd-neo feeds
 * this block at one complex sample per recovered symbol.
 */

#include <cmath>
#include <cstring>
#include <dsd-neo/dsp/equalizer.h>

namespace {

static int
sanitize_taps(int taps) {
    if (taps < 3) {
        taps = 3;
    }
    if (taps > DSD_CQPSK_CMA_EQ_MAX_TAPS) {
        taps = DSD_CQPSK_CMA_EQ_MAX_TAPS;
    }
    if ((taps & 1) == 0) {
        taps += 1;
    }
    if (taps > DSD_CQPSK_CMA_EQ_MAX_TAPS) {
        taps = DSD_CQPSK_CMA_EQ_MAX_TAPS;
    }
    return taps;
}

static float
clip_unit(float v) {
    if (v > 1.0f) {
        return 1.0f;
    }
    if (v < -1.0f) {
        return -1.0f;
    }
    return v;
}

static void
reset_center_spike(dsd_cqpsk_cma_equalizer_state_t* st, int taps) {
    if (!st) {
        return;
    }
    taps = sanitize_taps(taps);
    *st = dsd_cqpsk_cma_equalizer_state_t{};
    st->taps = taps;
    st->taps_r[taps / 2] = 1.0f;
    st->initialized = 1;
}

static bool
state_is_finite(const dsd_cqpsk_cma_equalizer_state_t* st, int taps) {
    if (!st) {
        return false;
    }
    for (int k = 0; k < taps; k++) {
        if (!std::isfinite(st->taps_r[k]) || !std::isfinite(st->taps_i[k])) {
            return false;
        }
    }
    return true;
}

static float
tap_mag(float r, float i) {
    return std::sqrt(r * r + i * i);
}

static float
smoothstep(float edge0, float edge1, float x) {
    if (x <= edge0) {
        return 0.0f;
    }
    if (x >= edge1) {
        return 1.0f;
    }
    float t = (x - edge0) / (edge1 - edge0);
    return t * t * (3.0f - 2.0f * t);
}

static float
cqpsk_cma_effective_step(float step, float abs_err, unsigned int symbols) {
    constexpr float kErrBoostLow = 0.02f;
    constexpr float kErrBoostHigh = 0.08f;
    constexpr float kErrBoostMax = 1.5f;
    constexpr unsigned int kWarmupSymbols = 2400U;
    constexpr float kWarmupBoost = 1.25f;

    if (!(step > 0.0f) || !std::isfinite(step)) {
        return DSD_CQPSK_CMA_EQ_DEFAULT_MU;
    }

    float boost = 1.0f;
    if (std::isfinite(abs_err)) {
        boost += (kErrBoostMax - 1.0f) * smoothstep(kErrBoostLow, kErrBoostHigh, abs_err);
    }
    if (symbols < kWarmupSymbols) {
        boost *= kWarmupBoost;
    }

    float effective = step * boost;
    if (effective > 0.01f) {
        effective = 0.01f;
    }
    return effective;
}

static float
sanitize_step(float step) {
    if (!(step > 0.0f) || !std::isfinite(step)) {
        step = DSD_CQPSK_CMA_EQ_DEFAULT_MU;
    }
    if (step > 0.01f) {
        step = 0.01f;
    }
    return step;
}

static float
sanitize_modulus(float modulus) {
    if (!(modulus > 0.0f) || !std::isfinite(modulus)) {
        modulus = DSD_CQPSK_CMA_EQ_DEFAULT_MODULUS;
    }
    return modulus;
}

static void
sanitize_iq_sample(float* in_r, float* in_i) {
    if (!std::isfinite(*in_r)) {
        *in_r = 0.0f;
    }
    if (!std::isfinite(*in_i)) {
        *in_i = 0.0f;
    }
}

static void
push_iq_history(dsd_cqpsk_cma_equalizer_state_t* st, int taps, float in_r, float in_i) {
    for (int k = taps - 1; k > 0; k--) {
        st->hist_r[k] = st->hist_r[k - 1];
        st->hist_i[k] = st->hist_i[k - 1];
    }
    st->hist_r[0] = in_r;
    st->hist_i[0] = in_i;
    if (st->filled < taps) {
        st->filled++;
    }
}

static void
equalize_sample(const dsd_cqpsk_cma_equalizer_state_t* st, int taps, float* out_r, float* out_i) {
    float y_r = 0.0f;
    float y_i = 0.0f;
    for (int k = 0; k < taps; k++) {
        const float tr = st->taps_r[k];
        const float ti = st->taps_i[k];
        const float xr = st->hist_r[k];
        const float xi = st->hist_i[k];
        y_r += tr * xr - ti * xi;
        y_i += tr * xi + ti * xr;
    }
    *out_r = y_r;
    *out_i = y_i;
}

static bool
ready_for_cma_update(const dsd_cqpsk_cma_equalizer_state_t* st, int taps, float mag2) {
    if (st->filled < taps) {
        return false;
    }
    if (mag2 < 1.0e-8f) {
        return false;
    }
    return std::isfinite(mag2);
}

static float
update_cma_taps(dsd_cqpsk_cma_equalizer_state_t* st, int taps, float mu_err_r, float mu_err_i) {
    float tap_energy = 0.0f;
    for (int k = 0; k < taps; k++) {
        const float xr = st->hist_r[k];
        const float xi = st->hist_i[k];
        const float tr = st->taps_r[k] + mu_err_r * xr + mu_err_i * xi;
        const float ti = st->taps_i[k] + mu_err_i * xr - mu_err_r * xi;
        st->taps_r[k] = tr;
        st->taps_i[k] = ti;
        tap_energy += tr * tr + ti * ti;
    }
    return tap_energy;
}

static bool
tap_energy_is_valid(float tap_energy) {
    if (!std::isfinite(tap_energy)) {
        return false;
    }
    if (tap_energy > 16.0f) {
        return false;
    }
    return tap_energy >= 1.0e-6f;
}

static void
update_cma_emas(dsd_cqpsk_cma_equalizer_state_t* st, float abs_err, float mag2) {
    if (st->symbols == 0U) {
        st->err_ema = abs_err;
        st->mag2_ema = mag2;
        return;
    }
    st->err_ema += 0.02f * (abs_err - st->err_ema);
    st->mag2_ema += 0.02f * (mag2 - st->mag2_ema);
}

} // namespace

extern "C" void
dsd_cqpsk_cma_equalizer_init(dsd_cqpsk_cma_equalizer_state_t* st, int taps) {
    reset_center_spike(st, taps);
}

extern "C" void
dsd_cqpsk_cma_equalizer_reset(dsd_cqpsk_cma_equalizer_state_t* st, int taps) {
    reset_center_spike(st, taps);
}

extern "C" void
dsd_cqpsk_cma_equalizer_apply(dsd_cqpsk_cma_equalizer_state_t* st, float* iq, int len, int taps, float step,
                              float modulus) {
    if (!st || !iq || len < 2) {
        return;
    }

    taps = sanitize_taps(taps);
    if (!st->initialized || st->taps != taps || !state_is_finite(st, taps)) {
        reset_center_spike(st, taps);
    }

    step = sanitize_step(step);
    modulus = sanitize_modulus(modulus);

    const int pairs = len >> 1;
    for (int n = 0; n < pairs; n++) {
        const size_t ii = (size_t)n << 1;
        float in_r = iq[ii];
        float in_i = iq[ii + 1];
        sanitize_iq_sample(&in_r, &in_i);
        push_iq_history(st, taps, in_r, in_i);

        float out_r = 0.0f;
        float out_i = 0.0f;
        equalize_sample(st, taps, &out_r, &out_i);

        iq[ii] = out_r;
        iq[ii + 1] = out_i;

        const float mag2 = out_r * out_r + out_i * out_i;
        if (!ready_for_cma_update(st, taps, mag2)) {
            continue;
        }

        const float abs_err = std::fabs(mag2 - modulus);
        const float effective_step = cqpsk_cma_effective_step(step, abs_err, st->symbols);
        float err_r = out_r * (mag2 - modulus);
        float err_i = out_i * (mag2 - modulus);
        err_r = clip_unit(err_r);
        err_i = clip_unit(err_i);

        const float mu_err_r = -effective_step * err_r;
        const float mu_err_i = -effective_step * err_i;
        const float tap_energy = update_cma_taps(st, taps, mu_err_r, mu_err_i);
        if (!tap_energy_is_valid(tap_energy)) {
            reset_center_spike(st, taps);
            continue;
        }

        update_cma_emas(st, abs_err, mag2);
        st->symbols++;
    }
}

extern "C" void
dsd_cqpsk_cma_equalizer_get_metrics(const dsd_cqpsk_cma_equalizer_state_t* st, dsd_cqpsk_cma_equalizer_metrics_t* out) {
    if (!out) {
        return;
    }
    *out = dsd_cqpsk_cma_equalizer_metrics_t{};
    if (!st) {
        return;
    }

    int taps = st->taps;
    if (taps < 0) {
        taps = 0;
    }
    if (taps > DSD_CQPSK_CMA_EQ_MAX_TAPS) {
        taps = DSD_CQPSK_CMA_EQ_MAX_TAPS;
    }
    out->initialized = st->initialized ? 1 : 0;
    out->taps = taps;
    out->symbols = st->symbols;
    out->err_ema = std::isfinite(st->err_ema) ? st->err_ema : 0.0f;
    out->mag2_ema = std::isfinite(st->mag2_ema) ? st->mag2_ema : 0.0f;

    const int center = taps / 2;
    for (int k = 0; k < taps; k++) {
        const float tr = std::isfinite(st->taps_r[k]) ? st->taps_r[k] : 0.0f;
        const float ti = std::isfinite(st->taps_i[k]) ? st->taps_i[k] : 0.0f;
        const float energy = tr * tr + ti * ti;
        out->tap_energy += energy;
        const float mag = tap_mag(tr, ti);
        if (k == center) {
            out->center_tap_mag = mag;
        } else if (mag > out->max_side_tap_mag) {
            out->max_side_tap_mag = mag;
        }
    }
}
