// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The attribution rule every position producer that names a report's sender from a call, not from the report
 * itself, borrows that identity by (issue #800, spec 2.2 and 2.19).
 *
 * A DMR embedded GPS LC, a P25 Phase 1 GPS link control and a P25 Phase 2 L3Harris MAC carry a position but no sender:
 * the sender is the one a checked message of the same call named. The producer records that evidence when the checked
 * message arrives (dsd_position_attribution_learn()) and asks for it when the report arrives
 * (dsd_position_attribution_grant()). Attribution is granted only when all of these hold:
 *  - the call the report rides on is open: ACTIVE, or the call a terminator carrying the report has just ended;
 *  - that call's protocol class (dsd_position_attribution_class(): P25 Phase 1 and Phase 2 apart) is the report's and
 *    the evidence's: a call of another protocol, or of the other P25 phase, still open on the slot is not this one;
 *  - it is the call the evidence was learned in: the same epoch, or for a producer that binds to the transmission
 *    (DMR, whose call layer heals a fade into a new epoch of one transmission) the same transmission;
 *  - the reception has not changed since: the received identity's epoch (dsd_received_identity::epoch, moved by
 *    another access code, system, site or family on the carrier) is the one the evidence was learned under. A trunk
 *    follow keeps that epoch; a carrier change ends the call or clears the evidence;
 *  - the call still names the evidence's source (and, for the target, its target);
 *  - for DMR, the burst carrying the report is the evidence's: the same sender kind by its sync word (a repeater's
 *    BS-sourced burst, a radio's MS-sourced one, direct mode's) on the same over-the-air slot
 *    (dsd_position_dmr_air_same()). A direct-mode call whose terminator was missed is not the repeater-mode call
 *    that follows it on the carrier, whatever the call layer kept.
 * Anything else (a relock to another colour code, a NAC or system change, a new talker, another slot's or phase's
 * call) leaves the report anonymous. The evidence itself is only ever learned from content whose own check passed.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_POSITION_ATTRIBUTION_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_POSITION_ATTRIBUTION_H_

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dsd_event_closing_call;

/** How a producer binds its evidence to a call. */
typedef enum {
    DSD_POSITION_ATTRIBUTION_BIND_EPOCH = 0,        /**< the call epoch it was learned in */
    DSD_POSITION_ATTRIBUTION_BIND_TRANSMISSION = 1, /**< the call's transmission (dsd_call_snapshot_transmission()) */
} dsd_position_attribution_bind;

/** The checked identity a producer learned for one call, bound to the call and the reception it was learned under. */
typedef struct dsd_position_attribution {
    uint64_t call_epoch;   /**< the call's epoch when learned; 0 = no evidence */
    uint64_t transmission; /**< the call's transmission when bound to it (BIND_TRANSMISSION); 0 binds to the epoch */
    uint32_t source;       /**< the source the checked content named, which the call then held; never 0 */
    uint32_t target;       /**< the target it named, which the call then held; 0 = not named */
    uint32_t rx_epoch;     /**< dsd_state::rx_id epoch when learned */
    int32_t protocol;      /**< the call's DSD_SYNC_* value */
    uint8_t group;         /**< 1 when @c target is a talkgroup */
    uint8_t air;           /**< DMR: the burst identity it was learned on (dsd_position_dmr_air()); else 0 */
    uint8_t reserved[6];
} dsd_position_attribution;

/** The call a report would borrow from, as the producer sees it. */
typedef struct dsd_position_attribution_call {
    uint64_t epoch;        /**< 0 = no call */
    uint64_t transmission; /**< dsd_call_snapshot_transmission(); 0 when unknown (a captured closing call) */
    uint64_t source;       /**< dsd_call_snapshot::ota_source_id */
    uint64_t target;       /**< dsd_call_snapshot::ota_target_id; 0 = unknown */
    int32_t protocol;      /**< DSD_SYNC_* */
    uint8_t open;          /**< 1: an ACTIVE call, or the call the report's own terminator has just ended */
    uint8_t reserved[3];
} dsd_position_attribution_call;

