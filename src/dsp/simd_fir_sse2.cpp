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

// NOLINTBEGIN(portability-simd-intrinsics)

namespace {

struct ComplexIq {
    float i;
    float q;
};

struct HbComplexBoundary {
    const float* in;
    int ch_len;
    const float* hist_i;
    const float* hist_q;
    int hist_len;
    float last_i;
    float last_q;
};

static thread_local std::vector<float> tls_scratch_iq;
static thread_local std::vector<float> tls_scratch_real;

static float*
prepare_complex_scratch(const float* in, int in_len, const float* hist_i, const float* hist_q, int hist_len, int pad) {
    const int samples = in_len >> 1;
    const int total_len = hist_len + samples;
    const int scratch_len = (total_len + pad) * 2;

    if (tls_scratch_iq.size() < (size_t)scratch_len) {
        tls_scratch_iq.resize((size_t)scratch_len);
    }

    float* scratch = tls_scratch_iq.data();
    for (int k = 0; k < hist_len; k++) {
        const size_t kk = (size_t)k;
        scratch[2 * kk] = hist_i[k];
        scratch[2 * kk + 1] = hist_q[k];
    }

    DSD_MEMCPY(scratch + (size_t)hist_len * 2, in, (size_t)samples * 2 * sizeof(float));

    const float last_i = (samples > 0) ? in[(samples - 1) << 1] : 0.0f;
    const float last_q = (samples > 0) ? in[((samples - 1) << 1) + 1] : 0.0f;
    for (int k = 0; k < pad; k++) {
        const size_t kk = (size_t)total_len + (size_t)k;
        scratch[2 * kk] = last_i;
        scratch[2 * kk + 1] = last_q;
    }

    return scratch;
}

static void
update_complex_history(const float* in, int samples, float* hist_i, float* hist_q, int hist_len) {
    if (samples >= hist_len) {
        for (int k = 0; k < hist_len; k++) {
            const int rel = samples - hist_len + k;
            hist_i[k] = in[rel << 1];
            hist_q[k] = in[(rel << 1) + 1];
        }
        return;
    }

    const int need = hist_len - samples;
    DSD_MEMMOVE(hist_i, hist_i + (hist_len - need), (size_t)need * sizeof(float));
    DSD_MEMMOVE(hist_q, hist_q + (hist_len - need), (size_t)need * sizeof(float));
    for (int k = 0; k < samples; k++) {
        hist_i[need + k] = in[k << 1];
        hist_q[need + k] = in[(k << 1) + 1];
    }
}

static float*
prepare_real_scratch(const float* in, int in_len, const float* hist, int hist_len, int pad) {
    const int total_len = hist_len + in_len;
    const int scratch_len = total_len + pad;

    if (tls_scratch_real.size() < (size_t)scratch_len) {
        tls_scratch_real.resize((size_t)scratch_len);
    }

    float* scratch = tls_scratch_real.data();
    DSD_MEMCPY(scratch, hist, (size_t)hist_len * sizeof(float));
    DSD_MEMCPY(scratch + hist_len, in, (size_t)in_len * sizeof(float));

    const float last = in[in_len - 1];
    for (int k = 0; k < pad; k++) {
        scratch[total_len + k] = last;
    }

    return scratch;
}

static void
update_real_history(const float* in, int in_len, float* hist, int hist_len) {
    if (in_len >= hist_len) {
        DSD_MEMCPY(hist, in + (in_len - hist_len), (size_t)hist_len * sizeof(float));
        return;
    }

    const int need = hist_len - in_len;
    DSD_MEMMOVE(hist, hist + in_len, (size_t)need * sizeof(float));
    DSD_MEMCPY(hist + need, in, (size_t)in_len * sizeof(float));
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
    if (rel < source.ch_len) {
        return {source.in[rel << 1], source.in[(rel << 1) + 1]};
    }
    return {source.last_i, source.last_q};
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

template <int TapsLen>
static inline ComplexIq
hb_complex_fixed_accumulate_scalar(const HbComplexBoundary& source, const float* taps, int n) {
    constexpr int center = (TapsLen - 1) >> 1;
    constexpr int side_count = (center + 1) >> 1;
    const int center_rel = n << 1;
    const ComplexIq center_sample = {source.in[center_rel << 1], source.in[(center_rel << 1) + 1]};
    ComplexIq acc = {taps[center] * center_sample.i, taps[center] * center_sample.q};
    HbComplexFixedScalarSide<0, side_count>::apply(source, taps, center_rel, acc.i, acc.q);
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

template <int TapsLen>
static int
hb_complex_decim2_fixed(const float* in, int ch_len, float* out, const float* hist_i, const float* hist_q,
                        const float* taps, float last_i, float last_q) {
    static_assert(TapsLen == 15 || TapsLen == 31, "fixed half-band kernel supports 15 or 31 taps");
    constexpr int center = (TapsLen - 1) >> 1;
    constexpr int side_count = (center + 1) >> 1;
    const int out_ch_len = ch_len >> 1;
    const HbComplexBoundary source = {in, ch_len, hist_i, hist_q, TapsLen - 1, last_i, last_q};

    int n = 0;
    for (; n < out_ch_len && (n << 1) < center; n++) {
        const ComplexIq acc = hb_complex_fixed_accumulate_scalar<TapsLen>(source, taps, n);
        out[n << 1] = acc.i;
        out[(n << 1) + 1] = acc.q;
    }

    /* Four outputs use two XMM accumulators; guard each helper's complete load footprint. */
    for (; n + 3 < out_ch_len && (n << 1) + center + 7 < ch_len; n += 4) {
        const float* center_base = in + (n << 2);
        const __m128 center_tap = _mm_set1_ps(taps[center]);
        __m128 acc0 = _mm_mul_ps(center_tap, load_iq_stride2_2(center_base));
        __m128 acc1 = _mm_mul_ps(center_tap, load_iq_stride2_2(center_base + 8));

        HbComplexFixedVectorSide<0, side_count>::apply(center_base, taps, acc0, acc1);
        _mm_storeu_ps(out + (n << 1), acc0);
        _mm_storeu_ps(out + ((n + 2) << 1), acc1);
    }

    for (; n < out_ch_len; n++) {
        const ComplexIq acc = hb_complex_fixed_accumulate_scalar<TapsLen>(source, taps, n);
        out[n << 1] = acc.i;
        out[(n << 1) + 1] = acc.q;
    }

    return out_ch_len << 1;
}

static int
hb_complex_decim2_fixed_dispatch(const float* in, int ch_len, float* out, float* hist_i, float* hist_q,
                                 const float* taps, int taps_len) {
    const float last_i = in[(ch_len - 1) << 1];
    const float last_q = in[((ch_len - 1) << 1) + 1];
    const int out_len = taps_len == 15
                            ? hb_complex_decim2_fixed<15>(in, ch_len, out, hist_i, hist_q, taps, last_i, last_q)
                            : hb_complex_decim2_fixed<31>(in, ch_len, out, hist_i, hist_q, taps, last_i, last_q);
    update_complex_history(in, ch_len, hist_i, hist_q, taps_len - 1);
    return out_len;
}

} /* namespace */

/**
 * SSE2 complex symmetric FIR filter (no decimation).
 * Processes 2 complex samples (4 floats) at a time.
 */
extern "C" void
simd_fir_complex_apply_sse2(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
                            int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0 || in_len < 2) {
        return;
    }

    const int N = in_len >> 1; /* complex samples */
    const int hist_len = taps_len - 1;
    const int center = (taps_len - 1) >> 1;
    const int pad = center + 2;
    float* scratch = prepare_complex_scratch(in, in_len, hist_i, hist_q, hist_len, pad);

    auto get_iq = [&](int idx, float& xi, float& xq) {
        const size_t ii = (size_t)idx;
        xi = scratch[2 * ii];
        xq = scratch[2 * ii + 1];
    };

    /* Process 2 complex samples at a time (4 floats) */
    int n = 0;
    for (; n + 1 < N; n += 2) {
        __m128 acc = _mm_setzero_ps(); /* [I0, Q0, I1, Q1] */

        /* Center tap for both samples */
        float cc = taps[center];
        __m128 tap_c = _mm_set1_ps(cc);

        const size_t center_offset = ((size_t)hist_len + (size_t)n) << 1;
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

            const size_t minus_offset = ((size_t)(hist_len + n - d)) << 1;
            const size_t plus_offset = ((size_t)(hist_len + n + d)) << 1;
            __m128 sum_m = _mm_loadu_ps(scratch + minus_offset);
            __m128 sum_p = _mm_loadu_ps(scratch + plus_offset);
            __m128 sum = _mm_add_ps(sum_m, sum_p);
            acc = _mm_add_ps(acc, _mm_mul_ps(tap_e, sum));
        }

        _mm_storeu_ps(out + (n << 1), acc);
    }

