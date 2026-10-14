// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA calls: the CMCE calls the downlink signals, which one the decoder follows, and the canonical call it
 * publishes.
 *
 * The MCCH (timeslot 1 of the main carrier, EN 300 392-2 clause 9.4.2.1) signals the calls of every carrier and timeslot
 * of a cell, several at once, so every call is a record keyed by the 14-bit call identifier that each call PDU of
 * clause 14.7.1 carries: a PDU updates the record it names and no other, and a release ends the record it names. The
 * traffic channel a call is given comes in a channel allocation element (clause 21.5.2, table 21.87) of the MAC PDU
 * that carried the call's CMCE PDU, or of a PDU addressed to one of the call's parties; it attaches to that call only,
 * and takes the channel from any other: the downlink of a traffic channel carries one call at a time (clauses 23.5.5,
 * 23.5.6.1.1), so the call that held it holds no channel from there; a trunking state machine following that channel
 * moves to the new call when the policy follows that one, and lets go of the channel when it does not.
 *
 * Following (trunking on): an allocation that came with a CMCE PDU of a call whose D-SETUP named its called party is
 * judged by the talkgroup policy and the encryption lockout, and the trunking state machine (tetra_trunk_sm.h) is
 * handed the chosen one. A header-only allocation, or one addressed to a party no call record holds, never tunes.
 *
 * Canonical calls: the followed call publishes on canonical call slot 0 (core/call_state.h) from its CRC-passed
 * signalling once a block before it proved the carrier (tetra_confirm), with its called party as the target, its
 * transmitting party as the source, its traffic channel, its basic service as the service options and its crypto
 * class. D-TX CEASED and a release end it as over-the-air terminators; the state machine leaving it ends it as a
 * teardown. Not trunking, the call whose speech is heard publishes the same way, from its speech and then its
 * signalling, and ends TETRA_CALL_SPEECH_END_S after its speech stops; speech no call record names publishes as an
 * anonymous call on a proven carrier. Decoded speech marks the call's media. Decoded short data, status messages and
 * MM enable/disable become data and control notices.
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
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
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

/** The bit of timeslot @p tn (1..4) in a channel allocation's timeslot bitmap, which is sent TN1 first (EN 300 392-2
 * table 21.87); 0 for any other @p tn. */
static inline uint8_t
tetra_slot_bit(uint8_t tn) {
    return (tn >= 1u && tn <= 4u) ? (uint8_t)(1u << (4u - tn)) : 0u;
}

/**
 * What the MAC knows of a TM-SDU it hands the MLE (EN 300 392-2 clause 21.4.3.1): the address of the PDU that carried
 * it, and the channel allocation that came with it. A call PDU in the TM-SDU takes the allocation (@c alloc_taken);
 * the MAC attaches one no call PDU took by the address alone (tetra_calls_on_allocation()).
 */
typedef struct {
    tetra_mac_address addr; /**< the address of the MAC-RESOURCE (or of the one that started a fragmented TM-SDU) */
    tetra_chan_alloc alloc; /**< valid when @c has_alloc and not @c alloc_unread */
    uint8_t has_addr;       /**< @c addr is the PDU's: a MAC-D-BLCK carries only an event label */
    uint8_t has_alloc;      /**< the PDU carried a channel allocation */
    uint8_t alloc_unread;   /**< ...which is ciphertext (EN 300 392-7 clause 6.7.1.2): its channel is not known */
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
    TETRA_CALL_HAS_CALLING = 1u << 0,     /* calling_ssi: the calling party of D-SETUP */
    TETRA_CALL_HAS_TALKER = 1u << 1,      /* talker_ssi: the transmitting party of D-TX GRANTED or D-TX INTERRUPT */
    TETRA_CALL_HAS_NEW_ID = 1u << 2,      /* new_call_id: D-INFO or D-CALL RESTORE renames the call */
    TETRA_CALL_HAS_BASIC = 1u << 3,       /* basic_service: the basic service information (table 14.35) */
    TETRA_CALL_HAS_TIMEOUT = 1u << 4,     /* timeout: the call time-out (table 14.50) */
    TETRA_CALL_HAS_GRANT = 1u << 5,       /* grant: the transmission grant (table 14.80) */
    TETRA_CALL_HAS_ENCRYPTION = 1u << 6,  /* encryption: the encryption control of D-TX GRANTED or D-TX INTERRUPT */
    TETRA_CALL_HAS_CALLING_EXT = 1u << 7, /* calling_ext: the extension of a calling party of type 2 (table 14.43) */
};

