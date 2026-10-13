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

/* MLE network broadcast elements, EN 300 392-2 clause 18.5. Each type keeps the fields something reads. */

/** Neighbour cell information for CA (table 18.64). */
typedef struct {
    uint16_t main_carrier;  /**< main carrier number */
    uint8_t cell_id;        /**< cell identifier CA */
    uint8_t reselect_types; /**< cell reselection types supported */
} tetra_mle_ca_neighbor;

/** Channel class (table 18.41). */
typedef struct {
    uint32_t characteristics; /**< channel characteristics, table 18.40 */
    uint8_t id;               /**< channel class identifier, table 18.42 */
    uint8_t bs_power;         /**< BS transmit power relative to the main carrier */
} tetra_mle_channel_class;

/** Neighbour cell channel class for CA or DA (tables 18.62 and 18.63). */
typedef struct {
    tetra_mle_channel_class channel_class;
    uint8_t cell_id;
} tetra_mle_neighbor_channel_class;

/** Irregular channel detail (table 18.96). */
typedef struct {
    uint16_t carrier;           /**< carrier number */
    uint16_t carrier_extension; /**< carrier number extension, when extension_valid */
    uint8_t channel_id;
    uint8_t extension_valid;
} tetra_mle_irregular_channel;

/** Neighbour cell irregular channel detail for CA or DA (tables 18.66 and 18.67). */
typedef struct {
    tetra_mle_irregular_channel channel;
    uint8_t cell_id;
} tetra_mle_neighbor_irregular_channel;

/**
 * The channel-class and irregular-channel lists that D-NWRK-BROADCAST EXTENSION (table 18.4) and
 * D-NWRK-BROADCAST-DA (table 18.3) both carry. Each count is the decoded list size; its present flag tells an
 * advertised empty list from an omitted one. The array sizes are the largest counts the count widths can carry.
 */
typedef struct {
    tetra_mle_channel_class serving_class[15];
    tetra_mle_neighbor_channel_class neighbor_class[31];
    tetra_mle_irregular_channel serving_irregular[31];
    tetra_mle_neighbor_irregular_channel neighbor_irregular[63];
    uint8_t serving_classes_present;
    uint8_t serving_class_count;
    uint8_t neighbor_classes_present;
    uint8_t neighbor_class_count;
    uint8_t serving_irregular_present;
    uint8_t serving_irregular_count;
    uint8_t neighbor_irregular_present;
    uint8_t neighbor_irregular_count;
} tetra_mle_channel_lists;

/** Neighbour cell information for DA (table 18.65). */
typedef struct {
    uint16_t main_carrier;      /**< main carrier number */
    uint16_t optional_mask;     /**< bit i set: the i-th type-2 element of table 18.65 was present */
    uint16_t carrier_extension; /**< main carrier number extension (type-2 element 0) */
    uint16_t la;                /**< location area (type-2 element 3) */
    uint8_t cell_id;            /**< cell identifier DA */
    uint8_t load_known;         /**< cell load known; load is valid when set */
    uint8_t load;               /**< cell load DA */
} tetra_mle_da_neighbor;

/** D-NWRK-BROADCAST-DA (table 18.3). Each *_valid / *_present flag reports its type-2 element. */
typedef struct {
    uint64_t network_time; /**< TETRA network time, table 18.100 */
    tetra_mle_channel_lists lists;
    tetra_mle_da_neighbor neighbor[7];
    tetra_mle_ca_neighbor local_ca; /**< neighbour cell information for the local CA cell */
    uint16_t reselect;              /**< cell re-select parameters */
    uint8_t known;                  /**< 1 once a complete PDU was decoded */
    uint8_t cell_id_valid;
    uint8_t cell_id; /**< cell identifier DA */
    uint8_t reselect_valid;
    uint8_t load_valid;
    uint8_t load; /**< cell load DA */
    uint8_t network_time_valid;
    uint8_t local_ca_valid;
    uint8_t neighbor_list_present;
    uint8_t neighbor_count;
} tetra_mle_da_broadcast;

