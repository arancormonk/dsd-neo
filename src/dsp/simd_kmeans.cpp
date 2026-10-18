// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <atomic>
#include <stddef.h>

#include "simd_kmeans_internal.h"
#include "simd_x86_cpu.h"

template <int Levels>
static void
kmeans_scalar(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    *out = simd_kmeans_pass{};
    simd_kmeans_tail<Levels>(vals, 0, count, centers, with_noise, out);
}

void
simd_kmeans4_scalar(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    kmeans_scalar<4>(vals, count, centers, with_noise, out);
}

void
simd_kmeans2_scalar(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    kmeans_scalar<2>(vals, count, centers, with_noise, out);
}

using kmeans_fn = void (*)(const float*, size_t, const double*, bool, simd_kmeans_pass*);

static kmeans_fn g_kmeans4 = simd_kmeans4_scalar;
static kmeans_fn g_kmeans2 = simd_kmeans2_scalar;
static std::atomic<int> g_kmeans_init{0};

static void
simd_kmeans_init(void) {
    int expected = 0;
    if (!g_kmeans_init.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        while (g_kmeans_init.load(std::memory_order_acquire) != 2) {
            /* The initializing thread publishes both function pointers together. */
        }
        return;
    }
#if defined(__x86_64__) || defined(_M_X64)
#if defined(DSD_NEO_DSP_HAVE_AVX2_IMPL) && DSD_NEO_X86_AVX2_RUNTIME_PROBE_SUPPORTED
    if (dsd_neo_cpu_has_avx2_with_os_support()) {
        g_kmeans4 = simd_kmeans4_avx2;
        g_kmeans2 = simd_kmeans2_avx2;
    } else
#endif
    {
        g_kmeans4 = simd_kmeans4_sse2;
        g_kmeans2 = simd_kmeans2_sse2;
    }
#elif defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    g_kmeans4 = simd_kmeans4_neon;
    g_kmeans2 = simd_kmeans2_neon;
#endif
    g_kmeans_init.store(2, std::memory_order_release);
}

void
simd_kmeans4(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    if (g_kmeans_init.load(std::memory_order_acquire) != 2) {
        simd_kmeans_init();
    }
    g_kmeans4(vals, count, centers, with_noise, out);
}

void
simd_kmeans2(const float* vals, size_t count, const double* centers, bool with_noise, simd_kmeans_pass* out) {
    if (g_kmeans_init.load(std::memory_order_acquire) != 2) {
        simd_kmeans_init();
    }
    g_kmeans2(vals, count, centers, with_noise, out);
}
