// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Multi-consumer telemetry feed for the JSON API.
 *
 * Registers a telemetry observer with app-control and, on the decode thread,
 * turns the live decoder state into JSON Lines: call/alpha-tag updates, event
 * history rows, system/site identity, RF metrics, P25 quality and a status
 * record that also carries the context the stateful commands need. Production
 * is gated on at least one authenticated client, so a server with no clients
 * costs one atomic load per publish.
 *
 * Every static below except the cache is touched only on the decode thread (in
 * the observer callbacks) or while the observer is unregistered.
 */

#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/app_control/p25_metrics.h>
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/app_control/telemetry_observers.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "api_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "json.h"

enum { DSD_API_TELEMETRY_INTERVAL_MS = 250 };

static dsd_app_telemetry_observer g_observer;
static int g_observer_added = 0;
static const dsd_opts* g_live_opts = NULL;

static dsd_json_buf g_latest[DSD_API_TOPIC_COUNT]; /* guarded by g_feed_mu */
static dsd_mutex_t g_feed_mu;
static atomic_int g_feed_mu_state = 0;

static dsd_json_buf g_line;   /* one encoded line */
static dsd_json_buf g_events; /* event lines gathered under the history transaction */
static uint64_t g_seq = 0;

/* Event-history diffing state, per slot: the ring and counters last seen, and a fingerprint of each committed row as
   last seen, to find rows enriched or merged in place after they were sent. */
static uint64_t g_eh_instance[2];
static uint64_t g_eh_push[2];
static uint64_t g_eh_commit[2];
static uint64_t g_eh_fp[2][DSD_EVENT_HISTORY_LEN];
static int g_eh_valid = 0;

static uint64_t g_last_telemetry_ms = 0;

static void
ensure_feed_mu(void) {
    if (atomic_load(&g_feed_mu_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_feed_mu_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_feed_mu);
        atomic_store(&g_feed_mu_state, 2);
        return;
    }
    while (atomic_load(&g_feed_mu_state) != 2) {
        dsd_thread_yield();
    }
}

static int
topic_index(uint32_t bit) {
    for (int i = 0; i < DSD_API_TOPIC_COUNT; i++) {
        if (bit == (1u << i)) {
            return i;
        }
    }
    return -1;
}

static void
cache_store(uint32_t topic, const dsd_json_buf* line) {
    const int idx = topic_index(topic);
    if (idx < 0) {
        return;
    }
    dsd_mutex_lock(&g_feed_mu);
    dsd_json_buf_reset(&g_latest[idx]);
    /* The cached copy drops the newline: `get` splices it into a response line. */
    if (line->data != NULL && line->len > 1U && dsd_json_buf_append(&g_latest[idx], line->data, line->len - 1U) != 0) {
        dsd_json_buf_reset(&g_latest[idx]);
    }
    dsd_mutex_unlock(&g_feed_mu);
}

int
dsd_api_feed_latest(const char* what, dsd_json_buf* out) {
    if (what == NULL || out == NULL) {
        return -1;
    }
    uint32_t topic = 0U;
    if (strcmp(what, "status") == 0 || strcmp(what, "snapshot") == 0) {
        topic = DSD_API_TOPIC_STATUS;
    } else if (strcmp(what, "call") == 0) {
        topic = DSD_API_TOPIC_CALL;
    } else if (strcmp(what, "system") == 0) {
        topic = DSD_API_TOPIC_SYSTEM;
    } else if (strcmp(what, "metrics") == 0) {
        topic = DSD_API_TOPIC_METRICS;
    } else if (strcmp(what, "quality") == 0) {
        topic = DSD_API_TOPIC_QUALITY;
    } else {
        return -1;
    }
    const int idx = topic_index(topic);
    ensure_feed_mu();
    dsd_mutex_lock(&g_feed_mu);
    dsd_json_buf_reset(out);
    int rc = 0;
    if (g_latest[idx].data != NULL && g_latest[idx].len > 0U
        && dsd_json_buf_append(out, g_latest[idx].data, g_latest[idx].len) == 0) {
        rc = 1;
    }
    dsd_mutex_unlock(&g_feed_mu);
    return rc;
}

/*============================================================================
 * Encoders
 *============================================================================*/

static void
begin_line(dsd_json_buf* b, dsd_json_writer* w, const char* type) {
    dsd_json_buf_reset(b);
    dsd_json_writer_init(w, b);
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_str(w, "type", type);
    (void)dsd_json_kv_u64(w, "seq", ++g_seq);
}