/** One decoded CMCE call PDU, as the call records need it. */
typedef struct {
    uint32_t calling_ssi;
    uint32_t calling_ext; /**< the calling party's network, MCC << 14 | MNC (table 14.44) */
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
 * trunking state machine). An allocation in the same MAC PDU attaches to the named call, takes its downlink from any
 * other call that held it and, while trunking, may be followed. Nothing here settles the trunking state machine: the
 * burst's entry did (tetra_sm_settle() from tetra.c), so every PDU of a burst sees the same follow. @p sdu may be
 * NULL (a TM-SDU decoded without its MAC PDU); @p opts may be NULL.
 */
void tetra_calls_on_event(dsd_opts* opts, dsd_state* state, const tetra_call_event* ev, tetra_mac_sdu* sdu);

/**
 * @brief The MAC delivered @p sdu's PDU (a fragmented message at its MAC-END) and its call PDUs acted; an encrypted PDU
 * is delivered too, as far as its clear address goes (@c alloc_unread when it carried an allocation). A channel
 * allocation no call PDU took attaches to the call whose party the PDU addressed (by SSI, or by an event label assigned
 * to it on this channel): it updates that call's traffic channel, taking it from any other call that held it, and a
 * "go to the MCCH" allocation (no timeslots) to the followed call ends it; it never tunes. An event label or usage
 * marker the address assigns takes effect here and only here (EN 300 392-2 clause 23.4.1.2.3.1; the event label
 * lifecycle in tetra_calls.c).
 */
void tetra_calls_on_allocation(dsd_opts* opts, dsd_state* state, tetra_mac_sdu* sdu);

/**
 * @brief An addressed MAC-RESOURCE (EN 300 392-2 table 21.55) was decoded on timeslot @p tn (0 when the TDMA clock does
 * not place it) with encryption mode @p enc_mode, a fragmented TM-SDU's first fragment included (an event label it
 * assigns takes effect at delivery, tetra_calls_on_allocation()): an encrypted allocation to
 * a call's party sets the call's air encryption, which the call's canonical call takes at once when it is published and
 * which, while encrypted calls are not followed, locks out and leaves a followed call it makes undecryptable; a PDU to
 * the followed call's party proves that call on the carrier (tetra_confirm), and the timeslot's signalling encryption
 * is noted for speech no call record names. @p opts may be NULL.
 */
void tetra_calls_on_addressed(dsd_opts* opts, dsd_state* state, const tetra_mac_address* addr, uint8_t enc_mode,
                              int alloc_present, uint8_t tn);

/**
 * @brief The number of the carrier being received, for TB5 (EN 300 392-7 clause 6.3.2.2) and the event labels bound to
 * an allocated channel: the followed call's carrier on its traffic channel, else the carrier whose downlink frequency
 * (EN 300 392-2 clause 21.4.4.1, with the SYSINFO band and offset) is exactly the frequency the receiver is on
 * (tetra_sm_receiver_freq(): the tuned one, on the control channel too, which may be a secondary carrier broadcasting
 * the main carrier's SYSINFO). @return 1 and @p carrier when it is known; 0 in direct mode, while a retune is in flight
 * and when no carrier matches.
 */
int tetra_calls_carrier(const dsd_opts* opts, const dsd_state* state, uint16_t* carrier);

/** @brief The record of call @p call_id, or NULL. */
tetra_call_t* tetra_calls_find(dsd_state* state, uint16_t call_id);

/** @brief The record of call @p call_id, or NULL; never creates TETRA state. */
const tetra_call_t* tetra_calls_peek(const dsd_state* state, uint16_t call_id);

/**
 * @brief The target canonical slot 0 names for @p c, and the one its speech is judged under: a group call's called
 * group, and for an individual call the party that is not transmitting (EN 300 392-2 clause 14.5.2: an individual call
 * has two parties), the calling one while its called party talks. With the transmitting party (else the calling party)
 * as source, the talkgroup policy's private-call audio and record gates judge both parties whoever transmits, as DMR
 * and P25 name a unit-to-unit transmission. The called party while the calling one is not known, and for a call whose
 * D-SETUP went unheard; 0 for NULL.
 */
uint32_t tetra_calls_target(const tetra_call_t* c);

/** @brief The record of the call the trunking state machine follows, or NULL when it follows none. */
const tetra_call_t* tetra_calls_followed(const dsd_state* state);

/**
 * @brief The receiver reached the followed channel (tetra_trunk_sm.c): when the followed call is the canonical call,
 * which a reallocation of it moved to another carrier, the canonical call takes the channel it is followed on now;
 * when another call is (the one followed before, which a grant of another call took the receiver from), it ends as a
 * teardown, as the state machine leaving a call ends it, and the followed call publishes once it proves itself there.
 * Never creates TETRA state.
 */
void tetra_calls_followed_moved(dsd_opts* opts, dsd_state* state);

/**
 * @brief The trunking state machine stopped following a call (tetra_trunk_sm.c): its canonical call ends as a
 * teardown and its talkgroup-policy route is cleared. @p opts may be NULL when the engine ended and committed the
 * canonical calls itself. Never creates TETRA state.
 */
