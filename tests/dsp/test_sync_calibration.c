// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for generic sync calibration module.
 *
 * Tests the protocol-agnostic symbol history and warm-start APIs
 * provided by sync_calibration.h.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/runtime/config.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "test_env.h"

#define FLOAT_TOL 0.01f

static int g_test_count = 0;
static int g_fail_count = 0;

static void
check_float(const char* name, float expected, float actual, float tol) {
    g_test_count++;
    if (fabsf(expected - actual) > tol) {
        printf("FAIL: %s: expected %.4f, got %.4f\n", name, (double)expected, (double)actual);
        g_fail_count++;
    }
}

static void
check_int(const char* name, int expected, int actual) {
    g_test_count++;
    if (expected != actual) {
        printf("FAIL: %s: expected %d, got %d\n", name, expected, actual);
        g_fail_count++;
    }
}

static void
attach_history_fixture(struct dsd_state* state, float* history, int symbols) {
    DSD_MEMSET(history, 0, sizeof(*history) * (size_t)symbols);
    state->symbol_history = history;
    state->symbol_history_size = symbols;
    state->symbol_history_head = 0;
    state->symbol_history_count = 0;
}

/**
 * @brief Test symbol history basic operations.
 */
static void
test_history_basic_ops(void) {
    printf("=== test_history_basic_ops ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    attach_history_fixture(&state, history, 64);
    check_int("initial count", 0, dsd_symbol_history_count(&state));

    /* Push some symbols */
    dsd_symbol_history_push(&state, 1.0f);
    dsd_symbol_history_push(&state, 2.0f);
    dsd_symbol_history_push(&state, 3.0f);
    check_int("count after push", 3, dsd_symbol_history_count(&state));

    attach_history_fixture(&state, history, 2);
    check_int("fixture count", 0, dsd_symbol_history_count(&state));
    check_int("fixture size", 2, state.symbol_history_size);

    dsd_symbol_history_push(&state, 4.0f);
    dsd_symbol_history_push(&state, 5.0f);

    /* Get symbols back */
    check_float("get_back(0)", 5.0f, dsd_symbol_history_get_back(&state, 0), FLOAT_TOL);
    check_float("get_back(1)", 4.0f, dsd_symbol_history_get_back(&state, 1), FLOAT_TOL);

    attach_history_fixture(&state, history, 2);
    check_int("count after reset", 0, dsd_symbol_history_count(&state));

    printf("test_history_basic_ops: passed\n\n");
}

/**
 * @brief Test history buffer wrap-around.
 */
static void
test_history_wraparound(void) {
    printf("=== test_history_wraparound ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[4];

    attach_history_fixture(&state, history, 4);

    /* Push 6 symbols to force wrap */
    for (int i = 1; i <= 6; i++) {
        dsd_symbol_history_push(&state, (float)i);
    }

    /* Count should be capped at buffer size */
    check_int("count capped", 4, dsd_symbol_history_count(&state));

    /* Most recent 4 symbols should be 6, 5, 4, 3 */
    check_float("get_back(0)", 6.0f, dsd_symbol_history_get_back(&state, 0), FLOAT_TOL);
    check_float("get_back(1)", 5.0f, dsd_symbol_history_get_back(&state, 1), FLOAT_TOL);
    check_float("get_back(2)", 4.0f, dsd_symbol_history_get_back(&state, 2), FLOAT_TOL);
    check_float("get_back(3)", 3.0f, dsd_symbol_history_get_back(&state, 3), FLOAT_TOL);

    /* Out of range should return 0 */
    check_float("get_back(4) oob", 0.0f, dsd_symbol_history_get_back(&state, 4), FLOAT_TOL);

    printf("test_history_wraparound: passed\n\n");
}

/**
 * @brief Test warm-start with ideal outer-only sync pattern.
 */
static void
test_warm_start_ideal(void) {
    printf("=== test_warm_start_ideal ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 64;

    attach_history_fixture(&state, history, 64);

    /* Push a 24-symbol outer-only sync pattern (12 x +3, 12 x -3) */
    float pattern[] = {+3.0f, -3.0f, +3.0f, +3.0f, +3.0f, +3.0f, -3.0f, -3.0f, +3.0f, -3.0f, +3.0f, +3.0f,
                       -3.0f, +3.0f, +3.0f, -3.0f, +3.0f, -3.0f, +3.0f, +3.0f, -3.0f, +3.0f, -3.0f, +3.0f};
    for (int i = 0; i < 24; i++) {
        dsd_symbol_history_push(&state, pattern[i]);
    }

    /* Apply warm-start */
    dsd_warm_start_result_t result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24);
    check_int("warm_start result", DSD_WARM_START_OK, result);

    /* Verify thresholds */
    check_float("max", 3.0f, state.max, FLOAT_TOL);
    check_float("min", -3.0f, state.min, FLOAT_TOL);
    check_float("center", 0.0f, state.center, FLOAT_TOL);

    /* Mid thresholds: 62.5% from center toward extremes */
    check_float("umid", 1.875f, state.umid, FLOAT_TOL);
    check_float("lmid", -1.875f, state.lmid, FLOAT_TOL);

    printf("test_warm_start_ideal: passed\n\n");
}

/**
 * @brief A re-seeded slicer must not leave the crossing measured against the old one.
 *
 * state->jitter holds the index of the first zero crossing inside a symbol, and
 * symbol_adjust_timing_index() slips the symbol grid by a sample on it. It is
 * measured against center/maxref/minref -- exactly what warm-start replaces --
 * and it is assigned only while negative, so a value left here survives the whole
 * frame this sync opens (the slip does not run inside a frame) and is then handed
 * to the first symbol of the next sync search. That symbol would move the grid on
 * a crossing measured against a calibration that no longer exists (issue #404).
 */
static void
test_warm_start_drops_stale_crossing(void) {
    printf("=== test_warm_start_drops_stale_crossing ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 64;

    attach_history_fixture(&state, history, 64);
    for (int i = 0; i < 24; i++) {
        dsd_symbol_history_push(&state, (i % 2 == 0) ? +3.0f : -3.0f);
    }

    /* A crossing latched against the thresholds the warm-start is about to
       replace, in the band that slips the grid. */
    state.jitter = 9;

    dsd_warm_start_result_t result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24);
    check_int("warm_start result", DSD_WARM_START_OK, result);
    check_int("stale crossing dropped", -1, state.jitter);

    /* A warm-start that does not re-seed anything must leave the crossing alone:
       it is still measured against the thresholds in force. */
    DSD_MEMSET(&state, 0, sizeof(state));
    attach_history_fixture(&state, history, 64);
    for (int i = 0; i < 4; i++) {
        dsd_symbol_history_push(&state, +3.0f);
    }
    state.jitter = 9;
    result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24);
    check_int("no-history result", DSD_WARM_START_NO_HISTORY, result);
    check_int("crossing kept when nothing was re-seeded", 9, state.jitter);

    printf("test_warm_start_drops_stale_crossing: passed\n\n");
}

/**
 * @brief Test warm-start with DC offset.
 */
static void
test_warm_start_dc_offset(void) {
    printf("=== test_warm_start_dc_offset ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 64;

    attach_history_fixture(&state, history, 64);

    /* Sync pattern with +0.5 DC offset */
    float dc = 0.5f;
    float pattern[] = {+3.0f + dc, -3.0f + dc, +3.0f + dc, +3.0f + dc, +3.0f + dc, +3.0f + dc, -3.0f + dc, -3.0f + dc,
                       +3.0f + dc, -3.0f + dc, +3.0f + dc, +3.0f + dc, -3.0f + dc, +3.0f + dc, +3.0f + dc, -3.0f + dc,
                       +3.0f + dc, -3.0f + dc, +3.0f + dc, +3.0f + dc, -3.0f + dc, +3.0f + dc, -3.0f + dc, +3.0f + dc};
    for (int i = 0; i < 24; i++) {
        dsd_symbol_history_push(&state, pattern[i]);
    }

    dsd_warm_start_result_t result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24);
    check_int("warm_start result", DSD_WARM_START_OK, result);

    check_float("max", 3.5f, state.max, FLOAT_TOL);
    check_float("min", -2.5f, state.min, FLOAT_TOL);
    check_float("center", 0.5f, state.center, FLOAT_TOL);

    printf("test_warm_start_dc_offset: passed\n\n");
}

/**
 * @brief Test CQPSK-safe "center-only" warm-start.
 *
 * This should update only state.center (DC bias estimate) and leave other
 * thresholds unchanged.
 */
static void
test_center_only_warm_start(void) {
    printf("=== test_center_only_warm_start ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    attach_history_fixture(&state, history, 64);

    /* Seed state with sentinel values to ensure only center changes. */
    state.center = 123.0f;
    state.max = 9.0f;
    state.min = -9.0f;
    state.umid = 7.0f;
    state.lmid = -7.0f;
    state.maxref = 8.0f;
    state.minref = -8.0f;

    /* Unbalanced outer-only sync (+3/-3) with DC offset (matches P25p1 characteristic imbalance). */
    float dc = 0.5f;
    for (int i = 0; i < 11; i++) {
        dsd_symbol_history_push(&state, 3.0f + dc);
    }
    for (int i = 0; i < 13; i++) {
        dsd_symbol_history_push(&state, -3.0f + dc);
    }

    dsd_warm_start_result_t result = dsd_sync_warm_start_center_outer_only(&state, 24);
    check_int("center_only result", DSD_WARM_START_OK, result);
    check_float("center", dc, state.center, FLOAT_TOL);

    /* Verify other thresholds are untouched. */
    check_float("max unchanged", 9.0f, state.max, FLOAT_TOL);
    check_float("min unchanged", -9.0f, state.min, FLOAT_TOL);
    check_float("umid unchanged", 7.0f, state.umid, FLOAT_TOL);
    check_float("lmid unchanged", -7.0f, state.lmid, FLOAT_TOL);
    check_float("maxref unchanged", 8.0f, state.maxref, FLOAT_TOL);
    check_float("minref unchanged", -8.0f, state.minref, FLOAT_TOL);

    printf("test_center_only_warm_start: passed\n\n");
}

/**
 * @brief Test center-only warm-start remains robust under a large DC bias.
 */
static void
test_center_only_large_bias(void) {
    printf("=== test_center_only_large_bias ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    attach_history_fixture(&state, history, 64);

    /* DC bias large enough that both clusters are positive. */
    float dc = 10.0f;
    for (int i = 0; i < 10; i++) {
        dsd_symbol_history_push(&state, 3.0f + dc);
        dsd_symbol_history_push(&state, -3.0f + dc);
    }

    dsd_warm_start_result_t result = dsd_sync_warm_start_center_outer_only(&state, 20);
    check_int("center_only result", DSD_WARM_START_OK, result);
    check_float("center", dc, state.center, FLOAT_TOL);

    printf("test_center_only_large_bias: passed\n\n");
}

static void
test_center_only_failure_modes(void) {
    printf("=== test_center_only_failure_modes ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[8];

    dsd_warm_start_result_t result = dsd_sync_warm_start_center_outer_only(NULL, 24);
    check_int("center null state", DSD_WARM_START_NULL_STATE, result);

    result = dsd_sync_warm_start_center_outer_only(&state, 1);
    check_int("center invalid length", DSD_WARM_START_DEGENERATE, result);

    result = dsd_sync_warm_start_center_outer_only(&state, 4);
    check_int("center no history", DSD_WARM_START_NO_HISTORY, result);

    attach_history_fixture(&state, history, 8);
    dsd_symbol_history_push(&state, -3.0f);
    dsd_symbol_history_push(&state, -3.0f);
    dsd_symbol_history_push(&state, 3.0f);
    result = dsd_sync_warm_start_center_outer_only(&state, 3);
    check_int("center singleton high cluster", DSD_WARM_START_DEGENERATE, result);

    attach_history_fixture(&state, history, 8);
    dsd_symbol_history_push(&state, -0.2f);
    dsd_symbol_history_push(&state, -0.2f);
    dsd_symbol_history_push(&state, 0.2f);
    dsd_symbol_history_push(&state, 0.2f);
    result = dsd_sync_warm_start_center_outer_only(&state, 4);
    check_int("center small span", DSD_WARM_START_DEGENERATE, result);

    printf("test_center_only_failure_modes: passed\n\n");
}

static void
test_center_only_large_sync_uses_heap_sorted_copy(void) {
    printf("=== test_center_only_large_sync_uses_heap_sorted_copy ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[80];

    attach_history_fixture(&state, history, 80);
    for (int i = 0; i < 35; i++) {
        dsd_symbol_history_push(&state, -2.0f);
        dsd_symbol_history_push(&state, 4.0f);
    }

    dsd_warm_start_result_t result = dsd_sync_warm_start_center_outer_only(&state, 70);
    check_int("center large sync result", DSD_WARM_START_OK, result);
    check_float("center large sync", 1.0f, state.center, FLOAT_TOL);

    printf("test_center_only_large_sync_uses_heap_sorted_copy: passed\n\n");
}

/**
 * @brief Test warm-start returns appropriate error when history is insufficient.
 */
static void
test_warm_start_insufficient_history(void) {
    printf("=== test_warm_start_insufficient_history ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 64;

    attach_history_fixture(&state, history, 64);

    /* Push only 10 symbols but request 24 */
    for (int i = 0; i < 10; i++) {
        dsd_symbol_history_push(&state, 3.0f);
    }

    dsd_warm_start_result_t result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24);
    check_int("warm_start result", DSD_WARM_START_NO_HISTORY, result);

    printf("test_warm_start_insufficient_history: passed\n\n");
}

/**
 * @brief Test warm-start returns appropriate error for degenerate signal.
 */
static void
test_warm_start_degenerate(void) {
    printf("=== test_warm_start_degenerate ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 64;

    attach_history_fixture(&state, history, 64);

    /* Push all positive symbols (no negative) */
    for (int i = 0; i < 24; i++) {
        dsd_symbol_history_push(&state, 3.0f);
    }

    dsd_warm_start_result_t result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24);
    check_int("warm_start result (all pos)", DSD_WARM_START_DEGENERATE, result);

    /* Now test very small span */
    attach_history_fixture(&state, history, 64);
    for (int i = 0; i < 12; i++) {
        dsd_symbol_history_push(&state, 0.3f);
        dsd_symbol_history_push(&state, -0.3f);
    }

    result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24);
    check_int("warm_start result (small span)", DSD_WARM_START_DEGENERATE, result);

    printf("test_warm_start_degenerate: passed\n\n");
}

/**
 * @brief Test warm-start with different sync lengths.
 *
 * Verifies the generic API works with various protocol sync lengths.
 */
static void
test_warm_start_various_sync_lengths(void) {
    printf("=== test_warm_start_various_sync_lengths ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 64;

    /* Test with sync lengths from different protocols */
    int sync_lengths[] = {8, 10, 12, 20, 24}; /* M17, NXDN, dPMR, YSF, DMR/P25 */
    int num_lengths = sizeof(sync_lengths) / sizeof(sync_lengths[0]);

    for (int t = 0; t < num_lengths; t++) {
        int sync_len = sync_lengths[t];
        attach_history_fixture(&state, history, 64);

        /* Push alternating +3/-3 pattern */
        for (int i = 0; i < sync_len; i++) {
            float sym = (i % 2 == 0) ? 3.0f : -3.0f;
            dsd_symbol_history_push(&state, sym);
        }

        dsd_warm_start_result_t result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, sync_len);
        char buf[64];
        DSD_SNPRINTF(buf, sizeof(buf), "sync_len=%d result", sync_len);
        check_int(buf, DSD_WARM_START_OK, result);

        DSD_SNPRINTF(buf, sizeof(buf), "sync_len=%d max", sync_len);
        check_float(buf, 3.0f, state.max, FLOAT_TOL);

        DSD_SNPRINTF(buf, sizeof(buf), "sync_len=%d min", sync_len);
        check_float(buf, -3.0f, state.min, FLOAT_TOL);
    }

    printf("test_warm_start_various_sync_lengths: passed\n\n");
}

/**
 * @brief Test null pointer handling.
 */
static void
test_null_handling(void) {
    printf("=== test_null_handling ===\n");

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 64;

    /* NULL state */
    dsd_warm_start_result_t result = dsd_sync_warm_start_thresholds_outer_only(&opts, NULL, 24);
    check_int("null state", DSD_WARM_START_NULL_STATE, result);

    /* NULL state for history functions should not crash */
    dsd_symbol_history_push(NULL, 3.0f);
    check_float("push to null", 0.0f, dsd_symbol_history_get_back(NULL, 0), FLOAT_TOL);
    check_int("count from null", 0, dsd_symbol_history_count(NULL));

    printf("test_null_handling: passed\n\n");
}

/**
 * @brief Test buffer pre-fill during warm-start.
 */
static void
test_buffer_prefill(void) {
    printf("=== test_buffer_prefill ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 32;

    attach_history_fixture(&state, history, 64);

    /* Push sync pattern */
    for (int i = 0; i < 24; i++) {
        float sym = (i % 2 == 0) ? 3.0f : -3.0f;
        dsd_symbol_history_push(&state, sym);
    }

    dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24);

    /* Check buffers are pre-filled */
    int all_filled = 1;
    for (int i = 0; i < opts.msize; i++) {
        if (fabsf(state.maxbuf[i] - 3.0f) > FLOAT_TOL) {
            all_filled = 0;
            printf("FAIL: maxbuf[%d] = %.4f\n", i, (double)state.maxbuf[i]);
            break;
        }
        if (fabsf(state.minbuf[i] - (-3.0f)) > FLOAT_TOL) {
            all_filled = 0;
            printf("FAIL: minbuf[%d] = %.4f\n", i, (double)state.minbuf[i]);
            break;
        }
    }

    g_test_count++;
    if (!all_filled) {
        g_fail_count++;
    }

    printf("test_buffer_prefill: passed\n\n");
}

static void
test_warm_start_prefill_clamps_to_internal_buffer(void) {
    printf("=== test_warm_start_prefill_clamps_to_internal_buffer ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[64];

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 2048;

    attach_history_fixture(&state, history, 64);
    for (int i = 0; i < 24; i++) {
        dsd_symbol_history_push(&state, (i % 2 == 0) ? 3.0f : -3.0f);
    }

    dsd_warm_start_result_t result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24);
    check_int("prefill clamp result", DSD_WARM_START_OK, result);
    check_float("maxbuf last clamped", 3.0f, state.maxbuf[1023], FLOAT_TOL);
    check_float("minbuf last clamped", -3.0f, state.minbuf[1023], FLOAT_TOL);

    printf("test_warm_start_prefill_clamps_to_internal_buffer: passed\n\n");
}

/* The level a sync-pattern dibit character stands for ('0' +1, '1' +3, '2' -1, '3' -3). */
static float
level_of_dibit_char(char c) {
    static const float levels[4] = {1.0f, 3.0f, -1.0f, -3.0f};
    return levels[c - '0'];
}

/* The sync word's own levels, scaled and offset as a receiver would read them, pushed oldest first. */
static void
push_word_levels(struct dsd_state* state, const char* dibits, float scale, float offset) {
    for (const char* c = dibits; *c != '\0'; c++) {
        dsd_symbol_history_push(state, (level_of_dibit_char(*c) * scale) + offset);
    }
}

/* NXDN's FSW and YSF's frame sync carry inner symbols. Read as outer-only, NXDN's put the outer rails at 2.6 of 3
   and YSF's put the centre a quarter of an outer level off; fitted to the word's own levels, the thresholds land
   on the true rails, at either polarity (issue #834). */
static void
test_warm_start_from_dibits_fits_inner_symbols(void) {
    printf("=== test_warm_start_from_dibits_fits_inner_symbols ===\n");

    static struct dsd_state state;
    static float history[64];
    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.msize = 16;

    const struct {
        const char* name;
        const char* dibits;
    } words[] = {
        {"NXDN FSW", NXDN_FSW_DIBITS},
        {"inverted NXDN FSW", INV_NXDN_FSW_DIBITS},
        {"YSF frame sync", FUSION_SYNC_DIBITS},
        {"inverted YSF frame sync", INV_FUSION_SYNC_DIBITS},
    };

    for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        DSD_MEMSET(&state, 0, sizeof(state));
        attach_history_fixture(&state, history, 64);
        const int len = (int)strlen(words[w].dibits);
        push_word_levels(&state, words[w].dibits, 1000.0f, 250.0f);
        state.jitter = 7;

        check_int(words[w].name, DSD_WARM_START_OK,
                  dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, words[w].dibits, len));
        check_float("max", 3250.0f, state.max, 1.0f);
        check_float("min", -2750.0f, state.min, 1.0f);
        check_float("center", 250.0f, state.center, 1.0f);
        check_float("umid", 250.0f + (3000.0f * DSD_WARM_START_MID_FRACTION), state.umid, 1.0f);
        check_float("lmid", 250.0f - (3000.0f * DSD_WARM_START_MID_FRACTION), state.lmid, 1.0f);
        check_float("maxref", 3250.0f * 0.80f, state.maxref, 1.0f);
        check_float("minref", -2750.0f * 0.80f, state.minref, 1.0f);
        check_int("stale crossing dropped", -1, state.jitter);
        check_float("maxbuf prefilled", 3250.0f, state.maxbuf[opts.msize - 1], 1.0f);
        check_float("minbuf prefilled", -2750.0f, state.minbuf[opts.msize - 1], 1.0f);
    }

    /* What the outer-only start makes of the same NXDN FSW: the rails a fifth of an inner step short. */
    DSD_MEMSET(&state, 0, sizeof(state));
    attach_history_fixture(&state, history, 64);
    push_word_levels(&state, NXDN_FSW_DIBITS, 1000.0f, 0.0f);
    check_int("outer-only on the FSW", DSD_WARM_START_OK, dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 10));
    check_float("outer-only FSW max", 2600.0f, state.max, 1.0f);

    printf("test_warm_start_from_dibits_fits_inner_symbols: passed\n\n");
}

/* On a word of outer symbols alone the fit is the outer-only start's two means. */
static void
test_warm_start_from_dibits_matches_outer_only_on_outer_words(void) {
    printf("=== test_warm_start_from_dibits_matches_outer_only_on_outer_words ===\n");

    static struct dsd_state state;
    static float history[64];
    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));

    static const float jitter[24] = {0.11f,  -0.07f, 0.03f,  0.19f,  -0.13f, 0.05f,  -0.02f, 0.17f,
                                     -0.09f, 0.01f,  0.08f,  -0.15f, 0.12f,  -0.04f, 0.06f,  -0.18f,
                                     0.02f,  0.14f,  -0.11f, 0.09f,  -0.06f, 0.04f,  -0.01f, 0.16f};
    float outer_only_max = 0.0f;
    float outer_only_min = 0.0f;
    for (int pass = 0; pass < 2; pass++) {
        DSD_MEMSET(&state, 0, sizeof(state));
        attach_history_fixture(&state, history, 64);
        for (int i = 0; i < 24; i++) {
            dsd_symbol_history_push(&state, (level_of_dibit_char(DMR_BS_VOICE_SYNC[i]) * 0.9f) + jitter[i]);
        }
        if (pass == 0) {
            check_int("outer only", DSD_WARM_START_OK, dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 24));
            outer_only_max = state.max;
            outer_only_min = state.min;
        } else {
            check_int("from dibits", DSD_WARM_START_OK,
                      dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, DMR_BS_VOICE_SYNC, 24));
            check_float("max as outer only", outer_only_max, state.max, 1e-4f);
            check_float("min as outer only", outer_only_min, state.min, 1e-4f);
        }
    }

    printf("test_warm_start_from_dibits_matches_outer_only_on_outer_words: passed\n\n");
}

