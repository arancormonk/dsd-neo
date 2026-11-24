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

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>

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

/**
 * A P25 radio's home identity, its Source SUID (WACN, System ID, unit ID), as an extended voice user names it. It is
 * shown, and matched against a talker alias that names the same SUID, but it is never a local radio ID: the call's
 * ota_source_id stays the radio's working address on the system received (0 when the message names none). It names
 * the talker only while no later voice user on the call names another working address than @ref source
 * (dsd_call_state_note_source_address()).
 */
typedef struct {
    uint32_t wacn; /**< 20 bits. */
    uint32_t unit; /**< 24 bits; names a radio when valid. */
    /** The working address the voice user that named it carried; 0: none (named by its home alone). */
    uint32_t source;
    uint16_t sysid; /**< 12 bits. */
    uint8_t valid;  /**< 0: no home identity. */
    /** 1 (with valid 0): a home named an earlier talker of this call and a later voice user withdrew it
        (dsd_call_state_note_source_address()), so a row merged from this call drops the home and alias it holds. */
    uint8_t withdrawn;
} dsd_call_source_home;

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
    uint8_t channel_kind; /**< dsd_call_channel_kind of @ref channel; ignored without a channel. */
    int64_t frequency_hz;
    uint16_t service_options;
    uint8_t emergency;
    uint8_t priority;
    uint8_t has_service_metadata; /**< Non-zero when service options, emergency, and priority were observed. */
    /**
     * Non-zero when the call names a party its decoder could not read as an ID (a played SDRTrunk roaming or ISSI
     * identity, issue #888): sticky for the epoch (dsd_call_snapshot::identity_unread). 0 says nothing.
     */
    uint8_t identity_unread;
    /**
     * dsd_analog_mode (<dsd-neo/core/analog_mode.h>) of an analog (DSD_SYNC_ANALOG) call; 0 (unknown) otherwise.
     * Taken by the observation that begins an epoch; a later observation never changes it.
     */
    uint8_t analog_mode;
    /**
     * Set on a BEGIN observation: @ref receive_seed is the receive identity the call is heard with, used instead of
     * the live publication (dsd_receive_identity_current()), and the call's service name is the seed's. The analog
     * tracker hands over the tone it confirmed before the call qualified, which the live publication may since have
     * lost or changed (issue #667). Ignored on CONTINUE.
     */
    uint8_t has_receive_seed;
    dsd_receive_identity receive_seed;
    /**
     * Decode-clock wall seconds (the dsd_decode_* wall domain) of the call's first sample, on the observation that
     * begins an epoch; kept, fractional, as dsd_call_snapshot::start_wall. 0: none, the row's stamps are derived from
     * its render as before. With it, that observation's @ref observed_m must be the same first-sample instant on the
     * decode-mono clock, since the row's end is this start plus the epoch's elapsed from @ref observed_m.
     */
    double start_wall;
    double observed_m;
} dsd_call_observation;

static inline dsd_call_observation
dsd_call_observation_data(int protocol, uint8_t slot, uint64_t source_id, uint64_t target_id) {
    /* Zeroed rather than `= {0}`: this header is compiled as both C11 and C++14,
     * and GCC's -Wmissing-field-initializers (via -Wextra, which the analyzer
     * runs) names every field the brace form leaves out when the translation
     * unit is C++. Spelling the fields out instead would be positional — C11 has
     * no designated initializers that C++14 also accepts — and would silently
     * mis-assign the day a field is reordered. Same device as dsd_opts. */
    dsd_call_observation observation;
    DSD_MEMSET(&observation, 0, sizeof observation);
    observation.protocol = protocol;
    observation.slot = slot;
    observation.kind = DSD_CALL_KIND_DATA;
    observation.ota_source_id = source_id;
    observation.ota_target_id = target_id;
    return observation;
}

