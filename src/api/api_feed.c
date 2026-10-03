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
 * history rows, system/site identity, RF metrics and P25 quality. Production is
 * gated on at least one connected client so a server with no clients costs one
 * atomic load per publish.
 */

#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/app_control/p25_metrics.h>
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/app_control/telemetry_observers.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/decode_clock.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "api_internal.h"

enum { DSD_API_TELEMETRY_INTERVAL_MS = 250 };

static dsd_app_telemetry_observer g_observer;
static int g_observer_added = 0;
static const dsd_opts* g_live_opts = NULL;

static dsd_json_buf g_latest[DSD_API_TOPIC_COUNT];
static dsd_mutex_t g_feed_mu;
static atomic_int g_feed_mu_state = 0;
static uint64_t g_seq = 0;

/* Event-history diffing state, one entry per slot. */
static uint64_t g_eh_instance[2];
static uint64_t g_eh_push[2];
static uint64_t g_eh_commit[2];
static int g_eh_valid = 0;

static unsigned long long g_last_telemetry_ms = 0;

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
cache_store(uint32_t topic, const dsd_json_buf* buf) {
    const int idx = topic_index(topic);
    if (idx < 0) {
        return;
    }
    dsd_mutex_lock(&g_feed_mu);
    dsd_json_buf_reset(&g_latest[idx]);
    if (buf->data != NULL) {
        (void)dsd_json_buf_append(&g_latest[idx], buf->data, buf->len);
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
    if (idx < 0) {
        return -1;
    }
    ensure_feed_mu();
    dsd_mutex_lock(&g_feed_mu);
    dsd_json_buf_reset(out);
    int rc = -1;
    if (g_latest[idx].data != NULL) {
        /* The cached line carries its newline for broadcast; a spliced value
         * must not, or it would break the enclosing response in two. */
        size_t len = g_latest[idx].len;
        if (len > 0U && g_latest[idx].data[len - 1U] == '\n') {
            len--;
        }
        (void)dsd_json_buf_append(out, g_latest[idx].data, len);
        rc = 0;
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

static void
end_line(dsd_json_buf* b, dsd_json_writer* w) {
    (void)dsd_json_obj_end(w);
    (void)dsd_json_buf_putc(b, '\n');
}

static void
encode_event_row(dsd_json_writer* w, const Event_History* e, int slot, uint64_t api_seq) {
    (void)dsd_json_key(w, "row");
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_u64(w, "seq", api_seq);
    (void)dsd_json_kv_i64(w, "slot", slot);
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
    (void)dsd_json_kv_str(w, "sysid_string", e->sysid_string);
    (void)dsd_json_kv_i64(w, "gi", e->gi);
    (void)dsd_json_kv_i64(w, "emergency", e->emergency);
    (void)dsd_json_kv_i64(w, "priority", e->priority);
    (void)dsd_json_kv_i64(w, "enc", e->enc);
    (void)dsd_json_kv_i64(w, "enc_alg", e->enc_alg);
    (void)dsd_json_kv_u64(w, "enc_key", e->enc_key);
    (void)dsd_json_kv_u64(w, "mi", e->mi);
    (void)dsd_json_kv_u64(w, "svc", e->svc);
    (void)dsd_json_kv_u64(w, "source_id", e->source_id);
    (void)dsd_json_kv_u64(w, "target_id", e->target_id);
    (void)dsd_json_kv_str(w, "src_str", e->src_str);
    (void)dsd_json_kv_str(w, "tgt_str", e->tgt_str);
    (void)dsd_json_kv_str(w, "t_name", e->t_name);
    (void)dsd_json_kv_str(w, "s_name", e->s_name);
    (void)dsd_json_kv_str(w, "t_mode", e->t_mode);
    (void)dsd_json_kv_str(w, "s_mode", e->s_mode);
    (void)dsd_json_kv_str(w, "channel_label", e->channel_label);
    (void)dsd_json_kv_u64(w, "channel", e->channel);
    (void)dsd_json_kv_i64(w, "event_time", (int64_t)e->event_time);
    (void)dsd_json_kv_i64(w, "event_start_time", (int64_t)e->event_start_time);
    (void)dsd_json_kv_str(w, "alias", e->alias);
    (void)dsd_json_kv_str(w, "gps", e->gps_s);
    (void)dsd_json_kv_str(w, "text", e->text_message);
    (void)dsd_json_kv_str(w, "event", e->event_string);
    (void)dsd_json_kv_str(w, "internal", e->internal_str);
    (void)dsd_json_obj_end(w);
}

static void
emit_events(const dsd_state* state) {
    if (state->event_history_s == NULL) {
        return;
    }
    for (int slot = 0; slot < 2; slot++) {
        const Event_History_I* ring = &state->event_history_s[slot];
        if (!g_eh_valid || ring->instance != g_eh_instance[slot]) {
            g_eh_instance[slot] = ring->instance;
            g_eh_push[slot] = ring->push_seq;
            g_eh_commit[slot] = ring->commit_rev;
            continue;
        }
        int emit_count = 0;
        uint64_t first_index = 0;
        if (ring->push_seq > g_eh_push[slot]) {
            uint64_t n = ring->push_seq - g_eh_push[slot];
            if (n > (uint64_t)(DSD_EVENT_HISTORY_LEN - 1)) {
                n = (uint64_t)(DSD_EVENT_HISTORY_LEN - 1);
            }
            emit_count = (int)n;
            first_index = n; /* index n is the oldest of the new rows */
            g_eh_push[slot] = ring->push_seq;
        } else if (ring->commit_rev != g_eh_commit[slot]) {
            emit_count = 1;
            first_index = 1; /* re-emit the newest committed row after enrichment */
        }
        g_eh_commit[slot] = ring->commit_rev;
        for (int k = 0; k < emit_count; k++) {
            const uint64_t idx = first_index - (uint64_t)k;
            if (idx == 0 || idx >= (uint64_t)DSD_EVENT_HISTORY_LEN) {
                continue;
            }
            dsd_json_buf b;
            dsd_json_buf_init(&b);
            dsd_json_writer w;
            begin_line(&b, &w, "event");
            encode_event_row(&w, &ring->Event_History_Items[idx], slot, g_seq);
            end_line(&b, &w);
            if (!dsd_json_writer_failed(&w) && b.data != NULL) {
                dsd_api_broadcast(b.data, b.len, DSD_API_TOPIC_EVENT);
            }
            dsd_json_buf_free(&b);
        }
    }
    g_eh_valid = 1;
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
    (void)dsd_json_kv_str(w, "name", view.name);
    (void)dsd_json_kv_str(w, "tg_text", view.tg_text);
    (void)dsd_json_kv_str(w, "src_text", view.src_text);
    (void)dsd_json_kv_str(w, "channel", view.channel);
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

static void
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
    end_line(b, &w);
}

static void
build_status(const dsd_state* state, dsd_json_buf* b) {
    dsd_app_notification_status st;
    dsd_json_writer w;
    begin_line(b, &w, "status");
    if (dsd_app_notification_get(&st)) {
        (void)dsd_json_kv_str(&w, "protocol", st.protocol);
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
            (void)dsd_json_kv_str(&w, "name", st.slots[slot].name);
            (void)dsd_json_kv_str(&w, "src_text", st.slots[slot].src_text);
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
    end_line(b, &w);
}

static void
build_system(const dsd_state* state, dsd_json_buf* b) {
    dsd_json_writer w;
    begin_line(b, &w, "system");
    dsd_app_notification_status st;
    if (dsd_app_notification_get(&st)) {
        (void)dsd_json_kv_str(&w, "protocol", st.protocol);
        (void)dsd_json_kv_i64(&w, "vc_freq_hz", st.vc_freq_hz);
        (void)dsd_json_kv_i64(&w, "cc_freq_hz", st.cc_freq_hz);
        (void)dsd_json_kv_i64(&w, "center_freq_hz", st.center_freq_hz);
        (void)dsd_json_kv_bool(&w, "trunking", st.trunking);
    }
    (void)dsd_json_kv_i64(&w, "synctype", state->synctype);

    dsd_app_p25_neighbor neighbors[16];
    const int n = dsd_app_p25_neighbors(state, neighbors, 16);
    if (n > 0) {
        (void)dsd_json_key(&w, "p25_neighbors");
        (void)dsd_json_arr_begin(&w);
        for (int i = 0; i < n; i++) {
            (void)dsd_json_obj_begin(&w);
            (void)dsd_json_kv_i64(&w, "freq_hz", neighbors[i].freq_hz);
            (void)dsd_json_kv_u64(&w, "wacn", neighbors[i].wacn);
            (void)dsd_json_kv_u64(&w, "sysid", neighbors[i].sysid);
            (void)dsd_json_kv_u64(&w, "rfss", neighbors[i].rfss);
            (void)dsd_json_kv_u64(&w, "site", neighbors[i].site);
            (void)dsd_json_kv_u64(&w, "lra", neighbors[i].lra);
            (void)dsd_json_kv_bool(&w, "current_cc", neighbors[i].is_current_cc);
            (void)dsd_json_kv_bool(&w, "candidate", neighbors[i].is_candidate);
            (void)dsd_json_kv_str(&w, "cfva", neighbors[i].cfva_text);
            (void)dsd_json_obj_end(&w);
        }
        (void)dsd_json_arr_end(&w);
    }
    end_line(b, &w);
}

static void
build_metrics(const dsd_state* state, dsd_json_buf* b) {
    dsd_frontend_metrics m;
    memset(&m, 0, sizeof m);
    if (g_live_opts == NULL
        || dsd_app_frontend_get_metrics_for_snapshot(g_live_opts, state, &m, DSD_FRONTEND_SNR_FALLBACK_ALL) != 0) {
        dsd_json_writer w;
        begin_line(b, &w, "metrics");
        (void)dsd_json_kv_bool(&w, "available", 0);
        end_line(b, &w);
        return;
    }
    dsd_json_writer w;
    begin_line(b, &w, "metrics");
    (void)dsd_json_kv_u64(&w, "output_rate_hz", m.output_rate_hz);
    (void)dsd_json_kv_i64(&w, "symbol_rate_hz", m.symbol_rate_hz);
    (void)dsd_json_kv_i64(&w, "symbol_levels", m.symbol_levels);
    (void)dsd_json_kv_i64(&w, "channel_profile", m.channel_profile);
    (void)dsd_json_kv_i64(&w, "channel_bandwidth_hz", m.channel_bandwidth_hz);
    (void)dsd_json_kv_double(&w, "cfo_hz", m.cfo_hz);
    (void)dsd_json_kv_bool(&w, "carrier_lock", m.carrier_lock);
    (void)dsd_json_kv_double(&w, "snr_c4fm_db", m.snr_c4fm_db);
    (void)dsd_json_kv_double(&w, "snr_cqpsk_db", m.snr_cqpsk_db);
    (void)dsd_json_kv_double(&w, "snr_gfsk_db", m.snr_gfsk_db);
    (void)dsd_json_kv_i64(&w, "spectrum_size", m.spectrum_size);
    (void)dsd_json_kv_i64(&w, "requested_ppm", m.requested_ppm);
    (void)dsd_json_kv_i64(&w, "tuner_gain_tenth_db", m.tuner_gain_tenth_db);
    (void)dsd_json_kv_bool(&w, "tuner_gain_is_auto", m.tuner_gain_is_auto);
    (void)dsd_json_kv_bool(&w, "auto_ppm_locked", m.auto_ppm_locked);
    (void)dsd_json_kv_i64(&w, "auto_ppm_locked_ppm", m.auto_ppm_locked_ppm);
    (void)dsd_json_key(&w, "decode_health");
    (void)dsd_json_obj_begin(&w);
    (void)dsd_json_kv_u64(&w, "p25p1_fec_ok", m.decode_health.p25p1_fec_ok);
    (void)dsd_json_kv_u64(&w, "p25p1_fec_err", m.decode_health.p25p1_fec_err);
    (void)dsd_json_kv_u64(&w, "p25p2_facch_ok", m.decode_health.p25p2_facch_ok);
    (void)dsd_json_kv_u64(&w, "p25p2_facch_err", m.decode_health.p25p2_facch_err);
    (void)dsd_json_kv_u64(&w, "p25p2_sacch_ok", m.decode_health.p25p2_sacch_ok);
    (void)dsd_json_kv_u64(&w, "p25p2_sacch_err", m.decode_health.p25p2_sacch_err);
    (void)dsd_json_kv_u64(&w, "p25p2_voice_err", m.decode_health.p25p2_voice_err);
    (void)dsd_json_obj_end(&w);
    end_line(b, &w);
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
encode_voice_errs(dsd_json_writer* w, const char* key, const dsd_app_voice_errs* v) {
    (void)dsd_json_key(w, key);
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_bool(w, "valid", v->valid);
    (void)dsd_json_kv_double(w, "errs_per_frame", v->errs_per_frame);
    (void)dsd_json_kv_u64(w, "samples", v->samples);
    (void)dsd_json_obj_end(w);
}

static void
build_quality(const dsd_state* state, dsd_json_buf* b) {
    dsd_app_p25_quality q;
    dsd_app_p25_quality_from_state(state, &q);
    dsd_json_writer w;
    begin_line(b, &w, "quality");
    (void)dsd_json_kv_bool(&w, "valid", q.valid);
    encode_fec(&w, "cc_fec", &q.cc_fec);
    encode_fec(&w, "voice_fec", &q.voice_fec);
    encode_fec(&w, "rs", &q.rs);
    encode_voice_errs(&w, "p1_voice", &q.p1_voice);
    (void)dsd_json_key(&w, "p2_voice");
    (void)dsd_json_arr_begin(&w);
    for (int i = 0; i < 2; i++) {
        (void)dsd_json_obj_begin(&w);
        (void)dsd_json_kv_bool(&w, "valid", q.p2_voice[i].valid);
        (void)dsd_json_kv_double(&w, "errs_per_frame", q.p2_voice[i].errs_per_frame);
        (void)dsd_json_kv_u64(&w, "samples", q.p2_voice[i].samples);
        (void)dsd_json_obj_end(&w);
    }
    (void)dsd_json_arr_end(&w);
    end_line(b, &w);
}

/*============================================================================
 * Observer
 *============================================================================*/

static void
emit_topic(uint32_t topic, dsd_json_buf* b) {
    cache_store(topic, b);
    dsd_api_broadcast(b->data, b->len, topic);
}

static void
feed_state(const dsd_state* state, void* user) {
    (void)user;
    if (state == NULL) {
        return;
    }
    if (dsd_api_client_count() <= 0) {
        return;
    }
    if (dsd_api_topic_interest(DSD_API_TOPIC_EVENT)) {
        emit_events(state);
    }
    const unsigned long long now = (unsigned long long)dsd_realtime_mono_ms();
    if (g_last_telemetry_ms != 0ULL && now - g_last_telemetry_ms < (unsigned long long)DSD_API_TELEMETRY_INTERVAL_MS) {
        return;
    }
    g_last_telemetry_ms = now;

    dsd_json_buf b;
    dsd_json_buf_init(&b);
    if (dsd_api_topic_interest(DSD_API_TOPIC_CALL)) {
        build_call(state, &b);
        emit_topic(DSD_API_TOPIC_CALL, &b);
    }
    if (dsd_api_topic_interest(DSD_API_TOPIC_STATUS)) {
        build_status(state, &b);
        emit_topic(DSD_API_TOPIC_STATUS, &b);
    }
    if (dsd_api_topic_interest(DSD_API_TOPIC_SYSTEM)) {
        build_system(state, &b);
        emit_topic(DSD_API_TOPIC_SYSTEM, &b);
    }
    if (dsd_api_topic_interest(DSD_API_TOPIC_METRICS)) {
        build_metrics(state, &b);
        emit_topic(DSD_API_TOPIC_METRICS, &b);
    }
    if (dsd_api_topic_interest(DSD_API_TOPIC_QUALITY)) {
        build_quality(state, &b);
        emit_topic(DSD_API_TOPIC_QUALITY, &b);
    }
    dsd_json_buf_free(&b);
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
    g_observer.state = feed_state;
    g_observer.opts = feed_opts;
    g_observer.user = NULL;
    if (dsd_app_telemetry_observer_add(&g_observer) == 0) {
        g_observer_added = 1;
    }
    g_eh_valid = 0;
    g_last_telemetry_ms = 0;
}

void
dsd_api_feed_stop(void) {
    if (!g_observer_added) {
        return;
    }
    (void)dsd_app_telemetry_observer_remove(&g_observer);
    g_observer_added = 0;
    g_live_opts = NULL;
    ensure_feed_mu();
    dsd_mutex_lock(&g_feed_mu);
    for (int i = 0; i < DSD_API_TOPIC_COUNT; i++) {
        dsd_json_buf_reset(&g_latest[i]);
    }
    dsd_mutex_unlock(&g_feed_mu);
}
