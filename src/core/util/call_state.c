// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "call_state_internal.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

_Static_assert(offsetof(dsd_call_state_ext, mutex) == 0U,
               "event-history transactions require the call-state mutex at offset zero");

static dsd_mutex_t g_call_state_alloc_mutex;
static atomic_int g_call_state_alloc_mutex_state = 0; /* 0=uninit, 1=initing, 2=init */

#ifdef DSD_NEO_TEST_HOOKS
static long g_call_state_alloc_fail_after = -1;
static long g_call_state_alloc_calls = 0;
#endif

static void
call_state_alloc_mutex_init(void) {
    if (atomic_load(&g_call_state_alloc_mutex_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_call_state_alloc_mutex_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_call_state_alloc_mutex);
        atomic_store(&g_call_state_alloc_mutex_state, 2);
        return;
    }
    while (atomic_load(&g_call_state_alloc_mutex_state) != 2) {}
}

static void*
call_state_calloc(size_t count, size_t size) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_call_state_alloc_fail_after >= 0 && g_call_state_alloc_calls >= g_call_state_alloc_fail_after) {
        return NULL;
    }
    g_call_state_alloc_calls++;
#endif
    return calloc(count, size);
}

static void
call_state_ext_cleanup(void* opaque) {
    dsd_call_state_ext* ext = (dsd_call_state_ext*)opaque;
    if (!ext) {
        return;
    }
    (void)dsd_mutex_destroy(&ext->mutex);
    free(ext);
}

static dsd_call_state_ext*
call_state_ext_allocate(void) {
    dsd_call_state_ext* ext = (dsd_call_state_ext*)call_state_calloc(1U, sizeof(*ext));
    if (!ext) {
        return NULL;
    }
    if (dsd_mutex_init(&ext->mutex) != 0) {
        free(ext);
        return NULL;
    }
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        ext->calls.slots[slot].slot = (uint8_t)slot;
        ext->calls.slots[slot].phase = DSD_CALL_PHASE_IDLE;
        ext->calls.slots[slot].protocol = DSD_SYNC_NONE;
    }
    return ext;
}

dsd_call_state_ext*
dsd_call_state_ext_get(dsd_state* state, int create) {
    if (!state) {
        return NULL;
    }
    dsd_call_state_ext* ext = DSD_STATE_EXT_GET_AS(dsd_call_state_ext, state, DSD_STATE_EXT_CORE_CALL_STATE);
    if (ext || !create) {
        return ext;
    }

    call_state_alloc_mutex_init();
    (void)dsd_mutex_lock(&g_call_state_alloc_mutex);
    ext = DSD_STATE_EXT_GET_AS(dsd_call_state_ext, state, DSD_STATE_EXT_CORE_CALL_STATE);
    if (!ext) {
        ext = call_state_ext_allocate();
        if (ext && dsd_state_ext_set(state, DSD_STATE_EXT_CORE_CALL_STATE, ext, call_state_ext_cleanup) != 0) {
            call_state_ext_cleanup(ext);
            ext = NULL;
        }
    }
    (void)dsd_mutex_unlock(&g_call_state_alloc_mutex);
    return ext;
}

const dsd_call_state_ext*
dsd_call_state_ext_peek(const dsd_state* state) {
    return state ? (const dsd_call_state_ext*)dsd_state_ext_get_const(state, DSD_STATE_EXT_CORE_CALL_STATE) : NULL;
}

void
dsd_call_state_ext_lock(const dsd_call_state_ext* ext) {
    if (ext) {
        (void)dsd_mutex_lock((dsd_mutex_t*)&ext->mutex);
    }
}

void
dsd_call_state_ext_unlock(const dsd_call_state_ext* ext) {
    if (ext) {
        (void)dsd_mutex_unlock((dsd_mutex_t*)&ext->mutex);
    }
}

int
dsd_call_state_ensure(dsd_state* state) {
    return dsd_call_state_ext_get(state, 1) != NULL ? 1 : 0;
}

// The fallback deliberately matches the resolution of the clock every caller supplies. Endpoints
// that pass observed_m all derive it from dsd_time_now_monotonic_s(), so truncating the fallback to
// whole milliseconds would let an end stamped at ns precision compare as later than a reopen that
// really followed it -- and call_state_reacquires_ended_epoch() would reject a legitimate
// reacquisition that landed inside the same millisecond.
static double
call_state_observed_m(double observed_m) {
    return observed_m > 0.0 ? observed_m : dsd_time_now_monotonic_s();
}

static uint64_t
call_state_next_nonzero(uint64_t value) {
    value++;
    return value == 0U ? 1U : value;
}

static uint64_t
call_state_effective_target_observation(const dsd_call_observation* observation) {
    return observation->ota_target_id != 0U ? observation->ota_target_id : observation->policy_target_id;
}

static uint64_t
call_state_effective_target_snapshot(const dsd_call_snapshot* snapshot) {
    return snapshot->ota_target_id != 0U ? snapshot->ota_target_id : snapshot->policy_target_id;
}

