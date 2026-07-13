// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for DMR resample-on-sync CACH re-digitization.
 *
 * Tests the complete resample-on-sync flow:
 * 1. Symbol history buffer push/get operations
 * 2. CACH re-digitization with corrected thresholds
 *
 * Verifies that re-digitization produces expected dibits in the correct
 * ring-buffer-relative positions.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/dsp/dmr_sync.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

#define FLOAT_TOL 0.01f

static int g_test_count = 0;
static int g_fail_count = 0;

static void
check_int(const char* name, int expected, int actual) {
    g_test_count++;
    if (expected != actual) {
        printf("FAIL: %s: expected %d, got %d\n", name, expected, actual);
        g_fail_count++;
    }
}

static void
check_float(const char* name, float expected, float actual, float tol) {
    g_test_count++;
    if (fabsf(expected - actual) > tol) {
        printf("FAIL: %s: expected %.4f, got %.4f\n", name, (double)expected, (double)actual);
        g_fail_count++;
    }
}

static float
sync_ascii_to_symbol(char dibit_char) {
    switch (dibit_char) {
        case '0': return +1.0f;
        case '1': return +3.0f;
        case '2': return -1.0f;
        case '3': return -3.0f;
        default: return 0.0f;
    }
}

static void
fill_symbols_from_sync_ascii(const char* sync, float out[DMR_SYNC_SYMBOLS]) {
    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++) {
        out[i] = sync_ascii_to_symbol(sync[i]);
    }
}

/**
 * @brief Test history buffer push and get operations.
 */
