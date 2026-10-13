// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA calls: the CMCE calls the downlink signals, which one the decoder follows, and the canonical call it
 * publishes.
 *
 * The MCCH signals the calls of every carrier and timeslot of a cell (EN 300 392-2 clause 9.5.1), several at once, so
 * every call is a record keyed by the 14-bit call identifier that each call PDU of clause 14.7.1 carries: a PDU updates
 * the record it names and no other, and a release ends the record it names. The traffic channel a call is given comes
 * in a channel allocation element (clause 21.5.2, table 21.87) of the MAC PDU that carried the call's CMCE PDU, or of
 * a PDU addressed to one of the call's parties; it attaches to that call only.
 *
 * Following (trunking on): an allocation that came with a CMCE PDU of a call whose D-SETUP named its called party is
 * judged by the talkgroup policy and the encryption lockout, and the trunking state machine (tetra_trunk_sm.h) is
 * handed the chosen one. A header-only allocation, or one addressed to a party no call record holds, never tunes.
 *
 * Direct mode (EN 300 396-3) carries one call on its channel, described by the DMAC-SYNC and DMAC-DATA PDUs of the
 * master; it is kept apart from the trunked mode table.
 *
 * Decoder thread only; the state lives in tetra_state_t::calls.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_CALLS_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_CALLS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#include "tetra_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Seconds a call record no PDU or speech has named is kept: longer than the gap between the late-entry D-SETUPs and
 * floor-control PDUs of a call that is still up, so only a call whose release went unheard ages out. The followed call
 * never ages; the trunking state machine's hangtime ends it. */
#define TETRA_CALL_IDLE_S       30.0

/** Seconds without speech after which the heard call (not trunking) counts as over: longer than the frame 18 gap of a
 * speech timeslot, short enough that the next transmission on another timeslot is heard. */
#define TETRA_CALL_SPEECH_END_S 1.0

/**
 * What the MAC knows of a TM-SDU it hands the MLE (EN 300 392-2 clause 21.4.3.1): the address of the PDU that carried
 * it, and the channel allocation that came with it. A call PDU in the TM-SDU takes the allocation (@c alloc_taken);
 * the MAC attaches one no call PDU took by the address alone (tetra_calls_on_allocation()).
 */
typedef struct {
    tetra_mac_address addr; /**< the address of the MAC-RESOURCE (or of the one that started a fragmented TM-SDU) */
    tetra_chan_alloc alloc; /**< valid when @c has_alloc */
    uint8_t has_addr;       /**< @c addr is the PDU's: a MAC-D-BLCK carries only an event label */
    uint8_t has_alloc;      /**< the PDU carried a channel allocation */
    uint8_t alloc_taken;    /**< a call PDU of the TM-SDU took the allocation, or named a call no record holds */
} tetra_mac_sdu;

/** CMCE PDUs that act on a call (EN 300 392-2 clause 14.7.1). */
typedef enum {
    TETRA_CALL_EV_SETUP = 0,
    TETRA_CALL_EV_ALERT,
    TETRA_CALL_EV_PROCEEDING,
    TETRA_CALL_EV_CONNECT,
    TETRA_CALL_EV_CONNECT_ACK,
    TETRA_CALL_EV_INFO,
    TETRA_CALL_EV_TX_GRANTED,
    TETRA_CALL_EV_TX_INTERRUPT,
    TETRA_CALL_EV_TX_CEASED,
    TETRA_CALL_EV_TX_CONTINUE,
    TETRA_CALL_EV_TX_WAIT,
    TETRA_CALL_EV_RESTORE,
    TETRA_CALL_EV_RELEASE,
} tetra_call_event_kind;

/* tetra_call_event::has */
enum {
    TETRA_CALL_HAS_CALLING = 1u << 0,    /* calling_ssi: the calling party of D-SETUP */
    TETRA_CALL_HAS_TALKER = 1u << 1,     /* talker_ssi: the transmitting party of D-TX GRANTED or D-TX INTERRUPT */
    TETRA_CALL_HAS_NEW_ID = 1u << 2,     /* new_call_id: D-INFO or D-CALL RESTORE renames the call */
    TETRA_CALL_HAS_BASIC = 1u << 3,      /* basic_service: the basic service information (table 14.35) */
    TETRA_CALL_HAS_TIMEOUT = 1u << 4,    /* timeout: the call time-out (table 14.50) */
    TETRA_CALL_HAS_GRANT = 1u << 5,      /* grant: the transmission grant (table 14.80) */
    TETRA_CALL_HAS_ENCRYPTION = 1u << 6, /* encryption: the encryption control of D-TX GRANTED or D-TX INTERRUPT */
};

