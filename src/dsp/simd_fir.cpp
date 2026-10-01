// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief SIMD FIR dispatch + scalar fallbacks + CPU detection.
 *
 * Implements runtime dispatch for FIR filter functions. Scalar implementations
 * serve as reference and fallback. CPU detection selects best available SIMD.
 */

#include <atomic>
#include <cstring>
#include <dsd-neo/dsp/simd_fir.h>
#include "simd_fir_internal.h"
#include "simd_x86_cpu.h"

#if defined(__x86_64__) || defined(_M_X64)
extern "C" int simd_fir_complex_apply_sse2(const float* in, int n_in, float* out, int out_cap, float* hist_i,
                                           float* hist_q, int hist_len, int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_complex_sse2(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                           int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_real_sse2(const float* in, int in_len, float* out, float* hist, int* pending,
                                        const float* taps, int taps_len);
#if defined(DSD_NEO_DSP_HAVE_AVX2_IMPL) && DSD_NEO_X86_AVX2_RUNTIME_PROBE_SUPPORTED
extern "C" int simd_fir_complex_apply_avx2(const float* in, int n_in, float* out, int out_cap, float* hist_i,
                                           float* hist_q, int hist_len, int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_complex_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                           int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_real_avx2(const float* in, int in_len, float* out, float* hist, int* pending,
                                        const float* taps, int taps_len);
#endif
#endif

#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
extern "C" int simd_fir_complex_apply_neon(const float* in, int n_in, float* out, int out_cap, float* hist_i,
                                           float* hist_q, int hist_len, int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_complex_neon(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                           int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_real_neon(const float* in, int in_len, float* out, float* hist, int* pending,
                                        const float* taps, int taps_len);
#endif

/* -------------------------------------------------------------------------- */
/* Scalar Reference Implementations                                           */
/* -------------------------------------------------------------------------- */

/**
 * Scalar complex symmetric FIR filter (no decimation), streaming: output n is centred at logical index
 * hist_len - pending + n of [history | block], so a call makes only the outputs whose look-ahead it holds.
 * Exploits tap symmetry: acc += tap[k] * (x[center-d] + x[center+d]).
 */
int
simd_fir_complex_apply_scalar(const float* in, int n_in, float* out, int out_cap, float* hist_i, float* hist_q,
                              int hist_len, int* pending, const float* taps, int taps_len) {
    const int outputs =
        simd_fir_complex_outputs(in, n_in, out, out_cap, hist_i, hist_q, hist_len, pending, taps, taps_len);
    if (outputs < 0) {
        return -1;
    }

    const int center = (taps_len - 1) >> 1;
    const int first_center = hist_len - *pending;

    /* Lambda to fetch sample from history or input */
    auto get_iq = [&](int src_idx, float& xi, float& xq) {
        if (src_idx < hist_len) {
            xi = hist_i[src_idx];
            xq = hist_q[src_idx];
        } else {
            const size_t rel = (size_t)(src_idx - hist_len);
            xi = in[2 * rel];
            xq = in[2 * rel + 1];
        }
    };

    for (int n = 0; n < outputs; n++) {
        int center_idx = first_center + n;
        float accI = 0.0f;
        float accQ = 0.0f;

        /* Center tap */
        float ci, cq;
        get_iq(center_idx, ci, cq);
        float cc = taps[center];
        accI += cc * ci;
        accQ += cc * cq;

        /* Symmetric pairs */
        for (int k = 0; k < center; k++) {
            float ce = taps[k];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - k;
            float xmI, xmQ, xpI, xpQ;
            get_iq(center_idx - d, xmI, xmQ);
            get_iq(center_idx + d, xpI, xpQ);
            accI += ce * (xmI + xpI);
            accQ += ce * (xmQ + xpQ);
        }

        out[n << 1] = accI;
        out[(n << 1) + 1] = accQ;
    }

    *pending += n_in - outputs;
    simd_fir_complex_push_history(in, n_in, hist_i, hist_q, hist_len);
    return outputs;
}

/**
 * Scalar complex half-band decimator by 2, streaming (simd_hb_decim2_complex()): output n is centred at logical index
 * (taps_len - 1) - pending + 2n of [history | block], so a call makes only the outputs whose look-ahead it holds and
 * carries the decimation phase in pending.
 * Exploits zero-valued odd taps (except center) AND tap symmetry.
 */
int
simd_hb_decim2_complex_scalar(const float* in, int in_len, float* out, float* hist_i, float* hist_q, int* pending,
                              const float* taps, int taps_len) {
    const int outputs = simd_hb_decim2_complex_outputs(in, in_len, out, hist_i, hist_q, pending, taps, taps_len);
    if (outputs < 0) {
        return -1;
    }

    const int n_in = in_len >> 1; /* complex samples */
    const int center = (taps_len - 1) >> 1;
    const int left_len = taps_len - 1;
    const int first_center = left_len - *pending;

    auto get_iq = [&](int src_idx, float& xi, float& xq) {
        if (src_idx < left_len) {
            xi = hist_i[src_idx];
            xq = hist_q[src_idx];
        } else {
            const size_t rel = (size_t)(src_idx - left_len);
            xi = in[2 * rel];
            xq = in[2 * rel + 1];
        }
    };

    for (int n = 0; n < outputs; n++) {
        int center_idx = first_center + (n << 1);
        float accI = 0.0f;
        float accQ = 0.0f;

        /* Center tap */
        float ci, cq;
        get_iq(center_idx, ci, cq);
        float cc = taps[center];
        accI += cc * ci;
        accQ += cc * cq;

        /* Symmetric pairs: only even indices have non-zero taps in half-band */
        for (int e = 0; e < center; e += 2) {
            float ce = taps[e];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - e;
            float xmI, xmQ, xpI, xpQ;
            get_iq(center_idx - d, xmI, xmQ);
            get_iq(center_idx + d, xpI, xpQ);
            accI += ce * (xmI + xpI);
            accQ += ce * (xmQ + xpQ);
        }

        out[n << 1] = accI;
        out[(n << 1) + 1] = accQ;
    }

    *pending += n_in - (outputs << 1);
    simd_fir_complex_push_history(in, n_in, hist_i, hist_q, left_len);
    return outputs << 1;
}

/**
 * Scalar real half-band decimator by 2, streaming (simd_hb_decim2_real()): the complex decimator's contract on real
 * samples.
 * Exploits zero-valued odd taps (except center) AND tap symmetry.
 */
int
simd_hb_decim2_real_scalar(const float* in, int in_len, float* out, float* hist, int* pending, const float* taps,
                           int taps_len) {
    const int outputs = simd_hb_decim2_real_outputs(in, in_len, out, hist, pending, taps, taps_len);
    if (outputs < 0) {
        return -1;
    }

    const int hist_len = taps_len - 1;
    const int center = (taps_len - 1) >> 1;
    const int first_center = hist_len - *pending;

    auto get_sample = [&](int src_idx) -> float {
        return (src_idx < hist_len) ? hist[src_idx] : in[src_idx - hist_len];
    };

    for (int n = 0; n < outputs; n++) {
        int center_idx = first_center + (n << 1);
        float acc = 0.0f;

        /* Center tap */
        acc += taps[center] * get_sample(center_idx);

        /* Symmetric pairs: only even indices have non-zero taps */
        for (int e = 0; e < center; e += 2) {
            float ce = taps[e];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - e;
            float xm = get_sample(center_idx - d);
            float xp = get_sample(center_idx + d);
            acc += ce * (xm + xp);
        }

        out[n] = acc;
    }

    *pending += in_len - (outputs << 1);
    simd_hb_real_push_history(in, in_len, hist, hist_len);
    return outputs;
}

/* -------------------------------------------------------------------------- */
/* Function Pointer Dispatch                                                  */
/* -------------------------------------------------------------------------- */

using fir_complex_fn = int (*)(const float*, int, float*, int, float*, float*, int, int*, const float*, int);
using hb_decim2_complex_fn = int (*)(const float*, int, float*, float*, float*, int*, const float*, int);
using hb_decim2_real_fn = int (*)(const float*, int, float*, float*, int*, const float*, int);

static fir_complex_fn g_fir_complex_impl = simd_fir_complex_apply_scalar;
static hb_decim2_complex_fn g_hb_decim2_complex_impl = simd_hb_decim2_complex_scalar;
static hb_decim2_real_fn g_hb_decim2_real_impl = simd_hb_decim2_real_scalar;
static const char* g_impl_name = "scalar";

/* Dispatch init state: 0 = not started, 1 = in progress, 2 = done */
static std::atomic<int> g_fir_init_done{0};

static inline bool
simd_fir_prefer_scalar_for_block(int in_len, int taps_len) {
    return in_len > 0 && taps_len > 0 && in_len < taps_len * 2;
}

static void
simd_fir_init_dispatch() {
    int expected = 0;
    if (!g_fir_init_done.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        /* Another thread is initializing; spin until done */
        while (g_fir_init_done.load(std::memory_order_acquire) != 2) {
            /* spin */
        }
        return;
    }

    /* Perform one-time initialization */
#if defined(__x86_64__) || defined(_M_X64)
#if defined(DSD_NEO_DSP_HAVE_AVX2_IMPL) && DSD_NEO_X86_AVX2_RUNTIME_PROBE_SUPPORTED
    if (dsd_neo_cpu_has_avx2_with_os_support()) {
        g_fir_complex_impl = simd_fir_complex_apply_avx2;
        g_hb_decim2_complex_impl = simd_hb_decim2_complex_avx2;
        g_hb_decim2_real_impl = simd_hb_decim2_real_avx2;
        g_impl_name = "avx2";
    } else
#endif
    {
        g_fir_complex_impl = simd_fir_complex_apply_sse2;
        g_hb_decim2_complex_impl = simd_hb_decim2_complex_sse2;
        g_hb_decim2_real_impl = simd_hb_decim2_real_sse2;
        g_impl_name = "sse2";
    }
#elif defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    g_fir_complex_impl = simd_fir_complex_apply_neon;
    g_hb_decim2_complex_impl = simd_hb_decim2_complex_neon;
    g_hb_decim2_real_impl = simd_hb_decim2_real_neon;
    g_impl_name = "neon";
#else
    /* Already set to scalar */
#endif

    g_fir_init_done.store(2, std::memory_order_release);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

extern "C" int
simd_fir_complex_apply(const float* in, int n_in, float* out, int out_cap, float* hist_i, float* hist_q, int hist_len,
                       int* pending, const float* taps, int taps_len) {
    /* Blocks shorter than the filter stay scalar, as they did when this counted floats (in_len < 2 * taps_len). */
    if (n_in > 0 && taps_len > 0 && n_in < taps_len) {
        return simd_fir_complex_apply_scalar(in, n_in, out, out_cap, hist_i, hist_q, hist_len, pending, taps, taps_len);
    }
    if (g_fir_init_done.load(std::memory_order_acquire) != 2) {
        simd_fir_init_dispatch();
    }
    return g_fir_complex_impl(in, n_in, out, out_cap, hist_i, hist_q, hist_len, pending, taps, taps_len);
}

extern "C" int
simd_hb_decim2_complex(const float* in, int in_len, float* out, float* hist_i, float* hist_q, int* pending,
                       const float* taps, int taps_len) {
    if (simd_fir_prefer_scalar_for_block(in_len, taps_len)) {
        return simd_hb_decim2_complex_scalar(in, in_len, out, hist_i, hist_q, pending, taps, taps_len);
    }
    if (g_fir_init_done.load(std::memory_order_acquire) != 2) {
        simd_fir_init_dispatch();
    }
    return g_hb_decim2_complex_impl(in, in_len, out, hist_i, hist_q, pending, taps, taps_len);
}

extern "C" int
simd_hb_decim2_real(const float* in, int in_len, float* out, float* hist, int* pending, const float* taps,
                    int taps_len) {
    if (simd_fir_prefer_scalar_for_block(in_len, taps_len)) {
        return simd_hb_decim2_real_scalar(in, in_len, out, hist, pending, taps, taps_len);
    }
    if (g_fir_init_done.load(std::memory_order_acquire) != 2) {
        simd_fir_init_dispatch();
    }
    return g_hb_decim2_real_impl(in, in_len, out, hist, pending, taps, taps_len);
}

extern "C" const char*
simd_fir_get_impl_name(void) {
    if (g_fir_init_done.load(std::memory_order_acquire) != 2) {
        simd_fir_init_dispatch();
    }
    return g_impl_name;
}