/* Close the line; 0 when it is a whole, valid line. */
static int
end_line(dsd_json_buf* b, dsd_json_writer* w) {
    (void)dsd_json_obj_end(w);
    if (dsd_json_writer_failed(w) || dsd_json_buf_putc(b, '\n') != 0) {
        return -1;
    }
    return 0;
}

static void
encode_event_row(dsd_json_writer* w, const Event_History* e, int slot, uint64_t push, uint64_t instance) {
    (void)dsd_json_key(w, "row");
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_i64(w, "slot", slot);
    (void)dsd_json_kv_u64(w, "push", push);
    (void)dsd_json_kv_u64_str(w, "ring", instance);
    (void)dsd_json_kv_i64(w, "systype", e->systype);
    (void)dsd_json_kv_i64(w, "subtype", e->subtype);
    (void)dsd_json_kv_i64(w, "severity", e->severity);
    (void)dsd_json_kv_i64(w, "category", e->category);
    (void)dsd_json_kv_i64(w, "crc_invalid", e->crc_invalid);
    (void)dsd_json_kv_u64(w, "sys_id1", e->sys_id1);
    (void)dsd_json_kv_u64(w, "sys_id2", e->sys_id2);
    (void)dsd_json_kv_u64(w, "sys_id3", e->sys_id3);
    (void)dsd_json_kv_u64(w, "sys_id4", e->sys_id4);
    (void)dsd_json_kv_u64(w, "sys_id5", e->sys_id5);
    (void)dsd_json_kv_strn(w, "sysid_string", e->sysid_string, sizeof e->sysid_string);
    (void)dsd_json_kv_i64(w, "gi", e->gi);
    (void)dsd_json_kv_i64(w, "emergency", e->emergency);
    (void)dsd_json_kv_i64(w, "priority", e->priority);
    (void)dsd_json_kv_i64(w, "enc", e->enc);
    (void)dsd_json_kv_i64(w, "enc_alg", e->enc_alg);
    (void)dsd_json_kv_u64(w, "enc_key", e->enc_key);
    (void)dsd_json_kv_u64_str(w, "mi", e->mi);
    (void)dsd_json_kv_u64(w, "svc", e->svc);
    (void)dsd_json_kv_u64(w, "source_id", e->source_id);
    (void)dsd_json_kv_u64(w, "target_id", e->target_id);
    (void)dsd_json_kv_strn(w, "src_str", e->src_str, sizeof e->src_str);
    (void)dsd_json_kv_strn(w, "tgt_str", e->tgt_str, sizeof e->tgt_str);
    (void)dsd_json_kv_strn(w, "t_name", e->t_name, sizeof e->t_name);
    (void)dsd_json_kv_strn(w, "s_name", e->s_name, sizeof e->s_name);
    (void)dsd_json_kv_strn(w, "t_mode", e->t_mode, sizeof e->t_mode);
    (void)dsd_json_kv_strn(w, "s_mode", e->s_mode, sizeof e->s_mode);
    (void)dsd_json_kv_strn(w, "channel_label", e->channel_label, sizeof e->channel_label);
    (void)dsd_json_kv_u64(w, "channel", e->channel);
    (void)dsd_json_kv_i64(w, "freq_hz", e->freq_hz);
    (void)dsd_json_kv_u64(w, "access_code_kind", e->access_code_kind);
    (void)dsd_json_kv_u64(w, "access_code", e->access_code);
    (void)dsd_json_kv_i64(w, "event_time", (int64_t)e->event_time);
    (void)dsd_json_kv_i64(w, "event_start_time", (int64_t)e->event_start_time);
    (void)dsd_json_kv_strn(w, "alias", e->alias, sizeof e->alias);
    (void)dsd_json_kv_strn(w, "gps", e->gps_s, sizeof e->gps_s);
    (void)dsd_json_kv_strn(w, "text", e->text_message, sizeof e->text_message);
    (void)dsd_json_kv_strn(w, "event", e->event_string, sizeof e->event_string);
    (void)dsd_json_kv_strn(w, "internal", e->internal_str, sizeof e->internal_str);
    (void)dsd_json_obj_end(w);
}

static uint64_t
fnv1a_byte(uint64_t h, unsigned char byte) {
    return (h ^ byte) * UINT64_C(1099511628211);
}

