// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief ARM64 NEON implementations of SIMD FIR filter functions.
 *
 * NEON is always available on AArch64. Processes 4 floats at a time using
 * 128-bit vector registers. Uses NEON FMA intrinsics for efficiency.
 */

#include <arm_neon.h>
#include <cstring>
#include <vector>

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
static inline float32x4_t
load_iq_stride2_2(const float* base) {
    const float32x4_t lo = vld1q_f32(base);
    const float32x4_t hi = vld1q_f32(base + 4);
    return vcombine_f32(vget_low_f32(lo), vget_low_f32(hi));
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
    apply(const float* center_base, const float* taps, float32x4_t& acc0, float32x4_t& acc1) {
        constexpr int even_tap = SideIndex << 1;
        constexpr int center = (SideCount << 1) - 1;
        constexpr int distance_floats = (center - even_tap) << 1;
        const float coefficient = taps[even_tap];
        if (coefficient != 0.0f) {
            const float* minus = center_base - distance_floats;
            const float* plus = center_base + distance_floats;
            const float32x4_t sum0 = vaddq_f32(load_iq_stride2_2(minus), load_iq_stride2_2(plus));
            const float32x4_t sum1 = vaddq_f32(load_iq_stride2_2(minus + 8), load_iq_stride2_2(plus + 8));
            const float32x4_t tap = vdupq_n_f32(coefficient);
            acc0 = vfmaq_f32(acc0, tap, sum0);
            acc1 = vfmaq_f32(acc1, tap, sum1);
        }
        HbComplexFixedVectorSide<SideIndex + 1, SideCount>::apply(center_base, taps, acc0, acc1);
    }
};

template <int SideCount>
struct HbComplexFixedVectorSide<SideCount, SideCount> {
    static inline void
    apply(const float*, const float*, float32x4_t&, float32x4_t&) {}
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

    /* Four outputs use two NEON accumulators; guard each helper's complete load footprint. */
    for (; n + 3 < out_ch_len && (n << 1) + center + 7 < ch_len; n += 4) {
        const float* center_base = in + (n << 2);
        const float32x4_t center_tap = vdupq_n_f32(taps[center]);
        float32x4_t acc0 = vmulq_f32(center_tap, load_iq_stride2_2(center_base));
        float32x4_t acc1 = vmulq_f32(center_tap, load_iq_stride2_2(center_base + 8));

        HbComplexFixedVectorSide<0, side_count>::apply(center_base, taps, acc0, acc1);
        vst1q_f32(out + (n << 1), acc0);
        vst1q_f32(out + ((n + 2) << 1), acc1);
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
 * NEON complex symmetric FIR filter (no decimation).
 * Processes 2 complex samples (4 floats) at a time.
 */
extern "C" void
simd_fir_complex_apply_neon(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
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
        float32x4_t acc = vdupq_n_f32(0.0f);

        /* Center tap */
        float cc = taps[center];
        float32x4_t tap_c = vdupq_n_f32(cc);

        const size_t center_offset = ((size_t)hist_len + (size_t)n) << 1;
        float32x4_t center_val = vld1q_f32(scratch + center_offset);
        acc = vfmaq_f32(acc, tap_c, center_val);

        /* Symmetric pairs */
        for (int k = 0; k < center; k++) {
            float ce = taps[k];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - k;
            float32x4_t tap_e = vdupq_n_f32(ce);

            const size_t minus_offset = ((size_t)(hist_len + n - d)) << 1;
            const size_t plus_offset = ((size_t)(hist_len + n + d)) << 1;
            float32x4_t sum_m = vld1q_f32(scratch + minus_offset);
            float32x4_t sum_p = vld1q_f32(scratch + plus_offset);
            float32x4_t sum = vaddq_f32(sum_m, sum_p);
            acc = vfmaq_f32(acc, tap_e, sum);
        }

        vst1q_f32(out + (n << 1), acc);
    }

    /* Scalar epilogue */
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
 * NEON complex half-band decimator by 2.
 * Fixed 15- and 31-tap kernels process 4 complex outputs directly; other odd
 * tap counts retain the scratch-backed 2-output kernel.
 */
extern "C" int
simd_hb_decim2_complex_neon(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
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
        float32x4_t acc = vdupq_n_f32(0.0f);

        float cc = taps[center];
        float32x4_t tap_c = vdupq_n_f32(cc);

        int center_idx0 = left_len + (n << 1);
        int center_idx1 = left_len + ((n + 1) << 1);

        float ci0, cq0, ci1, cq1;
        get_iq(center_idx0, ci0, cq0);
        get_iq(center_idx1, ci1, cq1);
        float center_arr[4] = {ci0, cq0, ci1, cq1};
        float32x4_t center_val = vld1q_f32(center_arr);
        acc = vfmaq_f32(acc, tap_c, center_val);

        for (int e = 0; e < center; e += 2) {
            float ce = taps[e];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - e;
            float32x4_t tap_e = vdupq_n_f32(ce);

            float xmI0, xmQ0, xpI0, xpQ0;
            get_iq(center_idx0 - d, xmI0, xmQ0);
            get_iq(center_idx0 + d, xpI0, xpQ0);

            float xmI1, xmQ1, xpI1, xpQ1;
            get_iq(center_idx1 - d, xmI1, xmQ1);
            get_iq(center_idx1 + d, xpI1, xpQ1);

            float sum_m_arr[4] = {xmI0, xmQ0, xmI1, xmQ1};
            float sum_p_arr[4] = {xpI0, xpQ0, xpI1, xpQ1};
            float32x4_t sum_m = vld1q_f32(sum_m_arr);
            float32x4_t sum_p = vld1q_f32(sum_p_arr);
            float32x4_t sum = vaddq_f32(sum_m, sum_p);
            acc = vfmaq_f32(acc, tap_e, sum);
        }

        vst1q_f32(out + (n << 1), acc);
    }

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
 * NEON real half-band decimator by 2.
 * Processes 4 output samples at a time.
 */
extern "C" int
simd_hb_decim2_real_neon(const float* in, int in_len, float* out, float* hist, const float* taps, int taps_len) {
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
        float32x4_t acc = vdupq_n_f32(0.0f);

        float cc = taps[center];
        float32x4_t tap_c = vdupq_n_f32(cc);

        int ci0 = hist_len + (n << 1);
        int ci1 = hist_len + ((n + 1) << 1);
        int ci2 = hist_len + ((n + 2) << 1);
        int ci3 = hist_len + ((n + 3) << 1);

        float center_arr[4] = {get_sample(ci0), get_sample(ci1), get_sample(ci2), get_sample(ci3)};
        float32x4_t center_val = vld1q_f32(center_arr);
        acc = vfmaq_f32(acc, tap_c, center_val);

        /* Half-band: only even indices */
        for (int e = 0; e < center; e += 2) {
            float ce = taps[e];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - e;
            float32x4_t tap_e = vdupq_n_f32(ce);

            float sum_m_arr[4] = {get_sample(ci0 - d), get_sample(ci1 - d), get_sample(ci2 - d), get_sample(ci3 - d)};
            float sum_p_arr[4] = {get_sample(ci0 + d), get_sample(ci1 + d), get_sample(ci2 + d), get_sample(ci3 + d)};
            float32x4_t sum_m = vld1q_f32(sum_m_arr);
            float32x4_t sum_p = vld1q_f32(sum_p_arr);
            float32x4_t sum = vaddq_f32(sum_m, sum_p);
            acc = vfmaq_f32(acc, tap_e, sum);
        }

        vst1q_f32(out + n, acc);
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