void tetra_calls_unfollowed(dsd_opts* opts, dsd_state* state);

/** @brief The SSI the MAC PDU of @p sdu names in the clear (tetra_mac_address), 0 for none or a NULL @p sdu. */
uint32_t tetra_calls_addressed_ssi(const tetra_mac_sdu* sdu);

/**
 * @brief Commit a notice of TETRA traffic to the event history: a data notice (SDS) or, with @p control, a control
 * notice (status, MM), from @p src to @p dst, with @p text (may be NULL) as its message. Only once a CRC-passed block
 * before the one that carried it proved the carrier (tetra_confirm_carrier_proven()).
 */
void tetra_calls_notice(dsd_opts* opts, dsd_state* state, int control, uint32_t src, uint32_t dst, const char* notice,
                        const char* text);

/** @brief Drop the records no PDU or speech named for TETRA_CALL_IDLE_S (the followed call excepted), and end the heard
 * call TETRA_CALL_SPEECH_END_S after its speech stopped and the direct mode call TETRA_CALL_DMO_IDLE_S after it was
 * last seen. */
void tetra_calls_tick(dsd_opts* opts, dsd_state* state);

/** @brief The carrier was left: forget what was learnt about its timeslots, which one was heard and the direct mode call
 * on its channel. Never creates TETRA state. */
void tetra_calls_forget_carrier(dsd_state* state);

/**
 * @brief The trunked carrier was left without a carrier boundary the engine saw: a BSCH named another cell, or a DSB
 * put the carrier in direct mode. What its timeslots' speech left is forgotten as at a carrier boundary, their
 * signalling encryption and the heard timeslot, and since the engine ended no canonical call, the heard call or the
 * anonymous speech canonical slot 0 carries ends here; the followed call is the state machine's. @p opts may be NULL.
 * Never creates TETRA state.
 */
void tetra_calls_forget_trunked_carrier(dsd_opts* opts, dsd_state* state);

/**
 * @brief A BSCH or SYSINFO named another cell of the network (tetra_bsch.h): the records stay, as the network's calls
 * go on across its cells, but what each holds of the last cell goes. A channel allocation names a carrier number and
 * timeslots of the cell that made it (EN 300 392-2 clause 21.5.2), which another cell may give any call, and an event
 * label or usage marker is valid on one channel of one cell (clauses 23.4.1.2.3, 23.5.5). So no speech on a channel the
 * last cell allocated is a record's (tetra_calls_on_timeslot()), no PDU addressed by a label the last cell assigned
 * attaches to one, and no record shows a channel of the last cell as its traffic channel, until the new cell allocates
 * one. The followed call's channel is the state machine's grant, which this leaves to it. Never creates TETRA state.
 */
void tetra_calls_forget_cell(dsd_state* state);

/**
 * @brief The identifiers the records are keyed by name nothing here any more: a BSCH named another network, whose SwMI
 * numbers its calls and parties itself (EN 300 392-2 clause 14.2.3: a downlink SSI is of the current network), or a
 * SYSINFO another location area, whose call identifiers may differ from this one's (clause 14.2.3). Every record ends
 * with its canonical call, and the state machine follows no identifier of them from here (tetra_sm_retire_call(), the
 * rollback of a tune in flight included); the heard call, anonymous speech and the timeslots' signalling encryption go
 * too, and the speech in progress starts over (tetra_calls_t::ends). @p opts may be NULL. Never creates TETRA state.
 */
void tetra_calls_forget_calls(dsd_opts* opts, dsd_state* state);

/**
 * @brief A SYSINFO named location area @p la (D-MLE-SYSINFO, EN 300 392-2 clause 18.4.2.2): the records are of that
 * area from here, and those made in another end first (tetra_calls_forget_calls()). @p opts may be NULL.
 */
void tetra_calls_note_location_area(dsd_opts* opts, dsd_state* state, uint16_t la);

/** @brief The concatenated SDS messages being reassembled (tetra_mle.c) go: their senders' SSIs name parties of a
 * network the receiver left. Never creates TETRA state. */
void tetra_mle_forget_reassembly(dsd_state* state);

/**
 * @brief The traffic channel the frontends show: the followed call's while trunking follows one, else the most recently
 * allocated call's. @return 1 and @p out when there is one.
 */
int tetra_calls_traffic(const dsd_state* state, tetra_chan_alloc* out);

/** Seconds without a PDU or speech of the direct mode call after which it counts as over, for a carrier that stays up
 * after the call (a DM-REP's presence signal) when its DM-TX CEASED went unheard: about three multiframes (1,02 s
 * each) of a transmission's speech and signalling. */
#define TETRA_CALL_DMO_IDLE_S 3.0

