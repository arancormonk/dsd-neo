// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief AVX2+FMA implementations of SIMD FIR filter functions.
 *
 * Compiled with -mavx2 -mfma (GCC/Clang) or /arch:AVX2 (MSVC).
 * Processes 8 floats at a time using 256-bit YMM registers.
 * Uses FMA (fused multiply-add) for efficiency.
 */

#if defined(__clang_analyzer__)
extern "C" int
simd_fir_complex_apply_avx2(const float* in, int n_in, float* out, int out_cap, float* hist_i, float* hist_q,
                            int hist_len, int* pending, const float* taps, int taps_len) {
    (void)in;
    (void)n_in;
    (void)out;
    (void)out_cap;
    (void)hist_i;
    (void)hist_q;
    (void)hist_len;
    (void)pending;
    (void)taps;
    (void)taps_len;
    return 0;
}

extern "C" int
simd_hb_decim2_complex_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q, int* pending,
                            const float* taps, int taps_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)hist_i;
    (void)hist_q;
    (void)pending;
    (void)taps;
    (void)taps_len;
    return 0;
}

extern "C" int
simd_hb_decim2_real_avx2(const float* in, int in_len, float* out, float* hist, int* pending, const float* taps,
                         int taps_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)hist;
    (void)pending;
    (void)taps;
    (void)taps_len;
    return 0;
}
#else

#include <cstring>
#include <immintrin.h>
#include <vector>
#include <xmmintrin.h>
#include "dsd-neo/core/safe_api.h"
#include "simd_fir_internal.h"

// NOLINTBEGIN(portability-simd-intrinsics)

static thread_local std::vector<float> tls_scratch_iq;
static thread_local std::vector<float> tls_scratch_real;

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

static inline void
copy_iq_history(float* scratch, const float* hist_i, const float* hist_q, int hist_len) {
    for (int k = 0; k < hist_len; k++) {
        const size_t kk = (size_t)k;
        scratch[2 * kk] = hist_i[k];
        scratch[2 * kk + 1] = hist_q[k];
    }
}

static inline void
copy_iq_input(float* scratch, int hist_len, const float* in, int sample_count) {
    DSD_MEMCPY(scratch + (size_t)hist_len * 2, in, (size_t)sample_count * 2 * sizeof(float));
}

static inline ComplexIq
load_iq(const float* scratch, int idx) {
    const size_t ii = (size_t)idx;
    return {scratch[2 * ii], scratch[2 * ii + 1]};
}

static inline __m256
pack_iq4(const float* scratch, int i0, int i1, int i2, int i3) {
    const ComplexIq s0 = load_iq(scratch, i0);
    const ComplexIq s1 = load_iq(scratch, i1);
    const ComplexIq s2 = load_iq(scratch, i2);
    const ComplexIq s3 = load_iq(scratch, i3);
    return _mm256_set_ps(s3.q, s3.i, s2.q, s2.i, s1.q, s1.i, s0.q, s0.i);
}

/* Four complex outputs from @p n on, output n centred at scratch index first_center + n. */
static inline __m256
fir_complex_accumulate4(const float* scratch, const float* taps, int first_center, int center, int n) {
    __m256 acc = _mm256_setzero_ps();
    const __m256 tap_c = _mm256_set1_ps(taps[center]);
    const size_t center_offset = ((size_t)(first_center + n)) << 1;
    const __m256 center_val = _mm256_loadu_ps(scratch + center_offset);
    acc = _mm256_fmadd_ps(tap_c, center_val, acc);

    for (int k = 0; k < center; k++) {
        const float ce = taps[k];
        if (ce == 0.0f) {
            continue;
        }
        const int d = center - k;
        const size_t minus_offset = ((size_t)(first_center + n - d)) << 1;
        const size_t plus_offset = ((size_t)(first_center + n + d)) << 1;
        const __m256 sum_m = _mm256_loadu_ps(scratch + minus_offset);
        const __m256 sum_p = _mm256_loadu_ps(scratch + plus_offset);
        const __m256 sum = _mm256_add_ps(sum_m, sum_p);
        acc = _mm256_fmadd_ps(_mm256_set1_ps(ce), sum, acc);
    }
    return acc;
}

