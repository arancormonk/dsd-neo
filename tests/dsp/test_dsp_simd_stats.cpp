// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/simd_stats.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "dsd-neo/core/safe_api.h"
#include "dsp/simd_stats_internal.h"
#if defined(__x86_64__) || defined(_M_X64)
#include "dsp/simd_x86_cpu.h"
#endif

using extrema_fn = simd_stats_extrema (*)(const float*, size_t);
using iq_fn = simd_stats_iq_abs (*)(const float*, size_t);
using rank5_fn = void (*)(const float*, size_t, float*, float*);

static uint32_t
bits_of(float x) {
    uint32_t bits = 0;
    DSD_MEMCPY(&bits, &x, sizeof bits);
    return bits;
}

static float
from_bits(uint32_t bits) {
    float x = 0.0f;
    DSD_MEMCPY(&x, &bits, sizeof x);
    return x;
}

static bool
same_float(float expected, float actual) {
    return (std::isnan(expected) && std::isnan(actual)) || bits_of(expected) == bits_of(actual);
}

/* Independent ordered reference, including the historical comparisons when
 * either of the first two values is NaN. The finite oracle below also sorts. */
static simd_stats_extrema
ordered_extrema(const float* x, size_t n) {
    float low[2] = {x[0], x[1]};
    float high[2] = {x[0], x[1]};
    if (low[1] < low[0]) {
        std::swap(low[0], low[1]);
    }
    if (high[1] > high[0]) {
        std::swap(high[0], high[1]);
    }
    for (size_t i = 2; i < n; ++i) {
        if (x[i] < low[0]) {
            low[1] = low[0];
            low[0] = x[i];
        } else if (x[i] < low[1]) {
            low[1] = x[i];
        }
        if (x[i] > high[0]) {
            high[1] = high[0];
            high[0] = x[i];
        } else if (x[i] > high[1]) {
            high[1] = x[i];
        }
    }
    return {low[0], low[1], high[0], high[1]};
}

static int
check_extrema(const char* name, extrema_fn fn, const float* x, size_t n, bool finite_sorted_oracle) {
    simd_stats_extrema want = ordered_extrema(x, n);
    if (finite_sorted_oracle) {
        std::vector<float> sorted(x, x + n);
        std::sort(sorted.begin(), sorted.end());
        want = {sorted[0], sorted[1], sorted[n - 1], sorted[n - 2]};
    }
    const simd_stats_extrema got = fn(x, n);
    if (!same_float(want.min1, got.min1) || !same_float(want.min2, got.min2) || !same_float(want.max1, got.max1)
        || !same_float(want.max2, got.max2)) {
        DSD_FPRINTF(stderr, "%s extrema mismatch, count=%zu\n", name, n);
        return 1;
    }
    float low = 0.0f, high = 0.0f;
    dsd_simd_extrema_avg2(x, n, &low, &high);
    if (!same_float((want.min1 + want.min2) * 0.5f, low) || !same_float((want.max1 + want.max2) * 0.5f, high)) {
        DSD_FPRINTF(stderr, "dispatched extrema mismatch, count=%zu\n", n);
        return 1;
    }
    return 0;
}

static int
check_iq(const char* name, iq_fn fn, const float* x, size_t pairs) {
    long double exact_sum = 0.0L;
    float peak = 0.0f;
    for (size_t i = 0; i < pairs; ++i) {
        const float ai = std::fabs(x[2 * i]);
        const float aq = std::fabs(x[2 * i + 1]);
        const float p = ai > aq ? ai : aq;
        if (p > peak) {
            peak = p;
        }
        exact_sum += (long double)ai + (long double)aq;
    }
    const simd_stats_iq_abs got = fn(x, pairs);
    const double want = (double)exact_sum;
    bool sum_ok = false;
    if (std::isnan(want)) {
        sum_ok = std::isnan(got.sum_abs);
    } else if (std::isinf(want)) {
        sum_ok = std::isinf(got.sum_abs) && !std::signbit(got.sum_abs);
    } else {
        sum_ok = std::fabs(got.sum_abs - want) <= 1e-12 * std::max(1.0, want);
    }
    float mean = 0.0f, maximum = 0.0f;
    dsd_simd_iq_abs_stats(x, pairs, &mean, &maximum);
    const float want_mean = (float)(exact_sum / (long double)pairs);
    const bool mean_ok = same_float(want_mean, mean)
                         || (std::isfinite(want_mean) && std::isfinite(mean)
                             && std::fabs(want_mean - mean) <= 2e-7f * std::max(1.0f, want_mean));
    if (!sum_ok || !same_float(peak, got.max_abs) || !same_float(peak, maximum) || !mean_ok) {
        DSD_FPRINTF(stderr, "%s I/Q mismatch, pairs=%zu\n", name, pairs);
        return 1;
    }
    return 0;
}