/* A scalar, a byte at a time by arithmetic rather than through its storage. */
static uint64_t
fnv1a_u64(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        h = fnv1a_byte(h, (unsigned char)(v >> (8 * i)));
    }
    return h;
}

/* A fixed char field up to its terminator (counted) or its end. */
static uint64_t
fnv1a_str(uint64_t h, const char* s, size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        h = fnv1a_byte(h, (unsigned char)s[i]);
        if (s[i] == '\0') {
            break;
        }
    }
    return h;
}

/* Everything a client sees of a committed row. */
static uint64_t
row_fingerprint(const Event_History* e) {
    const uint64_t scalars[] = {
        (uint64_t)(int64_t)e->event_time,
        (uint64_t)(int64_t)e->event_start_time,
        e->sys_id1,
        e->sys_id2,
        e->sys_id3,
        e->sys_id4,
        e->sys_id5,
        e->source_id,
        e->target_id,
        e->channel,
        (uint64_t)e->freq_hz,
        e->access_code_kind,
        e->access_code,
        e->mi,
        e->enc_key,
        e->svc,
        e->crc_invalid,
        e->severity,
        e->category,
        (uint64_t)(int64_t)e->systype,
        (uint64_t)(int64_t)e->subtype,
        (uint64_t)(int64_t)e->gi,
        e->emergency,
        e->priority,
        e->enc,
        e->enc_alg,
    };
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < sizeof scalars / sizeof scalars[0]; i++) {
        h = fnv1a_u64(h, scalars[i]);
    }
    h = fnv1a_str(h, e->sysid_string, sizeof e->sysid_string);
    h = fnv1a_str(h, e->src_str, sizeof e->src_str);
    h = fnv1a_str(h, e->tgt_str, sizeof e->tgt_str);
    h = fnv1a_str(h, e->t_name, sizeof e->t_name);
    h = fnv1a_str(h, e->s_name, sizeof e->s_name);
    h = fnv1a_str(h, e->t_mode, sizeof e->t_mode);
    h = fnv1a_str(h, e->s_mode, sizeof e->s_mode);
    h = fnv1a_str(h, e->channel_label, sizeof e->channel_label);
    h = fnv1a_str(h, e->alias, sizeof e->alias);
    h = fnv1a_str(h, e->gps_s, sizeof e->gps_s);
    h = fnv1a_str(h, e->text_message, sizeof e->text_message);
    h = fnv1a_str(h, e->event_string, sizeof e->event_string);
    h = fnv1a_str(h, e->internal_str, sizeof e->internal_str);
    return h;
}

/* The fingerprint of a row as init_event_history() leaves it: never written, or cleared by a history reset. */
static uint64_t g_eh_empty_fp = 0U;

static void
eh_init_empty_fingerprint(void) {
    static Event_History blank; /* static: zero-filled; only the fields init_event_history() sets differ */
    blank.systype = -1;
    blank.subtype = -1;
    blank.severity = DSD_EVENT_SEVERITY_UNKNOWN;
    blank.category = DSD_EVENT_CATEGORY_UNKNOWN;
    g_eh_empty_fp = row_fingerprint(&blank);
}

/* Append the line for committed row @p idx of @p ring to g_events. */
static void
gather_event(const Event_History_I* ring, int slot, uint64_t idx, int update) {
    dsd_json_writer w;
    begin_line(&g_line, &w, "event");
    (void)dsd_json_kv_bool(&w, "update", update);
    encode_event_row(&w, &ring->Event_History_Items[idx], slot, ring->push_seq - (idx - 1U), ring->instance);
    if (end_line(&g_line, &w) == 0) {
        (void)dsd_json_buf_append(&g_events, g_line.data, g_line.len);
    }
}

static void
eh_baseline(const Event_History_I* ring, int slot) {
    g_eh_instance[slot] = ring->instance;
    g_eh_push[slot] = ring->push_seq;
    g_eh_commit[slot] = ring->commit_rev;
    for (int i = 1; i < DSD_EVENT_HISTORY_LEN; i++) {
        g_eh_fp[slot][i] = row_fingerprint(&ring->Event_History_Items[i]);
    }
}

/* Gather one slot's rows pushed since the last look (new) and committed rows whose content changed in place (updates:
   late alias, GPS or text, a reacquisition merge), oldest first. */
