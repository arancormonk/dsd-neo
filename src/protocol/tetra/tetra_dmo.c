// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Direct Mode Operation: DM-MAC synchronization and signalling PDUs, the DM colour code and the protection of the
 * current call. EN 300 396-2 clauses 8.2.5 and 9.4.3; EN 300 396-3 clauses 8.2.4, 8.5.3 and 9; EN 300 396-4 clause 10;
 * EN 300 396-5 clause 14.
 *
 * The element tables follow the ETSI tables row for row. Reserved elements at the very end of a PDU are left out:
 * nothing after them is read, so a block that ends inside them is not short.
 */

#include <dsd-neo/protocol/tetra/tetra_dmo.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/state_fwd.h"
#include "tetra_calls.h"
#include "tetra_log.h"
#include "tetra_state.h"

#define DMO_SCH_S_BITS 60
#define DMO_SCH_H_BITS 124
#define DMO_COUNT(a)   (sizeof(a) / sizeof((a)[0]))

/* ---- Element tables ------------------------------------------------------------------------------------------- */

enum { DMO_NONE = 0xFF };

enum {
    DMO_STEP_FIELD = 0, /* `width` bits into e[elem], or skipped when elem is DMO_NONE */
    DMO_STEP_EXTERNAL,  /* external subscriber number, EN 300 396-5 clause 14.7.6 */
    DMO_STEP_SDS_DATA,  /* the user data an SDTI announces, EN 300 396-3 clause 9.7.16 */
};

/* One row of a PDU table. A row whose `cond` is not DMO_NONE is present only when bit e[cond] of `mask` is set. */
typedef struct {
    uint8_t elem;
    uint8_t width;
    uint8_t cond;
    uint8_t kind;
    uint16_t mask;
} dmo_step;

#define DMO_IS(v)             ((uint16_t)(1u << (v)))
#define DMO_EL(e, w)          {(uint8_t)(e), (uint8_t)(w), DMO_NONE, DMO_STEP_FIELD, 0u}
#define DMO_EL_IF(e, w, c, m) {(uint8_t)(e), (uint8_t)(w), (uint8_t)(c), DMO_STEP_FIELD, (uint16_t)(m)}
#define DMO_SKIP(w)           {DMO_NONE, (uint8_t)(w), DMO_NONE, DMO_STEP_FIELD, 0u}
#define DMO_SKIP_IF(w, c, m)  {DMO_NONE, (uint8_t)(w), (uint8_t)(c), DMO_STEP_FIELD, (uint16_t)(m)}
#define DMO_DO(k)             {DMO_NONE, 0u, DMO_NONE, (uint8_t)(k), 0u}
#define DMO_DO_IF(k, c, m)    {DMO_NONE, 0u, (uint8_t)(c), (uint8_t)(k), (uint16_t)(m)}

/* Timing change announced (or required) and the timing adjustment it brings, clauses 9.6.19 to 9.6.21. */
#define DMO_TIMING_CHANGE                                                                                              \
    DMO_EL(TETRA_DMO_E_TIMING_CHANGE, 1), DMO_EL_IF(TETRA_DMO_E_TIMING_ADJUST, 12, TETRA_DMO_E_TIMING_CHANGE, DMO_IS(1))

/* A calling party TSI (MNI then SSI, clause 9.7.4) present when e[c] is one of `m`. */
#define DMO_CALLING_TSI_IF(c, m)                                                                                       \
    DMO_EL_IF(TETRA_DMO_E_CALLING_MNI, 24, c, m), DMO_EL_IF(TETRA_DMO_E_CALLING_SSI, 24, c, m)

/* Additional addressing flag and address types (EN 300 396-5 table 25): 1 calling TSI, 2 area, 3 area and TSI, 4
 * external number, 5 external number and TSI; in each case the TSI comes first. */
#define DMO_ADDR_TYPE_TSI  (DMO_IS(1) | DMO_IS(3) | DMO_IS(5))
#define DMO_ADDR_TYPE_AREA (DMO_IS(2) | DMO_IS(3))
#define DMO_ADDR_TYPE_EXT  (DMO_IS(4) | DMO_IS(5))
#define DMO_EXTRA_ADDRESSING                                                                                           \
    DMO_EL(TETRA_DMO_E_EXTRA_ADDR, 1), DMO_EL_IF(TETRA_DMO_E_ADDR_TYPE, 4, TETRA_DMO_E_EXTRA_ADDR, DMO_IS(1)),         \
        DMO_CALLING_TSI_IF(TETRA_DMO_E_ADDR_TYPE, DMO_ADDR_TYPE_TSI)

/* EN 300 396-3 table 27, DM-RESERVED. The dual watch synchronization and two-frequency call flags at its end were not
 * in the first edition, so they are not read. */
static const dmo_step k_reserved_md[] = {
    DMO_SKIP(1), /* channel reservation type */
    DMO_EL(TETRA_DMO_E_RESERVATION, 6),
    DMO_EL(TETRA_DMO_E_TIMING_FLAG, 1),
    DMO_EL(TETRA_DMO_E_REQUESTS_FLAG, 1),
    DMO_SKIP(1),                                          /* changeover requests flag */
    DMO_SKIP_IF(8, TETRA_DMO_E_REQUESTS_FLAG, DMO_IS(1)), /* requests bitmap */
    DMO_EL(TETRA_DMO_E_POWER_CLASS, 3),
    DMO_EL(TETRA_DMO_E_POWER_CONTROL, 1),
    DMO_EL(TETRA_DMO_E_PRIORITY, 2),
};

/* Table 28, DM-SDS OCCUPIED. */
static const dmo_step k_sds_occupied_md[] = {
    DMO_EL(TETRA_DMO_E_SDS_REMAINING, 4),
    DMO_EL(TETRA_DMO_E_SDS_TX_TYPE, 1),
    DMO_EL(TETRA_DMO_E_PRIORITY, 2),
};

/* Tables 29 and 30, DM-TIMING REQUEST and DM-TIMING ACK. */
static const dmo_step k_timing_req_md[] = {DMO_EL(TETRA_DMO_E_TIMING_ADJUST, 12)};
static const dmo_step k_timing_ack_md[] = {DMO_EL(TETRA_DMO_E_TIMING_ACCEPT, 1), DMO_TIMING_CHANGE};

/* Tables 31, 35 and 36: DM-SETUP, DM-CONNECT ACK and DM-OCCUPIED. Table 32, DM-SETUP PRES, starts with 3 reserved bits
 * in place of the three flags. */
#define DMO_CALL_MD_TAIL                                                                                               \
    DMO_EL(TETRA_DMO_E_POWER_CLASS, 3), DMO_EL(TETRA_DMO_E_POWER_CONTROL, 1), DMO_SKIP(2),                             \
        DMO_EL(TETRA_DMO_E_DUAL_WATCH, 1), DMO_EL(TETRA_DMO_E_TWO_FREQ, 1), DMO_EL(TETRA_DMO_E_CIRCUIT_MODE, 4),       \
        DMO_SKIP(4), DMO_EL(TETRA_DMO_E_PRIORITY, 2)
static const dmo_step k_setup_md[] = {
    DMO_EL(TETRA_DMO_E_TIMING_FLAG, 1),
    DMO_EL(TETRA_DMO_E_LCH_FRAME3, 1),
    DMO_EL(TETRA_DMO_E_PREEMPT_FLAG, 1),
    DMO_CALL_MD_TAIL,
};
static const dmo_step k_setup_pres_md[] = {DMO_SKIP(3), DMO_CALL_MD_TAIL};
static const dmo_step k_setup_sdu[] = {
    DMO_EL(TETRA_DMO_E_E2EE, 1),
    DMO_EL(TETRA_DMO_E_CALL_TYPE, 1),
    DMO_EL(TETRA_DMO_E_EXT_SOURCE, 1),
};

/* Tables 33 and 34, DM-CONNECT and DM-DISCONNECT. */
static const dmo_step k_connect_md[] = {DMO_EL(TETRA_DMO_E_CIRCUIT_MODE, 4)};
static const dmo_step k_disconnect_sdu[] = {DMO_EL(TETRA_DMO_E_DISCONNECT_CAUSE, 4)};

/* Table 37, DM-RELEASE: the release cause extension follows release cause 15. */
static const dmo_step k_release_sdu[] = {
    DMO_EL(TETRA_DMO_E_RELEASE_CAUSE, 4),
    DMO_EL_IF(TETRA_DMO_E_RELEASE_EXT, 5, TETRA_DMO_E_RELEASE_CAUSE, DMO_IS(15)),
};

/* Table 38, DM-TX CEASED. */
static const dmo_step k_tx_ceased_md[] = {
    DMO_EL(TETRA_DMO_E_RESERVATION, 6),
    DMO_EL(TETRA_DMO_E_TIMING_FLAG, 1),
    DMO_EL(TETRA_DMO_E_REQUESTS_FLAG, 1),
    DMO_SKIP(1),                                          /* changeover requests flag */
    DMO_SKIP_IF(8, TETRA_DMO_E_REQUESTS_FLAG, DMO_IS(1)), /* requests bitmap */
    DMO_SKIP(1),                                          /* recent user priority flag */
    DMO_TIMING_CHANGE,
    DMO_EL(TETRA_DMO_E_PRIORITY, 2),
};
static const dmo_step k_tx_ceased_sdu[] = {DMO_EL(TETRA_DMO_E_CEASE_CAUSE, 4)};

/* Tables 39 to 42: DM-TX REQUEST, DM-TX ACCEPT, DM-PREEMPT and DM-PRE ACCEPT. */
static const dmo_step k_tx_request_md[] = {DMO_TIMING_CHANGE, DMO_EL(TETRA_DMO_E_PRIORITY, 2)};
static const dmo_step k_tx_accept_md[] = {DMO_TIMING_CHANGE};
static const dmo_step k_preempt_md[] = {
    DMO_EL(TETRA_DMO_E_CHANNEL_STATE, 2), DMO_TIMING_CHANGE,
    DMO_EL(TETRA_DMO_E_NEW_CALL, 1),      DMO_EL(TETRA_DMO_E_PREEMPT_TYPE, 4),
    DMO_EL(TETRA_DMO_E_PRIORITY, 2),
};
static const dmo_step k_pre_accept_md[] = {
    DMO_TIMING_CHANGE,
    DMO_EL(TETRA_DMO_E_NEW_CALL, 1),
    DMO_EL(TETRA_DMO_E_PREEMPT_TYPE, 4),
};

/* Table 43, DM-REJECT. */
static const dmo_step k_reject_sdu[] = {DMO_EL(TETRA_DMO_E_REJECT_CAUSE, 4)};

/* Table 44 with EN 300 396-5 table 7, DM-INFO: information type 0 is the calling TSI, 1 an external number, 2 both. */
static const dmo_step k_info_sdu[] = {
    DMO_EL(TETRA_DMO_E_INFO_TYPE, 3),
    DMO_CALLING_TSI_IF(TETRA_DMO_E_INFO_TYPE, DMO_IS(0) | DMO_IS(2)),
    DMO_DO_IF(DMO_STEP_EXTERNAL, TETRA_DMO_E_INFO_TYPE, DMO_IS(1) | DMO_IS(2)),
};

