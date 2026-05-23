// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * Gardner symbol recovery - ported from OP25 gardner_cc_impl.cc
 * Original: Copyright 2005,2006,2007 Free Software Foundation, Inc.
 *           Gardner symbol recovery block for GR - Copyright 2010-2015 KA1RBI
 *           Lock detector based on Yair Linn's research - Copyright 2022 gnorbury@bondcar.com
 */

/**
 * @file
 * @brief Timing Error Detector (TED) implementation: Gardner TED ported from OP25.
 *
 * This implementation mirrors OP25's gardner_cc algorithm for symbol timing recovery.
 * Key features:
 *   - 8-tap MMSE polyphase interpolation (matching GNU Radio's mmse_fir_interpolator_cc)
 *   - Circular delay line with doubled storage (OP25-style) for wrap-free access
 *   - OP25's Gardner error formula: (last - current) * mid
 *   - Dual-loop update: both omega (symbol period) and mu (phase)
 *   - Lock detector based on Yair Linn's research
 */

#include <dsd-neo/dsp/ted.h>
#include <math.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

#define IS_NAN(x) ((x) != (x))

/* OP25 default gains from p25_demodulator.py:
 *   gain_mu = 0.025
 *   gain_omega = 0.1 * gain_mu * gain_mu = 0.0000625
 *   omega_rel = 0.002 (hardcoded in constructor)
 */
static const float kDefaultGainMu = 0.025f;
static const float kDefaultGainOmega = 0.1f * kDefaultGainMu * kDefaultGainMu; /* 0.0000625 */
static const float kDefaultOmegaRel = 0.002f;
static const int kLockAccumWindow = 480; /* OP25 default: 480 symbols */
static const float kOmegaNormEps = 1e-6f;
static int s_warned_ted_dl_oversize = 0;

/*
 * GNU Radio MMSE 8-tap polyphase interpolator coefficients.
 * From gnuradio/gr-filter/include/gnuradio/filter/interpolator_taps.h
 *
 * NTAPS = 8, NSTEPS = 128
 * Optimized for signals with bandwidth B = Fs/4 (matches P25: 24kHz Fs, 4800 sym/s)
 *
 * The full table has 129 rows (mu = 0/128 to 128/128). We include a representative
 * subset at every 8th row for 17 total rows, then linearly interpolate between them.
 *
 * At mu=0 (row 0), output = sample[4] (tap[4] = 1.0, no fractional delay)
 * At mu=1 (row 128), output = sample[3] (tap[3] = 1.0, full sample delay)
 *
 * The filter interpolates between sample[4] (mu=0) and sample[3] (mu=1),
 * with the midpoint at row 64 showing equal weighting of both.
 */
#define MMSE_NTAPS  8
#define MMSE_NSTEPS 16 /* We use 17 rows (0..16) for 1/16 step resolution */

