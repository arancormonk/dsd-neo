// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frontend_internal.h"
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
static atomic_int g_mu_init = 0;
static unsigned long long g_pub_seq = 0;
static unsigned long long g_consume_seq = 0;
static unsigned long long g_pub_eh_seq = 0;
static unsigned long long g_consume_eh_seq = 0;
static int g_pub_eh_present = 0;

#define UI_SNAPSHOT_FIELD_END(field)            (offsetof(dsd_state, field) + sizeof(((dsd_state*)0)->field))
#define UI_SNAPSHOT_COPY_FIELD(dst, src, field) DSD_MEMCPY(&(dst)->field, &(src)->field, sizeof((dst)->field))
#define UI_SNAPSHOT_COPY_RANGE(dst, src, first, last)                                                                  \
    ui_snapshot_copy_range((dst), (src), offsetof(dsd_state, first), UI_SNAPSHOT_FIELD_END(last))

static void
ui_snapshot_copy_range(dsd_state* dst, const dsd_state* src, size_t begin, size_t end) {
    DSD_MEMCPY((char*)dst + begin, (const char*)src + begin, end - begin);
}

static int
frontend_snapshot_p25_iden_vu_bandwidth_hz(uint8_t bw_vu) {
    switch (bw_vu & 0x0FU) {
        case 0x4U: return 6250;
        case 0x5U: return 12500;
        default: return 0;
    }
}

static const char*
frontend_snapshot_p25_system_service_name_for_bit(unsigned int service_bit) {
    static const char* const k_service_names[24] = {
        NULL,
        NULL,
        "network active",
        NULL,
        "group voice",
        "individual voice",
        "PSTN-unit voice",
        "unit-PSTN voice",
        NULL,
        "group data",
        "individual data",
        NULL,
        "unit registration",
        "group affiliation",
        "group affiliation query",
        "authentication",
        "encryption",
        "user status",
        "user message",
        "unit status",
        "user status query",
        "unit status query",
        "unit page",
        "emergency alarm",
    };
    if (service_bit < 1U || service_bit > 24U) {
        return NULL;
    }
    return k_service_names[service_bit - 1U];
}

