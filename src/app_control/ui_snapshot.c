// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frontend_internal.h"
#include "snapshot_internal.h"
#include "telemetry_hooks_impl.h"

static dsd_state g_pub;     // latest published by demod thread
static dsd_state g_consume; // last copied out for UI
// Deep-copied backing for pointer-backed members the UI dereferences.
static Event_History_I g_pub_eh[2];
static Event_History_I g_consume_eh[2];
static dsd_trunk_cc_candidates g_pub_cc_candidates;
static dsd_trunk_cc_candidates g_consume_cc_candidates;
static int g_have = 0;
static dsd_mutex_t g_mu;
static atomic_int g_mu_init = 0;
static unsigned long long g_pub_seq = 0;
static unsigned long long g_consume_seq = 0;
static unsigned long long g_pub_eh_seq = 0;
static unsigned long long g_consume_eh_seq = 0;

#define UI_SNAPSHOT_FIELD_END(field)            (offsetof(dsd_state, field) + sizeof(((dsd_state*)0)->field))
#define UI_SNAPSHOT_COPY_FIELD(dst, src, field) DSD_MEMCPY(&(dst)->field, &(src)->field, sizeof((dst)->field))
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

static int
ui_event_history_item_equal(const Event_History* lhs, const Event_History* rhs) {
    if (lhs == NULL || rhs == NULL) {
        return 0;
    }

    const int scalar_checks[] = {
        lhs->write == rhs->write,
        lhs->color_pair == rhs->color_pair,
        lhs->systype == rhs->systype,
        lhs->subtype == rhs->subtype,
        lhs->sys_id1 == rhs->sys_id1,
        lhs->sys_id2 == rhs->sys_id2,
        lhs->sys_id3 == rhs->sys_id3,
        lhs->sys_id4 == rhs->sys_id4,
        lhs->sys_id5 == rhs->sys_id5,
        lhs->gi == rhs->gi,
        lhs->enc == rhs->enc,
        lhs->enc_alg == rhs->enc_alg,
        lhs->enc_key == rhs->enc_key,
        lhs->mi == rhs->mi,
        lhs->svc == rhs->svc,
        lhs->source_id == rhs->source_id,
        lhs->target_id == rhs->target_id,
        lhs->channel == rhs->channel,
        lhs->event_time == rhs->event_time,
    };
    const size_t scalar_count = sizeof(scalar_checks) / sizeof(scalar_checks[0]);
    for (size_t i = 0; i < scalar_count; i++) {
        if (!scalar_checks[i]) {
            return 0;
        }
    }

    struct UiByteSpan {
        const void* left;
        const void* right;
        size_t size;
    };
    const struct UiByteSpan spans[] = {
        {lhs->src_str, rhs->src_str, sizeof(lhs->src_str)},
        {lhs->tgt_str, rhs->tgt_str, sizeof(lhs->tgt_str)},
        {lhs->t_name, rhs->t_name, sizeof(lhs->t_name)},
        {lhs->s_name, rhs->s_name, sizeof(lhs->s_name)},
        {lhs->t_mode, rhs->t_mode, sizeof(lhs->t_mode)},
        {lhs->s_mode, rhs->s_mode, sizeof(lhs->s_mode)},
        {lhs->pdu, rhs->pdu, sizeof(lhs->pdu)},
        {lhs->sysid_string, rhs->sysid_string, sizeof(lhs->sysid_string)},
        {lhs->alias, rhs->alias, sizeof(lhs->alias)},
        {lhs->gps_s, rhs->gps_s, sizeof(lhs->gps_s)},
        {lhs->text_message, rhs->text_message, sizeof(lhs->text_message)},
        {lhs->event_string, rhs->event_string, sizeof(lhs->event_string)},
        {lhs->internal_str, rhs->internal_str, sizeof(lhs->internal_str)},
    };
    const size_t span_count = sizeof(spans) / sizeof(spans[0]);
    for (size_t i = 0; i < span_count; i++) {
        if (memcmp(spans[i].left, spans[i].right, spans[i].size) != 0) {
            return 0;
        }
    }
    return 1;
}