/* Tables 45 and 46 with EN 300 396-5 table 8: DM-SDS UDATA and DM-SDS DATA. */
static const dmo_step k_sds_md[] = {
    DMO_EL(TETRA_DMO_E_SDS_REMAINING, 4),
    DMO_EL(TETRA_DMO_E_SDS_TX_TYPE, 1),
    DMO_EL(TETRA_DMO_E_PRIORITY, 2),
    DMO_EL(TETRA_DMO_E_FCS_FLAG, 1),
};
static const dmo_step k_sds_sdu[] = {
    DMO_EXTRA_ADDRESSING,
    DMO_EL_IF(TETRA_DMO_E_AREA, 4, TETRA_DMO_E_ADDR_TYPE, DMO_ADDR_TYPE_AREA),
    DMO_DO_IF(DMO_STEP_EXTERNAL, TETRA_DMO_E_ADDR_TYPE, DMO_ADDR_TYPE_EXT),
    DMO_EL(TETRA_DMO_E_SDTI, 4),
    DMO_DO(DMO_STEP_SDS_DATA),
    DMO_EL_IF(TETRA_DMO_E_FCS, 32, TETRA_DMO_E_FCS_FLAG, DMO_IS(1)),
};

/* Table 47, DM-SDS ACK: acknowledgement type 1 carries data. */
static const dmo_step k_sds_ack_md[] = {DMO_EL(TETRA_DMO_E_FCS_FLAG, 1)};
static const dmo_step k_sds_ack_sdu[] = {
    DMO_EL(TETRA_DMO_E_ACK_TYPE, 4),
    DMO_EL_IF(TETRA_DMO_E_SDTI, 4, TETRA_DMO_E_ACK_TYPE, DMO_IS(1)),
    DMO_DO_IF(DMO_STEP_SDS_DATA, TETRA_DMO_E_ACK_TYPE, DMO_IS(1)),
    DMO_EL_IF(TETRA_DMO_E_FCS, 32, TETRA_DMO_E_FCS_FLAG, DMO_IS(1)),
};

/* EN 300 396-5 tables 9 to 22, gateway-specific messages, after the gateway message subtype. */
static const dmo_step k_gsetup_md[] = {
    DMO_EL(TETRA_DMO_E_CIRCUIT_MODE, 4),
    DMO_SKIP(4),
    DMO_EL(TETRA_DMO_E_PRIORITY, 2),
};
static const dmo_step k_gsetup_sdu[] = {
    DMO_EL(TETRA_DMO_E_E2EE, 1),
    DMO_EL(TETRA_DMO_E_CALL_TYPE, 1),
    DMO_SKIP(2),
    DMO_EL(TETRA_DMO_E_REQUEST_LABEL, 4),
    DMO_EL(TETRA_DMO_E_CALLED_TSI_FLAG, 1),
    DMO_EL(TETRA_DMO_E_CALLED_SSI, 24),
    DMO_EL_IF(TETRA_DMO_E_CALLED_MNI, 24, TETRA_DMO_E_CALLED_TSI_FLAG, DMO_IS(1)),
    DMO_EL(TETRA_DMO_E_AREA, 4),
    DMO_EXTRA_ADDRESSING,
    DMO_DO_IF(DMO_STEP_EXTERNAL, TETRA_DMO_E_ADDR_TYPE, DMO_ADDR_TYPE_EXT),
};
/* DM-GCONNECT, DM-GTX ACCEPT and DM-GPRE ACCEPT share their message dependent elements. */
static const dmo_step k_gconnect_md[] = {
    DMO_EL(TETRA_DMO_E_POWER_CLASS, 3), DMO_EL(TETRA_DMO_E_POWER_CONTROL, 1), DMO_SKIP(1),
    DMO_EL(TETRA_DMO_E_GW_ENC, 1),      DMO_EL(TETRA_DMO_E_CIRCUIT_MODE, 4),  DMO_SKIP(4),
};
static const dmo_step k_gconnect_sdu[] = {
    DMO_EL(TETRA_DMO_E_E2EE, 1),
    DMO_EL(TETRA_DMO_E_CALL_TYPE, 1),
    DMO_EL(TETRA_DMO_E_REQUEST_LABEL, 4),
};
static const dmo_step k_gtx_accept_sdu[] = {DMO_EL(TETRA_DMO_E_E2EE, 1), DMO_EL(TETRA_DMO_E_CALL_TYPE, 1)};
static const dmo_step k_gack_md[] = {
    DMO_TIMING_CHANGE,
    DMO_EL(TETRA_DMO_E_RESERVATION_ANNOUNCED, 1),
    DMO_SKIP_IF(1, TETRA_DMO_E_RESERVATION_ANNOUNCED, DMO_IS(1)), /* channel reservation type */
    DMO_EL_IF(TETRA_DMO_E_RESERVATION, 6, TETRA_DMO_E_RESERVATION_ANNOUNCED, DMO_IS(1)),
    DMO_EL_IF(TETRA_DMO_E_REQUESTS_FLAG, 1, TETRA_DMO_E_RESERVATION_ANNOUNCED, DMO_IS(1)),
    DMO_SKIP_IF(1, TETRA_DMO_E_RESERVATION_ANNOUNCED, DMO_IS(1)), /* changeover requests flag */
    DMO_SKIP_IF(8, TETRA_DMO_E_REQUESTS_FLAG, DMO_IS(1)),         /* requests bitmap */
    DMO_EL_IF(TETRA_DMO_E_POWER_CLASS, 3, TETRA_DMO_E_RESERVATION_ANNOUNCED, DMO_IS(1)),
    DMO_EL_IF(TETRA_DMO_E_POWER_CONTROL, 1, TETRA_DMO_E_RESERVATION_ANNOUNCED, DMO_IS(1)),
    DMO_SKIP_IF(1, TETRA_DMO_E_RESERVATION_ANNOUNCED, DMO_IS(1)),
    DMO_EL_IF(TETRA_DMO_E_PRIORITY, 2, TETRA_DMO_E_RESERVATION_ANNOUNCED, DMO_IS(1)),
    DMO_SKIP(4),
};
static const dmo_step k_gack_sdu[] = {DMO_EL(TETRA_DMO_E_GACK_TYPE, 2), DMO_EL(TETRA_DMO_E_WAIT_CODE, 3)};
static const dmo_step k_grelease_sdu[] = {DMO_EL(TETRA_DMO_E_RELEASE_CAUSE, 4)};
static const dmo_step k_gtx_request_md[] = {
    DMO_EL(TETRA_DMO_E_GTX_TYPE, 1), DMO_SKIP(3), DMO_EL(TETRA_DMO_E_CIRCUIT_MODE, 4), DMO_SKIP(4),
    DMO_EL(TETRA_DMO_E_PRIORITY, 2),
};
static const dmo_step k_gpreempt_md[] = {
    DMO_EL(TETRA_DMO_E_CHANNEL_STATE, 2), DMO_SKIP(2), DMO_EL(TETRA_DMO_E_CIRCUIT_MODE, 4), DMO_SKIP(4),
    DMO_EL(TETRA_DMO_E_PRIORITY, 2),
};
static const dmo_step k_gtx_request_sdu[] = {
    DMO_EL(TETRA_DMO_E_E2EE, 1),
    DMO_EL(TETRA_DMO_E_CALL_TYPE, 1),
    DMO_SKIP(2),
    DMO_EL(TETRA_DMO_E_CALLING_TSI_FLAG, 1),
    DMO_CALLING_TSI_IF(TETRA_DMO_E_CALLING_TSI_FLAG, DMO_IS(1)),
};
static const dmo_step k_greject_sdu[] = {DMO_EL(TETRA_DMO_E_REJECT_CAUSE, 4)};
static const dmo_step k_greg_request_md[] = {
    DMO_EL(TETRA_DMO_E_REG_TYPE, 1),
    DMO_EL(TETRA_DMO_E_REG_TIME, 4),
    DMO_SKIP(2),
};
static const dmo_step k_greg_request_sdu[] = {
    DMO_EL(TETRA_DMO_E_CALLING_TSI_FLAG, 1),
    DMO_CALLING_TSI_IF(TETRA_DMO_E_CALLING_TSI_FLAG, DMO_IS(1)),
    DMO_EL(TETRA_DMO_E_GSSI_FLAG, 1),
    DMO_EL_IF(TETRA_DMO_E_GSSI, 24, TETRA_DMO_E_GSSI_FLAG, DMO_IS(1)),
};
static const dmo_step k_greg_answer_md[] = {DMO_EL(TETRA_DMO_E_REG_TYPE, 1), DMO_SKIP(2)};
static const dmo_step k_greg_accept_sdu[] = {DMO_EL(TETRA_DMO_E_REG_LABEL, 4)};
static const dmo_step k_greg_reject_sdu[] = {DMO_EL(TETRA_DMO_E_REG_LABEL, 4), DMO_EL(TETRA_DMO_E_REJECT_CAUSE, 4)};
static const dmo_step k_greg_cancel_md[] = {DMO_EL(TETRA_DMO_E_REG_TIME, 4), DMO_SKIP(2)};
static const dmo_step k_gcancel_ack_md[] = {DMO_SKIP(2)};

typedef struct {
    const dmo_step* md;
    const dmo_step* sdu;
    uint8_t n_md;
    uint8_t n_sdu;
} dmo_layout;

#define DMO_LAYOUT(md, sdu) {(md), (sdu), (uint8_t)DMO_COUNT(md), (uint8_t)DMO_COUNT(sdu)}
#define DMO_LAYOUT_MD(md)   {(md), NULL, (uint8_t)DMO_COUNT(md), 0u}
#define DMO_LAYOUT_SDU(sdu) {NULL, (sdu), 0u, (uint8_t)DMO_COUNT(sdu)}

/* By message type. Types with no entry (reserved, proprietary, and 25 whose layout is per gateway subtype) read no
 * elements. */
static const dmo_layout k_msg_layouts[32] = {
    [TETRA_DMO_MSG_RESERVED] = DMO_LAYOUT_MD(k_reserved_md),
    [TETRA_DMO_MSG_SDS_OCCUPIED] = DMO_LAYOUT_MD(k_sds_occupied_md),
    [TETRA_DMO_MSG_TIMING_REQ] = DMO_LAYOUT_MD(k_timing_req_md),
    [TETRA_DMO_MSG_TIMING_ACK] = DMO_LAYOUT_MD(k_timing_ack_md),
    [TETRA_DMO_MSG_SETUP] = DMO_LAYOUT(k_setup_md, k_setup_sdu),
    [TETRA_DMO_MSG_SETUP_PRES] = DMO_LAYOUT(k_setup_pres_md, k_setup_sdu),
    [TETRA_DMO_MSG_CONNECT] = DMO_LAYOUT_MD(k_connect_md),
    [TETRA_DMO_MSG_DISCONNECT] = DMO_LAYOUT_SDU(k_disconnect_sdu),
    [TETRA_DMO_MSG_CONNECT_ACK] = DMO_LAYOUT(k_setup_md, k_setup_sdu),
    [TETRA_DMO_MSG_OCCUPIED] = DMO_LAYOUT(k_setup_md, k_setup_sdu),
    [TETRA_DMO_MSG_RELEASE] = DMO_LAYOUT_SDU(k_release_sdu),
    [TETRA_DMO_MSG_TX_CEASED] = DMO_LAYOUT(k_tx_ceased_md, k_tx_ceased_sdu),
    [TETRA_DMO_MSG_TX_REQUEST] = DMO_LAYOUT_MD(k_tx_request_md),
    [TETRA_DMO_MSG_TX_ACCEPT] = DMO_LAYOUT_MD(k_tx_accept_md),
    [TETRA_DMO_MSG_PREEMPT] = DMO_LAYOUT_MD(k_preempt_md),
    [TETRA_DMO_MSG_PRE_ACCEPT] = DMO_LAYOUT_MD(k_pre_accept_md),
    [TETRA_DMO_MSG_REJECT] = DMO_LAYOUT_SDU(k_reject_sdu),
    [TETRA_DMO_MSG_INFO] = DMO_LAYOUT_SDU(k_info_sdu),
    [TETRA_DMO_MSG_SDS_UDATA] = DMO_LAYOUT(k_sds_md, k_sds_sdu),
    [TETRA_DMO_MSG_SDS_DATA] = DMO_LAYOUT(k_sds_md, k_sds_sdu),
    [TETRA_DMO_MSG_SDS_ACK] = DMO_LAYOUT(k_sds_ack_md, k_sds_ack_sdu),
};

