// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Polyphase rational resampler implementation (L/M) using float samples.
 *
 * Simplified scalar upfirdn implementation used by the FM audio path.
 */

#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/runtime/mem.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

#if defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_PRAGMA(x) _Pragma(#x)
#define DSD_NEO_IVDEP     DSD_NEO_PRAGMA(GCC ivdep)
#else
#define DSD_NEO_IVDEP
#endif

#ifndef DSD_NEO_ALIGN
#define DSD_NEO_ALIGN 64
#endif

#if defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_RESTRICT __restrict__
#else
#define DSD_NEO_RESTRICT
#endif

static const double kPi = 3.14159265358979323846;
static const int kDefaultTapsPerPhase = 16;
static const uint64_t kResamplerStateCookie = 0x4453444e454f5253ULL;

template <typename T>
static inline T*
assume_aligned_ptr(T* p, size_t /*align_unused*/) {
#if defined(__GNUC__) || defined(__clang__)
    return (T*)__builtin_assume_aligned(p, 64);
#else
    return p;
#endif
}

template <typename T>
static inline const T*
assume_aligned_ptr(const T* p, size_t /*align_unused*/) {
#if defined(__GNUC__) || defined(__clang__)
    return (const T*)__builtin_assume_aligned(p, 64);
#else
    return p;
#endif
}

static inline double
dsd_neo_sinc(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    return sin(kPi * x) / (kPi * x);
}

static inline float
resamp_dot_contiguous(const float* DSD_NEO_RESTRICT samples, const float* DSD_NEO_RESTRICT taps, int len) {
    float acc = 0.0f;
    DSD_NEO_IVDEP
    for (int k = 0; k < len; k++) {
        acc += samples[k] * taps[k];
    }
    return acc;
}

static inline float
resamp_dot16_contiguous(const float* DSD_NEO_RESTRICT samples, const float* DSD_NEO_RESTRICT taps) {
    float acc0 = 0.0f;
    float acc1 = 0.0f;
    float acc2 = 0.0f;
    float acc3 = 0.0f;

    DSD_NEO_IVDEP
    for (int k = 0; k < 16; k += 4) {
        acc0 += samples[k + 0] * taps[k + 0];
        acc1 += samples[k + 1] * taps[k + 1];
        acc2 += samples[k + 2] * taps[k + 2];
        acc3 += samples[k + 3] * taps[k + 3];
    }

    return (acc0 + acc1) + (acc2 + acc3);
}

static inline void
dsd_resampler_state_init_defaults(dsd_resampler_state* state) {
    if (!state) {
        return;
    }
    state->enabled = 0;
    state->target_hz = 0;
    state->L = 1;
    state->M = 1;
    state->phase = 0;
    state->taps_len = 0;
    state->taps_per_phase = 0;
    state->hist_head = 0;
    state->taps = NULL;
    state->hist = NULL;
    state->internal_cookie = kResamplerStateCookie;
}

static inline int
dsd_resampler_state_is_initialized(const dsd_resampler_state* state) {
    if (!state) {
        return 0;
    }
    uint64_t cookie = 0;
    DSD_MEMCPY(&cookie, &state->internal_cookie, sizeof(cookie));
    return cookie == kResamplerStateCookie;
}

static int
dsd_resampler_required_out_len(int in_len, int L, int M, int phase) {
    if (in_len < 0 || L < 1 || M < 1 || phase < 0) {
        return -1;
    }

    int64_t numerator = (int64_t)in_len * (int64_t)L + (int64_t)M - 1 - (int64_t)phase;
    if (numerator <= 0) {
        return 0;
    }

    int64_t out_len = numerator / (int64_t)M;
    return (out_len > INT_MAX) ? -1 : (int)out_len;
}

static inline void
resampler_free_buffers(float* taps, float* hist) {
    if (taps) {
        dsd_neo_aligned_free(taps);
    }
    if (hist) {
        dsd_neo_aligned_free(hist);
    }
}

static int
resampler_alloc_buffers(int total_taps, int taps_per_phase, float** out_taps, float** out_hist) {
    float* taps = static_cast<float*>(dsd_neo_aligned_malloc((size_t)total_taps * sizeof(float)));
    float* hist = static_cast<float*>(dsd_neo_aligned_malloc((size_t)taps_per_phase * 2U * sizeof(float)));
    if (!taps || !hist) {
        resampler_free_buffers(taps, hist);
        *out_taps = NULL;
        *out_hist = NULL;
        return 0;
    }
    DSD_MEMSET(hist, 0, (size_t)taps_per_phase * 2U * sizeof(float));
    *out_taps = taps;
    *out_hist = hist;
    return 1;
}

