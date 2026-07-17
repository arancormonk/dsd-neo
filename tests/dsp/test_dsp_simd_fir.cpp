// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for SIMD FIR filter functions.
 *
 * Tests compare SIMD implementations against scalar reference for:
 * - Complex symmetric FIR filter (channel LPF)
 * - Complex half-band decimator
 * - Real half-band decimator
 *
 * Covers edge cases: small blocks, odd lengths, history continuity, alignment.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/simd_fir.h>
#include <memory>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "dsp/simd_fir_internal.h"

#if defined(__x86_64__) || defined(_M_X64)
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL)
#include "dsp/simd_x86_cpu.h"
#endif
extern "C" void simd_fir_complex_apply_sse2(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                            const float* taps, int taps_len);
extern "C" int simd_hb_decim2_complex_sse2(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                           const float* taps, int taps_len);
extern "C" int simd_hb_decim2_real_sse2(const float* in, int in_len, float* out, float* hist, const float* taps,
                                        int taps_len);
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL)
extern "C" void simd_fir_complex_apply_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                            const float* taps, int taps_len);
extern "C" int simd_hb_decim2_complex_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                           const float* taps, int taps_len);
extern "C" int simd_hb_decim2_real_avx2(const float* in, int in_len, float* out, float* hist, const float* taps,
                                        int taps_len);
#endif
#endif

#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
extern "C" void simd_fir_complex_apply_neon(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                            const float* taps, int taps_len);
extern "C" int simd_hb_decim2_complex_neon(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                           const float* taps, int taps_len);
extern "C" int simd_hb_decim2_real_neon(const float* in, int in_len, float* out, float* hist, const float* taps,
                                        int taps_len);
#endif

static const float kTolerance = 1e-5f;

static bool
arrays_close(const float* a, const float* b, int n, float tol) {
    for (int i = 0; i < n; i++) {
        if (std::fabs(a[i] - b[i]) > tol) {
            DSD_FPRINTF(stderr, "  Mismatch at [%d]: got %.8f, expected %.8f (diff=%.8e)\n", i, a[i], b[i],
                        a[i] - b[i]);
            return false;
        }
    }
    return true;
}

/* Generate pseudo-random float in [-1, 1] */
static uint32_t g_rand_state = 0xC0FFEE11u;

static uint32_t
rand_u32() {
    uint32_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

static float
randf() {
    float unit = (float)(rand_u32() >> 8) * (1.0f / 16777215.0f);
    return (unit * 2.0f) - 1.0f;
}

using complex_fir_backend_fn = void (*)(const float*, int, float*, float*, float*, const float*, int);
using complex_hb_backend_fn = int (*)(const float*, int, float*, float*, float*, const float*, int);
using real_hb_backend_fn = int (*)(const float*, int, float*, float*, const float*, int);

static int
test_direct_complex_fir_backend(const char* name, complex_fir_backend_fn fn) {
    std::printf("Testing direct complex FIR backend (%s)...\n", name);

    const int taps_len = 15;
    const int hist_len = taps_len - 1;
    const int N = 7;
    const float taps[taps_len] = {0.010f, -0.025f, 0.040f,  0.060f, -0.015f, 0.080f,  0.120f, 0.450f,
                                  0.120f, 0.080f,  -0.015f, 0.060f, 0.040f,  -0.025f, 0.010f};

    alignas(64) float in[N * 2];
    alignas(64) float out_simd[N * 2];
    alignas(64) float out_ref[N * 2];
    alignas(64) float hist_i_simd[hist_len];
    alignas(64) float hist_q_simd[hist_len];
    alignas(64) float hist_i_ref[hist_len];
    alignas(64) float hist_q_ref[hist_len];

    for (int i = 0; i < N; i++) {
        in[i << 1] = -0.75f + 0.125f * (float)i;
        in[(i << 1) + 1] = 0.60f - 0.09f * (float)i;
    }
    for (int i = 0; i < hist_len; i++) {
        hist_i_simd[i] = hist_i_ref[i] = -0.50f + 0.03f * (float)i;
        hist_q_simd[i] = hist_q_ref[i] = 0.40f - 0.02f * (float)i;
    }

    fn(in, N * 2, out_simd, hist_i_simd, hist_q_simd, taps, taps_len);
    simd_fir_complex_apply_scalar(in, N * 2, out_ref, hist_i_ref, hist_q_ref, taps, taps_len);

    if (!arrays_close(out_simd, out_ref, N * 2, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }
    if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History I mismatch\n");
        return 1;
    }
    if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History Q mismatch\n");
        return 1;
    }

    std::printf("  PASS\n");
    return 0;
}

static int
test_direct_complex_hb_backend(const char* name, complex_hb_backend_fn fn) {
    std::printf("Testing direct complex HB backend (%s)...\n", name);

    const int taps_len = 15;
    const int hist_len = taps_len - 1;
    const int N = 7;

    alignas(64) float in[N * 2];
    alignas(64) float out_simd[N * 2];
    alignas(64) float out_ref[N * 2];
    alignas(64) float hist_i_simd[hist_len];
    alignas(64) float hist_q_simd[hist_len];
    alignas(64) float hist_i_ref[hist_len];
    alignas(64) float hist_q_ref[hist_len];

    for (int i = 0; i < N; i++) {
        in[i << 1] = 0.20f * (float)(i - 3);
        in[(i << 1) + 1] = -0.15f * (float)i;
    }
    for (int i = 0; i < hist_len; i++) {
        hist_i_simd[i] = hist_i_ref[i] = 0.05f * (float)(i - 4);
        hist_q_simd[i] = hist_q_ref[i] = -0.04f * (float)(i + 1);
    }

    int len_simd = fn(in, N * 2, out_simd, hist_i_simd, hist_q_simd, hb_q15_taps, taps_len);
    int len_ref = simd_hb_decim2_complex_scalar(in, N * 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);

    if (len_simd != len_ref) {
        DSD_FPRINTF(stderr, "  FAIL: Output length mismatch (%d vs %d)\n", len_simd, len_ref);
        return 1;
    }
    if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }
    if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History I mismatch\n");
        return 1;
    }
    if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History Q mismatch\n");
        return 1;
    }

    std::printf("  PASS\n");
    return 0;
}

