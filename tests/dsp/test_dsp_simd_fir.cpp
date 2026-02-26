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

#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/simd_fir.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const float kTolerance = 1e-5f;

static bool
arrays_close(const float* a, const float* b, int n, float tol) {
    for (int i = 0; i < n; i++) {
        if (std::fabs(a[i] - b[i]) > tol) {
            std::fprintf(stderr, "  Mismatch at [%d]: got %.8f, expected %.8f (diff=%.8e)\n", i, a[i], b[i],
                         a[i] - b[i]);
            return false;
        }
    }
    return true;
}

/* Scalar reference for complex symmetric FIR (no decimation) */
static void
fir_complex_scalar_ref(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
                       int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0 || in_len < 2) {
        return;
    }

    const int N = in_len >> 1;
    const int hist_len = taps_len - 1;
    const int center = (taps_len - 1) >> 1;

    float lastI = (N > 0) ? in[(N - 1) << 1] : 0.0f;
    float lastQ = (N > 0) ? in[((N - 1) << 1) + 1] : 0.0f;

    auto get_iq = [&](int src_idx, float& xi, float& xq) {
        if (src_idx < hist_len) {
            xi = hist_i[src_idx];
            xq = hist_q[src_idx];
        } else {
            int rel = src_idx - hist_len;
            if (rel < N) {
                xi = in[rel << 1];
                xq = in[(rel << 1) + 1];
            } else {
                xi = lastI;
                xq = lastQ;
            }
        }
    };

    for (int n = 0; n < N; n++) {
        int center_idx = hist_len + n;
        float accI = 0.0f;
        float accQ = 0.0f;

        float ci, cq;
        get_iq(center_idx, ci, cq);
        accI += taps[center] * ci;
        accQ += taps[center] * cq;

        for (int k = 0; k < center; k++) {
            float ce = taps[k];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - k;
            float xmI, xmQ, xpI, xpQ;
            get_iq(center_idx - d, xmI, xmQ);
            get_iq(center_idx + d, xpI, xpQ);
            accI += ce * (xmI + xpI);
            accQ += ce * (xmQ + xpQ);
        }

        out[n << 1] = accI;
        out[(n << 1) + 1] = accQ;
    }

    /* Update history */
    if (N >= hist_len) {
        for (int k = 0; k < hist_len; k++) {
            int rel = N - hist_len + k;
            hist_i[k] = in[rel << 1];
            hist_q[k] = in[(rel << 1) + 1];
        }
    } else {
        int need = hist_len - N;
        if (need > 0) {
            std::memmove(hist_i, hist_i + (hist_len - need), (size_t)need * sizeof(float));
            std::memmove(hist_q, hist_q + (hist_len - need), (size_t)need * sizeof(float));
        }
        for (int k = 0; k < N; k++) {
            hist_i[need + k] = in[k << 1];
            hist_q[need + k] = in[(k << 1) + 1];
        }
    }
}

/* Scalar reference for complex half-band decimator */
static int
hb_decim2_complex_scalar_ref(const float* in, int in_len, float* out, float* hist_i, float* hist_q, const float* taps,
                             int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0) {
        return 0;
    }

    int ch_len = in_len >> 1;
    if (ch_len <= 0) {
        return 0;
    }
    int out_ch_len = ch_len >> 1;

    const int center = (taps_len - 1) >> 1;
    const int left_len = taps_len - 1;
    float lastI = in[in_len - 2];
    float lastQ = in[in_len - 1];

    auto get_iq = [&](int src_idx, float& xi, float& xq) {
        if (src_idx < left_len) {
            xi = hist_i[src_idx];
            xq = hist_q[src_idx];
        } else {
            int rel = src_idx - left_len;
            if (rel < ch_len) {
                xi = in[rel << 1];
                xq = in[(rel << 1) + 1];
            } else {
                xi = lastI;
                xq = lastQ;
            }
        }
    };

    for (int n = 0; n < out_ch_len; n++) {
        int center_idx = left_len + (n << 1);
        float accI = 0.0f;
        float accQ = 0.0f;

        float ci, cq;
        get_iq(center_idx, ci, cq);
        accI += taps[center] * ci;
        accQ += taps[center] * cq;

        for (int e = 0; e < center; e += 2) {
            float ce = taps[e];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - e;
            float xmI, xmQ, xpI, xpQ;
            get_iq(center_idx - d, xmI, xmQ);
            get_iq(center_idx + d, xpI, xpQ);
            accI += ce * (xmI + xpI);
            accQ += ce * (xmQ + xpQ);
        }

        out[n << 1] = accI;
        out[(n << 1) + 1] = accQ;
    }

    /* Update history */
    if (ch_len >= left_len) {
        int start = ch_len - left_len;
        for (int k = 0; k < left_len; k++) {
            int rel = start + k;
            hist_i[k] = in[rel << 1];
            hist_q[k] = in[(rel << 1) + 1];
        }
    } else {
        int keep = left_len - ch_len;
        std::memmove(hist_i, hist_i + ch_len, (size_t)keep * sizeof(float));
        std::memmove(hist_q, hist_q + ch_len, (size_t)keep * sizeof(float));
        for (int k = 0; k < ch_len; k++) {
            hist_i[keep + k] = in[k << 1];
            hist_q[keep + k] = in[(k << 1) + 1];
        }
    }

    return out_ch_len << 1;
}