/* By gateway message subtype, EN 300 396-5 clause 14.6.1. */
static const dmo_layout k_gw_layouts[TETRA_DMO_GW_CANCEL_ACK + 1] = {
    [TETRA_DMO_GW_SETUP] = DMO_LAYOUT(k_gsetup_md, k_gsetup_sdu),
    [TETRA_DMO_GW_CONNECT] = DMO_LAYOUT(k_gconnect_md, k_gconnect_sdu),
    [TETRA_DMO_GW_ACK] = DMO_LAYOUT(k_gack_md, k_gack_sdu),
    [TETRA_DMO_GW_RELEASE] = DMO_LAYOUT_SDU(k_grelease_sdu),
    [TETRA_DMO_GW_TX_REQUEST] = DMO_LAYOUT(k_gtx_request_md, k_gtx_request_sdu),
    [TETRA_DMO_GW_TX_ACCEPT] = DMO_LAYOUT(k_gconnect_md, k_gtx_accept_sdu),
    [TETRA_DMO_GW_PREEMPT] = DMO_LAYOUT(k_gpreempt_md, k_gtx_request_sdu),
    [TETRA_DMO_GW_PRE_ACCEPT] = DMO_LAYOUT(k_gconnect_md, k_gtx_accept_sdu),
    [TETRA_DMO_GW_REJECT] = DMO_LAYOUT_SDU(k_greject_sdu),
    [TETRA_DMO_GW_REG_REQUEST] = DMO_LAYOUT(k_greg_request_md, k_greg_request_sdu),
    [TETRA_DMO_GW_REG_ACCEPT] = DMO_LAYOUT(k_greg_answer_md, k_greg_accept_sdu),
    [TETRA_DMO_GW_REG_REJECT] = DMO_LAYOUT(k_greg_answer_md, k_greg_reject_sdu),
    [TETRA_DMO_GW_REG_CANCEL] = DMO_LAYOUT(k_greg_cancel_md, k_greg_reject_sdu),
    [TETRA_DMO_GW_CANCEL_ACK] = DMO_LAYOUT(k_gcancel_ack_md, k_greg_accept_sdu),
};

/* The element layout of `h`'s message, or NULL when it has none. A gateway message needs its subtype read first. */
static const dmo_layout*
dmo_layout_of(const tetra_dmo_sync_h_t* h) {
    const dmo_layout* lay = NULL;
    if (h->msg_type == TETRA_DMO_MSG_GATEWAY) {
        const uint32_t sub = h->e[TETRA_DMO_E_GW_SUBTYPE];
        lay = sub < DMO_COUNT(k_gw_layouts) ? &k_gw_layouts[sub] : NULL;
    } else if (h->msg_type < DMO_COUNT(k_msg_layouts)) {
        lay = &k_msg_layouts[h->msg_type];
    }
    return (lay != NULL && lay->n_md + lay->n_sdu > 0) ? lay : NULL;
}

/* ---- Element reader ------------------------------------------------------------------------------------------- */

static int
dmo_step_present(const dmo_step* st, const uint32_t* e) {
    if (st->cond == DMO_NONE) {
        return 1;
    }
    const uint32_t v = e[st->cond];
    return v < 16u && ((st->mask >> v) & 1u) != 0u;
}

/* External subscriber number (EN 300 396-5 clause 14.7.6): a digit count of at most 24, then 4 bits a digit. */
static int
dmo_read_external(tetra_bit_reader* r, tetra_dmo_sync_h_t* h) {
    static const char k_digits[] = "0123456789*#+???";
    const uint32_t n = tetra_bits_read(r, 5);
    if (!tetra_bits_ok(r) || n > 24u) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t d = tetra_bits_read(r, 4);
        if (!tetra_bits_ok(r)) {
            return 0;
        }
        h->external[i] = k_digits[d & 15u];
        h->external[i + 1u] = '\0';
        h->external_n = (uint8_t)(i + 1u);
    }
    return 1;
}

/* The user data an SDTI announces (EN 300 396-3 clauses 9.7.12 and 9.7.16 to 9.7.20): 16, 32 or 64 bits, an 11-bit
 * length indicator and that many bits (the first 64 kept), or a 16-bit pre-coded status. A reserved SDTI has no known
 * length, so nothing after it can be read. */
static int
dmo_read_sds_data(tetra_bit_reader* r, tetra_dmo_sync_h_t* h) {
    static const uint8_t k_width[5] = {16u, 32u, 64u, 0u, 16u};
    const uint32_t sdti = h->e[TETRA_DMO_E_SDTI];
    uint32_t width;
    if (sdti >= DMO_COUNT(k_width)) {
        return 0;
    }
    width = sdti == 3u ? tetra_bits_read(r, 11) : k_width[sdti];
    if (!tetra_bits_ok(r)) {
        return 0;
    }
    h->e[TETRA_DMO_E_SDS_LENGTH] = width;
    const uint32_t keep = width > 64u ? 64u : width;
    h->sds_value = tetra_bits_read64(r, (int)keep);
    return tetra_bits_skip(r, (int)(width - keep));
}

static int
dmo_step_read(tetra_bit_reader* r, const dmo_step* st, tetra_dmo_sync_h_t* h) {
    if (st->kind == DMO_STEP_EXTERNAL) {
        return dmo_read_external(r, h);
    }
    if (st->kind == DMO_STEP_SDS_DATA) {
        return dmo_read_sds_data(r, h);
    }
    if (st->elem == DMO_NONE) {
        return tetra_bits_skip(r, st->width);
    }
    const uint32_t v = tetra_bits_read(r, st->width);
    if (!tetra_bits_ok(r)) {
        return 0;
    }
    h->e[st->elem] = v;
    return 1;
}

/* Read the rows of one table. Returns 0 when the block ended inside an element; the elements before it are kept. */
static int
dmo_run(tetra_bit_reader* r, const dmo_step* steps, size_t n, tetra_dmo_sync_h_t* h) {
    for (size_t i = 0; i < n; i++) {
        if (dmo_step_present(&steps[i], h->e) && !dmo_step_read(r, &steps[i], h)) {
            return 0;
        }
    }
    return 1;
}

/* Test the DM-SDU elements read from bit `sdu_start` of `r` against the FCS a set FCS flag puts after them (EN 300
 * 396-3 clauses 6.3.4 and 9.7.10): the FCS of annex B, the same 32-bit CRC as the LLC FCS of EN 300 392-2 annex C. The
 * FCS ends the DM-SDU of every PDU that has one (tables 45 to 47), so it is the last 32 bits read. A DM-SDU the block
 * ended in has no FCS to test against. */
static void
dmo_check_fcs(const tetra_bit_reader* r, int sdu_start, tetra_dmo_sync_h_t* h) {
    if (!h->e[TETRA_DMO_E_FCS_FLAG]) {
        h->fcs_state = TETRA_DMO_FCS_NONE;
        return;
    }
    const int fcs_at = r->pos - 32;
    if (h->sdu_state != TETRA_DMO_SDU_CLEAR || fcs_at < sdu_start) {
        h->fcs_state = TETRA_DMO_FCS_FAILED;
        return;
    }
    const uint32_t expected = tetra_llc_fcs32(r->bits + sdu_start, fcs_at - sdu_start);
    h->fcs_state = (uint8_t)(expected == h->e[TETRA_DMO_E_FCS] ? TETRA_DMO_FCS_PASSED : TETRA_DMO_FCS_FAILED);
}

/* The digest of the h->sdu_bits DM-SDU bits read from bit `sdu_start` of `r` (tetra_dmo_sync_h_t::sdu_digest). An
 * FCS that ends a whole DM-SDU is left out: the CRC of annex B over data followed by its own FCS is the CRC's residue,
 * the same for every message whose FCS is right, and the elements before it say all the FCS does. */
static void
dmo_digest_sdu(const tetra_bit_reader* r, int sdu_start, tetra_dmo_sync_h_t* h) {
    int bits = (int)h->sdu_bits;
    if (h->e[TETRA_DMO_E_FCS_FLAG] && h->sdu_state == TETRA_DMO_SDU_CLEAR && bits >= 32) {
        bits -= 32;
    }
    h->sdu_digest = bits > 0 ? tetra_llc_fcs32(r->bits + sdu_start, bits) : 0u;
}

/* The elements after the message type of a PDU read under air interface encryption state `enc`. */
static void
dmo_parse_elems(tetra_bit_reader* r, uint8_t enc, tetra_dmo_sync_h_t* h) {
    if (h->msg_type == TETRA_DMO_MSG_GATEWAY) {
        h->e[TETRA_DMO_E_GW_SUBTYPE] = tetra_bits_read(r, 5);
        if (!tetra_bits_ok(r)) {
            return;
        }
    }
    const dmo_layout* lay = dmo_layout_of(h);
    if (lay == NULL || !dmo_run(r, lay->md, lay->n_md, h)) {
        return;
    }
    h->md_ok = 1;
    if (lay->n_sdu == 0u) {
        return;
    }
    if (enc != TETRA_DMO_AIE_DM1) {
        h->sdu_state = TETRA_DMO_SDU_ENCRYPTED;
        h->fcs_state = (uint8_t)(h->e[TETRA_DMO_E_FCS_FLAG] ? TETRA_DMO_FCS_ENCRYPTED : TETRA_DMO_FCS_NONE);
        return;
    }
    const int sdu_start = r->pos;
    if (!dmo_run(r, lay->sdu, lay->n_sdu, h)) {
        h->sdu_state = TETRA_DMO_SDU_TRUNCATED;
    }
    /* The reader stops at the element the block ended in, so a truncated DM-SDU digests the elements before it. */
    h->sdu_bits = (uint16_t)(r->pos - sdu_start);
    dmo_digest_sdu(r, sdu_start, h);
    dmo_check_fcs(r, sdu_start, h);
}

/* Addresses and message type (EN 300 396-3 tables 22 and 23), then the elements. The MNI is always present for direct
 * and DM-REP operation; through a gateway only when either address is a true SSI (EN 300 396-5 clauses 14.1.1 and
 * 14.2). */
