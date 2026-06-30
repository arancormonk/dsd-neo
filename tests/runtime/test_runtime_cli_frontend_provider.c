// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frontend.h"

#include <dsd-neo/app_control/frontend_provider.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/exitflag.h>
#if DSD_CLI_HAS_NATIVE_UI
#include <dsd-neo/ui/native_provider.h>
#endif
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/engine/engine.h"

#if DSD_PLATFORM_WIN_NATIVE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef DWORD test_thread_id;

static test_thread_id
test_current_thread_id(void) {
    return GetCurrentThreadId();
}

static int
test_same_thread(test_thread_id a, test_thread_id b) {
    return a == b;
}
#else
#include <pthread.h>
typedef pthread_t test_thread_id;

static test_thread_id
test_current_thread_id(void) {
    return pthread_self();
}

static int
test_same_thread(test_thread_id a, test_thread_id b) {
    return pthread_equal(a, b);
}
#endif

static int g_prepare_calls;

int
dsd_engine_run_with_lifecycle(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks) {
    (void)opts;
    (void)state;
    (void)hooks;
    return -99;
}

#if DSD_CLI_HAS_NATIVE_UI
static int
compiled_native_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    return 0;
}

static void
compiled_native_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
}

static int
compiled_native_prepare(const dsd_opts* opts, const dsd_state* state, dsd_engine_lifecycle_hooks* out) {
    (void)opts;
    (void)state;
    if (!out) {
        return -1;
    }
    *out = (dsd_engine_lifecycle_hooks){
        .start = compiled_native_start,
        .stop = compiled_native_stop,
    };
    return 0;
}

const dsd_frontend_provider*
dsd_native_frontend_provider(void) {
    static const dsd_frontend_provider provider = {
        .kind = DSD_FRONTEND_NATIVE,
        .name = "compiled-native-stub",
        .prepare = compiled_native_prepare,
        .flags = DSD_FRONTEND_PROVIDER_MAIN_THREAD_UI,
    };
    return &provider;
}
#endif

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
    static const dsd_frontend_provider fake_native = {
        .kind = DSD_FRONTEND_NATIVE,
        .name = "fake-native",
        .prepare = fake_prepare,
    };
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

static int
test_compiled_native_provider_availability(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_engine_lifecycle_hooks hooks = {0};
    const dsd_engine_lifecycle_hooks* out = NULL;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NATIVE;
    g_prepare_calls = 0;

    int rc = dsd_cli_frontend_select(&opts, &state, &hooks, &out);

#if DSD_CLI_HAS_NATIVE_UI
    int failures = expect_int("compiled native available rc", rc, 0);
    if (out != &hooks || hooks.start == NULL || hooks.stop == NULL) {
        DSD_FPRINTF(stderr, "compiled native hooks not returned as expected\n");
        failures |= 1;
    }
    return failures;
#else
    int failures = expect_int("compiled native unavailable rc", rc, -1);
    if (out != NULL) {
        DSD_FPRINTF(stderr, "compiled native unavailable out_hooks should be NULL\n");
        failures |= 1;
    }
    return failures;
#endif
}

typedef struct fake_main_thread_ctx {
    test_thread_id caller_thread;
    test_thread_id runner_thread;
    test_thread_id main_loop_thread;
    atomic_int runner_started;
    atomic_int runner_allow_finish;
    atomic_int main_loop_saw_engine_done;
    int runner_rc;
} fake_main_thread_ctx;

static fake_main_thread_ctx* g_fake_main_thread_context;

static int
fake_main_thread_prepare(const dsd_opts* opts, const dsd_state* state, dsd_engine_lifecycle_hooks* out) {
    (void)opts;
    (void)state;
    if (!out) {
        return -1;
    }
    *out = (dsd_engine_lifecycle_hooks){
        .context = g_fake_main_thread_context,
    };
    return 0;
}

static int
fake_blocking_engine_runner(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks, void* context) {
    (void)opts;
    (void)state;
    (void)hooks;
    fake_main_thread_ctx* ctx = (fake_main_thread_ctx*)context;
    ctx->runner_thread = test_current_thread_id();
    atomic_store(&ctx->runner_started, 1);
    for (int i = 0; i < 1000 && !dsd_exitflag_load() && !atomic_load(&ctx->runner_allow_finish); i++) {
        dsd_sleep_ms(1);
    }
    return ctx->runner_rc;
}

static int
fake_exiting_engine_runner(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks, void* context) {
    (void)opts;
    (void)state;
    (void)hooks;
    fake_main_thread_ctx* ctx = (fake_main_thread_ctx*)context;
    ctx->runner_thread = test_current_thread_id();
    atomic_store(&ctx->runner_started, 1);
    return ctx->runner_rc;
}