/**
 * Crypto provenance bits (dsd_call_crypto_update::received, dsd_call_snapshot::crypto_received): which of an update's
 * ALG and KID were decoded from over-the-air signalling that passed its protocol's CRC/FEC check (a P25 HDU, LDU2,
 * ESS or MAC_PTT, a DMR PI header, an NXDN corroborated VCALL, an M17 LSF). A forced, inferred, key-loaded, carried
 * or synthesized value never sets one.
 */
enum {
    DSD_CALL_CRYPTO_RX_ALG = 1U << 0,
    DSD_CALL_CRYPTO_RX_KID = 1U << 1,
};

/** dsd_call_crypto_update::enc_observed values. */
enum {
    DSD_CALL_ENC_OBSERVED_NONE = 0,      /**< this update carries no checked encryption indication */
    DSD_CALL_ENC_OBSERVED_ENCRYPTED = 1, /**< checked signalling says encrypted */
    DSD_CALL_ENC_OBSERVED_CLEAR = 2,     /**< checked signalling says clear */
};

typedef struct {
    dsd_call_crypto_state classification;
    uint8_t algid;
    uint16_t kid;
    uint64_t mi;
    uint8_t audio_permitted;
    double observed_m;
    /** DSD_CALL_CRYPTO_RX_* bits: which of algid and kid this update received over the air (0: none). */
    uint8_t received;
    /**
     * DSD_CALL_CRYPTO_RX_* bits whose received value checked signalling in this update contradicts: the epoch drops
     * those received fields (a field also in @c received is received anew instead). 0 leaves them alone.
     */
    uint8_t contradicted;
    /**
     * What checked over-the-air signalling in this update says about encryption (DSD_CALL_ENC_OBSERVED_*), kept in
     * dsd_call_snapshot::enc_received apart from the classification the decoder acts on. Applied even when @c carried
     * is set: it is the new call's own signalling.
     */
    uint8_t enc_observed;
    /**
     * Non-zero when algid, kid and mi are live decoder values a previous call left behind (its terminator missed),
     * which the decoder still decrypts with but which are not this epoch's: the record keeps its own algid, kid and
     * mi (none, on a new epoch) and takes only the classification and audio permission. received must be 0.
     */
    uint8_t carried;
} dsd_call_crypto_update;

typedef enum {
    DSD_CALL_KEY_NONE = 0,
    DSD_CALL_KEY_DIRECT = 1,
    DSD_CALL_KEY_SIGNALED = 2,
    DSD_CALL_KEY_TALKGROUP = 3,
    DSD_CALL_KEY_DESTINATION = 4,
    DSD_CALL_KEY_DEFAULT = 5
} dsd_call_key_source;

enum {
    DSD_CALL_KEY_FALLBACK_NONE = 0,
    DSD_CALL_KEY_FALLBACK_MAPPED_MISSING = 1,
    DSD_CALL_KEY_FALLBACK_MAPPED_INCOMPATIBLE = 2,
    DSD_CALL_KEY_FALLBACK_UNKNOWN_ALGORITHM = 3
};

typedef struct {
    uint64_t key_epoch;
    int32_t effective_id; /**< -1 for direct material without an identifier. */
    uint16_t signaled_id;
    uint8_t valid;
    uint8_t source;
    uint8_t algorithm;
    int8_t available; /**< -1 unknown/not applicable, 0 unavailable, 1 material available. */
    uint8_t fallback;
    char profile_ref[64];
} dsd_call_key_selection;