static inline ComplexIq
fir_complex_accumulate_scalar(const float* scratch, const float* taps, int center, int center_idx) {
    ComplexIq center_sample = load_iq(scratch, center_idx);
    ComplexIq acc = {taps[center] * center_sample.i, taps[center] * center_sample.q};

    for (int k = 0; k < center; k++) {
        const float ce = taps[k];
        if (ce == 0.0f) {
            continue;
        }
        const int d = center - k;
        const ComplexIq minus = load_iq(scratch, center_idx - d);
        const ComplexIq plus = load_iq(scratch, center_idx + d);
        acc.i += ce * (minus.i + plus.i);
        acc.q += ce * (minus.q + plus.q);
    }
    return acc;
}

/* Four decimated outputs from @p n on, output n centred at scratch index first_center + 2n. */
static inline __m256
hb_complex_accumulate4(const float* scratch, const float* taps, int first_center, int center, int n) {
    const int ci0 = first_center + (n << 1);
    const int ci1 = first_center + ((n + 1) << 1);
    const int ci2 = first_center + ((n + 2) << 1);
    const int ci3 = first_center + ((n + 3) << 1);

    __m256 acc = _mm256_setzero_ps();
    acc = _mm256_fmadd_ps(_mm256_set1_ps(taps[center]), pack_iq4(scratch, ci0, ci1, ci2, ci3), acc);

    for (int e = 0; e < center; e += 2) {
        const float ce = taps[e];
        if (ce == 0.0f) {
            continue;
        }
        const int d = center - e;
        const __m256 sum_m = pack_iq4(scratch, ci0 - d, ci1 - d, ci2 - d, ci3 - d);
        const __m256 sum_p = pack_iq4(scratch, ci0 + d, ci1 + d, ci2 + d, ci3 + d);
        const __m256 sum = _mm256_add_ps(sum_m, sum_p);
        acc = _mm256_fmadd_ps(_mm256_set1_ps(ce), sum, acc);
    }
    return acc;
}

static inline ComplexIq
hb_complex_accumulate_scalar(const float* scratch, const float* taps, int first_center, int center, int n) {
    const int center_idx = first_center + (n << 1);
    const ComplexIq center_sample = load_iq(scratch, center_idx);
    ComplexIq acc = {taps[center] * center_sample.i, taps[center] * center_sample.q};

    for (int e = 0; e < center; e += 2) {
        const float ce = taps[e];
        if (ce == 0.0f) {
            continue;
        }
        const int d = center - e;
        const ComplexIq minus = load_iq(scratch, center_idx - d);
        const ComplexIq plus = load_iq(scratch, center_idx + d);
        acc.i += ce * (minus.i + plus.i);
        acc.q += ce * (minus.q + plus.q);
    }
    return acc;
}

static inline float
load_real(const float* scratch, int idx) {
    return scratch[idx];
}

static inline float
hb_real_accumulate_scalar(const float* scratch, const float* taps, int first_center, int center, int n) {
    const int center_idx = first_center + (n << 1);
    float acc = taps[center] * load_real(scratch, center_idx);

    for (int e = 0; e < center; e += 2) {
        const float ce = taps[e];
        if (ce == 0.0f) {
            continue;
        }
        const int d = center - e;
        acc += ce * (load_real(scratch, center_idx - d) + load_real(scratch, center_idx + d));
    }
    return acc;
}

/*
 * Load four complex samples at indices base, base+2, base+4, and base+6.
 * Each source load is contiguous and unaligned; the shuffle first selects the
 * even complex pair from each 128-bit lane and the 64-bit permutation restores
 * sample order across the lanes.
 */