static void
dmo_read_addressed(tetra_bit_reader* r, int comm_type, uint8_t enc, tetra_dmo_sync_h_t* h) {
    h->dst_type = (uint8_t)tetra_bits_read(r, 2);
    if (h->dst_type != 2u) {
        h->dst = tetra_bits_read(r, 24);
    }
    h->src_type = (uint8_t)tetra_bits_read(r, 2);
    if (h->src_type != 2u) {
        h->src = tetra_bits_read(r, 24);
    }
    if ((comm_type != 2 && comm_type != 3) || h->dst_type == 0u || h->src_type == 0u) {
        h->mni = tetra_bits_read(r, 24);
    }
    h->msg_type = (uint8_t)tetra_bits_read(r, 5);
    if (!tetra_bits_ok(r)) {
        return;
    }
    h->has_addrs = 1;
    dmo_parse_elems(r, enc, h);
}

static void
dmo_hide_dst(tetra_dmo_sync_h_t* h) {
    h->dst = 0;
    h->dst_encrypted = (uint8_t)(h->dst_type != 2u);
}

static void
dmo_hide_sdu(tetra_dmo_sync_h_t* h) {
    const dmo_layout* lay = dmo_layout_of(h);
    DSD_MEMSET(&h->e[TETRA_DMO_E_SDU_FIRST], 0, sizeof(h->e[0]) * (size_t)(TETRA_DMO_E_COUNT - TETRA_DMO_E_SDU_FIRST));
    DSD_MEMSET(h->external, 0, sizeof(h->external));
    h->external_n = 0;
    h->sds_value = 0;
    h->sdu_digest = 0;
    h->sdu_bits = 0;
    h->sdu_state = (lay != NULL && lay->n_sdu > 0u && h->md_ok) ? TETRA_DMO_SDU_ENCRYPTED : TETRA_DMO_SDU_CLEAR;
    h->fcs_state =
        (uint8_t)((h->sdu_state == TETRA_DMO_SDU_ENCRYPTED && h->e[TETRA_DMO_E_FCS_FLAG]) ? TETRA_DMO_FCS_ENCRYPTED
                                                                                          : TETRA_DMO_FCS_NONE);
}

/* Clear what security class `enc` encrypts in a DMAC-SYNC SCH/H that was read as if it were clear (EN 300 396-3
 * clauses 8.5.3 and 9.3.2). DM-2-C leaves only the header up to the address types in the clear. */
static void
dmo_settle(tetra_dmo_sync_h_t* h, uint8_t enc) {
    h->enc_state = enc;
    if (enc == TETRA_DMO_AIE_DM1) {
        return;
    }
    dmo_hide_sdu(h);
    if (enc != TETRA_DMO_AIE_DM2A) {
        dmo_hide_dst(h);
    }
    if (enc == TETRA_DMO_AIE_DM2C) {
        DSD_MEMSET(h->e, 0, sizeof(h->e));
        h->src = 0;
        h->mni = 0;
        h->msg_type = 0;
        h->has_addrs = 0;
        h->md_ok = 0;
        h->sdu_state = TETRA_DMO_SDU_ENCRYPTED;
        h->fcs_state = TETRA_DMO_FCS_NONE; /* the FCS flag is encrypted too */
    }
}

/* ---- DSB and DNB parsers -------------------------------------------------------------------------------------- */

int
tetra_dmo_system_code_is_dmo(const uint8_t* sch_s_bits) {
    tetra_bit_reader r = tetra_bits_reader(sch_s_bits, DMO_SCH_S_BITS, 0);
    /* 0xxx TMO, 100y reserved, 1010..1111 DMO (EN 300 396-3 clause 9.3.29). */
    const uint32_t code = tetra_bits_read(&r, 4);
    return tetra_bits_ok(&r) && code >= 0xAu;
}

static uint8_t
dmo_frame_number(uint32_t v) {
    return (uint8_t)((v >= 1u && v <= 18u) ? v : 0u);
}

/* DMAC-SYNC SCH/S after the communication type, EN 300 396-3 table 21. */
static void
dmo_read_dmac_sync_s(tetra_bit_reader* r, tetra_dmo_sync_s_t* s) {
    /* Master/slave link flag (via DM-REP or DM-REP/GATE) or reserved, then the gateway generated message flag (via
     * DM-GATE or DM-REP/GATE) or reserved. */
    const uint32_t link = tetra_bits_read(r, 1);
    const uint32_t gateway = tetra_bits_read(r, 1);
    s->ms_link_flag = (uint8_t)((s->comm_type & 1u) ? link : 0u);
    s->gw_flag = (uint8_t)((s->comm_type & 2u) ? gateway : 0u);
    s->ab_usage = (uint8_t)tetra_bits_read(r, 2);
    s->slot = (uint8_t)(tetra_bits_read(r, 2) + 1u);
    s->frame = dmo_frame_number(tetra_bits_read(r, 5));
    s->enc_state = (uint8_t)tetra_bits_read(r, 2);
    if (s->enc_state != TETRA_DMO_AIE_DM1) {
        s->tvp = tetra_bits_read(r, 29);
        (void)tetra_bits_skip(r, 1);
        s->ksg = (uint8_t)tetra_bits_read(r, 4);
        s->key_number = (uint8_t)tetra_bits_read(r, 5);
    }
}

/* DPRES-SYNC SCH/S after the communication type (EN 300 396-4 table 2, EN 300 396-5 table 5). Each conditional element
 * of these tables shares its width with the reserved element that stands in for it, so every field has a fixed place;
 * what it means is settled from the flags afterwards. */
static void
dmo_read_pres_s(tetra_bit_reader* r, tetra_dmo_sync_s_t* s, tetra_dmo_pres_t* p) {
    const int gateway = s->comm_type >= 2u;
    p->comm_type = s->comm_type;
    p->mdmo = (uint8_t)tetra_bits_read(r, 1);
    const uint32_t gw_flags = tetra_bits_read(r, 2); /* SwMI availability and DM-REP function flags, or reserved */
    const uint32_t two_freq = tetra_bits_read(r, 1); /* two-frequency repeater flag, or reserved */
    (void)tetra_bits_skip(r, 2);                     /* repeater operating modes, or reserved */
    const uint32_t spacing = tetra_bits_read(r, 5);  /* spacing of uplink: duplex spacing, then normal/reverse */
    const uint32_t reverse = tetra_bits_read(r, 1);
    p->ms_link = (uint8_t)tetra_bits_read(r, 1);
    p->channel_usage = (uint8_t)tetra_bits_read(r, 2);
    p->channel_state = (uint8_t)tetra_bits_read(r, 2);
    s->slot = (uint8_t)(tetra_bits_read(r, 2) + 1u);
    s->frame = dmo_frame_number(tetra_bits_read(r, 5));
    p->power_class = (uint8_t)tetra_bits_read(r, 3);
    (void)tetra_bits_skip(r, 4);                     /* power control, reserved or registration phase, countdown */
    const uint32_t priority = tetra_bits_read(r, 2); /* priority level, or reserved, or timing for DM-REP function */
    if (gateway) {
        (void)tetra_bits_skip(r, 2); /* registrations permitted */
        p->reg_label = (uint8_t)tetra_bits_read(r, 4);
        (void)tetra_bits_skip(r, 8); /* registration phase time or DN232/DN233; access parameter or DT264; forwarded */
        p->gw_enc = (uint8_t)tetra_bits_read(r, 1);
    }
    p->swmi_up = (uint8_t)(gateway ? (gw_flags >> 1) : 0u);
    p->dmrep = (uint8_t)(gateway ? (gw_flags & 1u) : 0u);
    p->two_freq = (uint8_t)((!gateway || s->comm_type == 3u || p->dmrep) ? two_freq : 0u);
    p->spacing = (uint8_t)(p->two_freq ? spacing : 0u);
    p->reverse = (uint8_t)(p->two_freq ? reverse : 0u);
    p->priority = (uint8_t)(p->channel_state != 0u ? priority : 0u);
    s->ms_link_flag = p->ms_link;
    s->ab_usage = p->channel_usage;
}