    /* Scalar epilogue for remaining sample */
    for (; n < N; n++) {
        int center_idx = hist_len + n;
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

    update_complex_history(in, N, hist_i, hist_q, hist_len);
}

/**
 * SSE2 complex half-band decimator by 2.
 * Fixed 15- and 31-tap kernels process 4 complex outputs directly; other odd
 * tap counts retain the scratch-backed 2-output kernel.
 */
extern "C" int
simd_hb_decim2_complex_sse2(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
                            int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0) {
        return 0;
    }
    int ch_len = in_len >> 1;
    if (ch_len <= 0) {
        return 0;
    }
    int out_ch_len = ch_len >> 1;

    if (taps_len == 15 || taps_len == 31) {
        return hb_complex_decim2_fixed_dispatch(in, ch_len, out, hist_i, hist_q, taps, taps_len);
    }
    const int center = (taps_len - 1) >> 1;
    const int left_len = taps_len - 1;
    const int pad = center + 2;
    float* scratch = prepare_complex_scratch(in, in_len, hist_i, hist_q, left_len, pad);

    auto get_iq = [&](int idx, float& xi, float& xq) {
        const size_t ii = (size_t)idx;
        xi = scratch[2 * ii];
        xq = scratch[2 * ii + 1];
    };

    int n = 0;
    for (; n + 1 < out_ch_len; n += 2) {
        __m128 acc = _mm_setzero_ps();

        float cc = taps[center];
        __m128 tap_c = _mm_set1_ps(cc);

        int center_idx0 = left_len + (n << 1);
        int center_idx1 = left_len + ((n + 1) << 1);

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
    for (; n < out_ch_len; n++) {
        int center_idx = left_len + (n << 1);
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

    update_complex_history(in, ch_len, hist_i, hist_q, left_len);

    return out_ch_len << 1;
}

/**
 * SSE2 real half-band decimator by 2.
 * Processes 4 output samples at a time.
 */
extern "C" int
simd_hb_decim2_real_sse2(const float* in, int in_len, float* out, float* hist, const float* taps, int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0) {
        return 0;
    }
    if (in_len <= 0) {
        return 0;
    }

    const int hist_len = taps_len - 1;
    const int center = (taps_len - 1) >> 1;
    int out_len = in_len >> 1;
    const int pad = center + 2;
    float* scratch = prepare_real_scratch(in, in_len, hist, hist_len, pad);

    auto get_sample = [&](int idx) -> float { return scratch[idx]; };

    /* Process 4 output samples at a time */
    int n = 0;
    for (; n + 3 < out_len; n += 4) {
        __m128 acc = _mm_setzero_ps();

        /* Center tap */
        float cc = taps[center];
        __m128 tap_c = _mm_set1_ps(cc);

        /* Center indices for 4 outputs */
        int ci0 = hist_len + (n << 1);
        int ci1 = hist_len + ((n + 1) << 1);
        int ci2 = hist_len + ((n + 2) << 1);
        int ci3 = hist_len + ((n + 3) << 1);

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
        int center_idx = hist_len + (n << 1);
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

    update_real_history(in, in_len, hist, hist_len);

    return out_len;
}

// NOLINTEND(portability-simd-intrinsics)