static int
test_direct_real_hb_backend(const char* name, real_hb_backend_fn fn) {
    std::printf("Testing direct real HB backend (%s)...\n", name);

    const int taps_len = 15;
    const int hist_len = taps_len - 1;
    const int N = 11;

    alignas(64) float in[N];
    alignas(64) float out_simd[N];
    alignas(64) float out_ref[N];
    alignas(64) float hist_simd[hist_len];
    alignas(64) float hist_ref[hist_len];

    for (int i = 0; i < N; i++) {
        in[i] = -0.80f + 0.17f * (float)i;
    }
    for (int i = 0; i < hist_len; i++) {
        hist_simd[i] = hist_ref[i] = 0.30f - 0.025f * (float)i;
    }

    int len_simd = fn(in, N, out_simd, hist_simd, hb_q15_taps, taps_len);
    int len_ref = simd_hb_decim2_real_scalar(in, N, out_ref, hist_ref, hb_q15_taps, taps_len);

    if (len_simd != len_ref) {
        DSD_FPRINTF(stderr, "  FAIL: Output length mismatch (%d vs %d)\n", len_simd, len_ref);
        return 1;
    }
    if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }
    if (!arrays_close(hist_simd, hist_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History mismatch\n");
        return 1;
    }

    std::printf("  PASS\n");
    return 0;
}

static int
test_direct_backend_tail_mix(const char* name, complex_fir_backend_fn fir_fn, complex_hb_backend_fn hb_complex_fn,
                             real_hb_backend_fn hb_real_fn) {
    std::printf("Testing direct backend vector/tail mix (%s)...\n", name);

    int rc = 0;

    {
        const int taps_len = 15;
        const int hist_len = taps_len - 1;
        const int N = 13; /* AVX2 path: 8-sample vector + 4-sample vector + scalar tail */
        const float taps[taps_len] = {0.010f, -0.025f, 0.040f,  0.060f, -0.015f, 0.080f,  0.120f, 0.450f,
                                      0.120f, 0.080f,  -0.015f, 0.060f, 0.040f,  -0.025f, 0.010f};

        alignas(64) float in[N * 2];
        alignas(64) float out_simd[N * 2];
        alignas(64) float out_ref[N * 2];
        alignas(64) float hist_i_simd[hist_len];
        alignas(64) float hist_q_simd[hist_len];
        alignas(64) float hist_i_ref[hist_len];
        alignas(64) float hist_q_ref[hist_len];

        for (int i = 0; i < N; i++) {
            in[i << 1] = -0.9f + 0.11f * (float)i;
            in[(i << 1) + 1] = 0.7f - 0.07f * (float)i;
        }
        for (int i = 0; i < hist_len; i++) {
            hist_i_simd[i] = hist_i_ref[i] = 0.20f - 0.015f * (float)i;
            hist_q_simd[i] = hist_q_ref[i] = -0.35f + 0.02f * (float)i;
        }

        fir_fn(in, N * 2, out_simd, hist_i_simd, hist_q_simd, taps, taps_len);
        simd_fir_complex_apply_scalar(in, N * 2, out_ref, hist_i_ref, hist_q_ref, taps, taps_len);
        if (!arrays_close(out_simd, out_ref, N * 2, kTolerance)
            || !arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)
            || !arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: FIR vector/tail mix mismatch\n");
            rc = 1;
        }
    }

    {
        const int taps_len = 15;
        const int hist_len = taps_len - 1;
        const int N = 11; /* 5 decimated complex outputs: vector quartet + scalar tail */
        alignas(64) float in[N * 2];
        alignas(64) float out_simd[N * 2];
        alignas(64) float out_ref[N * 2];
        alignas(64) float hist_i_simd[hist_len];
        alignas(64) float hist_q_simd[hist_len];
        alignas(64) float hist_i_ref[hist_len];
        alignas(64) float hist_q_ref[hist_len];

        for (int i = 0; i < N; i++) {
            in[i << 1] = -0.5f + 0.09f * (float)i;
            in[(i << 1) + 1] = 0.25f + 0.06f * (float)i;
        }
        for (int i = 0; i < hist_len; i++) {
            hist_i_simd[i] = hist_i_ref[i] = -0.10f + 0.012f * (float)i;
            hist_q_simd[i] = hist_q_ref[i] = 0.33f - 0.018f * (float)i;
        }

        int len_simd = hb_complex_fn(in, N * 2, out_simd, hist_i_simd, hist_q_simd, hb_q15_taps, taps_len);
        int len_ref = simd_hb_decim2_complex_scalar(in, N * 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);
        if (len_simd != len_ref || !arrays_close(out_simd, out_ref, len_simd, kTolerance)
            || !arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)
            || !arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: complex HB vector/tail mix mismatch\n");
            rc = 1;
        }
    }

    {
        const int taps_len = 15;
        const int hist_len = taps_len - 1;
        const int N = 19; /* 9 decimated real outputs: vector octet + scalar tail */
        alignas(64) float in[N];
        alignas(64) float out_simd[N];
        alignas(64) float out_ref[N];
        alignas(64) float hist_simd[hist_len];
        alignas(64) float hist_ref[hist_len];

        for (int i = 0; i < N; i++) {
            in[i] = -0.65f + 0.08f * (float)i;
        }
        for (int i = 0; i < hist_len; i++) {
            hist_simd[i] = hist_ref[i] = 0.18f - 0.011f * (float)i;
        }

        int len_simd = hb_real_fn(in, N, out_simd, hist_simd, hb_q15_taps, taps_len);
        int len_ref = simd_hb_decim2_real_scalar(in, N, out_ref, hist_ref, hb_q15_taps, taps_len);
        if (len_simd != len_ref || !arrays_close(out_simd, out_ref, len_simd, kTolerance)
            || !arrays_close(hist_simd, hist_ref, hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: real HB vector/tail mix mismatch\n");
            rc = 1;
        }
    }

    if (rc == 0) {
        std::printf("  PASS\n");
    }
    return rc;
}

