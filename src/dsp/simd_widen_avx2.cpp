// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdint>
#include "dsd-neo/core/input_level.h"
#include "dsd-neo/core/safe_api.h"

#if defined(__clang_analyzer__)
extern "C" void
widen_u8_to_f32_bias127_moments_avx2(const unsigned char* src, float* dst, uint32_t len,
                                     dsd_input_level_cu8_moments* moments) {
    (void)src;
    (void)dst;
    (void)len;
    (void)moments;
}

extern "C" uint32_t
widen_rotate90_u8_to_f32_bias127_phase_avx2(const unsigned char* src, float* dst, uint32_t len, uint32_t phase) {
    (void)src;
    (void)dst;
    (void)len;
    return phase & 3U;
}

extern "C" uint32_t
widen_rotate90_u8_to_f32_bias127_phase_moments_avx2(const unsigned char* src, float* dst, uint32_t len, uint32_t phase,
                                                    dsd_input_level_cu8_moments* moments) {
    (void)src;
    (void)dst;
    (void)len;
    (void)moments;
    return phase & 3U;
}
#else

#include <emmintrin.h>
#include <immintrin.h>
#include <mmintrin.h>
#include <xmmintrin.h>

// NOLINTBEGIN(portability-simd-intrinsics)

namespace {

alignas(16) static const uint32_t kPhase0SignMask[4] = {0u, 0u, 0x80000000u, 0u};
alignas(16) static const uint32_t kPhase1SignMask[4] = {0x80000000u, 0u, 0x80000000u, 0x80000000u};
alignas(16) static const uint32_t kPhase2SignMask[4] = {0x80000000u, 0x80000000u, 0u, 0x80000000u};
alignas(16) static const uint32_t kPhase3SignMask[4] = {0u, 0x80000000u, 0u, 0u};

static inline __m128
apply_phase2_sse(__m128 vals, uint32_t phase) {
    switch (phase & 3U) {
        case 0: {
            __m128 perm = _mm_shuffle_ps(vals, vals, _MM_SHUFFLE(2, 3, 1, 0));
            __m128 sign = _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i*>(kPhase0SignMask)));
            return _mm_xor_ps(perm, sign);
        }
        case 1: {
            __m128 perm = _mm_shuffle_ps(vals, vals, _MM_SHUFFLE(3, 2, 0, 1));
            __m128 sign = _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i*>(kPhase1SignMask)));
            return _mm_xor_ps(perm, sign);
        }
        case 2: {
            __m128 perm = _mm_shuffle_ps(vals, vals, _MM_SHUFFLE(2, 3, 1, 0));
            __m128 sign = _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i*>(kPhase2SignMask)));
            return _mm_xor_ps(perm, sign);
        }
        default: {
            __m128 perm = _mm_shuffle_ps(vals, vals, _MM_SHUFFLE(3, 2, 0, 1));
            __m128 sign = _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i*>(kPhase3SignMask)));
            return _mm_xor_ps(perm, sign);
        }
    }
}

static inline __m256
widen8_u8_to_f32_bias127_avx2(const unsigned char* src) {
    uint64_t packed = 0;
    DSD_MEMCPY(&packed, src, sizeof(packed));

    __m128i bytes = _mm_cvtsi64_si128((long long)packed);
    __m256i ints = _mm256_cvtepu8_epi32(bytes);
    __m256 vals = _mm256_cvtepi32_ps(ints);
    __m256 bias = _mm256_set1_ps(127.5f);
    __m256 scale = _mm256_set1_ps(1.0f / 127.5f);
    return _mm256_mul_ps(_mm256_sub_ps(vals, bias), scale);
}

static inline __m128
widen4_u8_to_f32_bias127_sse(const unsigned char* src) {
    uint32_t packed = 0;
    DSD_MEMCPY(&packed, src, sizeof(packed));

    __m128i bytes = _mm_cvtsi32_si128((int)packed);
    __m128i zero = _mm_setzero_si128();
    __m128i words = _mm_unpacklo_epi8(bytes, zero);
    __m128i ints = _mm_unpacklo_epi16(words, zero);
    __m128 vals = _mm_cvtepi32_ps(ints);
    __m128 bias = _mm_set1_ps(127.5f);
    __m128 scale = _mm_set1_ps(1.0f / 127.5f);
    return _mm_mul_ps(_mm_sub_ps(vals, bias), scale);
}

static inline void
rotate_pair_scalar(float in_i, float in_q, uint32_t phase, float* out_i, float* out_q) {
    switch (phase & 3U) {
        case 0:
            *out_i = in_i;
            *out_q = in_q;
            break;
        case 1:
            *out_i = -in_q;
            *out_q = in_i;
            break;
        case 2:
            *out_i = -in_i;
            *out_q = -in_q;
            break;
        default:
            *out_i = in_q;
            *out_q = -in_i;
            break;
    }
}