/** Channel classes and channels to remove (tables 18.90 to 18.92). */
typedef struct {
    uint8_t class_count;
    uint8_t class_id[15];
    uint8_t channel_count;
    uint8_t channel_id[31];
} tetra_mle_channel_removal;

/** Removal data for a CA or DA cell (tables 18.90 and 18.91); remove_cell=1 removes the cell and carries no lists. */
typedef struct {
    tetra_mle_channel_removal channels;
    uint8_t cell_id;
    uint8_t remove_cell;
} tetra_mle_cell_removal;

/** D-NWRK-BROADCAST REMOVE (table 18.5). */
typedef struct {
    tetra_mle_cell_removal ca[31];
    tetra_mle_cell_removal da[255];
    tetra_mle_channel_removal serving; /**< removal data for the serving cell (table 18.92) */
    uint8_t known;                     /**< 1 once a complete PDU was decoded */
    uint8_t ca_present;
    uint8_t ca_count;
    uint8_t da_present;
    uint8_t da_count;
    uint8_t serving_present;
} tetra_mle_broadcast_removal;

typedef struct tetra_state {
    /* The speech path: codec library, speech integrity and PCM carry (tetra_voice.c). */
    tetra_voice_t voice;

    /* Call following (tetra_trunk_sm.c) and the CRC evidence it and the speech path rely on (tetra_confirm.c). */
    tetra_sm_t sm;
    tetra_confirm_t confirm;

    /* The calls the decoder tracks, follows and publishes (tetra_calls.c). */
    tetra_calls_t calls;

    /* Downlink frequency in Hz of the SYSINFO main carrier, 0 while unknown. Its block below describes it; it sits here
     * for alignment. */
    long dl_carrier_hz;

    /* The status downlink of the last complete D-MM STATUS (EN 300 392-2 table 16.3), for the frontends; mm_status_valid
     * once one was decoded (tetra_mm.c). */
    uint8_t mm_status_code;
    uint8_t mm_status_valid;

    /* Diagnostic gates mirrored from the options at every burst (tetra_frame_enter()), so parsers that are not handed
     * the options can still keep per-burst detail behind -Z (payload) and -v (verbose). */
    uint8_t log_payload;
    uint8_t log_verbose;

    /* uplink: 1 with --tetra-uplink. Normal bursts are then NUBs
     * (EN 300 392-2 table 9.5): the same training sequence as a downlink NDB,
     * but block 1 sits directly against it (no AACH). Scrambling stays the
     * TMO colour code, from a BSCH or from --tetra-net mcc,mnc,colour.
     * tetra_cb: this sync is a control uplink burst (extended training). */
    uint8_t uplink;
    /* 1 once the uplink logged that it has no colour code to descramble with. */
    uint8_t uplink_no_net_noted;

    /* Network identity from the BSCH (EN 300 392-2 table 21.76) or --tetra-net: net_known once MCC, MNC and the 6-bit
     * colour code are set, and lfsr_seed, the scrambling seed they give (tetra_compute_scramb_seed()). */
    uint32_t lfsr_seed;
    uint16_t mcc;
    uint16_t mnc;
    uint8_t net_known;
    uint8_t colour;

    /* Direct mode (EN 300 396-2, EN 300 396-3), set by tetra_dmo_apply_sync().
     * dmo:                1 once a DSB with a DMO system code was decoded: normal bursts have no AACH and use the DM
     *                     colour code. Cleared by a TMO BSCH.
     * dmo_dcc_valid:      1 while dmo_dcc_seed holds the DM colour code of the current transmission (clause 8.2.4).
     * dmo_comm_type:      communication type of the last DSB: 0 direct, 1 DM-REP, 2 DM-GATE, 3 DM-REP/GATE.
     * dmo_crypt:          tetra_dmo_crypt_e of the current call.
     * dmo_since_sync:     DNBs processed since the last DMAC-SYNC (colour code staleness). */
    uint8_t dmo;
    uint8_t dmo_dcc_valid;
    uint8_t dmo_comm_type;
    uint8_t dmo_crypt;
    uint16_t dmo_since_sync;
    uint32_t dmo_dcc_seed;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA network parameters — populated by MAC-BROADCAST/SYSINFO (MLE).
     *
     * sysinfo_known:    1 once a valid SYSINFO PDU has been decoded.
     * nwrk_bcast_known: 1 once a complete D-NWRK-BROADCAST (EN 300 392-2
     *                          table 18.2) has been decoded.
     * la:               14-bit Location Area number (last received from
     *                          either SYSINFO/MLE or D-NWRK-BROADCAST).
     * bs_service_det:   12-bit BS service details bitmask.
     * subscr_class:     16-bit subscriber class mask.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t sysinfo_known;
    uint8_t nwrk_bcast_known;
    uint16_t la;
    uint16_t bs_service_det;
    uint16_t subscr_class;

    /* Address and encryption mode of the last addressed MAC-RESOURCE (or of the MAC-RESOURCE that started the
     * TM-SDU a MAC-END completed). Null PDUs leave them alone. They describe the cell's signalling, for display: the
     * calls keep the encryption mode of their own allocation (tetra_call_t::air_enc), and no CMCE element writes them.
     *
     * ssi_valid:  1 when active_ssi holds a clear SSI, USSI or SMI; 0 for event labels and encrypted short
     *             identities, which are not subscriber identities.
     * active_ssi: 24-bit identity of that address.
     * enc_mode:   2-bit encryption mode (TETRA_ENC_MODE_* constants). */
    uint32_t active_ssi;
    uint8_t ssi_valid;
    uint8_t enc_mode;

    /* Frequency band and offset of the main carrier from the last SYSINFO with a valid main carrier. A channel
     * allocation without extended carrier numbering uses them (EN 300 392-2 clause 21.5.2). freq_band 0 means
     * none is known yet. */
    uint8_t freq_band;
    uint8_t freq_offset;

    /* SDS, EN 300 392-2 clauses 14.7.1.10/14.7.1.11 and 29, published by tetra_mle.c.
     *
     * sds_status / sds_status_src: the last D-STATUS pre-coded status and its calling party SSI (0 when the PDU named
     *   none); 0 status means none received yet.
     * sds_src / sds_text / sds_text_len: the last complete D-SDS-DATA, written together so the sender always belongs
     *   to the text. The text is UTF-8 with C0/C1 controls replaced by spaces, at most 255 bytes, never cut inside a
     *   character.
     * sds_forward_*: the SDS-TL storage/forward address (tables 29.12 and 29.14); type 0 SNA, 1 SSI, 2 TSI,
     *   3 external number, 7 none.
     * sds_concat_*: the last accepted concatenation part (clause 29.5.14); sds_concat_generation counts completed
     *   messages. sds_concat holds the reassembly contexts and is freed with the TETRA state. */
    uint32_t sds_src;
    uint32_t sds_status_src;
    uint32_t sds_forward_ssi;
    uint32_t sds_forward_extension;
    uint32_t sds_concat_generation;
    struct tetra_sds_concat_store* sds_concat; /* reassembly contexts, allocated on first use; see tetra_mle.c */
    uint16_t sds_status;
    uint16_t sds_text_len;
    uint16_t sds_concat_ref;
    char sds_text[256];
    char sds_forward_external[25];
    uint8_t sds_storage_forward;
    uint8_t sds_validity_period;
    uint8_t sds_forward_type;
    uint8_t sds_forward_valid;
    uint8_t sds_forward_sna;
    uint8_t sds_concat_total;
    uint8_t sds_concat_received;
    uint8_t sds_concat_valid;
    uint8_t sds_concat_duplicate;
    uint8_t sds_concat_complete;

    /* SYSINFO fields (EN 300 392-2 table 21.65).
     *
     * cck_valid:         1 when the last SYSINFO carried a CCK identifier or static cipher key version number
     *                    instead of the hyperframe number; cck_id then holds it.
     * duplex_spacing:    3-bit duplex spacing.
     * num_csch:          2-bit number of common secondary control channels on the main carrier.
     * ms_txpwr_max:      3-bit MS_TXPWR_MAX_CELL.
     * rxlev_access_min:  4-bit RXLEV_ACCESS_MIN. */
    uint8_t cck_valid;
    uint16_t cck_id;
    uint8_t duplex_spacing;
    uint8_t num_csch;
    uint8_t ms_txpwr_max;
    uint8_t rxlev_access_min;

    /* Last ACCESS-DEFINE (EN 300 392-2 table 21.79): access code (0..3 for A..D), IMM and WT. The remaining fields
     * are access_num_ra, access_frame_len_f, access_ts_ptr, access_min_pdu_pri and access_common_flag below. */
    uint8_t access_code;
    uint8_t access_imm;
    uint8_t access_wait_time;

    /* Upper MAC counters.
     *
     * frames_total:       signalling MAC blocks decoded.
     * frames_sysinfo:     SYSINFO PDUs.
     * frames_resource:    MAC-RESOURCE PDUs other than Null PDUs.
     * mac_encrypted_sdus: TM-SDUs (and fragmented TM-SDUs) held back because their encryption mode was not 00. */
    uint32_t frames_total;
    uint32_t frames_sysinfo;
    uint32_t frames_resource;
    uint32_t mac_encrypted_sdus;

    /* Fragmented TM-SDU reconstruction, one context per downlink timeslot (index = TN 1..4; index 0 while the TDMA
     * timeslot is unknown). A fragment run belongs to the control channel of its timeslot, so fragments of
     * different timeslots are never joined (EN 300 392-2 clause 23.4.3.1.1). */
    tetra_mac_fragment mac_frag[5];

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA decode quality counters (Phase 35):
     *
     * decode_errors: cumulative count of frames that failed FEC.
     * decode_ok:     cumulative count of frames successfully decoded.
     * ───────────────────────────────────────────────────────────────────────── */
    uint32_t decode_errors;
    uint32_t decode_ok;

    /* TDMA clock (EN 300 392-2 clause 7.3), set by a BSCH and run by tetra_tdma_follow(): while tdma_valid, the slot
     * that started at symbol count tdma_slot_start is timeslot tn 1..4 of frame fn 1..18 of multiframe mn 1..60. */
    uint8_t tdma_valid;
    uint8_t tn;
    uint8_t fn;
    uint8_t mn;
    uint32_t tdma_slot_start;

    /* sds_msg_ref: message reference (table 29.19) of the last SDS-TL TRANSFER, REPORT or ACK.
     * sds_last_cc: colour code the last accepted D-SDS-DATA arrived with. */
    uint8_t sds_msg_ref;
    int8_t sds_last_cc;

    /* D-SDS-DATA with short data type identifier 0: user defined data-1 (16 bits) and its calling party SSI;
     * sds_short_valid is cleared by any other D-SDS-DATA. */
    uint32_t sds_short_src;
    uint16_t sds_short_data;
    uint8_t sds_short_valid;

    /* MLE cell reselection and restoration (tables 18.6 to 18.10) and D-NWRK-BROADCAST (table 18.2). restore_ack is
     * set by a D-RESTORE-ACK whose D-CALL RESTORE decoded; mle_ca_neighbor_count_valid reports that the PDU carried
     * the number of CA neighbour cells. */
    tetra_mle_ca_neighbor mle_ca_neighbor[7];
    uint16_t mle_cell_reselect_params;
    uint8_t mle_cell_load;
    uint8_t mle_ca_neighbor_count;
    uint8_t mle_ca_neighbor_count_valid;
    uint8_t restore_ack;
    uint8_t mle_new_cell_channel_command;
    uint8_t mle_new_cell_valid;
    uint8_t mle_prepare_fail_cause;
    uint8_t mle_prepare_fail_valid;
    uint8_t mle_channel_response_type;
    uint8_t mle_channel_response_reason;
    uint8_t mle_channel_response_retry_delay;
    uint8_t mle_channel_response_valid;
    uint8_t mle_restore_fail_cause;
    uint8_t mle_restore_fail_valid;

    /* 1 when the last SDS text used the UCS-2/UTF-16BE text coding scheme (table 29.29 value 26). */
    uint8_t sds_text_unicode;

    /* D-NWRK-BROADCAST EXTENSION (table 18.4), D-NWRK-BROADCAST REMOVE (table 18.5) and D-NWRK-BROADCAST-DA
     * (table 18.3), each replaced whole by the next complete PDU of its kind. */
    tetra_mle_channel_lists mle_ext;
    tetra_mle_da_broadcast mle_da;
    tetra_mle_broadcast_removal mle_remove;
    uint8_t nwrk_bcast_ext_known;

    /* KSG 0..3 remembered from D-OTAR SCK/GCK Provide, keyed by MCC/MNC.
     * hyperframe is the SYSINFO value from a burst whose CCK flag is clear,
     * kept apart from cck_id so a later CCK identifier does not erase it, and
     * counted on by the TDMA clock at each MN 60 to MN 1; hyperframe_valid
     * drops with the clock and on a change of cell.
     * The operator air keys live in the key set (dsd_state::tetra_air_keys). */
    uint8_t air_ksg_valid[4];
    uint16_t air_ksg_mcc[4];
    uint16_t air_ksg_mnc[4];
    uint8_t air_ksg[4];
    uint8_t hyperframe_valid;
    uint16_t hyperframe;

    /* SDS-TL SDS-REPORT (table 29.12) in a D-SDS-DATA: the delivery status (table 29.16), delivery_ok when the status
     * is in the success range, and the message reference. */
    uint8_t sds_report_valid;
    uint8_t sds_report_delivery_ok;
    uint8_t sds_report_cause;
    uint8_t sds_report_msg_ref;

    /* ACCESS-DEFINE (EN 300 392-2 table 21.79), continued.
     * access_num_ra:      4-bit Nu, number of random access transmissions.
     * access_frame_len_f: 1-bit frame-length factor.
     * access_ts_ptr:      4-bit timeslot pointer.
     * access_min_pdu_pri: 3-bit minimum PDU priority. */
    uint8_t access_num_ra;
    uint8_t access_frame_len_f;
    uint8_t access_ts_ptr;
    uint8_t access_min_pdu_pri;

    /* Header of the last addressed MAC-RESOURCE (EN 300 392-2 table 21.55): fill bit indication, position of
     * grant, random access flag, length indication, address type, event label (10 bits) and usage marker (6 bits). */
    uint8_t mac_fill_bits;
    uint8_t mac_grant_pos;
    uint8_t mac_rand_acc;
    uint8_t mac_len_ind;
    uint8_t mac_addr_type;
    uint16_t mac_event_label;
    uint8_t mac_usage_marker;

    /* SYSINFO (EN 300 392-2 table 21.65): main carrier number, reverse operation, ACCESS_PARAMETER,
     * RADIO_DOWNLINK_TIMEOUT, optional field flag and the 20-bit optional field. access_common_flag is the common
     * or assigned control channel flag of the last ACCESS-DEFINE (table 21.79): 0 common, 1 assigned. */
    uint16_t sysinfo_main_carrier;
    uint8_t sysinfo_rev_op;
    uint8_t sysinfo_acc_param;
    uint8_t sysinfo_radio_dl_tmo;
    uint8_t sysinfo_opt_field_type;
    uint32_t sysinfo_opt_field_data;

    uint8_t access_common_flag;

    /* The short data type identifier of the last D-SDS-DATA (table 14.13). */
    uint8_t cmce_sds_data_type;

    /* SDS-TL SDS-ACK (table 29.11) and SDS-SHORT REPORT (table 29.13, carried as a D-STATUS pre-coded status):
     * delivery status or short report type, and the message reference. */
    uint8_t sds_ack_valid;
    uint8_t sds_ack_msg_ref;
    uint8_t sds_ack_delivery_status;
    uint8_t sds_short_report_valid;
    uint8_t sds_short_report_result;
    uint8_t sds_short_report_msg_ref;

    /* The last SN-PDU (EN 300 392-2 clause 28.4.4): its SN PDU type (table 28.121), its NSAPI (0, a reserved value,
     * for PDU types that carry none) and its length in bits. */
    uint16_t sndcp_nbits;
    uint8_t sndcp_valid;
    uint8_t sndcp_nsapi;
    uint8_t sndcp_pdu_type;
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