int
tetra_dmo_parse_sync_s(const uint8_t* bits, tetra_dmo_sync_s_t* out) {
    if (!bits || !out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    tetra_bit_reader r = tetra_bits_reader(bits, DMO_SCH_S_BITS, 0);
    out->system_code = (uint8_t)tetra_bits_read(&r, 4);
    out->sync_pdu_type = (uint8_t)tetra_bits_read(&r, 2);
    out->comm_type = (uint8_t)tetra_bits_read(&r, 2);
    if (out->sync_pdu_type == TETRA_DMO_SYNC_DMAC) {
        dmo_read_dmac_sync_s(&r, out);
        out->parsed = (uint8_t)tetra_bits_ok(&r);
    } else if (out->sync_pdu_type == TETRA_DMO_SYNC_PRES && out->comm_type != 0u) {
        /* Communication type 00 is managed direct mode (EN 300 396-10), which is not read. */
        tetra_dmo_pres_t pres;
        DSD_MEMSET(&pres, 0, sizeof(pres));
        dmo_read_pres_s(&r, out, &pres);
        out->parsed = (uint8_t)tetra_bits_ok(&r);
    }
    return out->parsed;
}

/* DPRES-SYNC SCH/H (EN 300 396-4 table 3, EN 300 396-5 table 6) up to the usage restriction type. */
static void
dmo_read_pres_h(tetra_bit_reader* r, tetra_dmo_pres_t* p) {
    p->repeater = (uint16_t)tetra_bits_read(r, 10);
    p->mni = tetra_bits_read(r, 24);
    (void)tetra_bits_skip(r, 12); /* validity time unit and units, maximum DM-MS power class, reserved */
    p->urt = (uint8_t)tetra_bits_read(r, 4);
}

int
tetra_dmo_parse_presence(const uint8_t* sch_s, const uint8_t* sch_h, tetra_dmo_sync_s_t* s, tetra_dmo_pres_t* out) {
    if (!sch_s || !s || !out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (s->sync_pdu_type != TETRA_DMO_SYNC_PRES || s->comm_type == 0u) {
        return 0;
    }
    tetra_bit_reader r = tetra_bits_reader(sch_s, DMO_SCH_S_BITS, 8);
    dmo_read_pres_s(&r, s, out);
    if (!tetra_bits_ok(&r)) {
        return 0;
    }
    s->parsed = 1;
    if (sch_h) {
        tetra_bit_reader rh = tetra_bits_reader(sch_h, DMO_SCH_H_BITS, 0);
        dmo_read_pres_h(&rh, out);
        out->parsed = (uint8_t)tetra_bits_ok(&rh);
    }
    return 1;
}

int
tetra_dmo_parse_sync_h(const uint8_t* bits, int comm_type, tetra_dmo_sync_h_t* out) {
    if (!bits || !out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    tetra_bit_reader r = tetra_bits_reader(bits, DMO_SCH_H_BITS, 0);
    /* Repeater address (via DM-REP), gateway address (via a gateway) or reserved: 10 bits in every case. */
    out->relay_addr = (uint16_t)tetra_bits_read(&r, 10);
    out->has_relay = (uint8_t)(comm_type != 0);
    out->fill_bit = (uint8_t)tetra_bits_read(&r, 1);
    out->frag_flag = (uint8_t)tetra_bits_read(&r, 1);
    if (out->frag_flag) {
        out->num_schf = (uint8_t)tetra_bits_read(&r, 4);
    }
    out->frame_countdown = (uint8_t)tetra_bits_read(&r, 2);
    dmo_read_addressed(&r, comm_type, TETRA_DMO_AIE_DM1, out);
    return 1;
}

int
tetra_dmo_parse_data(const uint8_t* bits, int len, int comm_type, tetra_dmo_sync_h_t* out) {
    if (!bits || !out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    tetra_bit_reader r = tetra_bits_reader(bits, len, 0);
    /* EN 300 396-3 table 23: MAC PDU type 00 is DMAC-DATA. */
    if (tetra_bits_read(&r, 2) != 0u || !tetra_bits_ok(&r)) {
        return 0;
    }
    out->fill_bit = (uint8_t)tetra_bits_read(&r, 1);
    out->stolen2 = (uint8_t)tetra_bits_read(&r, 1);
    out->frag_flag = (uint8_t)tetra_bits_read(&r, 1);
    out->null_pdu = (uint8_t)tetra_bits_read(&r, 1);
    if (out->null_pdu) {
        return 1;
    }
    out->frame_countdown = (uint8_t)tetra_bits_read(&r, 2);
    out->enc_state = (uint8_t)tetra_bits_read(&r, 2);
    if (out->enc_state == TETRA_DMO_AIE_DM2C) {
        /* DM-2-C encrypts a DMAC-DATA PDU from the destination address type on (clause 9.3.2). */
        out->sdu_state = TETRA_DMO_SDU_ENCRYPTED;
        return 1;
    }
    dmo_read_addressed(&r, comm_type, out->enc_state, out);
    if (out->enc_state == TETRA_DMO_AIE_DM2B) {
        dmo_hide_dst(out);
    }
    return 1;
}

/* ---- Text ----------------------------------------------------------------------------------------------------- */

typedef struct {
    char* buf;
    size_t cap;
    size_t len;
} dmo_text;

static void dmo_put(dmo_text* t, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 2, 3);

/* Append to `t`, truncating at its capacity. */
static void
dmo_put(dmo_text* t, const char* fmt, ...) {
    if (t->len + 1u >= t->cap) {
        return;
    }
    const size_t room = t->cap - t->len;
    va_list ap;
    va_start(ap, fmt);
    const int n = DSD_VSNPRINTF(t->buf + t->len, room, fmt, ap);
    va_end(ap);
    if (n > 0) {
        t->len += (size_t)n < room ? (size_t)n : room - 1u;
    }
}

/* names[v], or "reserved" for a value the table does not name. */
static const char*
dmo_name(const char* const* names, size_t n, uint32_t v) {
    return (v < n && names[v] != NULL) ? names[v] : "reserved";
}

#define DMO_NAME(names, v) dmo_name((names), DMO_COUNT(names), (v))

static const char* const k_priority[] = {"normal", "high", "pre-emptive", "emergency"};

/* Circuit mode type, clause 9.6.3. */
static const char*
dmo_circuit_name(uint32_t mode) {
    if (mode == 0u) {
        return "speech";
    }
    if (mode == 8u) {
        return "prop-speech";
    }
    return mode <= 7u ? "data" : "reserved";
}

static const char*
dmo_call_kind(const tetra_dmo_sync_h_t* h) {
    if (h->sdu_state == TETRA_DMO_SDU_ENCRYPTED) {
        return "call";
    }
    return h->e[TETRA_DMO_E_CALL_TYPE] ? "group" : "individual";
}

static int
dmo_sdu_clear(const tetra_dmo_sync_h_t* h) {
    return h->sdu_state != TETRA_DMO_SDU_ENCRYPTED;
}

/* Timing adjustment, clause 9.6.19: the delay in units of 5 symbols, 1 to 3671. */
static void
dmo_put_adjust(dmo_text* t, uint32_t adjust) {
    if (adjust >= 1u && adjust <= 3671u) {
        dmo_put(t, " timing=+%u symbols", (adjust * 5u));
    } else {
        dmo_put(t, " timing=reserved");
    }
}

static void
dmo_put_timing(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    if (h->e[TETRA_DMO_E_TIMING_CHANGE]) {
        dmo_put_adjust(t, h->e[TETRA_DMO_E_TIMING_ADJUST]);
    }
}

static void
dmo_put_calling(const tetra_dmo_sync_h_t* h, dmo_text* t, int present) {
    if (present) {
        const uint32_t mni = h->e[TETRA_DMO_E_CALLING_MNI];
        dmo_put(t, " calling=%u/%u ssi=%u", ((mni >> 14) & 0x3FFu), (mni & 0x3FFFu),
                (unsigned)h->e[TETRA_DMO_E_CALLING_SSI]);
    }
    if (h->external_n) {
        dmo_put(t, " ext=%s", h->external);
    }
}

static void
dmo_describe_call(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "%s %s prio=%s pwr=%u", dmo_call_kind(h), dmo_circuit_name(h->e[TETRA_DMO_E_CIRCUIT_MODE]),
            DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]), (unsigned)h->e[TETRA_DMO_E_POWER_CLASS]);
    if (h->e[TETRA_DMO_E_E2EE]) {
        dmo_put(t, " E2EE");
    }
    if (h->e[TETRA_DMO_E_PREEMPT_FLAG]) {
        dmo_put(t, " preempt");
    }
}

static void
dmo_describe_reserved(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "reserved %u frames prio=%s pwr=%u", (h->e[TETRA_DMO_E_RESERVATION] * 6u),
            DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]), (unsigned)h->e[TETRA_DMO_E_POWER_CLASS]);
}

static void
dmo_describe_sds_occupied(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "SDS time remaining=%u prio=%s", (unsigned)h->e[TETRA_DMO_E_SDS_REMAINING],
            DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]));
}

static void
dmo_describe_timing_req(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "timing request");
    dmo_put_adjust(t, h->e[TETRA_DMO_E_TIMING_ADJUST]);
}

static void
dmo_describe_timing_ack(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "timing %s", h->e[TETRA_DMO_E_TIMING_ACCEPT] ? "accepted" : "rejected");
    dmo_put_timing(h, t);
}

static void
dmo_describe_connect(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "accept %s", dmo_circuit_name(h->e[TETRA_DMO_E_CIRCUIT_MODE]));
}

static void
dmo_describe_disconnect(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    /* Clause 9.7.7. */
    static const char* const k_cause[] = {
        "unknown",     "no E2EE support", "service unsupported", "rejected by called party",
        "SCK failure", "poor quality",    "E2EE required",       "security mismatch",
    };
    dmo_put(t, "disconnect");
    if (dmo_sdu_clear(h)) {
        dmo_put(t, " %s", DMO_NAME(k_cause, h->e[TETRA_DMO_E_DISCONNECT_CAUSE]));
    }
}

static void
dmo_describe_release(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    const uint32_t cause = h->e[TETRA_DMO_E_RELEASE_CAUSE];
    dmo_put(t, "release");
    if (!dmo_sdu_clear(h)) {
        return;
    }
    /* Clauses 9.7.15 and 9.7.15A. */
    if (cause == 15u && h->e[TETRA_DMO_E_RELEASE_EXT] == 0u) {
        dmo_put(t, " occupied by other users");
    } else if (cause == 15u) {
        dmo_put(t, " extension=%u", (unsigned)h->e[TETRA_DMO_E_RELEASE_EXT]);
    } else {
        dmo_put(t, " cause=%u", (unsigned)cause);
    }
}

static void
dmo_describe_tx_ceased(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    /* Clause 9.7.6 and EN 300 396-5 table 26. */
    static const char* const k_cause[] = {
        "unknown",    "normal end",        "pre-emption",
        "time limit", "TMO interruption",  "TMO wait",
        "TMO break",  "TMO timing change", "TMO call timer expired",
    };
    dmo_put(t, "ceased reserve=%u frames prio=%s", (h->e[TETRA_DMO_E_RESERVATION] * 6u),
            DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]));
    if (dmo_sdu_clear(h)) {
        dmo_put(t, " cause=%s", DMO_NAME(k_cause, h->e[TETRA_DMO_E_CEASE_CAUSE]));
    }
}

static void
dmo_describe_tx_request(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "changeover prio=%s", DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]));
    dmo_put_timing(h, t);
}

static void
dmo_describe_tx_accept(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "changeover accepted");
    dmo_put_timing(h, t);
}

/* Type of pre-emption, clause 9.6.23 and EN 300 396-5 table 24. */
static const char* const k_preempt_type[] = {
    "circuit",   "short data", "late gateway", "TMO interrupt",       "TMO wait",
    "TMO break", "TMO timing", "TMO timer",    "gateway pre-emption",
};

static void
dmo_describe_preempt(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    /* Perceived channel state, clause 9.6.7 and EN 300 396-5 table 23. */
    static const char* const k_seen[] = {
        "circuit occupation",
        "reservation",
        "short data occupation",
        "registration phase",
    };
    dmo_put(t, "preempt %s %s %s prio=%s", h->e[TETRA_DMO_E_NEW_CALL] ? "new-call" : "ongoing",
            DMO_NAME(k_seen, h->e[TETRA_DMO_E_CHANNEL_STATE]), DMO_NAME(k_preempt_type, h->e[TETRA_DMO_E_PREEMPT_TYPE]),
            DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]));
    dmo_put_timing(h, t);
}

static void
dmo_describe_pre_accept(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "preempt accepted %s %s", h->e[TETRA_DMO_E_NEW_CALL] ? "new-call" : "ongoing",
            DMO_NAME(k_preempt_type, h->e[TETRA_DMO_E_PREEMPT_TYPE]));
    dmo_put_timing(h, t);
}

static void
dmo_describe_reject(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    /* Clause 9.7.14 and EN 300 396-5 table 28. */
    static const char* const k_cause[] = {
        "unknown",
        "invalid",
        "master will transmit",
        "no short data",
        "gateway no short data",
        "rejected by gateway",
        "security mismatch",
    };
    dmo_put(t, "reject");
    if (dmo_sdu_clear(h)) {
        dmo_put(t, " %s", DMO_NAME(k_cause, h->e[TETRA_DMO_E_REJECT_CAUSE]));
    }
}

static void
dmo_describe_info(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    const uint32_t type = h->e[TETRA_DMO_E_INFO_TYPE];
    dmo_put(t, "info");
    if (dmo_sdu_clear(h)) {
        dmo_put_calling(h, t, type == 0u || type == 2u);
    }
}

static void
dmo_describe_sds(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    const uint32_t sdti = h->e[TETRA_DMO_E_SDTI];
    const char* in_call = h->e[TETRA_DMO_E_SDS_TX_TYPE] ? " in-call" : "";
    if (!dmo_sdu_clear(h)) {
        dmo_put(t, "SDS prio=%s%s", DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]), in_call);
        return;
    }
    if (sdti == 4u) {
        dmo_put(t, "SDS status=0x%04x", (unsigned)(h->sds_value & 0xFFFFu));
    } else {
        dmo_put(t, "SDS data sdti=%u len=%u", (unsigned)sdti, (unsigned)h->e[TETRA_DMO_E_SDS_LENGTH]);
    }
    dmo_put(t, " prio=%s%s", DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]), in_call);
    dmo_put_calling(h, t, (DMO_ADDR_TYPE_TSI >> (h->e[TETRA_DMO_E_ADDR_TYPE] & 15u)) & 1u);
    if (h->e[TETRA_DMO_E_FCS_FLAG] && h->sdu_state == TETRA_DMO_SDU_CLEAR) {
        dmo_put(t, " fcs=0x%08x", (unsigned)h->e[TETRA_DMO_E_FCS]);
    }
}