static int
test_direct_backend_invalid_guards(const char* name, complex_fir_backend_fn fir_fn, complex_hb_backend_fn hb_complex_fn,
                                   real_hb_backend_fn hb_real_fn) {
    std::printf("Testing direct backend invalid guards (%s)...\n", name);

    alignas(64) float in[4] = {1.0f, -1.0f, 2.0f, -2.0f};
    alignas(64) float out[4] = {10.0f, 11.0f, 12.0f, 13.0f};
    alignas(64) float hist_i[4] = {20.0f, 21.0f, 22.0f, 23.0f};
    alignas(64) float hist_q[4] = {-20.0f, -21.0f, -22.0f, -23.0f};
    alignas(64) float hist_r[4] = {30.0f, 31.0f, 32.0f, 33.0f};
    const float even_taps[4] = {0.25f, 0.5f, 0.5f, 0.25f};
    const float short_taps[2] = {0.5f, 0.5f};

    fir_fn(in, 4, out, hist_i, hist_q, even_taps, 4);
    fir_fn(in, 1, out, hist_i, hist_q, short_taps, 2);
    int c0 = hb_complex_fn(in, 4, out, hist_i, hist_q, even_taps, 4);
    int c1 = hb_complex_fn(in, 0, out, hist_i, hist_q, hb_q15_taps, 15);
    int r0 = hb_real_fn(in, 4, out, hist_r, even_taps, 4);
    int r1 = hb_real_fn(in, 0, out, hist_r, hb_q15_taps, 15);

    if (c0 != 0 || c1 != 0 || r0 != 0 || r1 != 0) {
        DSD_FPRINTF(stderr, "  FAIL: invalid guards returned %d/%d/%d/%d\n", c0, c1, r0, r1);
        return 1;
    }
    const float want_out[4] = {10.0f, 11.0f, 12.0f, 13.0f};
    const float want_hist_i[4] = {20.0f, 21.0f, 22.0f, 23.0f};
    const float want_hist_q[4] = {-20.0f, -21.0f, -22.0f, -23.0f};
    const float want_hist_r[4] = {30.0f, 31.0f, 32.0f, 33.0f};
    if (!arrays_close(out, want_out, 4, 0.0f) || !arrays_close(hist_i, want_hist_i, 4, 0.0f)
        || !arrays_close(hist_q, want_hist_q, 4, 0.0f) || !arrays_close(hist_r, want_hist_r, 4, 0.0f)) {
        DSD_FPRINTF(stderr, "  FAIL: invalid guards mutated buffers\n");
        return 1;
    }

    std::printf("  PASS\n");
    return 0;
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64)            \
    || defined(_M_ARM64EC)

namespace {
enum class FixedBufferMode : uint8_t { Aligned, Unaligned, ExactSize };
} // namespace

static const char*
fixed_buffer_mode_name(FixedBufferMode mode) {
    switch (mode) {
        case FixedBufferMode::Aligned: return "aligned";
        case FixedBufferMode::Unaligned: return "unaligned";
        case FixedBufferMode::ExactSize: return "exact-size";
    }
    return "unknown";
}

static float*
fixed_test_buffer(std::vector<float>* storage, int count, FixedBufferMode mode) {
    if (mode == FixedBufferMode::ExactSize) {
        storage->resize((size_t)count);
        return storage->data();
    }

    storage->resize((size_t)count + 16U);
    void* buffer = storage->data();
    size_t space = storage->size() * sizeof(float);
    const size_t required = ((size_t)count + 1U) * sizeof(float);
    float* aligned = static_cast<float*>(std::align(32U, required, buffer, space));
    return (mode == FixedBufferMode::Unaligned) ? aligned + 1 : aligned;
}

template <int TapsLen>
static void
make_synthetic_halfband_taps(float* taps) {
    const int center = (TapsLen - 1) >> 1;
    for (int k = 0; k < TapsLen; k++) {
        taps[k] = 0.0f;
    }
    for (int e = 0; e < center; e += 2) {
        const float sign = ((e >> 1) & 1) ? -1.0f : 1.0f;
        const float value = sign * (0.004f + 0.001f * (float)(e + 1));
        taps[e] = value;
        taps[TapsLen - 1 - e] = value;
    }
    taps[center] = 0.47f;
}

template <int TapsLen>
static int
run_direct_fixed_hb_case(const char* backend, complex_hb_backend_fn fn, const float* taps, const char* tap_name,
                         int ch_len, FixedBufferMode mode) {
    const int in_len = ch_len << 1;
    const int hist_len = TapsLen - 1;
    const int out_capacity = std::max(ch_len, 2);
    constexpr float kOutputSentinel = 12345.0f;

    std::vector<float> in_storage;
    std::vector<float> out_storage;
    std::vector<float> hist_i_storage;
    std::vector<float> hist_q_storage;
    float* in = fixed_test_buffer(&in_storage, in_len, mode);
    float* out_simd = fixed_test_buffer(&out_storage, out_capacity, mode);
    float* hist_i_simd = fixed_test_buffer(&hist_i_storage, hist_len, mode);
    float* hist_q_simd = fixed_test_buffer(&hist_q_storage, hist_len, mode);

    std::vector<float> out_ref((size_t)out_capacity, kOutputSentinel);
    std::vector<float> hist_i_ref((size_t)hist_len);
    std::vector<float> hist_q_ref((size_t)hist_len);

    for (int k = 0; k < in_len; k++) {
        in[k] = (float)(((k * 37 + ch_len * 11) % 257) - 128) * (1.0f / 91.0f);
    }
    /* Make the repeated suffix conspicuous, including for odd complex block sizes. */
    in[in_len - 2] = 8.25f + 0.001f * (float)ch_len;
    in[in_len - 1] = -6.75f - 0.002f * (float)ch_len;
    for (int k = 0; k < hist_len; k++) {
        hist_i_simd[k] = hist_i_ref[(size_t)k] = -1.25f + 0.031f * (float)k;
        hist_q_simd[k] = hist_q_ref[(size_t)k] = 0.875f - 0.027f * (float)k;
    }
    std::fill(out_simd, out_simd + out_capacity, kOutputSentinel);

    const int len_simd = fn(in, in_len, out_simd, hist_i_simd, hist_q_simd, taps, TapsLen);
    const int len_ref =
        simd_hb_decim2_complex_scalar(in, in_len, out_ref.data(), hist_i_ref.data(), hist_q_ref.data(), taps, TapsLen);
    const int expected_len = (ch_len >> 1) << 1;

    if (len_simd != expected_len || len_simd != len_ref) {
        DSD_FPRINTF(stderr, "  FAIL: %s %s %d-tap size %d length %d/%d (expected %d)\n", backend,
                    fixed_buffer_mode_name(mode), TapsLen, ch_len, len_simd, len_ref, expected_len);
        return 1;
    }
    if (!arrays_close(out_simd, out_ref.data(), len_simd, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: %s %s %s %d-tap size %d output mismatch\n", backend, fixed_buffer_mode_name(mode),
                    tap_name, TapsLen, ch_len);
        return 1;
    }
    if (len_simd == 0 && out_simd[0] != kOutputSentinel) {
        DSD_FPRINTF(stderr, "  FAIL: %s %s %d-tap zero-output block mutated output\n", backend,
                    fixed_buffer_mode_name(mode), TapsLen);
        return 1;
    }
    if (!arrays_close(hist_i_simd, hist_i_ref.data(), hist_len, 0.0f)
        || !arrays_close(hist_q_simd, hist_q_ref.data(), hist_len, 0.0f)) {
        DSD_FPRINTF(stderr, "  FAIL: %s %s %s %d-tap size %d history mismatch\n", backend, fixed_buffer_mode_name(mode),
                    tap_name, TapsLen, ch_len);
        return 1;
    }
    return 0;
}

