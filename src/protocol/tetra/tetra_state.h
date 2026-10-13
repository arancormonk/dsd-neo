// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA decoder state, held in the TETRA state extension slot (DSD_STATE_EXT_PROTO_TETRA).
 *
 * Decoder-thread private. Frontends never read it; they get TETRA data through published views.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_STATE_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_STATE_H_

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <stdint.h>
#include <time.h>

#include "tetra_voice_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shared tuning state (dsd_opts, dsd_state) a trunking tune changes, kept to put back when the tune fails. */
typedef struct {
    long p25_vc_freq[2];
    long trunk_vc_freq[2];
    double last_vc_sync_time_m;
    double last_cc_sync_time_m;
    double p25_last_vc_tune_time_m;
    time_t last_vc_sync_time;
    time_t last_cc_sync_time;
    time_t p25_last_vc_tune_time;
    int trunk_is_tuned;
} tetra_sm_tune_snapshot;

/* Everything a tune puts back when it fails: the shared tuning state and the state machine's own. */
typedef struct {
    tetra_sm_tune_snapshot tuning;
    tetra_sm_grant followed;
    double t_tune_m;
    double t_voice_m;
    uint8_t state;
} tetra_sm_rollback;

/* The trunking state machine (tetra_trunk_sm.c). Times are decode-clock monotonic seconds. */
typedef struct {
    tetra_sm_rollback pending_prev; /* what the tune in flight puts back if it fails */
    tetra_sm_grant followed;        /* the call followed while TUNED */
    double t_tune_m;  /* the followed tune completed, or the grant was taken on the same carrier; 0 in flight */
    double t_voice_m; /* last speech on a followed timeslot since then; 0 for none */
    double pending_deadline_m;   /* the tune in flight counts as failed past this */
    uint64_t pending_request_id; /* the correlated trunk-tuning request in flight; 0 for none */
    uint8_t state;               /* tetra_sm_state_e */
    uint8_t pending_kind;        /* what the request in flight does: 0 none, 1 grant, 2 return to the control channel */
} tetra_sm_t;

/* CRC evidence about the carrier on air and the burst being decoded (tetra_confirm.c). A training sequence alone is
 * weak enough for noise to match; a block whose CRC-16 passed (EN 300 392-2 clause 8.2.3.3) is not. Cleared at every
 * carrier boundary, no-carrier pass and trunking retune (tetra_confirm_reset()). */
typedef struct {
    uint32_t signalling_blocks; /* CRC-passed BSCH and MAC blocks decoded on this carrier */
    uint8_t followed_call;      /* CRC-passed signalling of the followed call was decoded on this carrier */
    uint8_t burst_open;         /* a burst is being decoded (tetra_confirm_burst_begin() to _end()) */
    uint8_t burst_stale;        /* the carrier was reset while the burst was open: its blocks are the last carrier's */
    uint8_t burst_proven;       /* the open (or last) burst decoded a block whose CRC passed */
} tetra_confirm_t;

/** A channel allocation element (EN 300 392-2 table 21.87) as the MAC read it from a clear PDU. */
typedef struct {
    long freq_hz;       /**< downlink frequency of the carrier, 0 when its band is unknown */
    uint16_t carrier;   /**< carrier number */
    uint8_t type;       /**< allocation type */
    uint8_t timeslots;  /**< timeslot assigned: bit 3 is timeslot 1 ... bit 0 timeslot 4; 0 is "go to the MCCH" */
    uint8_t up_down;    /**< up/downlink assigned; for an augmented allocation, the augmented value */
    uint8_t bandwidth;  /**< augmented allocations only; 0 is 25 kHz */
    uint8_t modulation; /**< augmented allocations only; 0 is pi/4-DQPSK */
} tetra_chan_alloc;

/* Phase of a CMCE call (EN 300 392-2 clause 14.5): set up (D-SETUP, D-ALERT, D-CALL PROCEEDING), through-connected
 * (D-CONNECT, D-CONNECT ACKNOWLEDGE), a transmission granted (D-TX GRANTED), the transmission ceased (D-TX CEASED). */
enum {
    TETRA_CALL_FREE = 0,
    TETRA_CALL_SETUP,
    TETRA_CALL_CONNECTED,
    TETRA_CALL_TX_GRANTED,
    TETRA_CALL_CEASED,
};

