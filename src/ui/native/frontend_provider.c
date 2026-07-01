// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/ui/native_provider.h>

#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/log.h>

// cppcheck-suppress-begin constParameterCallback
static int
dsd_native_frontend_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    dsd_app_frontend_runtime_start(opts, state);
    return 0;
}

// cppcheck-suppress-end constParameterCallback

static void
dsd_native_frontend_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    dsd_app_frontend_runtime_stop();
}

static int
dsd_native_frontend_prepare(const dsd_opts* opts, const dsd_state* state, dsd_engine_lifecycle_hooks* out) {
    if (!opts || !out) {
        return -1;
    }
    (void)state;
    *out = (dsd_engine_lifecycle_hooks){
        .start = dsd_native_frontend_start,
        .stop = dsd_native_frontend_stop,
        .context = NULL,
    };
    return 0;
}

static int
dsd_native_frontend_run_main_loop(const dsd_frontend_host_callbacks* host, void* context) {
    (void)context;
    if (!host || !host->engine_finished) {
        return -1;
    }

    LOG_NOTICE("Native frontend scaffold only; no native GUI is rendered yet.\n");

    uint64_t last_event_history_sequence = 0;
    while (!host->engine_finished(host->context)) {
        dsd_frontend_snapshot snapshot;
        (void)dsd_app_frontend_redraw_consume();
        if (dsd_app_frontend_snapshot_get(&snapshot) == 0) {
            if (snapshot.event_history_present && snapshot.event_history_sequence != last_event_history_sequence) {
                dsd_frontend_event_history_summary items[8];
                dsd_frontend_event_history_query query = {
                    .slot = 0,
                    .offset = 0,
                    .limit = sizeof items / sizeof items[0],
                    .known_sequence = last_event_history_sequence,
                };
                dsd_frontend_event_history_page_info info = {0};
                if (dsd_app_frontend_event_history_page_get(&query, items, sizeof items / sizeof items[0], &info)
                    == 0) {
                    last_event_history_sequence = info.sequence;
                }
            }
        }
        dsd_sleep_ms(33);
    }

    return 0;
}

const dsd_frontend_provider*
dsd_native_frontend_provider(void) {
    static const dsd_frontend_provider provider = {
        .kind = DSD_FRONTEND_NATIVE,
        .name = "native",
        .prepare = dsd_native_frontend_prepare,
        .flags = DSD_FRONTEND_PROVIDER_MAIN_THREAD_UI,
        .run_main_loop = dsd_native_frontend_run_main_loop,
    };
    return &provider;
}