template <int TapsLen>
static int
test_direct_fixed_hb_tap_count(const char* backend, complex_hb_backend_fn fn, const float* canonical_taps,
                               int vector_load_tail) {
    alignas(64) float synthetic_taps[TapsLen];
    make_synthetic_halfband_taps<TapsLen>(synthetic_taps);

    const int sizes[] = {1, 2, 7, 15, 30, 31, 32, 33, 47, 48, 49, 65, 8192, 8193};
    const float* tap_sets[] = {canonical_taps, synthetic_taps};
    const char* tap_names[] = {"canonical", "synthetic"};
    const FixedBufferMode modes[] = {FixedBufferMode::Aligned, FixedBufferMode::Unaligned};

    for (int tap_set = 0; tap_set < 2; tap_set++) {
        for (FixedBufferMode mode : modes) {
            for (int ch_len : sizes) {
                if (run_direct_fixed_hb_case<TapsLen>(backend, fn, tap_sets[tap_set], tap_names[tap_set], ch_len, mode)
                    != 0) {
                    return 1;
                }
            }
        }
    }

    /* The first legal vector block ends exactly at the allocation boundary. */
    constexpr int center = (TapsLen - 1) >> 1;
    constexpr int first_vector_n = (center + 1) >> 1;
    const int exact_vector_ch_len = (first_vector_n << 1) + center + vector_load_tail + 1;
    for (int tap_set = 0; tap_set < 2; tap_set++) {
        if (run_direct_fixed_hb_case<TapsLen>(backend, fn, tap_sets[tap_set], tap_names[tap_set], exact_vector_ch_len,
                                              FixedBufferMode::ExactSize)
            != 0) {
            return 1;
        }
    }
    return 0;
}

template <int TapsLen>
static int
test_direct_fixed_hb_stream(const char* backend, complex_hb_backend_fn fn, const float* taps) {
    constexpr int hist_len = TapsLen - 1;
    const int block_sizes[] = {1, 31, 48, 17, 8193, 2, 65, 32};
    std::vector<float> hist_i_simd(hist_len);
    std::vector<float> hist_q_simd(hist_len);
    std::vector<float> hist_i_ref(hist_len);
    std::vector<float> hist_q_ref(hist_len);

    for (int k = 0; k < hist_len; k++) {
        hist_i_simd[(size_t)k] = hist_i_ref[(size_t)k] = 2.0f - 0.041f * (float)k;
        hist_q_simd[(size_t)k] = hist_q_ref[(size_t)k] = -1.0f + 0.037f * (float)k;
    }

    int stream_offset = 0;
    for (int ch_len : block_sizes) {
        const int in_len = ch_len << 1;
        std::vector<float> in((size_t)in_len);
        std::vector<float> out_simd((size_t)std::max(ch_len, 2), 23456.0f);
        std::vector<float> out_ref((size_t)std::max(ch_len, 2), 23456.0f);
        for (int k = 0; k < in_len; k++) {
            in[(size_t)k] = (float)(((stream_offset + k * 19) % 311) - 155) * (1.0f / 73.0f);
        }
        in[(size_t)in_len - 2] = 4.5f + 0.01f * (float)ch_len;
        in[(size_t)in_len - 1] = -3.75f - 0.02f * (float)ch_len;
        stream_offset += in_len;

        const int len_simd =
            fn(in.data(), in_len, out_simd.data(), hist_i_simd.data(), hist_q_simd.data(), taps, TapsLen);
        const int len_ref = simd_hb_decim2_complex_scalar(in.data(), in_len, out_ref.data(), hist_i_ref.data(),
                                                          hist_q_ref.data(), taps, TapsLen);
        if (len_simd != len_ref || !arrays_close(out_simd.data(), out_ref.data(), len_simd, kTolerance)
            || !arrays_close(hist_i_simd.data(), hist_i_ref.data(), hist_len, 0.0f)
            || !arrays_close(hist_q_simd.data(), hist_q_ref.data(), hist_len, 0.0f)) {
            DSD_FPRINTF(stderr, "  FAIL: %s %d-tap variable stream block size %d mismatch\n", backend, TapsLen, ch_len);
            return 1;
        }
    }
    return 0;
}

static int
test_direct_generic_23tap_hb(const char* backend, complex_hb_backend_fn fn) {
    constexpr int taps_len = 23;
    constexpr int hist_len = taps_len - 1;
    constexpr int ch_len = 127;
    alignas(64) float taps[taps_len];
    make_synthetic_halfband_taps<taps_len>(taps);
    std::vector<float> in((size_t)ch_len * 2U);
    std::vector<float> out_simd(ch_len);
    std::vector<float> out_ref(ch_len);
    std::vector<float> hist_i_simd(hist_len);
    std::vector<float> hist_q_simd(hist_len);
    std::vector<float> hist_i_ref(hist_len);
    std::vector<float> hist_q_ref(hist_len);

    for (int k = 0; k < ch_len * 2; k++) {
        in[(size_t)k] = (float)((k * 29) % 101 - 50) * 0.025f;
    }
    for (int k = 0; k < hist_len; k++) {
        hist_i_simd[(size_t)k] = hist_i_ref[(size_t)k] = 0.03f * (float)(k - 9);
        hist_q_simd[(size_t)k] = hist_q_ref[(size_t)k] = -0.02f * (float)(k + 3);
    }

    const int len_simd =
        fn(in.data(), ch_len * 2, out_simd.data(), hist_i_simd.data(), hist_q_simd.data(), taps, taps_len);
    const int len_ref = simd_hb_decim2_complex_scalar(in.data(), ch_len * 2, out_ref.data(), hist_i_ref.data(),
                                                      hist_q_ref.data(), taps, taps_len);
    if (len_simd != len_ref || !arrays_close(out_simd.data(), out_ref.data(), len_simd, kTolerance)
        || !arrays_close(hist_i_simd.data(), hist_i_ref.data(), hist_len, 0.0f)
        || !arrays_close(hist_q_simd.data(), hist_q_ref.data(), hist_len, 0.0f)) {
        DSD_FPRINTF(stderr, "  FAIL: %s generic 23-tap fallback mismatch\n", backend);
        return 1;
    }
    return 0;
}