static inline unsigned int
popcount_u32(unsigned int value) {
    unsigned int count = 0U;
    while (value != 0U) {
        value &= value - 1U;
        count++;
    }
    return count;
}

static inline void
accumulate_scalar(dsd_input_level_cu8_moments* local, unsigned char sample) {
    local->sum += (uint64_t)sample;
    local->sum_sq += (uint64_t)sample * (uint64_t)sample;
    local->clipped += (sample <= 1U || sample >= 254U) ? 1U : 0U;
    if (sample < local->min_sample) {
        local->min_sample = sample;
    }
    if (sample > local->max_sample) {
        local->max_sample = sample;
    }
}

static inline void
reduce32_u8_avx2(__m256i bytes, dsd_input_level_cu8_moments* local, __m256i* min_bytes, __m256i* max_bytes) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i ones = _mm256_set1_epi8(1);
    const __m256i high254 = _mm256_set1_epi8(-2);
    const __m256i high255 = _mm256_set1_epi8(-1);

    const __m256i sums = _mm256_sad_epu8(bytes, zero);
    alignas(32) uint64_t sum_lanes[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(sum_lanes), sums);
    for (unsigned int lane = 0U; lane < 4U; lane++) {
        local->sum += sum_lanes[lane];
    }

    const __m128i lo_bytes = _mm256_castsi256_si128(bytes);
    const __m128i hi_bytes = _mm256_extracti128_si256(bytes, 1);
    const __m256i lo_words = _mm256_cvtepu8_epi16(lo_bytes);
    const __m256i hi_words = _mm256_cvtepu8_epi16(hi_bytes);
    const __m256i sq_lo = _mm256_madd_epi16(lo_words, lo_words);
    const __m256i sq_hi = _mm256_madd_epi16(hi_words, hi_words);
    alignas(32) uint32_t sq_lanes[16];
    _mm256_store_si256(reinterpret_cast<__m256i*>(sq_lanes), sq_lo);
    _mm256_store_si256(reinterpret_cast<__m256i*>(sq_lanes + 8), sq_hi);
    for (unsigned int lane = 0U; lane < 16U; lane++) {
        local->sum_sq += sq_lanes[lane];
    }

    __m256i clipped = _mm256_or_si256(_mm256_cmpeq_epi8(bytes, zero), _mm256_cmpeq_epi8(bytes, ones));
    clipped = _mm256_or_si256(clipped, _mm256_cmpeq_epi8(bytes, high254));
    clipped = _mm256_or_si256(clipped, _mm256_cmpeq_epi8(bytes, high255));
    local->clipped += popcount_u32((unsigned int)_mm256_movemask_epi8(clipped));

    *min_bytes = _mm256_min_epu8(*min_bytes, bytes);
    *max_bytes = _mm256_max_epu8(*max_bytes, bytes);
}

static inline void
merge_vector_extrema(__m256i min_bytes, __m256i max_bytes, dsd_input_level_cu8_moments* local) {
    alignas(32) unsigned char mins[32];
    alignas(32) unsigned char maxes[32];
    _mm256_store_si256(reinterpret_cast<__m256i*>(mins), min_bytes);
    _mm256_store_si256(reinterpret_cast<__m256i*>(maxes), max_bytes);
    for (unsigned int lane = 0U; lane < 32U; lane++) {
        if (mins[lane] < local->min_sample) {
            local->min_sample = mins[lane];
        }
        if (maxes[lane] > local->max_sample) {
            local->max_sample = maxes[lane];
        }
    }
}

static inline void
rotate8_store_avx2(const unsigned char* src, float* dst, uint32_t phase) {
    const __m256 vals = widen8_u8_to_f32_bias127_avx2(src);
    __m128 lo = _mm256_castps256_ps128(vals);
    __m128 hi = _mm256_extractf128_ps(vals, 1);
    lo = apply_phase2_sse(lo, phase);
    hi = apply_phase2_sse(hi, (phase + 2U) & 3U);
    __m256 rotated = _mm256_castps128_ps256(lo);
    rotated = _mm256_insertf128_ps(rotated, hi, 1);
    _mm256_storeu_ps(dst, rotated);
}

} /* namespace */

