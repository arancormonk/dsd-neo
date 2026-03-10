// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for polyphase rational resampler (L/M). */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/runtime/mem.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static int
approx_eq(float a, float b, float tol) {
    float d = fabsf(a - b);
    return d <= tol;
}

static int
arrays_close(const float* a, const float* b, int n, float tol) {
    for (int i = 0; i < n; i++) {
        if (!approx_eq(a[i], b[i], tol)) {
            fprintf(stderr, "mismatch at [%d]: got %.8f expected %.8f\n", i, a[i], b[i]);
            return 0;
        }
    }
    return 1;
}

static int
expected_out_len_for_block(int in_len, int L, int M) {
    int phase = 0;
    int out_len = 0;
    for (int n = 0; n < in_len; n++) {
        int local = phase;
        while (local < L) {
            out_len++;
            local += M;
        }
        phase = local - L;
    }
    return out_len;
}

static demod_state*
alloc_demod_state(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (s) {
        memset(s, 0, sizeof(*s));
    }
    return s;
}

static void
free_demod_state(demod_state* s) {
    if (!s) {
        return;
    }
    if (s->resamp_taps) {
        dsd_neo_aligned_free(s->resamp_taps);
    }
    if (s->resamp_hist) {
        dsd_neo_aligned_free(s->resamp_hist);
    }
    free(s);
}

static int
resamp_process_block_ref(const demod_state* s, const float* in, int in_len, float* out, int* phase,
                         std::vector<float>& hist) {
    const int L = s->resamp_L;
    const int M = s->resamp_M;
    const int K = s->resamp_taps_per_phase;
    int out_len = 0;
    int cur_phase = *phase;

    for (int n = 0; n < in_len; n++) {
        memmove(hist.data(), hist.data() + 1, (size_t)(K - 1) * sizeof(float));
        hist[(size_t)K - 1U] = in[n];

        int local_phase = cur_phase;
        while (local_phase < L) {
            const float* phase_taps = s->resamp_taps + (size_t)local_phase * (size_t)K;
            float acc = 0.0f;
            for (int k = 0; k < K; k++) {
                acc += hist[(size_t)k] * phase_taps[k];
            }
            out[out_len++] = acc;
            local_phase += M;
        }
        cur_phase = local_phase - L;
    }

    *phase = cur_phase;
    return out_len;
}

static int
test_reference_equivalence(void) {
    demod_state* s = alloc_demod_state();
    if (!s) {
        fprintf(stderr, "alloc demod_state failed\n");
        return 1;
    }

    const int L = 3;
    const int M = 2;
    s->resamp_enabled = 1;
    resamp_design(s, L, M);
    if (!s->resamp_taps || !s->resamp_hist || s->resamp_taps_per_phase <= 0) {
        fprintf(stderr, "resamp_design failed to allocate/initialize\n");
        free_demod_state(s);
        return 1;
    }

    const int N = 73;
    std::vector<float> in((size_t)N);
    std::vector<float> out_impl((size_t)N * 4U);
    std::vector<float> out_ref((size_t)N * 4U);
    std::vector<float> hist_ref((size_t)s->resamp_taps_per_phase, 0.0f);
    int phase_ref = 0;

    for (int i = 0; i < N; i++) {
        in[(size_t)i] = cosf((float)i * 0.173f) + 0.25f * sinf((float)i * 0.071f);
    }

    int len_impl = resamp_process_block(s, in.data(), N, out_impl.data());
    int len_ref = resamp_process_block_ref(s, in.data(), N, out_ref.data(), &phase_ref, hist_ref);

    if (len_impl != len_ref) {
        fprintf(stderr, "reference len mismatch: got %d expected %d\n", len_impl, len_ref);
        free_demod_state(s);
        return 1;
    }
    if (s->resamp_phase != phase_ref) {
        fprintf(stderr, "reference phase mismatch: got %d expected %d\n", s->resamp_phase, phase_ref);
        free_demod_state(s);
        return 1;
    }
    if (!arrays_close(out_impl.data(), out_ref.data(), len_impl, 1e-5f)) {
        fprintf(stderr, "reference output mismatch\n");
        free_demod_state(s);
        return 1;
    }

    free_demod_state(s);
    return 0;
}

