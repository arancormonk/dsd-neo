// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdio>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rtl_stream.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <memory>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

namespace {
struct StubState {
    int open_rc;
    int tune_rc;
    int read_rc;
    uint32_t output_generation;
    int invalidate_first_read;
    int open_calls;
    int soft_stop_calls;
    int tune_calls;
    int read_calls;
    int register_calls;
    int unregister_calls;
    dsd_opts* last_open_opts;
    dsd_opts* last_registered_active;
    dsd_opts* last_registered_caller;
    dsd_opts* last_unregistered_active;
    dsd_opts* last_unregistered_caller;
    long int last_tune_hz;
    size_t last_read_count;
};

static StubState g_stub;

static void
reset_stubs(void) {
    g_stub = {};
    g_stub.output_generation = 1U;
}

static std::unique_ptr<dsd_opts>
make_opts(int ppm = 0) {
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    *opts = {};
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_ppm_error = ppm;
    return opts;
}

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_ptr_eq(const char* label, const void* got, const void* want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got=%p want=%p\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_ptr_ne(const char* label, const void* got, const void* not_want) {
    if (got == not_want) {
        DSD_FPRINTF(stderr, "%s: unexpected pointer=%p\n", label, got);
        return 1;
    }
    return 0;
}
} // namespace

extern "C" int
dsd_rtl_stream_open(dsd_opts* opts) {
    g_stub.open_calls++;
    g_stub.last_open_opts = opts;
    return g_stub.open_rc;
}

extern "C" int
dsd_rtl_stream_read(float* out, size_t count, dsd_opts* opts, const dsd_state* state) {
    (void)out;
    (void)state;
    g_stub.read_calls++;
    g_stub.last_open_opts = opts;
    g_stub.last_read_count = count;
    if (g_stub.invalidate_first_read && g_stub.read_calls == 1) {
        g_stub.output_generation++;
    }
    return g_stub.read_rc;
}

extern "C" uint32_t
rtl_stream_output_generation(void) {
    return g_stub.output_generation;
}

extern "C" int
dsd_rtl_stream_tune(dsd_opts* opts, long int frequency) {
    g_stub.tune_calls++;
    g_stub.last_open_opts = opts;
    g_stub.last_tune_hz = frequency;
    return g_stub.tune_rc;
}

extern "C" int
dsd_rtl_stream_soft_stop(void) {
    g_stub.soft_stop_calls++;
    return 0;
}

extern "C" void
dsd_rtl_stream_register_requested_ppm_opts(dsd_opts* active_opts, dsd_opts* caller_opts) {
    g_stub.register_calls++;
    g_stub.last_registered_active = active_opts;
    g_stub.last_registered_caller = caller_opts;
}

extern "C" void
dsd_rtl_stream_unregister_requested_ppm_opts(dsd_opts* active_opts, dsd_opts* caller_opts) {
    g_stub.unregister_calls++;
    g_stub.last_unregistered_active = active_opts;
    g_stub.last_unregistered_caller = caller_opts;
}

static int
test_constructor_snapshots_and_registers_opts(void) {
    reset_stubs();
    std::unique_ptr<dsd_opts> caller = make_opts(7);

    int rc = 0;
    {
        RtlSdrOrchestrator stream(*caller);
        caller->rtlsdr_ppm_error = 99;
        rc |= expect_int_eq("constructor registers active opts", g_stub.register_calls, 1);
        rc |= expect_ptr_ne("constructor copies opts", g_stub.last_registered_active, caller.get());
        rc |= expect_ptr_eq("constructor records caller opts", g_stub.last_registered_caller, caller.get());
        rc |=
            expect_int_eq("active snapshot preserves original ppm", g_stub.last_registered_active->rtlsdr_ppm_error, 7);
    }
    rc |= expect_int_eq("destructor unregisters active opts", g_stub.unregister_calls, 1);
    rc |= expect_ptr_eq("destructor unregisters same active opts", g_stub.last_unregistered_active,
                        g_stub.last_registered_active);
    rc |= expect_ptr_eq("destructor unregisters caller opts", g_stub.last_unregistered_caller, caller.get());
    return rc;
}

static int
test_start_stop_and_destructor_lifecycle(void) {
    reset_stubs();
    std::unique_ptr<dsd_opts> opts = make_opts();

    int rc = 0;
    {
        RtlSdrOrchestrator stream(*opts);
        rc |= expect_int_eq("start succeeds", stream.start(), 0);
        rc |= expect_int_eq("start calls open once", g_stub.open_calls, 1);
        rc |= expect_int_eq("second start is no-op", stream.start(), 0);
        rc |= expect_int_eq("second start keeps open count", g_stub.open_calls, 1);
        rc |= expect_int_eq("stop succeeds", stream.stop(), 0);
        rc |= expect_int_eq("stop uses soft stop", g_stub.soft_stop_calls, 1);
        rc |= expect_int_eq("second stop is no-op", stream.stop(), 0);
        rc |= expect_int_eq("second stop keeps soft stop count", g_stub.soft_stop_calls, 1);
    }
    rc |= expect_int_eq("destructor does not stop already stopped stream", g_stub.soft_stop_calls, 1);

    reset_stubs();
    opts = make_opts();
    {
        RtlSdrOrchestrator stream(*opts);
        rc |= expect_int_eq("start succeeds before destructor", stream.start(), 0);
    }
    rc |= expect_int_eq("destructor stops active stream", g_stub.soft_stop_calls, 1);
    return rc;
}

static int
test_start_failure_and_prestart_errors(void) {
    reset_stubs();
    g_stub.open_rc = -7;
    std::unique_ptr<dsd_opts> opts = make_opts();
    RtlSdrOrchestrator stream(*opts);

    int rc = 0;
    rc |= expect_int_eq("start propagates open failure", stream.start(), -7);
    rc |= expect_int_eq("failed start does not soft stop", g_stub.soft_stop_calls, 0);
    rc |= expect_int_eq("prestart tune rejected", stream.tune(851000000U), -1);
    rc |= expect_int_eq("prestart tune does not call backend tune", g_stub.tune_calls, 0);

    float sample = 0.0f;
    int got = 123;
    rc |= expect_int_eq("prestart read rejected", stream.read(&sample, 1U, got), -1);
    rc |= expect_int_eq("prestart read leaves got unchanged", got, 123);
    rc |= expect_int_eq("prestart read does not call backend read", g_stub.read_calls, 0);
    return rc;
}

static int
test_tune_and_read_error_propagation(void) {
    reset_stubs();
    std::unique_ptr<dsd_opts> opts = make_opts(3);
    RtlSdrOrchestrator stream(*opts);

    int rc = 0;
    rc |= expect_int_eq("start succeeds", stream.start(), 0);
    g_stub.tune_rc = -5;
    rc |= expect_int_eq("tune propagates failure", stream.tune(851012500U), -5);
    rc |= expect_int_eq("tune records frequency", (int)g_stub.last_tune_hz, 851012500);
    g_stub.tune_rc = 0;
    rc |= expect_int_eq("tune succeeds", stream.tune(851025000U), 0);
    float samples[4] = {};
    int got = 0;
    g_stub.read_rc = 3;
    rc |= expect_int_eq("read success", stream.read(samples, 4U, got), 0);
    rc |= expect_int_eq("read returns got count", got, 3);
    rc |= expect_int_eq("read records count", (int)g_stub.last_read_count, 4);

    g_stub.read_calls = 0;
    g_stub.invalidate_first_read = 1;
    rc |= expect_int_eq("read retries an invalidated handoff", stream.read(samples, 4U, got), 0);
    rc |= expect_int_eq("invalidated handoff reads a fresh batch", g_stub.read_calls, 2);
    rc |= expect_int_eq("fresh handoff returns got count", got, 3);
    g_stub.invalidate_first_read = 0;

    g_stub.read_rc = -9;
    rc |= expect_int_eq("read propagates failure", stream.read(samples, 4U, got), -9);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_constructor_snapshots_and_registers_opts();
    rc |= test_start_stop_and_destructor_lifecycle();
    rc |= test_start_failure_and_prestart_errors();
    rc |= test_tune_and_read_error_propagation();
    return rc ? 1 : 0;
}