static void
gather_slot_events(const Event_History_I* ring, int slot, int send) {
    if (!g_eh_valid || ring->instance != g_eh_instance[slot] || ring->push_seq < g_eh_push[slot]) {
        /* First look, or a new ring: start from what is there now rather than replaying it. */
        eh_baseline(ring, slot);
        return;
    }
    if (ring->push_seq == g_eh_push[slot] && ring->commit_rev == g_eh_commit[slot]) {
        return;
    }
    uint64_t pushed = ring->push_seq - g_eh_push[slot];
    if (pushed > (uint64_t)(DSD_EVENT_HISTORY_LEN - 1)) {
        pushed = (uint64_t)(DSD_EVENT_HISTORY_LEN - 1);
    }
    /* Each push moved every committed row one index deeper; rows 1..pushed are new. */
    for (uint64_t i = (uint64_t)(DSD_EVENT_HISTORY_LEN - 1); i >= 1U; i--) {
        g_eh_fp[slot][i] = (i > pushed) ? g_eh_fp[slot][i - pushed] : 0U;
    }
    for (uint64_t i = (uint64_t)(DSD_EVENT_HISTORY_LEN - 1); i >= 1U; i--) {
        const uint64_t fp = row_fingerprint(&ring->Event_History_Items[i]);
        if (send && fp != g_eh_empty_fp && (i <= pushed || fp != g_eh_fp[slot][i])) {
            gather_event(ring, slot, i, i > pushed);
        }
        g_eh_fp[slot][i] = fp;
    }
    g_eh_push[slot] = ring->push_seq;
    g_eh_commit[slot] = ring->commit_rev;
}

/* Follow the event rings at every publish, whether or not anyone listens, and send what changed to the subscribers
   when there are any. Following them all along is what lets a new subscriber get every row committed after it
   subscribed: a baseline taken at its first publish would swallow a row committed in between. While nothing changes
   this reads two counters per slot. A row keeps its identity, (ring, slot, push), across new and update lines. */
static void
track_events(const dsd_state* state) {
    if (state->event_history_s == NULL) {
        g_eh_valid = 0;
        return;
    }
    dsd_json_buf_reset(&g_events);
    /* Other threads write the rings under this transaction; read them under it too. Interest is read under it as
       well: a row the watchdog commits after a subscription took effect cannot then be passed over as unwanted. */
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin((dsd_state*)state, &transaction);
    const int send = dsd_api_topic_interest(DSD_API_TOPIC_EVENT);
    for (int slot = 0; slot < 2; slot++) {
        gather_slot_events(&state->event_history_s[slot], slot, send);
    }
    dsd_event_history_transaction_end(&transaction);
    g_eh_valid = 1;
    if (send && g_events.len > 0U && g_events.data != NULL) {
        dsd_api_broadcast(g_events.data, g_events.len, DSD_API_TOPIC_EVENT);
    }
}

static void
encode_slot_call(dsd_json_writer* w, const dsd_state* state, int slot, double now_m) {
    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, (uint8_t)slot, now_m, &view);
    if (view.state == DSD_APP_CALL_LINE_NONE) {
        return;
    }
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_i64(w, "slot", slot);
    (void)dsd_json_kv_i64(w, "state", view.state);
    (void)dsd_json_kv_strn(w, "name", view.name, sizeof view.name);
    (void)dsd_json_kv_strn(w, "tg_text", view.tg_text, sizeof view.tg_text);
    (void)dsd_json_kv_strn(w, "src_text", view.src_text, sizeof view.src_text);
    (void)dsd_json_kv_strn(w, "channel", view.channel, sizeof view.channel);
    (void)dsd_json_kv_u64(w, "tg_id", view.tg_id);
    (void)dsd_json_kv_u64(w, "elapsed_ms", view.elapsed_ms);
    (void)dsd_json_kv_u64(w, "kid", view.kid);
    (void)dsd_json_kv_u64(w, "algid", view.algid);
    (void)dsd_json_kv_bool(w, "emergency", view.emergency);
    (void)dsd_json_kv_u64(w, "priority", view.priority);
    (void)dsd_json_kv_bool(w, "enc", view.enc);
    dsd_call_snapshot snap;
    if (dsd_call_state_get(state, (uint8_t)slot, &snap) > 0) {
        (void)dsd_json_kv_i64(w, "frequency_hz", snap.frequency_hz);
        (void)dsd_json_kv_i64(w, "protocol", snap.protocol);
        (void)dsd_json_kv_i64(w, "kind", snap.kind);
        (void)dsd_json_kv_u64(w, "ota_source_id", snap.ota_source_id);
    }
    (void)dsd_json_obj_end(w);
}