/* tetra_call_t::flags */
enum {
    TETRA_CALL_F_SETUP_SEEN = 1u << 0,   /* a D-SETUP named the called party and the basic service */
    TETRA_CALL_F_EVENT_LABEL = 1u << 1,  /* event_label is assigned to a party of the call */
    TETRA_CALL_F_USAGE_MARKER = 1u << 2, /* usage_marker is assigned to the call */
};

/** One CMCE call the downlink signalled, keyed by its 14-bit call identifier (tetra_calls.c). */
typedef struct {
    double seen_m;          /**< decode clock: the last CRC-passed PDU that named the call, or its speech */
    double alloc_m;         /**< decode clock: when its channel allocation was last set; 0 for none */
    tetra_chan_alloc alloc; /**< the traffic channel it was last allocated; no timeslots for none */
    uint32_t called_ssi;    /**< called party: the GSSI of a group call, the ISSI of an individual one; 0 unknown */
    uint32_t calling_ssi;   /**< calling party; 0 unknown */
    uint32_t talker_ssi;    /**< transmitting party of the transmission in progress; 0 none or unknown */
    uint16_t call_id;       /**< call identifier */
    uint16_t event_label;   /**< event label assigned to a party of the call (TETRA_CALL_F_EVENT_LABEL) */
    uint8_t phase;          /**< TETRA_CALL_*; TETRA_CALL_FREE for an unused entry */
    uint8_t flags;          /**< TETRA_CALL_F_* */
    uint8_t basic_service;  /**< basic service information (table 14.35) as last signalled */
    uint8_t e2ee;           /**< end-to-end encrypted: the basic service or D-TX GRANTED encryption control says so */
    uint8_t air_enc;        /**< encryption mode (TETRA_ENC_MODE_*) of the MAC-RESOURCE that allocated its channel */
    uint8_t priority;       /**< call priority from D-SETUP */
    uint8_t timeout;        /**< call time-out (table 14.50) */
    uint8_t usage_marker;   /**< usage marker assigned to the call (TETRA_CALL_F_USAGE_MARKER) */
} tetra_call_t;

/* The CMCE calls tracked at once. The MCCH of a carrier with four timeslots signals the calls of every carrier of the
 * cell, so eight leave room for calls that are set up while others are on air. */
#define TETRA_CALLS_MAX 8

/* What canonical call slot 0 carries for TETRA (tetra_calls_t::published). */
enum {
    TETRA_PUB_NONE = 0, /* nothing */
    TETRA_PUB_CALL,     /* the CMCE call published_call_id */
    TETRA_PUB_ANON,     /* trunked-mode speech no call record names */
    TETRA_PUB_DMO,      /* the direct mode call */
};

/** The direct mode call on the channel, from its DMAC-SYNC and DMAC-DATA PDUs (EN 300 396-3 clause 9). */
typedef struct {
    double seen_m;        /**< decode clock: the last PDU or speech of the call */
    double sds_m;         /**< decode clock: when the last short data notice went out */
    uint64_t sds_value;   /**< that notice's user data or status, with sds_src, sds_dst and sds_type */
    uint32_t src;         /**< source address (talker) */
    uint32_t dst;         /**< destination address; 0 when DM-2-B hides it */
    uint32_t sds_src;     /**< source of the last short data notice */
    uint32_t sds_dst;     /**< destination of the last short data notice */
    uint8_t active;       /**< a DM-SETUP, DM-CONNECT ACK or DM-OCCUPIED set it up and no end has come since */
    uint8_t addressed;    /**< the addresses were clear (no DM-2-C) */
    uint8_t group;        /**< call type element: 1 group call, 0 individual call (group_known) */
    uint8_t group_known;  /**< the DM-SDU with the call type was clear */
    uint8_t circuit_mode; /**< circuit mode type (0 speech); circuit_known */
    uint8_t circuit_known;
    uint8_t sds_type; /**< message type of the last short data notice; 0 for none */
} tetra_dmo_call_t;

/** Calls (tetra_calls.c): the trunked mode call table, the direct mode call and what the decoder publishes. */
typedef struct {
    tetra_call_t call[TETRA_CALLS_MAX];
    tetra_dmo_call_t dmo;
    double heard_m;             /* decode clock: the last good speech of the heard timeslot (not trunking) */
    uint16_t published_call_id; /* the call TETRA_PUB_CALL names */
    uint16_t heard_call_id;     /* the call the heard speech belongs to; heard_known */
    uint8_t published;          /* TETRA_PUB_* */
    uint8_t heard;              /* speech is being heard on heard_tn (not trunking): other timeslots are not */
    uint8_t heard_tn;           /* its timeslot, 0 while the TDMA clock does not place it */
    uint8_t heard_known;        /* heard_call_id names its call */
    /* 1 + the encryption mode of the last addressed MAC-RESOURCE decoded on this carrier since it was taken, 0 for
     * none: per timeslot at index TN, and at index 0 on any timeslot (or while the TDMA clock is stopped). */
    uint8_t slot_enc[5];
} tetra_calls_t;

