// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/ui/ui_opts_snapshot.h>

#include <pthread.h>
#include <string.h>

static dsd_opts g_pub_opts;     // latest published
static dsd_opts g_consume_opts; // last copied out for UI
static int g_have_opts = 0;
static pthread_mutex_t g_opts_mu = PTHREAD_MUTEX_INITIALIZER;

void
ui_publish_opts_snapshot(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    pthread_mutex_lock(&g_opts_mu);
    memcpy(&g_pub_opts, opts, sizeof(dsd_opts));
    g_have_opts = 1;
    pthread_mutex_unlock(&g_opts_mu);
}

const dsd_opts*
ui_get_latest_opts_snapshot(void) {
    pthread_mutex_lock(&g_opts_mu);
    if (!g_have_opts) {
        pthread_mutex_unlock(&g_opts_mu);
        return NULL;
    }
    memcpy(&g_consume_opts, &g_pub_opts, sizeof(dsd_opts));
    pthread_mutex_unlock(&g_opts_mu);
    return &g_consume_opts;
}
