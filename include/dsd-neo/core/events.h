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
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
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

// Only the two-slot protocols annotate their log lines with a slot number. X2-TDMA belongs here for
// the same reason the other two do: it carries two timeslots and its callers attribute every
// observation through state->currentslot, so a log line without the annotation is ambiguous.
static inline int
dsd_event_systype_has_slots(int systype) {
    return DSD_SYNC_IS_DMR_BS(systype) || DSD_SYNC_IS_P25P2(systype) || DSD_SYNC_IS_X2TDMA(systype);
}

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

/**
 * Stage a data PDU's text/GPS independently of the active call's history row.
 * Replace operations accept NULL or "" to clear; append ignores NULL. Clear resets both fields.
 * Writers are no-ops without history or for a slot outside DSD_CALL_STATE_SLOT_COUNT, and
 * getters return "" in those cases. Staging alone does not change history revisions.
 * Writers take the history transaction internally: never call them while holding a history
 * transaction (the mutex is non-recursive).
 * Getters return borrowed, decoder-thread-owned views into state, invalidated by the next
 * staging write, consumption or reset. They are not telemetry snapshot fields.
 */
void dsd_event_stage_text(dsd_state* state, uint8_t slot, const char* text);
void dsd_event_stage_text_append(dsd_state* state, uint8_t slot, const char* text);
void dsd_event_stage_gps(dsd_state* state, uint8_t slot, const char* gps);
const char* dsd_event_staged_text(const dsd_state* state, uint8_t slot);
const char* dsd_event_staged_gps(const dsd_state* state, uint8_t slot);
void dsd_event_stage_clear(dsd_state* state, uint8_t slot);
/**
 * Stage an admitted position report and its text for the slot's next data notice (dsd_event_emit_data_notice() or
 * dsd_event_emit_data_notice_classified()), replacing any staged GPS text and position: the two are staged together.
 * dsd_event_stage_gps() stages text without a position and clears a staged one. @p fix must be the record
 * dsd_position_admit() returned for an admitted report (its rev is set); @p gps_text is the text shown as the row's
 * GPS. The notice checks the report's typed source against its own observation when it emits (see
 * dsd_event_emit_data_notice_with_position()). Takes the history transaction internally.
 *
 * @return 1 when staged, -1 for invalid arguments (NULL, no history, a slot outside DSD_CALL_STATE_SLOT_COUNT, or a
 *         report that was not admitted), which leaves the staging alone.
 */
int dsd_event_stage_position(dsd_state* state, uint8_t slot, const dsd_position_fix* fix, const char* gps_text);
/** The admitted report staged for the slot's next data notice; a record with rev 0 when none is (or without history or
 * for an invalid slot). Borrowed like the other staging getters. */
const dsd_position_fix* dsd_event_staged_position(const dsd_state* state, uint8_t slot);

void write_event_to_log_file(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t swrite,
                             const char* event_string);
void watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot);
/**
 * Render the slot's canonical call into its staged row, opening the call's epoch first (committing the outgoing epoch's
 * staged row as it stands) when no sync has opened it yet.
 */
void watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot);
/**
 * Attach a note (internal_str: a lockout, a skip) to the slot's canonical call and render it.
 *
 * The call's epoch is opened first, so a note written right after a protocol observed a new call lands on that call's
 * row rather than on the outgoing epoch's. The note is declined when the slot has no call to carry it: no call has been
 * observed, or the call ended and already committed its row (an ended call still finalizing takes it). Written into the
 * blank staged row instead, it would become a note-only row when the next call opened. Use this rather than writing
 * Items[0].internal_str and calling watchdog_event_current(). Callers must not hold an event-history transaction.
 */
void dsd_event_note_current_call(dsd_opts* opts, dsd_state* state, uint8_t slot, const char* note);

/** The decoded per-call detail a protocol writes into the slot's call row. */
typedef enum {
    DSD_EVENT_DETAIL_ALIAS = 0, /**< Talker alias (Event_History::alias). */
    DSD_EVENT_DETAIL_GPS = 1,   /**< Position report (Event_History::gps_s). */
    DSD_EVENT_DETAIL_TEXT = 2,  /**< Text message (Event_History::text_message). */
} dsd_event_detail_field;