int
dsd_call_state_protocol_family(int protocol) {
    if (DSD_SYNC_IS_P25P1(protocol)) {
        return 1;
    }
    if (DSD_SYNC_IS_P25P2(protocol)) {
        return 2;
    }
    if (DSD_SYNC_IS_X2TDMA(protocol)) {
        return 3;
    }
    if (DSD_SYNC_IS_DSTAR(protocol)) {
        return 4;
    }
    if (DSD_SYNC_IS_M17(protocol)) {
        return 5;
    }
    if (DSD_SYNC_IS_DMR(protocol)) {
        return 6;
    }
    if (DSD_SYNC_IS_EDACS(protocol)) {
        return 7;
    }
    if (DSD_SYNC_IS_DPMR(protocol)) {
        return 8;
    }
    if (DSD_SYNC_IS_NXDN(protocol)) {
        return 9;
    }
    if (DSD_SYNC_IS_YSF(protocol)) {
        return 10;
    }
    return protocol == DSD_SYNC_NONE ? 0 : 1000 + protocol;
}

static void
call_state_normalize_text(char dst[DSD_CALL_IDENTITY_TEXT_SIZE], const char* src) {
    size_t out = 0U;
    int pending_space = 0;
    DSD_MEMSET(dst, 0, DSD_CALL_IDENTITY_TEXT_SIZE);
    if (!src) {
        return;
    }
    while (*src != '\0' && isspace((unsigned char)*src)) {
        src++;
    }
    for (; *src != '\0'; src++) {
        unsigned char ch = (unsigned char)*src;
        if (isspace(ch)) {
            pending_space = out != 0U;
            continue;
        }
        if (pending_space && out + 1U < DSD_CALL_IDENTITY_TEXT_SIZE) {
            dst[out++] = ' ';
        }
        pending_space = 0;
        if (out + 1U < DSD_CALL_IDENTITY_TEXT_SIZE) {
            dst[out++] = (ch < 0x20U || ch == 0x7FU) ? '_' : (char)ch;
        }
    }
    dst[out] = '\0';
}

static int
call_state_known_text_changed(const char* current, const char* incoming) {
    char normalized[DSD_CALL_IDENTITY_TEXT_SIZE];
    call_state_normalize_text(normalized, incoming);
    return current[0] != '\0' && normalized[0] != '\0' && strcmp(current, normalized) != 0;
}

static int
call_state_kind_changed(dsd_call_kind old_kind, dsd_call_kind new_kind) {
    if (old_kind == DSD_CALL_KIND_UNKNOWN || new_kind == DSD_CALL_KIND_UNKNOWN) {
        return 0;
    }
    if ((old_kind == DSD_CALL_KIND_VOICE || new_kind == DSD_CALL_KIND_VOICE)
        && (old_kind == DSD_CALL_KIND_VOICE || old_kind == DSD_CALL_KIND_GROUP_VOICE
            || old_kind == DSD_CALL_KIND_PRIVATE_VOICE)
        && (new_kind == DSD_CALL_KIND_VOICE || new_kind == DSD_CALL_KIND_GROUP_VOICE
            || new_kind == DSD_CALL_KIND_PRIVATE_VOICE)) {
        return 0;
    }
    return old_kind != new_kind;
}

static int
call_state_kind_is_voice(dsd_call_kind kind) {
    return kind == DSD_CALL_KIND_VOICE || kind == DSD_CALL_KIND_GROUP_VOICE || kind == DSD_CALL_KIND_PRIVATE_VOICE;
}

static int
call_state_text_is_known(const char* text) {
    char normalized[DSD_CALL_IDENTITY_TEXT_SIZE];
    call_state_normalize_text(normalized, text);
    return normalized[0] != '\0';
}

static int
call_state_snapshot_is_provisional_voice(const dsd_call_snapshot* current) {
    return current->phase == DSD_CALL_PHASE_ACTIVE && current->kind == DSD_CALL_KIND_VOICE
           && call_state_effective_target_snapshot(current) == 0U && current->ota_source_id == 0U
           && current->source_text[0] == '\0' && current->target_text[0] == '\0' && current->route_text[0][0] == '\0'
           && current->route_text[1][0] == '\0';
}

static int
call_state_observation_has_identity(const dsd_call_observation* observation) {
    return call_state_effective_target_observation(observation) != 0U || observation->ota_source_id != 0U
           || call_state_text_is_known(observation->source_text) || call_state_text_is_known(observation->target_text)
           || call_state_text_is_known(observation->route_text[0])
           || call_state_text_is_known(observation->route_text[1]);
}

static int
call_state_begin_specializes_provisional_voice(const dsd_call_snapshot* current,
                                               const dsd_call_observation* observation, dsd_call_boundary boundary) {
    if (boundary != DSD_CALL_BOUNDARY_BEGIN || !call_state_snapshot_is_provisional_voice(current)
        || !call_state_kind_is_voice(observation->kind) || !call_state_observation_has_identity(observation)) {
        return 0;
    }
    if (current->protocol == DSD_SYNC_NONE || observation->protocol == DSD_SYNC_NONE) {
        return 1;
    }
    return dsd_call_state_protocol_family(current->protocol) == dsd_call_state_protocol_family(observation->protocol);
}

