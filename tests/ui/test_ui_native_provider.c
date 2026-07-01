// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/ui/native_provider.h>
#include <stdarg.h>
#include <stdio.h>

static int g_runtime_start_calls;
static int g_runtime_stop_calls;
static int g_snapshot_calls;
static int g_sleep_calls;
static int g_log_calls;
static int g_engine_finished_calls;
static int g_engine_stop_requests;

void
dsd_app_frontend_runtime_start(const dsd_opts* initial_opts, const dsd_state* initial_state) {
    (void)initial_opts;
    (void)initial_state;
    g_runtime_start_calls++;
}

void
dsd_app_frontend_runtime_stop(void) {
    g_runtime_stop_calls++;
}

int
dsd_app_frontend_redraw_consume(void) {
    return 0;
}

int
dsd_app_frontend_snapshot_get(dsd_frontend_snapshot* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof *out);
    }
    g_snapshot_calls++;
    return 0;
}

int
dsd_app_frontend_event_history_page_get(const dsd_frontend_event_history_query* query,
                                        dsd_frontend_event_history_summary* out_items, size_t max_items,
                                        dsd_frontend_event_history_page_info* out_info) {
    (void)query;
    (void)out_items;
    (void)max_items;
    if (out_info) {
        DSD_MEMSET(out_info, 0, sizeof *out_info);
    }
    return 0;
}

void
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
    g_sleep_calls++;
}

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
    va_list ap;
    va_start(ap, format);
    va_end(ap);
    g_log_calls++;
}

static int
host_engine_finished(void* context) {
    (void)context;
    g_engine_finished_calls++;
    return g_engine_finished_calls >= 3;
}

static void
host_request_engine_stop(void* context) {
    (void)context;
    g_engine_stop_requests++;
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
expect_true(const char* label, int condition) {
    if (!condition) {
        DSD_FPRINTF(stderr, "%s: expectation failed\n", label);
        return 1;
    }
    return 0;
}

int
main(void) {
    const dsd_frontend_provider* provider = dsd_native_frontend_provider();
    int rc = 0;
    rc |= expect_true("provider exists", provider != NULL);
    rc |= expect_int("provider kind", provider ? (int)provider->kind : -1, DSD_FRONTEND_NATIVE);
    rc |= expect_true("provider is main-thread", provider && (provider->flags & DSD_FRONTEND_PROVIDER_MAIN_THREAD_UI));
    rc |= expect_true("provider has main loop", provider && provider->run_main_loop != NULL);

    dsd_engine_lifecycle_hooks hooks = {0};
    rc |= expect_int("prepare", provider->prepare((const dsd_opts*)1, (const dsd_state*)2, &hooks), 0);
    rc |= expect_true("start hook", hooks.start != NULL);
    rc |= expect_true("stop hook", hooks.stop != NULL);

    rc |= expect_int("start rc", hooks.start((dsd_opts*)1, (dsd_state*)2, hooks.context), 0);
    hooks.stop((dsd_opts*)1, (dsd_state*)2, hooks.context);
    rc |= expect_int("runtime start calls", g_runtime_start_calls, 1);
    rc |= expect_int("runtime stop calls", g_runtime_stop_calls, 1);

    const dsd_frontend_host_callbacks host = {
        .context = NULL,
        .engine_finished = host_engine_finished,
        .request_engine_stop = host_request_engine_stop,
    };
    rc |= expect_int("main loop", provider->run_main_loop(&host, hooks.context), 0);
    rc |= expect_int("engine stop requests", g_engine_stop_requests, 0);
    rc |= expect_int("engine finished polls", g_engine_finished_calls, 3);
    rc |= expect_true("snapshot calls", g_snapshot_calls >= 2);
    rc |= expect_true("sleep calls", g_sleep_calls >= 2);
    rc |= expect_true("log calls", g_log_calls >= 1);
    return rc;
}