static const float mmse_taps[MMSE_NSTEPS + 1][MMSE_NTAPS] = {
    /* Row 0/128 (mu=0): output = sample[4] */
    {0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f},
    /* Row 8/128 (mu=0.0625) */
    {-1.23337e-03f, 6.84261e-03f, -2.24178e-02f, 6.57852e-02f, 9.83392e-01f, -4.04519e-02f, 9.56876e-03f,
     -1.54221e-03f},
    /* Row 16/128 (mu=0.125) */
    {-2.43121e-03f, 1.35716e-02f, -4.49929e-02f, 1.36968e-01f, 9.55956e-01f, -7.43154e-02f, 1.80759e-02f,
     -2.94361e-03f},
    /* Row 24/128 (mu=0.1875) */
    {-3.55283e-03f, 1.99599e-02f, -6.70018e-02f, 2.12443e-01f, 9.18329e-01f, -1.01501e-01f, 2.53295e-02f,
     -4.16581e-03f},
    /* Row 32/128 (mu=0.25) */
    {-4.55932e-03f, 2.57844e-02f, -8.77011e-02f, 2.91006e-01f, 8.71305e-01f, -1.22047e-01f, 3.11866e-02f,
     -5.17776e-03f},
    /* Row 40/128 (mu=0.3125) */
    {-5.41467e-03f, 3.08323e-02f, -1.06342e-01f, 3.71376e-01f, 8.15826e-01f, -1.36111e-01f, 3.55525e-02f,
     -5.95620e-03f},
    /* Row 48/128 (mu=0.375) */
    {-6.08674e-03f, 3.49066e-02f, -1.22185e-01f, 4.52218e-01f, 7.52958e-01f, -1.43968e-01f, 3.83800e-02f,
     -6.48585e-03f},
    /* Row 56/128 (mu=0.4375) */
    {-6.54823e-03f, 3.78315e-02f, -1.34515e-01f, 5.32164e-01f, 6.83875e-01f, -1.45993e-01f, 3.96678e-02f,
     -6.75943e-03f},
    /* Row 64/128 (mu=0.5): midpoint, symmetric */
    {-6.77751e-03f, 3.94578e-02f, -1.42658e-01f, 6.09836e-01f, 6.09836e-01f, -1.42658e-01f, 3.94578e-02f,
     -6.77751e-03f},
    /* Row 72/128 (mu=0.5625) */
    {-6.73929e-03f, 3.95900e-02f, -1.46043e-01f, 6.92808e-01f, 5.22267e-01f, -1.33190e-01f, 3.75341e-02f,
     -6.50285e-03f},
    /* Row 80/128 (mu=0.625) */
    {-6.48585e-03f, 3.83800e-02f, -1.43968e-01f, 7.52958e-01f, 4.52218e-01f, -1.22185e-01f, 3.49066e-02f,
     -6.08674e-03f},
    /* Row 88/128 (mu=0.6875) */
    {-5.95620e-03f, 3.55525e-02f, -1.36111e-01f, 8.15826e-01f, 3.71376e-01f, -1.06342e-01f, 3.08323e-02f,
     -5.41467e-03f},
    /* Row 96/128 (mu=0.75) */
    {-5.17776e-03f, 3.11866e-02f, -1.22047e-01f, 8.71305e-01f, 2.91006e-01f, -8.77011e-02f, 2.57844e-02f,
     -4.55932e-03f},
    /* Row 104/128 (mu=0.8125) */
    {-4.16581e-03f, 2.53295e-02f, -1.01501e-01f, 9.18329e-01f, 2.12443e-01f, -6.70018e-02f, 1.99599e-02f,
     -3.55283e-03f},
    /* Row 112/128 (mu=0.875) */
    {-2.94361e-03f, 1.80759e-02f, -7.43154e-02f, 9.55956e-01f, 1.36968e-01f, -4.49929e-02f, 1.35716e-02f,
     -2.43121e-03f},
    /* Row 120/128 (mu=0.9375) */
    {-1.54221e-03f, 9.56876e-03f, -4.04519e-02f, 9.83392e-01f, 6.57852e-02f, -2.24178e-02f, 6.84261e-03f,
     -1.23337e-03f},
    /* Row 128/128 (mu=1.0): output = sample[3] */
    {0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f},
};

/**
 * @brief Initialize TED state with default values.
 *
 * @param state TED state to initialize.
 */
void
ted_init_state(ted_state_t* state) {
    if (!state) {
        return;
    }
    state->mu = 0.0f;
    state->omega = 0.0f; /* will be set on first call based on config->sps */
    state->omega_mid = 0.0f;
    state->omega_min = 0.0f;
    state->omega_max = 0.0f;
    state->last_r = 0.0f;
    state->last_j = 0.0f;
    state->e_ema = 0.0f;
    state->lock_accum = 0.0f;
    state->lock_count = 0;
    state->event_count = 0;
    /* Initialize delay line */
    for (int i = 0; i < TED_DL_SIZE * 2 * 2; i++) {
        state->dl[i] = 0.0f;
    }
    state->dl_index = 0;
    state->twice_sps = 0;
    state->sps = 0;
}