/**
 * Write a protocol's decoded detail into the row of the slot's open call.
 *
 * Written only while the slot's canonical call is active and its epoch has been opened, so the staged row is that
 * call's own. Otherwise the detail is declined: decoded before any call for the transmission was observed (an RF
 * header that failed its CRC, late entry, an alias ahead of the call message), or after the call ended, it has no row
 * to go to, and written into the staged row it would become a detail-only row or land on another call's row. Slow data
 * and aliases repeat, so declined detail lands once the call is open. Callers must not hold an event-history
 * transaction.
 *
 * @return 1 when written, 0 when declined, -1 for invalid arguments.
 */
int dsd_event_set_open_call_detail(dsd_state* state, uint8_t slot, dsd_event_detail_field field, const char* value);
void dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot);
/**
 * Re-judge the rdio-scanner export of slot @p slot's staged row now, as its next render would (issue #773 D7), so a
 * recording closed before that render -- a WAV stop or another pre-commit close in the same command drain as a policy
 * import or edit -- follows the policy in force at the close. Only the slot's call's own staged row changes. Callers
 * must not hold the call-state lock.
 */
void dsd_event_refresh_stream_verdict(const dsd_opts* opts, dsd_state* state, uint8_t slot);
int dsd_event_emit_call_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                               const char* detail);
/** Commit an informational call notice without call-end alerts, WAV rotation, or slot-identity reset. */
int dsd_event_emit_call_notice_nonfinalizing(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                             const dsd_call_snapshot* call, const char* detail);
int dsd_event_history_copy_snapshot(const dsd_state* state, Event_History_I out[2]);
/**
 * Retire any VOICE_END alert still being held open against a possible reacquisition, on both
 * slots, without waiting for its deadline.
 *
 * For callers that know no further audio can arrive -- shutdown being the only one today. The
 * per-frame drain in dsd_event_sync_slot() only fires once the reacquisition window has elapsed,
 * so an end armed in the last half second before exit would otherwise never be heard.
 */
void dsd_event_flush_pending_alerts(dsd_opts* opts, dsd_state* state);
/**
 * Close the per-call recordings no call owns as the receiver leaves its carrier (issue #809 B4).
 *
 * For each slot whose per-call WAV holds audio while the slot has no active canonical call (speech heard before any
 * signalling admitted a call, a TETRA anonymous call's lead-in), the file is closed, published without an rdio-scanner
 * export and reopened, as a commit rotates it: pending on the slot's ended epoch whose row has yet to commit (a held
 * drop verdict), which that row's commit links and a drop unlinks, else as no row's (nothing names it, as on the drop
 * path). Without it the next carrier's audio would join the same file. A slot with an active call is left to its own
 * commit, so a call that continues across the move as one call (a TETRA reallocation) stays one recording. An empty
 * file, per-call recording off and a static WAV are left alone. Run at every carrier departure, after the departure
 * ended its calls. The check and the close run under the call-state lock, which every commit holds while it rotates
 * a slot, so a flush on the P25 watchdog's thread never closes a file a decoder-thread commit closes too.
 * Callers must not hold the call-state lock or an event-history transaction.
 */
void dsd_event_flush_orphan_recordings(dsd_opts* opts, dsd_state* state);
/**
 * Clear every history row, staged data payload and per-slot commit bookkeeping on both slots.
 *
 * Callers must not hold an event-history transaction: this opens its own. Clearing the rows
 * without clearing the bookkeeping would leave a reacquired transmission trying to merge into
 * a row that no longer exists.
 */
void dsd_event_history_reset(dsd_state* state);
/* Copy telemetry atomically, copying history slots only when forced or when their source revision changed. */
int dsd_event_state_copy_snapshot_incremental(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2],
                                              const uint64_t source_revisions[2], int force_copy, uint8_t copied[2]);
int dsd_event_state_copy_snapshot(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2]);
void watchdog_event_status(dsd_state* state, const char* status_string, uint8_t slot);
/**
 * Commit a data/control notice using neutral event metadata, consuming the slot's staged data payload.
 *
 * Only DSD_EVENT_CATEGORY_DATA and DSD_EVENT_CATEGORY_CONTROL are accepted.
 * Invalid categories are rejected without changing event history.
 */
int dsd_event_emit_data_notice_classified(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                          const dsd_call_observation* observation, dsd_event_category category,
                                          const char* notice);
/**
 * Commit a data/control notice with GPS owned by the new event, leaving the staged payload and active row unchanged.
 *
 * Only DSD_EVENT_CATEGORY_DATA and DSD_EVENT_CATEGORY_CONTROL are accepted.
 * Invalid categories or a NULL GPS value are rejected without changing event history.
 */
