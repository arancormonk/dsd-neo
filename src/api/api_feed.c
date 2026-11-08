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
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/app_control/p25_metrics.h>
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/app_control/receive_view.h>
#include <dsd-neo/app_control/scan_avoid_view.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/app_control/scan_state_view.h>
#include <dsd-neo/app_control/site_identity_view.h>
#include <dsd-neo/app_control/telemetry_observers.h>
#include <dsd-neo/core/analog_mode.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
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
/* Decode thread: the cache (g_latest and the scan avoid copy) holds records of this run, refreshed while a client is
   connected, and the state instance they describe. */
static int g_cache_live = 0;
static uint64_t g_cache_instance = 0;

/* The session's scan avoids (issue #672) as the feed last collected them, for status.scan_avoids and the paged `get`.
   The decode thread collects raw entries (no formatting) into a new immutable block only when the scan avoid view's
   key moved, and swaps it in; a session thread pins the block it reads with a reference taken under g_feed_mu and
   formats its page outside every lock, so a list of any size never holds the lock or the decode thread up. The last
   holder frees a replaced block. */
typedef struct {
    atomic_int refs;
    dsd_app_scan_avoid_raw raw;
} api_scan_avoid_block;

static api_scan_avoid_block* g_sa_block = NULL; /* guarded by g_feed_mu; NULL after a failed collection */
static dsd_app_scan_avoid_summary g_sa_shown;   /* guarded by g_feed_mu; written by the observer (or once it is gone) */
static int g_sa_shown_valid = 0;                /* guarded by g_feed_mu; 0 until the decoder's options are known */
/* Decode thread only: the key of the last successful collection, kept apart from what is shown, so a failed
   collection is retried at the next tick even though the source's key has not moved. */
static dsd_app_scan_avoid_summary g_sa_built;
static int g_sa_built_valid = 0;

#ifdef DSD_NEO_TEST_HOOKS
static void (*g_sa_test_page_pinned)(void) = NULL; /* guarded by g_feed_mu */
static int g_sa_test_block_alloc_failures = 0;     /* decode thread */
#endif

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
 * Scan avoids (issue #672)
 *============================================================================*/

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_api_feed_test_on_page_pinned(void (*hook)(void)) {
    ensure_feed_mu();
    dsd_mutex_lock(&g_feed_mu);
    g_sa_test_page_pinned = hook;
    dsd_mutex_unlock(&g_feed_mu);
}

void
dsd_api_feed_test_fail_block_allocs(int count) {
    g_sa_test_block_alloc_failures = count;
}
#endif

static api_scan_avoid_block*
scan_avoid_block_new(void) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_sa_test_block_alloc_failures > 0) {
        g_sa_test_block_alloc_failures--;
        return NULL;
    }
#endif
    return (api_scan_avoid_block*)calloc(1U, sizeof(api_scan_avoid_block));
}

static void
scan_avoid_block_release(api_scan_avoid_block* block) {
    if (block != NULL && atomic_fetch_add(&block->refs, -1) == 1) {
        dsd_app_scan_avoid_raw_free(&block->raw);
        free(block);
    }
}

/* Swap in @p block (NULL: none) and the summary to report (NULL: nothing, as before the options are known). */
static void
scan_avoids_publish(api_scan_avoid_block* block, const dsd_app_scan_avoid_summary* shown) {
    dsd_mutex_lock(&g_feed_mu);
    api_scan_avoid_block* old = g_sa_block;
    g_sa_block = block;
    if (shown != NULL) {
        g_sa_shown = *shown;
    } else {
        DSD_MEMSET(&g_sa_shown, 0, sizeof g_sa_shown);
    }
    g_sa_shown_valid = shown != NULL;
    dsd_mutex_unlock(&g_feed_mu);
    scan_avoid_block_release(old);
}

/* Report nothing and forget the built key (a block exists only with a summary shown); one flag test when already so. */
static void
scan_avoids_drop_idle(void) {
    g_sa_built_valid = 0;
    if (g_sa_shown_valid) {
        scan_avoids_publish(NULL, NULL);
    }
}

