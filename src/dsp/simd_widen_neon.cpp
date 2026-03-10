// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <arm_neon.h>
#include <cstdint>
#include <cstring>

namespace {

alignas(16) static const float kPhase0Signs[4] = {1.0f, 1.0f, -1.0f, 1.0f};
alignas(16) static const float kPhase1Signs[4] = {-1.0f, 1.0f, -1.0f, -1.0f};
alignas(16) static const float kPhase2Signs[4] = {-1.0f, -1.0f, 1.0f, -1.0f};
alignas(16) static const float kPhase3Signs[4] = {1.0f, -1.0f, 1.0f, 1.0f};

static inline float32x4_t
widen4_u8_to_f32_bias127_neon(const unsigned char* src) {
    uint32_t packed = 0;
    std::memcpy(&packed, src, sizeof(packed));

    uint8x8_t bytes = vreinterpret_u8_u32(vdup_n_u32(packed));
    uint16x8_t words = vmovl_u8(bytes);
    uint32x4_t ints = vmovl_u16(vget_low_u16(words));
    float32x4_t vals = vcvtq_f32_u32(ints);
    return vmulq_n_f32(vsubq_f32(vals, vdupq_n_f32(127.5f)), 1.0f / 127.5f);
}

static inline float32x4_t
apply_phase2_neon(float32x4_t vals, uint32_t phase) {
    float32x2_t lo = vget_low_f32(vals);
    float32x2_t hi = vget_high_f32(vals);

    switch (phase & 3U) {
        case 0: {
            float32x4_t perm = vcombine_f32(lo, vrev64_f32(hi));
            return vmulq_f32(perm, vld1q_f32(kPhase0Signs));
        }
        case 1: {
            float32x4_t perm = vcombine_f32(vrev64_f32(lo), hi);
            return vmulq_f32(perm, vld1q_f32(kPhase1Signs));
        }
        case 2: {
            float32x4_t perm = vcombine_f32(lo, vrev64_f32(hi));
            return vmulq_f32(perm, vld1q_f32(kPhase2Signs));
        }
        default: {
            float32x4_t perm = vcombine_f32(vrev64_f32(lo), hi);
            return vmulq_f32(perm, vld1q_f32(kPhase3Signs));
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
widen_rotate90_u8_to_f32_bias127_phase_neon(const unsigned char* src, float* dst, uint32_t len, uint32_t phase) {
    uint32_t cur_phase = phase & 3U;
    if (!src || !dst || len < 2) {
        return cur_phase;
    }

    const float inv = 1.0f / 127.5f;
    uint32_t pairs = len >> 1;
    uint32_t n = 0;

    for (; n + 1 < pairs; n += 2) {
        uint32_t idx = n << 1;
        float32x4_t vals = widen4_u8_to_f32_bias127_neon(src + idx);
        float32x4_t rotated = apply_phase2_neon(vals, cur_phase);
        vst1q_f32(dst + idx, rotated);
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
