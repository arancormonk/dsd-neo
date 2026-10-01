// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief SSE2 implementations of SIMD FIR filter functions.
 *
 * Compiled with -msse2 flag. Processes 4 floats at a time using 128-bit XMM
 * registers. Uses scalar epilogue for remaining samples.
 */

#include <cstring>
#include <vector>
#include <xmmintrin.h>
#include "dsd-neo/core/safe_api.h"
#include "simd_fir_internal.h"

// NOLINTBEGIN(portability-simd-intrinsics)

namespace {

struct ComplexIq {
    float i;
    float q;
};

/* A half-band window's samples: relative index rel of the block, negative ones reaching into the history. */
struct HbComplexBoundary {
    const float* in;
    const float* hist_i;
    const float* hist_q;
    int hist_len;
};

static thread_local std::vector<float> tls_scratch_iq;
static thread_local std::vector<float> tls_scratch_real;

/* The streaming FIR's windows: the history's last @p take samples, then the block, as interleaved I/Q. */
static float*
prepare_fir_scratch(const float* in, int n_in, const float* hist_i, const float* hist_q, int hist_len, int take) {
    const size_t scratch_len = ((size_t)take + (size_t)n_in) * 2U;
    if (tls_scratch_iq.size() < scratch_len) {
        tls_scratch_iq.resize(scratch_len);
    }

    float* scratch = tls_scratch_iq.data();
    const int first = hist_len - take;
    for (int k = 0; k < take; k++) {
        const size_t kk = (size_t)k;
        scratch[2 * kk] = hist_i[first + k];
        scratch[2 * kk + 1] = hist_q[first + k];
    }
    if (n_in > 0) {
        DSD_MEMCPY(scratch + (size_t)take * 2, in, (size_t)n_in * 2 * sizeof(float));
    }
    return scratch;
}

/* The real half-band decimator's windows: the history's last @p take samples, then the block. */
static float*
prepare_real_scratch(const float* in, int n_in, const float* hist, int hist_len, int take) {
    const size_t scratch_len = (size_t)take + (size_t)n_in;
    if (tls_scratch_real.size() < scratch_len) {
        tls_scratch_real.resize(scratch_len);
    }

    float* scratch = tls_scratch_real.data();
    DSD_MEMCPY(scratch, hist + (hist_len - take), (size_t)take * sizeof(float));
    if (n_in > 0) {
        DSD_MEMCPY(scratch + take, in, (size_t)n_in * sizeof(float));
    }
    return scratch;
}

/* Load complex samples at indices base and base+2 from contiguous unaligned loads. */
static inline __m128
load_iq_stride2_2(const float* base) {
    const __m128 lo = _mm_loadu_ps(base);
    const __m128 hi = _mm_loadu_ps(base + 4);
    return _mm_movelh_ps(lo, hi);
}

static inline ComplexIq
load_hb_complex_boundary(const HbComplexBoundary& source, int rel) {
    if (rel < 0) {
        const int hist_idx = source.hist_len + rel;
        return {source.hist_i[hist_idx], source.hist_q[hist_idx]};
    }
    return {source.in[rel << 1], source.in[(rel << 1) + 1]};
}

template <int SideIndex, int SideCount>
struct HbComplexFixedScalarSide {
    static inline void
    apply(const HbComplexBoundary& source, const float* taps, int center_rel, float& acc_i, float& acc_q) {
        constexpr int even_tap = SideIndex << 1;
        constexpr int center = (SideCount << 1) - 1;
        const float coefficient = taps[even_tap];
        if (coefficient != 0.0f) {
            constexpr int distance = center - even_tap;
            const ComplexIq minus = load_hb_complex_boundary(source, center_rel - distance);
            const ComplexIq plus = load_hb_complex_boundary(source, center_rel + distance);
            acc_i += coefficient * (minus.i + plus.i);
            acc_q += coefficient * (minus.q + plus.q);
        }
        HbComplexFixedScalarSide<SideIndex + 1, SideCount>::apply(source, taps, center_rel, acc_i, acc_q);
    }
};

template <int SideCount>
struct HbComplexFixedScalarSide<SideCount, SideCount> {
    static inline void
    apply(const HbComplexBoundary&, const float*, int, float&, float&) {}
};

/* One output centred at relative index @p center_rel, which may lie in the history (center_rel < 0 once pending > 0). */
template <int TapsLen>
static inline ComplexIq
hb_complex_fixed_accumulate_scalar(const HbComplexBoundary& source, const float* taps, int center_rel) {
    constexpr int center = (TapsLen - 1) >> 1;
    const ComplexIq center_sample = load_hb_complex_boundary(source, center_rel);
    ComplexIq acc = {taps[center] * center_sample.i, taps[center] * center_sample.q};
    HbComplexFixedScalarSide<0, (TapsLen + 1) / 4>::apply(source, taps, center_rel, acc.i, acc.q);
    return acc;
}

template <int SideIndex, int SideCount>
struct HbComplexFixedVectorSide {
    static inline void
    apply(const float* center_base, const float* taps, __m128& acc0, __m128& acc1) {
        constexpr int even_tap = SideIndex << 1;
        constexpr int center = (SideCount << 1) - 1;
        constexpr int distance_floats = (center - even_tap) << 1;
        const float coefficient = taps[even_tap];
        if (coefficient != 0.0f) {
            const float* minus = center_base - distance_floats;
            const float* plus = center_base + distance_floats;
            const __m128 sum0 = _mm_add_ps(load_iq_stride2_2(minus), load_iq_stride2_2(plus));
            const __m128 sum1 = _mm_add_ps(load_iq_stride2_2(minus + 8), load_iq_stride2_2(plus + 8));
            const __m128 tap = _mm_set1_ps(coefficient);
            acc0 = _mm_add_ps(acc0, _mm_mul_ps(tap, sum0));
            acc1 = _mm_add_ps(acc1, _mm_mul_ps(tap, sum1));
        }
        HbComplexFixedVectorSide<SideIndex + 1, SideCount>::apply(center_base, taps, acc0, acc1);
    }
};

template <int SideCount>
struct HbComplexFixedVectorSide<SideCount, SideCount> {
    static inline void
    apply(const float*, const float*, __m128&, __m128&) {}
};

/* @p outputs outputs from a block of @p n_in samples at @p pending: output n centred at centre_rel = 2n - pending. */
template <int TapsLen>
static void
hb_complex_decim2_fixed(const float* in, int n_in, float* out, int outputs, const float* hist_i, const float* hist_q,
                        int pending, const float* taps) {
    static_assert(TapsLen == 15 || TapsLen == 31, "fixed half-band kernel supports 15 or 31 taps");
    constexpr int center = (TapsLen - 1) >> 1;
    const HbComplexBoundary source = {in, hist_i, hist_q, TapsLen - 1};

    /* Prefix outputs whose window starts in the history stay on the boundary path. */
    int n = 0;
    for (; n < outputs && (n << 1) - pending < center; n++) {
        const ComplexIq acc = hb_complex_fixed_accumulate_scalar<TapsLen>(source, taps, (n << 1) - pending);
        out[n << 1] = acc.i;
        out[(n << 1) + 1] = acc.q;
    }

    /* Four outputs use two XMM accumulators; guard each helper's complete load footprint, which ends at
       centre_rel + center + 7. */
    for (; n + 3 < outputs && (n << 1) - pending + center + 7 < n_in; n += 4) {
        const float* center_base = in + ((size_t)((n << 1) - pending) << 1);
        const __m128 center_tap = _mm_set1_ps(taps[center]);
        __m128 acc0 = _mm_mul_ps(center_tap, load_iq_stride2_2(center_base));
        __m128 acc1 = _mm_mul_ps(center_tap, load_iq_stride2_2(center_base + 8));

        HbComplexFixedVectorSide<0, (TapsLen + 1) / 4>::apply(center_base, taps, acc0, acc1);
        _mm_storeu_ps(out + (n << 1), acc0);
        _mm_storeu_ps(out + ((n + 2) << 1), acc1);
    }

    for (; n < outputs; n++) {
        const ComplexIq acc = hb_complex_fixed_accumulate_scalar<TapsLen>(source, taps, (n << 1) - pending);
        out[n << 1] = acc.i;
        out[(n << 1) + 1] = acc.q;
    }
}

/* Other odd tap counts: windows over the history's last pending + center samples and the block, output n centred at
   scratch index center + 2n. */
static void
hb_complex_decim2_scratch(const float* in, int n_in, float* out, int outputs, const float* hist_i, const float* hist_q,
                          int pending, const float* taps, int taps_len) {
    const int center = (taps_len - 1) >> 1;
    const float* scratch = prepare_fir_scratch(in, n_in, hist_i, hist_q, taps_len - 1, pending + center);

    auto get_iq = [&](int idx, float& xi, float& xq) {
        const size_t ii = (size_t)idx;
        xi = scratch[2 * ii];
        xq = scratch[2 * ii + 1];
    };

    int n = 0;
    for (; n + 1 < outputs; n += 2) {
        __m128 acc = _mm_setzero_ps();

        float cc = taps[center];
        __m128 tap_c = _mm_set1_ps(cc);

        int center_idx0 = center + (n << 1);
        int center_idx1 = center + ((n + 1) << 1);

        float ci0, cq0, ci1, cq1;
        get_iq(center_idx0, ci0, cq0);
        get_iq(center_idx1, ci1, cq1);
        __m128 center_val = _mm_set_ps(cq1, ci1, cq0, ci0);
        acc = _mm_add_ps(acc, _mm_mul_ps(tap_c, center_val));

        /* Half-band: only even tap indices are non-zero */
        for (int e = 0; e < center; e += 2) {
            float ce = taps[e];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - e;
            __m128 tap_e = _mm_set1_ps(ce);

            float xmI0, xmQ0, xpI0, xpQ0;
            get_iq(center_idx0 - d, xmI0, xmQ0);
            get_iq(center_idx0 + d, xpI0, xpQ0);

            float xmI1, xmQ1, xpI1, xpQ1;
            get_iq(center_idx1 - d, xmI1, xmQ1);
            get_iq(center_idx1 + d, xpI1, xpQ1);

            __m128 sum_m = _mm_set_ps(xmQ1, xmI1, xmQ0, xmI0);
            __m128 sum_p = _mm_set_ps(xpQ1, xpI1, xpQ0, xpI0);
            __m128 sum = _mm_add_ps(sum_m, sum_p);
            acc = _mm_add_ps(acc, _mm_mul_ps(tap_e, sum));
        }

        _mm_storeu_ps(out + (n << 1), acc);
    }

    /* Scalar epilogue */
    for (; n < outputs; n++) {
        int center_idx = center + (n << 1);
        float accI = 0.0f;
        float accQ = 0.0f;

        float ci, cq;
        get_iq(center_idx, ci, cq);
        accI += taps[center] * ci;
        accQ += taps[center] * cq;

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
}

} /* namespace */

/**
 * SSE2 complex symmetric FIR filter (no decimation), streaming (simd_fir_complex_apply()).
 * Processes 2 complex samples (4 floats) at a time.
 */
extern "C" int
simd_fir_complex_apply_sse2(const float* in, int n_in, float* out, int out_cap, float* hist_i, float* hist_q,
                            int hist_len, int* pending, const float* taps, int taps_len) {
    const int outputs =
        simd_fir_complex_outputs(in, n_in, out, out_cap, hist_i, hist_q, hist_len, pending, taps, taps_len);
    if (outputs < 0) {
        return -1;
    }

    const int center = (taps_len - 1) >> 1;
    if (outputs > 0) {
        /* The windows span the history's last pending + center samples and the block: output n is centred at
           scratch index center + n, and the last window ends on the block's last sample. */
        const float* scratch = prepare_fir_scratch(in, n_in, hist_i, hist_q, hist_len, *pending + center);

        auto get_iq = [&](int idx, float& xi, float& xq) {
            const size_t ii = (size_t)idx;
            xi = scratch[2 * ii];
            xq = scratch[2 * ii + 1];
        };

        /* Process 2 complex samples at a time (4 floats) */
        int n = 0;
        for (; n + 1 < outputs; n += 2) {
            __m128 acc = _mm_setzero_ps(); /* [I0, Q0, I1, Q1] */

            /* Center tap for both samples */
            float cc = taps[center];
            __m128 tap_c = _mm_set1_ps(cc);

            const size_t center_offset = ((size_t)center + (size_t)n) << 1;
            __m128 center_val = _mm_loadu_ps(scratch + center_offset);
            acc = _mm_add_ps(acc, _mm_mul_ps(tap_c, center_val));

            /* Symmetric pairs */
            for (int k = 0; k < center; k++) {
                float ce = taps[k];
                if (ce == 0.0f) {
                    continue;
                }
                int d = center - k;
                __m128 tap_e = _mm_set1_ps(ce);

                const size_t minus_offset = ((size_t)(center + n - d)) << 1;
                const size_t plus_offset = ((size_t)(center + n + d)) << 1;
                __m128 sum_m = _mm_loadu_ps(scratch + minus_offset);
                __m128 sum_p = _mm_loadu_ps(scratch + plus_offset);
                __m128 sum = _mm_add_ps(sum_m, sum_p);
                acc = _mm_add_ps(acc, _mm_mul_ps(tap_e, sum));
            }

            _mm_storeu_ps(out + (n << 1), acc);
        }

        /* Scalar epilogue for remaining sample */
        for (; n < outputs; n++) {
            int center_idx = center + n;
            float accI = 0.0f;
            float accQ = 0.0f;

            float ci, cq;
            get_iq(center_idx, ci, cq);
            float cc = taps[center];
            accI += cc * ci;
            accQ += cc * cq;

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
    }

    *pending += n_in - outputs;
    simd_fir_complex_push_history(in, n_in, hist_i, hist_q, hist_len);
    return outputs;
}

/**
 * SSE2 complex half-band decimator by 2, streaming (simd_hb_decim2_complex()).
 * Fixed 15- and 31-tap kernels process 4 complex outputs directly; other odd
 * tap counts use the scratch-backed 2-output kernel.
 */
extern "C" int
simd_hb_decim2_complex_sse2(const float* in, int in_len, float* out, float* hist_i, float* hist_q, int* pending,
                            const float* taps, int taps_len) {
    const int outputs = simd_hb_decim2_complex_outputs(in, in_len, out, hist_i, hist_q, pending, taps, taps_len);
    if (outputs < 0) {
        return -1;
    }
    const int n_in = in_len >> 1; /* complex samples */
    const int held = *pending;
    if (outputs > 0) {
        if (taps_len == 15) {
            hb_complex_decim2_fixed<15>(in, n_in, out, outputs, hist_i, hist_q, held, taps);
        } else if (taps_len == 31) {
            hb_complex_decim2_fixed<31>(in, n_in, out, outputs, hist_i, hist_q, held, taps);
        } else {
            hb_complex_decim2_scratch(in, n_in, out, outputs, hist_i, hist_q, held, taps, taps_len);
        }
    }
    *pending = held + n_in - (outputs << 1);
    simd_fir_complex_push_history(in, n_in, hist_i, hist_q, taps_len - 1);
    return outputs << 1;
}

/**
 * SSE2 real half-band decimator by 2, streaming (simd_hb_decim2_real()).
 * Processes 4 output samples at a time over the history's last pending + center samples and the block, output n
 * centred at scratch index center + 2n.
 */
extern "C" int
simd_hb_decim2_real_sse2(const float* in, int in_len, float* out, float* hist, int* pending, const float* taps,
                         int taps_len) {
    const int out_len = simd_hb_decim2_real_outputs(in, in_len, out, hist, pending, taps, taps_len);
    if (out_len < 0) {
        return -1;
    }

    const int hist_len = taps_len - 1;
    const int center = (taps_len - 1) >> 1;
    const int held = *pending;
    if (out_len > 0) {
        const float* scratch = prepare_real_scratch(in, in_len, hist, hist_len, held + center);

        auto get_sample = [&](int idx) -> float { return scratch[idx]; };

        /* Process 4 output samples at a time */
        int n = 0;
        for (; n + 3 < out_len; n += 4) {
            __m128 acc = _mm_setzero_ps();

            /* Center tap */
            float cc = taps[center];
            __m128 tap_c = _mm_set1_ps(cc);

            /* Center indices for 4 outputs */
            int ci0 = center + (n << 1);
            int ci1 = center + ((n + 1) << 1);
            int ci2 = center + ((n + 2) << 1);
            int ci3 = center + ((n + 3) << 1);

            __m128 center_val = _mm_set_ps(get_sample(ci3), get_sample(ci2), get_sample(ci1), get_sample(ci0));
            acc = _mm_add_ps(acc, _mm_mul_ps(tap_c, center_val));

            /* Half-band: only even indices are non-zero */
            for (int e = 0; e < center; e += 2) {
                float ce = taps[e];
                if (ce == 0.0f) {
                    continue;
                }
                int d = center - e;
                __m128 tap_e = _mm_set1_ps(ce);

                __m128 sum_m =
                    _mm_set_ps(get_sample(ci3 - d), get_sample(ci2 - d), get_sample(ci1 - d), get_sample(ci0 - d));
                __m128 sum_p =
                    _mm_set_ps(get_sample(ci3 + d), get_sample(ci2 + d), get_sample(ci1 + d), get_sample(ci0 + d));
                __m128 sum = _mm_add_ps(sum_m, sum_p);
                acc = _mm_add_ps(acc, _mm_mul_ps(tap_e, sum));
            }

            _mm_storeu_ps(out + n, acc);
        }

        /* Scalar epilogue */
        for (; n < out_len; n++) {
            int center_idx = center + (n << 1);
            float acc = 0.0f;

            acc += taps[center] * get_sample(center_idx);

            for (int e = 0; e < center; e += 2) {
                float ce = taps[e];
                if (ce == 0.0f) {
                    continue;
                }
                int d = center - e;
                acc += ce * (get_sample(center_idx - d) + get_sample(center_idx + d));
            }

            out[n] = acc;
        }
    }

    *pending = held + in_len - (out_len << 1);
    simd_hb_real_push_history(in, in_len, hist, hist_len);
    return out_len;
}

// NOLINTEND(portability-simd-intrinsics)