/**
 * @brief Soft reset TED state, preserving mu and omega for phase continuity.
 *
 * This reset matches OP25's reset() behavior in gardner_cc_impl.cc:
 *   - Resets d_phase = 0 (we don't have this, it's Costas-side)
 *   - Resets d_update_request = 0 (not applicable)
 *   - Resets d_last_sample = 0 (we reset to 0+0j to match)
 *   - Resets d_lock_accum.reset()
 *
 * Note: OP25's set_omega() does NOT reset d_last_sample, only the delay line.
 * This function is for explicit reset() calls, not SPS changes.
 *
 * @param state TED state to soft-reset.
 */
void
ted_soft_reset(ted_state_t* state) {
    if (!state) {
        return;
    }
    /* OP25 reset() in gardner_cc_impl.cc:
     *   d_phase = 0;           // Costas-side, not applicable here
     *   d_update_request = 0;  // Not applicable
     *   d_last_sample = 0;     // Complex zero
     *   d_lock_accum.reset();  // Clear deque and accumulator
     */
    state->last_r = 0.0f;
    state->last_j = 0.0f;
    state->e_ema = 0.0f;
    state->lock_accum = 0.0f;
    state->lock_count = 0;
    state->event_count = 0;
    /* OP25 reset() does NOT clear the delay line or dl_index */
}

/**
 * @brief 8-tap MMSE FIR interpolation matching GNU Radio's mmse_fir_interpolator.
 *
 * Uses pre-computed polyphase coefficients with linear interpolation between
 * adjacent rows for sub-step accuracy.
 *
 * @param s Array of 8 samples: s[0] through s[7]
 * @param mu Fractional delay in [0,1)
 * @return Interpolated value (between s[3] and s[4])
 */
static inline float
mmse_interp_8tap(const float* s, float mu) {
    /* Scale mu to table index range [0, MMSE_NSTEPS] */
    float idx_f = mu * (float)MMSE_NSTEPS;
    int idx_lo = (int)idx_f;
    float frac = idx_f - (float)idx_lo;

    /* Clamp to valid range */
    if (idx_lo < 0) {
        idx_lo = 0;
        frac = 0.0f;
    }
    if (idx_lo >= MMSE_NSTEPS) {
        idx_lo = MMSE_NSTEPS - 1;
        frac = 1.0f;
    }
    int idx_hi = idx_lo + 1;

    /* Interpolate between two adjacent filter coefficient sets */
    const float* taps_lo = mmse_taps[idx_lo];
    const float* taps_hi = mmse_taps[idx_hi];
    float one_minus_frac = 1.0f - frac;

    float acc = 0.0f;
    for (int k = 0; k < MMSE_NTAPS; k++) {
        float tap = one_minus_frac * taps_lo[k] + frac * taps_hi[k];
        acc += tap * s[k];
    }
    return acc;
}

/**
 * @brief Complex 8-tap MMSE interpolation (OP25-compatible).
 *
 * Uses the OP25 convention where the delay line is doubled (samples written
 * at both dl_index and dl_index + twice_sps) so we can index directly without
 * wrap-around handling. The caller passes a pointer to the start position.
 *
 * @param dl Pointer to delay line at current index (interleaved I/Q)
 * @param mu Fractional delay [0,1)
 * @param out_r Output real part
 * @param out_j Output imaginary part
 */
static inline void
mmse_interp_cc_8tap(const float* dl, float mu, float* out_r, float* out_j) {
    float sr[MMSE_NTAPS];
    float sj[MMSE_NTAPS];

    /* Extract 8 consecutive samples directly (no wrap needed due to doubled storage) */
    for (int k = 0; k < MMSE_NTAPS; k++) {
        const size_t kk = (size_t)k;
        sr[k] = dl[kk * 2];
        sj[k] = dl[kk * 2 + 1];
    }

    *out_r = mmse_interp_8tap(sr, mu);
    *out_j = mmse_interp_8tap(sj, mu);
}