typedef struct {
    uint64_t revision;
    uint64_t epoch;
    /**
     * The epoch that opened this logical transmission: this epoch's own, or, for an epoch that reacquired a
     * recoverably-ended one, that epoch's transmission_epoch. State bound to a call (a trunking identity) keys on it
     * (dsd_call_snapshot_transmission()), so a fade the canonical layer healed keeps the binding and a new transmission
     * never inherits it.
     */
    uint64_t transmission_epoch;
    uint64_t ota_target_id;
    uint64_t policy_target_id;
    uint64_t ota_source_id;
    int64_t frequency_hz;
    uint64_t mi;
    double started_m;
    double updated_m;
    double ended_m;
    /**
     * Decode-clock wall seconds of the epoch's first sample, as the observation that began it gave it
     * (dsd_call_observation::start_wall), or 0. Kept fractional: an analog row's start stamp is its floor and its end
     * stamp the floor of it plus the epoch's elapsed (issue #667).
     */
    double start_wall;
    /** First decoded-media update in this logical transmission (monotonic seconds), or 0. */
    double media_started_m;
    /** Most recent decoded-media update in this logical transmission (monotonic seconds), or 0. */
    double media_updated_m;
    /**
     * Checked media (issue #890): the first and the latest frame of this logical transmission that its decoder output
     * as speech, successfully and unconcealed (dsd_call_state_note_checked_media()), monotonic seconds, or 0; and the
     * scan observation origin those frames were read under (dsd_scan_observation_origin: the scan visit and the carrier
     * count). A frame of another origin starts the span again. The media clocks above take every frame the call plays;
     * only these are scan credit (dsd_scan_voice_probe()).
     */
    double checked_media_started_m;
    double checked_media_updated_m;
    uint64_t checked_media_visit;
    uint32_t checked_media_carrier_seq;
    /** 1 when the checked media span is TETRA direct mode speech (dsd_call_state_note_checked_media_tetra(), issue
     * #890): a TETRA DMO scan row takes no other TETRA media. A frame of the other mode starts the span again. */
    uint8_t checked_media_tetra_direct;
    /** The reception generation when this epoch began (dsd_state_reception_generation()). */
    uint64_t rx_gen;
    dsd_call_phase phase;
    int protocol; /**< DSD_SYNC_* value, or DSD_SYNC_NONE when unobserved. */
    dsd_call_kind kind;
    uint32_t channel;
    /**
     * dsd_call_channel_kind of @ref channel (a DMR trunked call's LSN, LPCN or LCN), meaningful with a nonzero channel.
     * Set with the channel it names and never replaced afterwards: once a call has a channel of a known kind, neither
     * changes for the epoch (a reacquired epoch keeps them). NONE: another protocol's channel, replaced as observed.
     */
    uint8_t channel_kind;
    /**
     * dsd_analog_mode (<dsd-neo/core/analog_mode.h>) of an analog call, from the observation that began the epoch and
     * kept for it: the demodulator the call was heard through, or unknown on audio input. 0 on every other protocol.
     */
    uint8_t analog_mode;
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
    uint8_t end_reason;  /**< dsd_call_end_reason; meaningful only while phase is DSD_CALL_PHASE_ENDED. */
    uint8_t crc_invalid; /**< Sticky DSD_CRC_* bits (crc_provenance.h) for this transmission; zero is not verified. */
    /**
     * 1 once an observation of this transmission named a party its decoder could not read as an ID
     * (dsd_call_observation::identity_unread); a reacquired epoch carries it. Its history row never takes the rdio
     * fallback talkgroup (Event_History::fallback_blocked, issue #888).
     */
    uint8_t identity_unread;
    /**
     * 1 when the call ended on a terminator its own protocol's decoder verified, of a call kind that does not
     * contradict its own (dsd_call_state_end_own_terminator()), 0 for any other end, another protocol's terminator or
     * another call's on the slot included, a restored context's teardown (dsd_call_context_restore_snapshot()), and
     * while the call is active. A YSF row takes the rdio fallback talkgroup only with it
     * (Event_History::fallback_blocked, issue #888).
     */
    uint8_t own_terminator;
    /**
     * DSD_CALL_CRYPTO_RX_* bits: which of received_algid and received_kid this transmission received over the air.
     * Sticky for the epoch: an update that did not receive a field leaves its received value alone, so a forced or
     * inferred ALG ID, or a live value a sync loss cleared, changes algid/kid but never these. A new epoch starts
     * with none; a reacquired epoch carries the ending epoch's, as it carries its identity.
     */
    uint8_t crypto_received;
    uint8_t received_algid;
    uint16_t received_kid;
    /**
     * Non-zero while algid, kid and mi are the values a reacquired epoch was seeded with: an update that neither
     * received nor names an ALG ID or key id (the live values a sync loss cleared) leaves them, so the recovered call
     * keeps its crypto until the transmission's own signalling replaces it.
     */
    uint8_t crypto_restored;
    /**
     * 1 while checked over-the-air signalling of this epoch says the call is encrypted (dsd_call_crypto_update::
     * enc_observed): a DMR LC privacy bit, a P25 ALG ID other than 0x80 or the service options' encryption bit, an
     * NXDN VCALL or SACCH-2 cipher, a dPMR verified scrambled half, an M17 LSF encryption type. Checked signalling
     * saying clear resets it. Never set by a forced, inferred or key-loaded classification, so the crypto
     * classification can say encrypted while this says nothing was received.
     */
    uint8_t enc_received;
    char source_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char target_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char route_text[DSD_CALL_ROUTE_COUNT][DSD_CALL_IDENTITY_TEXT_SIZE];
    dsd_call_source_home source_home; /**< The talker's P25 home SUID, when a voice user named it. */
    /** The P25 trunk state machine's identity verdict on ota_source_id (issue #800), mirrored from its record of the
        call (dsd_call_state_note_source_verdict()): the source it checked, and the reception it was checked under. A
        talker alias is learned for a P25 source only while this positively names it (dsd_call_state_source_learnable()):
        never for a foreign home unit ID, an unchecked decode, or a working address an addressless roamer replaced. */
    uint8_t source_checked;
    uint32_t source_checked_id;
    uint64_t source_checked_rx_gen;
    dsd_call_key_selection key_selection;
    dsd_receive_identity receive_identity;
    uint8_t receive_verdict;
    char service_label[DSD_SERVICE_LABEL_SIZE];
    /** dsd_received_identity::epoch of dsd_state::rx_id when this epoch began: the carrier the call was heard on. */
    uint32_t rx_id_epoch;
    /**
     * The received identity record (dsd_state::rx_id) as it stood when this epoch ended, captured at that transition
     * while the reception was still the one the call began on (the same reception generation, rx_gen: no reset, other
     * record, retune or change of P25 phase in between); otherwise, and while the call is active, empty (family
     * DSD_RXID_FAMILY_NONE). Either way it describes the reception of rx_gen, never another. Call history freezes an
     * ended call's system and site at it, so identity received after the last render and before the end counts, and
     * identity received after the end, or on another carrier, does not. Reacquisition compares reacquire_rx_id instead,
     * which outlives a record that moved before the end.
     */
    dsd_received_identity ended_rx_id;
    /**
     * The record a reopen of this call is compared with (dsd_call_record_site_compatible(), whole, so the trunking
     * system a Capacity Plus rest LSN or an XPT free LCN names stays): dsd_state::rx_id as of the epoch's last
     * observation, or its end, made while the record was still the call's (its epoch equal to rx_id_epoch). A record
     * that moved later (another system named, a reset, a sync of another protocol family wiping it) leaves it as it
     * was, so a call heard again under an empty record reopens and one heard under another identity does not. A
     * reacquired epoch starts with the ending one's and keeps it while the live record names no site, so a second
     * fade still compares with the site the call was heard on. After such a heal a live record that names only part
     * of the earlier site replaces the retained copy, so the missing parts drop out of later comparisons until they
     * are noted again (a decided limit). Refreshed only at an observation and at the end: identity learned after the
     * last observation of a call whose end captured nothing compares as compatible (a decided limit). Empty while
     * nothing was received.
     */
    dsd_received_identity reacquire_rx_id;
    /** 1 when a TETRA call ended on a direct mode carrier, which belongs to no network and leaves ended_rx_id empty;
     * call history marks the ended row direct mode from it. */
    uint8_t ended_tetra_direct;
    /**
     * The source's display label (source alias, else group-list name) the event layer resolved for this epoch's row,
     * cached when the row commits and again when a late talker alias renames it, so a view of the ended call shows it
     * after the staged row has gone without searching the source tables per render. Valid only while
     * source_label_id equals ota_source_id; empty when the epoch's row had none.
     */
    char source_label[DSD_CALL_IDENTITY_TEXT_SIZE];
    uint64_t source_label_id;
    /**
     * The target's display label (dsd_call_target_label_lookup(): the talkgroup's deciding row for a group call, the
     * radio label for a private call) the event layer resolved for this epoch's row, cached when the row commits so the
     * ended-call hold still names the call after the staged row has gone. Valid only while target_label_id equals the
     * call's target (ota_target_id, else policy_target_id); empty when the epoch's row had none.
     */
    char target_label[DSD_CALL_IDENTITY_TEXT_SIZE];
    uint64_t target_label_id;
    /**
     * Issue #782: the label of the channel the epoch's row was heard on (a `-Y` row name or a trunk-scan target label),
     * cached when the row commits so a view of the ended call names it as it did on air. Valid only while
     * channel_label_epoch equals epoch; empty when the row had none.
     */
    char channel_label[64];
    uint64_t channel_label_epoch;
} dsd_call_snapshot;