/* What the fit refuses, leaving the thresholds it found. */
static void
test_warm_start_from_dibits_refuses(void) {
    printf("=== test_warm_start_from_dibits_refuses ===\n");

    static struct dsd_state state;
    static float history[64];
    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));

    DSD_MEMSET(&state, 0, sizeof(state));
    attach_history_fixture(&state, history, 64);
    push_word_levels(&state, NXDN_FSW_DIBITS, 1000.0f, 0.0f);
    state.max = 7.0f;
    state.min = -7.0f;
    check_int("one level", DSD_WARM_START_DEGENERATE,
              dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, "1111111111", 10));
    check_int("not a dibit", DSD_WARM_START_DEGENERATE,
              dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, "30313x1121", 10));
    /* The word names the polarity: an FSW read at the other one fits outer levels the wrong way round. */
    check_int("other polarity", DSD_WARM_START_DEGENERATE,
              dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, INV_NXDN_FSW_DIBITS, 10));
    check_int("too short", DSD_WARM_START_DEGENERATE,
              dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, NXDN_FSW_DIBITS, 1));
    check_int("no word", DSD_WARM_START_DEGENERATE,
              dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, NULL, 10));
    check_int("more than the history holds", DSD_WARM_START_NO_HISTORY,
              dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, FUSION_SYNC_DIBITS, 20));
    check_int("no state", DSD_WARM_START_NULL_STATE,
              dsd_sync_warm_start_thresholds_from_dibits(&opts, NULL, NXDN_FSW_DIBITS, 10));
    check_float("max untouched", 7.0f, state.max, FLOAT_TOL);
    check_float("min untouched", -7.0f, state.min, FLOAT_TOL);

    /* Weaker than the outer-only start's floor. */
    DSD_MEMSET(&state, 0, sizeof(state));
    attach_history_fixture(&state, history, 64);
    push_word_levels(&state, NXDN_FSW_DIBITS, 0.1f, 0.0f);
    check_int("span under the floor", DSD_WARM_START_DEGENERATE,
              dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, NXDN_FSW_DIBITS, 10));

    /* A NaN or an infinity among the symbols (a float symbol replay passes what the file holds) fits nothing, and the
       thresholds stay as they were. The values come from their bit patterns: a NAN or INFINITY literal is a build
       error under Clang's fast-math (-Wnan-infinity-disabled), and the bits reach the fast-math guard unchanged. */
    static const uint32_t bad_bits[2] = {0x7FC00000u /* NaN */, 0x7F800000u /* +Inf */};
    for (int b = 0; b < 2; b++) {
        DSD_MEMSET(&state, 0, sizeof(state));
        attach_history_fixture(&state, history, 64);
        push_word_levels(&state, NXDN_FSW_DIBITS, 1000.0f, 0.0f);
        float bad_value = 0.0f;
        DSD_MEMCPY(&bad_value, &bad_bits[b], sizeof(bad_value));
        history[(state.symbol_history_head + 63) % 64] = bad_value; /* the word's last symbol */
        state.max = 7.0f;
        state.min = -7.0f;
        check_int(b == 0 ? "NaN symbol" : "infinite symbol", DSD_WARM_START_DEGENERATE,
                  dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, NXDN_FSW_DIBITS, 10));
        check_float("max untouched by a non-finite symbol", 7.0f, state.max, FLOAT_TOL);
        check_float("min untouched by a non-finite symbol", -7.0f, state.min, FLOAT_TOL);
    }

    printf("test_warm_start_from_dibits_refuses: passed\n\n");
}

