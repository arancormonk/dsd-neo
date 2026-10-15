// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA Direct Mode Operation (DMO): DM-MAC PDUs, the DM colour code and the protection of the current call.
 *
 * EN 300 396-2 (radio aspects), EN 300 396-3 (MS-MS air interface protocol), EN 300 396-4 (DM-REP), EN 300 396-5
 * (DM-GATE) and EN 300 396-6 (security).
 *
 * DMO differs from the TMO downlink in four ways that matter to the decoder:
 * - the DM synchronization burst (DSB) carries SCH/S (60 bits) and SCH/H (124 bits) and has no broadcast block (AACH)
 *   after the training sequence;
 * - the DM normal burst (DNB) has no AACH either: block 1 is the 108 dibits right before the training sequence and
 *   block 2 the 108 dibits right after it;
 * - SCH/S and SCH/H of the DSB are scrambled with an all-zero colour code (LFSR seed 3); every other channel uses the
 *   30-bit DM colour code e(1..30) = MNI[5:0] || source address[23:0] (EN 300 396-3 clause 8.2.4);
 * - the system code (first 4 SCH/S bits) is 1010..1111 for DMO and 0xxx for TMO (EN 300 396-3 clause 9.3.29).
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_DMO_H
#define DSD_NEO_PROTOCOL_TETRA_DMO_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Message type, EN 300 396-3 clause 9.3.19. */
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
#define TETRA_DMO_MSG_GATEWAY      25 /* a gateway message subtype follows, EN 300 396-5 clause 14.6.1 */

/* Gateway message subtype, EN 300 396-5 clause 14.6.1. */
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

/* SYNC PDU type, EN 300 396-3 clause 9.3.28. */
#define TETRA_DMO_SYNC_DMAC        0 /* DMAC-SYNC */
#define TETRA_DMO_SYNC_PRES        1 /* DPRES-SYNC: DM-REP or gateway presence signal */

/* Air interface encryption state, EN 300 396-3 clause 9.3.2 and clause 8.5.3. */
#define TETRA_DMO_AIE_DM1          0 /* security class DM-1: nothing encrypted */
#define TETRA_DMO_AIE_DM2C         1 /* DM-2-C: addresses, message type, all elements and traffic encrypted */
#define TETRA_DMO_AIE_DM2A         2 /* DM-2-A: DM-SDU and traffic encrypted */
#define TETRA_DMO_AIE_DM2B         3 /* DM-2-B: destination address, DM-SDU and traffic encrypted */

/* SCH/S of a DSB (60 type-1 bits): DMAC-SYNC (EN 300 396-3 table 21) or DPRES-SYNC (EN 300 396-4 table 2,
 * EN 300 396-5 table 5). */
typedef struct {
    uint8_t system_code;   /* 1010..1111 for DMO */
    uint8_t sync_pdu_type; /* TETRA_DMO_SYNC_DMAC or TETRA_DMO_SYNC_PRES */
    uint8_t comm_type;     /* 0 direct, 1 via DM-REP, 2 via DM-GATE, 3 via DM-REP/GATE (clause 9.3.3) */
    uint8_t parsed;        /* 1 when the block was read with the layout of this PDU type and communication type */
    uint8_t ms_link_flag;  /* master/slave link flag, with a DM-REP or DM-REP/GATE */
    uint8_t gw_flag;       /* DMAC-SYNC via a gateway: gateway generated message flag */
    uint8_t ab_usage;      /* 0 channel A normal, 1 channel A frequency efficient, 2 channel B */
    uint8_t slot;          /* 1..4 */
    uint8_t frame;         /* 1..18; 0 when the element held a reserved value */
    uint8_t enc_state;     /* DMAC-SYNC: TETRA_DMO_AIE_*; a presence signal carries none and reads DM-1 */
    uint8_t ksg;           /* KSG number (clause 9.3.14) when enc_state is not DM-1 */
    uint8_t key_number;    /* encryption key number 0..31 for SCK 1..32 (clause 9.3.7) when enc_state is not DM-1 */
    uint32_t tvp;          /* time variant parameter (clause 9.3.30) when enc_state is not DM-1 */
} tetra_dmo_sync_s_t;