/* Branchless clip helper (matches GNU Radio) */
static inline float
branchless_clip(float x, float limit) {
    return 0.5f * (fabsf(x + limit) - fabsf(x - limit));
}

namespace {

typedef struct {
    float mu;
    float omega;
    float last_r;
    float last_j;
    int dl_index;
    int twice_sps;
    int input_index;
    int output_index;
    int bounds_skip_count;
    float gain_mu;
    float gain_omega;
    int buf_len;
    int nc;
    float* dl;
    float* x;
    float* y;
} ted_runtime_t;

} // namespace

static inline float farrow_cubic_eval(float s_m1, float s0, float s1, float s2, float u);

static int
ted_should_run(const ted_config_t* config, const ted_state_t* state, const float* x, const int* N, const float* y) {
    if (!config || !state || !x || !N || !y) {
        return 0;
    }
    if (!config->enabled || config->sps < 2) {
        return 0;
    }
    return ((*N >> 1) >= 4) ? 1 : 0;
}

static int
ted_disable_if_oversize(int twice_sps_required, int sps, int* N) {
    if (twice_sps_required <= TED_DL_SIZE) {
        return 0;
    }
    if (!s_warned_ted_dl_oversize) {
        DSD_FPRINTF(stderr, "[TED] disabled: required delay line %d exceeds TED_DL_SIZE=%d (sps=%d)\n",
                    twice_sps_required, TED_DL_SIZE, sps);
        s_warned_ted_dl_oversize = 1;
    }
    *N = 0;
    return 1;
}

static int
ted_required_twice_sps(float op25_omega, float omega_max) {
    int twice_sps_op25 = 2 * (int)ceilf(op25_omega);
    int twice_sps_mmse = (int)ceilf(omega_max / 2.0f) + MMSE_NTAPS + 1;
    return (twice_sps_op25 > twice_sps_mmse) ? twice_sps_op25 : twice_sps_mmse;
}

static int
ted_handle_sps_change(const ted_config_t* config, ted_state_t* state, float sps_f, float omega_rel, int* N) {
    if (state->sps <= 0 || state->sps == config->sps) {
        return 0;
    }

    state->omega = sps_f;
    state->omega_rel = omega_rel;
    state->omega_min = sps_f * (1.0f - omega_rel);
    state->omega_max = sps_f * (1.0f + omega_rel);
    state->omega_mid = 0.5f * (state->omega_min + state->omega_max);

    int twice_sps_required = ted_required_twice_sps(state->omega, state->omega_max);
    if (ted_disable_if_oversize(twice_sps_required, config->sps, N)) {
        return -1;
    }

    state->twice_sps = twice_sps_required;
    state->sps = config->sps;
    state->dl[0] = 0.0f;
    state->dl[1] = 0.0f;
    return 0;
}

static int
ted_init_if_needed(const ted_config_t* config, ted_state_t* state, float sps_f, float omega_rel, int* N) {
    if (state->omega_mid != 0.0f && state->twice_sps >= 2) {
        return 0;
    }

    state->omega = sps_f;
    state->omega_rel = omega_rel;
    state->omega_mid = sps_f;
    state->omega_min = sps_f * (1.0f - omega_rel);
    state->omega_max = sps_f * (1.0f + omega_rel);

    int twice_sps_required = ted_required_twice_sps(state->omega_max, state->omega_max);
    if (ted_disable_if_oversize(twice_sps_required, config->sps, N)) {
        return -1;
    }

    state->twice_sps = twice_sps_required;
    state->dl_index = 0;
    state->sps = config->sps;
    state->mu = (float)(twice_sps_required + 1);
    return 0;
}