/** The logical transmission @p call belongs to (dsd_call_snapshot::transmission_epoch, else its own epoch); 0 none. */
static inline uint64_t
dsd_call_snapshot_transmission(const dsd_call_snapshot* call) {
    if (!call) {
        return 0U;
    }
    return call->transmission_epoch != 0U ? call->transmission_epoch : call->epoch;
}

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
    /* The call's VOICE_START was withheld while the receive policy was still checking it (issue #637): it sounds once
       the call is admitted, never for one rejected. */
    uint8_t start_alert_pending;
    double end_alert_due_m;
    /* An identity-less voice row whose recoverable end lacks positive end evidence is held
     * un-committed rather than dropped: a terminator decoding after the fade retracts the
     * recoverable reason and vouches for the row, and an immediate drop would be irrevocable.
     * Stamped with the monotonic deadline to give up and drop at -- the same reacquisition
     * window the canonical layer applies to the end reason. */
    uint8_t drop_hold_pending;
    double drop_hold_due_m;
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
/** Refresh active admission and naming from current validated reception; ended records are immutable. */
void dsd_call_state_refresh_receive(const dsd_opts* opts, dsd_state* state, uint8_t slot);
/** Whether a call (voice or data) is active on either slot that the receive policy in force admits, judged now by the
 * call's own protocol and slot, and still heard: observed within @p window_s of @p now_m (decode clock). A call whose
 * terminator was missed stays ACTIVE, so its phase alone holds nothing (issue #637). */