/** One decoded CMCE call PDU, as the call records need it. */
typedef struct {
    uint32_t calling_ssi;
    uint32_t talker_ssi;
    uint16_t call_id;
    uint16_t new_call_id;
    uint8_t kind;          /**< tetra_call_event_kind */
    uint8_t has;           /**< TETRA_CALL_HAS_* */
    uint8_t basic_service; /**< table 14.35 */
    uint8_t priority;      /**< D-SETUP call priority */
    uint8_t timeout;       /**< table 14.50 */
    uint8_t grant;         /**< table 14.80: 0 granted, 1 not granted, 2 queued, 3 granted to another user */
    uint8_t encryption;    /**< encryption control: 1 end-to-end encrypted */
} tetra_call_event;

/**
 * @brief Apply one CRC-passed CMCE call PDU to the call it names, then the allocation that came with it, if any.
 *
 * A D-SETUP makes or refreshes the record of its call; every other PDU updates the record it names and nothing when
 * there is none. A release frees the record (ending its canonical call and, when it is the followed call, telling the
 * trunking state machine). An allocation in the same MAC PDU attaches to the named call and, while trunking, may be
 * followed. @p sdu may be NULL (a TM-SDU decoded without its MAC PDU); @p opts may be NULL.
 */
void tetra_calls_on_event(dsd_opts* opts, dsd_state* state, const tetra_call_event* ev, tetra_mac_sdu* sdu);

/**
 * @brief Attach a channel allocation no call PDU took to the call whose party the PDU addressed (by SSI or by an event
 * label assigned to it). It updates that call's traffic channel, and a "go to the MCCH" allocation (no timeslots) to
 * the followed call ends it; it never tunes.
 */
void tetra_calls_on_allocation(dsd_opts* opts, dsd_state* state, tetra_mac_sdu* sdu);

/**
 * @brief An addressed MAC-RESOURCE (EN 300 392-2 table 21.55) was decoded on timeslot @p tn (0 when the TDMA clock does
 * not place it) with encryption mode @p enc_mode: an event label (address types 101, 111) or usage marker (110)
 * assignment to a party of a call binds it to that call, an encrypted allocation to a call's party sets the call's air
 * encryption, a PDU to the followed call's party proves that call on the carrier (tetra_confirm), and the timeslot's
 * signalling encryption is noted for speech no call record names.
 */
void tetra_calls_on_addressed(dsd_state* state, const tetra_mac_address* addr, uint8_t enc_mode, int alloc_present,
                              uint8_t tn);

/** @brief The record of call @p call_id, or NULL. */
tetra_call_t* tetra_calls_find(dsd_state* state, uint16_t call_id);

/** @brief The record of call @p call_id, or NULL; never creates TETRA state. */
const tetra_call_t* tetra_calls_peek(const dsd_state* state, uint16_t call_id);

/** @brief The record of the call the trunking state machine follows, or NULL when it follows none. */
const tetra_call_t* tetra_calls_followed(const dsd_state* state);

/**
 * @brief The trunking state machine stopped following a call (tetra_trunk_sm.c): its talkgroup-policy route is cleared.
 * Never creates TETRA state.
 */
void tetra_calls_unfollowed(dsd_state* state);

/** @brief Drop the records no PDU or speech named for TETRA_CALL_IDLE_S (the followed call excepted). */
void tetra_calls_tick(dsd_opts* opts, dsd_state* state);

/** @brief The carrier was left: forget what was learnt about its timeslots. Never creates TETRA state. */
void tetra_calls_forget_carrier(dsd_state* state);

/**
 * @brief The traffic channel the frontends show: the followed call's while trunking follows one, else the most recently
 * allocated call's. @return 1 and @p out when there is one.
 */
int tetra_calls_traffic(const dsd_state* state, tetra_chan_alloc* out);

/**
 * @brief Decode one downlink TM-SDU whose MAC context is @p sdu (tetra_mle_dispatch() without one). The MAC's entry
 * into tetra_mle.c.
 */
void tetra_mle_dispatch_sdu(const uint8_t* bits, int nbits, int cc, tetra_mac_sdu* sdu, dsd_opts* opts,
                            dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_CALLS_H_ */