static ted_runtime_t
ted_make_runtime(const ted_config_t* config, ted_state_t* state, float* x, float* y, int buf_len, int nc) {
    ted_runtime_t rt = {};
    rt.mu = state->mu;
    rt.omega = state->omega;
    rt.last_r = state->last_r;
    rt.last_j = state->last_j;
    rt.dl_index = state->dl_index;
    rt.twice_sps = state->twice_sps;
    rt.input_index = 0;
    rt.output_index = 0;
    rt.bounds_skip_count = 0;
    rt.gain_mu = config->gain_mu > 0.0f ? config->gain_mu : kDefaultGainMu;
    rt.gain_omega = config->gain_omega > 0.0f ? config->gain_omega : kDefaultGainOmega;
    rt.buf_len = buf_len;
    rt.nc = nc;
    rt.dl = state->dl;
    rt.x = x;
    rt.y = y;
    return rt;
}

static void
ted_consume_input_samples(ted_runtime_t* rt) {
    while (rt->mu > 1.0f && rt->input_index < rt->nc) {
        rt->mu -= 1.0f;
        const size_t ii = (size_t)rt->input_index;
        float in_r = rt->x[ii * 2];
        float in_j = rt->x[ii * 2 + 1];
        if (IS_NAN(in_r)) {
            in_r = 0.0f;
        }
        if (IS_NAN(in_j)) {
            in_j = 0.0f;
        }

        const size_t dl_i = (size_t)rt->dl_index;
        const size_t dl_i2 = (size_t)rt->dl_index + (size_t)rt->twice_sps;
        rt->dl[dl_i * 2] = in_r;
        rt->dl[dl_i * 2 + 1] = in_j;
        rt->dl[dl_i2 * 2] = in_r;
        rt->dl[dl_i2 * 2 + 1] = in_j;

        rt->dl_index++;
        if (rt->dl_index >= rt->twice_sps) {
            rt->dl_index = 0;
        }
        rt->input_index++;
    }
}

static void
ted_half_symbol_params(float mu, float omega, int* half_sps, float* half_mu) {
    float half_omega = omega / 2.0f;
    *half_sps = (int)floorf(half_omega);
    *half_mu = mu + half_omega - (float)(*half_sps);
    if (*half_mu > 1.0f) {
        *half_mu -= 1.0f;
        (*half_sps)++;
    }
    if (*half_sps < 0) {
        *half_sps = 0;
    }
}

static int
ted_check_interp_bounds(ted_runtime_t* rt, int half_sps) {
    int max_mid_idx = rt->dl_index + MMSE_NTAPS - 1;
    int max_sym_idx = rt->dl_index + half_sps + MMSE_NTAPS - 1;
    if (max_mid_idx < 2 * rt->twice_sps && max_sym_idx < 2 * rt->twice_sps) {
        rt->bounds_skip_count = 0;
        return 1;
    }

    rt->bounds_skip_count++;
    if (rt->bounds_skip_count > rt->nc) {
        return -1;
    }
    rt->mu += rt->omega;
    return 0;
}

static void
ted_update_lock_and_error(ted_state_t* state, float symbol_error, float sym_r, float sym_j, float mid_r, float mid_j) {
    const float kLockEps = 1e-9f;
    float ie2 = sym_r * sym_r;
    float io2 = mid_r * mid_r;
    float qe2 = sym_j * sym_j;
    float qo2 = mid_j * mid_j;
    float yi = (ie2 + io2 > kLockEps) ? (ie2 - io2) / (ie2 + io2) : 0.0f;
    float yq = (qe2 + qo2 > kLockEps) ? (qe2 - qo2) / (qe2 + qo2) : 0.0f;
    float lock_contrib = yi + yq;

    state->lock_accum += lock_contrib;
    state->lock_count++;
    if (state->lock_count >= kLockAccumWindow) {
        state->lock_accum *= 0.5f;
        state->lock_count = kLockAccumWindow / 2;
    }

    const float kEmaAlpha = 1.0f / 64.0f;
    state->e_ema = state->e_ema + kEmaAlpha * (symbol_error - state->e_ema);
}