int dsd_call_state_admitted_call_active(const dsd_opts* opts, const dsd_state* state, double now_m, double window_s);
int dsd_call_state_observe(dsd_state* state, const dsd_call_observation* observation, dsd_call_boundary boundary);
/*
 * Whose call a crypto or identity update describes.
 *
 * Every update below that writes a call's crypto (ALG ID, key id, MI, classification, audio permission, key choice) or
 * its identity text names @p protocol, the DSD_SYNC_* value of the protocol whose signalling it comes from. It applies
 * only to a call of that protocol (dsd_call_state_protocol_family(), with P25 Phase 1 and Phase 2 one protocol: a Phase
 * 2 voice channel's call can be opened from a Phase 1 control channel's grant). A slot keeps an ended call, and another
 * protocol's call whose end was never decoded, until the next call begins there, so a frame of one protocol must never
 * describe what the slot holds without asking whose it is: such an update is refused and returns 0, as for a call in a
 * phase the update does not take. A call whose protocol is DSD_SYNC_NONE is no protocol's. @p protocol DSD_SYNC_NONE
 * is a bad argument (-1).
 */
/** Update crypto metadata on the slot's active epoch, a call of @p protocol (see above). Returns 1 when applied (or
 * already so), 0 when the slot holds no active call of that protocol, -1 on bad arguments. */
