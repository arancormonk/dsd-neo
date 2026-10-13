// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Direct Mode Operation (DMO) MS-MS air interface helpers.
 *
 * Sources: ETSI EN 300 396-2 V1.4.1 (radio aspects: DSB/DNB, scrambling) and
 * ETSI EN 300 396-3 V1.4.1 (DM-MAC: DMAC-SYNC, DMAC-DATA, DM-SETUP ...).
 *
 * DMO differs from TMO downlink in four ways that matter to the decoder:
 *   - the DM Synchronization Burst (DSB) carries SCH/S (60 bits) + SCH/H
 *     (124 bits) and has NO broadcast/AACH block after the training sequence;
 *   - the DM Normal Burst (DNB) has no AACH either: block 1 is the 108 dibits
 *     immediately before the training sequence and block 2 the 108 dibits
 *     immediately after it;
 *   - SCH/S and SCH/H of the DSB are scrambled with an all-zero colour code
 *     (LFSR seed 3); every other channel uses the 30-bit DM colour code
 *     e(1..30) = MNI[5:0] || source address[23:0];
 *   - the system code (first 4 SCH/S bits) is 1xxx for DMO and 0xxx for TMO.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_DMO_H
#define DSD_NEO_PROTOCOL_TETRA_DMO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dsd_opts;
struct dsd_state;

/* DM-MAC message types (EN 300 396-3 clause 9.3.19). */
#define TETRA_DMO_MSG_RESERVED     0
#define TETRA_DMO_MSG_SDS_OCCUPIED 1
#define TETRA_DMO_MSG_TIMING_REQ   2
#define TETRA_DMO_MSG_TIMING_ACK   3
#define TETRA_DMO_MSG_SETUP        8
#define TETRA_DMO_MSG_SETUP_PRES   9
#define TETRA_DMO_MSG_CONNECT      10
#define TETRA_DMO_MSG_DISCONNECT   11
#define TETRA_DMO_MSG_CONNECT_ACK  12
#define TETRA_DMO_MSG_OCCUPIED     13
#define TETRA_DMO_MSG_RELEASE      14
#define TETRA_DMO_MSG_TX_CEASED    15
#define TETRA_DMO_MSG_TX_REQUEST   16
#define TETRA_DMO_MSG_TX_ACCEPT    17
#define TETRA_DMO_MSG_PREEMPT      18
#define TETRA_DMO_MSG_PRE_ACCEPT   19
#define TETRA_DMO_MSG_REJECT       20
#define TETRA_DMO_MSG_INFO         21
#define TETRA_DMO_MSG_SDS_UDATA    22
#define TETRA_DMO_MSG_SDS_DATA     23
#define TETRA_DMO_MSG_SDS_ACK      24
#define TETRA_DMO_MSG_GATEWAY      25 /* subtype follows; EN 300 396-5 clause 14.6.1 */

#define TETRA_DMO_GW_SETUP         0
#define TETRA_DMO_GW_CONNECT       1
#define TETRA_DMO_GW_ACK           2
#define TETRA_DMO_GW_RELEASE       3
#define TETRA_DMO_GW_TX_REQUEST    4
#define TETRA_DMO_GW_TX_ACCEPT     5
#define TETRA_DMO_GW_PREEMPT       6
#define TETRA_DMO_GW_PRE_ACCEPT    7
#define TETRA_DMO_GW_REJECT        8
#define TETRA_DMO_GW_REG_REQUEST   9
#define TETRA_DMO_GW_REG_ACCEPT    10
#define TETRA_DMO_GW_REG_REJECT    11
#define TETRA_DMO_GW_REG_CANCEL    12
#define TETRA_DMO_GW_CANCEL_ACK    13