static int
ted_process_one_symbol(ted_runtime_t* rt, ted_state_t* state) {
    int half_sps = 0;
    float half_mu = 0.0f;
    ted_half_symbol_params(rt->mu, rt->omega, &half_sps, &half_mu);

    int bounds = ted_check_interp_bounds(rt, half_sps);
    if (bounds < 0) {
        return -1;
    }
    if (bounds == 0) {
        return 0;
    }

    float mid_r = 0.0f, mid_j = 0.0f;
    mmse_interp_cc_8tap(rt->dl + (size_t)rt->dl_index * 2, rt->mu, &mid_r, &mid_j);

    float sym_r = 0.0f, sym_j = 0.0f;
    mmse_interp_cc_8tap(rt->dl + (size_t)(rt->dl_index + half_sps) * 2, half_mu, &sym_r, &sym_j);

    float error_real = (rt->last_r - sym_r) * mid_r;
    float error_imag = (rt->last_j - sym_j) * mid_j;
    float symbol_error = error_real + error_imag;
    if (IS_NAN(symbol_error)) {
        symbol_error = 0.0f;
    }
    if (symbol_error < -1.0f) {
        symbol_error = -1.0f;
    }
    if (symbol_error > 1.0f) {
        symbol_error = 1.0f;
    }

    float sym_pow = sym_r * sym_r + sym_j * sym_j;
    if (sym_pow < kOmegaNormEps) {
        sym_pow = kOmegaNormEps;
    }
    float omega_error = branchless_clip(symbol_error / sym_pow, 1.0f);
    rt->omega = rt->omega + rt->gain_omega * omega_error;
    rt->omega = state->omega_mid + branchless_clip(rt->omega - state->omega_mid, state->omega_mid * state->omega_rel);

    rt->last_r = sym_r;
    rt->last_j = sym_j;
    rt->mu += rt->omega + rt->gain_mu * symbol_error;

    ted_update_lock_and_error(state, symbol_error, sym_r, sym_j, mid_r, mid_j);

    rt->y[rt->output_index++] = sym_r;
    rt->y[rt->output_index++] = sym_j;
    return 1;
}

static void
ted_commit_runtime_state(ted_state_t* state, const ted_runtime_t* rt) {
    state->mu = rt->mu;
    state->omega = rt->omega;
    state->last_r = rt->last_r;
    state->last_j = rt->last_j;
    state->dl_index = rt->dl_index;
}

static void
ted_copy_output_to_input(float* x, const float* y, int out_n, int* N) {
    if (out_n < 2) {
        *N = 0;
        return;
    }

    for (int i = 0; i < out_n; i++) {
        x[i] = y[i];
    }
    *N = out_n;
}

static inline int
farrow_clamp_complex_index(int idx, int nc) {
    if (idx < 0) {
        return 0;
    }
    if (idx >= nc) {
        return nc - 1;
    }
    return idx;
}

static inline void
farrow_interp_complex(const float* x, int nc, int center_c, float u, float* out_r, float* out_j) {
    int c_m1 = farrow_clamp_complex_index(center_c - 1, nc);
    int c0 = farrow_clamp_complex_index(center_c, nc);
    int c1 = farrow_clamp_complex_index(center_c + 1, nc);
    int c2 = farrow_clamp_complex_index(center_c + 2, nc);

    int i_m1 = c_m1 << 1;
    int i0 = c0 << 1;
    int i1 = c1 << 1;
    int i2 = c2 << 1;

    *out_r = farrow_cubic_eval(x[i_m1], x[i0], x[i1], x[i2], u);
    *out_j = farrow_cubic_eval(x[i_m1 + 1], x[i0 + 1], x[i1 + 1], x[i2 + 1], u);
}

static inline float
farrow_wrap_mu(float mu) {
    while (mu >= 1.0f) {
        mu -= 1.0f;
    }
    while (mu < 0.0f) {
        mu += 1.0f;
    }
    return mu;
}