static inline __m256
load_iq_stride2_4(const float* base) {
    const __m256 lo = _mm256_loadu_ps(base);
    const __m256 hi = _mm256_loadu_ps(base + 8);
    const __m256 selected = _mm256_shuffle_ps(lo, hi, _MM_SHUFFLE(1, 0, 1, 0));
    return _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(selected), 0xD8));
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
    apply(const float* center_base, const float* taps, __m256& acc0, __m256& acc1) {
        constexpr int even_tap = SideIndex << 1;
        constexpr int center = (SideCount << 1) - 1;
        constexpr int distance_floats = (center - even_tap) << 1;
        const float coefficient = taps[even_tap];
        if (coefficient != 0.0f) {
            const float* minus = center_base - distance_floats;
            const float* plus = center_base + distance_floats;
            const __m256 sum0 = _mm256_add_ps(load_iq_stride2_4(minus), load_iq_stride2_4(plus));
            const __m256 sum1 = _mm256_add_ps(load_iq_stride2_4(minus + 16), load_iq_stride2_4(plus + 16));
            const __m256 tap = _mm256_set1_ps(coefficient);
            acc0 = _mm256_fmadd_ps(tap, sum0, acc0);
            acc1 = _mm256_fmadd_ps(tap, sum1, acc1);
        }
        HbComplexFixedVectorSide<SideIndex + 1, SideCount>::apply(center_base, taps, acc0, acc1);
    }
};

template <int SideCount>
struct HbComplexFixedVectorSide<SideCount, SideCount> {
    static inline void
    apply(const float*, const float*, __m256&, __m256&) {}
};

/* @p outputs outputs from a block of @p n_in samples at @p pending: output n centred at centre_rel = 2n - pending. */
template <int TapsLen>
static void
hb_complex_decim2_fixed(const float* in, int n_in, float* out, int outputs, const float* hist_i, const float* hist_q,
                        int pending, const float* taps) {
    static_assert(TapsLen == 15 || TapsLen == 31, "fixed half-band kernel supports 15 or 31 taps");
    constexpr int center = (TapsLen - 1) >> 1;
    const HbComplexBoundary source = {in, hist_i, hist_q, TapsLen - 1};

    int n = 0;

    /* Prefix outputs whose window starts in the history stay on the scalar boundary path. */
    for (; n < outputs && (n << 1) - pending < center; n++) {
        const ComplexIq acc = hb_complex_fixed_accumulate_scalar<TapsLen>(source, taps, (n << 1) - pending);
        out[n << 1] = acc.i;
        out[(n << 1) + 1] = acc.q;
    }

    /*
     * Eight outputs use two YMM accumulators. The final side-tap helper loads
     * through centre_rel + center + 15, including the unused samples within
     * its second contiguous load, so guard that complete footprint.
     */
    for (; n + 7 < outputs && (n << 1) - pending + center + 15 < n_in; n += 8) {
        const float* center_base = in + ((size_t)((n << 1) - pending) << 1);
        const __m256 center_tap = _mm256_set1_ps(taps[center]);
        __m256 acc0 = _mm256_fmadd_ps(center_tap, load_iq_stride2_4(center_base), _mm256_setzero_ps());
        __m256 acc1 = _mm256_fmadd_ps(center_tap, load_iq_stride2_4(center_base + 16), _mm256_setzero_ps());

        HbComplexFixedVectorSide<0, (TapsLen + 1) / 4>::apply(center_base, taps, acc0, acc1);
        _mm256_storeu_ps(out + (n << 1), acc0);
        _mm256_storeu_ps(out + ((n + 4) << 1), acc1);
    }

    /* The vector remainder, whose load footprint would pass the block's end, stays scalar. */
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
    const int take = pending + center;
    const size_t scratch_len = ((size_t)take + (size_t)n_in) * 2U; /* *2 for complex (I, Q) */

    /* Resize thread-local buffer if needed (amortized O(1)) */
    if (tls_scratch_iq.size() < scratch_len) {
        tls_scratch_iq.resize(scratch_len);
    }
    float* scratch = tls_scratch_iq.data();

    const int left_len = taps_len - 1;
    copy_iq_history(scratch, hist_i + (left_len - take), hist_q + (left_len - take), take);
    copy_iq_input(scratch, take, in, n_in);

    /* Process 4 output samples at a time */
    int n = 0;
    for (; n + 3 < outputs; n += 4) {
        const __m256 acc = hb_complex_accumulate4(scratch, taps, center, center, n);
        _mm256_storeu_ps(out + (n << 1), acc);
    }

    /* Scalar epilogue */
    for (; n < outputs; n++) {
        const ComplexIq acc = hb_complex_accumulate_scalar(scratch, taps, center, center, n);
        out[n << 1] = acc.i;
        out[(n << 1) + 1] = acc.q;
    }
}

} // namespace