static int
build_call(const dsd_state* state, dsd_json_buf* b) {
    const double now_m = dsd_decode_now_mono_s();
    dsd_json_writer w;
    begin_line(b, &w, "call");
    (void)dsd_json_key(&w, "slots");
    (void)dsd_json_arr_begin(&w);
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        encode_slot_call(&w, state, slot, now_m);
    }
    (void)dsd_json_arr_end(&w);
    return end_line(b, &w);
}

/* What the stateful commands must quote back: the talkgroup list version, the decryption context, the scan row. */
static void
encode_control_context(dsd_json_writer* w, const dsd_state* state) {
    uint64_t policy_context = 0U;
    unsigned int policy_generation = 0U;
    dsd_tg_policy_table_version(state, &policy_context, &policy_generation);
    (void)dsd_json_key(w, "tg_policy");
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_u64_str(w, "context", policy_context);
    (void)dsd_json_kv_u64(w, "generation", policy_generation);
    (void)dsd_json_obj_end(w);

    (void)dsd_json_key(w, "decryption");
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_strn(w, "target_id", state->trunk_scan_active_id, sizeof state->trunk_scan_active_id);
    (void)dsd_json_kv_u64_str(w, "tune_generation", dsd_trunk_tuning_generation());
    (void)dsd_json_kv_u64_str(w, "key_epoch", state->enc_lockout_key_epoch);
    (void)dsd_json_obj_end(w);

    dsd_app_scan_row_view row;
    if (g_live_opts != NULL && dsd_app_scan_row_view_get(g_live_opts, state, &row) == 0 && row.active) {
        (void)dsd_json_key(w, "scan_row");
        (void)dsd_json_obj_begin(w);
        (void)dsd_json_kv_i64(w, "scanner", row.scanner);
        (void)dsd_json_kv_u64(w, "session", row.session);
        (void)dsd_json_kv_i64(w, "row", row.row);
        (void)dsd_json_kv_u64(w, "mode", row.mode);
        (void)dsd_json_kv_u64(w, "editable", row.editable);
        (void)dsd_json_kv_u64(w, "edited", row.edited);
        (void)dsd_json_kv_u64(w, "listed", row.listed);
        (void)dsd_json_kv_bool(w, "opts_match", row.opts_match);
        (void)dsd_json_kv_strn(w, "target_id", row.target_id, sizeof row.target_id);
        (void)dsd_json_kv_strn(w, "label", row.label, sizeof row.label);
        (void)dsd_json_obj_end(w);
    } else {
        (void)dsd_json_kv_null(w, "scan_row");
    }
}

static int
build_status(const dsd_state* state, dsd_json_buf* b) {
    dsd_app_notification_status st;
    dsd_json_writer w;
    begin_line(b, &w, "status");
    if (dsd_app_notification_get(&st)) {
        (void)dsd_json_kv_bool(&w, "available", 1);
        (void)dsd_json_kv_strn(&w, "protocol", st.protocol, sizeof st.protocol);
        (void)dsd_json_kv_i64(&w, "vc_freq_hz", st.vc_freq_hz);
        (void)dsd_json_kv_i64(&w, "cc_freq_hz", st.cc_freq_hz);
        (void)dsd_json_kv_i64(&w, "center_freq_hz", st.center_freq_hz);
        (void)dsd_json_kv_bool(&w, "radio_input", st.radio_input);
        (void)dsd_json_kv_bool(&w, "trunking", st.trunking);
        (void)dsd_json_kv_bool(&w, "trunk_tuned", st.trunk_tuned);
        (void)dsd_json_kv_i64(&w, "lead_slot", st.lead_slot);
        (void)dsd_json_key(&w, "slots");
        (void)dsd_json_arr_begin(&w);
        for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
            (void)dsd_json_obj_begin(&w);
            (void)dsd_json_kv_i64(&w, "slot", slot);
            (void)dsd_json_kv_i64(&w, "state", st.slots[slot].state);
            (void)dsd_json_kv_strn(&w, "name", st.slots[slot].name, sizeof st.slots[slot].name);
            (void)dsd_json_kv_strn(&w, "src_text", st.slots[slot].src_text, sizeof st.slots[slot].src_text);
            (void)dsd_json_kv_u64(&w, "tg_id", st.slots[slot].tg_id);
            (void)dsd_json_kv_bool(&w, "emergency", st.slots[slot].emergency);
            (void)dsd_json_kv_bool(&w, "enc", st.slots[slot].enc);
            (void)dsd_json_obj_end(&w);
        }
        (void)dsd_json_arr_end(&w);
    } else {
        (void)dsd_json_kv_bool(&w, "available", 0);
    }
    (void)dsd_json_kv_i64(&w, "synctype", state->synctype);
    /* The decoder's notice line -- among other things how a queued command it refused says so. */
    const time_t now = dsd_realtime_time();
    if (state->ui_msg[0] != '\0' && state->ui_msg_expire > now) {
        (void)dsd_json_key(&w, "message");
        (void)dsd_json_obj_begin(&w);
        (void)dsd_json_kv_strn(&w, "text", state->ui_msg, sizeof state->ui_msg);
        (void)dsd_json_kv_i64(&w, "expires", (int64_t)state->ui_msg_expire);
        (void)dsd_json_obj_end(&w);
    } else {
        (void)dsd_json_kv_null(&w, "message");
    }
    encode_control_context(&w, state);
    return end_line(b, &w);
}

