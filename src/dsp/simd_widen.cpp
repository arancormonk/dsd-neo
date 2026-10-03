// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Scalar helpers to widen RTL u8 IQ into normalized float samples.
 *
 * Converts unsigned 8-bit I/Q into centered float in [-1.0, 1.0] with an
 * unbiased midpoint at 127.5. No clamping is applied so headroom is retained
 * for downstream float processing.
 */

#include <dsd-neo/core/input_level.h>
#include <dsd-neo/dsp/simd_widen.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"
#include "simd_widen_internal.h"
#include "simd_x86_cpu.h"

static inline void
apply_j4_rotation_f32(float in_i, float in_q, uint32_t phase, float* out_i, float* out_q) {
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

using widen_rot_phase_fn = uint32_t (*)(const unsigned char*, float*, uint32_t, uint32_t);
using widen_moments_fn = void (*)(const unsigned char*, float*, uint32_t, dsd_input_level_cu8_moments*);
using widen_rot_phase_moments_fn = uint32_t (*)(const unsigned char*, float*, uint32_t, uint32_t,
                                                dsd_input_level_cu8_moments*);
using bound_cf32_fn = void (*)(const void*, float*, size_t);
using bound_rot_cf32_fn = uint32_t (*)(const void*, float*, size_t, uint32_t);

#if defined(__x86_64__) || defined(_M_X64)
extern "C" void widen_u8_to_f32_bias127_moments_sse2(const unsigned char* src, float* dst, uint32_t len,
                                                     dsd_input_level_cu8_moments* moments);
extern "C" uint32_t widen_rotate90_u8_to_f32_bias127_phase_sse2(const unsigned char* src, float* dst, uint32_t len,
                                                                uint32_t phase);
extern "C" uint32_t widen_rotate90_u8_to_f32_bias127_phase_moments_sse2(const unsigned char* src, float* dst,
                                                                        uint32_t len, uint32_t phase,
                                                                        dsd_input_level_cu8_moments* moments);
extern "C" void bound_cf32_to_f32_sse2(const void* src, float* dst, size_t pairs);
extern "C" uint32_t bound_rotate90_cf32_to_f32_phase_sse2(const void* src, float* dst, size_t pairs, uint32_t phase);
#if defined(DSD_NEO_DSP_HAVE_AVX2_IMPL) && DSD_NEO_X86_AVX2_RUNTIME_PROBE_SUPPORTED
extern "C" void widen_u8_to_f32_bias127_moments_avx2(const unsigned char* src, float* dst, uint32_t len,
                                                     dsd_input_level_cu8_moments* moments);
extern "C" uint32_t widen_rotate90_u8_to_f32_bias127_phase_avx2(const unsigned char* src, float* dst, uint32_t len,
                                                                uint32_t phase);
extern "C" uint32_t widen_rotate90_u8_to_f32_bias127_phase_moments_avx2(const unsigned char* src, float* dst,
                                                                        uint32_t len, uint32_t phase,
                                                                        dsd_input_level_cu8_moments* moments);
extern "C" void bound_cf32_to_f32_avx2(const void* src, float* dst, size_t pairs);
extern "C" uint32_t bound_rotate90_cf32_to_f32_phase_avx2(const void* src, float* dst, size_t pairs, uint32_t phase);
#endif
#endif

#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
extern "C" void widen_u8_to_f32_bias127_moments_neon(const unsigned char* src, float* dst, uint32_t len,
                                                     dsd_input_level_cu8_moments* moments);
extern "C" uint32_t widen_rotate90_u8_to_f32_bias127_phase_neon(const unsigned char* src, float* dst, uint32_t len,
                                                                uint32_t phase);
extern "C" uint32_t widen_rotate90_u8_to_f32_bias127_phase_moments_neon(const unsigned char* src, float* dst,
                                                                        uint32_t len, uint32_t phase,
                                                                        dsd_input_level_cu8_moments* moments);
extern "C" void bound_cf32_to_f32_neon(const void* src, float* dst, size_t pairs);
extern "C" uint32_t bound_rotate90_cf32_to_f32_phase_neon(const void* src, float* dst, size_t pairs, uint32_t phase);
#endif

static void widen_u8_to_f32_bias127_moments_scalar(const unsigned char* src, float* dst, uint32_t len,
                                                   dsd_input_level_cu8_moments* moments);
static uint32_t widen_rotate90_u8_to_f32_bias127_phase_scalar(const unsigned char* src, float* dst, uint32_t len,
                                                              uint32_t phase);
static uint32_t widen_rotate90_u8_to_f32_bias127_phase_moments_scalar(const unsigned char* src, float* dst,
                                                                      uint32_t len, uint32_t phase,
                                                                      dsd_input_level_cu8_moments* moments);
static void bound_cf32_to_f32_scalar(const void* src, float* dst, size_t pairs);
static uint32_t bound_rotate90_cf32_to_f32_phase_scalar(const void* src, float* dst, size_t pairs, uint32_t phase);

static widen_rot_phase_fn g_widen_rot_phase_impl = widen_rotate90_u8_to_f32_bias127_phase_scalar;
static widen_moments_fn g_widen_moments_impl = widen_u8_to_f32_bias127_moments_scalar;
static widen_rot_phase_moments_fn g_widen_rot_phase_moments_impl =
    widen_rotate90_u8_to_f32_bias127_phase_moments_scalar;
static bound_cf32_fn g_bound_cf32_impl = bound_cf32_to_f32_scalar;
static bound_rot_cf32_fn g_bound_rot_cf32_impl = bound_rotate90_cf32_to_f32_phase_scalar;
static std::atomic<int> g_widen_init_done{0};

static void
simd_widen_init_dispatch(void) {
    int expected = 0;
    if (!g_widen_init_done.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        while (g_widen_init_done.load(std::memory_order_acquire) != 2) {
            /* spin */
        }
        return;
    }

#if defined(__x86_64__) || defined(_M_X64)
#if defined(DSD_NEO_DSP_HAVE_AVX2_IMPL) && DSD_NEO_X86_AVX2_RUNTIME_PROBE_SUPPORTED
    if (dsd_neo_cpu_has_avx2_with_os_support()) {
        g_widen_rot_phase_impl = widen_rotate90_u8_to_f32_bias127_phase_avx2;
        g_widen_moments_impl = widen_u8_to_f32_bias127_moments_avx2;
        g_widen_rot_phase_moments_impl = widen_rotate90_u8_to_f32_bias127_phase_moments_avx2;
        g_bound_cf32_impl = bound_cf32_to_f32_avx2;
        g_bound_rot_cf32_impl = bound_rotate90_cf32_to_f32_phase_avx2;
    } else
#endif
    {
        g_widen_rot_phase_impl = widen_rotate90_u8_to_f32_bias127_phase_sse2;
        g_widen_moments_impl = widen_u8_to_f32_bias127_moments_sse2;
        g_widen_rot_phase_moments_impl = widen_rotate90_u8_to_f32_bias127_phase_moments_sse2;
        g_bound_cf32_impl = bound_cf32_to_f32_sse2;
        g_bound_rot_cf32_impl = bound_rotate90_cf32_to_f32_phase_sse2;
    }
#elif defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    g_widen_rot_phase_impl = widen_rotate90_u8_to_f32_bias127_phase_neon;
    g_widen_moments_impl = widen_u8_to_f32_bias127_moments_neon;
    g_widen_rot_phase_moments_impl = widen_rotate90_u8_to_f32_bias127_phase_moments_neon;
    g_bound_cf32_impl = bound_cf32_to_f32_neon;
    g_bound_rot_cf32_impl = bound_rotate90_cf32_to_f32_phase_neon;
#endif

    g_widen_init_done.store(2, std::memory_order_release);
}

static inline void
accumulate_cu8_local(dsd_input_level_cu8_moments* local, unsigned char sample) {
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

void
widen_u8_to_f32_bias127(const unsigned char* src, float* dst, uint32_t len) {
    if (!src || !dst || len == 0U) {
        return;
    }
    const float inv = 1.0f / 127.5f;
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = ((float)src[i] - 127.5f) * inv;
    }
}

static void
widen_u8_to_f32_bias127_moments_scalar(const unsigned char* src, float* dst, uint32_t len,
                                       dsd_input_level_cu8_moments* moments) {
    if (!src || !dst || !moments || len == 0U) {
        return;
    }
    dsd_input_level_cu8_moments local;
    dsd_input_level_cu8_moments_reset(&local);
    local.count = len;
    const float inv = 1.0f / 127.5f;
    for (uint32_t i = 0; i < len; i++) {
        const unsigned char sample = src[i];
        dst[i] = ((float)sample - 127.5f) * inv;
        accumulate_cu8_local(&local, sample);
    }
    (void)dsd_input_level_cu8_moments_merge(moments, &local);
}

static uint32_t
widen_rotate90_u8_to_f32_bias127_phase_moments_scalar(const unsigned char* src, float* dst, uint32_t len,
                                                      uint32_t phase, dsd_input_level_cu8_moments* moments) {
    uint32_t cur_phase = phase & 3U;
    if (!src || !dst || len < 2U) {
        return cur_phase;
    }

    const float inv = 1.0f / 127.5f;
    const uint32_t pairs = len >> 1;
    dsd_input_level_cu8_moments local;
    if (moments) {
        dsd_input_level_cu8_moments_reset(&local);
        local.count = (uint64_t)pairs * 2U;
    }
    for (uint32_t n = 0; n < pairs; n++) {
        const uint32_t idx = n << 1;
        const unsigned char in_i = src[idx + 0U];
        const unsigned char in_q = src[idx + 1U];
        const float i_raw = ((float)in_i - 127.5f) * inv;
        const float q_raw = ((float)in_q - 127.5f) * inv;
        apply_j4_rotation_f32(i_raw, q_raw, cur_phase, &dst[idx + 0U], &dst[idx + 1U]);
        if (moments) {
            accumulate_cu8_local(&local, in_i);
            accumulate_cu8_local(&local, in_q);
        }
        cur_phase = (cur_phase + 1U) & 3U;
    }
    if (moments) {
        (void)dsd_input_level_cu8_moments_merge(moments, &local);
    }
    return cur_phase;
}

static uint32_t
widen_rotate90_u8_to_f32_bias127_phase_scalar(const unsigned char* src, float* dst, uint32_t len, uint32_t phase) {
    return widen_rotate90_u8_to_f32_bias127_phase_moments_scalar(src, dst, len, phase, nullptr);
}

namespace {
/* The bounded cf32 copy's period kernel for targets with no SIMD backend. Scalar code gains nothing from checking
   apart: the periods go in bounded, and nothing is left to report. */
struct cf32_period_scalar {
    using period_copy = cf32_periods_bounded;

    template <bool kRotate>
    static inline void
    copy(float* dst, const unsigned char* src) {
        uint32_t lanes[8];
        DSD_MEMCPY(lanes, src, sizeof(lanes));
        std::transform(std::begin(lanes), std::end(lanes), std::begin(lanes),
                       [](uint32_t bits) { return cf32_bound_bits(bits); });
        if (kRotate) {
            uint32_t out[8] = {lanes[0], lanes[1], lanes[3], lanes[2], lanes[4], lanes[5], lanes[7], lanes[6]};
            for (size_t k = 0U; k < 8U; k++) {
                out[k] ^= kCf32PeriodSigns[k];
            }
            DSD_MEMCPY(dst, out, sizeof(out));
        } else {
            DSD_MEMCPY(dst, lanes, sizeof(lanes));
        }
    }

    static inline void
    copy_periods(float* dst, const unsigned char* src, size_t periods) {
        for (size_t k = 0U; k < periods; k++) {
            copy<false>(dst + (k * 8U), src + (k * 32U));
        }
    }
};
} /* namespace */

static void
bound_cf32_to_f32_scalar(const void* src, float* dst, size_t pairs) {
    (void)cf32_copy_bounded<cf32_period_scalar>(dst, src, pairs, false, 0U);
}

static uint32_t
bound_rotate90_cf32_to_f32_phase_scalar(const void* src, float* dst, size_t pairs, uint32_t phase) {
    return cf32_copy_bounded<cf32_period_scalar>(dst, src, pairs, true, phase);
}

#ifdef DSD_NEO_TEST_HOOKS
extern "C" void
dsd_test_widen_u8_to_f32_bias127_moments_scalar(const unsigned char* src, float* dst, uint32_t len,
                                                dsd_input_level_cu8_moments* moments) {
    widen_u8_to_f32_bias127_moments_scalar(src, dst, len, moments);
}

extern "C" uint32_t
dsd_test_widen_rotate90_u8_to_f32_bias127_phase_scalar(const unsigned char* src, float* dst, uint32_t len,
                                                       uint32_t phase) {
    return widen_rotate90_u8_to_f32_bias127_phase_scalar(src, dst, len, phase);
}

extern "C" uint32_t
dsd_test_widen_rotate90_u8_to_f32_bias127_phase_moments_scalar(const unsigned char* src, float* dst, uint32_t len,
                                                               uint32_t phase, dsd_input_level_cu8_moments* moments) {
    return widen_rotate90_u8_to_f32_bias127_phase_moments_scalar(src, dst, len, phase, moments);
}

extern "C" void
dsd_test_bound_cf32_to_f32_scalar(const void* src, float* dst, size_t pairs) {
    bound_cf32_to_f32_scalar(src, dst, pairs);
}

extern "C" uint32_t
dsd_test_bound_rotate90_cf32_to_f32_phase_scalar(const void* src, float* dst, size_t pairs, uint32_t phase) {
    return bound_rotate90_cf32_to_f32_phase_scalar(src, dst, pairs, phase);
}
#endif

void
widen_u8_to_f32_bias127_moments(const unsigned char* src, float* dst, uint32_t len,
                                dsd_input_level_cu8_moments* moments) {
    if (g_widen_init_done.load(std::memory_order_acquire) != 2) {
        simd_widen_init_dispatch();
    }
    g_widen_moments_impl(src, dst, len, moments);
}

uint32_t
widen_rotate90_u8_to_f32_bias127_phase(const unsigned char* src, float* dst, uint32_t len, uint32_t phase) {
    if (g_widen_init_done.load(std::memory_order_acquire) != 2) {
        simd_widen_init_dispatch();
    }
    return g_widen_rot_phase_impl(src, dst, len, phase);
}

uint32_t
widen_rotate90_u8_to_f32_bias127_phase_moments(const unsigned char* src, float* dst, uint32_t len, uint32_t phase,
                                               dsd_input_level_cu8_moments* moments) {
    if (g_widen_init_done.load(std::memory_order_acquire) != 2) {
        simd_widen_init_dispatch();
    }
    return g_widen_rot_phase_moments_impl(src, dst, len, phase, moments);
}

void
bound_cf32_to_f32(const void* src, float* dst, size_t pairs) {
    if (g_widen_init_done.load(std::memory_order_acquire) != 2) {
        simd_widen_init_dispatch();
    }
    g_bound_cf32_impl(src, dst, pairs);
}

uint32_t
bound_rotate90_cf32_to_f32_phase(const void* src, float* dst, size_t pairs, uint32_t phase) {
    if (g_widen_init_done.load(std::memory_order_acquire) != 2) {
        simd_widen_init_dispatch();
    }
    return g_bound_rot_cf32_impl(src, dst, pairs, phase);
}
