// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/history_tap.h>
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
/* The session ended (dsd_app_opts_snapshot_wipe_keys()): the consumer's next read wipes the API keys its buffer
   holds. */
static int g_consume_opts_wipe_keys = 0;
/* The consumer's buffer lost its API keys while the published copy kept them: its next read copies it again. */
static int g_consume_opts_stale = 0;
/* The reader keeps no API keys in its buffer for now (dsd_app_opts_snapshot_consumer_withhold_keys()). */
static int g_consume_opts_keys_withheld = 0;
/* A publication the P25 SM tick guard put off (dsd_app_telemetry_opts_snapshot_pending()). */
static atomic_int g_pub_opts_pending = 0;

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

/* The secrets an options copy holds: the rdio-scanner API key and URL (a URL can carry credentials) and the
   RadioReference API key, which a frontend reads from its copy (the terminal UI's RadioReference panel and its config
   save), and the API token, which never reaches one. */
static void
ui_opts_snapshot_wipe_key_copies(dsd_opts* snap) {
    DSD_SECURE_ZERO(snap->rdio_api_key, sizeof snap->rdio_api_key);
    DSD_SECURE_ZERO(snap->rdio_api_url, sizeof snap->rdio_api_url);
    DSD_SECURE_ZERO(snap->rr_app_key, sizeof snap->rr_app_key);
    DSD_SECURE_ZERO(snap->api_token, sizeof snap->api_token);
}

static void
ui_opts_snapshot_publish(const dsd_opts* opts) {
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

void
dsd_app_telemetry_publish_opts_snapshot(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    /* The P25 watchdog's tunes write the options too (trunk_is_tuned, the tuner's centre frequency) under the tick
       guard, so the copy holds it, and a tick in progress puts it off, as the state snapshot's does (ui_snapshot.c). */
    const dsd_app_publish_guard guard = dsd_app_publish_guard_try();
    if (guard == DSD_APP_PUBLISH_GUARD_BUSY) {
        atomic_store(&g_pub_opts_pending, 1);
        return;
    }
    atomic_store(&g_pub_opts_pending, 0);
    ui_opts_snapshot_publish(opts);
    dsd_app_publish_guard_leave(guard);
}

int
dsd_app_telemetry_opts_snapshot_pending(void) {
    return atomic_load(&g_pub_opts_pending);
}

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    ensure_opts_mu_init();
    dsd_mutex_lock(&g_opts_mu);
    if (g_consume_opts_wipe_keys) {
        ui_opts_snapshot_wipe_key_copies(&g_consume_opts);
        g_consume_opts_wipe_keys = 0;
    }
    if (!g_have_opts) {
        dsd_mutex_unlock(&g_opts_mu);
        return NULL;
    }
    if (g_consume_opts_seq != g_pub_opts_seq || g_consume_opts_stale) {
        DSD_MEMCPY(&g_consume_opts, &g_pub_opts, sizeof(dsd_opts));
        g_consume_opts_seq = g_pub_opts_seq;
        g_consume_opts_stale = 0;
        if (g_consume_opts_keys_withheld) {
            ui_opts_snapshot_wipe_key_copies(&g_consume_opts);
        }
    }
    dsd_mutex_unlock(&g_opts_mu);
    return &g_consume_opts;
}

void
dsd_app_opts_snapshot_wipe_keys(void) {
    ensure_opts_mu_init();
    dsd_mutex_lock(&g_opts_mu);
    ui_opts_snapshot_wipe_key_copies(&g_pub_opts);
    /* The consumer's buffer is read without the lock on its own thread (snapshot.h), so that thread wipes it, at its
       next read. */
    g_consume_opts_wipe_keys = 1;
    dsd_mutex_unlock(&g_opts_mu);
}

void
dsd_app_opts_snapshot_consumer_withhold_keys(int withhold) {
    ensure_opts_mu_init();
    dsd_mutex_lock(&g_opts_mu);
    g_consume_opts_keys_withheld = withhold != 0;
    if (g_consume_opts_keys_withheld) {
        ui_opts_snapshot_wipe_key_copies(&g_consume_opts);
    }
    g_consume_opts_stale = 1;
    dsd_mutex_unlock(&g_opts_mu);
}

void
dsd_app_opts_snapshot_consumer_wipe_keys(void) {
    ensure_opts_mu_init();
    dsd_mutex_lock(&g_opts_mu);
    ui_opts_snapshot_wipe_key_copies(&g_consume_opts);
    g_consume_opts_wipe_keys = 0;
    g_consume_opts_stale = 1;
    dsd_mutex_unlock(&g_opts_mu);
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
    dsd_mutex_unlock(&g_opts_mu);
}