static size_t
frontend_snapshot_format_p25_system_service_names(uint32_t service_mask, char* out, size_t out_len) {
    if (out && out_len > 0U) {
        out[0] = '\0';
    }
    size_t count = 0U;
    size_t used = 0U;
    for (unsigned int bit = 1U; bit <= 24U; bit++) {
        const char* name = frontend_snapshot_p25_system_service_name_for_bit(bit);
        if (!name || ((service_mask >> (24U - bit)) & 1U) == 0U) {
            continue;
        }
        count++;
        if (!out || out_len == 0U || used >= out_len - 1U) {
            continue;
        }
        const char* sep = (used > 0U) ? ", " : "";
        int wrote = DSD_SNPRINTF(out + used, out_len - used, "%s%s", sep, name);
        if (wrote < 0) {
            out[used] = '\0';
            continue;
        }
        if ((size_t)wrote >= out_len - used) {
            used = out_len - 1U;
            out[used] = '\0';
        } else {
            used += (size_t)wrote;
        }
    }
    return count;
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
        lhs->severity == rhs->severity,
        lhs->category == rhs->category,
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
        int eh_changed = g_pub_eh_present ? 0 : 1;
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
        g_pub_eh_present = 1;
        g_pub.event_history_s = g_pub_eh;
    } else {
        if (g_pub_eh_present) {
            g_pub_eh_seq++;
        }
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

static const dsd_state*
dsd_app_get_latest_live_snapshot(void) {
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
    dsd_mutex_unlock(&g_mu);
    return &g_consume;
}

static void
frontend_copy_text(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    DSD_SNPRINTF(dst, dst_size, "%s", src ? src : "");
    dst[dst_size - 1] = '\0';
}

static int
frontend_text_has_prefix(const char* text, const char* prefix) {
    if (!text || !prefix) {
        return 0;
    }
    const size_t n = strlen(prefix);
    return strncmp(text, prefix, n) == 0;
}

static dsd_frontend_event_severity
frontend_severity_from_core(uint8_t severity) {
    switch ((dsd_event_severity)severity) {
        case DSD_EVENT_SEVERITY_DEBUG: return DSD_FRONTEND_EVENT_SEVERITY_DEBUG;
        case DSD_EVENT_SEVERITY_INFO: return DSD_FRONTEND_EVENT_SEVERITY_INFO;
        case DSD_EVENT_SEVERITY_WARNING: return DSD_FRONTEND_EVENT_SEVERITY_WARNING;
        case DSD_EVENT_SEVERITY_ERROR: return DSD_FRONTEND_EVENT_SEVERITY_ERROR;
        case DSD_EVENT_SEVERITY_UNKNOWN:
        default: return DSD_FRONTEND_EVENT_SEVERITY_UNKNOWN;
    }
}

static dsd_frontend_event_category
frontend_category_from_core(uint8_t category) {
    switch ((dsd_event_category)category) {
        case DSD_EVENT_CATEGORY_STATUS: return DSD_FRONTEND_EVENT_CATEGORY_STATUS;
        case DSD_EVENT_CATEGORY_VOICE: return DSD_FRONTEND_EVENT_CATEGORY_VOICE;
        case DSD_EVENT_CATEGORY_DATA: return DSD_FRONTEND_EVENT_CATEGORY_DATA;
        case DSD_EVENT_CATEGORY_CONTROL: return DSD_FRONTEND_EVENT_CATEGORY_CONTROL;
        case DSD_EVENT_CATEGORY_SYSTEM: return DSD_FRONTEND_EVENT_CATEGORY_SYSTEM;
        case DSD_EVENT_CATEGORY_UNKNOWN:
        default: return DSD_FRONTEND_EVENT_CATEGORY_UNKNOWN;
    }
}

static dsd_frontend_event_severity
frontend_severity_from_text(const char* text) {
    if (frontend_text_has_prefix(text, "Failed") || frontend_text_has_prefix(text, "Error")
        || frontend_text_has_prefix(text, "ERR")) {
        return DSD_FRONTEND_EVENT_SEVERITY_ERROR;
    }
    if (frontend_text_has_prefix(text, "Unsupported") || frontend_text_has_prefix(text, "Warning")
        || frontend_text_has_prefix(text, "WARN")) {
        return DSD_FRONTEND_EVENT_SEVERITY_WARNING;
    }
    return DSD_FRONTEND_EVENT_SEVERITY_INFO;
}

static dsd_frontend_protocol
frontend_protocol_from_systype(int systype) {
    if (DSD_SYNC_IS_P25(systype)) {
        return DSD_FRONTEND_PROTOCOL_P25;
    }
    if (DSD_SYNC_IS_DMR(systype)) {
        return DSD_FRONTEND_PROTOCOL_DMR;
    }
    if (DSD_SYNC_IS_NXDN(systype)) {
        return DSD_FRONTEND_PROTOCOL_NXDN;
    }
    if (DSD_SYNC_IS_YSF(systype)) {
        return DSD_FRONTEND_PROTOCOL_YSF;
    }
    if (DSD_SYNC_IS_M17(systype)) {
        return DSD_FRONTEND_PROTOCOL_M17;
    }
    if (DSD_SYNC_IS_DSTAR(systype)) {
        return DSD_FRONTEND_PROTOCOL_DSTAR;
    }
    if (DSD_SYNC_IS_DPMR(systype)) {
        return DSD_FRONTEND_PROTOCOL_DPMR;
    }
    if (DSD_SYNC_IS_PROVOICE(systype)) {
        return DSD_FRONTEND_PROTOCOL_PROVOICE;
    }
    if (DSD_SYNC_IS_EDACS_ONLY(systype)) {
        return DSD_FRONTEND_PROTOCOL_EDACS;
    }
    if (DSD_SYNC_IS_X2TDMA(systype)) {
        return DSD_FRONTEND_PROTOCOL_X2TDMA;
    }
    if (systype == DSD_SYNC_ANALOG) {
        return DSD_FRONTEND_PROTOCOL_ANALOG;
    }
    if (systype == DSD_SYNC_DIGITAL) {
        return DSD_FRONTEND_PROTOCOL_DIGITAL;
    }
    return DSD_FRONTEND_PROTOCOL_UNKNOWN;
}

static int
frontend_text_has_nonspace(const char* text) {
    if (text == NULL) {
        return 0;
    }
    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
        if (!isspace(*p)) {
            return 1;
        }
    }
    return 0;
}