static void
resampler_design_taps(float* taps, int L, int taps_per_phase, int total_taps, double fc) {
    const int mid = (total_taps - 1) / 2;
    double gain = 0.0;
    for (int n = 0; n < total_taps; n++) {
        const int m = n - mid;
        const double w = 0.54 - 0.46 * cos(2.0 * kPi * (double)n / (double)(total_taps - 1));
        const double h = 2.0 * fc * dsd_neo_sinc(2.0 * fc * (double)m);
        gain += h * w;
    }
    if (gain == 0.0) {
        gain = 1.0;
    }

    const double phase_gain_comp = (double)L;
    for (int phase = 0; phase < L; phase++) {
        float* phase_taps = taps + (size_t)phase * (size_t)taps_per_phase;
        for (int k = 0; k < taps_per_phase; k++) {
            const int src_index = phase + ((taps_per_phase - 1 - k) * L);
            const int m = src_index - mid;
            const double w = 0.54 - 0.46 * cos(2.0 * kPi * (double)src_index / (double)(total_taps - 1));
            const double h = 2.0 * fc * dsd_neo_sinc(2.0 * fc * (double)m);
            phase_taps[k] = (float)((h * w / gain) * phase_gain_comp);
        }
    }
}

static inline int
resampler_passthrough(const float* DSD_NEO_RESTRICT in, int in_len, float* DSD_NEO_RESTRICT out, int out_cap) {
    if (out_cap < in_len) {
        return -1;
    }
    if (out != in) {
        DSD_MEMCPY(out, in, (size_t)in_len * sizeof(float));
    }
    return in_len;
}

static inline int
resampler_push_sample(float* DSD_NEO_RESTRICT hist, int taps_per_phase, int head, float sample) {
    hist[head] = sample;
    hist[head + taps_per_phase] = sample;
    head++;
    if (head == taps_per_phase) {
        head = 0;
    }
    return head;
}

static inline void
resampler_emit_outputs(const float* DSD_NEO_RESTRICT hist_window, const float* DSD_NEO_RESTRICT taps_al,
                       int taps_per_phase, int L, int M, int* phase, float* DSD_NEO_RESTRICT out, int* out_len) {
    int local_phase = *phase;
    while (local_phase < L) {
        const float* DSD_NEO_RESTRICT phase_taps = taps_al + (size_t)local_phase * (size_t)taps_per_phase;
        const float acc = (taps_per_phase == kDefaultTapsPerPhase)
                              ? resamp_dot16_contiguous(hist_window, phase_taps)
                              : resamp_dot_contiguous(hist_window, phase_taps, taps_per_phase);
        out[(*out_len)++] = acc;
        local_phase += M;
    }
    *phase = local_phase - L;
}

void
dsd_resampler_reset(dsd_resampler_state* state) {
    if (!state) {
        return;
    }

    if (dsd_resampler_state_is_initialized(state)) {
        if (state->taps) {
            dsd_neo_aligned_free(state->taps);
        }
        if (state->hist) {
            dsd_neo_aligned_free(state->hist);
        }
    }

    dsd_resampler_state_init_defaults(state);
}

void
dsd_resampler_clear_history(dsd_resampler_state* state) {
    if (!state) {
        return;
    }

    if (!dsd_resampler_state_is_initialized(state)) {
        dsd_resampler_state_init_defaults(state);
        return;
    }

    state->phase = 0;
    state->hist_head = 0;
    if (state->hist && state->taps_per_phase > 0) {
        DSD_MEMSET(state->hist, 0, (size_t)state->taps_per_phase * 2U * sizeof(float));
    }
}

int
dsd_resampler_design(dsd_resampler_state* state, int L, int M) {
    if (!state || L < 1 || M < 1) {
        if (state) {
            dsd_resampler_reset(state);
        }
        return 0;
    }

    const int state_initialized = dsd_resampler_state_is_initialized(state);
    const int target_hz = state_initialized ? state->target_hz : 0;
    float* old_taps = state_initialized ? state->taps : NULL;
    float* old_hist = state_initialized ? state->hist : NULL;

    int taps_per_phase = (kDefaultTapsPerPhase < 8) ? 8 : kDefaultTapsPerPhase; /* K */
    int total_taps = taps_per_phase * L;
    if (total_taps < L) {
        total_taps = L;
    }

    const double fc = 0.45 / (double)((L > M) ? L : M);
    float* new_taps = NULL;
    float* new_hist = NULL;
    if (!resampler_alloc_buffers(total_taps, taps_per_phase, &new_taps, &new_hist)) {
        if (!state_initialized) {
            dsd_resampler_state_init_defaults(state);
        }
        return 0;
    }
    resampler_design_taps(new_taps, L, taps_per_phase, total_taps, fc);

    dsd_resampler_state next_state;
    dsd_resampler_state_init_defaults(&next_state);
    next_state.target_hz = target_hz;
    next_state.L = L;
    next_state.M = M;
    next_state.phase = 0;
    next_state.taps_len = total_taps;
    next_state.taps_per_phase = taps_per_phase;
    next_state.hist_head = 0;
    next_state.taps = new_taps;
    next_state.hist = new_hist;
    next_state.enabled = 1;

    resampler_free_buffers(old_taps, old_hist);

    *state = next_state;
    return 1;
}

