// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Unit tests for P25 status symbol accumulator and AFC gate logic.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

typedef struct {
    dsd_state* state;
    dsd_opts* opts;
} test_ctx;

static test_ctx
make_ctx(const char* test_name) {
    test_ctx ctx = {(dsd_state*)calloc(1, sizeof(*ctx.state)), (dsd_opts*)calloc(1, sizeof(*ctx.opts))};
    if (!ctx.state || !ctx.opts) {
        fprintf(stderr, "%s: alloc failed\n", test_name);
        free(ctx.state);
        free(ctx.opts);
        ctx.state = NULL;
        ctx.opts = NULL;
    }
    return ctx;
}

static void
free_ctx(test_ctx* ctx) {
    free(ctx->state);
    free(ctx->opts);
    ctx->state = NULL;
    ctx->opts = NULL;
}

static int
expect_u32(const char* test_name, const char* field_name, unsigned int actual, unsigned int expected) {
    if (actual != expected) {
        fprintf(stderr, "%s: expected %s=%u, got %u\n", test_name, field_name, expected, actual);
        return 1;
    }
    return 0;
}

static int
expect_class(const char* test_name, const dsd_state* state, p25_ss_classification_t expected) {
    return expect_u32(test_name, "classification", state->p25_ss_classification, (unsigned int)expected);
}

static int
test_accum_reset_zeroes_state(void) {
    const char* test = "test_accum_reset_zeroes_state";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    ctx.state->p25_ss_count = 5;
    ctx.state->p25_ss_classification = (uint8_t)P25_SS_CLASS_INFRASTRUCTURE;
    ctx.state->p25_ss_buf[0] = 0x03;

    p25_status_accum_reset(ctx.state);

    int rc = 0;
    rc |= expect_u32(test, "count", ctx.state->p25_ss_count, 0);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_UNKNOWN);
    rc |= expect_u32(test, "frame_active", ctx.state->p25_ss_frame_active, 1);

    free_ctx(&ctx);
    return rc;
}

static int
test_ensure_started_preserves_dispatch_status(void) {
    const char* test = "test_ensure_started_preserves_dispatch_status";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    p25_status_accum_reset(ctx.state);
    p25_status_accum_add(ctx.state, 0x01);
    p25_status_accum_ensure_started(ctx.state);

    int rc = 0;
    rc |= expect_u32(test, "count after ensure", ctx.state->p25_ss_count, 1);
    rc |= expect_u32(test, "first status", ctx.state->p25_ss_buf[0], 0x01);

    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_INFRASTRUCTURE);

    p25_status_accum_ensure_started(ctx.state);
    rc |= expect_u32(test, "count after completed ensure", ctx.state->p25_ss_count, 0);
    rc |= expect_u32(test, "frame_active after completed ensure", ctx.state->p25_ss_frame_active, 1);

    free_ctx(&ctx);
    return rc;
}

static int
test_accum_add_single_value(void) {
    const char* test = "test_accum_add_single_value";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    p25_status_accum_reset(ctx.state);
    p25_status_accum_add(ctx.state, 0x02);

    int rc = 0;
    rc |= expect_u32(test, "count", ctx.state->p25_ss_count, 1);
    rc |= expect_u32(test, "buf[0]", ctx.state->p25_ss_buf[0], 0x02);

    free_ctx(&ctx);
    return rc;
}

static int
test_accum_accepts_full_ldu_status_count(void) {
    const char* test = "test_accum_accepts_full_ldu_status_count";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    p25_status_accum_reset(ctx.state);
    for (int i = 0; i < 12; i++) {
        p25_status_accum_add(ctx.state, 0x02);
    }
    for (int i = 0; i < 12; i++) {
        p25_status_accum_add(ctx.state, 0x01);
    }
    p25_status_accum_classify(ctx.state, ctx.opts);

    int rc = 0;
    rc |= expect_u32(test, "count", ctx.state->p25_ss_count, P25_STATUS_ACCUM_MAX);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_INFRASTRUCTURE);

    free_ctx(&ctx);
    return rc;
}

static int
test_classify_status_values(void) {
    const char* test = "test_classify_status_values";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    int rc = 0;

    p25_status_accum_reset(ctx.state);
    p25_status_accum_add(ctx.state, 0x01);
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_INFRASTRUCTURE);

    p25_status_accum_reset(ctx.state);
    p25_status_accum_add(ctx.state, 0x03);
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_INFRASTRUCTURE);

    p25_status_accum_reset(ctx.state);
    for (int i = 0; i < 24; i++) {
        p25_status_accum_add(ctx.state, 0x00);
    }
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_SUBSCRIBER);

    p25_status_accum_reset(ctx.state);
    for (int i = 0; i < 24; i++) {
        p25_status_accum_add(ctx.state, 0x02);
    }
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_UNKNOWN);

    free_ctx(&ctx);
    return rc;
}

