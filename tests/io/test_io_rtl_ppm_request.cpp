// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdio>
#include <stdint.h>

#include <dsd-neo/core/constants.h>

#include "rtl_ppm_request.h"

using dsd::io::radio::rtl_ppm_plan_apply_to_active_stream;
using dsd::io::radio::rtl_ppm_resolve_rejected_request;
using dsd::io::radio::rtl_ppm_should_reconfigure_after_apply;
using dsd::io::radio::rtl_ppm_should_schedule_request;
using dsd::io::radio::RtlPpmApplyPlan;
using dsd::io::radio::RtlPpmControllerRequestState;
using dsd::io::radio::RtlPpmRejectedRequestResolution;

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* label, bool cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_false(const char* label, bool cond) {
    return expect_true(label, !cond);
}

static RtlPpmControllerRequestState
make_request_state(int pending, int ppm, uint32_t request_id) {
    RtlPpmControllerRequestState state = {};
    state.pending = pending;
    state.ppm = ppm;
    state.request_id = request_id;
    return state;
}

static int
test_schedule_request_when_desired_ppm_differs_from_applied(void) {
    return expect_true(
        "idle controller schedules desired ppm",
        rtl_ppm_should_schedule_request(0, 5, 7, make_request_state(0, 0, 0), make_request_state(0, 0, 0)));
}

static int
test_schedule_request_skips_duplicate_active_generation(void) {
    return expect_false(
        "matching in-flight generation is not re-queued",
        rtl_ppm_should_schedule_request(0, 5, 7, make_request_state(0, 0, 0), make_request_state(1, 5, 7)));
}

static int
test_schedule_request_overwrites_stale_queued_generation(void) {
    return expect_true(
        "stale queued request is overwritten even when active matches desired",
        rtl_ppm_should_schedule_request(0, 5, 7, make_request_state(1, 9, 6), make_request_state(1, 5, 7)));
}

static int
test_schedule_request_overwrites_stale_queue_with_applied_value(void) {
    return expect_true(
        "stale queued request is replaced by applied-value snapshot",
        rtl_ppm_should_schedule_request(0, 0, 8, make_request_state(1, 5, 7), make_request_state(0, 0, 0)));
}

static int
test_rejected_request_rolls_back_matching_requested_ppm(void) {
    int rc = 0;
    RtlPpmRejectedRequestResolution resolution = rtl_ppm_resolve_rejected_request(0, 5, 7, 7);
    rc |= expect_int_eq("rollback updates requested ppm", resolution.requested_ppm, 0);
    rc |= expect_int_eq("rollback keeps matching request id", (int)resolution.requested_request_id, 7);
    rc |= expect_int_eq("rollback marks stale request handled", resolution.rolled_back, 1);
    return rc;
}

static int
test_rejected_request_preserves_newer_requested_ppm(void) {
    int rc = 0;
    RtlPpmRejectedRequestResolution resolution = rtl_ppm_resolve_rejected_request(0, 6, 8, 7);
    rc |= expect_int_eq("newer request stays intact", resolution.requested_ppm, 6);
    rc |= expect_int_eq("newer request keeps newer id", (int)resolution.requested_request_id, 8);
    rc |= expect_int_eq("newer request is not rolled back", resolution.rolled_back, 0);
    return rc;
}

static int
test_rejected_request_preserves_same_value_retry(void) {
    int rc = 0;
    RtlPpmRejectedRequestResolution resolution = rtl_ppm_resolve_rejected_request(0, 5, 8, 7);
    rc |= expect_int_eq("same-value retry stays intact", resolution.requested_ppm, 5);
    rc |= expect_int_eq("same-value retry keeps newer id", (int)resolution.requested_request_id, 8);
    rc |= expect_int_eq("same-value retry is not rolled back", resolution.rolled_back, 0);
    return rc;
}

static int
test_rejected_request_ignores_already_applied_request(void) {
    int rc = 0;
    RtlPpmRejectedRequestResolution resolution = rtl_ppm_resolve_rejected_request(-2, -2, 3, 7);
    rc |= expect_int_eq("already applied request stays intact", resolution.requested_ppm, -2);
    rc |= expect_int_eq("already applied request keeps its id", (int)resolution.requested_request_id, 3);
    rc |= expect_int_eq("already applied request does not roll back", resolution.rolled_back, 0);
    return rc;
}