static int
test_backend(const char* name, extrema_fn extrema, iq_fn iq) {
    uint32_t rng = 0x76d513U;
    for (size_t offset = 0; offset < 8; ++offset) {
        for (size_t count = 2; count <= 257; ++count) {
            std::unique_ptr<float[]> storage(new float[offset + count]);
            float* x = storage.get() + offset;
            for (int shape = 0; shape < 6; ++shape) {
                for (size_t i = 0; i < count; ++i) {
                    rng = rng * 1664525U + 1013904223U;
                    switch (shape) {
                        case 0: x[i] = (float)((int)(rng >> 16) - 32768) / 32768.0f; break;
                        case 1: x[i] = (float)i; break;
                        case 2: x[i] = -(float)i; break;
                        case 3: x[i] = (float)((int)(rng >> 30) * 2 - 3); break;
                        case 4: x[i] = 0.0f; break;
                        default: x[i] = (i & 1U) ? 1e30f : -1e-30f; break;
                    }
                }
                /* The descending sequence starts at -0; retain its ordered tie semantics. */
                if (check_extrema(name, extrema, x, count, shape != 2) || check_iq(name, iq, x, count / 2)) {
                    return 1;
                }
            }
        }
    }

    const uint32_t exceptional[] = {0x80000000U, 0x7fc12345U, 0x7f800000U, 0xff800000U};
    for (uint32_t bits : exceptional) {
        for (size_t position = 0; position < 65; ++position) {
            float x[66];
            for (size_t i = 0; i < 66; ++i) {
                x[i] = (float)((int)(i % 9) - 4);
            }
            x[position] = from_bits(bits);
            if (check_extrema(name, extrema, x, 65, false) || check_iq(name, iq, x, 33)) {
                return 1;
            }
        }
    }
    /* Exact-sized larger input exercises double accumulation beyond the small windows. */
    std::vector<float> large(16384);
    for (size_t i = 0; i < large.size(); ++i) {
        large[i] = (i & 1U) ? std::numeric_limits<float>::max() : std::numeric_limits<float>::min();
    }
    return check_iq(name, iq, large.data(), large.size() / 2);
}