static void
dmo_describe_sds_ack(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    /* Acknowledgement type, clause 9.7.1. */
    static const char* const k_ack[] = {"full", "full+data", "fcs failed", "incomplete"};
    dmo_put(t, "SDS ack");
    if (dmo_sdu_clear(h)) {
        dmo_put(t, " %s", DMO_NAME(k_ack, h->e[TETRA_DMO_E_ACK_TYPE]));
    }
}

/* ---- Gateway-specific messages, EN 300 396-5 clause 14.5 ------------------------------------------------------ */

static const char* const k_gw_names[] = {
    "DM-GSETUP",
    "DM-GCONNECT",
    "DM-GACK",
    "DM-GRELEASE",
    "DM-GTX REQUEST",
    "DM-GTX ACCEPT",
    "DM-GPREEMPT",
    "DM-GPRE ACCEPT",
    "DM-GREJECT",
    "DM-GREGISTER REQUEST",
    "DM-GREGISTER ACCEPT",
    "DM-GREGISTER REJECT",
    "DM-GREGISTER CANCEL",
    "DM-GCANCEL ACK",
};

static void
dmo_describe_gsetup(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, " %s %s prio=%s", dmo_call_kind(h), dmo_circuit_name(h->e[TETRA_DMO_E_CIRCUIT_MODE]),
            DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]));
    if (!dmo_sdu_clear(h)) {
        return;
    }
    const uint32_t mni = h->e[TETRA_DMO_E_CALLED_MNI];
    const uint32_t area = h->e[TETRA_DMO_E_AREA];
    dmo_put(t, " label=%u called=%u", (unsigned)h->e[TETRA_DMO_E_REQUEST_LABEL],
            (unsigned)h->e[TETRA_DMO_E_CALLED_SSI]);
    if (h->e[TETRA_DMO_E_CALLED_TSI_FLAG]) {
        dmo_put(t, " MNI=%u/%u", ((mni >> 14) & 0x3FFu), (mni & 0x3FFFu));
    }
    /* Area selection, EN 300 396-5 clause 14.7.1. */
    if (area == 0u) {
        dmo_put(t, " area=unset");
    } else if (area == 15u) {
        dmo_put(t, " area=all");
    } else {
        dmo_put(t, " area=%u", (unsigned)area);
    }
    dmo_put_calling(h, t, (DMO_ADDR_TYPE_TSI >> (h->e[TETRA_DMO_E_ADDR_TYPE] & 15u)) & 1u);
}

static void
dmo_describe_gconnect(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, " %s %s pwr=%u", dmo_call_kind(h), dmo_circuit_name(h->e[TETRA_DMO_E_CIRCUIT_MODE]),
            (unsigned)h->e[TETRA_DMO_E_POWER_CLASS]);
    if (h->e[TETRA_DMO_E_GW_SUBTYPE] == TETRA_DMO_GW_CONNECT && dmo_sdu_clear(h)) {
        dmo_put(t, " label=%u", (unsigned)h->e[TETRA_DMO_E_REQUEST_LABEL]);
    }
    if (h->e[TETRA_DMO_E_GW_ENC]) {
        dmo_put(t, " SwMI-enc");
    }
}

static void
dmo_describe_gack(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    /* Value of DM-MS waiting timer, EN 300 396-5 clause 14.7.15. */
    static const char* const k_wait[] = {"predefined", "1s", "2s", "5s", "10s", "20s", "30s", "60s"};
    if (dmo_sdu_clear(h)) {
        dmo_put(t, " %s wait=%s", h->e[TETRA_DMO_E_GACK_TYPE] == 1u ? "incomplete" : "full",
                DMO_NAME(k_wait, h->e[TETRA_DMO_E_WAIT_CODE]));
    }
    dmo_put_timing(h, t);
    if (h->e[TETRA_DMO_E_RESERVATION_ANNOUNCED]) {
        dmo_put(t, " reserve=%u frames prio=%s", (h->e[TETRA_DMO_E_RESERVATION] * 6u),
                DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]));
    }
}

static void
dmo_describe_grelease(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    /* Gateway release cause, EN 300 396-5 clause 14.7.8. */
    static const char* const k_cause[] = {"unknown", "TMO resource lost", "TMO call timer expired"};
    if (dmo_sdu_clear(h)) {
        dmo_put(t, " %s", DMO_NAME(k_cause, h->e[TETRA_DMO_E_RELEASE_CAUSE]));
    }
}

static void
dmo_describe_greject(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    /* Gateway reject cause, EN 300 396-5 clause 14.7.7. */
    static const char* const k_cause[] = {
        "unknown", "invalid", "rejected by gateway", "rejected by SwMI", "TMO transmission failure",
    };
    if (dmo_sdu_clear(h)) {
        dmo_put(t, " %s", DMO_NAME(k_cause, h->e[TETRA_DMO_E_REJECT_CAUSE]));
    }
}

static void
dmo_describe_gtx_request(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    dmo_put(t, "%s %s %s prio=%s", h->e[TETRA_DMO_E_GTX_TYPE] ? " after-preempt" : "", dmo_call_kind(h),
            dmo_circuit_name(h->e[TETRA_DMO_E_CIRCUIT_MODE]), DMO_NAME(k_priority, h->e[TETRA_DMO_E_PRIORITY]));
    if (dmo_sdu_clear(h)) {
        dmo_put_calling(h, t, h->e[TETRA_DMO_E_CALLING_TSI_FLAG] != 0u);
    }
}

static void
dmo_describe_greg(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    /* Registration reject cause, EN 300 396-5 clause 14.7.10. */
    static const char* const k_cause[] = {
        "unknown",           "ITSI unknown", "no SwMI response", "attachment rejected", "rejected by gateway",
        "group unsupported",
    };
    const uint32_t sub = h->e[TETRA_DMO_E_GW_SUBTYPE];
    if (sub == TETRA_DMO_GW_REG_REQUEST || sub == TETRA_DMO_GW_REG_CANCEL) {
        dmo_put(t, " time=%u frames", (unsigned)h->e[TETRA_DMO_E_REG_TIME]);
    }
    if (sub != TETRA_DMO_GW_REG_CANCEL && sub != TETRA_DMO_GW_CANCEL_ACK) {
        dmo_put(t, " %s", h->e[TETRA_DMO_E_REG_TYPE] ? "unsolicited" : "solicited");
    }
    if (!dmo_sdu_clear(h)) {
        return;
    }
    if (sub == TETRA_DMO_GW_REG_REQUEST) {
        dmo_put_calling(h, t, h->e[TETRA_DMO_E_CALLING_TSI_FLAG] != 0u);
        if (h->e[TETRA_DMO_E_GSSI_FLAG]) {
            dmo_put(t, " gssi=%u", (unsigned)h->e[TETRA_DMO_E_GSSI]);
        }
        return;
    }
    dmo_put(t, " label=%u", (unsigned)h->e[TETRA_DMO_E_REG_LABEL]);
    if (sub == TETRA_DMO_GW_REG_REJECT || sub == TETRA_DMO_GW_REG_CANCEL) {
        dmo_put(t, " %s", DMO_NAME(k_cause, h->e[TETRA_DMO_E_REJECT_CAUSE]));
    }
}

typedef void (*dmo_describer)(const tetra_dmo_sync_h_t* h, dmo_text* t);

static const dmo_describer k_gw_describers[TETRA_DMO_GW_CANCEL_ACK + 1] = {
    [TETRA_DMO_GW_SETUP] = dmo_describe_gsetup,
    [TETRA_DMO_GW_CONNECT] = dmo_describe_gconnect,
    [TETRA_DMO_GW_ACK] = dmo_describe_gack,
    [TETRA_DMO_GW_RELEASE] = dmo_describe_grelease,
    [TETRA_DMO_GW_TX_REQUEST] = dmo_describe_gtx_request,
    [TETRA_DMO_GW_TX_ACCEPT] = dmo_describe_gconnect,
    [TETRA_DMO_GW_PREEMPT] = dmo_describe_gtx_request,
    [TETRA_DMO_GW_PRE_ACCEPT] = dmo_describe_gconnect,
    [TETRA_DMO_GW_REJECT] = dmo_describe_greject,
    [TETRA_DMO_GW_REG_REQUEST] = dmo_describe_greg,
    [TETRA_DMO_GW_REG_ACCEPT] = dmo_describe_greg,
    [TETRA_DMO_GW_REG_REJECT] = dmo_describe_greg,
    [TETRA_DMO_GW_REG_CANCEL] = dmo_describe_greg,
    [TETRA_DMO_GW_CANCEL_ACK] = dmo_describe_greg,
};

static void
dmo_describe_gateway(const tetra_dmo_sync_h_t* h, dmo_text* t) {
    const uint32_t sub = h->e[TETRA_DMO_E_GW_SUBTYPE];
    dmo_put(t, "%s", DMO_NAME(k_gw_names, sub));
    if (sub < DMO_COUNT(k_gw_describers) && k_gw_describers[sub] != NULL) {
        k_gw_describers[sub](h, t);
    }
}

static const dmo_describer k_describers[32] = {
    [TETRA_DMO_MSG_RESERVED] = dmo_describe_reserved,
    [TETRA_DMO_MSG_SDS_OCCUPIED] = dmo_describe_sds_occupied,
    [TETRA_DMO_MSG_TIMING_REQ] = dmo_describe_timing_req,
    [TETRA_DMO_MSG_TIMING_ACK] = dmo_describe_timing_ack,
    [TETRA_DMO_MSG_SETUP] = dmo_describe_call,
    [TETRA_DMO_MSG_SETUP_PRES] = dmo_describe_call,
    [TETRA_DMO_MSG_CONNECT] = dmo_describe_connect,
    [TETRA_DMO_MSG_DISCONNECT] = dmo_describe_disconnect,
    [TETRA_DMO_MSG_CONNECT_ACK] = dmo_describe_call,
    [TETRA_DMO_MSG_OCCUPIED] = dmo_describe_call,
    [TETRA_DMO_MSG_RELEASE] = dmo_describe_release,
    [TETRA_DMO_MSG_TX_CEASED] = dmo_describe_tx_ceased,
    [TETRA_DMO_MSG_TX_REQUEST] = dmo_describe_tx_request,
    [TETRA_DMO_MSG_TX_ACCEPT] = dmo_describe_tx_accept,
    [TETRA_DMO_MSG_PREEMPT] = dmo_describe_preempt,
    [TETRA_DMO_MSG_PRE_ACCEPT] = dmo_describe_pre_accept,
    [TETRA_DMO_MSG_REJECT] = dmo_describe_reject,
    [TETRA_DMO_MSG_INFO] = dmo_describe_info,
    [TETRA_DMO_MSG_SDS_UDATA] = dmo_describe_sds,
    [TETRA_DMO_MSG_SDS_DATA] = dmo_describe_sds,
    [TETRA_DMO_MSG_SDS_ACK] = dmo_describe_sds_ack,
    [TETRA_DMO_MSG_GATEWAY] = dmo_describe_gateway,
};