int dsd_call_state_update_crypto(dsd_state* state, uint8_t slot, int protocol, const dsd_call_crypto_update* update);
/** Record resolver provenance on the matching call epoch, a call of @p protocol, without changing call
 * identity, activity age, crypto classification, media or listening policy. */
int dsd_call_state_note_key_selection(dsd_state* state, uint8_t slot, int protocol, uint64_t epoch,
                                      dsd_call_key_source source, int signaled_id, int effective_id, int available,
                                      int fallback);
/** The slot's active epoch now describes another transmission (one that replaced it while the canonical layer kept the
 * epoch, e.g. an identity-less epoch a new call's LC names): forget the ALG ID, key id, MI, received crypto and
 * restored mark the replaced transmission published into it, so they are never shown as the new call's. The
 * classification and audio permission stay until the new call's next crypto update. Returns 1 when the record
 * changed, 0 when the slot holds no active epoch of @p protocol (or nothing to forget), -1 on bad arguments. */
int dsd_call_state_forget_crypto_values(dsd_state* state, uint8_t slot, int protocol);
/** Update crypto metadata on an existing active or retained ended epoch, a call of @p protocol. */
int dsd_call_state_update_retained_crypto(dsd_state* state, uint8_t slot, int protocol,
                                          const dsd_call_crypto_update* update);
int dsd_call_state_update_media(dsd_state* state, uint8_t slot, int media_active, double observed_m);
/**
 * A frame of the slot's active call that its decoder output as speech, successfully and unconcealed, and could play
 * clear or decrypted (issue #890): an AMBE or IMBE frame mbelib-neo synthesized with no erasure, repeat, mute, silence
 * or uncorrectable C0 (and no tone), an M17 Codec2 frame Codec2 decoded, a TETRA speech frame whose CRC passed and the
 * codec decoded. Encrypted voice without a key is never one, however its FEC decodes. Producers note it only while a
 * scan that reads it runs (dsd_scan_uses_checked_media()). It raises the call's
 * checked media clocks (dsd_call_snapshot::checked_media_started_m, checked_media_updated_m, at @p observed_m, 0 for
 * now on the decode clock) under the frame's scan observation origin (dsd_state_scan_observation_origin()); a frame of
 * another origin starts the span again, and a frame a carrier boundary or an input interruption cut raises nothing. The
 * frame is @p protocol's (a DSD_SYNC_* value: the frame's own protocol, not the one whose vocoder it borrowed): it
 * counts only for a call of that protocol (P25 Phase 1 and Phase 2 one protocol). Playback, the media clocks and the
 * call's revision are left alone. Returns 1 when raised, 0 when the slot holds no active call of @p protocol or the
 * frame was cut, -1 on bad arguments. Decoder thread.
 */
int dsd_call_state_note_checked_media(dsd_state* state, uint8_t slot, int protocol, double observed_m);
/**
 * dsd_call_state_note_checked_media() for a TETRA speech frame (issue #890), which also names the mode its burst was
 * read in: @p direct 1 for direct mode (EN 300 396), 0 for trunked mode. The span keeps the mode
 * (dsd_call_snapshot::checked_media_tetra_direct), and a frame of the other mode starts it again, as a frame of
 * another origin does.
 */