static void
test_history_buffer_ops(void) {
    printf("=== test_history_buffer_ops ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    /* Initialize history buffer */
    int ret = dsd_symbol_history_init(&state, DSD_SYMBOL_HISTORY_SIZE);
    check_int("init return", 0, ret);
    check_int("buffer allocated", 1, state.symbol_history != NULL);
    check_int("size", DSD_SYMBOL_HISTORY_SIZE, state.symbol_history_size);
    check_int("head", 0, state.symbol_history_head);
    check_int("count", 0, state.symbol_history_count);

    /* Push some values */
    dsd_symbol_history_push(&state, 1.0f);
    dsd_symbol_history_push(&state, 2.0f);
    dsd_symbol_history_push(&state, 3.0f);

    check_int("count after push", 3, state.symbol_history_count);
    check_int("head after push", 3, state.symbol_history_head);

    /* Generic back offsets are non-negative: 0 is newest. */
    check_float("get 0", 3.0f, dsd_symbol_history_get_back(&state, 0), FLOAT_TOL);
    check_float("get 1", 2.0f, dsd_symbol_history_get_back(&state, 1), FLOAT_TOL);
    check_float("get 2", 1.0f, dsd_symbol_history_get_back(&state, 2), FLOAT_TOL);

    /* Reset */
    dsd_symbol_history_reset(&state);
    check_int("count after reset", 0, state.symbol_history_count);
    check_int("head after reset", 0, state.symbol_history_head);

    /* Cleanup */
    dsd_symbol_history_free(&state);
    check_int("buffer freed", 1, state.symbol_history == NULL);

    printf("test_history_buffer_ops: passed\n\n");
}

/**
 * @brief Test history buffer wrap-around.
 */
static void
test_history_buffer_wrap(void) {
    printf("=== test_history_buffer_wrap ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    /* Use small buffer for wrap test */
    check_int("small init", 0, dsd_symbol_history_init(&state, 4));

    /* Push 6 values into size-4 buffer */
    for (int i = 1; i <= 6; i++) {
        dsd_symbol_history_push(&state, (float)i);
    }

    /* Count should be clamped at buffer size */
    check_int("count clamped", 4, state.symbol_history_count);

    /* Head should have wrapped */
    check_int("head wrapped", 2, state.symbol_history_head);

    /* Buffer should contain [5, 6, 3, 4] with head at 2 */
    /* Most recent is 6, then 5, then 4, then 3 */
    check_float("get 0 (most recent)", 6.0f, dsd_symbol_history_get_back(&state, 0), FLOAT_TOL);
    check_float("get 1", 5.0f, dsd_symbol_history_get_back(&state, 1), FLOAT_TOL);
    check_float("get 2", 4.0f, dsd_symbol_history_get_back(&state, 2), FLOAT_TOL);
    check_float("get 3", 3.0f, dsd_symbol_history_get_back(&state, 3), FLOAT_TOL);

    dsd_symbol_history_free(&state);
    printf("test_history_buffer_wrap: passed\n\n");
}

/**
 * @brief Test CACH re-digitization with ideal thresholds.
 *
 * This test verifies that re-digitization produces expected dibits
 * in the payload buffer.
 */
static void
test_cach_redigitize(void) {
    printf("=== test_cach_redigitize ===\n");

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    /* Initialize history buffer */
    dsd_symbol_history_init(&state, DSD_SYMBOL_HISTORY_SIZE);

    /* Set up ideal thresholds for ±3/±1 symbol levels */
    state.max = 3.0f;
    state.min = -3.0f;
    state.center = 0.0f;
    state.umid = 1.875f;  /* 0.625 * 3 */
    state.lmid = -1.875f; /* 0.625 * -3 */
    state.maxref = 2.4f;
    state.minref = -2.4f;

    /* Create test pattern: known symbol values that map to known dibits
     * Symbol levels: +3 -> dibit 1, +1 -> dibit 0, -1 -> dibit 2, -3 -> dibit 3
     */
    float test_symbols[DMR_RESAMPLE_SYMBOLS + DMR_SYNC_SYMBOLS];

    /* Fill CACH region (66 symbols) with known pattern */
    for (int i = 0; i < DMR_RESAMPLE_SYMBOLS; i++) {
        /* Cycle through all 4 symbol levels */
        switch (i % 4) {
            case 0: test_symbols[i] = 3.0f; break;  /* -> dibit 1 */
            case 1: test_symbols[i] = 1.0f; break;  /* -> dibit 0 */
            case 2: test_symbols[i] = -1.0f; break; /* -> dibit 2 */
            case 3: test_symbols[i] = -3.0f; break; /* -> dibit 3 */
            default: test_symbols[i] = 0.0f; break;
        }
    }

    /* Fill sync region (24 symbols) with BS_VOICE pattern */
    float bs_voice[DMR_SYNC_SYMBOLS];
    fill_symbols_from_sync_ascii(DMR_BS_VOICE_SYNC, bs_voice);
    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++) {
        test_symbols[DMR_RESAMPLE_SYMBOLS + i] = bs_voice[i];
    }

    /* Push all symbols into history */
    for (int i = 0; i < DMR_RESAMPLE_SYMBOLS + DMR_SYNC_SYMBOLS; i++) {
        dsd_symbol_history_push(&state, test_symbols[i]);
    }

    /* Allocate payload buffer */
    const int payload_len = DMR_RESAMPLE_SYMBOLS + DMR_SYNC_SYMBOLS;
    state.dmr_payload_buf = (int*)malloc(sizeof(int) * payload_len);
    DSD_MEMSET(state.dmr_payload_buf, 0xFF, sizeof(int) * payload_len);
    /* Mimic real decoder state: dmr_payload_p points one past the most recent dibit. */
    state.dmr_payload_p = state.dmr_payload_buf + payload_len;

    /* Call CACH resample */
    dmr_resample_cach(&opts, &state, 0);

    /* Verify re-digitized dibits */
    int all_correct = 1;
    for (int i = 0; i < DMR_RESAMPLE_SYMBOLS; i++) {
        int expected_dibit;
        switch (i % 4) {
            case 0: expected_dibit = 1; break; /* +3 */
            case 1: expected_dibit = 0; break; /* +1 */
            case 2: expected_dibit = 2; break; /* -1 */
            case 3: expected_dibit = 3; break; /* -3 */
            default: expected_dibit = -1; break;
        }

        if (state.dmr_payload_buf[i] != expected_dibit) {
            if (all_correct) {
                printf("FAIL: dibit mismatch at index %d: expected %d, got %d\n", i, expected_dibit,
                       state.dmr_payload_buf[i]);
            }
            all_correct = 0;
        }
    }

    g_test_count++;
    if (!all_correct) {
        g_fail_count++;
    }

    free(state.dmr_payload_buf);
    dsd_symbol_history_free(&state);
    printf("test_cach_redigitize: passed\n\n");
}

/**
 * @brief Test full resample_on_sync flow.
 */
static void
test_full_resample_on_sync(void) {
    printf("=== test_full_resample_on_sync ===\n");

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 128;

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    /* Initialize history buffer */
    dsd_symbol_history_init(&state, DSD_SYMBOL_HISTORY_SIZE);

    /* Allocate the CACH/prefix and sync-sized rolling payload window. */
    const int payload_len = DMR_RESAMPLE_SYMBOLS + DMR_SYNC_SYMBOLS;
    state.dmr_payload_buf = (int*)malloc(sizeof(int) * (size_t)payload_len);
    DSD_MEMSET(state.dmr_payload_buf, 0xFF, sizeof(int) * (size_t)payload_len);
    state.dmr_payload_p = state.dmr_payload_buf + payload_len;

    /* Push CACH + sync worth of symbols */
    /* CACH with mild DC offset */
    float dc_offset = 0.2f;
    for (int i = 0; i < DMR_RESAMPLE_SYMBOLS; i++) {
        float sym;
        switch (i % 4) {
            case 0: sym = 3.0f + dc_offset; break;
            case 1: sym = 1.0f + dc_offset; break;
            case 2: sym = -1.0f + dc_offset; break;
            case 3: sym = -3.0f + dc_offset; break;
            default: sym = 0.0f; break;
        }
        dsd_symbol_history_push(&state, sym);
    }

    /* Sync pattern with the same DC offset and deterministic first-frame noise. */
    float bs_voice[DMR_SYNC_SYMBOLS];
    const float noise[DMR_SYNC_SYMBOLS] = {0.1f,  -0.2f,  0.15f, -0.05f, 0.2f, -0.1f, 0.05f, -0.15f,
                                           0.3f,  -0.25f, 0.1f,  -0.3f,  0.2f, -0.2f, 0.15f, -0.1f,
                                           0.05f, -0.05f, 0.1f,  -0.1f,  0.2f, -0.2f, 0.25f, -0.25f};
    fill_symbols_from_sync_ascii(DMR_BS_VOICE_SYNC, bs_voice);
    float sum_pos = 0.0f;
    float sum_neg = 0.0f;
    int count_pos = 0;
    int count_neg = 0;
    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++) {
        float observed = bs_voice[i] + dc_offset + noise[i];
        dsd_symbol_history_push(&state, observed);
        if (observed > 0.0f) {
            sum_pos += observed;
            count_pos++;
        } else {
            sum_neg += observed;
            count_neg++;
        }
    }

    /* Call full resample_on_sync */
    int ret = dmr_resample_on_sync(&opts, &state);

    check_int("resample_on_sync return", 0, ret);

    /* The generic warm-start must reproduce the former DMR noisy-sync means exactly. */
    float expected_max = sum_pos / (float)count_pos;
    float expected_min = sum_neg / (float)count_neg;
    float expected_center = (expected_max + expected_min) / 2.0f;
    check_float("noisy max", expected_max, state.max, FLOAT_TOL);
    check_float("noisy min", expected_min, state.min, FLOAT_TOL);
    check_float("noisy center", expected_center, state.center, FLOAT_TOL);

    /* CACH/prefix re-digitization remains part of the full warm-start path. */
    int all_correct = 1;
    for (int i = 0; i < DMR_RESAMPLE_SYMBOLS; i++) {
        const int expected_dibit[4] = {1, 0, 2, 3};
        if (state.dmr_payload_buf[i] != expected_dibit[i % 4]) {
            if (all_correct) {
                printf("FAIL: noisy full-path dibit mismatch at index %d: expected %d, got %d\n", i,
                       expected_dibit[i % 4], state.dmr_payload_buf[i]);
            }
            all_correct = 0;
        }
    }
    g_test_count++;
    if (!all_correct) {
        g_fail_count++;
    }

    free(state.dmr_payload_buf);
    dsd_symbol_history_free(&state);
    printf("test_full_resample_on_sync: passed\n\n");
}

int
main(void) {
    printf("DMR Resample-on-Sync Tests\n");
    printf("==========================\n\n");

    test_history_buffer_ops();
    test_history_buffer_wrap();
    test_cach_redigitize();
    test_full_resample_on_sync();

    printf("==========================\n");
    printf("Tests: %d, Failures: %d\n", g_test_count, g_fail_count);

    if (g_fail_count > 0) {
        printf("FAILED\n");
        return 1;
    }

    printf("PASSED: All resample-on-sync tests passed\n");
    return 0;
}
