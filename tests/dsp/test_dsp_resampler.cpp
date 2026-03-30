// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for polyphase rational resampler (L/M). */

#include <cmath>
#include <cstdint>
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
expected_out_len_for_phase(int in_len, int L, int M, int phase) {
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

static int
expected_out_len_for_block(int in_len, int L, int M) {
    return expected_out_len_for_phase(in_len, L, M, 0);
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

static int
test_generic_state_api(void) {
    dsd_resampler_state s_aligned = {0};
    dsd_resampler_state s_unaligned = {0};
    float* in_aligned = NULL;
    float* in_unaligned_base = NULL;
    float* in_unaligned = NULL;
    float* out_aligned = NULL;
    float* out_unaligned_base = NULL;
    float* out_unaligned = NULL;
    const int L = 3;
    const int M = 2;
    const int N = 96;
    const int out_cap = N * 4;
    int aligned_len = -1;
    int out_len = -1;
    int exp_len = 0;
    int warm = 0;
    int rc = 1;

    if (!dsd_resampler_design(&s_aligned, L, M) || !dsd_resampler_design(&s_unaligned, L, M)) {
        fprintf(stderr, "generic dsd_resampler_design failed\n");
        goto cleanup;
    }
    if (!s_aligned.taps || !s_aligned.hist || s_aligned.taps_per_phase <= 0 || !s_aligned.enabled || !s_unaligned.taps
        || !s_unaligned.hist || s_unaligned.taps_per_phase <= 0 || !s_unaligned.enabled) {
        fprintf(stderr, "generic resampler state not initialized\n");
        goto cleanup;
    }

    in_aligned = (float*)dsd_neo_aligned_malloc((size_t)N * sizeof(float));
    in_unaligned_base = (float*)dsd_neo_aligned_malloc((size_t)(N + 1) * sizeof(float));
    out_aligned = (float*)dsd_neo_aligned_malloc((size_t)out_cap * sizeof(float));
    out_unaligned_base = (float*)dsd_neo_aligned_malloc((size_t)(out_cap + 1) * sizeof(float));
    if (!in_aligned || !in_unaligned_base || !out_aligned || !out_unaligned_base) {
        fprintf(stderr, "generic resampler buffer allocation failed\n");
        goto cleanup;
    }

    in_unaligned = in_unaligned_base + 1;
    out_unaligned = out_unaligned_base + 1;
    if ((((uintptr_t)in_unaligned) % 64U) == 0U || (((uintptr_t)out_unaligned) % 64U) == 0U) {
        fprintf(stderr, "generic resampler unaligned buffers unexpectedly 64-byte aligned\n");
        goto cleanup;
    }

    for (int i = 0; i < N; i++) {
        in_aligned[i] = 1.0f;
        in_unaligned[i] = 1.0f;
    }
    memset(out_aligned, 0, (size_t)out_cap * sizeof(float));
    memset(out_unaligned, 0, (size_t)out_cap * sizeof(float));

    aligned_len = dsd_resampler_process_block(&s_aligned, in_aligned, N, out_aligned, out_cap);
    out_len = dsd_resampler_process_block(&s_unaligned, in_unaligned, N, out_unaligned, out_cap);
    exp_len = expected_out_len_for_block(N, L, M);
    if (aligned_len != exp_len || out_len != exp_len) {
        fprintf(stderr, "generic resampler len mismatch: aligned=%d unaligned=%d expected=%d\n", aligned_len, out_len,
                exp_len);
        goto cleanup;
    }
    if (!arrays_close(out_aligned, out_unaligned, out_len, 1e-5f)) {
        fprintf(stderr, "generic resampler unaligned output mismatch\n");
        goto cleanup;
    }

    warm = s_unaligned.taps_per_phase * 2;
    if (warm < 0) {
        warm = 0;
    }
    if (warm > out_len) {
        warm = out_len;
    }
    for (int i = warm; i < out_len; i++) {
        if (!approx_eq(out_unaligned[i], 1.0f, 2e-3f)) {
            fprintf(stderr, "generic resampler out[%d]=%f not within tol of 1.0\n", i, out_unaligned[i]);
            goto cleanup;
        }
    }

    dsd_resampler_clear_history(&s_unaligned);
    if (s_unaligned.phase != 0 || s_unaligned.hist_head != 0) {
        fprintf(stderr, "generic resampler clear_history did not rewind state\n");
        goto cleanup;
    }

    dsd_resampler_reset(&s_unaligned);
    if (s_unaligned.enabled != 0 || s_unaligned.taps != NULL || s_unaligned.hist != NULL || s_unaligned.L != 1
        || s_unaligned.M != 1) {
        fprintf(stderr, "generic resampler reset did not clear state\n");
        goto cleanup;
    }

    rc = 0;

cleanup:
    dsd_resampler_reset(&s_aligned);
    dsd_resampler_reset(&s_unaligned);
    if (in_aligned) {
        dsd_neo_aligned_free(in_aligned);
    }
    if (in_unaligned_base) {
        dsd_neo_aligned_free(in_unaligned_base);
    }
    if (out_aligned) {
        dsd_neo_aligned_free(out_aligned);
    }
    if (out_unaligned_base) {
        dsd_neo_aligned_free(out_unaligned_base);
    }
    return rc;
}

static int
test_generic_state_first_use_from_poisoned_storage(void) {
    dsd_resampler_state s;
    memset(&s, 0xA5, sizeof(s));

    if (!dsd_resampler_design(&s, 3, 2)) {
        fprintf(stderr, "generic resampler design failed from poisoned first-use state\n");
        dsd_resampler_reset(&s);
        return 1;
    }
    if (!s.enabled || s.L != 3 || s.M != 2 || s.taps == NULL || s.hist == NULL || s.taps_per_phase <= 0) {
        fprintf(stderr, "generic resampler poisoned first-use state not initialized correctly\n");
        dsd_resampler_reset(&s);
        return 1;
    }

    dsd_resampler_reset(&s);
    if (s.enabled != 0 || s.taps != NULL || s.hist != NULL || s.L != 1 || s.M != 1) {
        fprintf(stderr, "generic resampler poisoned first-use reset did not clear state\n");
        return 1;
    }

    return 0;
}

static int
test_generic_state_capacity_failure_is_transactional(void) {
    dsd_resampler_state s_retry = {0};
    dsd_resampler_state s_ref = {0};
    const int L = 5;
    const int M = 2;
    const int N = 19;
    int required = 0;
    int fail_cap = 0;
    int phase_before = 0;
    int head_before = 0;
    int fail_rc = -1;
    int retry_len = -1;
    int ref_len = -1;
    int rc = 1;
    std::vector<float> in((size_t)N);
    std::vector<float> hist_before;
    std::vector<float> out_fail;
    std::vector<float> out_retry;
    std::vector<float> out_ref;

    if (!dsd_resampler_design(&s_retry, L, M) || !dsd_resampler_design(&s_ref, L, M)) {
        fprintf(stderr, "generic transactional capacity test design failed\n");
        goto cleanup;
    }

    for (int i = 0; i < N; i++) {
        in[(size_t)i] = 0.4f * sinf((float)i * 0.17f) + 0.2f * cosf((float)i * 0.09f) - 0.03f * (float)(i - 6);
    }

    required = expected_out_len_for_phase(N, L, M, s_retry.phase);
    if (required <= 1) {
        fprintf(stderr, "generic transactional capacity test expected too few outputs: %d\n", required);
        goto cleanup;
    }
    fail_cap = required - 1;
    phase_before = s_retry.phase;
    head_before = s_retry.hist_head;
    hist_before.resize((size_t)s_retry.taps_per_phase * 2U);
    memcpy(hist_before.data(), s_retry.hist, hist_before.size() * sizeof(float));
    out_fail.resize((size_t)fail_cap);
    out_retry.resize((size_t)required);
    out_ref.resize((size_t)required);

    fail_rc = dsd_resampler_process_block(&s_retry, in.data(), N, out_fail.data(), fail_cap);
    if (fail_rc != -1) {
        fprintf(stderr, "generic transactional capacity test expected failure, got %d\n", fail_rc);
        goto cleanup;
    }
    if (s_retry.phase != phase_before || s_retry.hist_head != head_before) {
        fprintf(stderr, "generic transactional capacity test mutated phase/head on failure\n");
        goto cleanup;
    }
    if (memcmp(s_retry.hist, hist_before.data(), hist_before.size() * sizeof(float)) != 0) {
        fprintf(stderr, "generic transactional capacity test mutated history on failure\n");
        goto cleanup;
    }

    retry_len = dsd_resampler_process_block(&s_retry, in.data(), N, out_retry.data(), required);
    ref_len = dsd_resampler_process_block(&s_ref, in.data(), N, out_ref.data(), required);
    if (retry_len != ref_len || retry_len != required) {
        fprintf(stderr, "generic transactional capacity retry len mismatch: retry=%d ref=%d expected=%d\n", retry_len,
                ref_len, required);
        goto cleanup;
    }
    if (!arrays_close(out_retry.data(), out_ref.data(), required, 1e-5f)) {
        fprintf(stderr, "generic transactional capacity retry output mismatch\n");
        goto cleanup;
    }
    if (s_retry.phase != s_ref.phase || s_retry.hist_head != s_ref.hist_head) {
        fprintf(stderr, "generic transactional capacity retry state mismatch\n");
        goto cleanup;
    }
    if (memcmp(s_retry.hist, s_ref.hist, (size_t)s_retry.taps_per_phase * 2U * sizeof(float)) != 0) {
        fprintf(stderr, "generic transactional capacity retry history mismatch\n");
        goto cleanup;
    }

    rc = 0;

cleanup:
    dsd_resampler_reset(&s_retry);
    dsd_resampler_reset(&s_ref);
    return rc;
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
    if (test_generic_state_api() != 0) {
        free_demod_state(s);
        return 1;
    }
    if (test_generic_state_first_use_from_poisoned_storage() != 0) {
        free_demod_state(s);
        return 1;
    }
    if (test_generic_state_capacity_failure_is_transactional() != 0) {
        free_demod_state(s);
        return 1;
    }

    free_demod_state(s);
    return 0;
}