/* Elements after the message type, indexed into tetra_dmo_sync_h_t::e. Message dependent elements (EN 300 396-3
 * clauses 9.4 to 9.6, EN 300 396-5 clause 14.6) come first and are clear unless the security class is DM-2-C; DM-SDU
 * elements (EN 300 396-3 clause 9.7, EN 300 396-5 clause 14.7) are encrypted in every class but DM-1. */
typedef enum {
    TETRA_DMO_E_TIMING_FLAG,
    TETRA_DMO_E_LCH_FRAME3,
    TETRA_DMO_E_PREEMPT_FLAG,
    TETRA_DMO_E_POWER_CLASS, /* 0 not defined, 1..5 */
    TETRA_DMO_E_POWER_CONTROL,
    TETRA_DMO_E_DUAL_WATCH,
    TETRA_DMO_E_TWO_FREQ,
    TETRA_DMO_E_CIRCUIT_MODE, /* 0 TETRA speech, 1..7 circuit mode data, 8 proprietary speech */
    TETRA_DMO_E_PRIORITY,     /* 0 normal, 1 high, 2 pre-emptive, 3 emergency */
    TETRA_DMO_E_RESERVATION,  /* reservation time remaining, units of 6 frames */
    TETRA_DMO_E_REQUESTS_FLAG,
    TETRA_DMO_E_TIMING_CHANGE, /* timing change announced or required */
    TETRA_DMO_E_TIMING_ADJUST, /* timing adjustment code; the delay is the code times 5 symbols */
    TETRA_DMO_E_TIMING_ACCEPT,
    TETRA_DMO_E_CHANNEL_STATE, /* perceived channel state */
    TETRA_DMO_E_NEW_CALL,
    TETRA_DMO_E_PREEMPT_TYPE,
    TETRA_DMO_E_SDS_REMAINING,
    TETRA_DMO_E_SDS_TX_TYPE, /* 0 stand-alone, 1 inside a circuit mode call */
    TETRA_DMO_E_FCS_FLAG,
    TETRA_DMO_E_GW_SUBTYPE,
    TETRA_DMO_E_GW_ENC, /* gateway encryption state on the SwMI */
    TETRA_DMO_E_RESERVATION_ANNOUNCED,
    TETRA_DMO_E_GTX_TYPE,  /* type of DM-GTX REQUEST: 1 after a pre-emption */
    TETRA_DMO_E_REG_TYPE,  /* type of registration request: 1 unsolicited */
    TETRA_DMO_E_REG_TIME,  /* registration transaction time remaining */
    TETRA_DMO_E_E2EE,      /* the first DM-SDU element */
    TETRA_DMO_E_CALL_TYPE, /* 1 group call, 0 individual call */
    TETRA_DMO_E_EXT_SOURCE,
    TETRA_DMO_E_CEASE_CAUSE,
    TETRA_DMO_E_RELEASE_CAUSE,
    TETRA_DMO_E_RELEASE_EXT, /* present when the release cause is 15 */
    TETRA_DMO_E_DISCONNECT_CAUSE,
    TETRA_DMO_E_REJECT_CAUSE,
    TETRA_DMO_E_INFO_TYPE,
    TETRA_DMO_E_EXTRA_ADDR, /* additional addressing flag */
    TETRA_DMO_E_ADDR_TYPE,  /* additional address type(s) */
    TETRA_DMO_E_CALLING_TSI_FLAG,
    TETRA_DMO_E_CALLING_MNI,
    TETRA_DMO_E_CALLING_SSI,
    TETRA_DMO_E_AREA,
    TETRA_DMO_E_SDTI,
    TETRA_DMO_E_SDS_LENGTH, /* user data width in bits */
    TETRA_DMO_E_FCS,
    TETRA_DMO_E_ACK_TYPE,
    TETRA_DMO_E_REQUEST_LABEL,
    TETRA_DMO_E_CALLED_TSI_FLAG,
    TETRA_DMO_E_CALLED_SSI,
    TETRA_DMO_E_CALLED_MNI,
    TETRA_DMO_E_GACK_TYPE, /* 0 fully received, 1 not fully received */
    TETRA_DMO_E_WAIT_CODE, /* DM-MS waiting timer code */
    TETRA_DMO_E_REG_LABEL,
    TETRA_DMO_E_GSSI_FLAG,
    TETRA_DMO_E_GSSI,
    TETRA_DMO_E_COUNT,
} tetra_dmo_elem;

