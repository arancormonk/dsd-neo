// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frontend.h"

#include <dsd-neo/app_control/frontend_provider.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/engine/engine.h"

static int g_prepare_calls;

static int
fake_prepare(const dsd_opts* opts, const dsd_state* state, dsd_engine_lifecycle_hooks* out) {
    (void)opts;
    (void)state;
    g_prepare_calls++;
    if (!out) {
        return -1;
    }
    *out = (dsd_engine_lifecycle_hooks){
        .start = NULL,
        .stop = NULL,
        .context = (void*)0x1234,
    };
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
test_none_selects_no_provider(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_engine_lifecycle_hooks hooks = {0};
    const dsd_engine_lifecycle_hooks* out = (const dsd_engine_lifecycle_hooks*)0x1;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NONE;
    g_prepare_calls = 0;

    int rc = dsd_cli_frontend_select_from_registry(&opts, &state, &hooks, &out, NULL, 0);

    int failures = 0;
    failures |= expect_int("none rc", rc, 0);
    failures |= expect_int("none prepare calls", g_prepare_calls, 0);
    if (out != NULL) {
        DSD_FPRINTF(stderr, "none out_hooks should be NULL\n");
        failures |= 1;
    }
    return failures;
}

static int
test_unavailable_native_fails_cleanly(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_engine_lifecycle_hooks hooks = {0};
    const dsd_engine_lifecycle_hooks* out = NULL;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NATIVE;
    g_prepare_calls = 0;

    int rc = dsd_cli_frontend_select_from_registry(&opts, &state, &hooks, &out, NULL, 0);

    int failures = 0;
    failures |= expect_int("native unavailable rc", rc, -1);
    failures |= expect_int("native unavailable prepare calls", g_prepare_calls, 0);
    if (out != NULL) {
        DSD_FPRINTF(stderr, "native unavailable out_hooks should be NULL\n");
        failures |= 1;
    }
    return failures;
}

static int
test_unavailable_terminal_matches_disabled_build_registry(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_engine_lifecycle_hooks hooks = {0};
    const dsd_engine_lifecycle_hooks* out = NULL;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    g_prepare_calls = 0;

    int rc = dsd_cli_frontend_select_from_registry(&opts, &state, &hooks, &out, NULL, 0);

    int failures = 0;
    failures |= expect_int("terminal unavailable rc", rc, -1);
    failures |= expect_int("terminal unavailable prepare calls", g_prepare_calls, 0);
    return failures;
}

static int
test_fake_provider_is_selected(void) {
    static const dsd_frontend_provider fake_native = {DSD_FRONTEND_NATIVE, "fake-native", fake_prepare};
    const dsd_frontend_provider* providers[] = {&fake_native};
    static dsd_opts opts;
    static dsd_state state;
    dsd_engine_lifecycle_hooks hooks = {0};
    const dsd_engine_lifecycle_hooks* out = NULL;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NATIVE;
    g_prepare_calls = 0;

    int rc = dsd_cli_frontend_select_from_registry(&opts, &state, &hooks, &out, providers, 1);

    int failures = 0;
    failures |= expect_int("fake provider rc", rc, 0);
    failures |= expect_int("fake provider prepare calls", g_prepare_calls, 1);
    if (out != &hooks || hooks.context != (void*)0x1234) {
        DSD_FPRINTF(stderr, "fake provider hooks not returned as expected\n");
        failures |= 1;
    }
    return failures;
}

int
main(void) {
    int rc = 0;
    rc |= test_none_selects_no_provider();
    rc |= test_unavailable_native_fails_cleanly();
    rc |= test_unavailable_terminal_matches_disabled_build_registry();
    rc |= test_fake_provider_is_selected();
    if (rc == 0) {
        puts("RUNTIME_CLI_FRONTEND_PROVIDER: OK");
    }
    return rc;
}