static int
test_concurrent_dispatch(void) {
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    std::thread workers[4];
    for (auto& worker : workers) {
        worker = std::thread([&start, &failures]() {
            const float x[8] = {-3, -3, -1, -1, 1, 1, 3, 3};
            while (!start.load(std::memory_order_acquire)) {}
            for (int i = 0; i < 50; ++i) {
                float low = 0.0f, high = 0.0f, mean = 0.0f, peak = 0.0f;
                dsd_simd_extrema_avg2(x, 8, &low, &high);
                dsd_simd_iq_abs_stats(x, 4, &mean, &peak);
                if (std::fabs(low + 3) > 1e-7f || std::fabs(high - 3) > 1e-7f || std::fabs(mean - 4) > 1e-7f
                    || std::fabs(peak - 3) > 1e-7f) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    return failures.load(std::memory_order_relaxed) != 0 ? 1 : 0;
}

static int
test_empty_and_optional_outputs(void) {
    const float x[2] = {-1.0f, 3.0f};
    float low = 9.0f, high = 9.0f;
    dsd_simd_extrema_avg2(nullptr, 128, &low, &high);
    if (std::fabs(low) > 1e-7f || std::fabs(high) > 1e-7f) {
        return 1;
    }
    dsd_simd_extrema_avg2(x, 1, &low, &high);
    if (std::fabs(low) > 1e-7f || std::fabs(high) > 1e-7f) {
        return 1;
    }
    dsd_simd_extrema_avg2(x, 2, &low, nullptr);
    dsd_simd_extrema_avg2(x, 2, nullptr, &high);
    if (std::fabs(low - 1) > 1e-7f || std::fabs(high - 1) > 1e-7f) {
        return 1;
    }
    dsd_simd_iq_abs_stats(nullptr, 128, &low, &high);
    dsd_simd_iq_abs_stats(x, 0, &low, &high);
    if (std::fabs(low) > 1e-7f || std::fabs(high) > 1e-7f) {
        return 1;
    }
    dsd_simd_iq_abs_stats(x, 1, &low, nullptr);
    dsd_simd_iq_abs_stats(x, 1, nullptr, &high);
    return std::fabs(low - 4) > 1e-7f || std::fabs(high - 3) > 1e-7f ? 1 : 0;
}

namespace {
struct rank5_result {
    float lmn[5];
    float lmx[5];
};
} // namespace

/* Independent ordered reference: strict comparisons against FLT_MAX / -FLT_MAX
 * seeds, so NaN is never kept and equal values keep their input order. */
static rank5_result
ordered_rank5(const float* x, size_t n) {
    rank5_result r = {{FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX},
                      {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX}};
    for (size_t i = 0; i < n; ++i) {
        for (int j = 0; j < 5; ++j) {
            if (x[i] < r.lmn[j]) {
                for (int m = 4; m > j; --m) {
                    r.lmn[m] = r.lmn[m - 1];
                }
                r.lmn[j] = x[i];
                break;
            }
        }
        for (int j = 0; j < 5; ++j) {
            if (x[i] > r.lmx[j]) {
                for (int m = 4; m > j; --m) {
                    r.lmx[m] = r.lmx[m - 1];
                }
                r.lmx[j] = x[i];
                break;
            }
        }
    }
    return r;
}

static int
check_rank5(const char* name, rank5_fn fn, const float* x, size_t n, bool finite_sorted_oracle) {
    rank5_result want = ordered_rank5(x, n);
    if (finite_sorted_oracle) {
        std::vector<float> sorted(x, x + n);
        std::sort(sorted.begin(), sorted.end());
        for (size_t j = 0; j < 5; ++j) {
            want.lmn[j] = j < n ? sorted[j] : FLT_MAX;
            want.lmx[j] = j < n ? sorted[n - 1 - j] : -FLT_MAX;
        }
    }
    rank5_result got;
    fn(x, n, got.lmn, got.lmx);
    float lmn[5], lmx[5];
    dsd_simd_extrema_rank5(x, n, lmn, lmx);
    for (int j = 0; j < 5; ++j) {
        if (bits_of(want.lmn[j]) != bits_of(got.lmn[j]) || bits_of(want.lmx[j]) != bits_of(got.lmx[j])) {
            DSD_FPRINTF(stderr, "%s rank5 mismatch, count=%zu slot=%d\n", name, n, j);
            return 1;
        }
        if (bits_of(want.lmn[j]) != bits_of(lmn[j]) || bits_of(want.lmx[j]) != bits_of(lmx[j])) {
            DSD_FPRINTF(stderr, "dispatched rank5 mismatch, count=%zu slot=%d\n", n, j);
            return 1;
        }
    }
    return 0;
}

static int
test_rank5_backend(const char* name, rank5_fn fn) {
    uint32_t rng = 0x5a17c3U;
    for (size_t offset = 0; offset < 8; ++offset) {
        for (size_t count = 1; count <= 80; ++count) {
            std::unique_ptr<float[]> storage(new float[offset + count]);
            float* x = storage.get() + offset;
            for (int shape = 0; shape < 9; ++shape) {
                for (size_t i = 0; i < count; ++i) {
                    rng = rng * 1664525U + 1013904223U;
                    switch (shape) {
                        case 0: x[i] = (float)((int)(rng >> 16) - 32768) / 32768.0f; break;
                        case 1: x[i] = (float)i + 1.0f; break;
                        case 2: x[i] = -(float)i - 1.0f; break;
                        case 3: x[i] = (float)((int)(rng >> 30) * 2 - 3); break;
                        case 4: x[i] = 0.0f; break;
                        case 5: x[i] = (i & 1U) ? 1e30f : -1e-30f; break;
                        case 6: x[i] = (float)((int)(rng >> 29) - 4) + ((rng >> 8) & 1U ? 0.125f : 0.0f); break;
                        case 7: x[i] = (rng >> 31) ? FLT_MAX : -FLT_MAX; break;
                        default: x[i] = (float)((int)(rng >> 16) - 32768) * 1e-38f; break;
                    }
                }
                if (check_rank5(name, fn, x, count, true)) {
                    return 1;
                }
            }
        }
    }

    /* Exceptional values at every position keep the ordered comparison semantics,
     * including -0 beside +0 ties and infinities against the seeds. */
    const uint32_t exceptional[] = {0x80000000U, 0x7fc12345U, 0xffc00001U, 0x7f800000U, 0xff800000U};
    for (uint32_t bits : exceptional) {
        for (size_t count = 1; count <= 49; ++count) {
            for (size_t position = 0; position < count; ++position) {
                float x[49];
                for (size_t i = 0; i < count; ++i) {
                    x[i] = (i % 3U == 0U) ? 0.0f : (float)((int)(i % 7U) - 3);
                }
                x[position] = from_bits(bits);
                if (check_rank5(name, fn, x, count, false)) {
                    return 1;
                }
            }
        }
    }
    /* Signed-zero-only windows: order of -0 and +0 among the retained ties. */
    for (size_t count = 1; count <= 24; ++count) {
        for (uint32_t pattern = 0; pattern < 64; ++pattern) {
            float x[24];
            for (size_t i = 0; i < count; ++i) {
                x[i] = ((pattern >> (i % 6U)) & 1U) ? -0.0f : 0.0f;
            }
            if (check_rank5(name, fn, x, count, false)) {
                return 1;
            }
        }
    }
    return 0;
}

static int
test_rank5_empty_and_optional_outputs(void) {
    const float x[3] = {2.0f, -1.0f, 3.0f};
    float lmn[5] = {0}, lmx[5] = {0};
    dsd_simd_extrema_rank5(nullptr, 3, lmn, lmx);
    for (int j = 0; j < 5; ++j) {
        if (bits_of(lmn[j]) != bits_of(FLT_MAX) || bits_of(lmx[j]) != bits_of(-FLT_MAX)) {
            return 1;
        }
    }
    DSD_MEMSET(lmn, 0, sizeof lmn);
    dsd_simd_extrema_rank5(x, 0, lmn, nullptr);
    dsd_simd_extrema_rank5(x, 3, nullptr, lmx);
    const float want_max[5] = {3.0f, 2.0f, -1.0f, -FLT_MAX, -FLT_MAX};
    for (int j = 0; j < 5; ++j) {
        if (bits_of(lmn[j]) != bits_of(FLT_MAX) || bits_of(lmx[j]) != bits_of(want_max[j])) {
            return 1;
        }
    }
    return 0;
}

int
main(void) {
    int rc = test_concurrent_dispatch();
    rc |= test_empty_and_optional_outputs();
    rc |= test_backend("scalar", simd_stats_extrema_scalar, simd_stats_iq_abs_scalar);
    rc |= test_rank5_empty_and_optional_outputs();
    rc |= test_rank5_backend("scalar", simd_stats_rank5_scalar);
#if defined(__x86_64__) || defined(_M_X64)
    rc |= test_backend("SSE2", simd_stats_extrema_sse2, simd_stats_iq_abs_sse2);
    rc |= test_rank5_backend("SSE2", simd_stats_rank5_sse2);
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL) && DSD_NEO_X86_AVX2_RUNTIME_PROBE_SUPPORTED
    if (dsd_neo_cpu_has_avx2_with_os_support()) {
        rc |= test_backend("AVX2", simd_stats_extrema_avx2, simd_stats_iq_abs_avx2);
        rc |= test_rank5_backend("AVX2", simd_stats_rank5_avx2);
    }
#endif
#elif defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    rc |= test_backend("NEON", simd_stats_extrema_neon, simd_stats_iq_abs_neon);
    rc |= test_rank5_backend("NEON", simd_stats_rank5_neon);
#endif
    if (rc == 0) {
        std::puts("DSP_SIMD_STATS: OK");
    }
    return rc;
}
