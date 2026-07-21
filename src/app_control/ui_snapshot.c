// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/call_state.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "snapshot_internal.h"

static dsd_state g_pub;     // latest published by demod thread
static dsd_state g_consume; // last copied out for UI
// Deep-copied backing for pointer-backed members the UI dereferences.
static Event_History_I g_pub_eh[2];
static Event_History_I g_consume_eh[2];
static dsd_trunk_cc_candidates g_pub_cc_candidates;
static dsd_trunk_cc_candidates g_consume_cc_candidates;
static int g_have = 0;
static dsd_mutex_t g_mu;
static atomic_int g_mu_state = 0; /* 0=uninit, 1=initing, 2=init */
static unsigned long long g_pub_seq = 0;
static unsigned long long g_consume_seq = 0;
static uint64_t g_pub_eh_seq[2] = {0};
static uint64_t g_consume_eh_seq[2] = {0};
static uint64_t g_pub_eh_source_revision[2] = {0};
static const Event_History_I* g_pub_eh_source = NULL;
static int g_pub_eh_present = 0;

#ifdef DSD_NEO_TEST_HOOKS
static dsd_app_snapshot_event_history_copy_counts g_eh_copy_counts;
#define UI_SNAPSHOT_COUNT_SOURCE_COPY(slot)   g_eh_copy_counts.source_to_published[(slot)]++
#define UI_SNAPSHOT_COUNT_CONSUMER_COPY(slot) g_eh_copy_counts.published_to_consumer[(slot)]++
#else
#define UI_SNAPSHOT_COUNT_SOURCE_COPY(slot)   ((void)(slot))
#define UI_SNAPSHOT_COUNT_CONSUMER_COPY(slot) ((void)(slot))
#endif

#define UI_SNAPSHOT_FIELD_END(field) (offsetof(dsd_state, field) + sizeof(((dsd_state*)0)->field))
#define UI_SNAPSHOT_COPY_RANGE(dst, src, first, last)                                                                  \
    ui_snapshot_copy_range((dst), (src), offsetof(dsd_state, first), UI_SNAPSHOT_FIELD_END(last))

static void
ui_snapshot_copy_range(dsd_state* dst, const dsd_state* src, size_t begin, size_t end) {
    DSD_MEMCPY((char*)dst + begin, (const char*)src + begin, end - begin);
}

static void
ui_snapshot_copy_trunk_cc_candidates(dsd_state* dst, const dsd_state* src, dsd_trunk_cc_candidates* backing) {
    const dsd_trunk_cc_candidates* cc_candidates =
        (const dsd_trunk_cc_candidates*)src->state_ext[DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES];
    if (cc_candidates == NULL) {
        DSD_MEMSET(backing, 0, sizeof(*backing));
        dst->state_ext[DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES] = NULL;
        dst->state_ext_cleanup[DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES] = NULL;
        return;
    }

    DSD_MEMCPY(backing, cc_candidates, sizeof(*backing));
    dst->state_ext[DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES] = backing;
    dst->state_ext_cleanup[DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES] = NULL;
}

static void
ui_snapshot_copy_trunk_chan_map(dsd_state* dst, const dsd_state* src) {
    if (dst->trunk_chan_map_seq == src->trunk_chan_map_seq
        && dst->trunk_chan_map_used_count == src->trunk_chan_map_used_count) {
        return;
    }

    uint32_t old_count = dst->trunk_chan_map_used_count;
    if (old_count > DSD_TRUNK_CHAN_MAP_SIZE) {
        old_count = DSD_TRUNK_CHAN_MAP_SIZE;
    }
    for (uint32_t i = 0; i < old_count; i++) {
        const uint16_t channel = dst->trunk_chan_map_used[i];
        if (dsd_state_trunk_chan_tracked(channel)) {
            dst->trunk_chan_map[channel] = 0;
        }
        dst->trunk_chan_map_used[i] = 0;
    }
    dst->trunk_chan_map_used_count = 0;

    uint32_t count = src->trunk_chan_map_used_count;
    if (count > DSD_TRUNK_CHAN_MAP_SIZE) {
        count = DSD_TRUNK_CHAN_MAP_SIZE;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint16_t channel = src->trunk_chan_map_used[i];
        if (!dsd_state_trunk_chan_tracked(channel)) {
            continue;
        }
        const long int freq = src->trunk_chan_map[channel];
        if (freq == 0) {
            continue;
        }
        dst->trunk_chan_map[channel] = freq;
        dst->trunk_chan_map_used[dst->trunk_chan_map_used_count++] = channel;
    }
    dst->trunk_chan_map_seq = src->trunk_chan_map_seq;
}

