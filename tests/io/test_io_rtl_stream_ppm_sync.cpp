// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rtl_stream.h>
#include <dsd-neo/io/rtl_stream_c.h>

#include <cstdio>

#include "dsd-neo/core/opts_fwd.h"

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static void
init_test_opts(dsd_opts* opts) {
    *opts = {};
    opts->audio_in_type = AUDIO_IN_RTL;
}

static int
test_caller_request_updates_active_snapshot(void) {
    dsd_opts caller_opts;
    init_test_opts(&caller_opts);
    caller_opts.rtlsdr_ppm_error = 0;

    RtlSdrOrchestrator stream(caller_opts, &caller_opts);

    int rc = 0;
    rc |= expect_int_eq("caller request rc", rtl_stream_request_ppm(&caller_opts, 5), 0);
    rc |= expect_int_eq("caller ppm mirrors request", caller_opts.rtlsdr_ppm_error, 5);
    rc |= expect_int_eq("active snapshot mirrors caller request", stream.requested_ppm(), 5);
    rc |= expect_int_eq("requested ppm getter returns live value", rtl_stream_get_requested_ppm(&caller_opts), 5);
    return rc;
}

static int
test_active_request_updates_caller_snapshot(void) {
    dsd_opts caller_opts;
    init_test_opts(&caller_opts);
    caller_opts.rtlsdr_ppm_error = 0;

    RtlSdrOrchestrator stream(caller_opts, &caller_opts);

    int rc = 0;
    rc |= expect_int_eq("active request rc", stream.request_ppm(-4), 0);
    rc |= expect_int_eq("caller mirrors active request", caller_opts.rtlsdr_ppm_error, -4);
    rc |= expect_int_eq("active request applied", stream.requested_ppm(), -4);
    return rc;
}

static int
test_adjust_and_getter_use_active_snapshot_when_caller_is_stale(void) {
    dsd_opts caller_opts;
    init_test_opts(&caller_opts);
    caller_opts.rtlsdr_ppm_error = 0;

    RtlSdrOrchestrator stream(caller_opts, &caller_opts);

    int rc = 0;
    rc |= expect_int_eq("seed active request", stream.request_ppm(9), 0);

    caller_opts.rtlsdr_ppm_error = 1;
    rc |= expect_int_eq("getter prefers active snapshot", rtl_stream_get_requested_ppm(&caller_opts), 9);

    rc |= expect_int_eq("adjust rc", rtl_stream_adjust_ppm(&caller_opts, -2), 0);
    rc |= expect_int_eq("adjust uses active snapshot", stream.requested_ppm(), 7);
    rc |= expect_int_eq("adjust resyncs caller snapshot", caller_opts.rtlsdr_ppm_error, 7);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_caller_request_updates_active_snapshot();
    rc |= test_active_request_updates_caller_snapshot();
    rc |= test_adjust_and_getter_use_active_snapshot_when_caller_is_stale();
    return rc ? 1 : 0;
}