/* Once per telemetry tick: nothing beyond the view's constant-cost summary and a key compare unless the key moved
   since the last successful collection. A failed collection drops the copy, so status and `get` both report the list
   unavailable at that revision rather than an older list as current, and the next tick tries again. */
static void
scan_avoids_refresh(const dsd_state* state) {
    dsd_app_scan_avoid_summary seen;
    if (g_live_opts == NULL || dsd_app_scan_avoid_summary_get(g_live_opts, state, &seen) != 0) {
        scan_avoids_drop_idle();
        return;
    }
    if (g_sa_built_valid && dsd_app_scan_avoid_summary_same_key(&seen, &g_sa_built)) {
        return;
    }
    api_scan_avoid_block* block = scan_avoid_block_new();
    if (block != NULL && dsd_app_scan_avoid_raw_collect(g_live_opts, state, &block->raw) == 0) {
        atomic_store(&block->refs, 1);
        g_sa_built = block->raw.summary;
        g_sa_built_valid = 1;
        scan_avoids_publish(block, &block->raw.summary);
        return;
    }
    if (block != NULL) {
        dsd_app_scan_avoid_raw_free(&block->raw);
        free(block);
    }
    g_sa_built_valid = 0;
    seen.available = 0U;
    seen.total_known = 0U;
    seen.total = 0U;
    scan_avoids_publish(NULL, &seen);
}

static const char*
scan_avoid_kind_name(uint32_t kind) {
    switch (kind) {
        case DSD_APP_SCAN_AVOID_KIND_LCN: return "lcn";
        case DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN: return "trunk_scan";
        default: return "none";
    }
}

/* The summary's fields, inside an open object: what status.scan_avoids carries and every `get` page starts with. */
static void
encode_scan_avoid_summary(dsd_json_writer* w, const dsd_app_scan_avoid_summary* s) {
    (void)dsd_json_kv_bool(w, "available", s->available);
    (void)dsd_json_kv_str(w, "kind", scan_avoid_kind_name(s->kind));
    (void)(s->total_known ? dsd_json_kv_u64(w, "total", s->total) : dsd_json_kv_null(w, "total"));
    (void)dsd_json_kv_u64_str(w, "instance", s->instance);
    if (s->kind == DSD_APP_SCAN_AVOID_KIND_LCN) {
        (void)dsd_json_kv_u64_str(w, "list_rev", s->list_rev);
    } else if (s->kind == DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN) {
        (void)dsd_json_kv_u64(w, "session", s->session);
    }
    (void)dsd_json_kv_u64_str(w, "revision", s->revision);
}

/* One entry: the identity scan_avoid_remove takes, field for field, then the stored name and the view's label. */
static void
encode_scan_avoid_entry(dsd_json_writer* w, const dsd_app_scan_avoid_raw_entry* e) {
    const dsd_app_scan_avoid_remove_payload* id = &e->identity;
    char label[DSD_APP_SCAN_AVOID_LABEL_SIZE];
    (void)dsd_app_scan_avoid_raw_label(e, label, sizeof label);
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_str(w, "kind", scan_avoid_kind_name(id->kind));
    if (id->kind == DSD_APP_SCAN_AVOID_KIND_LCN) {
        (void)dsd_json_kv_u64_str(w, "instance", id->instance);
        (void)dsd_json_kv_u64_str(w, "list_rev", id->list_rev);
        (void)dsd_json_kv_i64(w, "row", id->row);
    } else {
        (void)dsd_json_kv_u64(w, "session", id->session);
        (void)dsd_json_kv_strn(w, "target_id", id->target_id, sizeof id->target_id);
    }
    (void)dsd_json_kv_u64(w, "frequency_hz", id->frequency_hz);
    (void)dsd_json_kv_strn(w, "name", e->name, sizeof e->name);
    (void)dsd_json_kv_strn(w, "label", label, sizeof label);
    (void)dsd_json_obj_end(w);
}