int
tetra_dmo_describe_elems(const tetra_dmo_sync_h_t* h, char* buf, int buf_len) {
    if (!h || !buf || buf_len <= 0) {
        return 0;
    }
    dmo_text t = {buf, (size_t)buf_len, 0u};
    buf[0] = '\0';
    if (!h->md_ok || h->msg_type >= DMO_COUNT(k_describers) || k_describers[h->msg_type] == NULL) {
        return 0;
    }
    k_describers[h->msg_type](h, &t);
    if (h->sdu_state == TETRA_DMO_SDU_TRUNCATED) {
        dmo_put(&t, " truncated");
    } else if (h->sdu_state == TETRA_DMO_SDU_ENCRYPTED) {
        dmo_put(&t, " DM-SDU encrypted");
    } else if (h->fcs_state == TETRA_DMO_FCS_FAILED) {
        dmo_put(&t, " FCS failed");
    }
    return (int)t.len;
}

int
tetra_dmo_describe_presence(const tetra_dmo_pres_t* p, char* buf, int buf_len) {
    /* Channel state (EN 300 396-4 clause 10.3.5): value 3 is the gateway registration phase (EN 300 396-5). */
    static const char* const k_state[] = {"free", "occupied", "reserved", "registration"};
    /* Duplex spacing in kHz (EN 300 396-4 clause 10.3.13), code 0 reserved. */
    static const uint16_t k_spacing_khz[9] = {0u, 1600u, 4500u, 8000u, 10000u, 18000u, 30000u, 39000u, 45000u};
    if (!p || !buf || buf_len <= 0) {
        return 0;
    }
    dmo_text t = {buf, (size_t)buf_len, 0u};
    const int gateway = p->comm_type >= 2u;
    buf[0] = '\0';
    dmo_put(&t, "%s=%u MNI=%u/%u %s %s pwr=%u urt=%u", gateway ? "gw" : "rep", (unsigned)p->repeater,
            ((p->mni >> 14) & 0x3FFu), (p->mni & 0x3FFFu), p->ms_link ? "master" : "slave",
            (gateway || p->channel_state < 3u) ? DMO_NAME(k_state, p->channel_state) : "reserved",
            (unsigned)p->power_class, (unsigned)p->urt);
    if (gateway) {
        dmo_put(&t, " swmi=%s label=%u", p->swmi_up ? "up" : "down", (unsigned)p->reg_label);
    }
    if (p->two_freq && p->spacing >= 1u && p->spacing < DMO_COUNT(k_spacing_khz)) {
        const unsigned khz = k_spacing_khz[p->spacing];
        dmo_put(&t, " uplink=%s%u.%03u MHz", p->reverse ? "-" : "+", khz / 1000u, khz % 1000u);
    }
    return (int)t.len;
}

/* ---- Decoder state -------------------------------------------------------------------------------------------- */

uint32_t
tetra_dmo_dcc_seed(uint32_t net_id, uint32_t src_addr) {
    /* e(1) is the most significant bit of the 30-bit colour code, as for the TMO extended colour code, so the same
     * "<<2 | 3" LFSR seed applies (EN 300 396-2 clause 8.2.5). Only the low 6 bits of `net_id` are used. */
    const uint32_t e = ((net_id & 0x3Fu) << 24) | (src_addr & 0xFFFFFFu);
    return (e << 2) | 3u;
}

/* What a DMAC-SYNC does to the call state, by message type. */
enum { DMO_ROLE_NONE = 0, DMO_ROLE_CALL, DMO_ROLE_FRAGMENTS, DMO_ROLE_END };

static int
dmo_sync_role(const tetra_dmo_sync_h_t* h) {
    static const uint8_t k_roles[32] = {
        [TETRA_DMO_MSG_SETUP] = DMO_ROLE_CALL,         [TETRA_DMO_MSG_CONNECT_ACK] = DMO_ROLE_CALL,
        [TETRA_DMO_MSG_OCCUPIED] = DMO_ROLE_CALL,      [TETRA_DMO_MSG_SDS_UDATA] = DMO_ROLE_FRAGMENTS,
        [TETRA_DMO_MSG_SDS_DATA] = DMO_ROLE_FRAGMENTS, [TETRA_DMO_MSG_SDS_ACK] = DMO_ROLE_FRAGMENTS,
        [TETRA_DMO_MSG_TX_CEASED] = DMO_ROLE_END,      [TETRA_DMO_MSG_DISCONNECT] = DMO_ROLE_END,
        [TETRA_DMO_MSG_RELEASE] = DMO_ROLE_END,
    };
    if (h->msg_type == TETRA_DMO_MSG_GATEWAY) {
        /* A fragmented DM-GSETUP scrambles its SCH/F like a fragmented short data message (EN 300 396-5 clause
         * 8.2.4); DM-GRELEASE ends the call. The subtype is known only once the elements were read. */
        const uint32_t sub = h->md_ok ? h->e[TETRA_DMO_E_GW_SUBTYPE] : (uint32_t)DMO_NONE;
        if (sub == TETRA_DMO_GW_SETUP) {
            return DMO_ROLE_FRAGMENTS;
        }
        return sub == TETRA_DMO_GW_RELEASE ? DMO_ROLE_END : DMO_ROLE_NONE;
    }
    return h->msg_type < DMO_COUNT(k_roles) ? k_roles[h->msg_type] : DMO_ROLE_NONE;
}

/* The DM colour code from the "mobile network identity" (or the gateway address) and "source address" of this
 * DMAC-SYNC (EN 300 396-3 clause 8.2.4, EN 300 396-5 clause 8.2.4). */
static void
dmo_learn_dcc(tetra_state_t* ts, const tetra_dmo_sync_s_t* s, const tetra_dmo_sync_h_t* h) {
    if (h->src_type == 2u) {
        ts->dmo_dcc_valid = 0;
        return;
    }
    const uint32_t net_id = s->comm_type >= 2u ? (uint32_t)h->relay_addr : h->mni;
    ts->dmo_dcc_seed = tetra_dmo_dcc_seed(net_id, h->src);
    ts->dmo_dcc_valid = 1;
}

static void
dmo_set_crypt(tetra_state_t* ts, tetra_dmo_crypt_e crypt) {
    ts->dmo_crypt = (uint8_t)crypt;
}

/* The current traffic transmission is over: its colour code and its protection, which the DMAC-SYNC that set it up gave
 * (EN 300 396-3 clauses 8.2.4 and 8.5.3), are no other transmission's; the next one's DMAC-SYNC gives its own. */
static void
dmo_transmission_over(tetra_state_t* ts) {
    ts->dmo_dcc_valid = 0;
    ts->dmo_tx_crypt = (uint8_t)TETRA_DMO_CRYPT_UNKNOWN;
    ts->dmo_tx_aie = TETRA_DMO_AIE_DM1;
    ts->dmo_tx_seed = 0U;
    dmo_set_crypt(ts, TETRA_DMO_CRYPT_UNKNOWN);
}

/* Another traffic transmission starts, of security class @p aie and protected as @p crypt, from the source whose colour
 * code is @p seed (0 when none is known): the call canonical slot 0 shows is this one's from here
 * (tetra_state_t::dmo_tx_seq). */
static void
dmo_start_transmission(tetra_state_t* ts, uint8_t aie, tetra_dmo_crypt_e crypt, uint32_t seed) {
    ts->dmo_tx_crypt = (uint8_t)crypt;
    ts->dmo_tx_aie = aie;
    ts->dmo_tx_seed = seed;
    ts->dmo_tx_seq++;
    dmo_set_crypt(ts, crypt);
}

/* A transmission is in progress, and of security class @p aie: the class its set-up gave, which it keeps (EN 300 396-3
 * clause 8.5.3). */
static int
dmo_in_transmission_of(const tetra_state_t* ts, uint8_t aie) {
    return ts->dmo_tx_crypt != (uint8_t)TETRA_DMO_CRYPT_UNKNOWN && ts->dmo_tx_aie == aie;
}

/* How closed a protection is: only CLEAR is audible, and the more closed of two is the one evidence allows. */
static int
dmo_closure(tetra_dmo_crypt_e crypt) {
    switch (crypt) {
        case TETRA_DMO_CRYPT_CLEAR: return 0;
        case TETRA_DMO_CRYPT_E2E: return 2;
        case TETRA_DMO_CRYPT_AIR: return 3;
        case TETRA_DMO_CRYPT_UNKNOWN:
        default: return 1;
    }
}

/* A DMAC-SYNC whose message went unread: its SCH/H failed its CRC or was skipped, or DM-2-C encrypts the message type.
 * It may be the DM-SETUP, DM-CONNECT ACK or DM-OCCUPIED that sets the protection of the traffic after it (EN 300 396-3
 * clause 8.5.3), so its SCH/S, which is never encrypted (clause 8.5.3 NOTE 2), stands for that message: an encrypted
 * security class seals the channel's traffic, whatever an earlier DMAC-SYNC said. DM-2-C scrambles that traffic with
 * the encrypted short identity (clause 8.2.4), which needs the key, so its colour code is not known either. A DM-1
 * SCH/S closes nothing it does not know: it leaves the protection as it was. */
static void
dmo_apply_unread(tetra_state_t* ts, const tetra_dmo_sync_s_t* s) {
    if (s->enc_state == TETRA_DMO_AIE_DM1) {
        return;
    }
    if (s->enc_state == TETRA_DMO_AIE_DM2C) {
        ts->dmo_dcc_valid = 0;
        /* A transmission keeps its security class (clause 8.5.3): a DM-2-C one is not the clear, DM-2-A or DM-2-B one
         * before it, whose source and destination it would otherwise go on with. */
        if (!dmo_in_transmission_of(ts, TETRA_DMO_AIE_DM2C)) {
            dmo_start_transmission(ts, TETRA_DMO_AIE_DM2C, TETRA_DMO_CRYPT_AIR, 0U);
            return;
        }
    }
    dmo_set_crypt(ts, TETRA_DMO_CRYPT_AIR);
}

/* The protection a DMAC-SYNC that sets up or occupies the channel gives its traffic: its security class, and under
 * DM-1 its DM-SDU's end-to-end encryption flag (clause 9.7.8). */
static tetra_dmo_crypt_e
dmo_call_class(const tetra_dmo_sync_s_t* s, const tetra_dmo_sync_h_t* h) {
    if (s->enc_state != TETRA_DMO_AIE_DM1) {
        return TETRA_DMO_CRYPT_AIR;
    }
    return h->e[TETRA_DMO_E_E2EE] ? TETRA_DMO_CRYPT_E2E : TETRA_DMO_CRYPT_CLEAR;
}

/* The DM colour code seed the DMAC-SYNC's source gives (clause 8.2.4), 0 when it names no source. */
static uint32_t
dmo_source_seed(const tetra_dmo_sync_s_t* s, const tetra_dmo_sync_h_t* h) {
    if (h->src_type == 2u) {
        return 0U;
    }
    const uint32_t net_id = s->comm_type >= 2u ? (uint32_t)h->relay_addr : h->mni;
    return tetra_dmo_dcc_seed(net_id, h->src);
}

