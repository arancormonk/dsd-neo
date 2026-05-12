// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/ui/ui_snapshot.h>
#include <stddef.h>
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

#define UI_SNAPSHOT_FIELD_END(field)            (offsetof(dsd_state, field) + sizeof(((dsd_state*)0)->field))
#define UI_SNAPSHOT_COPY_FIELD(dst, src, field) memcpy(&(dst)->field, &(src)->field, sizeof((dst)->field))
#define UI_SNAPSHOT_COPY_RANGE(dst, src, first, last)                                                                  \
    ui_snapshot_copy_range((dst), (src), offsetof(dsd_state, first), UI_SNAPSHOT_FIELD_END(last))

static void
ui_snapshot_copy_range(dsd_state* dst, const dsd_state* src, size_t begin, size_t end) {
    memcpy((char*)dst + begin, (const char*)src + begin, end - begin);
}

static void
ui_snapshot_copy_render_state(dsd_state* dst, const dsd_state* src) {
    UI_SNAPSHOT_COPY_RANGE(dst, src, dibit_buf, trunk_lcn_freq);
    UI_SNAPSHOT_COPY_FIELD(dst, src, trunk_chan_map);
    UI_SNAPSHOT_COPY_FIELD(dst, src, group_array);

    UI_SNAPSHOT_COPY_RANGE(dst, src, audio_out_idx, lastsample);
    UI_SNAPSHOT_COPY_RANGE(dst, src, err_str, aout_gainA);
    UI_SNAPSHOT_COPY_RANGE(dst, src, aout_max_buf_idx, last_dibit);

    UI_SNAPSHOT_COPY_RANGE(dst, src, input_sample_buffer, directmode);
    UI_SNAPSHOT_COPY_RANGE(dst, src, dmr_alias_format, DMRvcR);
    UI_SNAPSHOT_COPY_RANGE(dst, src, octet_counter, p25_p2_audio_allowed);
    UI_SNAPSHOT_COPY_RANGE(dst, src, p25_p2_audio_ring_count, dstar_gps);
    UI_SNAPSHOT_COPY_RANGE(dst, src, m17_pbc_ct, straight_frame_step);
    UI_SNAPSHOT_COPY_RANGE(dst, src, vertex_ks_count, ui_msg);
}

static int
ui_event_history_slot_equal(const Event_History_I* lhs, const Event_History_I* rhs) {
    // Event history storage is calloc-initialized and copied whole when changed, so padding stays stable here.
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
    return memcmp(lhs, rhs, sizeof(*lhs)) == 0;
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
    ui_snapshot_copy_render_state(&g_pub, state);
    // Deep copy pointer-backed UI data (event history for 2 slots) only when changed.
    // Event history storage is zero-initialized and copied as a whole slot.
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
        ui_snapshot_copy_render_state(&g_consume, &g_pub);
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
