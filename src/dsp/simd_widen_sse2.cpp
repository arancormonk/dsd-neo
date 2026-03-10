// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <xmmintrin.h>

namespace {

alignas(16) static const uint32_t kPhase0SignMask[4] = {0u, 0u, 0x80000000u, 0u};
alignas(16) static const uint32_t kPhase1SignMask[4] = {0x80000000u, 0u, 0x80000000u, 0x80000000u};
alignas(16) static const uint32_t kPhase2SignMask[4] = {0x80000000u, 0x80000000u, 0u, 0x80000000u};
alignas(16) static const uint32_t kPhase3SignMask[4] = {0u, 0x80000000u, 0u, 0u};

static inline __m128
widen4_u8_to_f32_bias127_sse2(const unsigned char* src) {
    uint32_t packed = 0;
    std::memcpy(&packed, src, sizeof(packed));

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

} /* namespace */

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
