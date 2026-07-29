// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_CORE_UTIL_CALL_STATE_INTERNAL_H_
#define DSD_NEO_SRC_CORE_UTIL_CALL_STATE_INTERNAL_H_

#include <dsd-neo/core/call_state.h>
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

/**
 * True when the snapshot names a call concretely enough for "the same call" to mean anything:
 * a target or source id, a source/target text, or a route text. The canonical identity notion
 * shared by reacquisition (call_state.c) and the event layer's decision to drop voice rows
 * whose epoch never named a call (dsd_events.c). NULL-safe.
 */
int dsd_call_state_snapshot_has_identity(const dsd_call_snapshot* current);

/**
 * True for protocols whose voice traffic never carries per-call identity (standalone X2-TDMA and
 * ProVoice), so an all-zero voice row is the protocol's whole story rather than noise. Protocol
 * capability lives here, beside the identity notion it qualifies, so the event layer's
 * keep-or-drop decision never grows a private per-protocol list.
 */
int dsd_call_state_protocol_voice_is_anonymous(int protocol);

/**
 * True for protocols whose voice traffic can signal its own end over the air (a terminator
 * burst, an end frame, an EOT). False for D-STAR, whose transmissions only ever end by sync
 * loss or an engine teardown, so the event layer's keep-or-drop verdict must accept a sync-loss
 * end as the positive end evidence those modes can never produce.
 */
int dsd_call_state_protocol_voice_has_terminator(int protocol);

/**
 * True for the end reasons that leave the epoch reacquirable (SYNC_LOSS and
 * UNVERIFIED_TERMINATOR). The canonical predicate behind both reacquisition (call_state.c) and
 * the event layer's decision to hold a VOICE_END alert open until the window closes
 * (dsd_events.c); a private mirror of the reason list would silently diverge.
 */
int dsd_call_state_end_reason_is_recoverable(uint8_t end_reason);

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

#endif /* DSD_NEO_SRC_CORE_UTIL_CALL_STATE_INTERNAL_H_ */