static int
build_system(const dsd_state* state, dsd_json_buf* b) {
    dsd_json_writer w;
    begin_line(b, &w, "system");
    dsd_app_notification_status st;
    if (dsd_app_notification_get(&st)) {
        (void)dsd_json_kv_strn(&w, "protocol", st.protocol, sizeof st.protocol);
        (void)dsd_json_kv_i64(&w, "vc_freq_hz", st.vc_freq_hz);
        (void)dsd_json_kv_i64(&w, "cc_freq_hz", st.cc_freq_hz);
        (void)dsd_json_kv_i64(&w, "center_freq_hz", st.center_freq_hz);
        (void)dsd_json_kv_bool(&w, "trunking", st.trunking);
    }
    (void)dsd_json_kv_i64(&w, "synctype", state->synctype);

    /* The whole table; a WACN or LRA no announcement carried (an abbreviated or frequency-only one) is null, not 0. */
    dsd_app_p25_neighbor neighbors[P25_NB_MAX];
    const int n = dsd_app_p25_neighbors(state, neighbors, P25_NB_MAX);
    (void)dsd_json_key(&w, "p25_neighbors");
    (void)dsd_json_arr_begin(&w);
    for (int i = 0; i < n; i++) {
        const dsd_app_p25_neighbor* nb = &neighbors[i];
        (void)dsd_json_obj_begin(&w);
        (void)dsd_json_kv_i64(&w, "freq_hz", nb->freq_hz);
        (void)(nb->wacn_valid ? dsd_json_kv_u64(&w, "wacn", nb->wacn) : dsd_json_kv_null(&w, "wacn"));
        (void)dsd_json_kv_u64(&w, "sysid", nb->sysid);
        (void)dsd_json_kv_u64(&w, "rfss", nb->rfss);
        (void)dsd_json_kv_u64(&w, "site", nb->site);
        (void)(nb->lra_valid ? dsd_json_kv_u64(&w, "lra", nb->lra) : dsd_json_kv_null(&w, "lra"));
        (void)dsd_json_kv_bool(&w, "current_cc", nb->is_current_cc);
        (void)dsd_json_kv_bool(&w, "candidate", nb->is_candidate);
        (void)dsd_json_kv_strn(&w, "cfva", nb->cfva_text, sizeof nb->cfva_text);
        (void)(nb->last_seen > 0 ? dsd_json_kv_i64(&w, "last_seen", (int64_t)nb->last_seen)
                                 : dsd_json_kv_null(&w, "last_seen"));
        (void)dsd_json_obj_end(&w);
    }
    (void)dsd_json_arr_end(&w);
    return end_line(b, &w);
}

/* RF readings follow the rules every frontend applies (docs/code_map.md, app-control behavior notes): readings exist
   only while an RTL-family stream runs, the SNR is the shared per-modulation pick, and a reading nobody has produced is
   null, never its sentinel. */