// True when the observation names a different call than the one the slot holds.
// An absent field on either side is not a change: protocols fill identity in
// over several bursts, and a partial re-description must not fork the epoch.
static int
call_state_observation_changes_identity(const dsd_call_snapshot* current, const dsd_call_observation* observation) {
    if (current->protocol != DSD_SYNC_NONE && observation->protocol != DSD_SYNC_NONE
        && dsd_call_state_protocol_family(current->protocol) != dsd_call_state_protocol_family(observation->protocol)) {
        return 1;
    }
    const uint64_t old_target = call_state_effective_target_snapshot(current);
    const uint64_t new_target = call_state_effective_target_observation(observation);
    if (old_target != 0U && new_target != 0U && old_target != new_target) {
        return 1;
    }
    if (call_state_kind_changed(current->kind, observation->kind)) {
        return 1;
    }
    if (current->ota_source_id != 0U && observation->ota_source_id != 0U
        && current->ota_source_id != observation->ota_source_id) {
        return 1;
    }
    // Route text is compared here rather than only where reacquisition consults it: both
    // call_state_observation_has_identity() and dsd_call_state_snapshot_has_identity() already count
    // it as identity, so leaving it out made it the one anchor no contradiction could ever reject.
    // A route-only D-STAR or YSF snapshot would then match every later observation. Like the other
    // text fields it only reports a contradiction when both sides are known, so a repeater pair
    // that has not decoded yet -- or one reset to spaces, which normalizes to empty -- is not a
    // change.
    return call_state_known_text_changed(current->source_text, observation->source_text)
           || call_state_known_text_changed(current->target_text, observation->target_text)
           || call_state_known_text_changed(current->route_text[0], observation->route_text[0])
           || call_state_known_text_changed(current->route_text[1], observation->route_text[1]);
}

static int
call_state_observation_begins_epoch(const dsd_call_snapshot* current, const dsd_call_observation* observation,
                                    dsd_call_boundary boundary) {
    if (current->phase != DSD_CALL_PHASE_ACTIVE) {
        return 1;
    }
    // An explicit BEGIN is positive per-transmission evidence (a PTT, a grant, a
    // voice header) and always opens an epoch. Callers that repeat a header for
    // the transmission already running must observe a CONTINUE instead.
    if (boundary == DSD_CALL_BOUNDARY_BEGIN
        && !call_state_begin_specializes_provisional_voice(current, observation, boundary)) {
        return 1;
    }
    return call_state_observation_changes_identity(current, observation);
}

/*
 * Seconds after a sync-loss end within which the next epoch describing the same call is
 * treated as that transmission being reacquired rather than a new one.
 *
 * This is a backstop, not the discriminator: DSD_CALL_END_SYNC_LOSS plus an unchanged identity
 * carry the decision, and the window only bounds a pathological gap. The anchor already sits
 * after the no-sync timeout (~1800 symbols, dsd_frame_sync.c -- about 0.4 s at 4800 sym/s), so
 * the tolerated air gap is effectively that timeout plus this window.
 *
 * Residual and accepted: an operator who un-keys during the fade and re-keys the same TG/SRC
 * inside the window coalesces. That end really was a sync loss and the identity really does
 * match, so time is the only discriminator left; the short window keeps it rare.
 *
 * Measured on whatever timeline the endpoints supply through call_state_observed_m(). No
 * decode-derived clock exists today -- every non-zero observed_m in the tree ultimately comes
 * from dsd_time_now_monotonic_s(), and callers that pass 0.0 get the same wall clock -- so under
 * unpaced replay (--iq-replay-rate fast, the default) gaps appear shorter than they were on air
 * and coalescing is correspondingly more eager. Documented in docs/iq-capture-replay.md; use
 * --iq-replay-rate realtime to reproduce live timing. If an air-time clock is ever added, route
 * it through call_state_observed_m() rather than introducing a second clock here.
 *
 * The constant itself is DSD_CALL_REACQUIRE_GAP_S in <dsd-neo/core/call_state.h>; the event layer
 * needs it to know how long to hold a VOICE_END alert open.
 */

// True when the slot names a call concretely enough for "the same call" to mean anything.
// call_state_observation_changes_identity() only reports a *contradiction*, so a snapshot that
// never learned an identity is compatible with every observation; reacquisition needs a positive
// anchor or it would coalesce two unrelated transmissions. Exported through
// call_state_internal.h: the event layer keys its drop-identity-less-voice-rows decision on the
// same notion of "named a call", and a private mirror of the field list would silently diverge.
int
dsd_call_state_snapshot_has_identity(const dsd_call_snapshot* current) {
    if (current == NULL) {
        return 0;
    }
    return call_state_effective_target_snapshot(current) != 0U || current->ota_source_id != 0U
           || call_state_text_is_known(current->source_text) || call_state_text_is_known(current->target_text)
           || call_state_text_is_known(current->route_text[0]) || call_state_text_is_known(current->route_text[1]);
}

// Protocol capability, kept in the canonical layer so the event layer never grows its own
// per-protocol knowledge: these modes never parse their voice traffic into a talkgroup or
// source, so an identity-less voice epoch is the protocol's whole story -- that the channel
// carried voice -- not a decode failure. A protocol added here keeps its all-zero rows;
// everywhere else an identity-less voice epoch must earn its row another way. (EDACS-trunked
// ProVoice rows carry AFS/LID strings and never present as identity-less; the entry only
// matters for the standalone mode.)
int
dsd_call_state_protocol_voice_is_anonymous(int protocol) {
    return DSD_SYNC_IS_X2TDMA(protocol) || DSD_SYNC_IS_PROVOICE(protocol);
}

// Exported through call_state_internal.h: the event layer's decision to hold a VOICE_END alert
// open keys on the same notion of "may still be reacquired", and a private mirror of the reason
// list would silently diverge when a reason is added.
int
dsd_call_state_end_reason_is_recoverable(uint8_t end_reason) {
    return end_reason == (uint8_t)DSD_CALL_END_SYNC_LOSS || end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR;
}

