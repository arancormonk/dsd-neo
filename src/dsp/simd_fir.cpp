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
#include <dsd-neo/dsp/simd_fir.h>
#include <stddef.h>
#include <vector>
#include "simd_fir_internal.h"
#include "simd_x86_cpu.h"

float*
simd_fir_tls_scratch_iq(size_t len) {
    static thread_local std::vector<float> scratch;
    if (scratch.size() < len) {
        scratch.resize(len);
    }
    return scratch.data();
}

float*
simd_fir_tls_scratch_real(size_t len) {
    static thread_local std::vector<float> scratch;
    if (scratch.size() < len) {
        scratch.resize(len);
    }
    return scratch.data();
}

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

/*
 * One output of the scalar complex FIR, centred at @p c of the contiguous window @p x (interleaved I/Q): the centre tap,
 * then each symmetric pair from the outermost in, acc += tap[k] * (x[c-d] + x[c+d]), skipping zero taps.
 */
static inline void
fir_complex_scalar_one(const float* x, int c, const float* taps, int center, float* out) {
    float acc_i = 0.0f;
    float acc_q = 0.0f;
    const float* xc = x + ((size_t)c << 1);
    acc_i += taps[center] * xc[0];
    acc_q += taps[center] * xc[1];
    for (int k = 0; k < center; k++) {
        const float ce = taps[k];
        if (ce == 0.0f) {
            continue;
        }
        const size_t d = (size_t)(center - k) << 1;
        const float* m = xc - d;
        const float* p = xc + d;
        acc_i += ce * (m[0] + p[0]);
        acc_q += ce * (m[1] + p[1]);
    }
    out[0] = acc_i;
    out[1] = acc_q;
}

/**
 * Scalar complex symmetric FIR filter (no decimation), streaming: output n is centred at logical index
 * hist_len - pending + n of [history | block], so a call makes only the outputs whose look-ahead it holds.
 * Exploits tap symmetry: acc += tap[k] * (x[center-d] + x[center+d]).
 *
 * The windows are read from one contiguous copy of the history's last pending + center samples and the block, and four
 * outputs run side by side, each summed in the order a single one is (fir_complex_scalar_one()): the four independent
 * sums overlap where one would wait on its own adds, and the results are the same bits.
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
    if (outputs > 0) {
        /* Output n is centred at scratch index center + n; the last window ends on the block's last sample. */
        const int take = *pending + center;
        float* x = simd_fir_tls_scratch_iq(((size_t)take + (size_t)n_in) * 2U);
        const int first = hist_len - take;
        for (int k = 0; k < take; k++) {
            x[(size_t)k * 2U] = hist_i[first + k];
            x[((size_t)k * 2U) + 1U] = hist_q[first + k];
        }
        if (n_in > 0) {
            DSD_MEMCPY(x + (size_t)take * 2U, in, (size_t)n_in * 2U * sizeof(float));
        }
        const float cc = taps[center];
        int n = 0;
        for (; n + 3 < outputs; n += 4) {
            const float* x0 = x + ((size_t)(center + n) << 1);
            float i0 = 0.0f;
            float q0 = 0.0f;
            float i1 = 0.0f;
            float q1 = 0.0f;
            float i2 = 0.0f;
            float q2 = 0.0f;
            float i3 = 0.0f;
            float q3 = 0.0f;
            i0 += cc * x0[0];
            q0 += cc * x0[1];
            i1 += cc * x0[2];
            q1 += cc * x0[3];
            i2 += cc * x0[4];
            q2 += cc * x0[5];
            i3 += cc * x0[6];
            q3 += cc * x0[7];
            for (int k = 0; k < center; k++) {
                const float ce = taps[k];
                if (ce == 0.0f) {
                    continue;
                }
                const size_t d = (size_t)(center - k) << 1;
                const float* m = x0 - d;
                const float* p = x0 + d;
                i0 += ce * (m[0] + p[0]);
                q0 += ce * (m[1] + p[1]);
                i1 += ce * (m[2] + p[2]);
                q1 += ce * (m[3] + p[3]);
                i2 += ce * (m[4] + p[4]);
                q2 += ce * (m[5] + p[5]);
                i3 += ce * (m[6] + p[6]);
                q3 += ce * (m[7] + p[7]);
            }
            float* o = out + ((size_t)n << 1);
            o[0] = i0;
            o[1] = q0;
            o[2] = i1;
            o[3] = q1;
            o[4] = i2;
            o[5] = q2;
            o[6] = i3;
            o[7] = q3;
        }
        for (; n < outputs; n++) {
            fir_complex_scalar_one(x, center + n, taps, center, out + ((size_t)n << 1));
        }
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
    /* Avoid scratch preparation for tiny blocks. Longer blocks use SIMD even when the window spans history. */
    if (n_in > 0 && n_in < 16 && n_in < taps_len) {
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