/**
 * AVX2+FMA complex symmetric FIR filter (no decimation), streaming (simd_fir_complex_apply()).
 * Processes 4 complex samples (8 floats) at a time.
 * Uses pre-concatenated scratch buffer to eliminate branching in hot loop.
 */
extern "C" int
simd_fir_complex_apply_avx2(const float* in, int n_in, float* out, int out_cap, float* hist_i, float* hist_q,
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
        const int take = *pending + center;
        const size_t scratch_len = ((size_t)take + (size_t)n_in) * 2U; /* *2 for complex (I, Q) */

        /* Resize thread-local buffer if needed (amortized O(1)) */
        if (tls_scratch_iq.size() < scratch_len) {
            tls_scratch_iq.resize(scratch_len);
        }
        float* scratch = tls_scratch_iq.data();

        copy_iq_history(scratch, hist_i + (hist_len - take), hist_q + (hist_len - take), take);
        if (n_in > 0) {
            copy_iq_input(scratch, take, in, n_in);
        }

        /* Process 8 complex samples at a time (16 floats). */
        int n = 0;
        for (; n + 7 < outputs; n += 8) {
            const __m256 acc0 = fir_complex_accumulate4(scratch, taps, center, center, n);
            const __m256 acc1 = fir_complex_accumulate4(scratch, taps, center, center, n + 4);
            _mm256_storeu_ps(out + (n << 1), acc0);
            _mm256_storeu_ps(out + ((n + 4) << 1), acc1);
        }

        /* Process 4 complex samples at a time (8 floats) */
        for (; n + 3 < outputs; n += 4) {
            const __m256 acc = fir_complex_accumulate4(scratch, taps, center, center, n);
            _mm256_storeu_ps(out + (n << 1), acc);
        }

        /* Scalar epilogue for remaining samples */
        for (; n < outputs; n++) {
            const ComplexIq acc = fir_complex_accumulate_scalar(scratch, taps, center, center + n);
            out[n << 1] = acc.i;
            out[(n << 1) + 1] = acc.q;
        }
        _mm256_zeroupper(); /* Avoid AVX-SSE transition penalty */
    }

    *pending += n_in - outputs;
    simd_fir_complex_push_history(in, n_in, hist_i, hist_q, hist_len);
    return outputs;
}

/**
 * AVX2+FMA complex half-band decimator by 2, streaming (simd_hb_decim2_complex()).
 * The fixed 15- and 31-tap kernels process 8 complex outputs directly from
 * the input; other odd tap counts use the scratch-backed 4-output kernel.
 */
extern "C" int
simd_hb_decim2_complex_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q, int* pending,
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
        _mm256_zeroupper(); /* Avoid AVX-SSE transition penalty */
    }
    *pending = held + n_in - (outputs << 1);
    simd_fir_complex_push_history(in, n_in, hist_i, hist_q, taps_len - 1);
    return outputs << 1;
}