/* Scalar reference for real half-band decimator */
static int
hb_decim2_real_scalar_ref(const float* in, int in_len, float* out, float* hist, const float* taps, int taps_len) {
    if (taps_len < 3 || (taps_len & 1) == 0) {
        return 0;
    }
    if (in_len <= 0) {
        return 0;
    }

    const int hist_len = taps_len - 1;
    const int center = (taps_len - 1) >> 1;
    int out_len = in_len >> 1;

    float last = in[in_len - 1];

    auto get_sample = [&](int src_idx) -> float {
        if (src_idx < hist_len) {
            return hist[src_idx];
        } else {
            int rel = src_idx - hist_len;
            return (rel < in_len) ? in[rel] : last;
        }
    };

    for (int n = 0; n < out_len; n++) {
        int center_idx = hist_len + (n << 1);
        float acc = 0.0f;

        acc += taps[center] * get_sample(center_idx);

        for (int e = 0; e < center; e += 2) {
            float ce = taps[e];
            if (ce == 0.0f) {
                continue;
            }
            int d = center - e;
            acc += ce * (get_sample(center_idx - d) + get_sample(center_idx + d));
        }

        out[n] = acc;
    }

    /* Update history */
    if (in_len >= hist_len) {
        std::memcpy(hist, in + (in_len - hist_len), (size_t)hist_len * sizeof(float));
    } else {
        int need = hist_len - in_len;
        if (need > 0) {
            std::memmove(hist, hist + in_len, (size_t)need * sizeof(float));
        }
        std::memcpy(hist + need, in, (size_t)in_len * sizeof(float));
    }

    return out_len;
}