int dsd_event_emit_data_notice_classified_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                                   const dsd_call_observation* observation, dsd_event_category category,
                                                   const char* notice, const char* gps);
/**
 * Commit a data/control notice whose GPS is an admitted position report: the row takes @p gps_text and the report's
 * position together. The staged payload and the active row are left unchanged.
 *
 * Build the observation from the same report (ota_source_id = fix->source_id, source_text = fix->source_text, or the
 * same checked header the report's identity came from), and stage or emit right after admission. The report must be of
 * the notice's protocol family, and then:
 * - a report whose identity admission cleared (DSD_POS_FLAG_SOURCE_UNVERIFIED or _HASHED) makes the notice anonymous:
 *   the row's source and target (IDs and texts) are cleared and it keeps the position. Leave that source out of
 *   @p notice too;
 * - otherwise the report's source must be the notice's own: the same numeric source (observation->ota_source_id) when
 *   it names one, the same callsign (observation->source_text) when it names one, and a report naming neither only
 *   with an observation naming no source. Any other report leaves the notice without GPS text and without a position
 *   (the notice itself is still committed).
 *
 * Every data notice (this one, the staged ones and the text-only GPS ones) stamps where it was heard
 * (Event_History::rx_scope: the report's reception scope when it carries a position, else the live record's scope;
 * nothing playing back files; none for M17 and D-STAR), its sender identity (Event_History::pos.scope: the report's
 * sender scope, else the reception scope) and its site (Event_History::site_id) by the rule a call row's site follows
 * (none playing back files, for M17, dPMR and a TETRA direct mode carrier).
 *
 * @return 0 when handled (committed, or declined by the receive policy), -1 for invalid arguments: those of
 *         dsd_event_emit_data_notice_classified_with_gps(), a NULL @p fix or @p gps_text, or a report that was not
 *         admitted.
 */
int dsd_event_emit_data_notice_with_position(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                             const dsd_call_observation* observation, dsd_event_category category,
                                             const char* notice, const dsd_position_fix* fix, const char* gps_text);
/** Commit a data notice. Equivalent to dsd_event_emit_data_notice_classified() with the DATA category. */
int dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                               const char* notice);
/** Commit a data notice with GPS. Equivalent to the classified GPS API with the DATA category. */
int dsd_event_emit_data_notice_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                        const dsd_call_observation* observation, const char* notice, const char* gps);
/** Commit an informational system notice without attributing it to radio traffic. */
int dsd_event_emit_system_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const char* notice);
int dsd_event_enrich_alias(dsd_state* state, uint8_t slot, uint64_t epoch, const char* alias);
/**
 * dsd_event_enrich_alias() with the options the event log is written through. An alias that lands on the epoch's
 * already committed row (a Motorola alias sent in hangtime) also re-resolves that row's source label through the
 * source-alias/group-list precedence -- so a policy row the alias just taught names the row it arrived for, and an
 * imported name keeps precedence -- re-renders the row, caches the label on the call (dsd_call_snapshot::source_label)
 * and appends one "Enriched:" continuation to the event log carrying the row's own stamp. dsd_event_enrich_alias()
 * does the same without the log line. Call it after the alias has updated the learned policy row.
 */
int dsd_event_enrich_alias_ex(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint64_t epoch, const char* alias);
/** Text-only GPS enrichment of the epoch's row (no structured position): the row's position, if it had one, is
 * dropped with the text it replaces. Producers of position reports use dsd_event_enrich_position(). */
int dsd_event_enrich_gps(dsd_state* state, uint8_t slot, uint64_t epoch, const char* gps);
int dsd_event_enrich_text(dsd_state* state, uint8_t slot, uint64_t epoch, const char* text);