#define TETRA_DMO_E_SDU_FIRST   TETRA_DMO_E_E2EE

/* What became of the DM-SDU elements. */
#define TETRA_DMO_SDU_CLEAR     0 /* read whole, or the message has none */
#define TETRA_DMO_SDU_TRUNCATED 1 /* the block ended inside them; the elements before the cut are valid */
#define TETRA_DMO_SDU_ENCRYPTED 2 /* air interface encrypted: none were read */

/* What the FCS a set FCS flag appends to the DM-SDU (EN 300 396-3 clauses 6.3.4 and 9.7.10) says of the DM-SDU elements
 * before it. A message whose FCS fails is discarded (clause 6.3.2): nothing of its DM-SDU is published. */
#define TETRA_DMO_FCS_NONE      0 /* no FCS: the FCS flag is clear, or the message has none */
#define TETRA_DMO_FCS_PASSED    1
#define TETRA_DMO_FCS_FAILED 2 /* it does not match them, or the block ended before it, so it cannot vouch for them */
#define TETRA_DMO_FCS_ENCRYPTED 3 /* the DM-SDU, and the FCS in it, are air interface encrypted: neither was read */

/* A DM-MAC PDU with a full header: DMAC-SYNC SCH/H (EN 300 396-3 table 22) or DMAC-DATA (table 23). */
typedef struct {
    uint16_t relay_addr; /* DMAC-SYNC: repeater or gateway address, valid when has_relay */
    uint8_t has_relay;
    uint8_t enc_state; /* air interface encryption state the PDU was read under, TETRA_DMO_AIE_* */
    uint8_t fill_bit;
    uint8_t stolen2; /* DMAC-DATA: second half slot stolen flag */
    uint8_t frag_flag;
    uint8_t null_pdu; /* DMAC-DATA: a Null PDU, nothing follows */
    uint8_t num_schf; /* DMAC-SYNC with frag_flag: SCH/F slots that follow */
    uint8_t frame_countdown;
    uint8_t dst_type;
    uint8_t src_type;
    uint8_t dst_encrypted; /* DM-2-B: the destination address is encrypted; dst reads 0 */
    uint8_t has_addrs;     /* 1 when the header through the message type was read in the clear */
    uint8_t msg_type;
    uint8_t md_ok;      /* 1 when the message dependent elements were read whole */
    uint8_t sdu_state;  /* TETRA_DMO_SDU_* */
    uint8_t fcs_state;  /* TETRA_DMO_FCS_* */
    uint8_t external_n; /* DM-SDU: digits in external, at most 24 */
    uint32_t dst;       /* 24 bits */
    uint32_t src;       /* 24 bits */
    uint32_t mni;       /* 24 bits: MCC(10) || MNC(14) */
    uint32_t e[TETRA_DMO_E_COUNT];
    uint64_t sds_value; /* DM-SDU: user defined data 1 to 3, pre-coded status, or the first 64 bits of data 4 */
    /* DM-SDU: every element read in the clear, as its sdu_bits bits (an FCS that ends them included), and the CRC-32
     * (tetra_llc_fcs32()) of those elements but that FCS, which tells two messages apart wherever they differ: over the
     * FCS as well it would be the CRC's residue, the same for every message whose FCS is right. 0 when the DM-SDU was
     * not read (encrypted, or none). */
    uint32_t sdu_digest;
    uint16_t sdu_bits;
    char external[25]; /* DM-SDU: external subscriber number */
} tetra_dmo_sync_h_t;

