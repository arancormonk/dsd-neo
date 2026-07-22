// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Core event logging helpers.
 *
 * Declares event-history helpers implemented in core.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_EVENTS_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_EVENTS_H_H

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#ifdef DSD_NEO_EVENT_HISTORY_TRANSACTIONS
#include <dsd-neo/platform/threading.h>
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void init_event_history(Event_History_I* event_struct, uint8_t start, uint8_t stop);
void push_event_history(Event_History_I* event_struct);

/** Opaque guard for serializing event-history mutations with telemetry snapshots. */
typedef struct {
    void* opaque;
} dsd_event_history_transaction;

static inline void
dsd_event_history_transaction_begin(dsd_state* state, dsd_event_history_transaction* transaction) {
    if (transaction == NULL) {
        return;
    }
    transaction->opaque = state != NULL ? state->state_ext[DSD_STATE_EXT_CORE_CALL_STATE] : NULL;
#ifdef DSD_NEO_EVENT_HISTORY_TRANSACTIONS
    if (transaction->opaque != NULL) {
        (void)dsd_mutex_lock((dsd_mutex_t*)transaction->opaque);
    }
#endif
}

static inline void
dsd_event_history_transaction_end(dsd_event_history_transaction* transaction) {
    if (transaction == NULL || transaction->opaque == NULL) {
        return;
    }
#ifdef DSD_NEO_EVENT_HISTORY_TRANSACTIONS
    (void)dsd_mutex_unlock((dsd_mutex_t*)transaction->opaque);
#endif
    transaction->opaque = NULL;
}

static inline void
dsd_event_history_mark_dirty(Event_History_I* event_struct) {
    if (event_struct == NULL) {
        return;
    }

    event_struct->revision++;
    if (event_struct->revision == 0U) {
        event_struct->revision = 1U;
    }
}

static inline void
dsd_event_history_item_set_metadata(Event_History* item, dsd_event_severity severity, dsd_event_category category) {
    if (item == NULL) {
        return;
    }
    item->severity = (uint8_t)severity;
    item->category = (uint8_t)category;
}

static inline void
dsd_event_history_item_copy_text(char* dst, size_t cap, const char* src) {
    if (dst == NULL || cap == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0U;
    while (i + 1U < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static inline void
dsd_event_history_item_set_message(Event_History* item, dsd_event_severity severity, dsd_event_category category,
                                   const char* summary, const char* detail) {
    dsd_event_history_item_set_metadata(item, severity, category);
    if (item == NULL) {
        return;
    }
    if (summary != NULL) {
        dsd_event_history_item_copy_text(item->event_string, sizeof item->event_string, summary);
    }
    if (detail != NULL) {
        dsd_event_history_item_copy_text(item->internal_str, sizeof item->internal_str, detail);
    }
}

void write_event_to_log_file(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t swrite, char* event_string);
void watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot);
void watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot);
void dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot);
int dsd_event_emit_call_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                               const char* detail);
/** Commit an informational call notice without call-end alerts, WAV rotation, or slot-identity reset. */
int dsd_event_emit_call_notice_nonfinalizing(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                             const dsd_call_snapshot* call, const char* detail);
int dsd_event_history_copy_snapshot(const dsd_state* state, Event_History_I out[2]);
/* Copy telemetry atomically, copying history slots only when forced or when their source revision changed. */
int dsd_event_state_copy_snapshot_incremental(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2],
                                              const uint64_t source_revisions[2], int force_copy, uint8_t copied[2]);
int dsd_event_state_copy_snapshot(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2]);
void watchdog_event_status(dsd_state* state, const char* status_string, uint8_t slot);
int dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                               const char* notice);
/** Commit an informational system notice without attributing it to radio traffic. */
int dsd_event_emit_system_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const char* notice);
int dsd_event_enrich_alias(dsd_state* state, uint8_t slot, uint64_t epoch, const char* alias);
int dsd_event_enrich_gps(dsd_state* state, uint8_t slot, uint64_t epoch, const char* gps);
int dsd_event_enrich_text(dsd_state* state, uint8_t slot, uint64_t epoch, const char* text);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_EVENTS_H_H */