int dsd_call_state_note_checked_media_tetra(dsd_state* state, uint8_t slot, int direct, double observed_m);
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
/**
 * End the slot's call on a terminator @p protocol's decoder verified, which names a call of @p kind
 * (DSD_CALL_KIND_UNKNOWN: none): as dsd_call_state_end_ex() with DSD_CALL_END_TERMINATOR, whatever call the slot
 * holds, and, when that call is @p protocol's (dsd_call_state_protocol_family()) and @p kind does not contradict the
 * call's kind (a group call's end on a private call's terminator does; a generic voice kind contradicts no voice
 * kind), marked as ended on its own terminator (dsd_call_snapshot::own_terminator).
 * Returns as dsd_call_state_end_ex(); -1 also for @p protocol DSD_SYNC_NONE.
 */
int dsd_call_state_end_own_terminator(dsd_state* state, uint8_t slot, double observed_m, int protocol,
                                      dsd_call_kind kind);
/** End the active epoch as a deliberate teardown (DSD_CALL_END_EXPLICIT). */
int dsd_call_state_end(dsd_state* state, uint8_t slot, double observed_m);

/**
 * True for the end reasons that leave the epoch reacquirable (DSD_CALL_END_SYNC_LOSS and
 * DSD_CALL_END_UNVERIFIED_TERMINATOR).
 *
 * The canonical predicate behind reacquisition itself, the event layer's held VOICE_END alert,
 * and a reacquire-hook owner's decision about whether the state it is about to tear down may
 * still be needed back. Any private mirror of the reason list would silently diverge when a
 * reason is added, so callers ask here instead of comparing reasons themselves.
 */
int dsd_call_state_end_reason_is_recoverable(uint8_t end_reason);

/**
 * True while the ended epoch @p call may still be reacquired at @p now_m (decode clock): it ended for a recoverable
 * reason, names a call, and its reason's own heal window (DSD_CALL_REACQUIRE_GAP_S after a sync loss,
 * DSD_CALL_TERMINATOR_HEAL_GAP_S after an unverified terminator) has not run out. The same rule dsd_call_state_observe()
 * applies before it heals a transmission, short of what the reopening observation itself must satisfy (an unverified
 * terminator's heal takes only identity-less voice, and no reopen may name another call): a binding to the
 * transmission (dsd_call_snapshot_transmission()) that holds while this is true holds for the call a heal reopens.
 */
int dsd_call_snapshot_may_reacquire(const dsd_call_snapshot* call, double now_m);
/**
 * True when @p observation names another call than @p call: the rule by which an observation begins a new epoch on an
 * active call (another protocol family, target, source ID or contradicting kind, or identity text that differs where
 * both are known). A field either side lacks is no contradiction. For a decoder that compares what it verified with an
 * ended call it may not observe (issue #888).
 */
int dsd_call_snapshot_contradicted_by(const dsd_call_snapshot* call, const dsd_call_observation* observation);

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
/**
 * The slot's call epoch and revision alone, into @p epoch and @p revision (0 and 0 with no call state), where a caller
 * only compares them across a step and needs no snapshot copy. Returns as dsd_call_state_get().
 */
int dsd_call_state_get_revision(const dsd_state* state, uint8_t slot, uint64_t* epoch, uint64_t* revision);
/**
 * How many observations slot @p slot has taken (dsd_call_state_observe(), an identical repeat included), a count that
 * wraps and that only a caller comparing it across a step reads: an observation is a decoder's checked content naming
 * the slot's call, which a media refresh (dsd_call_state_update_media()) never is, since it may refresh a call an
 * earlier transmission left active (issue #890). 0 with no call store or a bad slot.
 */
uint32_t dsd_call_state_observation_seq(const dsd_state* state, uint8_t slot);
int dsd_call_state_copy_snapshot(const dsd_state* state, dsd_call_state_snapshot* out);
int dsd_call_state_restore_snapshot(dsd_state* state, const dsd_call_state_snapshot* snapshot);
int dsd_call_context_copy_snapshot(const dsd_state* state, dsd_call_context_snapshot* out);
int dsd_call_context_restore_snapshot(dsd_state* state, const dsd_call_context_snapshot* snapshot);
/** Records @p home on slot @p slot's ACTIVE call of @p protocol's family (display and alias matching only, as an
 * observation's dsd_call_observation::source_home). @p replaced is set to 1 when it replaced another valid home: a
 * different radio now talks on the call. Returns 1 when applied, 0 when the slot holds no such call, -1 on bad
 * arguments. */