// Exported through call_state_internal.h: the event layer's keep-or-drop verdict for identity-less
// voice rows keys on the same notion of "positively ended over the air", and a private mirror of
// the reason list would silently diverge when a reason is added.
int
dsd_call_state_end_reason_is_terminator(uint8_t end_reason) {
    return end_reason == (uint8_t)DSD_CALL_END_TERMINATOR || end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR;
}

// Exported through call_state_internal.h: the event layer holds a VOICE_END alert open for
// exactly as long as the end may still be reacquired, so the deadline must be selected by the
// same reason-keyed rule reacquisition itself applies below -- a second copy of the selection
// would drift the two windows apart.
double
dsd_call_state_end_reason_reacquire_gap_s(uint8_t end_reason) {
    return end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR ? DSD_CALL_TERMINATOR_HEAL_GAP_S
                                                                     : DSD_CALL_REACQUIRE_GAP_S;
}

// True when this observation reopens an epoch that a recoverable end closed moments ago while
// describing the same call -- one transmission the decoder lost and regained, not two
// transmissions. The boundary token is deliberately not consulted: the paths that reopen
// mid-transmission (the vocoder's per-frame media mark, a DMR Voice LC Header arriving after the
// gap, M17's stream mark, the P25p1 ESS ensure-call) all pass BEGIN, while P25 Phase 2 and the
// other re-announcing protocols pass CONTINUE. Both must arm.
static int
call_state_reacquires_ended_epoch(const dsd_call_snapshot* current, const dsd_call_observation* observation,
                                  double now_m) {
    if (current->phase != DSD_CALL_PHASE_ENDED || !dsd_call_state_end_reason_is_recoverable(current->end_reason)) {
        return 0;
    }
    // An unverified terminator was positive -- if fallible -- evidence the transmission ended,
    // so only identity-less continuations may reopen its epoch: the vocoder's per-frame media
    // mark that heals a voice burst mis-typed as a terminator arrives within one burst and
    // carries no identity. An identity-bearing observation after such an end is the next
    // transmission's header, and folding it in would merge a genuine second PTT on the same
    // TG/SRC into the terminated call's row. A sync-loss end carries no end evidence at all, so
    // there the identity-bearing reopen (a DMR Voice LC Header arriving after the gap, M17's
    // stream mark) stays a reacquisition.
    if (current->end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR
        && call_state_observation_has_identity(observation)) {
        return 0;
    }
    // The ending epoch must name a call. Without this an identity-less provisional epoch -- the
    // shape mark_vocoder_call_media() opens before any header decodes -- matches everything, and
    // the next unrelated call on the slot would be folded into its row.
    if (!dsd_call_state_snapshot_has_identity(current)) {
        return 0;
    }
    if (call_state_observation_changes_identity(current, observation)) {
        return 0;
    }
    // The heal window after an unverified terminator is much tighter than the sync-loss window:
    // the mis-typed voice burst it exists for is followed by the transmission's next voice frame
    // within a burst or two, while a real end followed by a fast re-key whose headers fail to
    // decode can put a new transmission's first identity-less media mark on the slot well inside
    // the sync-loss window -- and folding that in would hand the new call the terminated call's
    // identity and crypto.
    const double gap = dsd_call_state_end_reason_reacquire_gap_s(current->end_reason);
    return current->ended_m > 0.0 && now_m >= current->ended_m && (now_m - current->ended_m) <= gap;
}

// Carry the ending call's identity and metadata into the reopened epoch. Without this the UI and
// the staged history row blank out across the gap and have to relearn everything the previous
// segment already decoded.
static void
call_state_seed_reacquired_snapshot(dsd_call_snapshot* snapshot, const dsd_call_snapshot* previous) {
    snapshot->ota_source_id = previous->ota_source_id;
    snapshot->ota_target_id = previous->ota_target_id;
    snapshot->policy_target_id = previous->policy_target_id;
    DSD_MEMCPY(snapshot->source_text, previous->source_text, sizeof(snapshot->source_text));
    DSD_MEMCPY(snapshot->target_text, previous->target_text, sizeof(snapshot->target_text));
    DSD_MEMCPY(snapshot->route_text, previous->route_text, sizeof(snapshot->route_text));
    snapshot->kind = previous->kind;
    snapshot->protocol = previous->protocol;
    snapshot->channel = previous->channel;
    snapshot->frequency_hz = previous->frequency_hz;
    snapshot->crypto = previous->crypto;
    snapshot->algid = previous->algid;
    snapshot->kid = previous->kid;
    snapshot->mi = previous->mi;
    snapshot->service_options = previous->service_options;
    snapshot->has_service_metadata = previous->has_service_metadata;
    snapshot->emergency = previous->emergency;
    snapshot->priority = previous->priority;
    // audio_permitted and started_m are deliberately not carried: the reacquired segment
    // re-earns audio from the next crypto update exactly as it does today, and started_m stays
    // the reopen instant so per-segment durations remain the segment's own.
}

// A protocol whose end path tears down live decoder state it needs back on a heal (the DMR
// terminator reset clearing the slot's crypto) installs this hook and restores its own fields.
// The canonical layer only reports that a heal happened and hands over the ending snapshot; what
// was cleared and what is safe to put back is protocol knowledge and stays in the protocol.
// Written once from the decode path before the first heal can occur, read on the same thread.
static dsd_call_state_reacquire_hook g_call_state_reacquire_hook = NULL;