extern "C" void
widen_u8_to_f32_bias127_moments_avx2(const unsigned char* src, float* dst, uint32_t len,
                                     dsd_input_level_cu8_moments* moments) {
    if (!src || !dst || !moments || len == 0U) {
        return;
    }

    dsd_input_level_cu8_moments local;
    dsd_input_level_cu8_moments_reset(&local);
    local.count = len;
    __m256i min_bytes = _mm256_set1_epi8(-1);
    __m256i max_bytes = _mm256_setzero_si256();
    uint32_t i = 0U;
    for (; i + 31U < len; i += 32U) {
        const __m256i bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        reduce32_u8_avx2(bytes, &local, &min_bytes, &max_bytes);
        _mm256_storeu_ps(dst + i + 0U, widen8_u8_to_f32_bias127_avx2(src + i + 0U));
        _mm256_storeu_ps(dst + i + 8U, widen8_u8_to_f32_bias127_avx2(src + i + 8U));
        _mm256_storeu_ps(dst + i + 16U, widen8_u8_to_f32_bias127_avx2(src + i + 16U));
        _mm256_storeu_ps(dst + i + 24U, widen8_u8_to_f32_bias127_avx2(src + i + 24U));
    }
    if (i != 0U) {
        merge_vector_extrema(min_bytes, max_bytes, &local);
    }
    const float inv = 1.0f / 127.5f;
    for (; i < len; i++) {
        const unsigned char sample = src[i];
        dst[i] = ((float)sample - 127.5f) * inv;
        accumulate_scalar(&local, sample);
    }
    (void)dsd_input_level_cu8_moments_merge(moments, &local);
    _mm256_zeroupper();
}

extern "C" uint32_t
widen_rotate90_u8_to_f32_bias127_phase_avx2(const unsigned char* src, float* dst, uint32_t len, uint32_t phase) {
    uint32_t cur_phase = phase & 3U;
    if (!src || !dst || len < 2) {
        return cur_phase;
    }

    const float inv = 1.0f / 127.5f;
    uint32_t pairs = len >> 1;
    uint32_t n = 0;

    for (; n + 3 < pairs; n += 4) {
        uint32_t idx = n << 1;
        __m256 vals = widen8_u8_to_f32_bias127_avx2(src + idx);
        __m128 lo = _mm256_castps256_ps128(vals);
        __m128 hi = _mm256_extractf128_ps(vals, 1);
        lo = apply_phase2_sse(lo, cur_phase);
        hi = apply_phase2_sse(hi, (cur_phase + 2U) & 3U);

        __m256 rotated = _mm256_castps128_ps256(lo);
        rotated = _mm256_insertf128_ps(rotated, hi, 1);
        _mm256_storeu_ps(dst + idx, rotated);
    }

    for (; n + 1 < pairs; n += 2) {
        uint32_t idx = n << 1;
        __m128 vals = widen4_u8_to_f32_bias127_sse(src + idx);
        __m128 rotated = apply_phase2_sse(vals, cur_phase);
        _mm_storeu_ps(dst + idx, rotated);
        cur_phase = (cur_phase + 2U) & 3U;
    }

    for (; n < pairs; n++) {
        uint32_t idx = n << 1;
        float in_i = ((float)src[idx + 0] - 127.5f) * inv;
        float in_q = ((float)src[idx + 1] - 127.5f) * inv;
        rotate_pair_scalar(in_i, in_q, cur_phase, &dst[idx + 0], &dst[idx + 1]);
        cur_phase = (cur_phase + 1U) & 3U;
    }

    _mm256_zeroupper();
    return cur_phase;
}

extern "C" uint32_t
widen_rotate90_u8_to_f32_bias127_phase_moments_avx2(const unsigned char* src, float* dst, uint32_t len, uint32_t phase,
                                                    dsd_input_level_cu8_moments* moments) {
    uint32_t cur_phase = phase & 3U;
    if (!src || !dst || !moments || len < 2U) {
        return cur_phase;
    }

    const uint32_t pairs = len >> 1;
    dsd_input_level_cu8_moments local;
    dsd_input_level_cu8_moments_reset(&local);
    local.count = (uint64_t)pairs * 2U;
    __m256i min_bytes = _mm256_set1_epi8(-1);
    __m256i max_bytes = _mm256_setzero_si256();
    uint32_t n = 0U;
    for (; n + 15U < pairs; n += 16U) {
        const uint32_t idx = n << 1;
        const __m256i bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + idx));
        reduce32_u8_avx2(bytes, &local, &min_bytes, &max_bytes);
        for (uint32_t group = 0U; group < 4U; group++) {
            rotate8_store_avx2(src + idx + group * 8U, dst + idx + group * 8U, cur_phase);
        }
    }
    if (n != 0U) {
        merge_vector_extrema(min_bytes, max_bytes, &local);
    }

    const float inv = 1.0f / 127.5f;
    for (; n < pairs; n++) {
        const uint32_t idx = n << 1;
        const unsigned char in_i = src[idx + 0U];
        const unsigned char in_q = src[idx + 1U];
        const float i_raw = ((float)in_i - 127.5f) * inv;
        const float q_raw = ((float)in_q - 127.5f) * inv;
        rotate_pair_scalar(i_raw, q_raw, cur_phase, &dst[idx + 0U], &dst[idx + 1U]);
        accumulate_scalar(&local, in_i);
        accumulate_scalar(&local, in_q);
        cur_phase = (cur_phase + 1U) & 3U;
    }

    (void)dsd_input_level_cu8_moments_merge(moments, &local);
    _mm256_zeroupper();
    return cur_phase;
}

// NOLINTEND(portability-simd-intrinsics)

#endif
