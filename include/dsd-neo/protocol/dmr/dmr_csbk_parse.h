// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief CSBK parsing helpers shared across DMR control code.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR CSBK parsing and dispatch helpers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_CSBK_PARSE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_CSBK_PARSE_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dmr_csbk_result {
    uint8_t lb;
    uint8_t pf;
    uint8_t opcode;
    uint8_t fid;

    /* Channel grant-related fields (when applicable). */
    uint16_t lpcn;
    uint16_t pluschannum;
    uint8_t lcn;
    uint8_t st1;
    uint8_t st2;
    uint8_t st3;
    /* A grant's called MS or talkgroup and its calling MS (dmr_t3_grant_classify(); for a duplex grant its Call
       Direction decides which address is which). */
    uint32_t target;
    uint32_t source;

    /* The frequency in Hz the trunk state machine may use directly: the one an absolute grant states on air, or the
       user's entry for the grant's LPCN (dmr_t3_overlay_resolve(), discussion #900), which also replaces an absolute
       grant's frequency. 0 leaves the LPCN's channel-map resolution and its trust check (lpcn_is_trusted()) to the
       machine; the CSBK layer never hands it a map lookup of its own. */
    long freq_hz;

    /* The grant's classification (group or private, data or voice; dmr_t3_grant_classify()) and talkgroup policy
       verdict, as the CSBK layer judged it (dmr_csbk_handle() hands them to the trunk state machine, which never
       re-evaluates a grant). */
    int group_call;
    int data_call;
    int policy_valid;
    dsd_tg_policy_decision policy;

    const uint8_t* bits;
    const uint8_t* bytes;
};

/**
 * Decode basic CSBK header fields and, for channel-grant opcodes, the
 * common grant fields used by trunking logic.
 *
 * The PDU is taken as outbound (BS-sourced): opcode 56 reads as a group data
 * grant (dmr_t3_grant_classify() with outbound 1). The caller tells an inbound
 * BS_Dwn_Act (56 on MS-sourced data) apart and does not pass it here.
 *
 * @return 0 on success, negative on error.
 */
int dmr_csbk_parse(const uint8_t* cs_pdu_bits, const uint8_t* cs_pdu, struct dmr_csbk_result* out);

/**
 * Central dispatch helper for CSBK trunking actions.
 *
 * Currently routes channel-grant CSBKs to the DMR Tier III trunking
 * state machine (dmr_sm_submit_grant()) using the parsed fields in @p res,
 * with the call kind and policy verdict the CSBK layer computed.
 */
void dmr_csbk_handle(const struct dmr_csbk_result* res, dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_CSBK_PARSE_H_ */