void
dsd_call_state_set_reacquire_hook(dsd_call_state_reacquire_hook hook) {
    g_call_state_reacquire_hook = hook;
}

static void
call_state_apply_text(char dst[DSD_CALL_IDENTITY_TEXT_SIZE], const char* src) {
    char normalized[DSD_CALL_IDENTITY_TEXT_SIZE];
    call_state_normalize_text(normalized, src);
    if (normalized[0] != '\0') {
        DSD_MEMCPY(dst, normalized, sizeof(normalized));
    }
}

static void
call_state_apply_observation(dsd_call_snapshot* snapshot, const dsd_call_observation* observation) {
    if (observation->protocol != DSD_SYNC_NONE) {
        snapshot->protocol = observation->protocol;
    }
    if (observation->kind != DSD_CALL_KIND_UNKNOWN
        && !(observation->kind == DSD_CALL_KIND_VOICE
             && (snapshot->kind == DSD_CALL_KIND_GROUP_VOICE || snapshot->kind == DSD_CALL_KIND_PRIVATE_VOICE))) {
        snapshot->kind = observation->kind;
    }
    if (observation->ota_target_id != 0U) {
        snapshot->ota_target_id = observation->ota_target_id;
    }
    if (observation->policy_target_id != 0U) {
        snapshot->policy_target_id = observation->policy_target_id;
    }
    if (observation->ota_source_id != 0U) {
        snapshot->ota_source_id = observation->ota_source_id;
    }
    call_state_apply_text(snapshot->source_text, observation->source_text);
    call_state_apply_text(snapshot->target_text, observation->target_text);
    call_state_apply_text(snapshot->route_text[0], observation->route_text[0]);
    call_state_apply_text(snapshot->route_text[1], observation->route_text[1]);
    if (observation->channel != 0U) {
        snapshot->channel = observation->channel;
    }
    if (observation->frequency_hz != 0) {
        snapshot->frequency_hz = observation->frequency_hz;
    }
    if (observation->has_service_metadata != 0U) {
        snapshot->service_options = observation->service_options;
        snapshot->emergency = observation->emergency;
        snapshot->priority = observation->priority;
        snapshot->has_service_metadata = 1U;
    }
}