int
dsd_resampler_process_block(dsd_resampler_state* state, const float* DSD_NEO_RESTRICT in, int in_len,
                            float* DSD_NEO_RESTRICT out, int out_cap) {
    if (!state || !in || in_len < 0 || !out || out_cap < 0) {
        return -1;
    }

    if (!dsd_resampler_state_is_initialized(state)) {
        dsd_resampler_state_init_defaults(state);
    }

    if (!state->enabled || !state->taps || !state->hist) {
        return resampler_passthrough(in, in_len, out, out_cap);
    }
    const int L = state->L;
    const int M = state->M;
    const int K = state->taps_per_phase;
    const float* DSD_NEO_RESTRICT taps_al = assume_aligned_ptr(state->taps, DSD_NEO_ALIGN);
    int phase = state->phase;
    int head = state->hist_head;
    int out_len = 0;
    /* Generic callers may pass ordinary stack or struct buffers here. */
    float* DSD_NEO_RESTRICT hist = assume_aligned_ptr(state->hist, DSD_NEO_ALIGN);
    const int required_out = dsd_resampler_required_out_len(in_len, L, M, phase);

    if (required_out < 0 || required_out > out_cap) {
        return -1;
    }

    for (int n = 0; n < in_len; n++) {
        head = resampler_push_sample(hist, K, head, in[n]);
        const float* DSD_NEO_RESTRICT hist_window = hist + head;
        resampler_emit_outputs(hist_window, taps_al, K, L, M, &phase, out, &out_len);
    }

    state->phase = phase;
    state->hist_head = head;
    return out_len;
}

static dsd_resampler_state
demod_resampler_state_copy_in(const struct demod_state* demod) {
    dsd_resampler_state state{};
    dsd_resampler_state_init_defaults(&state);
    if (!demod) {
        return state;
    }
    state.enabled = demod->resamp_enabled;
    state.target_hz = demod->resamp_target_hz;
    state.L = demod->resamp_L;
    state.M = demod->resamp_M;
    state.phase = demod->resamp_phase;
    state.taps_len = demod->resamp_taps_len;
    state.taps_per_phase = demod->resamp_taps_per_phase;
    state.hist_head = demod->resamp_hist_head;
    state.taps = demod->resamp_taps;
    state.hist = demod->resamp_hist;
    return state;
}

static void
demod_resampler_state_copy_out(struct demod_state* demod, const dsd_resampler_state* state) {
    if (!demod || !state) {
        return;
    }
    demod->resamp_enabled = state->enabled;
    demod->resamp_target_hz = state->target_hz;
    demod->resamp_L = state->L;
    demod->resamp_M = state->M;
    demod->resamp_phase = state->phase;
    demod->resamp_taps_len = state->taps_len;
    demod->resamp_taps_per_phase = state->taps_per_phase;
    demod->resamp_hist_head = state->hist_head;
    demod->resamp_taps = state->taps;
    demod->resamp_hist = state->hist;
}

void
resamp_design(struct demod_state* s, int L, int M) {
    if (!s) {
        return;
    }
    dsd_resampler_state state = demod_resampler_state_copy_in(s);
    (void)dsd_resampler_design(&state, L, M);
    demod_resampler_state_copy_out(s, &state);
}

int
resamp_process_block(struct demod_state* s, const float* DSD_NEO_RESTRICT in, int in_len, float* DSD_NEO_RESTRICT out) {
    if (!s) {
        return -1;
    }
    dsd_resampler_state state = demod_resampler_state_copy_in(s);
    int out_len = dsd_resampler_process_block(&state, in, in_len, out, MAXIMUM_BUF_LENGTH * 4);
    demod_resampler_state_copy_out(s, &state);
    if (out_len >= 0) {
        return out_len;
    }
    if (out != in && in && out && in_len > 0) {
        DSD_MEMCPY(out, in, (size_t)in_len * sizeof(float));
    }
    return in_len;
}