/** Address element of a downlink MAC-RESOURCE PDU (EN 300 392-2 table 21.55). */
typedef struct {
    uint8_t type;         /* address type, TETRA_MAC_ADDR_* */
    uint8_t esi;          /* 1 when the SSI part is an encrypted short identity (EN 300 392-7 clause 4.2.6) */
    uint8_t usage_marker; /* 6 bits, address types SSI + usage marker */
    uint16_t event_label; /* 10 bits, address types with an event label */
    uint32_t ssi;         /* SSI, USSI or SMI */
} tetra_mac_address;

/* N.202, the largest TM-SDU (EN 300 392-2 annex B.2). */
#define TETRA_MAC_TM_SDU_MAX_BITS 2632

/** Reconstruction of one fragmented downlink TM-SDU (EN 300 392-2 clause 23.4.3.1.1). */
typedef struct {
    uint8_t active;          /* a MAC-RESOURCE with length indication 111111 started a TM-SDU */
    uint8_t encrypted;       /* its encryption mode was not 00, so every fragment is ciphertext */
    uint8_t idle_half_slots; /* half slots of this timeslot since the last fragment, for the N.203 limit */
    int8_t cc;               /* colour code of the block that started the TM-SDU */
    tetra_mac_address addr;  /* address of the starting MAC-RESOURCE; MAC-FRAG and MAC-END carry none */
    uint16_t nbits;          /* TM-SDU bits rebuilt so far */
    uint8_t bits[TETRA_MAC_TM_SDU_MAX_BITS]; /* one bit per byte */
} tetra_mac_fragment;