static int
test_direct_hb_cascade(const char* backend, complex_hb_backend_fn fn) {
    constexpr int stages = 5;
    constexpr int input_pairs = 8192;
    const int tap_lengths[stages] = {31, 15, 15, 15, 15};
    const float* tap_sets[stages] = {hb31_q15_taps, hb_q15_taps, hb_q15_taps, hb_q15_taps, hb_q15_taps};
    std::vector<float> current_simd((size_t)input_pairs * 2U);
    std::vector<float> current_ref((size_t)input_pairs * 2U);
    std::vector<float> hist_i_simd[stages];
    std::vector<float> hist_q_simd[stages];
    std::vector<float> hist_i_ref[stages];
    std::vector<float> hist_q_ref[stages];

    for (int k = 0; k < input_pairs * 2; k++) {
        current_simd[(size_t)k] = current_ref[(size_t)k] = (float)((k * 43) % 509 - 254) * (1.0f / 127.0f);
    }
    for (int stage = 0; stage < stages; stage++) {
        const int hist_len = tap_lengths[stage] - 1;
        hist_i_simd[stage].resize((size_t)hist_len);
        hist_q_simd[stage].resize((size_t)hist_len);
        hist_i_ref[stage].resize((size_t)hist_len);
        hist_q_ref[stage].resize((size_t)hist_len);
        for (int k = 0; k < hist_len; k++) {
            hist_i_simd[stage][(size_t)k] = hist_i_ref[stage][(size_t)k] =
                0.1f * (float)(stage + 1) + 0.007f * (float)k;
            hist_q_simd[stage][(size_t)k] = hist_q_ref[stage][(size_t)k] =
                -0.2f * (float)(stage + 1) - 0.005f * (float)k;
        }
    }

    for (int stage = 0; stage < stages; stage++) {
        std::vector<float> next_simd(current_simd.size() / 2U);
        std::vector<float> next_ref(current_ref.size() / 2U);
        const int taps_len = tap_lengths[stage];
        const int hist_len = taps_len - 1;
        const int len_simd = fn(current_simd.data(), (int)current_simd.size(), next_simd.data(),
                                hist_i_simd[stage].data(), hist_q_simd[stage].data(), tap_sets[stage], taps_len);
        const int len_ref = simd_hb_decim2_complex_scalar(current_ref.data(), (int)current_ref.size(), next_ref.data(),
                                                          hist_i_ref[stage].data(), hist_q_ref[stage].data(),
                                                          tap_sets[stage], taps_len);
        bool histories_exact = true;
        const int input_pairs = (int)current_simd.size() >> 1;
        for (int k = 0; k < hist_len; k++) {
            const int rel = input_pairs - hist_len + k;
            histories_exact = histories_exact && hist_i_simd[stage][(size_t)k] == current_simd[(size_t)rel * 2U]
                              && hist_q_simd[stage][(size_t)k] == current_simd[(size_t)rel * 2U + 1U]
                              && hist_i_ref[stage][(size_t)k] == current_ref[(size_t)rel * 2U]
                              && hist_q_ref[stage][(size_t)k] == current_ref[(size_t)rel * 2U + 1U];
        }
        if (len_simd != len_ref || len_simd != (int)next_simd.size()
            || !arrays_close(next_simd.data(), next_ref.data(), len_simd, kTolerance) || !histories_exact
            || !arrays_close(hist_i_simd[stage].data(), hist_i_ref[stage].data(), hist_len, kTolerance)
            || !arrays_close(hist_q_simd[stage].data(), hist_q_ref[stage].data(), hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: %s five-stage cascade mismatch at stage %d\n", backend, stage);
            return 1;
        }
        current_simd.swap(next_simd);
        current_ref.swap(next_ref);
    }
    return 0;
}

static int
test_direct_fixed_hb_kernels(const char* backend, complex_hb_backend_fn fn, int vector_load_tail) {
    std::printf("Testing direct %s fixed complex HB kernels...\n", backend);

    if (test_direct_fixed_hb_tap_count<15>(backend, fn, hb_q15_taps, vector_load_tail) != 0
        || test_direct_fixed_hb_tap_count<31>(backend, fn, hb31_q15_taps, vector_load_tail) != 0
        || test_direct_fixed_hb_stream<15>(backend, fn, hb_q15_taps) != 0
        || test_direct_fixed_hb_stream<31>(backend, fn, hb31_q15_taps) != 0
        || test_direct_generic_23tap_hb(backend, fn) != 0 || test_direct_hb_cascade(backend, fn) != 0) {
        return 1;
    }

    /* Invalid float lengths must not touch the fixed-kernel histories. */
    alignas(64) float in[1] = {7.0f};
    alignas(64) float out[2] = {19.0f, 23.0f};
    alignas(64) float hist_i[30];
    alignas(64) float hist_q[30];
    alignas(64) float hist_i_before[30];
    alignas(64) float hist_q_before[30];
    for (int k = 0; k < 30; k++) {
        hist_i[k] = hist_i_before[k] = (float)(k + 1);
        hist_q[k] = hist_q_before[k] = (float)(-k - 1);
    }
    const int len = fn(in, 1, out, hist_i, hist_q, hb31_q15_taps, 31);
    if (len != 0 || out[0] != 19.0f || out[1] != 23.0f || !arrays_close(hist_i, hist_i_before, 30, 0.0f)
        || !arrays_close(hist_q, hist_q_before, 30, 0.0f)) {
        DSD_FPRINTF(stderr, "  FAIL: %s fixed-kernel invalid guard mutated state\n", backend);
        return 1;
    }

    std::printf("  PASS\n");
    return 0;
}

#endif

/* Test 63-tap symmetric FIR (channel LPF style) */
static int
test_complex_fir_63tap() {
    std::printf("Testing simd_fir_complex_apply (63-tap)...\n");

    /* Create symmetric 63-tap filter (non-half-band, non-zero odd taps) */
    const int taps_len = 63;
    alignas(64) float taps[taps_len];
    for (int i = 0; i < taps_len / 2; i++) {
        float v = randf() * 0.1f;
        taps[i] = v;
        taps[taps_len - 1 - i] = v;
    }
    taps[taps_len / 2] = 0.5f; /* center tap */

    const int hist_len = taps_len - 1;
    const int N = 256; /* complex samples */

    alignas(64) float in[N * 2];
    alignas(64) float out_simd[N * 2];
    alignas(64) float out_ref[N * 2];
    alignas(64) float hist_i_simd[hist_len];
    alignas(64) float hist_q_simd[hist_len];
    alignas(64) float hist_i_ref[hist_len];
    alignas(64) float hist_q_ref[hist_len];

    DSD_MEMSET(hist_i_simd, 0, sizeof(hist_i_simd));
    DSD_MEMSET(hist_q_simd, 0, sizeof(hist_q_simd));
    DSD_MEMSET(hist_i_ref, 0, sizeof(hist_i_ref));
    DSD_MEMSET(hist_q_ref, 0, sizeof(hist_q_ref));

    /* Generate random input */
    for (int i = 0; i < N * 2; i++) {
        in[i] = randf();
    }

    /* Run both implementations */
    simd_fir_complex_apply(in, N * 2, out_simd, hist_i_simd, hist_q_simd, taps, taps_len);
    simd_fir_complex_apply_scalar(in, N * 2, out_ref, hist_i_ref, hist_q_ref, taps, taps_len);

    if (!arrays_close(out_simd, out_ref, N * 2, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History I mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History Q mismatch\n");
        return 1;
    }

    std::printf("  PASS (impl: %s)\n", simd_fir_get_impl_name());
    return 0;
}

/* Test complex half-band decimator with various tap lengths */
template <int TapsLen>
static int
test_complex_hb_decim(const float* taps, const char* name) {
    std::printf("Testing simd_hb_decim2_complex (%s)...\n", name);

    static_assert(TapsLen > 1, "TapsLen must be > 1");
    const int hist_len = TapsLen - 1;
    const int N = 512; /* input complex samples */

    alignas(64) float in[N * 2];
    alignas(64) float out_simd[N];
    alignas(64) float out_ref[N];
    alignas(64) float hist_i_simd[hist_len];
    alignas(64) float hist_q_simd[hist_len];
    alignas(64) float hist_i_ref[hist_len];
    alignas(64) float hist_q_ref[hist_len];

    DSD_MEMSET(hist_i_simd, 0, sizeof(hist_i_simd));
    DSD_MEMSET(hist_q_simd, 0, sizeof(hist_q_simd));
    DSD_MEMSET(hist_i_ref, 0, sizeof(hist_i_ref));
    DSD_MEMSET(hist_q_ref, 0, sizeof(hist_q_ref));

    for (int i = 0; i < N * 2; i++) {
        in[i] = randf();
    }

    int len_simd = simd_hb_decim2_complex(in, N * 2, out_simd, hist_i_simd, hist_q_simd, taps, TapsLen);
    int len_ref = simd_hb_decim2_complex_scalar(in, N * 2, out_ref, hist_i_ref, hist_q_ref, taps, TapsLen);

    if (len_simd != len_ref) {
        DSD_FPRINTF(stderr, "  FAIL: Output length mismatch (%d vs %d)\n", len_simd, len_ref);
        return 1;
    }

    if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History I mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History Q mismatch\n");
        return 1;
    }

    std::printf("  PASS\n");
    return 0;
}

/* Test real half-band decimator */
template <int TapsLen>
static int
test_real_hb_decim(const float* taps, const char* name) {
    std::printf("Testing simd_hb_decim2_real (%s)...\n", name);

    static_assert(TapsLen > 1, "TapsLen must be > 1");
    const int hist_len = TapsLen - 1;
    const int N = 512; /* input samples */

    alignas(64) float in[N];
    alignas(64) float out_simd[N / 2];
    alignas(64) float out_ref[N / 2];
    alignas(64) float hist_simd[hist_len];
    alignas(64) float hist_ref[hist_len];

    DSD_MEMSET(hist_simd, 0, sizeof(hist_simd));
    DSD_MEMSET(hist_ref, 0, sizeof(hist_ref));

    for (int i = 0; i < N; i++) {
        in[i] = randf();
    }

    int len_simd = simd_hb_decim2_real(in, N, out_simd, hist_simd, taps, TapsLen);
    int len_ref = simd_hb_decim2_real_scalar(in, N, out_ref, hist_ref, taps, TapsLen);

    if (len_simd != len_ref) {
        DSD_FPRINTF(stderr, "  FAIL: Output length mismatch (%d vs %d)\n", len_simd, len_ref);
        return 1;
    }

    if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_simd, hist_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History mismatch\n");
        return 1;
    }

    std::printf("  PASS\n");
    return 0;
}

