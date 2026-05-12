// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/ui/ui_snapshot.h>
#include <string.h>

#include "dsd-neo/core/state_fwd.h"
#include "telemetry_hooks_impl.h"

static dsd_state g_pub;     // latest published by demod thread
static dsd_state g_consume; // last copied out for UI
// Deep-copied backing for pointer-backed members the UI dereferences.
// Currently, only event history is pointer-backed in dsd_state for UI reads.
static Event_History_I g_pub_eh[2];
static Event_History_I g_consume_eh[2];
static int g_have = 0;
static dsd_mutex_t g_mu;
static atomic_int g_mu_init = 0;
static unsigned long long g_pub_seq = 0;
static unsigned long long g_consume_seq = 0;
static unsigned long long g_pub_eh_seq = 0;
static unsigned long long g_consume_eh_seq = 0;

static int
ui_event_history_item_equal(const Event_History* lhs, const Event_History* rhs) {
    if (lhs->write != rhs->write || lhs->color_pair != rhs->color_pair || lhs->systype != rhs->systype
        || lhs->subtype != rhs->subtype || lhs->sys_id1 != rhs->sys_id1 || lhs->sys_id2 != rhs->sys_id2
        || lhs->sys_id3 != rhs->sys_id3 || lhs->sys_id4 != rhs->sys_id4 || lhs->sys_id5 != rhs->sys_id5
        || lhs->gi != rhs->gi || lhs->enc != rhs->enc || lhs->enc_alg != rhs->enc_alg || lhs->enc_key != rhs->enc_key
        || lhs->mi != rhs->mi || lhs->svc != rhs->svc || lhs->source_id != rhs->source_id
        || lhs->target_id != rhs->target_id || lhs->channel != rhs->channel || lhs->event_time != rhs->event_time) {
        return 0;
    }
    return memcmp(lhs->src_str, rhs->src_str, sizeof(lhs->src_str)) == 0
           && memcmp(lhs->tgt_str, rhs->tgt_str, sizeof(lhs->tgt_str)) == 0
           && memcmp(lhs->t_name, rhs->t_name, sizeof(lhs->t_name)) == 0
           && memcmp(lhs->s_name, rhs->s_name, sizeof(lhs->s_name)) == 0
           && memcmp(lhs->t_mode, rhs->t_mode, sizeof(lhs->t_mode)) == 0
           && memcmp(lhs->s_mode, rhs->s_mode, sizeof(lhs->s_mode)) == 0
           && memcmp(lhs->pdu, rhs->pdu, sizeof(lhs->pdu)) == 0
           && memcmp(lhs->sysid_string, rhs->sysid_string, sizeof(lhs->sysid_string)) == 0
           && memcmp(lhs->alias, rhs->alias, sizeof(lhs->alias)) == 0
           && memcmp(lhs->gps_s, rhs->gps_s, sizeof(lhs->gps_s)) == 0
           && memcmp(lhs->text_message, rhs->text_message, sizeof(lhs->text_message)) == 0
           && memcmp(lhs->event_string, rhs->event_string, sizeof(lhs->event_string)) == 0
           && memcmp(lhs->internal_str, rhs->internal_str, sizeof(lhs->internal_str)) == 0;
}

static int
ui_event_history_slot_equal(const Event_History_I* lhs, const Event_History_I* rhs) {
    const size_t item_count = sizeof(lhs->Event_History_Items) / sizeof(lhs->Event_History_Items[0]);
    for (size_t i = 0; i < item_count; i++) {
        if (!ui_event_history_item_equal(&lhs->Event_History_Items[i], &rhs->Event_History_Items[i])) {
            return 0;
        }
    }
    return 1;
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
    // Compare fields instead of raw struct bytes so padding cannot affect detection.
    if (state->event_history_s != NULL) {
        int eh_changed = 0;
        if (!g_have || !ui_event_history_slot_equal(&g_pub_eh[0], &state->event_history_s[0])) {
            memcpy(&g_pub_eh[0], &state->event_history_s[0], sizeof(Event_History_I));
            eh_changed = 1;
        }
        if (!g_have || !ui_event_history_slot_equal(&g_pub_eh[1], &state->event_history_s[1])) {
            memcpy(&g_pub_eh[1], &state->event_history_s[1], sizeof(Event_History_I));
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
