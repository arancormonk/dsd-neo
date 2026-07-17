// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdint>
#include <emmintrin.h>
#include <mmintrin.h>
#include <xmmintrin.h>
#include "dsd-neo/core/input_level.h"
#include "dsd-neo/core/safe_api.h"

// NOLINTBEGIN(portability-simd-intrinsics)

namespace {

alignas(16) static const uint32_t kPhase0SignMask[4] = {0u, 0u, 0x80000000u, 0u};
alignas(16) static const uint32_t kPhase1SignMask[4] = {0x80000000u, 0u, 0x80000000u, 0x80000000u};
alignas(16) static const uint32_t kPhase2SignMask[4] = {0x80000000u, 0x80000000u, 0u, 0x80000000u};
alignas(16) static const uint32_t kPhase3SignMask[4] = {0u, 0x80000000u, 0u, 0u};

static inline __m128
widen4_u8_to_f32_bias127_sse2(const unsigned char* src) {
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

static inline __m128
apply_phase2_sse2(__m128 vals, uint32_t phase) {
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
popcount_u16(unsigned int value) {
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
reduce16_u8_sse2(__m128i bytes, dsd_input_level_cu8_moments* local, __m128i* min_bytes, __m128i* max_bytes) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i ones = _mm_set1_epi8(1);
    const __m128i high254 = _mm_set1_epi8((char)0xfe);
    const __m128i high255 = _mm_set1_epi8((char)0xff);

    const __m128i sums = _mm_sad_epu8(bytes, zero);
    alignas(16) uint64_t sum_lanes[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(sum_lanes), sums);
    local->sum += sum_lanes[0] + sum_lanes[1];

    const __m128i lo = _mm_unpacklo_epi8(bytes, zero);
    const __m128i hi = _mm_unpackhi_epi8(bytes, zero);
    const __m128i sq_lo = _mm_madd_epi16(lo, lo);
    const __m128i sq_hi = _mm_madd_epi16(hi, hi);
    alignas(16) uint32_t sq_lanes[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(sq_lanes), sq_lo);
    _mm_store_si128(reinterpret_cast<__m128i*>(sq_lanes + 4), sq_hi);
    for (unsigned int lane = 0U; lane < 8U; lane++) {
        local->sum_sq += sq_lanes[lane];
    }

    __m128i clipped = _mm_or_si128(_mm_cmpeq_epi8(bytes, zero), _mm_cmpeq_epi8(bytes, ones));
    clipped = _mm_or_si128(clipped, _mm_cmpeq_epi8(bytes, high254));
    clipped = _mm_or_si128(clipped, _mm_cmpeq_epi8(bytes, high255));
    local->clipped += popcount_u16((unsigned int)_mm_movemask_epi8(clipped));

    *min_bytes = _mm_min_epu8(*min_bytes, bytes);
    *max_bytes = _mm_max_epu8(*max_bytes, bytes);
}

static inline void
merge_vector_extrema(__m128i min_bytes, __m128i max_bytes, dsd_input_level_cu8_moments* local) {
    alignas(16) unsigned char mins[16];
    alignas(16) unsigned char maxes[16];
    _mm_store_si128(reinterpret_cast<__m128i*>(mins), min_bytes);
    _mm_store_si128(reinterpret_cast<__m128i*>(maxes), max_bytes);
    for (unsigned int lane = 0U; lane < 16U; lane++) {
        if (mins[lane] < local->min_sample) {
            local->min_sample = mins[lane];
        }
        if (maxes[lane] > local->max_sample) {
            local->max_sample = maxes[lane];
        }
    }
}

} /* namespace */

extern "C" void
widen_u8_to_f32_bias127_moments_sse2(const unsigned char* src, float* dst, uint32_t len,
                                     dsd_input_level_cu8_moments* moments) {
    if (!src || !dst || !moments || len == 0U) {
        return;
    }

    dsd_input_level_cu8_moments local;
    dsd_input_level_cu8_moments_reset(&local);
    local.count = len;
    __m128i min_bytes = _mm_set1_epi8((char)0xff);
    __m128i max_bytes = _mm_setzero_si128();
    uint32_t i = 0U;
    for (; i + 15U < len; i += 16U) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        reduce16_u8_sse2(bytes, &local, &min_bytes, &max_bytes);
        _mm_storeu_ps(dst + i + 0U, widen4_u8_to_f32_bias127_sse2(src + i + 0U));
        _mm_storeu_ps(dst + i + 4U, widen4_u8_to_f32_bias127_sse2(src + i + 4U));
        _mm_storeu_ps(dst + i + 8U, widen4_u8_to_f32_bias127_sse2(src + i + 8U));
        _mm_storeu_ps(dst + i + 12U, widen4_u8_to_f32_bias127_sse2(src + i + 12U));
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
}

extern "C" uint32_t
widen_rotate90_u8_to_f32_bias127_phase_sse2(const unsigned char* src, float* dst, uint32_t len, uint32_t phase) {
    uint32_t cur_phase = phase & 3U;
    if (!src || !dst || len < 2) {
        return cur_phase;
    }

    const float inv = 1.0f / 127.5f;
    uint32_t pairs = len >> 1;
    uint32_t n = 0;

    for (; n + 1 < pairs; n += 2) {
        uint32_t idx = n << 1;
        __m128 vals = widen4_u8_to_f32_bias127_sse2(src + idx);
        __m128 rotated = apply_phase2_sse2(vals, cur_phase);
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

    return cur_phase;
}

extern "C" uint32_t
widen_rotate90_u8_to_f32_bias127_phase_moments_sse2(const unsigned char* src, float* dst, uint32_t len, uint32_t phase,
                                                    dsd_input_level_cu8_moments* moments) {
    uint32_t cur_phase = phase & 3U;
    if (!src || !dst || !moments || len < 2U) {
        return cur_phase;
    }

    const uint32_t pairs = len >> 1;
    dsd_input_level_cu8_moments local;
    dsd_input_level_cu8_moments_reset(&local);
    local.count = (uint64_t)pairs * 2U;
    __m128i min_bytes = _mm_set1_epi8((char)0xff);
    __m128i max_bytes = _mm_setzero_si128();
    uint32_t n = 0U;
    for (; n + 7U < pairs; n += 8U) {
        const uint32_t idx = n << 1;
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + idx));
        reduce16_u8_sse2(bytes, &local, &min_bytes, &max_bytes);
        for (uint32_t group = 0U; group < 4U; group++) {
            const uint32_t off = idx + group * 4U;
            const __m128 vals = widen4_u8_to_f32_bias127_sse2(src + off);
            _mm_storeu_ps(dst + off, apply_phase2_sse2(vals, cur_phase));
            cur_phase = (cur_phase + 2U) & 3U;
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
    return cur_phase;
}

// NOLINTEND(portability-simd-intrinsics)