static int
test_split_continuity(void) {
    demod_state* s_full = alloc_demod_state();
    demod_state* s_split = alloc_demod_state();
    if (!s_full || !s_split) {
        fprintf(stderr, "alloc demod_state failed\n");
        free_demod_state(s_full);
        free_demod_state(s_split);
        return 1;
    }

    const int L = 5;
    const int M = 4;
    s_full->resamp_enabled = 1;
    s_split->resamp_enabled = 1;
    resamp_design(s_full, L, M);
    resamp_design(s_split, L, M);
    if (!s_full->resamp_taps || !s_full->resamp_hist || !s_split->resamp_taps || !s_split->resamp_hist) {
        fprintf(stderr, "resamp_design failed to allocate/initialize\n");
        free_demod_state(s_full);
        free_demod_state(s_split);
        return 1;
    }

    const int N = 81;
    const int tail_n = 19;
    std::vector<float> in((size_t)N);
    std::vector<float> tail((size_t)tail_n);
    std::vector<float> out_full((size_t)N * 4U);
    std::vector<float> out_split((size_t)N * 4U);
    std::vector<float> out_full_tail((size_t)tail_n * 4U);
    std::vector<float> out_split_tail((size_t)tail_n * 4U);

    for (int i = 0; i < N; i++) {
        in[(size_t)i] = sinf((float)i * 0.11f) - 0.35f * cosf((float)i * 0.037f);
    }
    for (int i = 0; i < tail_n; i++) {
        tail[(size_t)i] = 0.5f * sinf((float)(i + N) * 0.19f) + 0.1f * (float)(i - 5);
    }

    int len_full = resamp_process_block(s_full, in.data(), N, out_full.data());

    int offsets[] = {17, 24, 40};
    int produced = 0;
    int start = 0;
    for (int split = 0; split < 4; split++) {
        int end = (split < 3) ? offsets[split] : N;
        int chunk = end - start;
        int wrote = resamp_process_block(s_split, in.data() + start, chunk, out_split.data() + produced);
        produced += wrote;
        start = end;
    }

    if (len_full != produced) {
        fprintf(stderr, "split len mismatch: got %d expected %d\n", produced, len_full);
        free_demod_state(s_full);
        free_demod_state(s_split);
        return 1;
    }
    if (!arrays_close(out_full.data(), out_split.data(), len_full, 1e-5f)) {
        fprintf(stderr, "split output mismatch\n");
        free_demod_state(s_full);
        free_demod_state(s_split);
        return 1;
    }

    int tail_full = resamp_process_block(s_full, tail.data(), tail_n, out_full_tail.data());
    int tail_split = resamp_process_block(s_split, tail.data(), tail_n, out_split_tail.data());

    if (tail_full != tail_split) {
        fprintf(stderr, "split tail len mismatch: got %d expected %d\n", tail_split, tail_full);
        free_demod_state(s_full);
        free_demod_state(s_split);
        return 1;
    }
    if (!arrays_close(out_full_tail.data(), out_split_tail.data(), tail_full, 1e-5f)) {
        fprintf(stderr, "split tail output mismatch\n");
        free_demod_state(s_full);
        free_demod_state(s_split);
        return 1;
    }
    if (s_full->resamp_phase != s_split->resamp_phase || s_full->resamp_hist_head != s_split->resamp_hist_head) {
        fprintf(stderr, "split state mismatch\n");
        free_demod_state(s_full);
        free_demod_state(s_split);
        return 1;
    }

    free_demod_state(s_full);
    free_demod_state(s_split);
    return 0;
}

int
main(void) {
    // demod_state is large; allocate on heap to avoid stack overflow
    demod_state* s = alloc_demod_state();
    if (!s) {
        fprintf(stderr, "alloc demod_state failed\n");
        return 1;
    }
    const int L = 3, M = 2;
    s->resamp_enabled = 1;

    resamp_design(s, L, M);
    if (!s->resamp_taps || !s->resamp_hist || s->resamp_taps_per_phase <= 0) {
        fprintf(stderr, "resamp_design failed to allocate/initialize\n");
        free_demod_state(s);
        return 1;
    }

    const int N = 96;
    float in[N];
    for (int i = 0; i < N; i++) {
        in[i] = 1.0f; // DC input (normalized)
    }
    float out[N * 4];
    int out_len = resamp_process_block(s, in, N, out);

    int exp_len = expected_out_len_for_block(N, L, M);
    if (out_len != exp_len) {
        fprintf(stderr, "RESAMP: out_len=%d expected=%d\n", out_len, exp_len);
        free_demod_state(s);
        return 1;
    }
    // DC gain near unity after initial warm-up (history filled)
    int warm = s->resamp_taps_per_phase * 2;
    if (warm < 0) {
        warm = 0;
    }
    if (warm > out_len) {
        warm = out_len;
    }
    for (int i = warm; i < out_len; i++) {
        if (!approx_eq(out[i], 1.0f, 2e-3f)) {
            fprintf(stderr, "RESAMP: out[%d]=%f not within tol of 1.0\n", i, out[i]);
            free_demod_state(s);
            return 1;
        }
    }

    if (test_reference_equivalence() != 0) {
        free_demod_state(s);
        return 1;
    }
    if (test_split_continuity() != 0) {
        free_demod_state(s);
        return 1;
    }

    free_demod_state(s);
    return 0;
}
