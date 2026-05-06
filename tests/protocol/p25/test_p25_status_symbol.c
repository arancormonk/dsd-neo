// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for P25 status symbol accumulator and AFC gate logic.
 *
 * Validates the accumulator API (reset, add, classify) and the gate decision
 * logic including the disable-override path and counter tracking.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: reset zeroes accumulator state
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_accum_reset_zeroes_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        fprintf(stderr, "test_accum_reset_zeroes_state: alloc failed\n");
        return 1;
    }

    /* Seed with non-zero values to ensure reset actually clears them. */
    state->p25_ss_count = 5;
    state->p25_ss_classification = (uint8_t)P25_SS_CLASS_INFRASTRUCTURE;
    state->p25_ss_buf[0] = 0x03;

    p25_status_accum_reset(state);

    if (state->p25_ss_count != 0) {
        fprintf(stderr, "test_accum_reset_zeroes_state: expected count=0, got %u\n", state->p25_ss_count);
        free(state);
        return 1;
    }
    if (state->p25_ss_classification != (uint8_t)P25_SS_CLASS_UNKNOWN) {
        fprintf(stderr, "test_accum_reset_zeroes_state: expected classification=UNKNOWN(0), got %u\n",
                state->p25_ss_classification);
        free(state);
        return 1;
    }

    free(state);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: single add stores value and increments count
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_accum_add_single_value(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        fprintf(stderr, "test_accum_add_single_value: alloc failed\n");
        return 1;
    }

    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0x02);

    if (state->p25_ss_count != 1) {
        fprintf(stderr, "test_accum_add_single_value: expected count=1, got %u\n", state->p25_ss_count);
        free(state);
        return 1;
    }
    if (state->p25_ss_buf[0] != 0x02) {
        fprintf(stderr, "test_accum_add_single_value: expected buf[0]=0x02, got 0x%02x\n", state->p25_ss_buf[0]);
        free(state);
        return 1;
    }

    free(state);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: single 0x01 classifies as INFRASTRUCTURE
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_classify_single_01_is_infrastructure(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_classify_single_01_is_infrastructure: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0x01);
    p25_status_accum_classify(state, opts);

    if (state->p25_ss_classification != (uint8_t)P25_SS_CLASS_INFRASTRUCTURE) {
        fprintf(stderr, "test_classify_single_01_is_infrastructure: expected INFRASTRUCTURE(1), got %u\n",
                state->p25_ss_classification);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: single 0x03 classifies as INFRASTRUCTURE
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_classify_single_11_is_infrastructure(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_classify_single_11_is_infrastructure: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0x03);
    p25_status_accum_classify(state, opts);

    if (state->p25_ss_classification != (uint8_t)P25_SS_CLASS_INFRASTRUCTURE) {
        fprintf(stderr, "test_classify_single_11_is_infrastructure: expected INFRASTRUCTURE(1), got %u\n",
                state->p25_ss_classification);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: 11 zeros classifies as SUBSCRIBER
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_classify_all_00_is_subscriber(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_classify_all_00_is_subscriber: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);
    for (int i = 0; i < 11; i++) {
        p25_status_accum_add(state, 0x00);
    }
    p25_status_accum_classify(state, opts);

    if (state->p25_ss_classification != (uint8_t)P25_SS_CLASS_SUBSCRIBER) {
        fprintf(stderr, "test_classify_all_00_is_subscriber: expected SUBSCRIBER(2), got %u\n",
                state->p25_ss_classification);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: 11 twos classifies as UNKNOWN
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_classify_all_10_is_unknown(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_classify_all_10_is_unknown: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);
    for (int i = 0; i < 11; i++) {
        p25_status_accum_add(state, 0x02);
    }
    p25_status_accum_classify(state, opts);

    if (state->p25_ss_classification != (uint8_t)P25_SS_CLASS_UNKNOWN) {
        fprintf(stderr, "test_classify_all_10_is_unknown: expected UNKNOWN(0), got %u\n", state->p25_ss_classification);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: zero symbols classifies as UNKNOWN
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_classify_empty_is_unknown(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_classify_empty_is_unknown: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);
    /* Do not add any symbols — classify with empty accumulator. */
    p25_status_accum_classify(state, opts);

    if (state->p25_ss_classification != (uint8_t)P25_SS_CLASS_UNKNOWN) {
        fprintf(stderr, "test_classify_empty_is_unknown: expected UNKNOWN(0), got %u\n", state->p25_ss_classification);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: one 0x01 among 10 zeros classifies as INFRASTRUCTURE
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_classify_mixed_with_one_01_is_infra(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_classify_mixed_with_one_01_is_infra: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);
    /* 10 zeros followed by one 0x01 infrastructure indicator. */
    for (int i = 0; i < 10; i++) {
        p25_status_accum_add(state, 0x00);
    }
    p25_status_accum_add(state, 0x01);
    p25_status_accum_classify(state, opts);

    if (state->p25_ss_classification != (uint8_t)P25_SS_CLASS_INFRASTRUCTURE) {
        fprintf(stderr, "test_classify_mixed_with_one_01_is_infra: expected INFRASTRUCTURE(1), got %u\n",
                state->p25_ss_classification);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: gate_allow=1 when classification is INFRASTRUCTURE
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_gate_allow_when_infrastructure(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_gate_allow_when_infrastructure: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0x01);
    p25_status_accum_classify(state, opts);

    if (state->p25_afc_gate_allow != 1) {
        fprintf(stderr, "test_gate_allow_when_infrastructure: expected gate_allow=1, got %u\n",
                state->p25_afc_gate_allow);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: gate_allow=0 when classification is SUBSCRIBER
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_gate_suppress_when_subscriber(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_gate_suppress_when_subscriber: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0x00);
    p25_status_accum_classify(state, opts);

    if (state->p25_afc_gate_allow != 0) {
        fprintf(stderr, "test_gate_suppress_when_subscriber: expected gate_allow=0, got %u\n",
                state->p25_afc_gate_allow);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: gate_allow=0 when classification is UNKNOWN
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_gate_suppress_when_unknown(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_gate_suppress_when_unknown: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0x02);
    p25_status_accum_classify(state, opts);

    if (state->p25_afc_gate_allow != 0) {
        fprintf(stderr, "test_gate_suppress_when_unknown: expected gate_allow=0, got %u\n", state->p25_afc_gate_allow);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: gate_allow=1 regardless of classification when gating is disabled
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_gate_allow_when_disabled(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_gate_allow_when_disabled: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    /* Disable gating — gate should always be open. */
    opts->p25_afc_gate_disable = 1;

    /* Feed subscriber pattern (all zeros) which would normally suppress. */
    p25_status_accum_reset(state);
    for (int i = 0; i < 11; i++) {
        p25_status_accum_add(state, 0x00);
    }
    p25_status_accum_classify(state, opts);

    if (state->p25_afc_gate_allow != 1) {
        fprintf(stderr, "test_gate_allow_when_disabled: expected gate_allow=1 (disabled), got %u\n",
                state->p25_afc_gate_allow);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: allowed/suppressed counters increment correctly
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_counters_increment_correctly(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_counters_increment_correctly: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    /* First classify: infrastructure → allowed_count should increment. */
    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0x01);
    p25_status_accum_classify(state, opts);

    if (state->p25_afc_allowed_count != 1) {
        fprintf(stderr, "test_counters_increment_correctly: expected allowed=1, got %u\n",
                state->p25_afc_allowed_count);
        free(state);
        free(opts);
        return 1;
    }
    if (state->p25_afc_suppressed_count != 0) {
        fprintf(stderr, "test_counters_increment_correctly: expected suppressed=0, got %u\n",
                state->p25_afc_suppressed_count);
        free(state);
        free(opts);
        return 1;
    }

    /* Second classify: subscriber → suppressed_count should increment. */
    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0x00);
    p25_status_accum_classify(state, opts);

    if (state->p25_afc_allowed_count != 1) {
        fprintf(stderr, "test_counters_increment_correctly: expected allowed=1 (unchanged), got %u\n",
                state->p25_afc_allowed_count);
        free(state);
        free(opts);
        return 1;
    }
    if (state->p25_afc_suppressed_count != 1) {
        fprintf(stderr, "test_counters_increment_correctly: expected suppressed=1, got %u\n",
                state->p25_afc_suppressed_count);
        free(state);
        free(opts);
        return 1;
    }

    /* Third classify: unknown → suppressed_count should increment again. */
    p25_status_accum_reset(state);
    p25_status_accum_add(state, 0x02);
    p25_status_accum_classify(state, opts);

    if (state->p25_afc_allowed_count != 1) {
        fprintf(stderr, "test_counters_increment_correctly: expected allowed=1 (still), got %u\n",
                state->p25_afc_allowed_count);
        free(state);
        free(opts);
        return 1;
    }
    if (state->p25_afc_suppressed_count != 2) {
        fprintf(stderr, "test_counters_increment_correctly: expected suppressed=2, got %u\n",
                state->p25_afc_suppressed_count);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: fresh calloc'd state has UNKNOWN classification and zero counters
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_initial_state_is_unknown_zero_counts(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        fprintf(stderr, "test_initial_state_is_unknown_zero_counts: alloc failed\n");
        return 1;
    }

    /* calloc zero-initializes, so all fields should be zero/UNKNOWN. */
    if (state->p25_ss_classification != (uint8_t)P25_SS_CLASS_UNKNOWN) {
        fprintf(stderr, "test_initial_state_is_unknown_zero_counts: expected classification=UNKNOWN(0), got %u\n",
                state->p25_ss_classification);
        free(state);
        return 1;
    }
    if (state->p25_ss_count != 0) {
        fprintf(stderr, "test_initial_state_is_unknown_zero_counts: expected count=0, got %u\n", state->p25_ss_count);
        free(state);
        return 1;
    }
    if (state->p25_afc_gate_allow != 0) {
        fprintf(stderr, "test_initial_state_is_unknown_zero_counts: expected gate_allow=0, got %u\n",
                state->p25_afc_gate_allow);
        free(state);
        return 1;
    }
    if (state->p25_afc_allowed_count != 0) {
        fprintf(stderr, "test_initial_state_is_unknown_zero_counts: expected allowed_count=0, got %u\n",
                state->p25_afc_allowed_count);
        free(state);
        return 1;
    }
    if (state->p25_afc_suppressed_count != 0) {
        fprintf(stderr, "test_initial_state_is_unknown_zero_counts: expected suppressed_count=0, got %u\n",
                state->p25_afc_suppressed_count);
        free(state);
        return 1;
    }

    free(state);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Test: adding >12 symbols does not crash or corrupt state
 * ───────────────────────────────────────────────────────────────────────────── */
static int
test_overflow_ignored_gracefully(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        fprintf(stderr, "test_overflow_ignored_gracefully: alloc failed\n");
        free(state);
        free(opts);
        return 1;
    }

    p25_status_accum_reset(state);

    /* Add 15 symbols (exceeds P25_STATUS_ACCUM_MAX of 12). */
    for (int i = 0; i < 15; i++) {
        p25_status_accum_add(state, 0x01);
    }

    /* Count should be capped at P25_STATUS_ACCUM_MAX. */
    if (state->p25_ss_count != P25_STATUS_ACCUM_MAX) {
        fprintf(stderr, "test_overflow_ignored_gracefully: expected count=%d, got %u\n", P25_STATUS_ACCUM_MAX,
                state->p25_ss_count);
        free(state);
        free(opts);
        return 1;
    }

    /* Classification should still work correctly with the stored symbols. */
    p25_status_accum_classify(state, opts);

    if (state->p25_ss_classification != (uint8_t)P25_SS_CLASS_INFRASTRUCTURE) {
        fprintf(stderr, "test_overflow_ignored_gracefully: expected INFRASTRUCTURE(1), got %u\n",
                state->p25_ss_classification);
        free(state);
        free(opts);
        return 1;
    }

    free(state);
    free(opts);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Main — aggregate all test results
 * ───────────────────────────────────────────────────────────────────────────── */
int
main(void) {
    int rc = 0;

    rc |= test_accum_reset_zeroes_state();
    rc |= test_accum_add_single_value();
    rc |= test_classify_single_01_is_infrastructure();
    rc |= test_classify_single_11_is_infrastructure();
    rc |= test_classify_all_00_is_subscriber();
    rc |= test_classify_all_10_is_unknown();
    rc |= test_classify_empty_is_unknown();
    rc |= test_classify_mixed_with_one_01_is_infra();
    rc |= test_gate_allow_when_infrastructure();
    rc |= test_gate_suppress_when_subscriber();
    rc |= test_gate_suppress_when_unknown();
    rc |= test_gate_allow_when_disabled();
    rc |= test_counters_increment_correctly();
    rc |= test_initial_state_is_unknown_zero_counts();
    rc |= test_overflow_ignored_gracefully();

    return rc;
}