/**
 * @brief Enrich the row of the slot's ACTIVE call of epoch @p epoch with an admitted position report: the row's GPS
 * text becomes @p gps_text and its position the report's, in one step.
 *
 * Written only when all of these hold (else declined: only the CRC provenance below may change):
 * - the slot's canonical call is epoch @p epoch, ACTIVE, and not held or rejected by the receive policy;
 * - the epoch has a row: its staged row once a sync opened the epoch, or the row it already committed (a mid-call
 *   notice). An epoch no sync has opened yet takes only the CRC provenance, as dsd_event_enrich_gps() does;
 * - the report's typed source is compatible with the call's: the same protocol family; the same numeric source
 *   (dsd_call_snapshot::ota_source_id) when the report names one (a call with source 0 takes no named report); the
 *   same callsign (dsd_call_snapshot::source_text, both compared as the call layer keeps identity text, whitespace
 *   runs one space; a callsign using all DSD_POSITION_SOURCE_TEXT_SIZE - 1 bytes, its last not a space, matches by
 *   prefix) when it names one; an anonymous report (hashed, unverified, or neither ID nor callsign) only on a call that
 *   names no source (spec 2.14);
 * - the report was heard where the call was. Same reception: the received identity epoch the report came from (the
 *   low 32 bits of dsd_position_fix::origin_gen) is the one the call began on (dsd_call_snapshot::rx_id_epoch); the
 *   scopes are then not compared, since the record may have learned more between the two reads. Another reception
 *   (the record was reset since): the report's reception scope (dsd_position_fix::reception_scope, with its P25 NAC
 *   discriminator) equals the row's known one (Event_History::rx_scope); an unknown row scope matches nothing. The
 *   report's sender scope is never compared (it is only the track key): a TETRA radio of a foreign network heard here
 *   enriches its own call;
 * - the report is newer (greater rev) than the position the row holds, if any.
 *
 * @p fix must be the record dsd_position_admit() returned for an admitted report (rev set). The CRC provenance of the
 * slot (dsd_state::event_crc_invalid) marks the call and the epoch's row as every enrichment does, whether or not the
 * row takes the position. Callers must not hold the
 * call-state lock or a history transaction. Decoder thread.
 *
 * @return 1 when written, 0 when declined, -1 for invalid arguments (NULLs, no history, slot >= 2, epoch 0, or a report
 *         that was not admitted).
 */
int dsd_event_enrich_position(dsd_state* state, uint8_t slot, uint64_t epoch, const dsd_position_fix* fix,
                              const char* gps_text);

/** A call captured just before it ends, for detail decoded after the end in the same message (spec N2: a P25 Phase 1
 * TDULC ends the call before it dispatches its own link control). */
typedef struct dsd_event_closing_call {
    uint64_t epoch;     /**< the call's epoch; 0 when nothing was captured */
    uint64_t source_id; /**< the call's dsd_call_snapshot::ota_source_id when captured; 0 = none */
    int protocol;       /**< the call's DSD_SYNC_* value */
    uint8_t slot;       /**< the slot it was captured on */
    uint8_t valid;      /**< 1 when the slot had an ACTIVE call to capture */
} dsd_event_closing_call;

/**
 * @brief Capture the slot's ACTIVE call into @p out, just before the code that ends it, for
 * dsd_event_enrich_position_closing(). @p out is zeroed (valid 0) when the slot has no active call.
 *
 * @return 1 when captured, 0 when there was no active call, -1 for NULL arguments or slot >= 2.
 */
int dsd_event_capture_closing_call(const dsd_state* state, uint8_t slot, dsd_event_closing_call* out);

/**
 * @brief dsd_event_enrich_position() for the call @p closing captured, which may have ended since (ENDED, its row
 * staged or committed): every other condition is the same, including the typed-source rule against that call. Use it
 * only for the message that ended the call (the P25 Phase 1 terminator's own GPS link control); any later message
 * enriches ACTIVE calls only. A capture with valid 0, or a slot whose call has since moved to another epoch, declines.
 */
int dsd_event_enrich_position_closing(dsd_state* state, const dsd_event_closing_call* closing,
                                      const dsd_position_fix* fix, const char* gps_text);

/**
 * @brief dsd_event_set_open_call_detail(DSD_EVENT_DETAIL_GPS) with an admitted position report: written into the
 * slot's open call's row (ACTIVE, epoch opened, not held or rejected by the receive policy) with its text, under the
 * typed-source, same-reception and newest-rev rules of dsd_event_enrich_position(). For detail that does not name its
 * call (D-STAR slow-data DPRS). dsd_event_set_open_call_detail(DSD_EVENT_DETAIL_GPS) writes text without a position
 * and drops the row's position.
 *
 * @return 1 when written, 0 when declined, -1 for invalid arguments (as dsd_event_enrich_position()).
 */
int dsd_event_set_open_call_position(dsd_state* state, uint8_t slot, const dsd_position_fix* fix, const char* gps_text);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_EVENTS_H_H */