void
dsd_api_feed_scan_avoids_page(dsd_json_writer* w, uint64_t offset, size_t limit) {
    dsd_app_scan_avoid_summary shown;
    ensure_feed_mu();
    dsd_mutex_lock(&g_feed_mu);
    const int valid = g_sa_shown_valid;
    shown = g_sa_shown;
    api_scan_avoid_block* block = g_sa_block;
    if (block != NULL) {
        (void)atomic_fetch_add(&block->refs, 1);
    }
#ifdef DSD_NEO_TEST_HOOKS
    void (*pinned)(void) = g_sa_test_page_pinned;
#endif
    dsd_mutex_unlock(&g_feed_mu);
#ifdef DSD_NEO_TEST_HOOKS
    if (pinned != NULL) {
        pinned();
    }
#endif
    if (!valid) {
        (void)dsd_json_value_null(w);
        scan_avoid_block_release(block);
        return;
    }
    (void)dsd_json_obj_begin(w);
    encode_scan_avoid_summary(w, &shown);
    (void)dsd_json_kv_u64(w, "offset", offset);
    (void)dsd_json_kv_u64(w, "limit", limit);
    (void)dsd_json_key(w, "entries");
    (void)dsd_json_arr_begin(w);
    const size_t count = block != NULL ? block->raw.count : 0U;
    for (uint64_t i = offset; i < count && i - offset < limit; i++) {
        encode_scan_avoid_entry(w, &block->raw.entries[i]);
    }
    (void)dsd_json_arr_end(w);
    (void)dsd_json_obj_end(w);
    scan_avoid_block_release(block);
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

/* The row's site parts by name, each only while its valid bit is set (Tier III's network and site numbers raw, as the
   record keeps them; Capacity Plus has a site and no network). */
static void
encode_site_p25(dsd_json_writer* w, const dsd_received_identity* id) {
    if ((id->valid & DSD_RXID_P25_WACN) != 0U) {
        (void)dsd_json_kv_u64(w, "wacn", id->p25_wacn);
    }
    if ((id->valid & DSD_RXID_P25_SYSID) != 0U) {
        (void)dsd_json_kv_u64(w, "sysid", id->p25_sysid);
    }
    if ((id->valid & DSD_RXID_P25_SITE) != 0U) {
        (void)dsd_json_kv_u64(w, "rfss", id->p25_rfss);
        (void)dsd_json_kv_u64(w, "site", id->p25_site);
    }
    if ((id->valid & DSD_RXID_P25_LRA) != 0U) {
        (void)dsd_json_kv_u64(w, "lra", id->p25_lra);
    }
}

static void
encode_site_dmr(dsd_json_writer* w, const dsd_received_identity* id) {
    if ((id->valid
         & (DSD_RXID_DMR_SITE | DSD_RXID_DMR_NET | DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_SITE_TEXT
            | DSD_RXID_DMR_BRAND))
        != 0U) {
        (void)dsd_json_kv_u64(w, "dmr_trunk", id->dmr_trunk);
    }
    if ((id->valid & DSD_RXID_DMR_SITE) != 0U && id->dmr_trunk == (uint8_t)DSD_RXID_DMR_TRUNK_TIER3) {
        (void)dsd_json_kv_u64(w, "dmr_t3_model", id->dmr_t3_model);
    }
    if ((id->valid & DSD_RXID_DMR_NET) != 0U) {
        (void)dsd_json_kv_u64(w, "dmr_net", id->dmr_net);
    }
    if ((id->valid & DSD_RXID_DMR_SITE) != 0U) {
        (void)dsd_json_kv_u64(w, "dmr_site", id->dmr_site);
    }
    if ((id->valid & DSD_RXID_DMR_T3_SYSCODE) != 0U) {
        (void)dsd_json_kv_u64(w, "dmr_t3_syscode", id->dmr_t3_syscode);
    }
    if ((id->valid & DSD_RXID_DMR_SITE_TEXT) != 0U) {
        (void)dsd_json_kv_strn(w, "dmr_site_text", id->dmr_site_text, sizeof id->dmr_site_text);
    }
    if ((id->valid & DSD_RXID_DMR_BRAND) != 0U) {
        (void)dsd_json_kv_strn(w, "dmr_brand", id->dmr_brand, sizeof id->dmr_brand);
    }
}

static void
encode_site_nxdn(dsd_json_writer* w, const dsd_received_identity* id) {
    if ((id->valid & DSD_RXID_NXDN_LOCATION) != 0U) {
        (void)dsd_json_kv_u64(w, "nxdn_category", id->nxdn_category);
        (void)dsd_json_kv_u64(w, "nxdn_sys", id->nxdn_sys);
        (void)dsd_json_kv_u64(w, "nxdn_site", id->nxdn_site);
    }
    if ((id->valid & DSD_RXID_NXDN_IDAS_AREA) != 0U) {
        (void)dsd_json_kv_u64(w, "idas_area", id->nxdn_idas_area);
    }
    if ((id->valid & DSD_RXID_NXDN_IDAS_SITE) != 0U) {
        (void)dsd_json_kv_u64(w, "idas_site_type", id->nxdn_idas_site_type);
        (void)dsd_json_kv_u64(w, "idas_site_code", id->nxdn_idas_site_code);
    }
    if ((id->valid & DSD_RXID_NXDN_TYPED_SYSTEM) != 0U) {
        (void)dsd_json_kv_u64(w, "nxdn_typed_sys", id->nxdn_typed_sys);
        (void)dsd_json_kv_u64(w, "nxdn_typed_site", id->nxdn_typed_site);
    }
}

static void
encode_site_edacs_tetra(dsd_json_writer* w, const dsd_received_identity* id) {
    const uint32_t edacs = DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_AREA | DSD_RXID_EDACS_SYSTEM;
    if ((id->valid & DSD_RXID_EDACS_SITE) != 0U) {
        (void)dsd_json_kv_u64(w, "edacs_site", id->edacs_site);
    }
    if ((id->valid & DSD_RXID_EDACS_AREA) != 0U) {
        (void)dsd_json_kv_u64(w, "edacs_area", id->edacs_area);
    }
    if ((id->valid & DSD_RXID_EDACS_SYSTEM) != 0U) {
        (void)dsd_json_kv_u64(w, "edacs_system", id->edacs_system);
    }
    if ((id->valid & edacs) != 0U) {
        (void)dsd_json_kv_i64(w, "edacs_ea_mode", id->edacs_ea_mode);
    }
    if ((id->valid & DSD_RXID_TETRA_NET) != 0U) {
        (void)dsd_json_kv_u64(w, "tetra_mcc", id->tetra_mcc);
        (void)dsd_json_kv_u64(w, "tetra_mnc", id->tetra_mnc);
        (void)dsd_json_kv_u64(w, "tetra_colour", id->tetra_colour);
    }
    if ((id->valid & DSD_RXID_TETRA_LA) != 0U) {
        (void)dsd_json_kv_u64(w, "tetra_la", id->tetra_la);
    }
}

static const char*
channel_kind_name(uint8_t kind) {
    switch (kind) {
        case DSD_CALL_CHANNEL_LSN: return "lsn";
        case DSD_CALL_CHANNEL_LPCN: return "lpcn";
        case DSD_CALL_CHANNEL_LCN: return "lcn";
        default: return "none";
    }
}

/* The site the row was heard on and the numbering of its channel, after the keys a client already reads. */
static void
encode_event_site(dsd_json_writer* w, const Event_History* e) {
    const dsd_received_identity* id = &e->site_id;
    (void)dsd_json_kv_str(w, "channel_kind", channel_kind_name(e->call_channel_kind));
    (void)dsd_json_key(w, "site");
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_u64(w, "family", id->family);
    encode_site_p25(w, id);
    encode_site_dmr(w, id);
    encode_site_nxdn(w, id);
    encode_site_edacs_tetra(w, id);
    (void)dsd_json_obj_end(w);
    char text[DSD_APP_SITE_HISTORY_TEXT_SIZE];
    text[0] = '\0';
    (void)dsd_app_site_history_text(id, e->call_channel_kind, e->channel, text, sizeof text);
    (void)dsd_json_kv_str(w, "site_text", text);
}

/* An analog call's mode as the API spells it (issue #667): "nfm" or "am" as the demodulator ran, "unknown" on audio
   input; "" for any other protocol, whose mode byte means nothing. */
static const char*
api_analog_mode(int protocol, uint8_t mode) {
    if (protocol != DSD_SYNC_ANALOG) {
        return "";
    }
    switch (mode) {
        case DSD_ANALOG_MODE_NFM: return "nfm";
        case DSD_ANALOG_MODE_AM: return "am";
        default: return "unknown";
    }
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
    // Two flags rather than the raw bits: crc_invalid kept its meaning (a check failed) when RAS was split out.
    (void)dsd_json_kv_i64(w, "crc_invalid", dsd_crc_is_failed(e->crc_invalid) ? 1 : 0);
    (void)dsd_json_kv_i64(w, "crc_ras", (e->crc_invalid & DSD_CRC_RAS_UNVERIFIED) != 0U ? 1 : 0);
    (void)dsd_json_kv_u64(w, "sys_id1", e->sys_id1);
    (void)dsd_json_kv_u64(w, "sys_id2", e->sys_id2);
    (void)dsd_json_kv_u64(w, "sys_id3", e->sys_id3);
    (void)dsd_json_kv_u64(w, "sys_id4", e->sys_id4);
    (void)dsd_json_kv_u64(w, "sys_id5", e->sys_id5);
    (void)dsd_json_kv_strn(w, "sysid_string", e->sysid_string, sizeof e->sysid_string);
    (void)dsd_json_kv_str(w, "analog_mode", api_analog_mode(e->systype, e->analog_mode));
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
    (void)dsd_json_kv_strn(w, "service_label", e->service_label, sizeof e->service_label);
    (void)dsd_json_kv_u64(w, "receive_kind", e->receive_identity.kind);
    (void)dsd_json_kv_u64(w, "receive_value", e->receive_identity.value);
    (void)dsd_json_kv_bool(w, "receive_known", e->receive_identity.known);
    (void)dsd_json_kv_u64(w, "receive_slot", e->receive_identity.slot);
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
    encode_event_site(w, e);
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

/* The site parts a row reports (the keys encode_event_site() writes), by value. */
static uint64_t
fnv1a_site(uint64_t h, const dsd_received_identity* id) {
    const uint64_t parts[] = {
        id->family,
        id->valid,
        id->p25_wacn,
        id->p25_sysid,
        id->p25_rfss,
        id->p25_site,
        id->p25_lra,
        id->dmr_trunk,
        id->dmr_t3_model,
        id->dmr_net,
        id->dmr_site,
        id->dmr_t3_syscode,
        id->nxdn_category,
        id->nxdn_sys,
        id->nxdn_site,
        id->nxdn_idas_area,
        id->nxdn_idas_site_type,
        id->nxdn_idas_site_code,
        id->nxdn_typed_sys,
        id->nxdn_typed_site,
        id->edacs_site,
        id->edacs_area,
        id->edacs_system,
        (uint64_t)(int64_t)id->edacs_ea_mode,
        id->tetra_mcc,
        id->tetra_mnc,
        id->tetra_colour,
        id->tetra_la,
    };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        h = fnv1a_u64(h, parts[i]);
    }
    h = fnv1a_str(h, id->dmr_site_text, sizeof id->dmr_site_text);
    return fnv1a_str(h, id->dmr_brand, sizeof id->dmr_brand);
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
        e->analog_mode,
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
    h = fnv1a_str(h, e->service_label, sizeof e->service_label);
    h = fnv1a_byte(h, e->receive_identity.kind);
    h = fnv1a_byte(h, e->receive_identity.known);
    h = fnv1a_byte(h, e->receive_identity.slot);
    h = fnv1a_byte(h, (unsigned char)e->receive_identity.value);
    h = fnv1a_byte(h, (unsigned char)(e->receive_identity.value >> 8));
    h = fnv1a_str(h, e->alias, sizeof e->alias);
    h = fnv1a_str(h, e->gps_s, sizeof e->gps_s);
    h = fnv1a_str(h, e->text_message, sizeof e->text_message);
    h = fnv1a_str(h, e->event_string, sizeof e->event_string);
    h = fnv1a_str(h, e->internal_str, sizeof e->internal_str);
    h = fnv1a_byte(h, e->call_channel_kind);
    return fnv1a_site(h, &e->site_id);
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
    blank.receive_identity.kind = (uint8_t)DSD_RECEIVE_NONE; /* Kind "none", never kind 0 (slot) (issue #637). */
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
    /* Issue #773: the parties apart from the display strings above -- the source radio's id and label, the target's own
       label -- and the protocol's display name. The call kind keeps its one "kind" key below. */
    (void)dsd_json_kv_u64(w, "src_id", view.src_id);
    (void)dsd_json_kv_strn(w, "src_label", view.src_label, sizeof view.src_label);
    (void)dsd_json_kv_strn(w, "tg_label", view.tg_label, sizeof view.tg_label);
    (void)dsd_json_kv_strn(w, "protocol_label", view.protocol_label, sizeof view.protocol_label);
    dsd_call_snapshot snap;
    DSD_MEMSET(&snap, 0, sizeof snap); /* read only when has_call, which MSVC's code generation cannot follow (C4701) */
    const int has_call = (view.state == DSD_APP_CALL_LINE_ACTIVE || view.state == DSD_APP_CALL_LINE_ENDED)
                         && dsd_call_state_get(state, (uint8_t)slot, &snap) > 0;
    (void)dsd_json_kv_str(w, "analog_mode", has_call ? api_analog_mode(snap.protocol, snap.analog_mode) : "");
    (void)dsd_json_kv_strn(w, "channel", view.channel, sizeof view.channel);
    (void)dsd_json_kv_strn(w, "service_label", view.service, sizeof view.service);
    (void)dsd_json_kv_u64(w, "receive_kind", view.receive_identity.kind);
    (void)dsd_json_kv_u64(w, "receive_value", view.receive_identity.value);
    (void)dsd_json_kv_bool(w, "receive_known", view.receive_identity.known);
    (void)dsd_json_kv_u64(w, "tg_id", view.tg_id);
    (void)dsd_json_kv_u64(w, "elapsed_ms", view.elapsed_ms);
    (void)dsd_json_kv_u64(w, "kid", view.kid);
    (void)dsd_json_kv_u64(w, "algid", view.algid);
    (void)dsd_json_kv_bool(w, "emergency", view.emergency);
    (void)dsd_json_kv_u64(w, "priority", view.priority);
    (void)dsd_json_kv_bool(w, "enc", view.enc);
    if (has_call) {
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

/* The receive policy (issue #637): its verdict on current reception, and the configured filters and naming table that
   receive_set edits, in receive_set's own spelling, so a client can read a filter or the table before it replaces it. */
static void
encode_receive(dsd_json_writer* w, const dsd_opts* opts, const dsd_state* state) {
    char status[128];
    dsd_app_receive_status_text(opts, state, status, sizeof status);
    dsd_receive_policy policy;
    dsd_scan_mode_configured_receive(opts, state, &policy);
    (void)dsd_json_key(w, "receive");
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_strn(w, "status", status, sizeof status);
    (void)dsd_json_key(w, "filters");
    (void)dsd_json_arr_begin(w);
    for (int kind = 0; kind < DSD_RECEIVE_FILTER_COUNT; kind++) {
        char allow[DSD_RECEIVE_LIST_MAX + 1];
        char block[DSD_RECEIVE_LIST_MAX + 1];
        (void)dsd_receive_set_format(kind, &policy.filters[kind].allow, allow, sizeof allow);
        (void)dsd_receive_set_format(kind, &policy.filters[kind].block, block, sizeof block);
        (void)dsd_json_obj_begin(w);
        (void)dsd_json_kv_i64(w, "kind", kind);
        /* 0/1, as receive_set takes it, so a client can send a filter back as it read it. */
        (void)dsd_json_kv_i64(w, "enabled", policy.filters[kind].enabled ? 1 : 0);
        (void)dsd_json_kv_strn(w, "allow", allow, sizeof allow);
        (void)dsd_json_kv_strn(w, "block", block, sizeof block);
        (void)dsd_json_obj_end(w);
    }
    (void)dsd_json_arr_end(w);
    char names[DSD_RECEIVE_NAMES_TEXT_MAX + 1];
    (void)dsd_receive_names_format(&policy.names, names, sizeof names);
    (void)dsd_json_kv_strn(w, "names", names, sizeof names);
    (void)dsd_json_obj_end(w);
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
    if (g_live_opts != NULL) {
        encode_receive(w, g_live_opts, state);
    } else {
        (void)dsd_json_kv_null(w, "receive");
    }
}

/* The avoid list's summary as the feed last collected it (the decode thread is its only writer, so it reads it without
   the lock); the entries are paged through `get {"what":"scan_avoids"}`. Null until the options are known. */
static void
encode_scan_avoids(dsd_json_writer* w) {
    if (!g_sa_shown_valid) {
        (void)dsd_json_kv_null(w, "scan_avoids");
        return;
    }
    (void)dsd_json_key(w, "scan_avoids");
    (void)dsd_json_obj_begin(w);
    encode_scan_avoid_summary(w, &g_sa_shown);
    (void)dsd_json_obj_end(w);
}

/* The --trunk-scan target lists (issue #697), from the scan list view: the session and the list sequence a
   scan_list_set and its result quote, the list that names the target on air (the lowest-numbered list holding it that
   is on, else the lowest holding it; null when none is on air), and each list in list order: the targets its file
   holds, shared ones included, and whether it holds the target on air. With no trunk scan: session 0, list null, no
   lists. */
static void
encode_scan_lists(dsd_json_writer* w, const dsd_app_scan_list_view* view) {
    (void)dsd_json_kv_u64(w, "session", view->session);
    (void)dsd_json_kv_u64(w, "lists_seq", view->lists_seq);
    (void)(view->active_list != 0U ? dsd_json_kv_u64(w, "list", view->active_list) : dsd_json_kv_null(w, "list"));
    (void)dsd_json_key(w, "lists");
    (void)dsd_json_arr_begin(w);
    for (uint8_t i = 0; i < view->list_count; i++) {
        const dsd_app_scan_list_entry* e = &view->lists[i];
        (void)dsd_json_obj_begin(w);
        (void)dsd_json_kv_u64(w, "number", e->number);
        (void)dsd_json_kv_strn(w, "name", e->name, sizeof e->name);
        /* 0/1, as scan_list_set takes it. */
        (void)dsd_json_kv_i64(w, "enabled", e->enabled);
        (void)dsd_json_kv_u64(w, "targets", e->targets);
        (void)dsd_json_kv_bool(w, "has_active", e->has_active);
        (void)dsd_json_obj_end(w);
    }
    (void)dsd_json_arr_end(w);
}

/* The operator's hold on the row or target on air (scan_hold_toggle), and whether it is an entry hold, the hold a
   frequency typed by hand set, which scanner_toggle and scan_resume continue (issue #644); then the trunk scan's
   position as every frontend shows it (issue #697): the target on air's place among the targets of the lists switched
   on (null while every list holding it is off, or none is on air) and their count, avoided targets included and a
   target several lists hold counted once. */
static void
encode_scan_position(dsd_json_writer* w, const dsd_app_scan_state_view* scan, const dsd_state* state) {
    const int trunk = scan->kind == (uint8_t)DSD_APP_SCAN_KIND_TRUNK;
    uint8_t hold = 0U;
    if (trunk) {
        hold = state->trunk_scan_hold;
    } else if (scan->kind == (uint8_t)DSD_APP_SCAN_KIND_CHANNEL) {
        hold = state->lcn_scan_hold;
    }
    (void)dsd_json_kv_bool(w, "hold", hold != 0U);
    (void)dsd_json_kv_bool(w, "entry_hold", hold != 0U && scan->entry_hold != 0U);
    const unsigned int ordinal = trunk ? state->trunk_scan_rotation_ordinal : 0U;
    (void)(ordinal != 0U ? dsd_json_kv_u64(w, "ordinal", ordinal) : dsd_json_kv_null(w, "ordinal"));
    (void)dsd_json_kv_u64(w, "count", trunk ? state->trunk_scan_rotation_count : 0U);
}

/* Which scan the session has and whether it runs or the operator paused it (issue #671), as every frontend shows it:
   "scan_resume" resumes a paused one; then its hold and position, and its target lists. Null until the decoder has
   published its options. */
static void
encode_scan(dsd_json_writer* w, const dsd_state* state) {
    dsd_app_scan_state_view scan;
    dsd_app_scan_list_view lists;
    if (g_live_opts == NULL || dsd_app_scan_state_view_get(g_live_opts, state, &scan) != 0
        || dsd_app_scan_list_view_get(g_live_opts, state, &lists) != 0) {
        (void)dsd_json_kv_null(w, "scan");
        return;
    }
    (void)dsd_json_key(w, "scan");
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_str(w, "kind", dsd_app_scan_kind_name(scan.kind));
    (void)dsd_json_kv_str(w, "state", dsd_app_scan_state_name(&scan));
    (void)dsd_json_kv_u64(w, "targets", scan.target_count);
    encode_scan_position(w, &scan, state);
    encode_scan_lists(w, &lists);
    (void)dsd_json_obj_end(w);
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
    encode_scan(&w, state);
    encode_scan_avoids(&w);
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

/* Forget every cached record and the scan avoid copy: `get` answers null until the next update, which the next publish
   with a client runs at once. */
static void
feed_cache_drop(void) {
    dsd_mutex_lock(&g_feed_mu);
    for (int i = 0; i < DSD_API_TOPIC_COUNT; i++) {
        dsd_json_buf_free(&g_latest[i]);
    }
    dsd_mutex_unlock(&g_feed_mu);
    scan_avoids_drop_idle();
    g_cache_live = 0;
    g_last_telemetry_ms = 0U;
}

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
        /* Nothing is refreshed without a client, so nothing is kept that a later client could take for current. */
        if (g_cache_live) {
            feed_cache_drop();
        }
        return;
    }
    /* A restarted decoder (a new state instance): no record of the previous run is served, and this publish runs the
       update. One compare per publish. */
    if (g_cache_live && state->state_instance != g_cache_instance) {
        feed_cache_drop();
    }
    const uint64_t now = dsd_realtime_mono_ms();
    if (g_last_telemetry_ms != 0U && now - g_last_telemetry_ms < (uint64_t)DSD_API_TELEMETRY_INTERVAL_MS) {
        return;
    }
    g_last_telemetry_ms = now;
    g_cache_live = 1;
    g_cache_instance = state->state_instance;
    /* Every get-able topic is kept current while a client is connected, so a one-shot get never waits. */
    scan_avoids_refresh(state);
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
    g_sa_built_valid = 0;
    g_cache_live = 0;
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
    /* A session still formatting a page keeps its own reference to the dropped copy. */
    scan_avoids_publish(NULL, NULL);
    g_sa_built_valid = 0;
}