int dsd_call_state_note_source_home(dsd_state* state, uint8_t slot, int protocol, const dsd_call_source_home* home,
                                    int* replaced);
/** A voice user that names no home identity (an abbreviated form, or an extended one whose SUID names no radio) named
 * the working address @p source on slot @p slot's ACTIVE call of @p protocol's family. A home the call holds names the
 * talker only when the voice user that named it carried @p source too: one named with another working address, or with
 * none (a roaming radio named by its home alone), is an earlier talker's, and goes (dsd_call_source_home::withdrawn
 * records that it went). @p dropped is set to 1 when a home went. Returns 1 when applied, 0 when the slot holds no such
 * call, -1 on bad arguments (@p source 0 included). */
int dsd_call_state_note_source_address(dsd_state* state, uint8_t slot, int protocol, uint32_t source, int* dropped);
/** Mirrors the P25 trunk state machine's identity verdict on slot @p slot's call at @p epoch: @p checked 1 when its
 * record checks @p source under reception generation @p rx_gen, 0 when it withdrew or never earned it. Returns 1 when
 * applied, 0 when the slot holds another call. */
int dsd_call_state_note_source_verdict(dsd_state* state, uint8_t slot, uint64_t epoch, uint32_t source, int checked,
                                       uint64_t rx_gen);
/** Whether a talker alias may be learned for @p call's P25 source: its mirrored verdict (source_checked) names that
 * source under the reception @p state is in now. Other protocols' calls: 1 for a nonzero source. */
int dsd_call_state_source_learnable(const dsd_state* state, const dsd_call_snapshot* call);
/** The display-only source of a P25 call with no working address whose extended voice user named the talker's home
 * SUID (a roaming radio): `unit@system`, the decimal unit ID and the hex home System ID, into @p out. Returns 1 when
 * written; 0, with @p out empty, for any other call. Never a radio ID: shown, never matched against local lists. */
int dsd_call_state_format_home_source(const dsd_call_snapshot* call, char* out, size_t out_size);
/** Set the active epoch @p epoch's identity text, a call of @p protocol (dsd_call_state_update_crypto()); a NULL text
 * keeps its own. Returns 1 when applied, 0 when the slot holds no such call, -1 on bad arguments. */
int dsd_call_state_enrich_text(dsd_state* state, uint8_t slot, int protocol, uint64_t epoch, const char* source_text,
                               const char* target_text, const char* route0_text, const char* route1_text,
                               double observed_m);
/**
 * @brief Re-point the ACTIVE epoch @p epoch on @p slot at policy target @p policy_target, in place.
 *
 * For a patched P25 call whose resolution re-selected the member it is judged on: no boundary, and the over-the-air
 * identity, received service options and priority stay as received. Not an observation (updated_m is kept).
 *
 * @return 1 changed, 0 when the slot holds no ACTIVE epoch @p epoch or already names @p policy_target, -1 invalid
 *         arguments (NULL state, a slot out of range, epoch or target 0).
 */
int dsd_call_state_update_policy_target(dsd_state* state, uint8_t slot, uint64_t epoch, uint64_t policy_target);

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
/** How many whole-call snapshots dsd_call_state_get() has copied out since the process started. */
long dsd_call_state_test_snapshot_copies(void);
void dsd_call_state_test_alloc_reset(void);
void dsd_call_state_test_alloc_fail_after(long fail_after);
int dsd_call_state_test_set_epoch(dsd_state* state, uint8_t slot, uint64_t epoch);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_STATE_H_H */
