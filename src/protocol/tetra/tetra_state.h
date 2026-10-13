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
#include <stdint.h>

#include "tetra_voice_state.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* Mobility management snapshots (EN 300 392-2 clause 16, EN 300 392-7 annex A). tetra_mm.c decodes a whole PDU
 * before it replaces the snapshot of its kind, so each one holds the last complete PDU. Sealed keys are kept as
 * received; a passive monitor never unwraps them. */

/** Address extension: the MCC and MNC of a TSI (EN 300 392-2 table 16.28). */
typedef struct {
    uint16_t mcc;
    uint16_t mnc;
    uint8_t valid;
} tetra_mm_address_t;

/** Time a key change takes effect (EN 300 392-7 table A.111). */
typedef struct {
    uint64_t network_time; /**< time type 1: EN 300 392-2 clause 18.5.24 network time */
    uint16_t hyperframe;   /**< time type 0: absolute IV */
    uint8_t type;          /**< 0 absolute IV, 1 network time, 2 next multiframe, 3 currently in use */
    uint8_t slot;
    uint8_t frame;
    uint8_t multiframe;
} tetra_mm_time_t;

/** SCKs a key change activates (EN 300 392-7 tables A.26 and A.27d): count 0 selects one SCK subset. */
typedef struct {
    uint16_t subset_vn;   /**< SCK-VN of the subset */
    uint16_t version[15]; /**< SCK-VN of each SCK */
    uint8_t count;
    uint8_t grouping; /**< SCK subset grouping type (table A.100a) */
    uint8_t subset;   /**< SCK subset number (table A.100b) */
    uint8_t number[15];
} tetra_mm_sck_set_t;

/** CCK information with its location area information (EN 300 392-7 tables A.42 and A.43). */
typedef struct {
    uint16_t id;
    uint16_t la[15];
    uint16_t la_mask;
    uint16_t la_selector;
    uint16_t la_low;
    uint16_t la_high;
    uint8_t key_type; /**< 0 current, 1 future */
    uint8_t sealed[15];
    uint8_t la_type; /**< 0 all, 1 list, 2 mask and selector, 3 range */
    uint8_t la_count;
    uint8_t future;
    uint8_t future_sealed[15];
} tetra_mm_cck_info_t;

/** D-AUTHENTICATION demand, response, result or reject (EN 300 392-7 tables A.1 to A.4). */
typedef struct {
    uint32_t response; /**< RES2 */
    uint8_t valid;
    uint8_t subtype;              /**< table A.40 */
    uint8_t random_challenge[10]; /**< RAND1 */
    uint8_t random_seed[10];      /**< RS */
    uint8_t mutual;
    uint8_t result;
    uint8_t reject_reason;
} tetra_mm_auth_t;

/** D-OTAR (EN 300 392-7 tables A.9, A.12, A.15, A.16, A.19, A.20, A.23, A.24, A.27d, A.27f, A.27h, A.30 and A.30a):
 * the union of the fields of every downlink sub-type; fields the last sub-type does not carry are zero. */
typedef struct {
    tetra_mm_cck_info_t cck;
    tetra_mm_address_t address;
    tetra_mm_sck_set_t dm_sck;
    tetra_mm_time_t dm_sck_time;
    uint32_t gssi;
    uint32_t gck_select;
    uint32_t group_gssi[30];
    uint32_t gck_reject_gssi[7];
    uint16_t max_response_timer;
    uint16_t gsko_version;
    uint16_t sck_version[7];
    uint16_t gck_number[7];
    uint16_t gck_version[7];
    uint16_t key_status_gck_number;
    uint16_t key_delete_gck_number[15];
    uint8_t valid;
    uint8_t subtype; /**< table A.85 */
    uint8_t cck_provision;
    uint8_t dck_forwarding_result;
    uint8_t ack;
    uint8_t explicit_response;
    uint8_t session_key;
    uint8_t random_seed[10];
    uint8_t key_count;
    uint8_t sck_number[7];
    uint8_t sck_use[7];
    uint8_t sck_sealed[7][15];
    uint8_t gck_sealed[7][15];
    uint8_t reject_reason[7];
    uint8_t group_association;
    uint8_t gck_reject_group_association[7];
    uint8_t key_association_type;
    uint8_t sck_select;
    uint8_t sck_subset_grouping;
    uint8_t group_count;
    uint8_t group_is_range;
    uint8_t group_value_count;
    uint8_t ksg;
    uint8_t retry_interval;
    uint8_t gsko_sealed[15];
    uint8_t key_status_type;
    uint8_t key_status_sck_number;
    uint8_t key_status_sck_grouping;
    uint8_t key_status_sck_subset;
    uint8_t key_delete_type;
    uint8_t key_delete_sck_count;
    uint8_t key_delete_sck_number[31];
    uint8_t key_delete_sck_grouping;
    uint8_t key_delete_sck_subset;
    uint8_t key_delete_gck_count;
    uint8_t dm_sck_ack;
} tetra_mm_otar_t;