/* Decoded DMAC-SYNC SCH/S block (60 type-1 bits). */
typedef struct {
    uint8_t system_code;   /* 4 bits: 1100/1101 = DMO */
    uint8_t sync_pdu_type; /* 0 = DMAC-SYNC */
    uint8_t comm_type;     /* 0 direct, 1 via DM-REP, 2 via DM-GATE, 3 REP/GATE */
    uint8_t ms_link_flag;  /* only meaningful when comm_type is 1 or 3 */
    uint8_t gw_flag;       /* only meaningful when comm_type is 2 or 3 */
    uint8_t ab_usage;      /* 0 ch A normal, 1 ch A freq-efficient, 2 ch B */
    uint8_t slot;          /* 1..4 */
    uint8_t frame;         /* 1..18 (0 and >18 reserved) */
    uint8_t enc_state;     /* 0 = DM-1 (clear) */
} tetra_dmo_sync_s_t;

/* Decoded DMAC-SYNC SCH/H block (124 type-1 bits). */
typedef struct {
    uint8_t fill_bit;
    uint8_t frag_flag;
    uint8_t num_schf; /* valid when frag_flag */
    uint8_t frame_countdown;
    uint8_t dst_type;
    uint8_t src_type;
    uint8_t msg_type;
    uint32_t dst;        /* 24 bits */
    uint32_t src;        /* 24 bits */
    uint32_t mni;        /* 24 bits: MCC(10) || MNC(14) */
    uint8_t has_addrs;   /* 1 when addresses and MNI were parsed */
    uint16_t relay_addr; /* 10-bit repeater or gateway address; valid when has_relay */
    uint8_t has_relay;

    /* Message-dependent elements (EN 300 396-3 clause 9.5). `elems` is a bit
     * mask of TETRA_DMO_ELEMS_*; only the fields of that group are valid. */
    uint16_t elems;
    uint8_t timing_flag;
    uint8_t lch_frame3;
    uint8_t preempt_flag;
    uint8_t power_class; /* 0 = not defined, 1..5 */
    uint8_t power_control;
    uint8_t dual_watch;
    uint8_t two_freq;
    uint8_t circuit_mode; /* 0 = TETRA speech, 8 = proprietary speech, 1..7 = circuit mode data */
    uint8_t priority;     /* 0 normal, 1 high, 2 pre-emptive, 3 emergency */
    uint8_t e2ee;         /* end-to-end encryption flag */
    uint8_t group_call;   /* call type flag: 1 group, 0 individual */
    uint8_t ext_source;
    uint8_t reservation;     /* reservation time remaining, units of 6 frames */
    uint8_t has_reservation; /* DM-GACK included reservation parameters */
    uint8_t cease_cause;     /* DM-TX CEASED */
    uint8_t release_cause;   /* DM-RELEASE */
    uint8_t release_ext;     /* DM-RELEASE: present when release_cause == 15 */
    uint8_t has_release_ext;
    uint8_t disconnect_cause; /* DM-DISCONNECT */
    uint8_t reject_cause;     /* DM-REJECT */
    uint8_t sds_remaining;    /* SDS time remaining */
    uint8_t sds_tx_type;      /* 0 stand-alone, 1 inside a circuit-mode call */
    uint8_t fcs_flag;
    uint8_t sdti;       /* short data type identifier */
    uint8_t ack_type;   /* DM-SDS ACK */
    uint16_t sds_len;   /* user-data width in bits */
    uint64_t sds_value; /* fixed user data, status, or the first 64 bits of data 4 */
    uint32_t sds_fcs;
    uint8_t has_fcs;
    uint8_t sds_truncated; /* a variable element ran past this block */
    uint8_t addr_type;     /* additional address type */
    uint8_t has_extra_area;
    uint8_t extra_area;
    uint8_t external_n; /* digits in external[], at most 24 */
    char external[25];
    uint8_t timing_change;  /* 1 when a timing-adjustment element is present */
    uint8_t timing_accept;  /* DM-TIMING ACK: 1 accepted, 0 rejected */
    uint16_t timing_adjust; /* delay code; symbol delay is the code times 5 */
    uint8_t channel_state;  /* DM-PREEMPT perceived channel state */
    uint8_t new_call;       /* 1: pre-emption starts a new call */
    uint8_t preempt_type;   /* DM-PREEMPT / DM-PRE ACCEPT */
    uint8_t info_type;      /* DM-INFO */
    uint32_t calling_mni;   /* DM-INFO or gateway calling-party TSI */
    uint32_t calling_ssi;
    uint8_t has_calling_tsi;
    /* Gateway-specific PDU (message type 25, EN 300 396-5 clause 14.5). */
    uint8_t gw_subtype;
    uint8_t gw_enc; /* gateway encryption state on the SwMI */
    uint8_t request_label;
    uint8_t called_tsi; /* 1: called party MNI is present */
    uint32_t called_ssi;
    uint32_t called_mni;
    uint8_t area;       /* DM-GSETUP area selection */
    uint8_t extra_addr; /* 1: additional addressing follows; its tail is not expanded */
    uint8_t gack_type;  /* 0 fully received, 1 not fully received */
    uint8_t wait_code;  /* DM-MS waiting-timer code */
    uint8_t reg_frames; /* registration transaction time remaining */
    uint8_t reg_label;
    uint8_t reg_unsolicited;
    uint8_t after_preempt; /* DM-GTX REQUEST follows a pre-emption */
    uint32_t gssi;
    uint8_t has_gssi;
} tetra_dmo_sync_h_t;

