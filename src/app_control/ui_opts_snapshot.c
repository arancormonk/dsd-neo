// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <stddef.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "recording_view_internal.h"
#include "snapshot_internal.h"

static dsd_opts g_pub_opts;     // latest published
static dsd_opts g_consume_opts; // last copied out for UI
static int g_have_opts = 0;
static dsd_mutex_t g_opts_mu;
static atomic_int g_opts_mu_init = 0;
static unsigned long long g_pub_opts_seq = 0;
static unsigned long long g_consume_opts_seq = 0;

/* 0 = uninitialized, 1 = initialization in flight, 2 = ready. The loser of the
 * first-call race must wait: taking a mutex another thread has not finished
 * initializing is undefined behavior. */
static void
ensure_opts_mu_init(void) {
    if (atomic_load(&g_opts_mu_init) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_opts_mu_init, &expected, 1)) {
        (void)dsd_mutex_init(&g_opts_mu);
        atomic_store(&g_opts_mu_init, 2);
        return;
    }
    while (atomic_load(&g_opts_mu_init) != 2) {
        dsd_thread_yield();
    }
}

void
dsd_app_telemetry_publish_opts_snapshot(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    ensure_opts_mu_init();
    dsd_mutex_lock(&g_opts_mu);
    DSD_MEMCPY(&g_pub_opts, opts, sizeof(dsd_opts));
    /* The API shared secret must never reach a snapshot consumer. */
    DSD_SECURE_ZERO(g_pub_opts.api_token, sizeof g_pub_opts.api_token);
    g_pub_opts_seq++;
    g_have_opts = 1;
    dsd_mutex_unlock(&g_opts_mu);
    /* Outside this module's lock on purpose, matching ui_snapshot.c: the notification
       publisher takes its own, and nesting the two would put a lock-order edge between
       the snapshot path and a JNI poll. */
    dsd_app_notification_publish_opts(opts);
    /* The recording view goes out with each options snapshot, so a command reads as applied together with the mode it
       left (issue #708). */
    dsd_app_recording_view_publish(opts);
    dsd_app_telemetry_notify_opts(opts);
}

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    ensure_opts_mu_init();
    dsd_mutex_lock(&g_opts_mu);
    if (!g_have_opts) {
        dsd_mutex_unlock(&g_opts_mu);
        return NULL;
    }
    if (g_consume_opts_seq != g_pub_opts_seq) {
        DSD_MEMCPY(&g_consume_opts, &g_pub_opts, sizeof(dsd_opts));
        g_consume_opts_seq = g_pub_opts_seq;
    }
    dsd_mutex_unlock(&g_opts_mu);
    return &g_consume_opts;
}

void
dsd_app_opts_snapshot_history_meta(dsd_app_history_tap_meta* meta) {
    if (meta == NULL) {
        return;
    }
    ensure_opts_mu_init();
    dsd_mutex_lock(&g_opts_mu);
    /* g_pub_opts only: the consumer's copy and its sequence stay the frontend's own. */
    meta->opts_published = g_have_opts;
    meta->scanner_mode = g_have_opts ? g_pub_opts.scanner_mode : 0;
    meta->trunk_scan_enabled = g_have_opts ? g_pub_opts.trunk_scan_enabled : 0;
    dsd_mutex_unlock(&g_opts_mu);
}