/** D-CK CHANGE DEMAND (EN 300 392-7 table A.26). */
typedef struct {
    tetra_mm_sck_set_t sck;
    tetra_mm_time_t time;
    uint16_t cck_id;
    uint16_t gck_vn;
    uint16_t gck_number[15];
    uint16_t gck_version[15];
    uint8_t valid;
    uint8_t ack;
    uint8_t security_class;
    uint8_t key_type; /**< key change type (table A.71) */
    uint8_t sck_use;
    uint8_t gck_count;
} tetra_mm_ck_change_t;

/** D-ENABLE or D-DISABLE (EN 300 392-7 tables A.31 and A.32). */
typedef struct {
    uint64_t tei;
    uint32_t ssi;
    uint16_t mcc;
    uint16_t mnc;
    uint8_t valid;
    uint8_t is_enable;
    uint8_t intent; /**< 0 intent, 1 confirm */
    uint8_t permanent;
    uint8_t equipment;
    uint8_t subscription;
    uint8_t auth_valid;
    uint8_t auth_challenge[20]; /**< RAND1 then RS (table A.37) */
} tetra_mm_enable_disable_t;

/** D-LOCATION UPDATE ACCEPT (EN 300 392-2 table 16.11) type 1 and type 2 elements. */
typedef struct {
    uint32_t ssi;
    tetra_mm_address_t address;
    uint16_t subscriber_class;
    uint16_t energy_saving;
    uint8_t valid;
    uint8_t type;
    uint8_t ssi_valid;
    uint8_t subscriber_class_valid;
    uint8_t energy_saving_valid;
    uint8_t scch;
    uint8_t scch_valid;
} tetra_mm_lu_accept_t;

/** D-LOCATION UPDATE COMMAND (EN 300 392-2 table 16.12). */
typedef struct {
    tetra_mm_address_t address;
    uint16_t cipher_params;
    uint8_t valid;
    uint8_t group_report;
    uint8_t cipher_control;
} tetra_mm_lu_command_t;

/** D-LOCATION UPDATE REJECT (EN 300 392-2 table 16.13). */
typedef struct {
    tetra_mm_address_t address;
    uint16_t cipher_params;
    uint8_t valid;
    uint8_t type;
    uint8_t cause;
    uint8_t cipher_control;
} tetra_mm_lu_reject_t;

/** D-LOCATION UPDATE PROCEEDING (EN 300 392-2 table 16.14). */
typedef struct {
    uint32_t ssi;
    tetra_mm_address_t address;
    uint8_t valid;
} tetra_mm_lu_proceeding_t;

