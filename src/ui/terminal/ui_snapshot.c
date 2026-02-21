// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/ui/ui_snapshot.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/state_fwd.h"
#include "telemetry_hooks_impl.h"

static dsd_state g_pub;     // latest published by demod thread
static dsd_state g_consume; // last copied out for UI
// Deep-copied backing for pointer-backed members the UI dereferences.
// Currently, only event history is pointer-backed in dsd_state for UI reads.
static Event_History_I g_pub_eh[2];
static Event_History_I g_consume_eh[2];
// Full-slot fingerprints to avoid deep-copying full history when unchanged.
static uint64_t g_pub_eh_hash[2];
static int g_have = 0;
static dsd_mutex_t g_mu;
static atomic_int g_mu_init = 0;
static unsigned long long g_pub_seq = 0;
static unsigned long long g_consume_seq = 0;
static unsigned long long g_pub_eh_seq = 0;
static unsigned long long g_consume_eh_seq = 0;

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
ensure_mu_init(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_mu_init, &expected, 1)) {
        dsd_mutex_init(&g_mu);
    }
}

void
ui_terminal_telemetry_publish_snapshot(const dsd_state* state) {
    if (!state) {
        return;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    // Coarse copy of the entire struct first
    memcpy(&g_pub, state, sizeof(dsd_state));
    // Deep copy pointer-backed UI data (event history for 2 slots) only when changed.
    // Fingerprint full slots so non-head updates (for example history reset) are not missed.
    if (state->event_history_s != NULL) {
        int eh_changed = 0;
        uint64_t slot0_hash = fnv1a64_bytes(&state->event_history_s[0], sizeof(Event_History_I));
        uint64_t slot1_hash = fnv1a64_bytes(&state->event_history_s[1], sizeof(Event_History_I));
        if (!g_have || slot0_hash != g_pub_eh_hash[0]) {
            memcpy(&g_pub_eh[0], &state->event_history_s[0], sizeof(Event_History_I));
            g_pub_eh_hash[0] = slot0_hash;
            eh_changed = 1;
        }
        if (!g_have || slot1_hash != g_pub_eh_hash[1]) {
            memcpy(&g_pub_eh[1], &state->event_history_s[1], sizeof(Event_History_I));
            g_pub_eh_hash[1] = slot1_hash;
            eh_changed = 1;
        }
        if (eh_changed) {
            g_pub_eh_seq++;
        }
        g_pub.event_history_s = g_pub_eh;
    } else {
        g_pub.event_history_s = NULL;
    }
    g_have = 1;
    g_pub_seq++;
    dsd_mutex_unlock(&g_mu);
}

const dsd_state*
ui_get_latest_snapshot(void) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    if (!g_have) {
        dsd_mutex_unlock(&g_mu);
        return NULL;
    }
    if (g_consume_seq != g_pub_seq) {
        // Copy the coarse snapshot only when publisher has new data.
        memcpy(&g_consume, &g_pub, sizeof(dsd_state));
        g_consume_seq = g_pub_seq;
    }
    // Deep copy event history only when the published history changed.
    if (g_pub.event_history_s != NULL) {
        if (g_consume_eh_seq != g_pub_eh_seq) {
            memcpy(&g_consume_eh[0], &g_pub_eh[0], sizeof(Event_History_I));
            memcpy(&g_consume_eh[1], &g_pub_eh[1], sizeof(Event_History_I));
            g_consume_eh_seq = g_pub_eh_seq;
        }
        g_consume.event_history_s = g_consume_eh;
    } else {
        g_consume.event_history_s = NULL;
    }
    dsd_mutex_unlock(&g_mu);
    return &g_consume;
}
