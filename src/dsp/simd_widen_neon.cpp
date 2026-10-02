// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <arm_neon.h>
#include <cstdint>
#include "dsd-neo/core/input_level.h"
#include "dsd-neo/core/safe_api.h"
#include "simd_widen_internal.h"

namespace {

alignas(16) static const float kPhase0Signs[4] = {1.0f, 1.0f, -1.0f, 1.0f};
alignas(16) static const float kPhase1Signs[4] = {-1.0f, 1.0f, -1.0f, -1.0f};
alignas(16) static const float kPhase2Signs[4] = {-1.0f, -1.0f, 1.0f, -1.0f};
alignas(16) static const float kPhase3Signs[4] = {1.0f, -1.0f, 1.0f, 1.0f};

static inline float32x4_t
widen4_u8_to_f32_bias127_neon(const unsigned char* src) {
    uint32_t packed = 0;
    DSD_MEMCPY(&packed, src, sizeof(packed));

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
reduce16_u8_neon(uint8x16_t bytes, dsd_input_level_cu8_moments* local, uint8x16_t* min_bytes, uint8x16_t* max_bytes) {
    local->sum += vaddlvq_u8(bytes);
    const uint16x8_t sq_lo = vmull_u8(vget_low_u8(bytes), vget_low_u8(bytes));
    const uint16x8_t sq_hi = vmull_u8(vget_high_u8(bytes), vget_high_u8(bytes));
    local->sum_sq += (uint64_t)vaddlvq_u16(sq_lo) + (uint64_t)vaddlvq_u16(sq_hi);

    uint8x16_t clipped = vorrq_u8(vceqq_u8(bytes, vdupq_n_u8(0U)), vceqq_u8(bytes, vdupq_n_u8(1U)));
    clipped = vorrq_u8(clipped, vceqq_u8(bytes, vdupq_n_u8(254U)));
    clipped = vorrq_u8(clipped, vceqq_u8(bytes, vdupq_n_u8(255U)));
    local->clipped += vaddlvq_u8(vshrq_n_u8(clipped, 7));
    *min_bytes = vminq_u8(*min_bytes, bytes);
    *max_bytes = vmaxq_u8(*max_bytes, bytes);
}

} /* namespace */

extern "C" void
widen_u8_to_f32_bias127_moments_neon(const unsigned char* src, float* dst, uint32_t len,
                                     dsd_input_level_cu8_moments* moments) {
    if (!src || !dst || !moments || len == 0U) {
        return;
    }

    dsd_input_level_cu8_moments local;
    dsd_input_level_cu8_moments_reset(&local);
    local.count = len;
    uint8x16_t min_bytes = vdupq_n_u8(255U);
    uint8x16_t max_bytes = vdupq_n_u8(0U);
    uint32_t i = 0U;
    for (; i + 15U < len; i += 16U) {
        const uint8x16_t bytes = vld1q_u8(src + i);
        reduce16_u8_neon(bytes, &local, &min_bytes, &max_bytes);
        vst1q_f32(dst + i + 0U, widen4_u8_to_f32_bias127_neon(src + i + 0U));
        vst1q_f32(dst + i + 4U, widen4_u8_to_f32_bias127_neon(src + i + 4U));
        vst1q_f32(dst + i + 8U, widen4_u8_to_f32_bias127_neon(src + i + 8U));
        vst1q_f32(dst + i + 12U, widen4_u8_to_f32_bias127_neon(src + i + 12U));
    }
    if (i != 0U) {
        local.min_sample = vminvq_u8(min_bytes);
        local.max_sample = vmaxvq_u8(max_bytes);
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

extern "C" uint32_t
widen_rotate90_u8_to_f32_bias127_phase_moments_neon(const unsigned char* src, float* dst, uint32_t len, uint32_t phase,
                                                    dsd_input_level_cu8_moments* moments) {
    uint32_t cur_phase = phase & 3U;
    if (!src || !dst || !moments || len < 2U) {
        return cur_phase;
    }

    const uint32_t pairs = len >> 1;
    dsd_input_level_cu8_moments local;
    dsd_input_level_cu8_moments_reset(&local);
    local.count = (uint64_t)pairs * 2U;
    uint8x16_t min_bytes = vdupq_n_u8(255U);
    uint8x16_t max_bytes = vdupq_n_u8(0U);
    uint32_t n = 0U;
    for (; n + 7U < pairs; n += 8U) {
        const uint32_t idx = n << 1;
        const uint8x16_t bytes = vld1q_u8(src + idx);
        reduce16_u8_neon(bytes, &local, &min_bytes, &max_bytes);
        for (uint32_t group = 0U; group < 4U; group++) {
            const uint32_t off = idx + group * 4U;
            const float32x4_t vals = widen4_u8_to_f32_bias127_neon(src + off);
            vst1q_f32(dst + off, apply_phase2_neon(vals, cur_phase));
            cur_phase = (cur_phase + 2U) & 3U;
        }
    }
    if (n != 0U) {
        local.min_sample = vminvq_u8(min_bytes);
        local.max_sample = vmaxvq_u8(max_bytes);
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

namespace {

/* The bounded cf32 copy's period kernel (simd_widen_internal.h): two 16-byte vectors. AArch64 is little-endian here
 * (Linux, Android, macOS, Windows), so a byte load puts float k in lane k. */
struct cf32_period_neon {
    using period_copy = cf32_periods_flagged;

    static inline uint32x4_t
    bound(uint32x4_t v) {
        const uint32x4_t magnitude = vandq_u32(v, vdupq_n_u32(kCf32MagnitudeMask));
        return vandq_u32(v, vcltq_u32(magnitude, vdupq_n_u32(kCf32MagnitudeLimitBits)));
    }

    /* The second sample of each 64-bit half swapped: lanes 0, 1, 3, 2. */
    static inline uint32x4_t
    swap_odd_sample(uint32x4_t v) {
        return vcombine_u32(vget_low_u32(v), vrev64_u32(vget_high_u32(v)));
    }

    template <bool kRotate>
    static inline void
    copy(void* dst, const unsigned char* src) {
        uint32x4_t lo = bound(vreinterpretq_u32_u8(vld1q_u8(src)));
        uint32x4_t hi = bound(vreinterpretq_u32_u8(vld1q_u8(src + 16)));
        if (kRotate) {
            lo = veorq_u32(swap_odd_sample(lo), vld1q_u32(kCf32PeriodSigns));
            hi = veorq_u32(swap_odd_sample(hi), vld1q_u32(kCf32PeriodSigns + 4));
        }
        unsigned char* out = static_cast<unsigned char*>(dst);
        vst1q_u8(out, vreinterpretq_u8_u32(lo));
        vst1q_u8(out + 16, vreinterpretq_u8_u32(hi));
    }

    /* Shifted left one bit, a component loses its sign and its magnitude keeps its order as an unsigned value: the
       largest of them reaches 2^60's, doubled, exactly when some component is out of range. Two periods a pass, into
       four maxima, keep the check off the copy's critical path. */
    static inline bool
    copy_periods_flagged(void* dst, const unsigned char* src, size_t periods) {
        /* Written out, not as loops over arrays, so that no compiler keeps them in memory. */
        uint32x4_t top0 = vdupq_n_u32(0U);
        uint32x4_t top1 = vdupq_n_u32(0U);
        uint32x4_t top2 = vdupq_n_u32(0U);
        uint32x4_t top3 = vdupq_n_u32(0U);
        unsigned char* out = static_cast<unsigned char*>(dst);
        size_t k = 0U;
        for (; k + 2U <= periods; k += 2U) {
            const unsigned char* in = src + (k * 32U);
            unsigned char* o = out + (k * 32U);
            const uint8x16_t a = vld1q_u8(in);
            const uint8x16_t b = vld1q_u8(in + 16);
            const uint8x16_t c = vld1q_u8(in + 32);
            const uint8x16_t d = vld1q_u8(in + 48);
            vst1q_u8(o, a);
            vst1q_u8(o + 16, b);
            vst1q_u8(o + 32, c);
            vst1q_u8(o + 48, d);
            top0 = vmaxq_u32(top0, vshlq_n_u32(vreinterpretq_u32_u8(a), 1));
            top1 = vmaxq_u32(top1, vshlq_n_u32(vreinterpretq_u32_u8(b), 1));
            top2 = vmaxq_u32(top2, vshlq_n_u32(vreinterpretq_u32_u8(c), 1));
            top3 = vmaxq_u32(top3, vshlq_n_u32(vreinterpretq_u32_u8(d), 1));
        }
        if (k < periods) {
            const unsigned char* in = src + (k * 32U);
            unsigned char* o = out + (k * 32U);
            const uint8x16_t a = vld1q_u8(in);
            const uint8x16_t b = vld1q_u8(in + 16);
            vst1q_u8(o, a);
            vst1q_u8(o + 16, b);
            top0 = vmaxq_u32(top0, vshlq_n_u32(vreinterpretq_u32_u8(a), 1));
            top1 = vmaxq_u32(top1, vshlq_n_u32(vreinterpretq_u32_u8(b), 1));
        }
        const uint32x4_t largest = vmaxq_u32(vmaxq_u32(top0, top1), vmaxq_u32(top2, top3));
        return vmaxvq_u32(largest) >= (kCf32MagnitudeLimitBits << 1);
    }
};

} /* namespace */

extern "C" void
bound_cf32_to_f32_neon(const void* src, float* dst, size_t pairs) {
    (void)cf32_copy_bounded<cf32_period_neon>(dst, src, pairs, false, 0U);
}

extern "C" uint32_t
bound_rotate90_cf32_to_f32_phase_neon(const void* src, float* dst, size_t pairs, uint32_t phase) {
    return cf32_copy_bounded<cf32_period_neon>(dst, src, pairs, true, phase);
}