static int
ui_event_history_slot_equal(const Event_History_I* lhs, const Event_History_I* rhs) {
    if (lhs == NULL || rhs == NULL) {
        return 0;
    }
    const size_t count = sizeof(lhs->Event_History_Items) / sizeof(lhs->Event_History_Items[0]);
    for (size_t i = 0; i < count; i++) {
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
dsd_app_telemetry_publish_snapshot(const dsd_state* state) {
    if (!state) {
        return;
    }
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    ui_snapshot_copy_render_state(&g_pub, state);
    ui_snapshot_copy_trunk_cc_candidates(&g_pub, state, &g_pub_cc_candidates);
    // Deep copy pointer-backed UI data (event history for 2 slots) only when changed.
    // Event history storage is zero-initialized and copied as a whole slot.
    if (state->event_history_s != NULL) {
        int eh_changed = 0;
        if (!g_have || !ui_event_history_slot_equal(&g_pub_eh[0], &state->event_history_s[0])) {
            DSD_MEMCPY(&g_pub_eh[0], &state->event_history_s[0], sizeof(Event_History_I));
            eh_changed = 1;
        }
        if (!g_have || !ui_event_history_slot_equal(&g_pub_eh[1], &state->event_history_s[1])) {
            DSD_MEMCPY(&g_pub_eh[1], &state->event_history_s[1], sizeof(Event_History_I));
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
        g_consume_seq = g_pub_seq;
    }
    // Deep copy event history only when the published history changed.
    if (g_pub.event_history_s != NULL) {
        if (g_consume_eh_seq != g_pub_eh_seq) {
            DSD_MEMCPY(&g_consume_eh[0], &g_pub_eh[0], sizeof(Event_History_I));
            DSD_MEMCPY(&g_consume_eh[1], &g_pub_eh[1], sizeof(Event_History_I));
            g_consume_eh_seq = g_pub_eh_seq;
        }
        g_consume.event_history_s = g_consume_eh;
    } else {
        g_consume.event_history_s = NULL;
    }
    dsd_mutex_unlock(&g_mu);
    return &g_consume;
}

void
ui_terminal_telemetry_publish_snapshot(const dsd_state* state) {
    dsd_app_telemetry_publish_snapshot(state);
}

const dsd_state*
ui_get_latest_snapshot(void) {
    return dsd_app_get_latest_snapshot();
}

static void
frontend_event_history_item_copy(dsd_frontend_event_history_item* dst, const Event_History* src) {
    if (!dst || !src) {
        return;
    }
    dst->write = src->write;
    dst->color_pair = src->color_pair;
    dst->systype = src->systype;
    dst->subtype = src->subtype;
    dst->sys_id1 = src->sys_id1;
    dst->sys_id2 = src->sys_id2;
    dst->sys_id3 = src->sys_id3;
    dst->sys_id4 = src->sys_id4;
    dst->sys_id5 = src->sys_id5;
    dst->gi = src->gi;
    dst->enc = src->enc;
    dst->enc_alg = src->enc_alg;
    dst->enc_key = src->enc_key;
    dst->mi = src->mi;
    dst->svc = src->svc;
    dst->source_id = src->source_id;
    dst->target_id = src->target_id;
    DSD_MEMCPY(dst->src_str, src->src_str, sizeof dst->src_str);
    DSD_MEMCPY(dst->tgt_str, src->tgt_str, sizeof dst->tgt_str);
    DSD_MEMCPY(dst->t_name, src->t_name, sizeof dst->t_name);
    DSD_MEMCPY(dst->s_name, src->s_name, sizeof dst->s_name);
    DSD_MEMCPY(dst->t_mode, src->t_mode, sizeof dst->t_mode);
    DSD_MEMCPY(dst->s_mode, src->s_mode, sizeof dst->s_mode);
    dst->channel = src->channel;
    dst->event_time = (int64_t)src->event_time;
    DSD_MEMCPY(dst->pdu, src->pdu, sizeof dst->pdu);
    DSD_MEMCPY(dst->sysid_string, src->sysid_string, sizeof dst->sysid_string);
    DSD_MEMCPY(dst->alias, src->alias, sizeof dst->alias);
    DSD_MEMCPY(dst->gps_s, src->gps_s, sizeof dst->gps_s);
    DSD_MEMCPY(dst->text_message, src->text_message, sizeof dst->text_message);
    DSD_MEMCPY(dst->event_string, src->event_string, sizeof dst->event_string);
    DSD_MEMCPY(dst->internal_str, src->internal_str, sizeof dst->internal_str);
}

static void
frontend_snapshot_copy_event_history(dsd_frontend_snapshot* out, const dsd_state* state) {
    if (!out || !state || !state->event_history_s) {
        return;
    }
    out->event_history_present = 1;
    for (size_t slot = 0; slot < DSD_FRONTEND_EVENT_HISTORY_SLOTS; slot++) {
        for (size_t i = 0; i < DSD_FRONTEND_EVENT_HISTORY_ITEMS; i++) {
            frontend_event_history_item_copy(&out->event_history[slot].items[i],
                                             &state->event_history_s[slot].Event_History_Items[i]);
        }
    }
}

static void
frontend_snapshot_copy_trunk_channels(dsd_frontend_snapshot* out, const dsd_state* state) {
    if (!out || !state) {
        return;
    }
    uint32_t count = state->trunk_chan_map_used_count;
    if (count > DSD_TRUNK_CHAN_MAP_SIZE) {
        count = DSD_TRUNK_CHAN_MAP_SIZE;
    }
    for (uint32_t i = 0; i < count && out->trunk_channel_count < DSD_FRONTEND_TRUNK_CHANNEL_MAX; i++) {
        const uint16_t channel = state->trunk_chan_map_used[i];
        if (!dsd_state_trunk_chan_tracked(channel)) {
            continue;
        }
        const long freq = state->trunk_chan_map[channel];
        if (freq == 0) {
            continue;
        }
        dsd_frontend_trunk_channel* dst = &out->trunk_channels[out->trunk_channel_count++];
        dst->channel = channel;
        dst->freq_hz = freq;
    }
    out->trunk_channel_sequence = state->trunk_chan_map_seq;
}

static void
frontend_snapshot_copy_cc_candidates(dsd_frontend_snapshot* out, const dsd_state* state) {
    const dsd_trunk_cc_candidates* cc = state ? dsd_trunk_cc_candidates_peek(state) : NULL;
    if (!out || !cc) {
        return;
    }
    int count = cc->count;
    if (count > DSD_FRONTEND_TRUNK_CC_CANDIDATES_MAX) {
        count = DSD_FRONTEND_TRUNK_CC_CANDIDATES_MAX;
    }
    out->trunk_cc_candidates.count = count;
    out->trunk_cc_candidates.index = cc->idx;
    out->trunk_cc_candidates.added = cc->added;
    out->trunk_cc_candidates.used = cc->used;
    for (int i = 0; i < count; i++) {
        out->trunk_cc_candidates.candidates[i].freq_hz = cc->candidates[i];
        out->trunk_cc_candidates.candidates[i].flags = cc->flags[i];
        out->trunk_cc_candidates.candidates[i].cool_until_monotonic_s = cc->cool_until[i];
    }
}

static void
frontend_snapshot_copy_state_scalars(dsd_frontend_snapshot* out, const dsd_state* state) {
    if (!out || !state) {
        return;
    }
    out->has_state = 1;
    DSD_SNPRINTF(out->ui_message.text, sizeof out->ui_message.text, "%s", state->ui_msg);
    out->ui_message.expire_unix_s = (int64_t)state->ui_msg_expire;
    out->input_level = state->input_level;
    out->input_level_last_toast_time = (int64_t)state->input_level_last_toast_time;
    out->input_level_last_toast_status = state->input_level_last_toast_status;
    out->input_level_last_toast_source = state->input_level_last_toast_source;
    out->slots[0].last_tg = (uint32_t)state->lasttg;
    out->slots[0].last_src = (uint32_t)state->lastsrc;
    out->slots[0].payload_algid = (uint32_t)state->payload_algid;
    out->slots[0].payload_keyid = (uint32_t)state->payload_keyid;
    out->slots[0].audio_allowed = state->p25_p2_audio_allowed[0];
    DSD_SNPRINTF(out->slots[0].call_string, sizeof out->slots[0].call_string, "%s", state->call_string[0]);
    out->slots[0].active_call = (out->slots[0].audio_allowed || out->slots[0].call_string[0] != '\0') ? 1 : 0;
    out->slots[1].last_tg = (uint32_t)state->lasttgR;
    out->slots[1].last_src = (uint32_t)state->lastsrcR;
    out->slots[1].payload_algid = (uint32_t)state->payload_algidR;
    out->slots[1].payload_keyid = (uint32_t)state->payload_keyidR;
    out->slots[1].audio_allowed = state->p25_p2_audio_allowed[1];
    DSD_SNPRINTF(out->slots[1].call_string, sizeof out->slots[1].call_string, "%s", state->call_string[1]);
    out->slots[1].active_call = (out->slots[1].audio_allowed || out->slots[1].call_string[0] != '\0') ? 1 : 0;

    out->p25.p2_wacn = state->p2_wacn;
    out->p25.p2_sysid = state->p2_sysid;
    out->p25.p2_cc = state->p2_cc;
    out->p25.trunk_cc_freq = state->trunk_cc_freq;
    out->p25.trunk_vc_freq = state->trunk_vc_freq[0];
    out->p25.p25_cc_freq = state->p25_cc_freq;
    out->p25.p25_vc_freq = state->p25_vc_freq[0];
    out->p25.p25_cc_is_tdma = state->p25_cc_is_tdma;
    out->p25.p25_p2_active_slot = state->p25_p2_active_slot;
    out->p25.p25_p2_audio_ring_count[0] = state->p25_p2_audio_ring_count[0];
    out->p25.p25_p2_audio_ring_count[1] = state->p25_p2_audio_ring_count[1];
    out->p25.p25_p2_audio_allowed[0] = state->p25_p2_audio_allowed[0];
    out->p25.p25_p2_audio_allowed[1] = state->p25_p2_audio_allowed[1];
    out->p25.p25_p1_fec_ok = state->p25_p1_fec_ok;
    out->p25.p25_p1_fec_err = state->p25_p1_fec_err;
    out->p25.p25_p2_facch_ok = state->p25_p2_rs_facch_ok;
    out->p25.p25_p2_facch_err = state->p25_p2_rs_facch_err;
    out->p25.p25_p2_sacch_ok = state->p25_p2_rs_sacch_ok;
    out->p25.p25_p2_sacch_err = state->p25_p2_rs_sacch_err;
    out->p25.p25_p2_voice_err = state->p25_p2_voice_err_hist_sum[0] + state->p25_p2_voice_err_hist_sum[1];
}

int
dsd_app_frontend_snapshot_get(dsd_frontend_snapshot* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    const dsd_opts* opts = dsd_app_get_latest_opts_snapshot();
    const dsd_state* state = dsd_app_get_latest_snapshot();
    out->has_options = opts ? 1 : 0;
    dsd_app_frontend_status_from_opts_state(opts, state, &out->status);
    (void)dsd_app_frontend_get_metrics_from_opts_state(opts, state, &out->metrics);
    if (state) {
        frontend_snapshot_copy_state_scalars(out, state);
        frontend_snapshot_copy_trunk_channels(out, state);
        frontend_snapshot_copy_cc_candidates(out, state);
        frontend_snapshot_copy_event_history(out, state);
    }
    return (opts || state) ? 0 : -1;
}