/** One group identity of a group identity downlink element (EN 300 392-2 table 16.54). */
typedef struct {
    uint32_t gssi;
    uint32_t vgssi;
    tetra_mm_address_t address; /**< present with a GTSI (address types 1 and 3) */
    uint8_t action;             /**< 0 attachment, 1 detachment */
    uint8_t attachment_lifetime;
    uint8_t class_of_usage;
    uint8_t detachment_reason;
    uint8_t address_type; /**< 0 GSSI, 1 GTSI, 2 (V)GSSI, 3 GTSI and (V)GSSI */
} tetra_mm_group_entry_t;

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

    /* The CRC evidence trunking and the speech path rely on (tetra_confirm.c). */
    tetra_confirm_t confirm;

    /* Downlink frequencies in Hz, 0 while unknown: the SYSINFO main carrier (dl_carrier_hz) and the carrier of the
     * last traffic-channel assignment (vc_freq_hz). Their blocks below describe them; they sit here for alignment. */
    long dl_carrier_hz;
    long vc_freq_hz;

    /* Mobility management (tetra_mm.c), widest alignment first so the block adds no padding. The group identity,
     * MM status and function-not-supported fields stay flat because tetra_channel_info.c reads them. */
    tetra_mm_otar_t mm_otar;
    tetra_mm_ck_change_t mm_ck_change;
    tetra_mm_enable_disable_t mm_enable_disable;
    tetra_mm_auth_t mm_auth;
    tetra_mm_lu_accept_t mm_lu_accept;
    tetra_mm_lu_proceeding_t mm_lu_proceeding;
    tetra_mm_group_entry_t mm_group_entry[63];
    tetra_mm_lu_command_t mm_lu_command;
    tetra_mm_lu_reject_t mm_lu_reject;
    uint8_t mm_group_entry_count;
    uint8_t mm_group_identity_valid;
    uint8_t mm_group_identity_report;
    uint8_t mm_group_identity_ack_request;
    uint8_t mm_group_identity_attach_detach_mode;
    uint8_t mm_group_ack_result;
    uint8_t mm_group_ack_valid;
    uint8_t mm_status_code;
    uint8_t mm_status_valid;
    uint8_t mm_fns_pdu_type;
    uint8_t mm_fns_valid;
    /* No decoder sets this; tetra_channel_info.c still reads it. */
    uint8_t mm_addr_type;

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
     * dmo_slot/frame:     slot 1..4 and frame 1..18 of the last DSB (frame 0 when it was reserved).
     * dmo_comm_type:      communication type of the last DSB: 0 direct, 1 DM-REP, 2 DM-GATE, 3 DM-REP/GATE.
     * dmo_ab_usage:       A/B channel usage of the last DSB.
     * dmo_crypt:          tetra_dmo_crypt_e of the current call; dmo_security_class its air interface encryption
     *                     state when that is DM-2-A, DM-2-B or DM-2-C.
     * dmo_since_sync:     DNBs processed since the last DMAC-SYNC (colour code staleness). */
    uint8_t dmo;
    uint8_t dmo_dcc_valid;
    uint8_t dmo_slot;
    uint8_t dmo_frame;
    uint8_t dmo_comm_type;
    uint8_t dmo_ab_usage;
    uint8_t dmo_crypt;
    uint8_t dmo_security_class;
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
     * TM-SDU a MAC-END completed). Null PDUs leave them alone.
     *
     * ssi_valid:  1 when active_ssi holds a clear SSI, USSI or SMI; 0 for event labels and encrypted short
     *             identities, which are not subscriber identities.
     * active_ssi: 24-bit identity of that address.
     * enc_mode:   2-bit encryption mode (TETRA_ENC_MODE_* constants). */
    uint32_t active_ssi;
    uint8_t ssi_valid;
    uint8_t enc_mode;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA MLE / CMCE call-event state — updated by tetra_mle_dispatch().
     *
     * call_active:  1 while a call is in progress (set on D-SETUP or
     *                     D-CONNECT, cleared on D-RELEASE / D-DISCONNECT).
     * call_type:    3-bit circuit-mode type from D-SETUP basic service.
     * calling_ssi:  24-bit SSI of the calling party extracted from the
     *                     CMCE D-SETUP "Calling party" optional IE.
     * gssi:         24-bit Group SSI (talkgroup ID) from the MAC-RESOURCE
     *                     address. Set for group/multipoint calls and cleared
     *                     for point-to-point calls.
     * call_id:      14-bit call identifier from CMCE D-SETUP.
     * ───────────────────────────────────────────────────────────────────────── */
    uint16_t call_id;
    uint32_t calling_ssi;
    uint32_t gssi;
    uint32_t cmce_call_generation; /* increments after complete call-establishment state */
    uint8_t call_active;
    uint8_t call_type;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA floor-control state — updated by CMCE D-TX-GRANTED / D-TX-CEASED.
     *
     * tx_granted_valid: 1 while a transmit grant is in force.
     * tx_granted_ssi:   24-bit SSI of the currently-speaking party
     *                          (from the D-TX-GRANTED optional IE).
     * ───────────────────────────────────────────────────────────────────────── */
    uint32_t tx_granted_ssi;
    uint8_t tx_granted_valid;

    /* Frequency band and offset of the main carrier from the last SYSINFO with a valid main carrier. A channel
     * allocation without extended carrier numbering uses them (EN 300 392-2 clause 21.5.2). freq_band 0 means
     * none is known yet. */
    uint8_t freq_band;
    uint8_t freq_offset;

    /* Traffic-channel assignment from the last channel allocation element (EN 300 392-2 table 21.87) of a
     * MAC-RESOURCE or MAC-END PDU.
     *
     * vc_assignment_type:  allocation type.
     * vc_carrier:          12-bit carrier number.
     * vc_slot:             timeslot bitmap, copy of vc_timeslot_bitmap kept for the audio diagnostics.
     * vc_freq_hz:          downlink frequency in Hz, 0 when unknown.
     * vc_addr:             address of the PDU that made the assignment. A later allocation with timeslot
     *                      assigned 0000 (go to the common control channel) releases the assignment only when it
     *                      is addressed to the same party, by SSI or by an event label assigned to that SSI. */
    tetra_mac_address vc_addr;
    uint16_t vc_carrier;
    uint8_t vc_assignment_type;
    uint8_t vc_slot;
    uint8_t vc_assignment_valid;
    uint8_t vc_timeslot_bitmap;
    uint8_t vc_uplink_downlink;
    uint8_t vc_addr_event_label_valid;
    uint8_t trunk_state; /* tetra_sm_state_e, mirrored for UI/state snapshots */

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

    /* call_timeout: call time-out (table 14.50) from D-SETUP, D-CONNECT, D-CONNECT ACKNOWLEDGE or D-INFO.
     * call_slots: speech service or slots per frame, the last two bits of the basic service information (table 14.35). */
    uint8_t call_timeout;
    uint8_t call_slots;

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

    /* D-CONNECT (table 14.7): the encryption flag and circuit mode type of its optional basic service information
     * (both 0 when absent), and connect_valid once a complete D-CONNECT was decoded. */
    uint8_t connect_enc_mode;
    uint8_t connect_call_type;
    uint8_t connect_valid;

    /* D-SDS-DATA with short data type identifier 0: user defined data-1 (16 bits) and its calling party SSI;
     * sds_short_valid is cleared by any other D-SDS-DATA. */
    uint32_t sds_short_src;
    uint16_t sds_short_data;
    uint8_t sds_short_valid;

    /* Floor-control events (tables 14.17, 14.19 and 14.20): tx_continue, tx_interrupted and tx_wait latch when
     * D-TX CONTINUE, D-TX INTERRUPT or D-TX WAIT arrives for the active call and clear on release; tx_event_* hold
     * the last such PDU's fields. tx_timed_out has no downlink PDU and stays 0. */
    uint32_t tx_event_party_ssi;
    uint32_t tx_event_party_extension;
    uint16_t tx_event_call_id;
    uint8_t tx_continue;
    uint8_t tx_interrupted;
    uint8_t tx_wait;
    uint8_t tx_timed_out;
    uint8_t tx_event_notification;
    uint8_t tx_event_continue;
    uint8_t tx_event_grant;
    uint8_t tx_event_encryption;
    uint8_t tx_event_notification_valid;
    uint8_t tx_event_party_type;
    uint8_t tx_event_party_type_valid;
    uint8_t tx_event_party_ssi_valid;
    uint8_t tx_event_party_extension_valid;

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

    /* D-INFO (table 14.11): d_info_call_timeout is the reset call time-out timer flag, d_info_poll_request the poll
     * request flag; each *_valid flag reports its type-2 element. d_info_valid is set once a complete D-INFO was
     * decoded. */
    uint32_t d_info_temporary_address;
    uint16_t d_info_call_id;
    uint16_t d_info_new_call_id;
    uint16_t d_info_modify;
    uint8_t d_info_call_timeout;
    uint8_t d_info_poll_request;
    uint8_t d_info_new_call_id_valid;
    uint8_t d_info_timeout;
    uint8_t d_info_timeout_valid;
    uint8_t d_info_setup_timeout;
    uint8_t d_info_setup_timeout_valid;
    uint8_t d_info_ownership;
    uint8_t d_info_ownership_valid;
    uint8_t d_info_modify_valid;
    uint8_t d_info_status;
    uint8_t d_info_status_valid;
    uint8_t d_info_temporary_address_valid;
    uint8_t d_info_notification_indicator;
    uint8_t d_info_notification_indicator_valid;
    uint8_t d_info_poll_percentage;
    uint8_t d_info_poll_percentage_valid;
    uint8_t d_info_poll_number;
    uint8_t d_info_poll_number_valid;
    uint8_t d_info_valid;

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

    /* D-ALERT (table 14.4), D-CALL PROCEEDING (table 14.5), D-CONNECT ACKNOWLEDGE (table 14.8), D-CALL RESTORE
     * (table 14.6) and CMCE FUNCTION NOT SUPPORTED (table 14.33), each replaced by the next complete PDU of its
     * kind. A *_basic_service, *_status or *_notification field is 0 when its type-2 element was absent. */
    uint16_t d_alert_call_id;
    uint16_t d_call_proc_call_id;
    uint16_t d_connect_ack_call_id;
    uint16_t call_restore_id;
    uint16_t call_restore_new_id;
    uint16_t call_restore_modify;
    uint16_t cmce_fns_call_id;
    uint8_t d_alert_valid;
    uint8_t d_alert_timeout;
    uint8_t d_alert_queued;
    uint8_t d_alert_basic_service;
    uint8_t d_alert_notification;
    uint8_t d_call_proc_valid;
    uint8_t d_call_proc_timeout;
    uint8_t d_call_proc_hook;
    uint8_t d_call_proc_basic_service;
    uint8_t d_call_proc_status;
    uint8_t d_call_proc_notification;
    uint8_t d_connect_ack_valid;
    uint8_t d_connect_ack_tx_grant;
    uint8_t d_connect_ack_tx_permission;
    uint8_t d_connect_ack_notification;
    uint8_t d_connect_ack_notification_valid;
    uint8_t call_restore_grant;
    uint8_t call_restore_reset;
    uint8_t call_restore_new_id_valid;
    uint8_t call_restore_timeout;
    uint8_t call_restore_timeout_valid;
    uint8_t call_restore_status;
    uint8_t call_restore_status_valid;
    uint8_t call_restore_modify_valid;
    uint8_t call_restore_notification;
    uint8_t call_restore_notification_valid;
    uint8_t call_restore_valid;
    uint8_t cmce_fns_rejected_pdu;
    uint8_t cmce_fns_call_id_present;
    uint8_t cmce_fns_pointer;
    uint8_t cmce_fns_extract_bits;
    uint8_t cmce_fns_valid;

    /* D-TX CEASED (table 14.16): its transmission request permission. */
    uint8_t tx_ceased_tx_perm;

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

    /* D-SETUP (table 14.15), D-CONNECT (table 14.7), D-RELEASE / D-DISCONNECT (tables 14.12 and 14.9) and
     * D-TX GRANTED (table 14.18). cmce_notif is the D-SETUP notification indicator, cmce_com_type the communication
     * type of the last basic service information, cmce_setup_calling_type the D-SETUP calling party type identifier
     * (table 14.43). Each *_valid flag reports its type-2 element. cmce_release_cause_type is not set by any PDU.
     * cmce_sds_data_type is the short data type identifier of the last D-SDS-DATA. */
    uint32_t cmce_setup_temporary_address;
    uint32_t cmce_setup_calling_extension;
    uint32_t cmce_connect_temporary_address;
    uint32_t cmce_tx_granted_party_extension;
    uint8_t cmce_notif;
    uint8_t cmce_com_type;
    uint8_t cmce_setup_calling_type;
    uint8_t cmce_setup_basic_service;
    uint8_t cmce_setup_priority;
    uint8_t cmce_setup_notification_valid;
    uint8_t cmce_setup_temporary_address_valid;
    uint8_t cmce_setup_calling_type_valid;
    uint8_t cmce_setup_calling_extension_valid;
    uint8_t cmce_connect_hook;
    uint8_t cmce_connect_duplex;
    uint8_t cmce_connect_tx_grant;
    uint8_t cmce_connect_tx_permission;
    uint8_t cmce_connect_ownership;
    uint8_t cmce_connect_priority;
    uint8_t cmce_connect_priority_valid;
    uint8_t cmce_connect_basic_service;
    uint8_t cmce_connect_basic_service_valid;
    uint8_t cmce_connect_temporary_address_valid;
    uint8_t cmce_connect_notification;
    uint8_t cmce_connect_notification_valid;
    uint8_t cmce_release_cause_type;
    uint8_t cmce_release_cause;
    uint8_t cmce_release_notification;
    uint8_t cmce_release_notification_valid;
    uint8_t cmce_release_was_disconnect;
    uint8_t cmce_tx_granted_perm;
    uint8_t cmce_tx_granted_reserv;
    uint8_t cmce_tx_granted_notification;
    uint8_t cmce_tx_granted_notification_valid;
    uint8_t cmce_tx_granted_party_type;
    uint8_t cmce_tx_granted_party_type_valid;
    uint8_t cmce_tx_granted_party_extension_valid;
    uint8_t cmce_sds_data_type;

    /* D-FACILITY (table 14.10): the SS type of its first SS-PDU. SDS-TL SDS-ACK (table 29.11) and SDS-SHORT REPORT
     * (table 29.13, carried as a D-STATUS pre-coded status): delivery status or short report type, and the message
     * reference. */
    uint8_t facility_valid;
    uint8_t facility_type;
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