#define TETRA_DMO_ELEMS_CALL       0x0001u /* DM-SETUP / DM-SETUP PRES / DM-CONNECT ACK / DM-OCCUPIED */
#define TETRA_DMO_ELEMS_CEASED     0x0002u /* DM-TX CEASED */
#define TETRA_DMO_ELEMS_RELEASE    0x0004u /* DM-RELEASE */
#define TETRA_DMO_ELEMS_RESERVED   0x0008u /* DM-RESERVED (reservation after DM-TX CEASED) */
#define TETRA_DMO_ELEMS_SDS_OCC    0x0010u /* DM-SDS OCCUPIED */
#define TETRA_DMO_ELEMS_CONNECT    0x0020u /* DM-CONNECT */
#define TETRA_DMO_ELEMS_DISCONNECT 0x0040u /* DM-DISCONNECT */
#define TETRA_DMO_ELEMS_TX_REQ     0x0080u /* DM-TX REQUEST */
#define TETRA_DMO_ELEMS_TX_ACCEPT  0x0100u /* DM-TX ACCEPT */
#define TETRA_DMO_ELEMS_PREEMPT    0x0200u /* DM-PREEMPT */
#define TETRA_DMO_ELEMS_PRE_ACCEPT 0x0400u /* DM-PRE ACCEPT */
#define TETRA_DMO_ELEMS_REJECT     0x0800u /* DM-REJECT */
#define TETRA_DMO_ELEMS_INFO       0x1000u /* DM-INFO */
#define TETRA_DMO_ELEMS_GW         0x2000u /* message type 25; see gw_subtype */
#define TETRA_DMO_ELEMS_TIMING     0x4000u /* DM-TIMING REQUEST / DM-TIMING ACK */
#define TETRA_DMO_ELEMS_SDS        0x8000u /* DM-SDS UDATA / DATA / ACK */

/* True when the first SCH/S bits carry a DMO system code (1010..1111). */
int tetra_dmo_system_code_is_dmo(const uint8_t* sch_s_bits);

/* Parse the 60 SCH/S type-1 bits. Returns 1 on success. */
int tetra_dmo_parse_sync_s(const uint8_t* bits, tetra_dmo_sync_s_t* out);

/* Presence signal, SCH/S + SCH/H. Communication type 01 is a DM-REP
 * (EN 300 396-4 clause 10.1.2); 10 and 11 are a gateway (EN 300 396-5 clause 14.1.2). */