static int
frontend_event_has_content(const Event_History* src) {
    return src != NULL
           && (src->source_id != 0U || src->target_id != 0U || src->event_string[0] != '\0'
               || src->internal_str[0] != '\0' || src->text_message[0] != '\0' || src->gps_s[0] != '\0'
               || src->alias[0] != '\0' || src->src_str[0] != '\0' || src->tgt_str[0] != '\0');
}

static size_t
frontend_event_pdu_len(const uint8_t* pdu, size_t cap) {
    if (!pdu) {
        return 0;
    }
    size_t n = cap;
    while (n > 0U && pdu[n - 1U] == 0U) {
        n--;
    }
    return n;
}

static int
frontend_systype_is_data(int8_t systype) {
    static const int8_t k_data_systypes[] = {
        DSD_SYNC_DMR_BS_DATA_POS,
        DSD_SYNC_DMR_BS_DATA_NEG,
        DSD_SYNC_DMR_MS_DATA,
        DSD_SYNC_DMR_RC_DATA,
    };
    for (size_t i = 0; i < sizeof k_data_systypes / sizeof k_data_systypes[0]; i++) {
        if (k_data_systypes[i] == systype) {
            return 1;
        }
    }
    return 0;
}

static int
frontend_event_is_data(const Event_History* src) {
    return src != NULL
           && (src->text_message[0] != '\0' || src->gps_s[0] != '\0'
               || frontend_event_pdu_len(src->pdu, sizeof src->pdu) > 0U || src->subtype == INT8_MAX
               || frontend_systype_is_data(src->systype));
}

static int
frontend_event_is_voice(const Event_History* src) {
    return src != NULL
           && (src->source_id != 0U || src->target_id != 0U || src->src_str[0] != '\0' || src->tgt_str[0] != '\0');
}

static dsd_frontend_event_category
frontend_category_from_event(const Event_History* src) {
    if (!src) {
        return DSD_FRONTEND_EVENT_CATEGORY_UNKNOWN;
    }
    if (frontend_event_is_data(src)) {
        return DSD_FRONTEND_EVENT_CATEGORY_DATA;
    }
    if (frontend_event_is_voice(src)) {
        return DSD_FRONTEND_EVENT_CATEGORY_VOICE;
    }
    if (src->internal_str[0] != '\0') {
        return DSD_FRONTEND_EVENT_CATEGORY_SYSTEM;
    }
    if (src->event_string[0] != '\0') {
        return DSD_FRONTEND_EVENT_CATEGORY_STATUS;
    }
    return DSD_FRONTEND_EVENT_CATEGORY_UNKNOWN;
}

static dsd_frontend_encryption_state
frontend_encryption_from_event(const Event_History* src) {
    if (!src) {
        return DSD_FRONTEND_ENCRYPTION_UNKNOWN;
    }
    return src->enc ? DSD_FRONTEND_ENCRYPTION_ENCRYPTED : DSD_FRONTEND_ENCRYPTION_CLEAR;
}

static void
frontend_event_history_item_copy(dsd_frontend_event_history_item* dst, const Event_History* src) {
    if (!dst || !src) {
        return;
    }
    dst->present = frontend_event_has_content(src) ? 1U : 0U;
    dst->write_pending = src->write;
    dst->severity = frontend_severity_from_core(src->severity);
    if (dst->severity == DSD_FRONTEND_EVENT_SEVERITY_UNKNOWN) {
        dst->severity = frontend_severity_from_text(src->internal_str[0] ? src->internal_str : src->event_string);
    }
    dst->category = frontend_category_from_core(src->category);
    if (dst->category == DSD_FRONTEND_EVENT_CATEGORY_UNKNOWN) {
        dst->category = frontend_category_from_event(src);
    }
    dst->protocol = frontend_protocol_from_systype(src->systype);
    dst->subtype = (int16_t)(int)src->subtype;
    dst->encryption_state = frontend_encryption_from_event(src);
    dst->encryption_alg_id = src->enc_alg;
    dst->encryption_key_id = src->enc_key;
    dst->encryption_message_indicator = src->mi;
    dst->service_options = src->svc;
    dst->group_individual = src->gi;
    dst->system_id[0] = src->sys_id1;
    dst->system_id[1] = src->sys_id2;
    dst->system_id[2] = src->sys_id3;
    dst->system_id[3] = src->sys_id4;
    dst->system_id[4] = src->sys_id5;
    dst->source_id = src->source_id;
    dst->target_id = src->target_id;
    dst->channel = src->channel;
    dst->timestamp_unix_s = (int64_t)src->event_time;
    frontend_copy_text(dst->source_text, sizeof dst->source_text, src->src_str);
    frontend_copy_text(dst->target_text, sizeof dst->target_text, src->tgt_str);
    frontend_copy_text(dst->source_label, sizeof dst->source_label, src->s_name);
    frontend_copy_text(dst->target_label, sizeof dst->target_label, src->t_name);
    frontend_copy_text(dst->source_mode, sizeof dst->source_mode, src->s_mode);
    frontend_copy_text(dst->target_mode, sizeof dst->target_mode, src->t_mode);
    frontend_copy_text(dst->system_label, sizeof dst->system_label, src->sysid_string);
    DSD_MEMCPY(dst->pdu, src->pdu, sizeof dst->pdu);
    dst->pdu_len = frontend_event_pdu_len(src->pdu, sizeof src->pdu);
    frontend_copy_text(dst->summary_text, sizeof dst->summary_text, src->event_string);
    frontend_copy_text(dst->detail_text, sizeof dst->detail_text, src->internal_str);
    frontend_copy_text(dst->gps_text, sizeof dst->gps_text, src->gps_s);
    frontend_copy_text(dst->text_message, sizeof dst->text_message, src->text_message);
    frontend_copy_text(dst->alias, sizeof dst->alias, src->alias);
}