static int
build_metrics(const dsd_state* state, dsd_json_buf* b) {
    dsd_frontend_metrics m;
    DSD_MEMSET(&m, 0, sizeof m);
    dsd_json_writer w;
    begin_line(b, &w, "metrics");
    if (g_live_opts == NULL
        || dsd_app_frontend_get_metrics_for_snapshot(g_live_opts, state, &m, DSD_FRONTEND_SNR_FALLBACK_ALL) != 0) {
        (void)dsd_json_kv_bool(&w, "available", 0);
        return end_line(b, &w);
    }
    const int stream = m.stream_active != 0;
    (void)dsd_json_kv_bool(&w, "available", 1);
    (void)dsd_json_kv_bool(&w, "stream_active", stream);
    (void)dsd_json_kv_i64(&w, "rf_mod", state->rf_mod);
    const dsd_frontend_snr_readout snr = dsd_app_frontend_snr_for_mod(&m, state->rf_mod);
    if (snr.valid) {
        (void)dsd_json_kv_double(&w, "snr_db", snr.snr_db);
    } else {
        (void)dsd_json_kv_null(&w, "snr_db");
    }
    if (stream) {
        (void)dsd_json_kv_u64(&w, "output_rate_hz", m.output_rate_hz);
        (void)dsd_json_kv_i64(&w, "symbol_rate_hz", m.symbol_rate_hz);
        (void)dsd_json_kv_i64(&w, "symbol_levels", m.symbol_levels);
        (void)dsd_json_kv_i64(&w, "channel_profile", m.channel_profile);
        (void)dsd_json_kv_i64(&w, "channel_bandwidth_hz", m.channel_bandwidth_hz);
        (void)dsd_json_kv_double(&w, "cfo_hz", m.cfo_hz);
        (void)dsd_json_kv_bool(&w, "carrier_lock", m.carrier_lock);
    } else {
        static const char* const k_stream_only[] = {
            "output_rate_hz",       "symbol_rate_hz", "symbol_levels", "channel_profile",
            "channel_bandwidth_hz", "cfo_hz",         "carrier_lock",
        };
        for (size_t i = 0; i < sizeof k_stream_only / sizeof k_stream_only[0]; i++) {
            (void)dsd_json_kv_null(&w, k_stream_only[i]);
        }
    }
    if (m.tuner_gain_valid) {
        (void)dsd_json_kv_i64(&w, "tuner_gain_tenth_db", m.tuner_gain_tenth_db);
        (void)dsd_json_kv_bool(&w, "tuner_gain_is_auto", m.tuner_gain_is_auto);
    } else {
        (void)dsd_json_kv_null(&w, "tuner_gain_tenth_db");
        (void)dsd_json_kv_null(&w, "tuner_gain_is_auto");
    }
    (void)dsd_json_kv_i64(&w, "requested_ppm", m.requested_ppm);
    (void)dsd_json_kv_bool(&w, "auto_ppm_enabled", m.auto_ppm_enabled);
    (void)dsd_json_kv_bool(&w, "auto_ppm_locked", m.auto_ppm_enabled && m.auto_ppm_locked);
    if (m.auto_ppm_enabled && m.auto_ppm_locked) {
        (void)dsd_json_kv_i64(&w, "auto_ppm_locked_ppm", m.auto_ppm_locked_ppm);
    } else {
        (void)dsd_json_kv_null(&w, "auto_ppm_locked_ppm");
    }
    (void)dsd_json_key(&w, "decode_health");
    if (m.decode_health.valid) {
        (void)dsd_json_obj_begin(&w);
        (void)dsd_json_kv_u64(&w, "p25p1_fec_ok", m.decode_health.p25p1_fec_ok);
        (void)dsd_json_kv_u64(&w, "p25p1_fec_err", m.decode_health.p25p1_fec_err);
        (void)dsd_json_kv_u64(&w, "p25p2_facch_ok", m.decode_health.p25p2_facch_ok);
        (void)dsd_json_kv_u64(&w, "p25p2_facch_err", m.decode_health.p25p2_facch_err);
        (void)dsd_json_kv_u64(&w, "p25p2_sacch_ok", m.decode_health.p25p2_sacch_ok);
        (void)dsd_json_kv_u64(&w, "p25p2_sacch_err", m.decode_health.p25p2_sacch_err);
        (void)dsd_json_kv_u64(&w, "p25p2_voice_err", m.decode_health.p25p2_voice_err);
        (void)dsd_json_obj_end(&w);
    } else {
        (void)dsd_json_value_null(&w);
    }
    return end_line(b, &w);
}