static void
ui_snapshot_copy_render_state(dsd_state* dst, const dsd_state* src) {
    UI_SNAPSHOT_COPY_RANGE(dst, src, dibit_buf, trunk_lcn_freq);
    ui_snapshot_copy_trunk_chan_map(dst, src);
    (void)dsd_tg_policy_copy_snapshot(dst, src);

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

static void
ensure_mu_init(void) {
    if (atomic_load(&g_mu_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_mu_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_mu);
        atomic_store(&g_mu_state, 2);
        return;
    }
    while (atomic_load(&g_mu_state) != 2) {}
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_app_snapshot_test_reset_event_history_copy_counts(void) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    DSD_MEMSET(&g_eh_copy_counts, 0, sizeof(g_eh_copy_counts));
    dsd_mutex_unlock(&g_mu);
}

void
dsd_app_snapshot_test_get_event_history_copy_counts(dsd_app_snapshot_event_history_copy_counts* counts) {
    if (counts == NULL) {
        return;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    *counts = g_eh_copy_counts;
    dsd_mutex_unlock(&g_mu);
}
#endif

void
dsd_app_telemetry_publish_snapshot(const dsd_state* state) {
    if (!state) {
        return;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    ui_snapshot_copy_render_state(&g_pub, state);
    ui_snapshot_copy_trunk_cc_candidates(&g_pub, state, &g_pub_cc_candidates);
    // Clone canonical calls, recent activity, and history under the core transaction lock.
    if (state->event_history_s != NULL) {
        const int force_copy = !g_have || !g_pub_eh_present || g_pub_eh_source != state->event_history_s;
        uint8_t copied[2] = {0U, 0U};
        (void)dsd_event_state_copy_snapshot_incremental(&g_pub, state, g_pub_eh, g_pub_eh_source_revision, force_copy,
                                                        copied);
        for (size_t slot = 0; slot < 2U; slot++) {
            if (copied[slot]) {
                g_pub_eh_source_revision[slot] = g_pub_eh[slot].revision;
                g_pub_eh_seq[slot]++;
                UI_SNAPSHOT_COUNT_SOURCE_COPY(slot);
            }
        }
        g_pub_eh_source = state->event_history_s;
        g_pub_eh_present = 1;
        g_pub.event_history_s = g_pub_eh;
    } else {
        (void)dsd_call_state_copy_to_state(&g_pub, state);
        g_pub_eh_source = NULL;
        g_pub_eh_present = 0;
        g_pub.event_history_s = NULL;
    }
    g_have = 1;
    g_pub_seq++;
    dsd_mutex_unlock(&g_mu);
}

const dsd_state*
dsd_app_get_latest_snapshot(void) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    if (!g_have) {
        dsd_mutex_unlock(&g_mu);
        return NULL;
    }
    if (g_consume_seq != g_pub_seq) {
        ui_snapshot_copy_render_state(&g_consume, &g_pub);
        ui_snapshot_copy_trunk_cc_candidates(&g_consume, &g_pub, &g_consume_cc_candidates);
        (void)dsd_call_state_copy_to_state(&g_consume, &g_pub);
        g_consume_seq = g_pub_seq;
    }
    // Deep copy event history only when the published history changed.
    if (g_pub.event_history_s != NULL) {
        for (size_t slot = 0; slot < 2U; slot++) {
            if (g_consume_eh_seq[slot] != g_pub_eh_seq[slot]) {
                DSD_MEMCPY(&g_consume_eh[slot], &g_pub_eh[slot], sizeof(Event_History_I));
                g_consume_eh_seq[slot] = g_pub_eh_seq[slot];
                UI_SNAPSHOT_COUNT_CONSUMER_COPY(slot);
            }
        }
        g_consume.event_history_s = g_consume_eh;
    } else {
        g_consume.event_history_s = NULL;
    }
    dsd_mutex_unlock(&g_mu);
    return &g_consume;
}
