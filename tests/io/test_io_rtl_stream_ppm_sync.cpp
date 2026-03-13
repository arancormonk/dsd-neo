// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rtl_stream.h>
#include <dsd-neo/io/rtl_stream_c.h>

#include <cstdio>
#include <memory>

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

static std::unique_ptr<dsd_opts>
make_test_opts() {
    // dsd_opts is ~34 KB; keep repeated snapshots off the function stack.
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    init_test_opts(opts.get());
    return opts;
}

static int
test_caller_request_updates_active_snapshot(void) {
    std::unique_ptr<dsd_opts> caller_opts = make_test_opts();
    caller_opts->rtlsdr_ppm_error = 0;

    RtlSdrOrchestrator stream(*caller_opts, caller_opts.get());

    int rc = 0;
    rc |= expect_int_eq("caller request rc", rtl_stream_request_ppm(caller_opts.get(), 5), 0);
    rc |= expect_int_eq("caller ppm mirrors request", caller_opts->rtlsdr_ppm_error, 5);
    rc |= expect_int_eq("active snapshot mirrors caller request", stream.requested_ppm(), 5);
    rc |= expect_int_eq("requested ppm getter returns live value", rtl_stream_get_requested_ppm(caller_opts.get()), 5);
    return rc;
}

static int
test_active_request_updates_caller_snapshot(void) {
    std::unique_ptr<dsd_opts> caller_opts = make_test_opts();
    caller_opts->rtlsdr_ppm_error = 0;

    RtlSdrOrchestrator stream(*caller_opts, caller_opts.get());

    int rc = 0;
    rc |= expect_int_eq("active request rc", stream.request_ppm(-4), 0);
    rc |= expect_int_eq("caller mirrors active request", caller_opts->rtlsdr_ppm_error, -4);
    rc |= expect_int_eq("active request applied", stream.requested_ppm(), -4);
    return rc;
}

static int
test_adjust_and_getter_use_active_snapshot_when_caller_is_stale(void) {
    std::unique_ptr<dsd_opts> caller_opts = make_test_opts();
    caller_opts->rtlsdr_ppm_error = 0;

    RtlSdrOrchestrator stream(*caller_opts, caller_opts.get());

    int rc = 0;
    rc |= expect_int_eq("seed active request", stream.request_ppm(9), 0);

    caller_opts->rtlsdr_ppm_error = 1;
    rc |= expect_int_eq("getter prefers active snapshot", rtl_stream_get_requested_ppm(caller_opts.get()), 9);

    rc |= expect_int_eq("adjust rc", rtl_stream_adjust_ppm(caller_opts.get(), -2), 0);
    rc |= expect_int_eq("adjust uses active snapshot", stream.requested_ppm(), 7);
    rc |= expect_int_eq("adjust resyncs caller snapshot", caller_opts->rtlsdr_ppm_error, 7);
    return rc;
}

static int
test_c_api_create_mirrored_updates_live_caller_opts(void) {
    std::unique_ptr<dsd_opts> caller_opts = make_test_opts();
    caller_opts->rtlsdr_ppm_error = 11;

    RtlSdrContext* ctx = nullptr;

    int rc = 0;
    rc |= expect_int_eq("c api mirrored create rc", rtl_stream_create_mirrored(caller_opts.get(), &ctx), 0);
    if (!ctx) {
        std::fprintf(stderr, "FAIL: c api mirrored create returned null ctx\n");
        return 1;
    }

    std::unique_ptr<dsd_opts> request_opts = make_test_opts();
    request_opts->rtlsdr_ppm_error = 0;

    rc |= expect_int_eq("request through live helper rc", rtl_stream_request_ppm(request_opts.get(), -12), 0);
    rc |= expect_int_eq("mirrored caller updated", caller_opts->rtlsdr_ppm_error, -12);
    rc |= expect_int_eq("request opts updated", request_opts->rtlsdr_ppm_error, -12);
    rc |= expect_int_eq("getter observes mirrored caller", rtl_stream_get_requested_ppm(caller_opts.get()), -12);

    rc |= expect_int_eq("c api mirrored destroy rc", rtl_stream_destroy(ctx), 0);
    return rc;
}

static int
test_c_api_create_preserves_const_source_snapshot(void) {
    std::unique_ptr<dsd_opts> seed_opts = make_test_opts();
    seed_opts->rtlsdr_ppm_error = 11;

    std::unique_ptr<dsd_opts> const_source_opts = std::make_unique<dsd_opts>(*seed_opts);
    RtlSdrContext* ctx = nullptr;

    int rc = 0;
    rc |= expect_int_eq("c api create rc", rtl_stream_create(const_source_opts.get(), &ctx), 0);
    if (!ctx) {
        std::fprintf(stderr, "FAIL: c api create returned null ctx\n");
        return 1;
    }

    std::unique_ptr<dsd_opts> request_opts = make_test_opts();
    request_opts->rtlsdr_ppm_error = 0;

    rc |= expect_int_eq("request through live helper rc", rtl_stream_request_ppm(request_opts.get(), -12), 0);
    rc |= expect_int_eq("const source opts remain unchanged", const_source_opts->rtlsdr_ppm_error, 11);
    rc |= expect_int_eq("request opts updated", request_opts->rtlsdr_ppm_error, -12);
    rc |= expect_int_eq("getter still observes active snapshot", rtl_stream_get_requested_ppm(const_source_opts.get()),
                        -12);

    rc |= expect_int_eq("c api destroy rc", rtl_stream_destroy(ctx), 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_caller_request_updates_active_snapshot();
    rc |= test_active_request_updates_caller_snapshot();
    rc |= test_adjust_and_getter_use_active_snapshot_when_caller_is_stale();
    rc |= test_c_api_create_mirrored_updates_live_caller_opts();
    rc |= test_c_api_create_preserves_const_source_snapshot();
    return rc ? 1 : 0;
}
