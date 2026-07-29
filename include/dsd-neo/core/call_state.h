// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Protocol-neutral per-slot call state and recent activity.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_STATE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_STATE_H_H

#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_CALL_STATE_SLOT_COUNT = 2,
    DSD_RECENT_ACTIVITY_COUNT = 31,
    DSD_RECENT_ACTIVITY_TEXT_SIZE = 200,
    DSD_CALL_IDENTITY_TEXT_SIZE = 64,
    DSD_CALL_ROUTE_COUNT = 2,
    DSD_RECENT_ACTIVITY_TTL_MS = 3000,
};

typedef enum {
    DSD_CALL_PHASE_IDLE = 0,
    DSD_CALL_PHASE_ACTIVE,
    DSD_CALL_PHASE_ENDED,
} dsd_call_phase;

typedef enum {
    DSD_CALL_KIND_UNKNOWN = 0,
    DSD_CALL_KIND_VOICE,
    DSD_CALL_KIND_GROUP_VOICE,
    DSD_CALL_KIND_PRIVATE_VOICE,
    DSD_CALL_KIND_DATA,
} dsd_call_kind;

typedef enum {
    DSD_CALL_CRYPTO_UNKNOWN = 0,
    DSD_CALL_CRYPTO_CLEAR,
    DSD_CALL_CRYPTO_ENCRYPTED_PENDING,
    DSD_CALL_CRYPTO_ENCRYPTED,
    DSD_CALL_CRYPTO_DECRYPTABLE,
} dsd_call_crypto_state;

/**
 * Why a call epoch ended.
 *
 * Only the recoverable reasons -- DSD_CALL_END_SYNC_LOSS and
 * DSD_CALL_END_UNVERIFIED_TERMINATOR -- permit the next epoch on the slot to be treated as the
 * same transmission being reacquired, and the unverified-terminator reason permits it only for
 * identity-less continuations: the terminator was positive (if fallible) evidence the
 * transmission ended, so an identity-bearing observation after it is the next transmission.
 * EXPLICIT is the default so an unconverted call site produces a second history row -- a
 * duplicate is recoverable, a deleted transmission is not.
 *
 * EXPLICIT and TERMINATOR are equally final; they differ only in what they say happened on the
 * air. The engine ends epochs EXPLICIT on retune and teardown, so an EXPLICIT end is not
 * evidence a transmission ended over the air -- a keep-or-drop decision about whether the air
 * carried a real, positively-ended transmission must key on the terminator reasons.
 */
typedef enum {
    DSD_CALL_END_EXPLICIT = 0,  /**< Teardown, retune, or an unconverted OTA end site (EOT, release). */
    DSD_CALL_END_SYNC_LOSS = 1, /**< Carrier or sync lost; the transmission may still resume. */
    /**
     * A terminator burst decoded but its link control failed verification (a RAS-masked CRC, a
     * protected LC, a marginal signal). Ends the call on the strength of the burst type while
     * staying recoverable, so a voice burst mis-typed as a terminator mid-call heals instead of
     * splitting the transmission. A second unverified terminator corroborates the end and
     * tightens it to TERMINATOR.
     */
    DSD_CALL_END_UNVERIFIED_TERMINATOR = 2,
    /**
     * A terminator whose link control verified: positive, trustworthy over-the-air evidence the
     * transmission ended. Final like EXPLICIT, but distinguishable from an engine retune so the
     * event layer can vouch for an audible transmission the terminator closed.
     */
    DSD_CALL_END_TERMINATOR = 3,
} dsd_call_end_reason;

/**
 * Seconds after a sync-loss end within which the next epoch describing the same call is read as
 * that transmission being reacquired rather than a new one. Also how long a VOICE_END alert is
 * held before the transmission is declared over. Rationale, residual risks and the replay-timing
 * caveat are documented at the point of use in src/core/util/call_state.c.
 */
#define DSD_CALL_REACQUIRE_GAP_S       0.5

/**
 * Seconds after an unverified-terminator end within which an identity-less continuation may
 * heal the epoch. Much tighter than DSD_CALL_REACQUIRE_GAP_S: the heal exists for a voice burst
 * mis-typed as a terminator, whose per-frame media mark follows within a burst or two, while a
 * real end followed by a fast re-key can put a new transmission's first decodable voice frame on
 * the slot well inside the sync-loss window -- and folding that into the terminated call would
 * hand it the previous call's identity and crypto.
 */
#define DSD_CALL_TERMINATOR_HEAL_GAP_S 0.25