/* Test history continuity across multiple blocks */
static int
test_history_continuity() {
    std::printf("Testing history continuity across blocks...\n");

    const int taps_len = 15;
    const int hist_len = taps_len - 1;
    const int block_size = 64; /* small blocks */
    const int num_blocks = 8;

    alignas(64) float hist_i_simd[hist_len];
    alignas(64) float hist_q_simd[hist_len];
    alignas(64) float hist_i_ref[hist_len];
    alignas(64) float hist_q_ref[hist_len];

    DSD_MEMSET(hist_i_simd, 0, sizeof(hist_i_simd));
    DSD_MEMSET(hist_q_simd, 0, sizeof(hist_q_simd));
    DSD_MEMSET(hist_i_ref, 0, sizeof(hist_i_ref));
    DSD_MEMSET(hist_q_ref, 0, sizeof(hist_q_ref));

    for (int blk = 0; blk < num_blocks; blk++) {
        alignas(64) float in[block_size * 2];
        alignas(64) float out_simd[block_size];
        alignas(64) float out_ref[block_size];

        for (int i = 0; i < block_size * 2; i++) {
            in[i] = randf();
        }

        int len_simd =
            simd_hb_decim2_complex(in, block_size * 2, out_simd, hist_i_simd, hist_q_simd, hb_q15_taps, taps_len);
        int len_ref =
            simd_hb_decim2_complex_scalar(in, block_size * 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);

        if (len_simd != len_ref) {
            DSD_FPRINTF(stderr, "  FAIL: Block %d length mismatch\n", blk);
            return 1;
        }

        if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: Block %d output mismatch\n", blk);
            return 1;
        }
    }

    std::printf("  PASS\n");
    return 0;
}

/* Test edge cases: small blocks */
static int
test_small_blocks() {
    std::printf("Testing small block sizes...\n");

    const int taps_len = 15;
    const int hist_len = taps_len - 1;

    /* Test very small blocks (< SIMD width) */
    int test_sizes[] = {2, 4, 6, 8, 10, 12, 14, 16, 32};
    int num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);
    constexpr int kMaxN = 32;

    for (int t = 0; t < num_tests; t++) {
        int N = test_sizes[t]; /* complex samples */
        if (N > kMaxN) {
            DSD_FPRINTF(stderr, "  FAIL: Test size exceeds max (%d > %d)\n", N, kMaxN);
            return 1;
        }

        alignas(64) float in[kMaxN * 2];
        alignas(64) float out_simd[kMaxN];
        alignas(64) float out_ref[kMaxN];
        alignas(64) float hist_i_simd[hist_len];
        alignas(64) float hist_q_simd[hist_len];
        alignas(64) float hist_i_ref[hist_len];
        alignas(64) float hist_q_ref[hist_len];

        DSD_MEMSET(hist_i_simd, 0, sizeof(hist_i_simd));
        DSD_MEMSET(hist_q_simd, 0, sizeof(hist_q_simd));
        DSD_MEMSET(hist_i_ref, 0, sizeof(hist_i_ref));
        DSD_MEMSET(hist_q_ref, 0, sizeof(hist_q_ref));

        for (int i = 0; i < N * 2; i++) {
            in[i] = randf();
        }

        int len_simd = simd_hb_decim2_complex(in, N * 2, out_simd, hist_i_simd, hist_q_simd, hb_q15_taps, taps_len);
        int len_ref = simd_hb_decim2_complex_scalar(in, N * 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);

        if (len_simd != len_ref) {
            DSD_FPRINTF(stderr, "  FAIL: Size %d length mismatch\n", N);
            return 1;
        }

        if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: Size %d output mismatch\n", N);
            return 1;
        }
    }

    std::printf("  PASS\n");
    return 0;
}