/**
 * @brief A CRC-passed DMAC-SYNC (@p sync_enc_state: its SCH/S air interface encryption state) or DMAC-DATA
 * (@p sync_enc_state -1) of the direct mode channel, read and settled to its security class (tetra_dmo.h).
 *
 * DM-SETUP, DM-SETUP PRES, DM-CONNECT ACK, DM-OCCUPIED and DM-GSETUP describe the call on the channel, which publishes
 * on canonical slot 0 with what the security class leaves in the clear (DM-2-B hides the destination); DM-TX CEASED,
 * DM-RELEASE, DM-DISCONNECT and DM-GRELEASE end it as terminators. A DM-2-C burst, whose addresses and message type
 * are encrypted, publishes an encrypted call without identities. DM-SDS DATA and DM-SDS UDATA become data notices, a
 * repeat of the last within a few seconds not again. @p opts may be NULL.
 */
void tetra_calls_dmo_pdu(dsd_opts* opts, dsd_state* state, int sync_enc_state, const tetra_dmo_sync_h_t* h);

/** @brief The protection of the direct mode channel's traffic changed (tetra_dmo_call_crypt()) without a call PDU that
 * publishes it: the direct mode call canonical slot 0 carries takes the new crypto class at once. @p opts may be NULL.
 * Never creates TETRA state. */
void tetra_calls_dmo_protection(dsd_opts* opts, dsd_state* state);

/** @brief Direct mode ended (tetra_dmo_leave()): the direct mode call, its canonical call and its short data memory go.
 * @p opts may be NULL. Never creates TETRA state. */
void tetra_calls_dmo_forget(dsd_opts* opts, dsd_state* state);

/** Which call a speech burst belongs to (tetra_speech::kind). */
enum {
    TETRA_SPEECH_ANON = 0, /**< trunked mode speech no call record names */
    TETRA_SPEECH_CALL,     /**< the CMCE call tetra_speech::call_id */
    TETRA_SPEECH_DMO,      /**< the direct mode call on the channel */
};

/** A speech burst the voice gates let through (tetra_voice_gate.h), and the call it belongs to. */
typedef struct {
    uint32_t target;  /**< the address the talkgroup policy judges it by: the call's called party, 0 when unknown */
    uint32_t source;  /**< the transmitting party: the call's talker, the direct mode source; 0 when unknown */
    uint16_t call_id; /**< the call, for TETRA_SPEECH_CALL */
    uint8_t kind;     /**< TETRA_SPEECH_* */
    uint8_t tn;       /**< its timeslot, 0 when the TDMA clock does not place it */
    uint8_t followed; /**< speech of the call the trunking state machine follows */
    uint8_t air_enc;  /**< encryption mode (TETRA_ENC_MODE_*) of the call's air interface, trunked mode */
    uint8_t audible;  /**< the burst plays: its encryption class and talkgroup policy let it (tetra_voice_audible()) */
    uint8_t
        sealed; /**< encrypted where no key stream here applies: end-to-end, or a direct mode call not known clear */
} tetra_speech;

/**
 * @brief The call record whose traffic channel is timeslot @p tn of the frequency the receiver is on
 * (tetra_sm_receiver_freq(): the channel the state machine holds, else the tuned frequency), the most recently allocated
 * of several; NULL when there is no timeslot, no such frequency known or no such record.
 */
const tetra_call_t* tetra_calls_on_timeslot(const dsd_opts* opts, const dsd_state* state, uint8_t tn);

/**
 * @brief Whether the talkgroup policy mutes @p sp's speech, judged on the identities the canonical call of its speech
 * carries (tetra_calls_on_speech() publishes it): a call record's kind, target and source, the direct mode call's, else a
 * call without identities. Before the speech is published, so that only speech that plays holds the receiver on its
 * timeslot (tetra_voice_hold()). @p opts may be NULL (nothing is muted).
 */
int tetra_calls_speech_muted(const dsd_opts* opts, const dsd_state* state, const tetra_speech* sp);

/**
 * @brief Speech of @p sp's call whose CRC passed, before it plays.
 *
 * Not trunking, it makes its timeslot the heard one, and when it plays (tetra_speech::audible) the one that holds the
 * receiver (tetra_voice_hold()): speech that does not play holds nothing. Its call is seen (it does not age) and publishes on canonical slot
 * 0 unless it already does: the followed call or a call record (not after its D-TX CEASED), the direct mode call while
 * a DM-SETUP or DM-OCCUPIED set one up, else an anonymous call of its timeslot once the carrier is proven (another
 * timeslot's anonymous speech is another call). The published call's media
 * turns active. A followed call that turned out undecryptable while encrypted calls are not followed is locked out and
 * left. Speech of the followed call keeps the trunking state machine on it (tetra_sm_note_voice()); other speech marks
 * reception. @p opts may be NULL.
 */
void tetra_calls_on_speech(dsd_opts* opts, dsd_state* state, const tetra_speech* sp);

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