/* Presence signal, SCH/S + SCH/H: a DM-REP (communication type 01, EN 300 396-4 clause 10.1.2) or a gateway (10 or 11,
 * EN 300 396-5 clause 14.1.2). */
typedef struct {
    uint8_t comm_type;     /* 1 repeater, 2 DM-GATE, 3 DM-REP/GATE */
    uint8_t mdmo;          /* managed direct mode presence signal (EN 300 396-10) */
    uint8_t swmi_up;       /* gateway: SwMI availability flag */
    uint8_t dmrep;         /* gateway: DM-REP function flag */
    uint8_t two_freq;      /* 1: duplex pair (type 1B / type 2, or a gateway acting as one) */
    uint8_t spacing;       /* 5-bit duplex spacing code, valid when two_freq */
    uint8_t reverse;       /* 1: uplink = downlink - spacing */
    uint8_t ms_link;       /* 0 slave link, 1 master link */
    uint8_t channel_usage; /* 0 the carrier, 1 channel A frequency efficient, 2 channel B */
    uint8_t channel_state; /* 0 free, 1 occupation, 2 reservation, 3 registration phase (gateway) */
    uint8_t power_class;
    uint8_t priority;  /* valid when the channel is not free */
    uint8_t reg_label; /* gateway: registration label */
    uint8_t gw_enc;    /* gateway: encryption state on the SwMI */
    uint8_t urt;       /* usage restriction type */
    uint8_t parsed;    /* 1 when the SCH/H block was read too */
    uint16_t repeater; /* 10-bit repeater or gateway address */
    uint32_t mni;      /* repeater MNI, or the gateway's SwMI MNI */
} tetra_dmo_pres_t;

/* How the traffic of the current direct mode call is protected, from the DMAC-SYNC that set it up or occupies the
 * channel (EN 300 396-3 clause 8.5.3). This decoder has no DMO air interface decryption (EN 300 396-6) and no
 * end-to-end decryption, so only CLEAR traffic is audible. */
typedef enum {
    TETRA_DMO_CRYPT_UNKNOWN = 0, /* no call set-up or occupation since the last transmission ended */
    TETRA_DMO_CRYPT_CLEAR,       /* security class DM-1 and the end-to-end encryption flag clear */
    TETRA_DMO_CRYPT_AIR,         /* security class DM-2-A, DM-2-B or DM-2-C */
    TETRA_DMO_CRYPT_E2E,         /* security class DM-1 with the end-to-end encryption flag set (clause 9.7.8) */
} tetra_dmo_crypt_e;

/* 1 when the first SCH/S bits carry a DMO system code (1010..1111). */
int tetra_dmo_system_code_is_dmo(const uint8_t* sch_s_bits);

/* Read the 60 SCH/S type-1 bits with the layout their SYNC PDU type and communication type select. Returns 1 when
 * that layout is known (out->parsed); the header fields are filled either way. */
int tetra_dmo_parse_sync_s(const uint8_t* bits, tetra_dmo_sync_s_t* out);

/* Read a DPRES-SYNC. SCH/H may be NULL when its CRC failed; `s` keeps the slot, frame and link of the SCH/S either
 * way. Returns 1 for a repeater (01) or gateway (10, 11) presence signal. */
int tetra_dmo_parse_presence(const uint8_t* sch_s, const uint8_t* sch_h, tetra_dmo_sync_s_t* s, tetra_dmo_pres_t* out);

/* "rep=N MNI=mcc/mnc slave free ..." (or "gw=" for a gateway) and, for a duplex pair, the uplink offset. Returns the
 * length written. */
int tetra_dmo_describe_presence(const tetra_dmo_pres_t* p, char* buf, int buf_len);