static int
test_classify_ignores_10_and_uses_counts(void) {
    const char* test = "test_classify_ignores_10_and_uses_counts";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    int rc = 0;

    p25_status_accum_reset(ctx.state);
    p25_status_accum_add(ctx.state, 0x00);
    for (int i = 0; i < 10; i++) {
        p25_status_accum_add(ctx.state, 0x02);
    }
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_SUBSCRIBER);

    p25_status_accum_reset(ctx.state);
    for (int i = 0; i < 10; i++) {
        p25_status_accum_add(ctx.state, 0x00);
    }
    p25_status_accum_add(ctx.state, 0x01);
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_SUBSCRIBER);

    p25_status_accum_reset(ctx.state);
    p25_status_accum_add(ctx.state, 0x00);
    p25_status_accum_add(ctx.state, 0x01);
    p25_status_accum_add(ctx.state, 0x03);
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_INFRASTRUCTURE);

    free_ctx(&ctx);
    return rc;
}

static int
test_gate_decisions_and_counters(void) {
    const char* test = "test_gate_decisions_and_counters";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    int rc = 0;

    p25_status_accum_reset(ctx.state);
    p25_status_accum_add(ctx.state, 0x01);
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_u32(test, "gate_allow infra", ctx.state->p25_afc_gate_allow, 1);
    rc |= expect_u32(test, "gate_valid infra", ctx.state->p25_afc_gate_valid, 1);
    rc |= expect_u32(test, "allowed_count", ctx.state->p25_afc_allowed_count, 1);
    rc |= expect_u32(test, "suppressed_count", ctx.state->p25_afc_suppressed_count, 0);

    p25_status_accum_reset(ctx.state);
    p25_status_accum_add(ctx.state, 0x00);
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_u32(test, "gate_allow subscriber", ctx.state->p25_afc_gate_allow, 0);
    rc |= expect_u32(test, "allowed_count after subscriber", ctx.state->p25_afc_allowed_count, 1);
    rc |= expect_u32(test, "suppressed_count after subscriber", ctx.state->p25_afc_suppressed_count, 1);

    p25_status_accum_reset(ctx.state);
    p25_status_accum_add(ctx.state, 0x02);
    p25_status_accum_classify(ctx.state, ctx.opts);
    rc |= expect_u32(test, "gate_allow unknown", ctx.state->p25_afc_gate_allow, 0);
    rc |= expect_u32(test, "suppressed_count after unknown", ctx.state->p25_afc_suppressed_count, 2);

    free_ctx(&ctx);
    return rc;
}

static int
test_gate_allow_when_disabled(void) {
    const char* test = "test_gate_allow_when_disabled";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    ctx.opts->p25_afc_gate_disable = 1;

    p25_status_accum_reset(ctx.state);
    for (int i = 0; i < 24; i++) {
        p25_status_accum_add(ctx.state, 0x00);
    }
    p25_status_accum_classify(ctx.state, ctx.opts);

    int rc = expect_u32(test, "gate_allow", ctx.state->p25_afc_gate_allow, 1);

    free_ctx(&ctx);
    return rc;
}

static int
test_initial_state_is_unknown_zero_counts(void) {
    const char* test = "test_initial_state_is_unknown_zero_counts";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    int rc = 0;
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_UNKNOWN);
    rc |= expect_u32(test, "count", ctx.state->p25_ss_count, 0);
    rc |= expect_u32(test, "gate_allow", ctx.state->p25_afc_gate_allow, 0);
    rc |= expect_u32(test, "gate_valid", ctx.state->p25_afc_gate_valid, 0);
    rc |= expect_u32(test, "allowed_count", ctx.state->p25_afc_allowed_count, 0);
    rc |= expect_u32(test, "suppressed_count", ctx.state->p25_afc_suppressed_count, 0);

    free_ctx(&ctx);
    return rc;
}

static int
test_overflow_ignored_gracefully(void) {
    const char* test = "test_overflow_ignored_gracefully";
    test_ctx ctx = make_ctx(test);
    if (!ctx.state) {
        return 1;
    }

    p25_status_accum_reset(ctx.state);
    for (int i = 0; i < 30; i++) {
        p25_status_accum_add(ctx.state, 0x01);
    }
    p25_status_accum_classify(ctx.state, ctx.opts);

    int rc = 0;
    rc |= expect_u32(test, "count", ctx.state->p25_ss_count, P25_STATUS_ACCUM_MAX);
    rc |= expect_class(test, ctx.state, P25_SS_CLASS_INFRASTRUCTURE);

    free_ctx(&ctx);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_accum_reset_zeroes_state();
    rc |= test_ensure_started_preserves_dispatch_status();
    rc |= test_accum_add_single_value();
    rc |= test_accum_accepts_full_ldu_status_count();
    rc |= test_classify_status_values();
    rc |= test_classify_ignores_10_and_uses_counts();
    rc |= test_gate_decisions_and_counters();
    rc |= test_gate_allow_when_disabled();
    rc |= test_initial_state_is_unknown_zero_counts();
    rc |= test_overflow_ignored_gracefully();

    return rc;
}