/** The protocol class attribution compares: P25 Phase 1, P25 Phase 2, else the protocol family
 * (dsd_sync_protocol_family()), so DMR base and mobile bursts are one class. */
int dsd_position_attribution_class(int protocol);

/** dsd_position_dmr_air(): not a DMR burst. */
#define DSD_POSITION_DMR_AIR_NONE    0U
/** dsd_position_dmr_air(): a DMR burst whose over-the-air slot the decoder has not proven; it binds to nothing. */
#define DSD_POSITION_DMR_AIR_UNKNOWN 0xFFU

/**
 * @brief The identity of the DMR burst @p state is decoding (issue #800, fail closed): who sent it by its own sync word
 * (a repeater's BS-sourced burst, a radio's MS-sourced one, or direct mode) and its over-the-air slot.
 *
 * The two-slot decoder keeps the slots apart by its lanes, so the slot is left out there. The single-slot decoder
 * (dsd_state::dmr_mono_rx_slot) carries both slots on one lane: there the slot is the one proven for the lane, a
 * direct-mode sync word's own, or none for an MS-sourced burst (it has no CACH to prove one, decided limit). A
 * repeater's burst on a lane still Checking its slot is DSD_POSITION_DMR_AIR_UNKNOWN. DSD_POSITION_DMR_AIR_NONE when
 * neither the current nor the last sync is DMR; NULL gives NONE.
 */
uint8_t dsd_position_dmr_air(const dsd_state* state);

/** 1 when two DMR burst identities (dsd_position_dmr_air()) are one sender's on one slot: equal, and neither
 * DSD_POSITION_DMR_AIR_UNKNOWN. */
int dsd_position_dmr_air_same(uint8_t a, uint8_t b);

/** Zero @p a (no evidence). NULL does nothing. Inline, so state helpers that clear it need no core link. */
static inline void
dsd_position_attribution_clear(dsd_position_attribution* a) {
    if (a) {
        DSD_MEMSET(a, 0, sizeof(*a));
    }
}

/** The slot's ACTIVE canonical call into @p out (open 1), else a closed view (open 0). Returns @c out->open. */
int dsd_position_attribution_call_from_slot(const dsd_state* state, uint8_t slot, dsd_position_attribution_call* out);

/** The call @p closing captured (dsd_event_capture_closing_call()), open while it is valid. Returns @c out->open. */
int dsd_position_attribution_call_from_closing(const struct dsd_event_closing_call* closing,
                                               dsd_position_attribution_call* out);

/**
 * @brief Record that content of @p call whose own check passed named @p source (and @p target with @p group).
 *
 * Recorded only while @p call is open and holds that source (a target the call does not hold is left out): @p out then
 * takes the call, its protocol, the binding @p bind, the reception's epoch and, for a DMR call, the identity of the
 * burst that carried the content (dsd_position_dmr_air()), and 1 is returned. Otherwise @p out is left as it was and 0
 * is returned.
 */
int dsd_position_attribution_learn(const dsd_state* state, const dsd_position_attribution_call* call, uint32_t source,
                                   uint32_t target, int group, dsd_position_attribution_bind bind,
                                   dsd_position_attribution* out);

/**
 * @brief The attribution a report of @p report_protocol on @p call takes from the evidence @p a (the file comment's
 * rule).
 *
 * @param[out] granted The source, and the target with its group flag while @p call still holds it; zeroed when nothing
 *             is granted. May be NULL.
 * @return 1 when the source is granted, 0 otherwise.
 */
int dsd_position_attribution_grant(const dsd_state* state, const dsd_position_attribution* a,
                                   const dsd_position_attribution_call* call, int report_protocol,
                                   dsd_position_attribution* granted);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_POSITION_ATTRIBUTION_H_ */
