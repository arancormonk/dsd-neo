// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_CORE_UTIL_CALL_STATE_INTERNAL_H_
#define DSD_NEO_SRC_CORE_UTIL_CALL_STATE_INTERNAL_H_

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/threading.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

typedef dsd_call_event_lifecycle_snapshot dsd_call_event_lifecycle;

typedef struct {
    dsd_mutex_t mutex;
    dsd_call_state_snapshot calls;
    dsd_recent_activity_snapshot recent;
    dsd_call_event_lifecycle events[DSD_CALL_STATE_SLOT_COUNT];
    uint64_t epoch_sequence[DSD_CALL_STATE_SLOT_COUNT];
} dsd_call_state_ext;

dsd_call_state_ext* dsd_call_state_ext_get(dsd_state* state, int create);
/* @p src as a call keeps an identity text (dsd_call_snapshot::source_text): surrounding whitespace dropped, inner runs
 * one space, control characters '_'. @p dst holds DSD_CALL_IDENTITY_TEXT_SIZE bytes; NULL @p src gives "". */
void dsd_call_state_normalize_identity_text(char* dst, const char* src);

/**
 * True when the snapshot names a call concretely enough for "the same call" to mean anything:
 * a target or source id, a source/target text, or a route text. The canonical identity notion
 * shared by reacquisition (call_state.c) and the event layer's decision to drop voice rows
 * whose epoch never named a call (dsd_events.c). NULL-safe.
 */
int dsd_call_state_snapshot_has_identity(const dsd_call_snapshot* current);
/*
 * Whether an update from @p protocol's signalling may describe the call @p snapshot holds: a call of that protocol
 * (dsd_call_state_protocol_family(), P25 Phase 1 and Phase 2 one protocol). A call that names no protocol is no
 * protocol's. Every crypto, identity and event-row enrichment writer is checked by it (dsd_call_state_update_crypto(),
 * dsd_event_enrich_alias(), dsd_event_set_open_call_detail()).
 */
int dsd_call_state_update_is_protocols(const dsd_call_snapshot* snapshot, int protocol);

/**
 * True for protocols whose voice traffic never carries per-call identity (standalone X2-TDMA and
 * ProVoice), so an all-zero voice row is the protocol's whole story rather than noise. Protocol
 * capability lives here, beside the identity notion it qualifies, so the event layer's
 * keep-or-drop decision never grows a private per-protocol list.
 */
int dsd_call_state_protocol_voice_is_anonymous(int protocol);

/**
 * True for protocols whose voice traffic can be relied on to signal its own end over the air:
 * DMR and P25 repeat their end signaling across the hangtime, and dPMR's FS3 end frame is a
 * sync-correlator match. False for D-STAR, which has no end signaling at all, and for M17, YSF
 * and NXDN, whose end marker is one un-repeated CRC- or error-gated burst that a fade or a
 * single damaged frame routinely costs. The event layer's keep-or-drop verdict must accept a
 * sync-loss end from the false side as the positive end evidence those modes cannot be counted
 * on to produce.
 */
int dsd_call_state_protocol_voice_has_terminator(int protocol);

/* dsd_call_state_end_reason_is_recoverable() is declared in <dsd-neo/core/call_state.h>: the
 * protocol reacquire-hook owners need the same predicate to decide whether state they are about
 * to tear down may still be healed back, so it is public rather than core-internal. */

/**
 * True for the end reasons that are positive over-the-air evidence a transmission ended (a
 * terminator burst, verified or not). EXPLICIT is deliberately excluded: the engine ends epochs
 * EXPLICIT on every retune and teardown, so it says nothing about the air. The canonical
 * predicate behind the event layer's keep-or-drop verdict for identity-less voice rows
 * (dsd_events.c); a private mirror of the reason list would silently diverge.
 */
int dsd_call_state_end_reason_is_terminator(uint8_t end_reason);

/**
 * Seconds after an end with this reason within which the epoch may still be reacquired:
 * DSD_CALL_TERMINATOR_HEAL_GAP_S after an unverified terminator, DSD_CALL_REACQUIRE_GAP_S
 * otherwise. The event layer holds a VOICE_END alert open for exactly this long, so the deadline
 * and the reacquisition window it waits on cannot drift apart.
 */
double dsd_call_state_end_reason_reacquire_gap_s(uint8_t end_reason);
const dsd_call_state_ext* dsd_call_state_ext_peek(const dsd_state* state);
void dsd_call_state_ext_lock(const dsd_call_state_ext* ext);
void dsd_call_state_ext_unlock(const dsd_call_state_ext* ext);

/**
 * Drop every reference this lifecycle holds into the event history, including a VOICE_END alert
 * still being held open.
 *
 * Two callers invalidate a lifecycle for the same underlying reason -- the rows it points at are
 * no longer the rows it was describing. dsd_event_history_reset() clears the ring outright, and
 * dsd_call_context_restore_snapshot() hops to another trunk-scan target while the ring stays
 * global. Both must forget the commit reference, the pending reacquisition and the held alert
 * together; leaving any one of them behind lets the next segment merge into, enrich, or beep for
 * a row that no longer exists. Callers must hold the call-state mutex.
 *
 * Deliberately not cleared: epoch, ended_committed and the notice markers. Those record which
 * call the slot has already rendered, not which row it landed in, and both callers still have
 * that call in hand -- a context restore carries it across alongside the call snapshot, and a
 * history reset leaves the live call untouched. Clearing them would make an ended-but-retained
 * call look unrendered and commit it a second time. The end-reason downgrade a context restore
 * performs lives on the call snapshot rather than the lifecycle, so it stays at that call site.
 */
void dsd_call_state_invalidate_event_lifecycle(dsd_call_event_lifecycle* lifecycle);

/**
 * The call epoch whose history row the slot's staged row will commit as, or 0 when there is none: no epoch rendered
 * yet, or that epoch's row already committed at its end. A per-call WAV closed before its call's row commits is
 * journaled as pending on it (issue #708). Takes the call-state mutex: callers must not hold it.
 */
uint64_t dsd_call_state_pending_row_epoch(const dsd_state* state, uint8_t slot);

/** dsd_call_state_pending_row_epoch() for a caller that holds the call-state mutex. */
uint64_t dsd_call_state_pending_row_epoch_locked(const dsd_call_state_ext* ext, uint8_t slot);

/**
 * Depth in @p ring of the row @p lifecycle last committed, or 0 when it can no longer be located. push_event_history()
 * copies row 0 into row 1, so immediately after a commit the row sits at index 1 and every push since -- including
 * interleaved data or system notices -- has pushed it one deeper. Shared by the event layer and the call state's
 * verified-target note, which revises that row in place. Callers hold the call-state mutex.
 */
static inline uint8_t
dsd_call_event_committed_row_index(const Event_History_I* ring, const dsd_call_event_lifecycle* lifecycle) {
    if (ring == NULL || lifecycle == NULL || !lifecycle->committed_valid || ring->push_seq < lifecycle->committed_seq) {
        return 0U;
    }
    const uint64_t depth = 1U + (ring->push_seq - lifecycle->committed_seq);
    return depth <= (DSD_EVENT_HISTORY_LEN - 1U) ? (uint8_t)depth : 0U;
}

#endif /* DSD_NEO_SRC_CORE_UTIL_CALL_STATE_INTERNAL_H_ */