static int
test_apply_plan_reconfigures_live_stream_on_ppm_change(void) {
    int rc = 0;
    RtlPpmApplyPlan plan = rtl_ppm_plan_apply_to_active_stream(769768750U, 851012500U, 0, 1);
    rc |= expect_int_eq("live stream ppm change uses reconfigure path", plan.reconfigure, 1);
    rc |= expect_int_eq("live stream reconfigures current applied freq", (int)plan.freq_hz, 769768750);
    return rc;
}

static int
test_apply_plan_uses_fallback_freq_when_last_applied_missing(void) {
    int rc = 0;
    RtlPpmApplyPlan plan = rtl_ppm_plan_apply_to_active_stream(0U, 851012500U, -2, 1);
    rc |= expect_int_eq("fallback freq still reconfigures", plan.reconfigure, 1);
    rc |= expect_int_eq("fallback freq selected", (int)plan.freq_hz, 851012500);
    return rc;
}

static int
test_apply_plan_skips_reconfigure_when_ppm_unchanged(void) {
    int rc = 0;
    RtlPpmApplyPlan plan = rtl_ppm_plan_apply_to_active_stream(769768750U, 851012500U, 1, 1);
    rc |= expect_int_eq("unchanged ppm skips reconfigure", plan.reconfigure, 0);
    rc |= expect_int_eq("unchanged ppm has no target freq", (int)plan.freq_hz, 0);
    return rc;
}

static int
test_apply_plan_allows_apply_only_without_known_freq(void) {
    int rc = 0;
    RtlPpmApplyPlan plan = rtl_ppm_plan_apply_to_active_stream(0U, 0U, 0, -3);
    rc |= expect_int_eq("unknown freq does not force reconfigure", plan.reconfigure, 0);
    rc |= expect_int_eq("unknown freq leaves target empty", (int)plan.freq_hz, 0);
    return rc;
}

static int
test_apply_plan_reconfigures_only_after_successful_ppm_apply(void) {
    RtlPpmApplyPlan plan = rtl_ppm_plan_apply_to_active_stream(769768750U, 851012500U, 0, 1);
    return expect_true("successful live ppm apply triggers reconfigure",
                       rtl_ppm_should_reconfigure_after_apply(plan, 0));
}

static int
test_apply_plan_skips_reconfigure_when_ppm_apply_is_rejected(void) {
    RtlPpmApplyPlan plan = rtl_ppm_plan_apply_to_active_stream(769768750U, 851012500U, 0, 1);
    return expect_false("rejected live ppm apply does not reconfigure",
                        rtl_ppm_should_reconfigure_after_apply(plan, DSD_ERR_NOT_SUPPORTED));
}

int
main(void) {
    int rc = 0;
    rc |= test_schedule_request_when_desired_ppm_differs_from_applied();
    rc |= test_schedule_request_skips_duplicate_active_generation();
    rc |= test_schedule_request_overwrites_stale_queued_generation();
    rc |= test_schedule_request_overwrites_stale_queue_with_applied_value();
    rc |= test_rejected_request_rolls_back_matching_requested_ppm();
    rc |= test_rejected_request_preserves_newer_requested_ppm();
    rc |= test_rejected_request_preserves_same_value_retry();
    rc |= test_rejected_request_ignores_already_applied_request();
    rc |= test_apply_plan_reconfigures_live_stream_on_ppm_change();
    rc |= test_apply_plan_uses_fallback_freq_when_last_applied_missing();
    rc |= test_apply_plan_skips_reconfigure_when_ppm_unchanged();
    rc |= test_apply_plan_allows_apply_only_without_known_freq();
    rc |= test_apply_plan_reconfigures_only_after_successful_ppm_apply();
    rc |= test_apply_plan_skips_reconfigure_when_ppm_apply_is_rejected();
    return rc ? 1 : 0;
}