static void
encode_fec(dsd_json_writer* w, const char* key, const dsd_app_fec_ratio* r) {
    (void)dsd_json_key(w, key);
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_bool(w, "valid", r->valid);
    (void)dsd_json_kv_u64(w, "ok", r->ok);
    (void)dsd_json_kv_u64(w, "err", r->err);
    (void)dsd_json_kv_double(w, "ok_pct", r->ok_pct);
    (void)dsd_json_obj_end(w);
}

static void
encode_voice_errs(dsd_json_writer* w, const dsd_app_voice_errs* v) {
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_bool(w, "valid", v->valid);
    (void)dsd_json_kv_double(w, "errs_per_frame", v->errs_per_frame);
    (void)dsd_json_kv_u64(w, "samples", v->samples);
    (void)dsd_json_obj_end(w);
}

static int
build_quality(const dsd_state* state, dsd_json_buf* b) {
    dsd_app_p25_quality q;
    dsd_app_p25_quality_from_state(state, &q);
    dsd_json_writer w;
    begin_line(b, &w, "quality");
    (void)dsd_json_kv_bool(&w, "valid", q.valid);
    encode_fec(&w, "cc_fec", &q.cc_fec);
    encode_fec(&w, "voice_fec", &q.voice_fec);
    encode_fec(&w, "rs", &q.rs);
    (void)dsd_json_key(&w, "p1_voice");
    encode_voice_errs(&w, &q.p1_voice);
    (void)dsd_json_key(&w, "p2_voice");
    (void)dsd_json_arr_begin(&w);
    for (int i = 0; i < 2; i++) {
        encode_voice_errs(&w, &q.p2_voice[i]);
    }
    (void)dsd_json_arr_end(&w);
    return end_line(b, &w);
}

/*============================================================================
 * Observer
 *============================================================================*/

static void
emit_topic(uint32_t topic, int built) {
    if (built != 0 || g_line.data == NULL) {
        return;
    }
    cache_store(topic, &g_line);
    if (dsd_api_topic_interest(topic)) {
        dsd_api_broadcast(g_line.data, g_line.len, topic);
    }
}

static void
feed_state(const dsd_state* state, void* user) {
    (void)user;
    if (state == NULL) {
        return;
    }
    track_events(state);
    if (dsd_api_client_count() <= 0) {
        return;
    }
    const uint64_t now = dsd_realtime_mono_ms();
    if (g_last_telemetry_ms != 0U && now - g_last_telemetry_ms < (uint64_t)DSD_API_TELEMETRY_INTERVAL_MS) {
        return;
    }
    g_last_telemetry_ms = now;
    /* Every get-able topic is kept current while a client is connected, so a one-shot get never waits. */
    emit_topic(DSD_API_TOPIC_CALL, build_call(state, &g_line));
    emit_topic(DSD_API_TOPIC_STATUS, build_status(state, &g_line));
    emit_topic(DSD_API_TOPIC_SYSTEM, build_system(state, &g_line));
    emit_topic(DSD_API_TOPIC_METRICS, build_metrics(state, &g_line));
    emit_topic(DSD_API_TOPIC_QUALITY, build_quality(state, &g_line));
}

static void
feed_opts(const dsd_opts* opts, void* user) {
    (void)user;
    g_live_opts = opts;
}

void
dsd_api_feed_start(void) {
    ensure_feed_mu();
    if (g_observer_added) {
        return;
    }
    /* Reset before registering: once registered, these belong to the decode thread. */
    eh_init_empty_fingerprint();
    g_eh_valid = 0;
    g_last_telemetry_ms = 0U;
    g_live_opts = NULL;
    g_observer.state = feed_state;
    g_observer.opts = feed_opts;
    g_observer.user = NULL;
    if (dsd_app_telemetry_observer_add(&g_observer) == 0) {
        g_observer_added = 1;
    }
}

void
dsd_api_feed_stop(void) {
    if (!g_observer_added) {
        return;
    }
    /* Synchronous: no feed callback is running once this returns. */
    (void)dsd_app_telemetry_observer_remove(&g_observer);
    g_observer_added = 0;
    g_live_opts = NULL;
    dsd_json_buf_free(&g_line);
    dsd_json_buf_free(&g_events);
    ensure_feed_mu();
    dsd_mutex_lock(&g_feed_mu);
    for (int i = 0; i < DSD_API_TOPIC_COUNT; i++) {
        dsd_json_buf_free(&g_latest[i]);
    }
    dsd_mutex_unlock(&g_feed_mu);
}