/* Regression: complex short blocks must preserve prior history samples */
static int
test_complex_short_block_history() {
    std::printf("Testing complex short-block history update...\n");

    const int taps_len = 15;
    const int hist_len = taps_len - 1;
    const int ch_len = 8; /* smaller than history length */

    alignas(64) float in[ch_len * 2];
    alignas(64) float out_simd[ch_len];
    alignas(64) float out_ref[ch_len];
    alignas(64) float hist_i_simd[hist_len];
    alignas(64) float hist_q_simd[hist_len];
    alignas(64) float hist_i_ref[hist_len];
    alignas(64) float hist_q_ref[hist_len];

    for (int i = 0; i < hist_len; i++) {
        hist_i_simd[i] = hist_i_ref[i] = -3.0f + (float)i * 0.25f;
        hist_q_simd[i] = hist_q_ref[i] = 2.0f - (float)i * 0.5f;
    }
    for (int i = 0; i < ch_len; i++) {
        in[i << 1] = (float)(10 + i);
        in[(i << 1) + 1] = (float)(-20 - i);
    }

    int len_simd = simd_hb_decim2_complex(in, ch_len * 2, out_simd, hist_i_simd, hist_q_simd, hb_q15_taps, taps_len);
    int len_ref = simd_hb_decim2_complex_scalar(in, ch_len * 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);

    if (len_simd != len_ref) {
        DSD_FPRINTF(stderr, "  FAIL: Output length mismatch (%d vs %d)\n", len_simd, len_ref);
        return 1;
    }
    if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }
    if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History I mismatch\n");
        return 1;
    }
    if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
        DSD_FPRINTF(stderr, "  FAIL: History Q mismatch\n");
        return 1;
    }

    std::printf("  PASS\n");
    return 0;
}

/* Regression: input blocks that produce no output must still update history */
static int
test_zero_output_history_updates() {
    std::printf("Testing zero-output history updates...\n");

    const int taps_len = 15;
    const int hist_len = taps_len - 1;

    {
        alignas(64) float in_complex[2] = {3.25f, -7.5f}; /* 1 complex sample => 0 output */
        alignas(64) float out_simd[2] = {0.0f, 0.0f};
        alignas(64) float out_ref[2] = {0.0f, 0.0f};
        alignas(64) float hist_i_simd[hist_len];
        alignas(64) float hist_q_simd[hist_len];
        alignas(64) float hist_i_ref[hist_len];
        alignas(64) float hist_q_ref[hist_len];

        for (int i = 0; i < hist_len; i++) {
            hist_i_simd[i] = hist_i_ref[i] = (float)(100 + i);
            hist_q_simd[i] = hist_q_ref[i] = (float)(-100 - i);
        }

        int len_simd = simd_hb_decim2_complex(in_complex, 2, out_simd, hist_i_simd, hist_q_simd, hb_q15_taps, taps_len);
        int len_ref =
            simd_hb_decim2_complex_scalar(in_complex, 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);

        if (len_simd != len_ref) {
            DSD_FPRINTF(stderr, "  FAIL: Complex zero-output length mismatch (%d vs %d)\n", len_simd, len_ref);
            return 1;
        }
        if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: Complex History I mismatch\n");
            return 1;
        }
        if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: Complex History Q mismatch\n");
            return 1;
        }
    }

    {
        alignas(64) float in_real[1] = {1.5f}; /* 1 real sample => 0 output */
        alignas(64) float out_simd[1] = {0.0f};
        alignas(64) float out_ref[1] = {0.0f};
        alignas(64) float hist_simd[hist_len];
        alignas(64) float hist_ref[hist_len];

        for (int i = 0; i < hist_len; i++) {
            hist_simd[i] = hist_ref[i] = (float)(50 - i);
        }

        int len_simd = simd_hb_decim2_real(in_real, 1, out_simd, hist_simd, hb_q15_taps, taps_len);
        int len_ref = simd_hb_decim2_real_scalar(in_real, 1, out_ref, hist_ref, hb_q15_taps, taps_len);

        if (len_simd != len_ref) {
            DSD_FPRINTF(stderr, "  FAIL: Real zero-output length mismatch (%d vs %d)\n", len_simd, len_ref);
            return 1;
        }
        if (!arrays_close(hist_simd, hist_ref, hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: Real History mismatch\n");
            return 1;
        }
    }

    std::printf("  PASS\n");
    return 0;
}

