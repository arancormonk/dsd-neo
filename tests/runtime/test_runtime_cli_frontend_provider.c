// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frontend.h"

#include <dsd-neo/app_control/frontend_provider.h>
#include <dsd-neo/core/frontend_types.h>
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
static int g_default_engine_calls;
static int g_default_engine_hooks_present;
static int g_selection_runner_calls;
static int g_selection_runner_hooks_null;
static void* g_selection_runner_hook_context;

int
dsd_engine_run_with_lifecycle(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks) {
    (void)opts;
    (void)state;
    g_default_engine_calls++;
    g_default_engine_hooks_present = hooks ? 1 : 0;
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
compiled_native_run_main_loop(const dsd_frontend_host_callbacks* host, void* context) {
    (void)context;
    for (int i = 0; i < 1000; i++) {
        if (host && host->engine_finished && host->engine_finished(host->context)) {
            return 0;
        }
        dsd_sleep_ms(1);
    }
    return -1;
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
        .run_main_loop = compiled_native_run_main_loop,
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
fake_selection_engine_runner(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    g_selection_runner_calls++;
    g_selection_runner_hooks_null = hooks ? 0 : 1;
    g_selection_runner_hook_context = hooks ? hooks->context : NULL;
    return 23;
}

static void
reset_selection_runner(void) {
    g_selection_runner_calls = 0;
    g_selection_runner_hooks_null = 0;
    g_selection_runner_hook_context = NULL;
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
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NONE;
    g_prepare_calls = 0;
    reset_selection_runner();

    int rc = dsd_cli_frontend_run_from_registry(&opts, &state, NULL, 0, fake_selection_engine_runner, NULL);

    int failures = 0;
    failures |= expect_int("none rc", rc, 23);
    failures |= expect_int("none prepare calls", g_prepare_calls, 0);
    failures |= expect_int("none runner calls", g_selection_runner_calls, 1);
    failures |= expect_int("none runner hooks null", g_selection_runner_hooks_null, 1);
    return failures;
}

static int
test_unavailable_native_fails_cleanly(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NATIVE;
    g_prepare_calls = 0;
    reset_selection_runner();

    int rc = dsd_cli_frontend_run_from_registry(&opts, &state, NULL, 0, fake_selection_engine_runner, NULL);

    int failures = 0;
    failures |= expect_int("native unavailable rc", rc, 1);
    failures |= expect_int("native unavailable prepare calls", g_prepare_calls, 0);
    failures |= expect_int("native unavailable runner calls", g_selection_runner_calls, 0);
    return failures;
}

static int
test_unavailable_terminal_matches_disabled_build_registry(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    g_prepare_calls = 0;
    reset_selection_runner();

    int rc = dsd_cli_frontend_run_from_registry(&opts, &state, NULL, 0, fake_selection_engine_runner, NULL);

    int failures = 0;
    failures |= expect_int("terminal unavailable rc", rc, 1);
    failures |= expect_int("terminal unavailable prepare calls", g_prepare_calls, 0);
    failures |= expect_int("terminal unavailable runner calls", g_selection_runner_calls, 0);
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
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NATIVE;
    g_prepare_calls = 0;
    reset_selection_runner();

    int rc = dsd_cli_frontend_run_from_registry(&opts, &state, providers, 1, fake_selection_engine_runner, NULL);

    int failures = 0;
    failures |= expect_int("fake provider rc", rc, 23);
    failures |= expect_int("fake provider prepare calls", g_prepare_calls, 1);
    failures |= expect_int("fake provider runner calls", g_selection_runner_calls, 1);
    failures |= expect_int("fake provider runner hooks null", g_selection_runner_hooks_null, 0);
    if (g_selection_runner_hook_context != (void*)0x1234) {
        DSD_FPRINTF(stderr, "fake provider hook context not returned as expected\n");
        failures |= 1;
    }
    return failures;
}

static int
test_compiled_native_provider_availability(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.frontend_kind = DSD_FRONTEND_NATIVE;
    g_prepare_calls = 0;
    g_default_engine_calls = 0;
    g_default_engine_hooks_present = 0;

    int rc = dsd_cli_frontend_run(&opts, &state);

#if DSD_CLI_HAS_NATIVE_UI
    int failures = expect_int("compiled native available rc", rc, -99);
    failures |= expect_int("compiled native engine calls", g_default_engine_calls, 1);
    failures |= expect_int("compiled native hooks present", g_default_engine_hooks_present, 1);
    return failures;
#else
    int failures = expect_int("compiled native unavailable rc", rc, 1);
    failures |= expect_int("compiled native engine calls", g_default_engine_calls, 0);
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
    atomic_int runner_saw_stop_after_reset;
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
fake_resetting_engine_runner(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks, void* context) {
    fake_main_thread_ctx* ctx = (fake_main_thread_ctx*)context;
    ctx->runner_thread = test_current_thread_id();
    atomic_store(&ctx->runner_started, 1);
    for (int i = 0; i < 1000 && !dsd_exitflag_load(); i++) {
        dsd_sleep_ms(1);
    }

    dsd_exitflag_store(0);
    if (hooks && hooks->start && hooks->start(opts, state, hooks->context) != 0) {
        return -1;
    }

    for (int i = 0; i < 1000; i++) {
        if (dsd_exitflag_load()) {
            atomic_store(&ctx->runner_saw_stop_after_reset, 1);
            break;
        }
        dsd_sleep_ms(1);
    }
    if (hooks && hooks->stop) {
        hooks->stop(opts, state, hooks->context);
    }
    return atomic_load(&ctx->runner_saw_stop_after_reset) ? ctx->runner_rc : -5;
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
fake_main_loop_requests_stop_then_waits(const dsd_frontend_host_callbacks* host, void* context) {
    fake_main_thread_ctx* ctx = (fake_main_thread_ctx*)context;
    ctx->main_loop_thread = test_current_thread_id();
    for (int i = 0; i < 1000 && !atomic_load(&ctx->runner_started); i++) {
        dsd_sleep_ms(1);
    }
    if (host && host->request_engine_stop) {
        host->request_engine_stop(host->context);
    }
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
test_main_thread_provider_stop_survives_engine_startup_reset(void) {
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
        .run_main_loop = fake_main_loop_requests_stop_then_waits,
    };
    const dsd_frontend_provider* providers[] = {&fake_main};
    int rc = dsd_cli_frontend_run_from_registry(&opts, &state, providers, 1, fake_resetting_engine_runner, &ctx);
    g_fake_main_thread_context = NULL;

    int failures = 0;
    failures |= expect_int("startup-reset fake run rc", rc, 0);
    failures |= expect_int("startup-reset runner saw durable stop", atomic_load(&ctx.runner_saw_stop_after_reset), 1);
    failures |= expect_int("startup-reset main saw done", atomic_load(&ctx.main_loop_saw_engine_done), 1);
    if (!test_same_thread(ctx.main_loop_thread, ctx.caller_thread)) {
        DSD_FPRINTF(stderr, "startup-reset main loop did not run on caller thread\n");
        failures |= 1;
    }
    if (test_same_thread(ctx.runner_thread, ctx.caller_thread)) {
        DSD_FPRINTF(stderr, "startup-reset runner unexpectedly ran on caller thread\n");
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
    rc |= test_main_thread_provider_stop_survives_engine_startup_reset();
    rc |= test_main_thread_provider_observes_engine_exit_first();
    if (rc == 0) {
        puts("RUNTIME_CLI_FRONTEND_PROVIDER: OK");
    }
    return rc;
}