static void
frontend_event_history_summary_copy(dsd_frontend_event_history_summary* dst, const Event_History* src, uint8_t slot) {
    if (!dst || !src) {
        return;
    }
    DSD_MEMSET(dst, 0, sizeof(*dst));
    dst->present = frontend_event_has_content(src) ? 1U : 0U;
    dst->write_pending = src->write;
    dst->slot = slot;
    dst->severity = frontend_severity_from_core(src->severity);
    if (dst->severity == DSD_FRONTEND_EVENT_SEVERITY_UNKNOWN) {
        dst->severity = frontend_severity_from_text(src->internal_str[0] ? src->internal_str : src->event_string);
    }
    dst->category = frontend_category_from_core(src->category);
    if (dst->category == DSD_FRONTEND_EVENT_CATEGORY_UNKNOWN) {
        dst->category = frontend_category_from_event(src);
    }
    dst->protocol = frontend_protocol_from_systype(src->systype);
    dst->encryption_state = frontend_encryption_from_event(src);
    dst->source_id = src->source_id;
    dst->target_id = src->target_id;
    dst->channel = src->channel;
    dst->timestamp_unix_s = (int64_t)src->event_time;
    frontend_copy_text(dst->source_text, sizeof dst->source_text, src->src_str);
    frontend_copy_text(dst->target_text, sizeof dst->target_text, src->tgt_str);
    frontend_copy_text(dst->source_label, sizeof dst->source_label, src->s_name);
    frontend_copy_text(dst->target_label, sizeof dst->target_label, src->t_name);
    frontend_copy_text(dst->system_label, sizeof dst->system_label, src->sysid_string);
    frontend_copy_text(dst->summary_text, sizeof dst->summary_text, src->event_string);
    frontend_copy_text(dst->detail_text, sizeof dst->detail_text, src->internal_str);
}

