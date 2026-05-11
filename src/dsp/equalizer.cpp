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

    if (!(step > 0.0f) || !std::isfinite(step)) {
        step = 0.0008f;
    }
    if (step > 0.01f) {
        step = 0.01f;
    }
    if (!(modulus > 0.0f) || !std::isfinite(modulus)) {
        modulus = 0.85f * 0.85f;
    }

    const int pairs = len >> 1;
    for (int n = 0; n < pairs; n++) {
        const size_t ii = (size_t)n << 1;
        float in_r = iq[ii];
        float in_i = iq[ii + 1];
        if (!std::isfinite(in_r)) {
            in_r = 0.0f;
        }
        if (!std::isfinite(in_i)) {
            in_i = 0.0f;
        }

        for (int k = taps - 1; k > 0; k--) {
            st->hist_r[k] = st->hist_r[k - 1];
            st->hist_i[k] = st->hist_i[k - 1];
        }
        st->hist_r[0] = in_r;
        st->hist_i[0] = in_i;
        if (st->filled < taps) {
            st->filled++;
        }

        float out_r = 0.0f;
        float out_i = 0.0f;
        for (int k = 0; k < taps; k++) {
            const float tr = st->taps_r[k];
            const float ti = st->taps_i[k];
            const float xr = st->hist_r[k];
            const float xi = st->hist_i[k];
            out_r += tr * xr - ti * xi;
            out_i += tr * xi + ti * xr;
        }

        iq[ii] = out_r;
        iq[ii + 1] = out_i;

        const float mag2 = out_r * out_r + out_i * out_i;
        if (st->filled < taps || mag2 < 1.0e-8f || !std::isfinite(mag2)) {
            continue;
        }

        float err_r = out_r * (mag2 - modulus);
        float err_i = out_i * (mag2 - modulus);
        err_r = clip_unit(err_r);
        err_i = clip_unit(err_i);

        const float mu_err_r = -step * err_r;
        const float mu_err_i = -step * err_i;
        for (int k = 0; k < taps; k++) {
            const float xr = st->hist_r[k];
            const float xi = st->hist_i[k];
            st->taps_r[k] += mu_err_r * xr + mu_err_i * xi;
            st->taps_i[k] += mu_err_i * xr - mu_err_r * xi;
        }

        float tap_energy = 0.0f;
        for (int k = 0; k < taps; k++) {
            tap_energy += st->taps_r[k] * st->taps_r[k] + st->taps_i[k] * st->taps_i[k];
        }
        if (!std::isfinite(tap_energy) || tap_energy > 16.0f || tap_energy < 1.0e-6f) {
            reset_center_spike(st, taps);
            continue;
        }

        const float abs_err = std::fabs(mag2 - modulus);
        if (st->symbols == 0U) {
            st->err_ema = abs_err;
            st->mag2_ema = mag2;
        } else {
            st->err_ema += 0.02f * (abs_err - st->err_ema);
            st->mag2_ema += 0.02f * (mag2 - st->mag2_ema);
        }
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