/* Read the 124 SCH/H type-1 bits of a DMAC-SYNC as if they were clear. The SCH/S communication type selects the
 * leading address field. tetra_dmo_apply_sync() then clears what the SCH/S security class encrypts. Returns 1. */
int tetra_dmo_parse_sync_h(const uint8_t* bits, int comm_type, tetra_dmo_sync_h_t* out);

/* Read a DMAC-DATA PDU (SCH/F or STCH, `len` type-1 bits) under its own air interface encryption state. Returns 1
 * for a DMAC-DATA PDU, 0 for another MAC PDU type. */
int tetra_dmo_parse_data(const uint8_t* bits, int len, int comm_type, tetra_dmo_sync_h_t* out);

/* A short summary of the elements ("group speech prio=normal pwr=3 ..."); returns its length, 0 when there is none. */
int tetra_dmo_describe_elems(const tetra_dmo_sync_h_t* h, char* buf, int buf_len);

/* LFSR seed for SCH/F, STCH and TCH from the DM colour code. e(1..6) are the six LSBs of `net_id`: the MNI for a direct
 * or DM-REP call (EN 300 396-3 clause 8.2.4), or the 10-bit gateway address for a gateway call (EN 300 396-5 clause
 * 8.2.4). e(7..30) is the source address. The DSB itself always uses seed 3 (all-zero colour code). */
uint32_t tetra_dmo_dcc_seed(uint32_t net_id, uint32_t src_addr);

/* Name of a DM-MAC message type, for logging. */
const char* tetra_dmo_msg_name(int msg_type);

/* Apply a decoded DSB to the decoder state: mode, DM clock, DM colour code and call protection. `h` may be NULL when
 * the SCH/H failed its CRC; otherwise its encrypted parts are first cleared to match the SCH/S security class. A
 * DMAC-SYNC whose message is not known (no `h`, or DM-2-C) may be the one that sets the protection of the traffic
 * after it, so an encrypted SCH/S security class seals that traffic (TETRA_DMO_CRYPT_AIR). */
void tetra_dmo_apply_sync(dsd_state* state, const tetra_dmo_sync_s_t* s, tetra_dmo_sync_h_t* h);

/* How the traffic of the current direct mode call is protected; UNKNOWN outside direct mode. */
tetra_dmo_crypt_e tetra_dmo_call_crypt(const dsd_state* state);

/* The carrier was left: forget its DM colour code (and its seed) and the protection of its call, so no normal burst of
 * the next carrier is descrambled with them and its traffic is not classified by them, and direct mode itself: the next
 * carrier is trunked until its own DSB says otherwise. Never creates TETRA state. */
void tetra_dmo_forget_carrier(dsd_state* state);

/* The direct mode channel's traffic transmission is over: a DM-TX CEASED, DM-RELEASE or DM-DISCONNECT on its STCH, or
 * nothing of its call heard for a while. Its DM colour code and protection are no other transmission's (EN 300 396-3
 * clauses 8.2.4 and 8.5.3): the next one's DMAC-SYNC gives its own. Never creates TETRA state. */
void tetra_dmo_transmission_over(dsd_state* state);

/* A trunked mode burst proved the carrier: a BSCH of a trunked system code or a control uplink burst's SCH/HU passed
 * its CRC. Direct mode ends: its normal bursts are trunked ones from here (EN 300 392-2 clause 9.4.4), and its DM colour
 * code, protection and call, with the call's canonical call, go with it. Nothing when the carrier was not in direct
 * mode; @p opts may be NULL. Never creates TETRA state. */
void tetra_dmo_leave(dsd_opts* opts, dsd_state* state);

/* Log a decoded DNB MAC block (SCH/F or STCH, `len` type-1 bits) and hand a DMAC-DATA PDU's call to the call records.
 * `kind` names the logical channel; `opts` may be NULL. Returns the MAC PDU type (0..3) or -1. */
int tetra_dmo_log_dnb_pdu(const uint8_t* bits, int len, const char* kind, dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_DMO_H */