static int
test_public_scalar_fallback_edges() {
    std::printf("Testing public scalar fallback edges...\n");

    const int taps_len = 5;
    const int hist_len = taps_len - 1;
    const float sparse_taps[taps_len] = {0.25f, 0.0f, 0.50f, 0.0f, 0.25f};

    // Complex FIR fallback should match scalar output and both I/Q histories.
    {
        alignas(64) float in[8] = {1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f};
        alignas(64) float out_simd[8];
        alignas(64) float out_ref[8];
        alignas(64) float hist_i_simd[hist_len];
        alignas(64) float hist_q_simd[hist_len];
        alignas(64) float hist_i_ref[hist_len];
        alignas(64) float hist_q_ref[hist_len];

        for (int i = 0; i < hist_len; i++) {
            hist_i_simd[i] = hist_i_ref[i] = -2.0f + (float)i;
            hist_q_simd[i] = hist_q_ref[i] = 2.0f - (float)i;
        }

        simd_fir_complex_apply(in, 8, out_simd, hist_i_simd, hist_q_simd, sparse_taps, taps_len);
        simd_fir_complex_apply_scalar(in, 8, out_ref, hist_i_ref, hist_q_ref, sparse_taps, taps_len);

        if (!arrays_close(out_simd, out_ref, 8, kTolerance)
            || !arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)
            || !arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: Scalar complex FIR fallback mismatch\n");
            return 1;
        }
    }

    // Complex halfband decimation fallback validates both returned length and history.
    {
        alignas(64) float in[8] = {1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f};
        alignas(64) float out_simd[4];
        alignas(64) float out_ref[4];
        alignas(64) float hist_i_simd[hist_len];
        alignas(64) float hist_q_simd[hist_len];
        alignas(64) float hist_i_ref[hist_len];
        alignas(64) float hist_q_ref[hist_len];

        for (int i = 0; i < hist_len; i++) {
            hist_i_simd[i] = hist_i_ref[i] = 10.0f + (float)i;
            hist_q_simd[i] = hist_q_ref[i] = -10.0f - (float)i;
        }

        int len_simd = simd_hb_decim2_complex(in, 8, out_simd, hist_i_simd, hist_q_simd, sparse_taps, taps_len);
        int len_ref = simd_hb_decim2_complex_scalar(in, 8, out_ref, hist_i_ref, hist_q_ref, sparse_taps, taps_len);

        if (len_simd != len_ref || !arrays_close(out_simd, out_ref, len_simd, kTolerance)
            || !arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)
            || !arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: Scalar complex HB fallback mismatch\n");
            return 1;
        }
    }

    // Real halfband fallback should preserve scalar equivalence on output and carry history.
    {
        alignas(64) float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        alignas(64) float out_simd[2];
        alignas(64) float out_ref[2];
        alignas(64) float hist_simd[hist_len];
        alignas(64) float hist_ref[hist_len];

        for (int i = 0; i < hist_len; i++) {
            hist_simd[i] = hist_ref[i] = -4.0f + (float)i;
        }

        int len_simd = simd_hb_decim2_real(in, 4, out_simd, hist_simd, sparse_taps, taps_len);
        int len_ref = simd_hb_decim2_real_scalar(in, 4, out_ref, hist_ref, sparse_taps, taps_len);

        if (len_simd != len_ref || !arrays_close(out_simd, out_ref, len_simd, kTolerance)
            || !arrays_close(hist_simd, hist_ref, hist_len, kTolerance)) {
            DSD_FPRINTF(stderr, "  FAIL: Scalar real HB fallback mismatch\n");
            return 1;
        }
    }

    // Guard paths for invalid lengths must leave sentinels intact and emit zero output length.
    {
        alignas(64) float in_complex[2] = {1.0f, -1.0f};
        alignas(64) float out_complex[2] = {11.0f, 12.0f};
        alignas(64) float hist_i[14] = {};
        alignas(64) float hist_q[14] = {};
        simd_fir_complex_apply(in_complex, 1, out_complex, hist_i, hist_q, hb_q15_taps, 15);
        if (out_complex[0] != 11.0f || out_complex[1] != 12.0f) {
            DSD_FPRINTF(stderr, "  FAIL: Invalid complex FIR guard mutated output\n");
            return 1;
        }

        int len = simd_hb_decim2_complex(in_complex, 1, out_complex, hist_i, hist_q, hb_q15_taps, 15);
        if (len != 0) {
            DSD_FPRINTF(stderr, "  FAIL: Complex HB guard returned %d\n", len);
            return 1;
        }

        alignas(64) float in_real[1] = {1.0f};
        alignas(64) float out_real[1] = {13.0f};
        alignas(64) float hist_real[14] = {};
        len = simd_hb_decim2_real(in_real, 1, out_real, hist_real, sparse_taps, 2);
        if (len != 0 || out_real[0] != 13.0f) {
            DSD_FPRINTF(stderr, "  FAIL: Real HB invalid-tap guard mutated output\n");
            return 1;
        }
    }

    std::printf("  PASS\n");
    return 0;
}

int
main(void) {
    std::printf("SIMD FIR implementation: %s\n\n", simd_fir_get_impl_name());

    int failures = 0;

    /* Test 63-tap symmetric FIR */
    failures += test_complex_fir_63tap();

    /* Test complex half-band decimators with different tap lengths */
    failures += test_complex_hb_decim<15>(hb_q15_taps, "15-tap");
    failures += test_complex_hb_decim<31>(hb31_q15_taps, "31-tap");

    /* Test real half-band decimators */
    failures += test_real_hb_decim<15>(hb_q15_taps, "15-tap");
    failures += test_real_hb_decim<31>(hb31_q15_taps, "31-tap");

    /* Test history continuity */
    failures += test_history_continuity();

    /* Test small blocks */
    failures += test_small_blocks();

    /* Regressions for history handling on short/zero-output blocks */
    failures += test_complex_short_block_history();
    failures += test_zero_output_history_updates();
    failures += test_public_scalar_fallback_edges();

#if defined(__x86_64__) || defined(_M_X64)
    failures += test_direct_complex_fir_backend("sse2", simd_fir_complex_apply_sse2);
    failures += test_direct_complex_hb_backend("sse2", simd_hb_decim2_complex_sse2);
    failures += test_direct_real_hb_backend("sse2", simd_hb_decim2_real_sse2);
    failures += test_direct_backend_tail_mix("sse2", simd_fir_complex_apply_sse2, simd_hb_decim2_complex_sse2,
                                             simd_hb_decim2_real_sse2);
    failures += test_direct_backend_invalid_guards("sse2", simd_fir_complex_apply_sse2, simd_hb_decim2_complex_sse2,
                                                   simd_hb_decim2_real_sse2);
    failures += test_direct_fixed_hb_kernels("sse2", simd_hb_decim2_complex_sse2, 7);
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL)
    if (dsd_neo_cpu_has_avx2_with_os_support()) {
        failures += test_direct_complex_fir_backend("avx2", simd_fir_complex_apply_avx2);
        failures += test_direct_complex_hb_backend("avx2", simd_hb_decim2_complex_avx2);
        failures += test_direct_real_hb_backend("avx2", simd_hb_decim2_real_avx2);
        failures += test_direct_backend_tail_mix("avx2", simd_fir_complex_apply_avx2, simd_hb_decim2_complex_avx2,
                                                 simd_hb_decim2_real_avx2);
        failures += test_direct_backend_invalid_guards("avx2", simd_fir_complex_apply_avx2, simd_hb_decim2_complex_avx2,
                                                       simd_hb_decim2_real_avx2);
        failures += test_direct_fixed_hb_kernels("avx2", simd_hb_decim2_complex_avx2, 15);
    } else {
        std::printf("Skipping direct AVX2 backend tests: CPU/OS AVX2+FMA support unavailable\n");
    }
#endif
#endif

#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    failures += test_direct_complex_fir_backend("neon", simd_fir_complex_apply_neon);
    failures += test_direct_complex_hb_backend("neon", simd_hb_decim2_complex_neon);
    failures += test_direct_real_hb_backend("neon", simd_hb_decim2_real_neon);
    failures += test_direct_backend_tail_mix("neon", simd_fir_complex_apply_neon, simd_hb_decim2_complex_neon,
                                             simd_hb_decim2_real_neon);
    failures += test_direct_backend_invalid_guards("neon", simd_fir_complex_apply_neon, simd_hb_decim2_complex_neon,
                                                   simd_hb_decim2_real_neon);
    failures += test_direct_fixed_hb_kernels("neon", simd_hb_decim2_complex_neon, 7);
#endif

    if (failures > 0) {
        std::printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }

    std::printf("\nAll tests PASSED\n");
    return 0;
}
