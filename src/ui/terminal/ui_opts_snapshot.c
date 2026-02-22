// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/ui/ui_opts_snapshot.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "telemetry_hooks_impl.h"

static dsd_opts g_pub_opts;     // latest published
static dsd_opts g_consume_opts; // last copied out for UI
static int g_have_opts = 0;
static dsd_mutex_t g_opts_mu;
static atomic_int g_opts_mu_init = 0;
static unsigned long long g_pub_opts_seq = 0;
static unsigned long long g_consume_opts_seq = 0;
static uint64_t g_pub_opts_hash = 0;

static uint64_t
fnv1a64_bytes(const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void
ensure_opts_mu_init(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_opts_mu_init, &expected, 1)) {
        dsd_mutex_init(&g_opts_mu);
    }
}

void
ui_terminal_telemetry_publish_opts_snapshot(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    ensure_opts_mu_init();
    dsd_mutex_lock(&g_opts_mu);
    uint64_t new_hash = fnv1a64_bytes(opts, sizeof(dsd_opts));
    if (!g_have_opts || new_hash != g_pub_opts_hash) {
        memcpy(&g_pub_opts, opts, sizeof(dsd_opts));
        g_pub_opts_hash = new_hash;
        g_pub_opts_seq++;
        g_have_opts = 1;
    }
    dsd_mutex_unlock(&g_opts_mu);
}

const dsd_opts*
ui_get_latest_opts_snapshot(void) {
    ensure_opts_mu_init();
    dsd_mutex_lock(&g_opts_mu);
    if (!g_have_opts) {
        dsd_mutex_unlock(&g_opts_mu);
        return NULL;
    }
    if (g_consume_opts_seq != g_pub_opts_seq) {
        memcpy(&g_consume_opts, &g_pub_opts, sizeof(dsd_opts));
        g_consume_opts_seq = g_pub_opts_seq;
    }
    dsd_mutex_unlock(&g_opts_mu);
    return &g_consume_opts;
}