/**
 * @brief OP25-compatible Gardner timing recovery.
 *
 * This is a direct port of OP25's gardner_cc_impl::general_work().
 * The algorithm:
 *   1. Accumulate input samples in a delay line (doubled storage to avoid wrap)
 *   2. When mu > 1, consume a sample and advance the delay line
 *   3. Interpolate at the current symbol point and at half-symbol using 8-tap MMSE
 *   4. Compute Gardner error: (last - current) * mid for I and Q
 *   5. Update omega (symbol period) and mu (phase) with clipping
 *   6. Output the interpolated symbol sample
 *
 * @param config TED configuration (gain_mu, gain_omega, omega_rel, sps)
 * @param state  TED state (mu, omega, last_sample, delay_line)
 * @param x      Input interleaved I/Q buffer
 * @param N      Pointer to buffer length (updated with output length)
 * @param y      Output buffer for symbol-rate samples
 */
void
gardner_timing_adjust(const ted_config_t* config, ted_state_t* state, float* x, int* N, float* y) {
    if (!ted_should_run(config, state, x, N, y)) {
        return;
    }

    /* Handle SPS change (e.g., P25P1 CC @ 4800 sps → P25P2 VC @ 6000 sps).
     *
     * This matches OP25's set_omega() in gardner_cc_impl.cc exactly:
     *   - Preserves d_mu (timing phase) - OP25 doesn't touch it
     *   - Preserves d_last_sample - OP25 doesn't touch it in set_omega
     *   - Clears only first element of delay line: *d_dl = gr_complex(0,0)
     *   - Updates omega bounds and twice_sps
     *
     * OP25's set_omega() code:
     *   d_omega = omega;
     *   d_min_omega = omega*(1.0 - d_omega_rel);
     *   d_max_omega = omega*(1.0 + d_omega_rel);
     *   d_omega_mid = 0.5*(d_min_omega+d_max_omega);
     *   d_twice_sps = 2 * (int) ceilf(d_omega);
     *   *d_dl = gr_complex(0,0);  // Only first element!
     */
    const int buf_len = *N;
    const int nc = buf_len >> 1;
    const float sps_f = (float)config->sps;
    float omega_rel = config->omega_rel > 0.0f ? config->omega_rel : kDefaultOmegaRel;

    if (ted_handle_sps_change(config, state, sps_f, omega_rel, N) < 0) {
        return;
    }
    if (ted_init_if_needed(config, state, sps_f, omega_rel, N) < 0) {
        return;
    }

    ted_runtime_t rt = ted_make_runtime(config, state, x, y, buf_len, nc);
    while (rt.output_index < rt.buf_len && rt.input_index < rt.nc) {
        ted_consume_input_samples(&rt);
        if (rt.input_index >= rt.nc) {
            break;
        }

        int rc = ted_process_one_symbol(&rt, state);
        if (rc < 0) {
            break;
        }
    }

    ted_commit_runtime_state(state, &rt);
    ted_copy_output_to_input(x, y, rt.output_index, N);
}

static void
farrow_compute_guard_samples(const float* x, int nc, int a_c, int half, float u, float* lr, float* lj, float* rr,
                             float* rj) {
    int l_c = a_c - half;
    int r_c = a_c + half;
    if (l_c < 0) {
        l_c = 0;
    }
    if (r_c < 0) {
        r_c = 0;
    }
    if (l_c >= nc - 1) {
        l_c = nc - 2;
    }
    if (r_c >= nc - 1) {
        r_c = nc - 2;
    }

    farrow_interp_complex(x, nc, l_c, u, lr, lj);
    farrow_interp_complex(x, nc, r_c, u, rr, rj);
}

static float
farrow_update_mu(float mu, float mu_nom, float gain, float e_norm) {
    float corr = gain * e_norm;
    float max_corr = mu_nom * 0.5f;
    if (corr > max_corr) {
        corr = max_corr;
    }
    if (corr < -max_corr) {
        corr = -max_corr;
    }
    return farrow_wrap_mu(mu + mu_nom + corr);
}

