// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/simd_stats.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <stddef.h>
#include <stdint.h>

#include "simd_stats_internal.h"
#include "simd_x86_cpu.h"

simd_stats_extrema
simd_stats_extrema_scalar(const float* samples, size_t count) {
    simd_stats_extrema r = simd_stats_extrema_seed(samples[0], samples[1]);
    for (size_t k = 2; k < count; ++k) {
        simd_stats_track_low(&r, samples[k]);
        simd_stats_track_high(&r, samples[k]);
    }
    return r;
}

simd_stats_iq_abs
simd_stats_iq_abs_scalar(const float* iq, size_t pairs) {
    simd_stats_iq_abs r = {0.0, 0.0f};
    for (size_t k = 0; k < pairs; ++k) {
        const float ai = std::fabs(iq[2 * k]);
        const float aq = std::fabs(iq[2 * k + 1]);
        const float peak = (ai > aq) ? ai : aq;
        if (peak > r.max_abs) {
            r.max_abs = peak;
        }
        r.sum_abs += (double)ai + (double)aq;
    }
    return r;
}

void
simd_stats_rank5_scalar(const float* samples, size_t count, float* lmn, float* lmx) {
    float low[5] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    float high[5] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t k = 0; k < count; ++k) {
        const float x = samples[k];
        if (x < low[4]) {
            int j = 4;
            for (; j > 0 && x < low[j - 1]; j--) {
                low[j] = low[j - 1];
            }
            low[j] = x;
        }
        if (x > high[4]) {
            int j = 4;
            for (; j > 0 && x > high[j - 1]; j--) {
                high[j] = high[j - 1];
            }
            high[j] = x;
        }
    }
    for (int k = 0; k < 5; ++k) {
        lmn[k] = low[k];
        lmx[k] = high[k];
    }
}

using extrema_fn = simd_stats_extrema (*)(const float*, size_t);
using iq_abs_fn = simd_stats_iq_abs (*)(const float*, size_t);
using rank5_fn = void (*)(const float*, size_t, float*, float*);

static extrema_fn g_extrema = simd_stats_extrema_scalar;
static iq_abs_fn g_iq_abs = simd_stats_iq_abs_scalar;
static rank5_fn g_rank5 = simd_stats_rank5_scalar;
static rank5_fn g_rank5_wide = simd_stats_rank5_scalar;
static std::atomic<int> g_stats_init{0};

static void
simd_stats_init(void) {
    int expected = 0;
    if (!g_stats_init.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        while (g_stats_init.load(std::memory_order_acquire) != 2) {
            /* The initializing thread publishes all function pointers together. */
        }
        return;
    }
#if defined(__x86_64__) || defined(_M_X64)
#if defined(DSD_NEO_DSP_HAVE_AVX2_IMPL) && DSD_NEO_X86_AVX2_RUNTIME_PROBE_SUPPORTED
    if (dsd_neo_cpu_has_avx2_with_os_support()) {
        g_extrema = simd_stats_extrema_avx2;
        g_iq_abs = simd_stats_iq_abs_avx2;
        g_rank5 = simd_stats_rank5_sse2;
        g_rank5_wide = simd_stats_rank5_avx2;
    } else
#endif
    {
        g_extrema = simd_stats_extrema_sse2;
        g_iq_abs = simd_stats_iq_abs_sse2;
        g_rank5 = simd_stats_rank5_sse2;
        g_rank5_wide = simd_stats_rank5_sse2;
    }
#elif defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    g_extrema = simd_stats_extrema_neon;
    g_iq_abs = simd_stats_iq_abs_neon;
    g_rank5 = simd_stats_rank5_neon;
    g_rank5_wide = simd_stats_rank5_neon;
#endif
    g_stats_init.store(2, std::memory_order_release);
}

extern "C" void
dsd_simd_extrema_avg2(const float* samples, size_t count, float* out_min, float* out_max) {
    float low = 0.0f;
    float high = 0.0f;
    if (samples && count >= 2) {
        if (g_stats_init.load(std::memory_order_acquire) != 2) {
            simd_stats_init();
        }
        const simd_stats_extrema r = g_extrema(samples, count);
        low = (r.min1 + r.min2) * 0.5f;
        high = (r.max1 + r.max2) * 0.5f;
    }
    if (out_min) {
        *out_min = low;
    }
    if (out_max) {
        *out_max = high;
    }
}

extern "C" void
dsd_simd_iq_abs_stats(const float* iq, size_t pairs, float* mean_abs, float* max_abs) {
    float mean = 0.0f;
    float peak = 0.0f;
    if (iq && pairs > 0 && pairs <= SIZE_MAX / 2) {
        if (g_stats_init.load(std::memory_order_acquire) != 2) {
            simd_stats_init();
        }
        const simd_stats_iq_abs r = g_iq_abs(iq, pairs);
        mean = (float)(r.sum_abs / (double)pairs);
        peak = r.max_abs;
    }
    if (mean_abs) {
        *mean_abs = mean;
    }
    if (max_abs) {
        *max_abs = peak;
    }
}

extern "C" void
dsd_simd_extrema_rank5(const float* samples, size_t count, float* lmn, float* lmx) {
    float scratch_low[5];
    float scratch_high[5];
    float* low = lmn ? lmn : scratch_low;
    float* high = lmx ? lmx : scratch_high;
    if (!samples || count == 0) {
        for (int k = 0; k < 5; ++k) {
            low[k] = FLT_MAX;
            high[k] = -FLT_MAX;
        }
        return;
    }
    if (g_stats_init.load(std::memory_order_acquire) != 2) {
        simd_stats_init();
    }
    (count < SIMD_STATS_RANK5_AVX2_MIN_COUNT ? g_rank5 : g_rank5_wide)(samples, count, low, high);
}