/* Generate pseudo-random float in [-1, 1] */
static float
randf() {
    return ((float)std::rand() / (float)RAND_MAX) * 2.0f - 1.0f;
}

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

    std::memset(hist_i_simd, 0, sizeof(hist_i_simd));
    std::memset(hist_q_simd, 0, sizeof(hist_q_simd));
    std::memset(hist_i_ref, 0, sizeof(hist_i_ref));
    std::memset(hist_q_ref, 0, sizeof(hist_q_ref));

    /* Generate random input */
    for (int i = 0; i < N * 2; i++) {
        in[i] = randf();
    }

    /* Run both implementations */
    simd_fir_complex_apply(in, N * 2, out_simd, hist_i_simd, hist_q_simd, taps, taps_len);
    fir_complex_scalar_ref(in, N * 2, out_ref, hist_i_ref, hist_q_ref, taps, taps_len);

    if (!arrays_close(out_simd, out_ref, N * 2, kTolerance)) {
        std::fprintf(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
        std::fprintf(stderr, "  FAIL: History I mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
        std::fprintf(stderr, "  FAIL: History Q mismatch\n");
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

    std::memset(hist_i_simd, 0, sizeof(hist_i_simd));
    std::memset(hist_q_simd, 0, sizeof(hist_q_simd));
    std::memset(hist_i_ref, 0, sizeof(hist_i_ref));
    std::memset(hist_q_ref, 0, sizeof(hist_q_ref));

    for (int i = 0; i < N * 2; i++) {
        in[i] = randf();
    }

    int len_simd = simd_hb_decim2_complex(in, N * 2, out_simd, hist_i_simd, hist_q_simd, taps, TapsLen);
    int len_ref = hb_decim2_complex_scalar_ref(in, N * 2, out_ref, hist_i_ref, hist_q_ref, taps, TapsLen);

    if (len_simd != len_ref) {
        std::fprintf(stderr, "  FAIL: Output length mismatch (%d vs %d)\n", len_simd, len_ref);
        return 1;
    }

    if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
        std::fprintf(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
        std::fprintf(stderr, "  FAIL: History I mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
        std::fprintf(stderr, "  FAIL: History Q mismatch\n");
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

    std::memset(hist_simd, 0, sizeof(hist_simd));
    std::memset(hist_ref, 0, sizeof(hist_ref));

    for (int i = 0; i < N; i++) {
        in[i] = randf();
    }

    int len_simd = simd_hb_decim2_real(in, N, out_simd, hist_simd, taps, TapsLen);
    int len_ref = hb_decim2_real_scalar_ref(in, N, out_ref, hist_ref, taps, TapsLen);

    if (len_simd != len_ref) {
        std::fprintf(stderr, "  FAIL: Output length mismatch (%d vs %d)\n", len_simd, len_ref);
        return 1;
    }

    if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
        std::fprintf(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }

    if (!arrays_close(hist_simd, hist_ref, hist_len, kTolerance)) {
        std::fprintf(stderr, "  FAIL: History mismatch\n");
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

    std::memset(hist_i_simd, 0, sizeof(hist_i_simd));
    std::memset(hist_q_simd, 0, sizeof(hist_q_simd));
    std::memset(hist_i_ref, 0, sizeof(hist_i_ref));
    std::memset(hist_q_ref, 0, sizeof(hist_q_ref));

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
            hb_decim2_complex_scalar_ref(in, block_size * 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);

        if (len_simd != len_ref) {
            std::fprintf(stderr, "  FAIL: Block %d length mismatch\n", blk);
            return 1;
        }

        if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
            std::fprintf(stderr, "  FAIL: Block %d output mismatch\n", blk);
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
            std::fprintf(stderr, "  FAIL: Test size exceeds max (%d > %d)\n", N, kMaxN);
            return 1;
        }

        alignas(64) float in[kMaxN * 2];
        alignas(64) float out_simd[kMaxN];
        alignas(64) float out_ref[kMaxN];
        alignas(64) float hist_i_simd[hist_len];
        alignas(64) float hist_q_simd[hist_len];
        alignas(64) float hist_i_ref[hist_len];
        alignas(64) float hist_q_ref[hist_len];

        std::memset(hist_i_simd, 0, sizeof(hist_i_simd));
        std::memset(hist_q_simd, 0, sizeof(hist_q_simd));
        std::memset(hist_i_ref, 0, sizeof(hist_i_ref));
        std::memset(hist_q_ref, 0, sizeof(hist_q_ref));

        for (int i = 0; i < N * 2; i++) {
            in[i] = randf();
        }

        int len_simd = simd_hb_decim2_complex(in, N * 2, out_simd, hist_i_simd, hist_q_simd, hb_q15_taps, taps_len);
        int len_ref = hb_decim2_complex_scalar_ref(in, N * 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);

        if (len_simd != len_ref) {
            std::fprintf(stderr, "  FAIL: Size %d length mismatch\n", N);
            return 1;
        }

        if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
            std::fprintf(stderr, "  FAIL: Size %d output mismatch\n", N);
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
    int len_ref = hb_decim2_complex_scalar_ref(in, ch_len * 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);

    if (len_simd != len_ref) {
        std::fprintf(stderr, "  FAIL: Output length mismatch (%d vs %d)\n", len_simd, len_ref);
        return 1;
    }
    if (!arrays_close(out_simd, out_ref, len_simd, kTolerance)) {
        std::fprintf(stderr, "  FAIL: Output mismatch\n");
        return 1;
    }
    if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
        std::fprintf(stderr, "  FAIL: History I mismatch\n");
        return 1;
    }
    if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
        std::fprintf(stderr, "  FAIL: History Q mismatch\n");
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
            hb_decim2_complex_scalar_ref(in_complex, 2, out_ref, hist_i_ref, hist_q_ref, hb_q15_taps, taps_len);

        if (len_simd != len_ref) {
            std::fprintf(stderr, "  FAIL: Complex zero-output length mismatch (%d vs %d)\n", len_simd, len_ref);
            return 1;
        }
        if (!arrays_close(hist_i_simd, hist_i_ref, hist_len, kTolerance)) {
            std::fprintf(stderr, "  FAIL: Complex History I mismatch\n");
            return 1;
        }
        if (!arrays_close(hist_q_simd, hist_q_ref, hist_len, kTolerance)) {
            std::fprintf(stderr, "  FAIL: Complex History Q mismatch\n");
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
        int len_ref = hb_decim2_real_scalar_ref(in_real, 1, out_ref, hist_ref, hb_q15_taps, taps_len);

        if (len_simd != len_ref) {
            std::fprintf(stderr, "  FAIL: Real zero-output length mismatch (%d vs %d)\n", len_simd, len_ref);
            return 1;
        }
        if (!arrays_close(hist_simd, hist_ref, hist_len, kTolerance)) {
            std::fprintf(stderr, "  FAIL: Real History mismatch\n");
            return 1;
        }
    }

    std::printf("  PASS\n");
    return 0;
}

int
main(void) {
    std::srand(12345);

    std::printf("SIMD FIR implementation: %s\n\n", simd_fir_get_impl_name());

    int failures = 0;

    /* Test 63-tap symmetric FIR */
    failures += test_complex_fir_63tap();

    /* Test complex half-band decimators with different tap lengths */
    failures += test_complex_hb_decim<15>(hb_q15_taps, "15-tap");
    failures += test_complex_hb_decim<23>(hb23_q15_taps, "23-tap");
    failures += test_complex_hb_decim<31>(hb31_q15_taps, "31-tap");

    /* Test real half-band decimators */
    failures += test_real_hb_decim<15>(hb_q15_taps, "15-tap");
    failures += test_real_hb_decim<23>(hb23_q15_taps, "23-tap");
    failures += test_real_hb_decim<31>(hb31_q15_taps, "31-tap");

    /* Test history continuity */
    failures += test_history_continuity();

    /* Test small blocks */
    failures += test_small_blocks();

    /* Regressions for history handling on short/zero-output blocks */
    failures += test_complex_short_block_history();
    failures += test_zero_output_history_updates();

    if (failures > 0) {
        std::printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }

    std::printf("\nAll tests PASSED\n");
    return 0;
}