static void
farrow_copy_output(float* x, const float* y, int out_n, int* N) {
    if (out_n < 2) {
        return;
    }
    for (int i = 0; i < out_n; i++) {
        x[i] = y[i];
    }
    *N = out_n;
}

/**
 * @brief Legacy non-decimating Gardner timing correction (Farrow-based).
 *
 * Uses cubic Farrow interpolation around the nominal samples-per-symbol to
 * reduce timing error while keeping the output at approximately the same
 * sample rate as the input (no symbol-rate decimation). This is suitable for
 * FM/C4FM paths where downstream processing expects sample-rate complex
 * baseband.
 */
static inline float
farrow_cubic_eval(float s_m1, float s0, float s1, float s2, float u) {
    /* Coefficients for cubic convolution (a = -0.5, Catmull-Rom):
       p(u) = c0 + c1*u + c2*u^2 + c3*u^3, u in [0,1]. */
    float c0 = s0;
    float c1 = 0.5f * (s1 - s_m1); /* 0.5*(s1 - s-1) */
    float c2 = s_m1 - 2.5f * s0 + 2.0f * s1 - 0.5f * s2;
    float c3 = 0.5f * (s2 - s_m1) + 1.5f * (s0 - s1);

    float u2 = u * u;
    float u3 = u2 * u;

    return ((c3 * u3) + (c2 * u2) + (c1 * u) + c0);
}

void
gardner_timing_adjust_farrow(const ted_config_t* config, ted_state_t* state, float* x, int* N, float* y) {
    if (!config || !state || !x || !N || !y) {
        return;
    }
    if (!config->enabled || config->sps <= 1) {
        return;
    }

    /* Guard: run TED only when we're near symbol rate to keep CPU low.
       Skip when samples-per-symbol is very high unless explicitly forced. */
    const int sps = config->sps;
    if (sps > 12 && !config->force) {
        return;
    }

    float mu = state->mu; /* fractional phase [0.0, 1.0) */
    float gain = config->gain_mu;
    if (gain <= 0.0f) {
        gain = 0.01f; /* conservative default for stability */
    }

    const int buf_len = *N;                 /* interleaved I/Q length */
    const float mu_nom = 1.0f / (float)sps; /* nominal advance per sample */

    if (buf_len < 6) {
        return;
    }

    const int nc = buf_len >> 1; /* complex sample count */
    const int half = sps >> 1;   /* half symbol in complex samples */

    int out_n = 0; /* interleaved I/Q index for output */

    for (int n_c = 0; n_c + 1 < nc; n_c++) {
        int a_c = n_c;
        float u = mu - floorf(mu); /* ensure [0,1) */

        float yr = 0.0f, yj = 0.0f;
        farrow_interp_complex(x, nc, a_c, u, &yr, &yj);
        y[out_n++] = yr;
        y[out_n++] = yj;

        float lr = 0.0f, lj = 0.0f, rr = 0.0f, rj = 0.0f;
        farrow_compute_guard_samples(x, nc, a_c, half, u, &lr, &lj, &rr, &rj);

        /* Gardner error: Re{ (x(+T/2) - x(-T/2)) * conj(y_mid) } */
        float dr = rr - lr;
        float dj = rj - lj;
        float e = dr * yr + dj * yj; /* instantaneous Gardner error */

        /* Normalize by instantaneous power to keep scale stable. */
        float p2 = yr * yr + yj * yj;
        if (p2 < 1e-9f) {
            p2 = 1e-9f;
        }
        float e_norm = e / p2;
        mu = farrow_update_mu(mu, mu_nom, gain, e_norm);

        /* Smooth residual using simple EMA with small weight (alpha≈1/64). */
        const float kEmaAlpha = 1.0f / 64.0f;
        state->e_ema = state->e_ema + kEmaAlpha * (e_norm - state->e_ema);
    }

    farrow_copy_output(x, y, out_n, N);
    state->mu = mu;
}