/* DM-SETUP, DM-CONNECT ACK or DM-OCCUPIED protected as @p crypt. One from another source than the transmission in
 * progress (one that names none, which EN 300 396-5 clause 13.5.2.1.1 keeps for gateway-specific messages, is the same
 * as another that names none), or with none in progress, starts a transmission, and so does a set-up that gives another
 * class: another security class (DM-2-A, DM-2-B and DM-2-C all encrypt the air interface), or under DM-1 another
 * end-to-end encryption; one that repeats the set-up's class goes on with it. A DM-OCCUPIED shall carry the class of
 * the DM-SETUP or DM-CONNECT ACK that initiated its transmission (clause 8.5.3): one that agrees confirms it, which
 * opens what an unread DMAC-SYNC sealed; one that contradicts it leaves the more closed of the two, so it never opens
 * speech its set-up closed. */
static void
dmo_apply_setup(tetra_state_t* ts, const tetra_dmo_sync_s_t* s, const tetra_dmo_sync_h_t* h) {
    const tetra_dmo_crypt_e crypt = dmo_call_class(s, h);
    const tetra_dmo_crypt_e tx = (tetra_dmo_crypt_e)ts->dmo_tx_crypt;
    const uint32_t seed = dmo_source_seed(s, h);
    const int occupied = h->msg_type == TETRA_DMO_MSG_OCCUPIED;
    const int other_class = crypt != tx || !dmo_in_transmission_of(ts, s->enc_state);
    if (tx == TETRA_DMO_CRYPT_UNKNOWN || seed != ts->dmo_tx_seed || (!occupied && other_class)) {
        dmo_learn_dcc(ts, s, h);
        dmo_start_transmission(ts, s->enc_state, crypt, seed);
        return;
    }
    dmo_learn_dcc(ts, s, h);
    dmo_set_crypt(ts, dmo_closure(crypt) >= dmo_closure(tx) ? crypt : tx);
}

/* A DMAC-SYNC that sets up or occupies the channel defines the protection of the traffic that follows (EN 300 396-3
 * clause 8.5.3); the end of a transmission leaves it unknown until the next talker's. Any other message's security
 * class applies to that message alone. */
static void
dmo_apply_call(tetra_state_t* ts, const tetra_dmo_sync_s_t* s, const tetra_dmo_sync_h_t* h) {
    const int role = dmo_sync_role(h);
    if (role == DMO_ROLE_CALL) {
        dmo_apply_setup(ts, s, h);
    } else if (role == DMO_ROLE_FRAGMENTS && h->frag_flag) {
        dmo_learn_dcc(ts, s, h);
    } else if (role == DMO_ROLE_END) {
        dmo_transmission_over(ts);
    }
}

static void
dmo_apply_sync(dsd_state* state, tetra_state_t* ts, const tetra_dmo_sync_s_t* s, tetra_dmo_sync_h_t* h) {
    ts->dmo = 1;
    /* The TMO timebase does not run on a direct mode carrier. */
    tetra_tdma_stop(state);
    if (!s->parsed) {
        return;
    }
    ts->dmo_comm_type = s->comm_type;
    /* A presence signal describes the repeater or gateway, not a call. */
    if (s->sync_pdu_type != TETRA_DMO_SYNC_DMAC) {
        return;
    }
    ts->dmo_since_sync = 0;
    if (h) {
        dmo_settle(h, s->enc_state);
    }
    if (h && h->has_addrs) {
        dmo_apply_call(ts, s, h);
    } else {
        dmo_apply_unread(ts, s);
    }
}

void
tetra_dmo_apply_sync(dsd_state* state, const tetra_dmo_sync_s_t* s, tetra_dmo_sync_h_t* h) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !s) {
        return;
    }
    dmo_apply_sync(state, ts, s, h);
    /* The call records and short data notices that follow read the carrier's mode and colour code from the
     * publication (dsd_access_code_current()): it describes direct mode from here, not the burst before. */
    tetra_publish(state);
}

tetra_dmo_crypt_e
tetra_dmo_call_crypt(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || !ts->dmo || ts->dmo_crypt > (uint8_t)TETRA_DMO_CRYPT_E2E) {
        return TETRA_DMO_CRYPT_UNKNOWN;
    }
    return (tetra_dmo_crypt_e)ts->dmo_crypt;
}

/* What the direct mode channel's transmissions left: the DM colour code, its seed and age, and the protection. */
static void
dmo_forget_channel(tetra_state_t* ts) {
    dmo_transmission_over(ts);
    ts->dmo_dcc_seed = 0U;
    ts->dmo_since_sync = 0U;
}

/* Direct mode is the carrier's: the next carrier is trunked until its own DSB says otherwise. Keeping the mode would
 * gain nothing, since without its colour code no DNB of the next carrier decodes before that DSB, and would keep an
 * uplink session, whose carriers have no BSCH to end it, from decoding its NUBs. */
void
tetra_dmo_forget_carrier(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    ts->dmo = 0;
    ts->dmo_comm_type = 0;
    dmo_forget_channel(ts);
}

void
tetra_dmo_transmission_over(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts) {
        dmo_transmission_over(ts);
    }
}

void
tetra_dmo_leave(dsd_opts* opts, dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || !ts->dmo) {
        return;
    }
    /* What a carrier boundary forgets of direct mode, and the call with its canonical call, which the engine ends there. */
    tetra_dmo_forget_carrier(state);
    tetra_calls_dmo_forget(opts, state);
}

/* ---- Logging and terminal lines ------------------------------------------------------------------------------- */

const char*
tetra_dmo_msg_name(int msg_type) {
    /* Message type, EN 300 396-3 clause 9.3.19. */
    static const char* const k_names[32] = {
        [TETRA_DMO_MSG_RESERVED] = "DM-RESERVED",
        [TETRA_DMO_MSG_SDS_OCCUPIED] = "DM-SDS OCCUPIED",
        [TETRA_DMO_MSG_TIMING_REQ] = "DM-TIMING REQUEST",
        [TETRA_DMO_MSG_TIMING_ACK] = "DM-TIMING ACK",
        [TETRA_DMO_MSG_SETUP] = "DM-SETUP",
        [TETRA_DMO_MSG_SETUP_PRES] = "DM-SETUP PRES",
        [TETRA_DMO_MSG_CONNECT] = "DM-CONNECT",
        [TETRA_DMO_MSG_DISCONNECT] = "DM-DISCONNECT",
        [TETRA_DMO_MSG_CONNECT_ACK] = "DM-CONNECT ACK",
        [TETRA_DMO_MSG_OCCUPIED] = "DM-OCCUPIED",
        [TETRA_DMO_MSG_RELEASE] = "DM-RELEASE",
        [TETRA_DMO_MSG_TX_CEASED] = "DM-TX CEASED",
        [TETRA_DMO_MSG_TX_REQUEST] = "DM-TX REQUEST",
        [TETRA_DMO_MSG_TX_ACCEPT] = "DM-TX ACCEPT",
        [TETRA_DMO_MSG_PREEMPT] = "DM-PREEMPT",
        [TETRA_DMO_MSG_PRE_ACCEPT] = "DM-PRE ACCEPT",
        [TETRA_DMO_MSG_REJECT] = "DM-REJECT",
        [TETRA_DMO_MSG_INFO] = "DM-INFO",
        [TETRA_DMO_MSG_SDS_UDATA] = "DM-SDS UDATA",
        [TETRA_DMO_MSG_SDS_DATA] = "DM-SDS DATA",
        [TETRA_DMO_MSG_SDS_ACK] = "DM-SDS ACK",
        [TETRA_DMO_MSG_GATEWAY] = "GATEWAY-SPECIFIC",
        [30] = "proprietary",
        [31] = "proprietary",
    };
    return msg_type >= 0 ? DMO_NAME(k_names, (uint32_t)msg_type) : "reserved";
}

/* Security class name for the logs. */
static const char*
dmo_security_name(uint32_t enc) {
    static const char* const k_names[] = {"DM-1", "DM-2-C", "DM-2-A", "DM-2-B"};
    return DMO_NAME(k_names, enc);
}

/* One decoded DMAC-DATA PDU: a signalling summary by default, the MAC header detail with -Z; the call it describes goes
 * to the call records (tetra_calls_dmo_pdu()). A DM-TX CEASED, DM-RELEASE or DM-DISCONNECT on the STCH ends the traffic
 * transmission as the same message in a DMAC-SYNC does, its colour code and protection with it; any other DMAC-DATA's
 * security class is its own message's (EN 300 396-3 clause 8.5.3). */
static void
dmo_log_data(const uint8_t* bits, int len, const char* chan, dsd_opts* opts, dsd_state* state,
             const tetra_state_t* ts) {
    tetra_dmo_sync_h_t h;
    char desc[160];
    char dst[16];
    (void)tetra_dmo_parse_data(bits, len, ts ? ts->dmo_comm_type : 0, &h);
    if (h.null_pdu) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA DMO %s] DMAC-DATA null PDU\n", chan);
        return;
    }
    TETRA_LOG_PAYLOAD(ts, "[TETRA DMO %s] DMAC-DATA fill=%u stolen2=%u frag=%u cd=%u %s\n", chan, h.fill_bit, h.stolen2,
                      h.frag_flag, h.frame_countdown, dmo_security_name(h.enc_state));
    if (!h.has_addrs) {
        DSD_FPRINTF(stderr, "[TETRA DMO %s] DMAC-DATA %s encrypted\n", chan, dmo_security_name(h.enc_state));
        return;
    }
    (void)tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc));
    if (dmo_sync_role(&h) == DMO_ROLE_END) {
        tetra_dmo_transmission_over(state);
    }
    tetra_calls_dmo_pdu(opts, state, -1, &h);
    if (h.dst_encrypted) {
        DSD_SNPRINTF(dst, sizeof dst, "encrypted");
    } else {
        DSD_SNPRINTF(dst, sizeof dst, "%u", (unsigned)h.dst);
    }
    DSD_FPRINTF(stderr, "[TETRA DMO %s] %s src=%u dst=%s MNI=%u/%u%s%s\n", chan, tetra_dmo_msg_name(h.msg_type),
                (unsigned)h.src, dst, (unsigned)((h.mni >> 14) & 0x3FFu), (unsigned)(h.mni & 0x3FFFu),
                desc[0] ? " | " : "", desc);
}

int
tetra_dmo_log_dnb_pdu(const uint8_t* bits, int len, const char* kind, dsd_opts* opts, dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    const char* chan = kind ? kind : "DNB";
    tetra_bit_reader r = tetra_bits_reader(bits, len, 0);
    /* MAC PDU type, EN 300 396-3 clause 9.3.16, and the two flags after it. */
    const uint32_t type = tetra_bits_read(&r, 2);
    const uint32_t flag2 = tetra_bits_read(&r, 1);
    const uint32_t flag3 = tetra_bits_read(&r, 1);
    if (!tetra_bits_ok(&r)) {
        return -1;
    }
    switch (type) {
        case 0u: dmo_log_data(bits, len, chan, opts, state, ts); break;
        case 1u: TETRA_LOG_PAYLOAD(ts, "[TETRA DMO %s] DMAC-%s fill=%u\n", chan, flag2 ? "END" : "FRAG", flag3); break;
        case 3u: TETRA_LOG_PAYLOAD(ts, "[TETRA DMO %s] DMAC-U-SIGNAL stolen2=%u\n", chan, flag2); break;
        default: TETRA_LOG_PAYLOAD(ts, "[TETRA DMO %s] reserved MAC PDU type\n", chan); break;
    }
    return (int)type;
}