int
dsd_call_state_observe(dsd_state* state, const dsd_call_observation* observation, dsd_call_boundary boundary) {
    if (!state || !observation || observation->slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[observation->slot];
    const double now_m = call_state_observed_m(observation->observed_m);
    const int begins_epoch = call_state_observation_begins_epoch(snapshot, observation, boundary);
    // Evaluated before the memset below: the ending snapshot is the comparison target.
    const int reacquires_ended_epoch = begins_epoch && call_state_reacquires_ended_epoch(snapshot, observation, now_m);
    // Copied out of the locked region for the reacquire hook: the hook runs protocol code and
    // must not execute under the canonical lock.
    dsd_call_snapshot previous;
    DSD_MEMSET(&previous, 0, sizeof(previous));
    if (begins_epoch) {
        previous = *snapshot;
        DSD_MEMSET(snapshot, 0, sizeof(*snapshot));
        ext->epoch_sequence[observation->slot] = call_state_next_nonzero(ext->epoch_sequence[observation->slot]);
        snapshot->epoch = ext->epoch_sequence[observation->slot];
        snapshot->slot = observation->slot;
        snapshot->protocol = DSD_SYNC_NONE;
        snapshot->crypto = DSD_CALL_CRYPTO_UNKNOWN;
        snapshot->started_m = now_m;
        if (reacquires_ended_epoch) {
            call_state_seed_reacquired_snapshot(snapshot, &previous);
            ext->events[observation->slot].reacquired_epoch = snapshot->epoch;
            // Which epoch was reopened. Whether a history row may actually be merged is the event
            // layer's call -- it pairs this against the epoch its committed row belongs to -- but
            // the identity seeding and the suppressed START above are canonical either way.
            ext->events[observation->slot].reacquired_from_epoch = previous.epoch;
        }
        // reacquired_epoch is deliberately not cleared here. It is compared against the epoch it
        // names, and epoch ids only increase, so a stale value can never match a later epoch.
        // Clearing it would disarm a reacquisition whose staged row is still waiting to flush,
        // and that row would then be committed a second time.
        ext->events[observation->slot].ended_committed = 0U;
        ext->events[observation->slot].notice_epoch = 0U;
        ext->events[observation->slot].notice_target_id = 0U;
        ext->events[observation->slot].notice_kind = DSD_CALL_KIND_UNKNOWN;
        ext->events[observation->slot].notice_handled = 0U;
    }
    snapshot->phase = DSD_CALL_PHASE_ACTIVE;
    call_state_apply_observation(snapshot, observation);
    snapshot->updated_m = now_m;
    snapshot->ended_m = 0.0;
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    if (reacquires_ended_epoch && g_call_state_reacquire_hook != NULL) {
        g_call_state_reacquire_hook(state, observation->slot, &previous);
    }
    return begins_epoch;
}

static int
call_state_crypto_target_accepts(const dsd_call_snapshot* snapshot, int include_ended) {
    if (snapshot->epoch == 0U) {
        return 0;
    }
    if (snapshot->phase == DSD_CALL_PHASE_ACTIVE) {
        return 1;
    }
    return include_ended && snapshot->phase == DSD_CALL_PHASE_ENDED;
}

static int
call_state_crypto_differs(const dsd_call_snapshot* snapshot, const dsd_call_crypto_update* update) {
    return snapshot->crypto != update->classification || snapshot->algid != update->algid
           || snapshot->kid != update->kid || snapshot->mi != update->mi
           || snapshot->audio_permitted != (update->audio_permitted ? 1U : 0U);
}

static int
call_state_update_crypto(dsd_state* state, uint8_t slot, const dsd_call_crypto_update* update, int include_ended) {
    if (!state || !update || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[slot];
    if (!call_state_crypto_target_accepts(snapshot, include_ended)) {
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    if (!call_state_crypto_differs(snapshot, update)) {
        // A retained ended call is re-described by every carrier repeat.
        // Bumping the revision for an identical snapshot makes consumers that
        // poll on revision churn once per repeat for no observable change.
        dsd_call_state_ext_unlock(ext);
        return 1;
    }
    snapshot->crypto = update->classification;
    snapshot->algid = update->algid;
    snapshot->kid = update->kid;
    snapshot->mi = update->mi;
    snapshot->audio_permitted = update->audio_permitted ? 1U : 0U;
    if (snapshot->phase == DSD_CALL_PHASE_ACTIVE) {
        snapshot->updated_m = call_state_observed_m(update->observed_m);
    }
    // An ended call keeps the updated_m it had when it ended: moving it past
    // ended_m would make a finished call look freshly updated to consumers
    // that order by recency.
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_update_crypto(dsd_state* state, uint8_t slot, const dsd_call_crypto_update* update) {
    return call_state_update_crypto(state, slot, update, 0);
}

int
dsd_call_state_update_retained_crypto(dsd_state* state, uint8_t slot, const dsd_call_crypto_update* update) {
    return call_state_update_crypto(state, slot, update, 1);
}

int
dsd_call_state_update_media(dsd_state* state, uint8_t slot, int media_active, double observed_m) {
    if (!state || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[slot];
    if (snapshot->epoch == 0U || snapshot->phase != DSD_CALL_PHASE_ACTIVE) {
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    snapshot->media_active = media_active ? 1U : 0U;
    snapshot->updated_m = call_state_observed_m(observed_m);
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_end_ex(dsd_state* state, uint8_t slot, double observed_m, dsd_call_end_reason reason) {
    if (!state || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[slot];
    // Ending an already-ended epoch is a no-op, so the repeated noCarrier() calls
    // that fire while unsynced do not re-stamp ended_m: the reacquisition gap
    // stays anchored at the first end.
    if (snapshot->epoch == 0U || snapshot->phase != DSD_CALL_PHASE_ACTIVE) {
        // One exception: positive evidence that the transmission is over, arriving after a
        // recoverable end, must be able to retract the reacquisition permission that end
        // granted. The fade is often the last thing heard before the terminator that explains
        // it, so without this a second PTT on the same identity inside the gap folds into the
        // terminated call's row. Two strengths of evidence qualify: a verified terminator or an
        // EXPLICIT teardown retracts either recoverable reason, and a second unverified
        // terminator corroborates an unverified-terminator end -- two independently mis-typed
        // bursts in a row is not a plausible fade. An unverified terminator alone never
        // tightens a sync-loss end: it is the same fallible evidence the recoverable end exists
        // to distrust, and trusting it there would split a faded transmission in two and
        // release its held VOICE_END early. Only tightening toward a final reason is allowed --
        // TERMINATOR where the evidence was a terminator (verified, or corroborated by repeat),
        // EXPLICIT for a teardown -- and ended_m is deliberately left alone: the reason
        // changes, the moment the transmission stopped does not.
        const int retracts = (reason == DSD_CALL_END_EXPLICIT || reason == DSD_CALL_END_TERMINATOR)
                             && dsd_call_state_end_reason_is_recoverable(snapshot->end_reason);
        const int corroborates = reason == DSD_CALL_END_UNVERIFIED_TERMINATOR
                                 && snapshot->end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR;
        if (snapshot->epoch != 0U && snapshot->phase == DSD_CALL_PHASE_ENDED && (retracts || corroborates)) {
            snapshot->end_reason = (reason == DSD_CALL_END_TERMINATOR || corroborates)
                                       ? (uint8_t)DSD_CALL_END_TERMINATOR
                                       : (uint8_t)DSD_CALL_END_EXPLICIT;
            snapshot->revision = call_state_next_nonzero(snapshot->revision);
            ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
            dsd_call_state_ext_unlock(ext);
            return 1;
        }
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    snapshot->phase = DSD_CALL_PHASE_ENDED;
    snapshot->end_reason = (uint8_t)reason;
    snapshot->media_active = 0U;
    snapshot->audio_permitted = 0U;
    snapshot->ended_m = call_state_observed_m(observed_m);
    snapshot->updated_m = snapshot->ended_m;
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_end(dsd_state* state, uint8_t slot, double observed_m) {
    return dsd_call_state_end_ex(state, slot, observed_m, DSD_CALL_END_EXPLICIT);
}

int
dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out) {
    if (!state || !out || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    *out = ext->calls.slots[slot];
    dsd_call_state_ext_unlock(ext);
    return out->epoch != 0U ? 1 : 0;
}

int
dsd_call_state_copy_snapshot(const dsd_state* state, dsd_call_state_snapshot* out) {
    if (!state || !out) {
        return -1;
    }
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (!ext) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    *out = ext->calls;
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_restore_snapshot(dsd_state* state, const dsd_call_state_snapshot* snapshot) {
    if (!state || !snapshot) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    ext->calls = *snapshot;
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (ext->epoch_sequence[slot] < snapshot->slots[slot].epoch) {
            ext->epoch_sequence[slot] = snapshot->slots[slot].epoch;
        }
    }
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_context_copy_snapshot(const dsd_state* state, dsd_call_context_snapshot* out) {
    if (!state || !out) {
        return -1;
    }
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (!ext) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    out->calls = ext->calls;
    out->recent = ext->recent;
    DSD_MEMCPY(out->events, ext->events, sizeof(out->events));
    dsd_call_state_ext_unlock(ext);
    return 1;
}

void
dsd_call_state_invalidate_event_lifecycle(dsd_call_event_lifecycle* lifecycle) {
    if (lifecycle == NULL) {
        return;
    }
    lifecycle->committed_seq = 0U;
    lifecycle->committed_epoch = 0U;
    lifecycle->committed_valid = 0U;
    lifecycle->reacquired_epoch = 0U;
    lifecycle->reacquired_from_epoch = 0U;
    // A held VOICE_END describes a row that is going away. Firing it later would beep the end of
    // a transmission the operator can no longer see.
    lifecycle->end_alert_pending = 0U;
    lifecycle->end_alert_due_m = 0.0;
    // Both halves of the env pair go, matching the epoch-change path in dsd_events.c. A row staged
    // directly by a protocol never passes through the renderer, so a surviving staged_env would be
    // promoted into committed_env when that row commits and a later merge would re-render against
    // a decoder context from before this invalidation.
    DSD_MEMSET(&lifecycle->staged_env, 0, sizeof(lifecycle->staged_env));
    DSD_MEMSET(&lifecycle->committed_env, 0, sizeof(lifecycle->committed_env));
}

int
dsd_call_context_restore_snapshot(dsd_state* state, const dsd_call_context_snapshot* snapshot) {
    if (!state || !snapshot) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    ext->calls = snapshot->calls;
    ext->recent = snapshot->recent;
    DSD_MEMCPY(ext->events, snapshot->events, sizeof(ext->events));
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (ext->epoch_sequence[slot] < snapshot->calls.slots[slot].epoch) {
            ext->epoch_sequence[slot] = snapshot->calls.slots[slot].epoch;
        }
        // The lifecycle is saved and restored per trunk-scan target while the event history
        // itself is global, so a commit reference -- or a VOICE_END held on the global monotonic
        // clock -- carried across a hop would describe one target's row while the other target is
        // running. Invalidate rather than relocate.
        dsd_call_state_invalidate_event_lifecycle(&ext->events[slot]);
        // Clearing the commit reference blocks the row merge but not the rest of reacquisition:
        // the monotonic clock is global, so a slot saved mid-fade would still satisfy the gap
        // test on the new target and suppress the first call's VOICE_START alert while seeding
        // it with the old target's identity. A restored end is a hop, never a resumable fade.
        // This lives on the call snapshot rather than the lifecycle, so it stays here.
        if (ext->calls.slots[slot].phase == DSD_CALL_PHASE_ENDED) {
            ext->calls.slots[slot].end_reason = (uint8_t)DSD_CALL_END_EXPLICIT;
        }
    }
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_call_state_enrich_text(dsd_state* state, uint8_t slot, uint64_t epoch, const char* source_text,
                           const char* target_text, const char* route0_text, const char* route1_text,
                           double observed_m) {
    if (!state || slot >= DSD_CALL_STATE_SLOT_COUNT || epoch == 0U) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    dsd_call_snapshot* snapshot = &ext->calls.slots[slot];
    if (snapshot->phase != DSD_CALL_PHASE_ACTIVE || snapshot->epoch != epoch) {
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    call_state_apply_text(snapshot->source_text, source_text);
    call_state_apply_text(snapshot->target_text, target_text);
    call_state_apply_text(snapshot->route_text[0], route0_text);
    call_state_apply_text(snapshot->route_text[1], route1_text);
    snapshot->updated_m = call_state_observed_m(observed_m);
    snapshot->revision = call_state_next_nonzero(snapshot->revision);
    ext->calls.revision = call_state_next_nonzero(ext->calls.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

static int
recent_activity_index_valid(uint8_t index) {
    return index < DSD_RECENT_ACTIVITY_COUNT;
}

static void
recent_activity_copy_text(char* dst, size_t dst_size, const char* text) {
    if (!text) {
        dst[0] = '\0';
        return;
    }
    DSD_SNPRINTF(dst, dst_size, "%s", text);
}

int
dsd_recent_activity_publish(dsd_state* state, uint8_t index, const dsd_call_observation* observation,
                            const char* notice, uint64_t observed_m_ms) {
    if (!state || !recent_activity_index_valid(index) || (!observation && (!notice || notice[0] == '\0'))) {
        return -1;
    }
    const uint64_t now_ms = observed_m_ms != 0U ? observed_m_ms : dsd_time_monotonic_ms();
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (!ext) {
        return -1;
    }
    dsd_call_state_ext_lock(ext);
    dsd_recent_activity_entry* entry = &ext->recent.entries[index];
    DSD_MEMSET(entry, 0, sizeof(*entry));
    if (observation) {
        entry->observation = *observation;
        call_state_normalize_text(entry->observation.source_text, observation->source_text);
        call_state_normalize_text(entry->observation.target_text, observation->target_text);
        call_state_normalize_text(entry->observation.route_text[0], observation->route_text[0]);
        call_state_normalize_text(entry->observation.route_text[1], observation->route_text[1]);
    }
    recent_activity_copy_text(entry->notice, sizeof(entry->notice), notice);
    entry->updated_m_ms = now_ms;
    ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_clear(dsd_state* state, uint8_t index) {
    if (!state || !recent_activity_index_valid(index)) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    DSD_MEMSET(&ext->recent.entries[index], 0, sizeof(ext->recent.entries[index]));
    ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_clear_all(dsd_state* state) {
    if (!state) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (ext) {
        dsd_call_state_ext_lock(ext);
        DSD_MEMSET(&ext->recent.entries, 0, sizeof(ext->recent.entries));
        ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
        dsd_call_state_ext_unlock(ext);
        return 1;
    }
    return 0;
}

int
dsd_recent_activity_copy_snapshot(const dsd_state* state, dsd_recent_activity_snapshot* out) {
    if (!state || !out) {
        return -1;
    }
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (!ext) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    *out = ext->recent;
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_restore_snapshot(dsd_state* state, const dsd_recent_activity_snapshot* snapshot) {
    if (!state || !snapshot) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 1);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    ext->recent = *snapshot;
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_expire(dsd_state* state, uint64_t now_m_ms, uint64_t ttl_ms) {
    if (!state) {
        return -1;
    }
    const uint64_t now_ms = now_m_ms != 0U ? now_m_ms : dsd_time_monotonic_ms();
    const uint64_t max_age_ms = ttl_ms != 0U ? ttl_ms : DSD_RECENT_ACTIVITY_TTL_MS;
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return 0;
    }
    int expired = 0;
    dsd_call_state_ext_lock(ext);
    for (int i = 0; i < DSD_RECENT_ACTIVITY_COUNT; i++) {
        dsd_recent_activity_entry* entry = &ext->recent.entries[i];
        if (entry->updated_m_ms == 0U || now_ms < entry->updated_m_ms || now_ms - entry->updated_m_ms <= max_age_ms) {
            continue;
        }
        DSD_MEMSET(entry, 0, sizeof(*entry));
        expired++;
    }
    if (expired > 0) {
        ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
    }
    dsd_call_state_ext_unlock(ext);
    return expired;
}

int
dsd_recent_activity_save(const dsd_state* state, uint8_t index, dsd_recent_activity_transaction* transaction) {
    if (!state || !transaction || !recent_activity_index_valid(index)) {
        return -1;
    }
    DSD_MEMSET(transaction, 0, sizeof(*transaction));
    transaction->valid = 1U;
    transaction->index = index;
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    transaction->entry = ext->recent.entries[index];
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_recent_activity_restore(dsd_state* state, const dsd_recent_activity_transaction* transaction) {
    if (!state || !transaction || !transaction->valid || !recent_activity_index_valid(transaction->index)) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (ext) {
        dsd_call_state_ext_lock(ext);
        ext->recent.entries[transaction->index] = transaction->entry;
        ext->recent.revision = call_state_next_nonzero(ext->recent.revision);
        dsd_call_state_ext_unlock(ext);
        return 1;
    }
    return 0;
}

int
dsd_call_state_copy_to_state(dsd_state* dst, const dsd_state* src) {
    if (!dst || !src) {
        return -1;
    }
    const dsd_call_state_ext* src_ext = dsd_call_state_ext_peek(src);
    if (!src_ext) {
        (void)dsd_state_ext_set(dst, DSD_STATE_EXT_CORE_CALL_STATE, NULL, NULL);
        return 0;
    }
    dsd_call_state_snapshot calls;
    dsd_recent_activity_snapshot recent;
    dsd_call_event_lifecycle events[DSD_CALL_STATE_SLOT_COUNT];
    uint64_t epoch_sequence[DSD_CALL_STATE_SLOT_COUNT];
    dsd_call_state_ext_lock(src_ext);
    calls = src_ext->calls;
    recent = src_ext->recent;
    DSD_MEMCPY(events, src_ext->events, sizeof(events));
    DSD_MEMCPY(epoch_sequence, src_ext->epoch_sequence, sizeof(epoch_sequence));
    dsd_call_state_ext_unlock(src_ext);

    dsd_call_state_ext* dst_ext = dsd_call_state_ext_get(dst, 1);
    if (!dst_ext) {
        (void)dsd_state_ext_set(dst, DSD_STATE_EXT_CORE_CALL_STATE, NULL, NULL);
        return -1;
    }
    dsd_call_state_ext_lock(dst_ext);
    dst_ext->calls = calls;
    dst_ext->recent = recent;
    DSD_MEMCPY(dst_ext->events, events, sizeof(events));
    DSD_MEMCPY(dst_ext->epoch_sequence, epoch_sequence, sizeof(epoch_sequence));
    dsd_call_state_ext_unlock(dst_ext);
    return 1;
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_call_state_test_alloc_reset(void) {
    g_call_state_alloc_calls = 0;
    g_call_state_alloc_fail_after = -1;
}

void
dsd_call_state_test_alloc_fail_after(long fail_after) {
    g_call_state_alloc_calls = 0;
    g_call_state_alloc_fail_after = fail_after;
}

int
dsd_call_state_test_set_epoch(dsd_state* state, uint8_t slot, uint64_t epoch) {
    if (!state || slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    ext->calls.slots[slot].epoch = epoch;
    ext->epoch_sequence[slot] = epoch;
    dsd_call_state_ext_unlock(ext);
    return 1;
}
#endif