typedef enum {
    DSD_CALL_BOUNDARY_CONTINUE = 0, /**< Merge compatible observations into the active call epoch. */
    /**
     * Begin a new epoch, except when an identity-bearing voice observation specializes the active
     * protocol-compatible, identity-less generic voice epoch.
     */
    DSD_CALL_BOUNDARY_BEGIN,
} dsd_call_boundary;

typedef struct {
    int protocol; /**< DSD_SYNC_* value; use DSD_SYNC_NONE when unobserved. */
    uint8_t slot;
    dsd_call_kind kind;
    uint64_t ota_target_id;
    uint64_t policy_target_id;
    uint64_t ota_source_id;
    char source_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char target_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char route_text[DSD_CALL_ROUTE_COUNT][DSD_CALL_IDENTITY_TEXT_SIZE];
    uint32_t channel;
    int64_t frequency_hz;
    uint16_t service_options;
    uint8_t emergency;
    uint8_t priority;
    uint8_t has_service_metadata; /**< Non-zero when service options, emergency, and priority were observed. */
    double observed_m;
} dsd_call_observation;

static inline dsd_call_observation
dsd_call_observation_data(int protocol, uint8_t slot, uint64_t source_id, uint64_t target_id) {
    dsd_call_observation observation = {0};
    observation.protocol = protocol;
    observation.slot = slot;
    observation.kind = DSD_CALL_KIND_DATA;
    observation.ota_source_id = source_id;
    observation.ota_target_id = target_id;
    return observation;
}

typedef struct {
    dsd_call_crypto_state classification;
    uint8_t algid;
    uint16_t kid;
    uint64_t mi;
    uint8_t audio_permitted;
    double observed_m;
} dsd_call_crypto_update;

typedef struct {
    uint64_t revision;
    uint64_t epoch;
    uint64_t ota_target_id;
    uint64_t policy_target_id;
    uint64_t ota_source_id;
    int64_t frequency_hz;
    uint64_t mi;
    double started_m;
    double updated_m;
    double ended_m;
    dsd_call_phase phase;
    int protocol; /**< DSD_SYNC_* value, or DSD_SYNC_NONE when unobserved. */
    dsd_call_kind kind;
    uint32_t channel;
    dsd_call_crypto_state crypto;
    uint16_t service_options;
    uint16_t kid;
    uint8_t slot;
    uint8_t has_service_metadata; /**< Non-zero after confirmed service metadata has been applied. */
    uint8_t emergency;
    uint8_t priority;
    uint8_t algid;
    uint8_t audio_permitted;
    uint8_t media_active;
    uint8_t end_reason; /**< dsd_call_end_reason; meaningful only while phase is DSD_CALL_PHASE_ENDED. */
    char source_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char target_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char route_text[DSD_CALL_ROUTE_COUNT][DSD_CALL_IDENTITY_TEXT_SIZE];
} dsd_call_snapshot;

typedef struct {
    uint64_t revision;
    dsd_call_snapshot slots[DSD_CALL_STATE_SLOT_COUNT];
} dsd_call_state_snapshot;

typedef struct {
    dsd_call_observation observation;
    char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
    uint64_t updated_m_ms;
} dsd_recent_activity_entry;

typedef struct {
    uint64_t revision;
    dsd_recent_activity_entry entries[DSD_RECENT_ACTIVITY_COUNT];
} dsd_recent_activity_snapshot;

/**
 * Live decoder inputs the per-protocol event builders read but a history row does not carry,
 * plus the render-time verdicts about the epoch the row describes.
 *
 * Captured when a row is committed and replayed when that row is re-rendered, so a merged
 * transmission is described by the system context it was decoded under rather than whatever the
 * decoder has retuned to since. Everything else a builder needs already lives on the row.
 *
 * The verdicts live inside this struct rather than as siblings so every site that clears or
 * copies the environment carries them automatically; a paired manual update would eventually be
 * missed and leave a stale verdict vouching for the wrong row.
 */