static int
fake_main_loop_exits_first(const dsd_frontend_host_callbacks* host, void* context) {
    fake_main_thread_ctx* ctx = (fake_main_thread_ctx*)context;
    ctx->main_loop_thread = test_current_thread_id();
    for (int i = 0; i < 1000 && !atomic_load(&ctx->runner_started); i++) {
        dsd_sleep_ms(1);
    }
    if (host && host->request_engine_stop) {
        host->request_engine_stop(host->context);
    }
    return 0;
}

static int
fake_main_loop_waits_for_engine(const dsd_frontend_host_callbacks* host, void* context) {
    fake_main_thread_ctx* ctx = (fake_main_thread_ctx*)context;
    ctx->main_loop_thread = test_current_thread_id();
    for (int i = 0; i < 1000; i++) {
        if (host && host->engine_finished && host->engine_finished(host->context)) {
            atomic_store(&ctx->main_loop_saw_engine_done, 1);
            return 0;
        }
        dsd_sleep_ms(1);
    }
    return -1;
}

static int
test_main_thread_provider_runs_engine_on_worker(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NATIVE;
    dsd_exitflag_store(0);

    static fake_main_thread_ctx ctx;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    ctx.caller_thread = test_current_thread_id();
    ctx.runner_rc = 0;
    g_fake_main_thread_context = &ctx;

    static const dsd_frontend_provider fake_main = {
        .kind = DSD_FRONTEND_NATIVE,
        .name = "fake-main",
        .prepare = fake_main_thread_prepare,
        .flags = DSD_FRONTEND_PROVIDER_MAIN_THREAD_UI,
        .run_main_loop = fake_main_loop_exits_first,
    };
    const dsd_frontend_provider* providers[] = {&fake_main};
    int rc = dsd_cli_frontend_run_from_registry(&opts, &state, providers, 1, fake_blocking_engine_runner, &ctx);
    g_fake_main_thread_context = NULL;

    int failures = 0;
    failures |= expect_int("main-thread fake run rc", rc, 0);
    failures |= expect_int("main-thread fake runner started", atomic_load(&ctx.runner_started), 1);
    if (!test_same_thread(ctx.main_loop_thread, ctx.caller_thread)) {
        DSD_FPRINTF(stderr, "main loop did not run on caller thread\n");
        failures |= 1;
    }
    if (test_same_thread(ctx.runner_thread, ctx.caller_thread)) {
        DSD_FPRINTF(stderr, "engine runner unexpectedly ran on caller thread\n");
        failures |= 1;
    }
    dsd_exitflag_store(0);
    return failures;
}

static int
test_main_thread_provider_observes_engine_exit_first(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NATIVE;
    dsd_exitflag_store(0);

    static fake_main_thread_ctx ctx;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    ctx.caller_thread = test_current_thread_id();
    ctx.runner_rc = 7;
    g_fake_main_thread_context = &ctx;

    static const dsd_frontend_provider fake_main = {
        .kind = DSD_FRONTEND_NATIVE,
        .name = "fake-main",
        .prepare = fake_main_thread_prepare,
        .flags = DSD_FRONTEND_PROVIDER_MAIN_THREAD_UI,
        .run_main_loop = fake_main_loop_waits_for_engine,
    };
    const dsd_frontend_provider* providers[] = {&fake_main};
    int rc = dsd_cli_frontend_run_from_registry(&opts, &state, providers, 1, fake_exiting_engine_runner, &ctx);
    g_fake_main_thread_context = NULL;

    int failures = 0;
    failures |= expect_int("engine-first fake run rc", rc, 7);
    failures |= expect_int("engine-first main saw done", atomic_load(&ctx.main_loop_saw_engine_done), 1);
    if (!test_same_thread(ctx.main_loop_thread, ctx.caller_thread)) {
        DSD_FPRINTF(stderr, "engine-first main loop did not run on caller thread\n");
        failures |= 1;
    }
    if (test_same_thread(ctx.runner_thread, ctx.caller_thread)) {
        DSD_FPRINTF(stderr, "engine-first runner unexpectedly ran on caller thread\n");
        failures |= 1;
    }
    dsd_exitflag_store(0);
    return failures;
}

int
main(void) {
    int rc = 0;
    rc |= test_none_selects_no_provider();
    rc |= test_unavailable_native_fails_cleanly();
    rc |= test_unavailable_terminal_matches_disabled_build_registry();
    rc |= test_fake_provider_is_selected();
    rc |= test_compiled_native_provider_availability();
    rc |= test_main_thread_provider_runs_engine_on_worker();
    rc |= test_main_thread_provider_observes_engine_exit_first();
    if (rc == 0) {
        puts("RUNTIME_CLI_FRONTEND_PROVIDER: OK");
    }
    return rc;
}