typedef struct tetra_state {
    /* The speech path: codec library, speech integrity and PCM carry (tetra_voice.c). */
    tetra_voice_t voice;

    /* Call following (tetra_trunk_sm.c) and the CRC evidence it and the speech path rely on (tetra_confirm.c). */
    tetra_sm_t sm;
    tetra_confirm_t confirm;

    /* The calls the decoder tracks, follows and publishes (tetra_calls.c). */
    tetra_calls_t calls;

    /* Fragmented TM-SDU reconstruction (tetra_mac.c), one context per downlink timeslot (index = TN 1..4; index 0 while
     * the TDMA timeslot is unknown). A fragment run belongs to the control channel of its timeslot, so fragments of
     * different timeslots are never joined (EN 300 392-2 clause 23.4.3.1.1). */
    tetra_mac_fragment mac_frag[5];

    /* Concatenated SDS reassembly contexts (EN 300 392-2 clause 29.5.14), allocated on first use and freed with the
     * TETRA state (tetra_mle.c). */
    struct tetra_sds_concat_store* sds_concat;

    /* The cell's SYSINFO (EN 300 392-2 table 21.65, tetra_mac.c): the downlink frequency in Hz of its main carrier (0
     * while unknown) and that carrier's number; the frequency band and offset of the last SYSINFO with a valid main
     * carrier, which a channel allocation without extended carrier numbering uses (clause 21.5.2; freq_band 0 while
     * none is known); and the location area of its D-MLE-SYSINFO (clause 18.4.2.2), sysinfo_known once one was
     * decoded. frames_sysinfo counts SYSINFO PDUs: the publication shows the main carrier once there was one. */
    long dl_carrier_hz;
    uint32_t frames_sysinfo;
    uint16_t sysinfo_main_carrier;
    uint16_t la;
    uint8_t freq_band;
    uint8_t freq_offset;
    uint8_t sysinfo_known;

    /* Network identity from the BSCH (EN 300 392-2 table 21.76) or --tetra-net: net_known once MCC, MNC and the 6-bit
     * colour code are set, and lfsr_seed, the scrambling seed they give (tetra_compute_scramb_seed()). */
    uint32_t lfsr_seed;
    uint16_t mcc;
    uint16_t mnc;
    uint8_t net_known;
    uint8_t colour;

    /* The encryption mode (TETRA_ENC_MODE_*) of the last addressed MAC-RESOURCE, or of the MAC-RESOURCE that started
     * the TM-SDU a MAC-END completed; Null PDUs leave it alone. It describes the cell's signalling, for display: the
     * calls keep the encryption mode of their own allocation (tetra_call_t::air_enc). frames_resource counts
     * MAC-RESOURCE PDUs other than Null PDUs, which tells a clear cell from one nothing was decoded on yet. */
    uint32_t frames_resource;
    uint8_t enc_mode;

    /* Bursts and blocks whose CRC passed; TETRA reads as decoded once one did (tetra_publish()). */
    uint32_t decode_ok;

    /* TDMA clock (EN 300 392-2 clause 7.3), set by a BSCH and run by tetra_tdma_follow(): while tdma_valid, the slot
     * that started at symbol count tdma_slot_start is timeslot tn 1..4 of frame fn 1..18 of multiframe mn 1..60. The
     * hyperframe number is the SYSINFO value from a block whose CCK flag is clear, counted on by the clock at each
     * MN 60 to MN 1; hyperframe_valid drops with the clock and on a change of cell. */
    uint32_t tdma_slot_start;
    uint16_t hyperframe;
    uint8_t hyperframe_valid;
    uint8_t tdma_valid;
    uint8_t tn;
    uint8_t fn;
    uint8_t mn;

    /* The KSG number (0..3, EN 300 392-7 table A.73) a D-OTAR SCK or GCK Provide named for a network, for up to four
     * networks keyed by MCC/MNC (tetra_air.c). The operator air keys live in the key set (dsd_state::tetra_air_keys). */
    uint16_t air_ksg_mcc[4];
    uint16_t air_ksg_mnc[4];
    uint8_t air_ksg_valid[4];
    uint8_t air_ksg[4];

    /* Direct mode (EN 300 396-2, EN 300 396-3), set by tetra_dmo_apply_sync().
     * dmo:            1 once a DSB with a DMO system code was decoded: normal bursts have no AACH and use the DM colour
     *                 code. Cleared by a TMO BSCH.
     * dmo_dcc_valid:  1 while dmo_dcc_seed holds the DM colour code of the current transmission (clause 8.2.4).
     * dmo_comm_type:  communication type of the last DSB: 0 direct, 1 DM-REP, 2 DM-GATE, 3 DM-REP/GATE.
     * dmo_crypt:      tetra_dmo_crypt_e of the current call.
     * dmo_since_sync: DNBs processed since the last DMAC-SYNC (colour code staleness). */
    uint32_t dmo_dcc_seed;
    uint16_t dmo_since_sync;
    uint8_t dmo;
    uint8_t dmo_dcc_valid;
    uint8_t dmo_comm_type;
    uint8_t dmo_crypt;

    /* The status downlink of the last complete D-MM STATUS (EN 300 392-2 table 16.3), for the frontends; mm_status_valid
     * once one was decoded (tetra_mm.c). */
    uint8_t mm_status_code;
    uint8_t mm_status_valid;

    /* Diagnostic gates mirrored from the options at every burst (tetra_frame_enter()), so parsers that are not handed
     * the options can still keep per-burst detail behind -Z (payload) and -v (verbose). */
    uint8_t log_payload;
    uint8_t log_verbose;

    /* 1 with --tetra-uplink: normal bursts are then NUBs (EN 300 392-2 table 9.5), the training sequence of a downlink
     * NDB with block 1 directly against it (no AACH), scrambled with the TMO colour code from a BSCH or --tetra-net.
     * uplink_no_net_noted once the uplink logged that it has no colour code to descramble with. */
    uint8_t uplink;
    uint8_t uplink_no_net_noted;
} tetra_state_t;

/** The TETRA state of @p state, created on first use; NULL only when @p state is NULL or memory runs out. */
tetra_state_t* tetra_st(dsd_state* state);

/** The TETRA state of @p state without creating it: a zeroed default when it has none; NULL only for a NULL @p state. */
const tetra_state_t* tetra_st_peek(const dsd_state* state);

/** The TETRA state of @p state if it has one, without creating it; NULL otherwise. */
tetra_state_t* tetra_st_existing(dsd_state* state);

/** Drop the TETRA state of @p state; the next tetra_st() starts from zero. Never from inside a TETRA decode, state
 * machine or trunk-tuning call: their callers hold the pointer. */
void tetra_state_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_STATE_H_ */