/**
 * AVX2+FMA real half-band decimator by 2, streaming (simd_hb_decim2_real()).
 * Processes 8 output samples at a time over the history's last pending + center samples and the block, output n
 * centred at scratch index center + 2n.
 */
extern "C" int
simd_hb_decim2_real_avx2(const float* in, int in_len, float* out, float* hist, int* pending, const float* taps,
                         int taps_len) {
    const int out_len = simd_hb_decim2_real_outputs(in, in_len, out, hist, pending, taps, taps_len);
    if (out_len < 0) {
        return -1;
    }

    const int hist_len = taps_len - 1;
    const int center = (taps_len - 1) >> 1;
    const int held = *pending;
    if (out_len > 0) {
        const int take = held + center;
        const size_t scratch_len = (size_t)take + (size_t)in_len;
        if (tls_scratch_real.size() < scratch_len) {
            tls_scratch_real.resize(scratch_len);
        }
        float* scratch = tls_scratch_real.data();
        DSD_MEMCPY(scratch, hist + (hist_len - take), (size_t)take * sizeof(float));
        DSD_MEMCPY(scratch + take, in, (size_t)in_len * sizeof(float));

        /* Process 8 output samples at a time */
        int n = 0;
        for (; n + 7 < out_len; n += 8) {
            __m256 acc = _mm256_setzero_ps();

            float cc = taps[center];
            __m256 tap_c = _mm256_set1_ps(cc);

            /* Center indices for 8 outputs */
            int ci0 = center + (n << 1);
            int ci1 = center + ((n + 1) << 1);
            int ci2 = center + ((n + 2) << 1);
            int ci3 = center + ((n + 3) << 1);
            int ci4 = center + ((n + 4) << 1);
            int ci5 = center + ((n + 5) << 1);
            int ci6 = center + ((n + 6) << 1);
            int ci7 = center + ((n + 7) << 1);

            __m256 center_val = _mm256_set_ps(load_real(scratch, ci7), load_real(scratch, ci6), load_real(scratch, ci5),
                                              load_real(scratch, ci4), load_real(scratch, ci3), load_real(scratch, ci2),
                                              load_real(scratch, ci1), load_real(scratch, ci0));
            acc = _mm256_fmadd_ps(tap_c, center_val, acc);

            /* Half-band: only even indices */
            for (int e = 0; e < center; e += 2) {
                float ce = taps[e];
                if (ce == 0.0f) {
                    continue;
                }
                int d = center - e;
                __m256 tap_e = _mm256_set1_ps(ce);

                __m256 sum_m =
                    _mm256_set_ps(load_real(scratch, ci7 - d), load_real(scratch, ci6 - d), load_real(scratch, ci5 - d),
                                  load_real(scratch, ci4 - d), load_real(scratch, ci3 - d), load_real(scratch, ci2 - d),
                                  load_real(scratch, ci1 - d), load_real(scratch, ci0 - d));
                __m256 sum_p =
                    _mm256_set_ps(load_real(scratch, ci7 + d), load_real(scratch, ci6 + d), load_real(scratch, ci5 + d),
                                  load_real(scratch, ci4 + d), load_real(scratch, ci3 + d), load_real(scratch, ci2 + d),
                                  load_real(scratch, ci1 + d), load_real(scratch, ci0 + d));
                __m256 sum = _mm256_add_ps(sum_m, sum_p);
                acc = _mm256_fmadd_ps(tap_e, sum, acc);
            }

            _mm256_storeu_ps(out + n, acc);
        }

        /* Scalar epilogue */
        for (; n < out_len; n++) {
            out[n] = hb_real_accumulate_scalar(scratch, taps, center, center, n);
        }
        _mm256_zeroupper();
    }

    *pending = held + in_len - (out_len << 1);
    simd_hb_real_push_history(in, in_len, hist, hist_len);
    return out_len;
}

// NOLINTEND(portability-simd-intrinsics)

#endif