static void
frontend_snapshot_copy_event_history_meta(dsd_frontend_snapshot* out) {
    if (!out) {
        return;
    }
    out->event_history_slot_count = DSD_FRONTEND_EVENT_HISTORY_SLOTS;
    out->event_history_items_per_slot = DSD_FRONTEND_EVENT_HISTORY_ITEMS;

    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    out->event_history_present = (g_have && g_pub_eh_present) ? 1 : 0;
    out->event_history_sequence = (uint64_t)g_pub_eh_seq;
    dsd_mutex_unlock(&g_mu);
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
frontend_snapshot_copy_active_channels(dsd_frontend_snapshot* out, const dsd_state* state) {
    if (!out || !state) {
        return;
    }
    for (size_t i = 0; i < DSD_FRONTEND_ACTIVE_CHANNEL_MAX; i++) {
        dsd_frontend_active_channel_summary* dst = &out->active_channels[i];
        const uint32_t target = (i == 0U) ? (uint32_t)state->lasttg : (uint32_t)state->lasttgR;
        const uint32_t source = (i == 0U) ? (uint32_t)state->lastsrc : (uint32_t)state->lastsrcR;
        const uint32_t algid = (i == 0U) ? (uint32_t)state->payload_algid : (uint32_t)state->payload_algidR;
        const uint32_t keyid = (i == 0U) ? (uint32_t)state->payload_keyid : (uint32_t)state->payload_keyidR;
        dst->slot = (uint8_t)i;
        dst->target_id = target;
        dst->source_id = source;
        dst->payload_algid = algid;
        dst->payload_keyid = keyid;
        dst->p25_vc_freq = state->p25_vc_freq[i];
        dst->trunk_vc_freq = state->trunk_vc_freq[i];
        dst->audio_allowed = state->p25_p2_audio_allowed[i];
        dst->present = (dst->p25_vc_freq != 0 || dst->trunk_vc_freq != 0 || target != 0U || source != 0U
                        || dst->audio_allowed != 0)
                           ? 1U
                           : 0U;
        if (dst->present) {
            dst->protocol = frontend_protocol_from_systype(state->lastsynctype);
            out->active_channel_count++;
        }
    }
}

static void
frontend_snapshot_copy_p25_neighbors(dsd_frontend_snapshot* out, const dsd_state* state) {
    if (!out || !state) {
        return;
    }
    int count = state->p25_nb_count;
    if (count < 0) {
        count = 0;
    }
    if (count > DSD_FRONTEND_P25_NEIGHBOR_MAX) {
        count = DSD_FRONTEND_P25_NEIGHBOR_MAX;
    }
    for (int i = 0; i < count; i++) {
        const p25_nb_entry_t* src = &state->p25_nb_entries[i];
        if (src->freq == 0) {
            continue;
        }
        dsd_frontend_p25_neighbor_summary* dst = &out->p25.neighbors[out->p25.neighbor_count++];
        dst->present = 1U;
        dst->wacn = src->wacn;
        dst->wacn_valid = src->wacn_valid;
        dst->freq_hz = src->freq;
        dst->sysid = src->sysid;
        dst->rfss = src->rfss;
        dst->site = src->site;
        dst->cfva = src->cfva;
        dst->lra = src->lra;
        dst->lra_valid = src->lra_valid;
        dst->cfva_valid = src->cfva_valid;
        dst->last_seen_unix_s = (int64_t)src->last_seen;
    }
}

static void
frontend_snapshot_copy_p25_secondary_ccs(dsd_frontend_snapshot* out, const dsd_state* state) {
    if (!out || !state) {
        return;
    }
    int count = state->p25_secondary_cc_count;
    if (count < 0) {
        count = 0;
    }
    if (count > DSD_FRONTEND_P25_SECONDARY_CC_MAX) {
        count = DSD_FRONTEND_P25_SECONDARY_CC_MAX;
    }
    for (int i = 0; i < count; i++) {
        const p25_secondary_cc_entry_t* src = &state->p25_secondary_cc_entries[i];
        if (src->freq == 0) {
            continue;
        }
        dsd_frontend_p25_secondary_cc_summary* dst = &out->p25.secondary_ccs[out->p25.secondary_cc_count++];
        dst->present = 1U;
        dst->channel = src->channel;
        dst->rfss = src->rfss;
        dst->site = src->site;
        dst->system_service_class = src->ssc;
        dst->freq_hz = src->freq;
        dst->last_seen_unix_s = (int64_t)src->last_seen;
    }
}

static void
frontend_snapshot_copy_p25_iden_entry(dsd_frontend_snapshot* out, int id, int tdma, const p25_iden_entry_t* src) {
    if (!out || !src || !src->populated || out->p25.iden_plan_count >= DSD_FRONTEND_P25_IDEN_PLAN_MAX) {
        return;
    }
    dsd_frontend_p25_iden_entry_summary* dst = &out->p25.iden_plan[out->p25.iden_plan_count++];
    dst->present = 1U;
    dst->id = (uint8_t)id;
    dst->tdma = tdma ? 1U : 0U;
    dst->trust = src->trust;
    dst->channel_type = (uint8_t)src->chan_type;
    dst->bandwidth = src->bw_vu;
    dst->bandwidth_hz = frontend_snapshot_p25_iden_vu_bandwidth_hz(src->bw_vu);
    dst->base_freq_hz = src->base_freq * 5L;
    dst->spacing_hz = src->chan_spac * 125;
    dst->transmit_offset = src->trans_off;
    dst->wacn = src->wacn;
    dst->sysid = src->sysid;
    dst->rfss = src->rfss;
    dst->site = src->site;
    if (src->trust >= 2U) {
        out->p25.iden_plan_confirmed_count++;
    }
}

static void
frontend_snapshot_copy_p25_iden_plan(dsd_frontend_snapshot* out, const dsd_state* state) {
    if (!out || !state) {
        return;
    }
    for (int id = 0; id < 16; id++) {
        frontend_snapshot_copy_p25_iden_entry(out, id, 0, &state->p25_iden_fdma[id]);
        frontend_snapshot_copy_p25_iden_entry(out, id, 1, &state->p25_iden_tdma[id]);
    }
}

static void
frontend_snapshot_copy_state_scalars(dsd_frontend_snapshot* out, const dsd_state* state) {
    if (!out || !state) {
        return;
    }
    out->has_state = 1;
    out->ui_message.present = state->ui_msg[0] != '\0' ? 1U : 0U;
    out->ui_message.severity = frontend_severity_from_text(state->ui_msg);
    out->ui_message.category = DSD_FRONTEND_EVENT_CATEGORY_STATUS;
    DSD_SNPRINTF(out->ui_message.source, sizeof out->ui_message.source, "%s", "decoder");
    out->ui_message.slot = -1;
    out->ui_message.created_unix_s = 0;
    frontend_copy_text(out->ui_message.text, sizeof out->ui_message.text, state->ui_msg);
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
    out->slots[0].active_call =
        (out->slots[0].audio_allowed || frontend_text_has_nonspace(out->slots[0].call_string)) ? 1 : 0;
    out->slots[1].last_tg = (uint32_t)state->lasttgR;
    out->slots[1].last_src = (uint32_t)state->lastsrcR;
    out->slots[1].payload_algid = (uint32_t)state->payload_algidR;
    out->slots[1].payload_keyid = (uint32_t)state->payload_keyidR;
    out->slots[1].audio_allowed = state->p25_p2_audio_allowed[1];
    DSD_SNPRINTF(out->slots[1].call_string, sizeof out->slots[1].call_string, "%s", state->call_string[1]);
    out->slots[1].active_call =
        (out->slots[1].audio_allowed || frontend_text_has_nonspace(out->slots[1].call_string)) ? 1 : 0;

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
    out->p25.p25_site_lra_valid = state->p25_site_lra_valid;
    out->p25.p25_site_lra = state->p25_site_lra;
    out->p25.p25_site_network_active_valid = state->p25_site_network_active_valid;
    out->p25.p25_site_network_active = state->p25_site_network_active;
    out->p25.p25_sys_services_valid = state->p25_sys_services_valid;
    out->p25.p25_sys_services_available = state->p25_sys_services_available;
    out->p25.p25_sys_services_supported = state->p25_sys_services_supported;
    out->p25.p25_sys_services_request_priority = state->p25_sys_services_request_priority;
    (void)frontend_snapshot_format_p25_system_service_names(state->p25_sys_services_available,
                                                            out->p25.p25_sys_services_available_names,
                                                            sizeof(out->p25.p25_sys_services_available_names));
    (void)frontend_snapshot_format_p25_system_service_names(state->p25_sys_services_supported,
                                                            out->p25.p25_sys_services_supported_names,
                                                            sizeof(out->p25.p25_sys_services_supported_names));
    out->p25.p25_cc_prot_valid = state->p25_cc_prot_valid;
    out->p25.p25_cc_prot_algid = state->p25_cc_prot_algid;
    out->p25.p25_sys_time_valid = state->p25_sys_time_valid;
    out->p25.p25_sys_time_unix_s = (int64_t)state->p25_sys_time;
    out->p25.p25_sys_time_offset_valid = state->p25_sys_time_offset_valid;
    out->p25.p25_sys_time_offset_minutes = state->p25_sys_time_offset;
    out->p25.p25_p1_fec_ok = state->p25_p1_fec_ok;
    out->p25.p25_p1_fec_err = state->p25_p1_fec_err;
    out->p25.p25_p2_facch_ok = state->p25_p2_rs_facch_ok;
    out->p25.p25_p2_facch_err = state->p25_p2_rs_facch_err;
    out->p25.p25_p2_sacch_ok = state->p25_p2_rs_sacch_ok;
    out->p25.p25_p2_sacch_err = state->p25_p2_rs_sacch_err;
    out->p25.p25_p2_voice_err = state->p25_p2_voice_err_hist_sum[0] + state->p25_p2_voice_err_hist_sum[1];
    frontend_snapshot_copy_active_channels(out, state);
    frontend_snapshot_copy_p25_neighbors(out, state);
    frontend_snapshot_copy_p25_secondary_ccs(out, state);
    frontend_snapshot_copy_p25_iden_plan(out, state);
}

int
dsd_app_frontend_snapshot_get(dsd_frontend_snapshot* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    const dsd_opts* opts = dsd_app_get_latest_opts_snapshot();
    const dsd_state* state = dsd_app_get_latest_live_snapshot();
    out->has_options = opts ? 1 : 0;
    dsd_app_frontend_status_from_opts_state(opts, state, &out->status);
    (void)dsd_app_frontend_get_metrics_from_opts_state(opts, state, &out->metrics);
    if (state) {
        frontend_snapshot_copy_state_scalars(out, state);
        frontend_snapshot_copy_trunk_channels(out, state);
        frontend_snapshot_copy_cc_candidates(out, state);
    }
    frontend_snapshot_copy_event_history_meta(out);
    return (opts || state) ? 0 : -1;
}

static int
frontend_event_history_query_valid(const dsd_frontend_event_history_query* query) {
    return query != NULL && query->slot < DSD_FRONTEND_EVENT_HISTORY_SLOTS;
}

static int
frontend_event_history_page_finish(dsd_frontend_event_history_page_info* out_info,
                                   const dsd_frontend_event_history_page_info* info, int rc) {
    if (out_info) {
        *out_info = *info;
    }
    return rc;
}

static size_t
frontend_event_history_page_count(const dsd_frontend_event_history_query* query,
                                  const dsd_frontend_event_history_summary* out_items, size_t max_items) {
    if (out_items == NULL || query->offset >= DSD_FRONTEND_EVENT_HISTORY_ITEMS) {
        return 0U;
    }
    size_t count = query->limit;
    if (count > max_items) {
        count = max_items;
    }
    const size_t available = DSD_FRONTEND_EVENT_HISTORY_ITEMS - query->offset;
    return count > available ? available : count;
}

int
dsd_app_frontend_event_history_page_get(const dsd_frontend_event_history_query* query,
                                        dsd_frontend_event_history_summary* out_items, size_t max_items,
                                        dsd_frontend_event_history_page_info* out_info) {
    if (!frontend_event_history_query_valid(query)) {
        return -1;
    }
    dsd_frontend_event_history_page_info info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.total_items = DSD_FRONTEND_EVENT_HISTORY_ITEMS;

    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    info.sequence = (uint64_t)g_pub_eh_seq;
    info.present = (g_have && g_pub_eh_present) ? 1 : 0;
    if (!info.present) {
        dsd_mutex_unlock(&g_mu);
        return frontend_event_history_page_finish(out_info, &info, g_have ? 0 : -1);
    }
    if (query->known_sequence != 0U && query->known_sequence == info.sequence) {
        info.unchanged = 1;
        dsd_mutex_unlock(&g_mu);
        return frontend_event_history_page_finish(out_info, &info, 0);
    }

    size_t count = frontend_event_history_page_count(query, out_items, max_items);
    for (size_t i = 0; i < count; i++) {
        size_t idx = query->offset + i;
        frontend_event_history_summary_copy(&out_items[i], &g_pub_eh[query->slot].Event_History_Items[idx],
                                            query->slot);
    }
    info.returned_items = count;
    dsd_mutex_unlock(&g_mu);

    return frontend_event_history_page_finish(out_info, &info, 0);
}

int
dsd_app_frontend_event_history_item_get(uint8_t slot, size_t index, dsd_frontend_event_history_item* out,
                                        uint64_t* out_sequence) {
    if (!out || slot >= DSD_FRONTEND_EVENT_HISTORY_SLOTS || index >= DSD_FRONTEND_EVENT_HISTORY_ITEMS) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    if (!g_have || !g_pub_eh_present) {
        dsd_mutex_unlock(&g_mu);
        return -1;
    }
    frontend_event_history_item_copy(out, &g_pub_eh[slot].Event_History_Items[index]);
    out->slot = slot;
    if (out_sequence) {
        *out_sequence = (uint64_t)g_pub_eh_seq;
    }
    dsd_mutex_unlock(&g_mu);
    return 0;
}
