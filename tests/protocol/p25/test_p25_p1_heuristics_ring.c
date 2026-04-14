// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify that the P25P1 heuristics circular history behaves as a proper ring:
 * - count saturates at HEURISTICS_SIZE
 * - sum tracks a sliding window over the most recent HEURISTICS_SIZE values
 * - index advances modulo HEURISTICS_SIZE
 */

#include <math.h>
#include <stdio.h>

#include <dsd-neo/dsp/p25p1_heuristics.h>

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float_close(const char* tag, float got, float want, float eps) {
    float diff = fabsf(got - want);
    if (diff > eps) {
        fprintf(stderr, "%s: got %.6f want %.6f (diff=%.6f)\n", tag, got, want, diff);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    /* First sample in a C4FM span has no valid previous dibit and must be skipped. */
    {
        P25Heuristics h_first;
        initialize_p25_heuristics(&h_first);

        AnalogSignal samples[2];
        /* Sentinel element to catch accidental i-1 reads deterministically. */
        samples[0].value = -9999;
        samples[0].dibit = 3;
        samples[0].corrected_dibit = 3;
        samples[0].sequence_broken = 0;
        /* Actual one-element span starts here. */
        samples[1].value = 1234;
        samples[1].dibit = 1;
        samples[1].corrected_dibit = 1;
        samples[1].sequence_broken = 0;

        contribute_to_heuristics(/* rf_mod=C4FM */ 0, &h_first, &samples[1], 1);

        int total_updates = 0;
        for (int prev = 0; prev < 4; prev++) {
            for (int dibit = 0; dibit < 4; dibit++) {
                total_updates += h_first.symbols[prev][dibit].count;
            }
        }
        rc |= expect_int("first sample without previous dibit is skipped", total_updates, 0);
    }

    /* Degenerate variance buckets should be safely ignored by PDF evaluation. */
    {
        P25Heuristics h_degenerate;
        initialize_p25_heuristics(&h_degenerate);

        const int modeled_count = HEURISTICS_SIZE;
        for (int d = 0; d < 4; d++) {
            SymbolHeuristics* sh = &h_degenerate.symbols[0][d];
            float mean = (float)(d * 10);
            sh->count = modeled_count;
            sh->sum = mean * (float)modeled_count;
            sh->var_sum = 0.0f; /* guard path: evaluate_pdf() returns 0 */
        }

        /* Only dibit 2 has non-degenerate variance and should win at analog_value=20. */
        h_degenerate.symbols[0][2].var_sum = 400.0f;

        int dibit = -1;
        int valid = estimate_symbol(/* rf_mod=QPSK (ignore previous dibit) */ 1, &h_degenerate, 3, 20, &dibit);
        rc |= expect_int("degenerate variance estimate valid", valid, 1);
        rc |= expect_int("degenerate variance winner", dibit, 2);
    }

    P25Heuristics h;
    initialize_p25_heuristics(&h);

    /* Drive a single SymbolHeuristics bucket (prev=0, dibit=0) past capacity. */
    enum { N = HEURISTICS_SIZE + 6 };

    AnalogSignal sig[N];
    for (int i = 0; i < N; i++) {
        sig[i].value = i; /* distinct, monotonically increasing samples */
        sig[i].dibit = 0;
        sig[i].corrected_dibit = 0;
        sig[i].sequence_broken = (i == 0) ? 1 : 0; /* first element skipped when using previous dibit */
    }

    /* rf_mod=0 => C4FM, enables USE_PREVIOUS_DIBIT path */
    int rf_mod = 0;
    contribute_to_heuristics(rf_mod, &h, sig, N);

    SymbolHeuristics* sh = &h.symbols[0][0];

    int updates = N - 1; /* first AnalogSignal skipped due to sequence_broken */
    int expected_count = (updates < HEURISTICS_SIZE) ? updates : HEURISTICS_SIZE;
    rc |= expect_int("count", sh->count, expected_count);

    int expected_index = updates % HEURISTICS_SIZE;
    rc |= expect_int("index", sh->index, expected_index);

    int start = 1;
    if (updates > HEURISTICS_SIZE) {
        start = updates - HEURISTICS_SIZE + 1;
    }
    int end = updates;

    /* Verify that sum tracks a sliding window over the most recent HEURISTICS_SIZE values. */
    float want_sum = 0.0f;
    for (int v = start; v <= end; v++) {
        want_sum += (float)v;
    }
    if (updates <= HEURISTICS_SIZE) {
        /* No eviction yet: sum of values 1..updates */
        rc |= expect_float_close("sum (no wrap)", sh->sum, want_sum, 1e-3f);
    } else {
        /* Eviction path: last HEURISTICS_SIZE values, i.e., start..updates */
        rc |= expect_float_close("sum (sliding window)", sh->sum, want_sum, 1e-2f);
    }

    /* Mean should match the active window mean (stored at the last written slot). */
    float want_mean = want_sum / (float)expected_count;
    int last_slot = (sh->index + HEURISTICS_SIZE - 1) % HEURISTICS_SIZE;
    rc |= expect_float_close("mean (last slot)", sh->means[last_slot], want_mean, 1e-4f);

    /* var_sum must match sum((x_i - mean)^2) over the active window. */
    float want_var_sum = 0.0f;
    for (int v = start; v <= end; v++) {
        float d = (float)v - want_mean;
        want_var_sum += d * d;
    }
    rc |= expect_float_close("var_sum (sliding window)", sh->var_sum, want_var_sum, 0.5f);

    return rc;
}