static void
test_warm_start_disabled_env(void) {
    printf("=== test_warm_start_disabled_env ===\n");

    static struct dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    static float history[4];
    attach_history_fixture(&state, history, 4);

    static struct dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    for (int i = 0; i < 4; i++) {
        dsd_symbol_history_push(&state, (i < 2) ? -3.0f : 3.0f);
    }

    (void)dsd_test_setenv("DSD_NEO_SYNC_WARMSTART", "0", 1);
    dsd_neo_config_init();

    dsd_warm_start_result_t result = dsd_sync_warm_start_thresholds_outer_only(&opts, &state, 4);
    check_int("thresholds disabled", DSD_WARM_START_DISABLED, result);
    result = dsd_sync_warm_start_center_outer_only(&state, 4);
    check_int("center disabled", DSD_WARM_START_DISABLED, result);
    result = dsd_sync_warm_start_thresholds_from_dibits(&opts, &state, "3311", 4);
    check_int("from dibits disabled", DSD_WARM_START_DISABLED, result);

    (void)dsd_test_unsetenv("DSD_NEO_SYNC_WARMSTART");
    dsd_neo_config_init();
    printf("test_warm_start_disabled_env: passed\n\n");
}

int
main(void) {
    printf("Generic Sync Calibration Module Tests\n");
    printf("======================================\n\n");

    test_history_basic_ops();
    test_history_wraparound();
    test_warm_start_ideal();
    test_warm_start_drops_stale_crossing();
    test_warm_start_dc_offset();
    test_center_only_warm_start();
    test_center_only_large_bias();
    test_center_only_failure_modes();
    test_center_only_large_sync_uses_heap_sorted_copy();
    test_warm_start_insufficient_history();
    test_warm_start_degenerate();
    test_warm_start_various_sync_lengths();
    test_null_handling();
    test_buffer_prefill();
    test_warm_start_prefill_clamps_to_internal_buffer();
    test_warm_start_from_dibits_fits_inner_symbols();
    test_warm_start_from_dibits_matches_outer_only_on_outer_words();
    test_warm_start_from_dibits_refuses();
    test_warm_start_disabled_env();
    printf("======================================\n");
    printf("Tests: %d, Failures: %d\n", g_test_count, g_fail_count);

    if (g_fail_count > 0) {
        printf("FAILED\n");
        return 1;
    }

    printf("PASSED: All generic sync calibration tests passed\n");
    return 0;
}