typedef struct {
    uint16_t nxdn_grant_chan;
    long nxdn_grant_freq;
    unsigned int mfid;
    int ea_mode;
    int edacs_a_bits;
    int edacs_f_bits;
    int edacs_s_bits;
    int edacs_a_shift;
    int edacs_f_shift;
    int edacs_a_mask;
    int edacs_f_mask;
    int edacs_s_mask;
    /* Whether the epoch had named a call when the row was last rendered. Vouches for identity
     * the row strings never carry -- the route text anchoring a D-STAR or YSF transmission whose
     * talker callsigns did not decode -- including on the epoch-change commit path where the
     * canonical snapshot is already gone. Identity only accrues within an epoch, so plain
     * assignment per render is already sticky. */
    uint8_t named_call;
    /* Whether the epoch carried decoded voice media at any render. Sticky for the epoch's
     * lifetime: the canonical snapshot clears media_active when the call ends, but the row's
     * keep-or-drop verdict is about whether audio ever ran, not whether it is running now. */
    uint8_t saw_media;
    /* Whether the epoch's end, as last rendered, was terminator-evidenced (TERMINATOR or
     * UNVERIFIED_TERMINATOR). Zero while the call is active, after a bare sync loss, and after
     * an engine retune or teardown -- those end EXPLICIT, which says nothing about the air. */
    uint8_t ended_positively;
} dsd_call_event_render_env;

/** Event bookkeeping paired with one canonical call slot. */
typedef struct {
    uint64_t epoch;
    uint64_t notice_epoch;
    uint64_t notice_target_id;
    /* Epoch that reopened a sync-loss-ended epoch describing the same call. Only
     * this reacquisition may merge into the row most recently committed.
     *
     * Never cleared when an unrelated epoch begins: the comparison is epoch-exact
     * and epoch ids only increase, so a stale value cannot match. Clearing it early
     * would disarm a reacquisition whose staged row has not been flushed yet, and
     * that row would then commit as a duplicate. */
    uint64_t reacquired_epoch;
    /* The sync-loss-ended epoch that reacquired_epoch reopened. A merge is only
     * legitimate into the row that epoch itself committed, so the event layer pairs
     * this against committed_epoch before folding anything. */
    uint64_t reacquired_from_epoch;
    /* The epoch whose row committed_seq locates. Both the reacquisition merge and
     * dsd_event_enrich_epoch() are epoch-scoped: an epoch that ends without pushing
     * a row leaves this pointing at an older epoch, and neither may act on it. */
    uint64_t committed_epoch;
    /* Value of Event_History_I::push_seq right after this slot's last voice
     * commit reached history. The retained row's current depth is
     * 1 + (push_seq - committed_seq), so interleaved notice pushes cannot make
     * the merge target the wrong row. */
    uint64_t committed_seq;
    /* Render inputs as they stood when the staged row was last rendered. Captured with the row's
     * content rather than at commit time: a row is sometimes committed only once the decoder has
     * already moved on to the next call, and by then the live values describe that call. */
    dsd_call_event_render_env staged_env;
    /* Render inputs belonging to committed_seq's row, promoted from staged_env when it was
     * pushed. */
    dsd_call_event_render_env committed_env;
    uint8_t committed_valid;
    uint8_t ended_committed;
    uint8_t notice_kind;
    uint8_t notice_handled;
    /* A sync-loss end may still be reacquired, so its VOICE_END alert is held until the
     * reacquisition window closes. Stamped with the monotonic deadline to beep at. */
    uint8_t end_alert_pending;
    double end_alert_due_m;
} dsd_call_event_lifecycle_snapshot;

/**
 * Complete canonical call context for runtime context switching.
 *
 * Restoring this snapshot keeps epoch allocation monotonic within the destination
 * state but deliberately drops event commit bookkeeping: the event history is
 * global while this context is per trunk-scan target, so a commit reference or a
 * pending reacquisition carried across a hop would describe another target's row.
 * dsd_call_context_restore_snapshot() invalidates both, and downgrades a retained
 * sync-loss end so the first call on the new target cannot be read as a
 * reacquisition of the old one.
 */
typedef struct {
    dsd_call_state_snapshot calls;
    dsd_recent_activity_snapshot recent;
    dsd_call_event_lifecycle_snapshot events[DSD_CALL_STATE_SLOT_COUNT];
} dsd_call_context_snapshot;

typedef struct {
    uint8_t valid;
    uint8_t index;
    dsd_recent_activity_entry entry;
} dsd_recent_activity_transaction;

/**
 * @brief Family id the store uses to decide whether two observations describe the same call.
 *
 * Observations from different families always begin a new epoch. Callers that pre-judge an epoch
 * boundary (to coalesce, suppress, or log one) must mirror this, or their decision will disagree
 * with what dsd_call_state_observe() actually does.
 */
int dsd_call_state_protocol_family(int protocol);