typedef struct {
    uint8_t comm_type;     /* 1 repeater, 2 DM-GATE, 3 DM-REP/GATE */
    uint8_t two_freq;      /* 1: duplex pair (type 1B / type 2, or a gateway acting as one) */
    uint8_t spacing;       /* 5-bit duplex spacing code, valid when two_freq */
    uint8_t reverse;       /* 1: uplink = downlink - spacing */
    uint8_t ms_link;       /* 0 slave link, 1 master link */
    uint8_t channel_state; /* 0 free, 1 occupation, 2 reservation, 3 registration (gateway) */
    uint8_t swmi_up;       /* gateway: SwMI availability flag */
    uint8_t power_class;
    uint8_t priority;  /* valid when the channel is not free */
    uint16_t repeater; /* 10-bit repeater or gateway address */
    uint32_t mni;      /* repeater MNI, or the gateway's SwMI MNI */
    uint8_t urt;       /* usage restriction type */
    uint8_t parsed;    /* 1 when the SCH/S layout matched this communication type */
} tetra_dmo_pres_t;

/* Parse a DPRES-SYNC. SCH/H may be NULL when its CRC failed; `s` still
 * receives slot and frame when the SCH/S layout applies.
 * Returns 1 for a repeater (01) or gateway (10, 11) presence signal. */
int tetra_dmo_parse_presence(const uint8_t* sch_s, const uint8_t* sch_h, tetra_dmo_sync_s_t* s, tetra_dmo_pres_t* out);

/* "rep=N MNI=mcc/mnc slave free" (or "gw=" for a gateway) and, when a
 * two-frequency offset is present, the uplink spacing. Returns the length written. */
int tetra_dmo_describe_presence(const tetra_dmo_pres_t* p, char* buf, int buf_len);

/* Parse the 124 SCH/H type-1 bits of a DMAC-SYNC PDU. The first block gives
 * the communication type, which selects the layout of the leading field. */
int tetra_dmo_parse_sync_h(const uint8_t* bits, int comm_type, tetra_dmo_sync_h_t* out);

/* Write a short human-readable summary of the parsed message elements
 * ("group speech prio=normal pwr=3 ...") into `buf`; returns its length (0 if none). */
int tetra_dmo_describe_elems(const tetra_dmo_sync_h_t* h, char* buf, int buf_len);

/* LFSR seed for SCH/F, STCH and TCH from the DM colour code.
 * e(1..6) are the six LSBs of `net_id`: the MNI for a direct or DM-REP call
 * (EN 300 396-3 8.2.4), or the 10-bit gateway address for a gateway call
 * (EN 300 396-5 8.2.4). e(7..30) is the source address.
 * The DSB itself always uses seed 3 (all-zero colour code). */
uint32_t tetra_dmo_dcc_seed(uint32_t net_id, uint32_t src_addr);

/* Name of a DM-MAC message type, for logging. */
const char* tetra_dmo_msg_name(int msg_type);

/* Apply a decoded DSB to the decoder state: mode, DM clock and DM colour code.
 * `sync_h` may be NULL when the SCH/H block failed its CRC. */
void tetra_dmo_apply_sync(struct dsd_state* state, const tetra_dmo_sync_s_t* s, const tetra_dmo_sync_h_t* h);

/* Log a decoded DNB MAC block (SCH/F or STCH, `len` type-1 bits). `kind` names
 * the logical channel. Returns the MAC PDU type (0..3) or -1. */
int tetra_dmo_log_dnb_pdu(const uint8_t* bits, int len, const char* kind, struct dsd_state* state);

/* Queue one line for the -N terminal. A repeat of the previous line is dropped
 * so a call that retransmits the same DMAC-SYNC does not fill the display. */
void tetra_dmo_publish(struct dsd_state* state, int msg_type, uint32_t src, uint32_t dst, const char* desc);
void tetra_dmo_publish_presence(struct dsd_state* state, const char* desc);

/* Working mode for the -N terminal: "TMO downlink", "TMO uplink", or
 * "DMO direct|repeater|gateway" with channel and encryption. */
int tetra_dmo_format_mode(const struct dsd_state* state, char* buf, int buf_len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_DMO_H */