/** Ensure the canonical call-state extension and its transaction mutex exist. */
int dsd_call_state_ensure(dsd_state* state);
int dsd_call_state_observe(dsd_state* state, const dsd_call_observation* observation, dsd_call_boundary boundary);
int dsd_call_state_update_crypto(dsd_state* state, uint8_t slot, const dsd_call_crypto_update* update);
/** Update crypto metadata on an existing active or retained ended epoch. */
int dsd_call_state_update_retained_crypto(dsd_state* state, uint8_t slot, const dsd_call_crypto_update* update);
int dsd_call_state_update_media(dsd_state* state, uint8_t slot, int media_active, double observed_m);
/**
 * End the active epoch, recording why it ended.
 *
 * Returns non-zero when the call state changed -- either the epoch was ended, or an already
 * recoverably-ended epoch had its reason tightened to a final one: a verified terminator or an
 * EXPLICIT teardown retracts either recoverable reason, and a second unverified terminator
 * corroborates an unverified-terminator end into DSD_CALL_END_TERMINATOR. An unverified
 * terminator alone never tightens a sync-loss end -- it is the same fallible evidence the
 * recoverable end exists to distrust. Tightening leaves ended_m untouched. Callers that gate
 * dsd_event_sync_slot() on this result therefore let the event layer see the retracted
 * reacquisition permission; callers that need "an active call was ended" specifically must
 * check DSD_CALL_PHASE_ACTIVE themselves beforehand.
 */
int dsd_call_state_end_ex(dsd_state* state, uint8_t slot, double observed_m, dsd_call_end_reason reason);
/** End the active epoch as a deliberate teardown (DSD_CALL_END_EXPLICIT). */
int dsd_call_state_end(dsd_state* state, uint8_t slot, double observed_m);

/**
 * Called after dsd_call_state_observe() heals a recoverably-ended epoch, with the ending
 * snapshot the reopened epoch was seeded from. The canonical layer stays protocol-neutral:
 * a protocol whose end path tears down live decoder state it would need back on a heal (the
 * DMR terminator reset clearing the slot's crypto) installs a hook and restores its own
 * fields, keyed on the snapshot's protocol and epoch. Invoked outside the canonical lock, on
 * the observing thread. Install once from the decode path before the first heal can occur;
 * installing NULL removes it.
 */
typedef void (*dsd_call_state_reacquire_hook)(dsd_state* state, uint8_t slot, const dsd_call_snapshot* previous);
void dsd_call_state_set_reacquire_hook(dsd_call_state_reacquire_hook hook);
int dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out);
int dsd_call_state_copy_snapshot(const dsd_state* state, dsd_call_state_snapshot* out);
int dsd_call_state_restore_snapshot(dsd_state* state, const dsd_call_state_snapshot* snapshot);
int dsd_call_context_copy_snapshot(const dsd_state* state, dsd_call_context_snapshot* out);
int dsd_call_context_restore_snapshot(dsd_state* state, const dsd_call_context_snapshot* snapshot);
int dsd_call_state_enrich_text(dsd_state* state, uint8_t slot, uint64_t epoch, const char* source_text,
                               const char* target_text, const char* route0_text, const char* route1_text,
                               double observed_m);

int dsd_recent_activity_publish(dsd_state* state, uint8_t index, const dsd_call_observation* observation,
                                const char* notice, uint64_t observed_m_ms);
int dsd_recent_activity_clear(dsd_state* state, uint8_t index);
int dsd_recent_activity_clear_all(dsd_state* state);
int dsd_recent_activity_copy_snapshot(const dsd_state* state, dsd_recent_activity_snapshot* out);
int dsd_recent_activity_restore_snapshot(dsd_state* state, const dsd_recent_activity_snapshot* snapshot);
int dsd_recent_activity_expire(dsd_state* state, uint64_t now_m_ms, uint64_t ttl_ms);
int dsd_recent_activity_save(const dsd_state* state, uint8_t index, dsd_recent_activity_transaction* transaction);
int dsd_recent_activity_restore(dsd_state* state, const dsd_recent_activity_transaction* transaction);

/** Copy the canonical extension into another `dsd_state` snapshot. */
int dsd_call_state_copy_to_state(dsd_state* dst, const dsd_state* src);

#ifdef DSD_NEO_TEST_HOOKS
void dsd_call_state_test_alloc_reset(void);
void dsd_call_state_test_alloc_fail_after(long fail_after);
int dsd_call_state_test_set_epoch(dsd_state* state, uint8_t slot, uint64_t epoch);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_STATE_H_H */
