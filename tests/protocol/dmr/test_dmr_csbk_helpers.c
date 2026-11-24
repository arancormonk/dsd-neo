// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused coverage for DMR CSBK parse/table helper contracts.
 */

#include "dsd-neo/core/safe_api.h"

#include <assert.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/dmr/dmr_csbk_parse.h>
#include <dsd-neo/protocol/dmr/dmr_csbk_tables.h>
#include <dsd-neo/protocol/dmr/dmr_t3_grant.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_group_calls;
static int g_indiv_calls;
static long g_last_freq;
static int g_last_lpcn;
static int g_last_slot;
static int g_last_target;
static int g_last_source;
static dmr_sm_event_t g_last_event;

/* The trunk state machine's grant entry: what the CSBK layer hands it, classification and policy verdict included. */
dmr_sm_grant_result_e
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_sm_submit_grant(dsd_opts* opts, dsd_state* state, const dmr_sm_event_t* ev) {
    (void)opts;
    (void)state;
    g_last_event = *ev;
    if (ev->is_group) {
        g_group_calls++;
        g_last_target = ev->tg;
    } else {
        g_indiv_calls++;
        g_last_target = ev->dst;
    }
    g_last_freq = ev->freq_hz;
    g_last_lpcn = ev->lpcn;
    g_last_slot = ev->slot;
    g_last_source = ev->src;
    return DMR_SM_GRANT_FOLLOWED;
}

static void
set_bits(uint8_t bits[80], size_t offset, size_t width, uint32_t value) {
    for (size_t i = 0; i < width; i++) {
        size_t shift = width - 1U - i;
        bits[offset + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
reset_grant_spy(void) {
    g_group_calls = 0;
    g_indiv_calls = 0;
    g_last_freq = 0;
    g_last_lpcn = 0;
    g_last_slot = -1;
    g_last_target = 0;
    g_last_source = 0;
    DSD_MEMSET(&g_last_event, 0, sizeof(g_last_event));
}

static void
test_parse_rejects_null_arguments(void) {
    uint8_t bits[80] = {0};
    uint8_t bytes[10] = {0};
    struct dmr_csbk_result result;

    assert(dmr_csbk_parse(NULL, bytes, &result) < 0);
    assert(dmr_csbk_parse(bits, NULL, &result) < 0);
    assert(dmr_csbk_parse(bits, bytes, NULL) < 0);
}

static void
test_parse_non_grant_header_only(void) {
    uint8_t bits[80] = {0};
    uint8_t bytes[10] = {0};
    struct dmr_csbk_result result;

    bytes[0] = 0x80U | 0x40U | 12U;
    bytes[1] = 0x23U;

    assert(dmr_csbk_parse(bits, bytes, &result) == 0);
    assert(result.lb == 1U);
    assert(result.pf == 1U);
    assert(result.opcode == 12U);
    assert(result.fid == 0x23U);
    assert(result.lpcn == 0U);
    assert(result.target == 0U);
    assert(result.bits == bits);
    assert(result.bytes == bytes);
}

static void
test_parse_and_handle_group_grant(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80] = {0};
    uint8_t bytes[10] = {0};
    struct dmr_csbk_result result;
    reset_grant_spy();

    bytes[0] = 0x80U | 49U;
    bytes[1] = 0x11U;
    set_bits(bits, 16U, 12U, 0x2A5U);
    set_bits(bits, 16U, 13U, 0x54BU);
    bits[29] = 1U;
    bits[30] = 0U;
    bits[31] = 1U;
    set_bits(bits, 32U, 24U, 0x123456U);
    set_bits(bits, 56U, 24U, 0x654321U);

    assert(dmr_csbk_parse(bits, bytes, &result) == 0);
    result.freq_hz = 851012500L;
    assert(result.opcode == 49U);
    assert(result.fid == 0x11U);
    assert(result.lpcn == 0x2A5U);
    assert(result.pluschannum == 0x54CU);
    assert(result.lcn == bits[28]);
    assert(result.st1 == 1U);
    assert(result.st2 == 0U);
    assert(result.st3 == 1U);
    assert(result.target == 0x123456U);
    assert(result.source == 0x654321U);
    assert(result.group_call == 1 && result.data_call == 0);

    result.policy_valid = 1;
    result.policy.tune_allowed = 1;
    result.policy.priority = 70;
    result.policy.preempt_requested = 1;
    dmr_csbk_handle(&result, &opts, &state);
    assert(g_group_calls == 1);
    assert(g_indiv_calls == 0);
    assert(g_last_freq == 851012500L);
    assert(g_last_lpcn == 0x2A5);
    assert(g_last_slot == 1);
    assert(g_last_target == 0x123456);
    assert(g_last_source == 0x654321);
    // The CSBK layer's classification and verdict travel to the state machine (it never re-evaluates a grant).
    assert(g_last_event.type == DMR_SM_EV_GRANT);
    assert(g_last_event.kind == DSD_CALL_KIND_GROUP_VOICE);
    assert(g_last_event.identity_known == 1);
    assert(g_last_event.origin == DMR_SM_ORIGIN_GRANT);
    assert(g_last_event.caller_busy == 0);
    assert(g_last_event.policy_valid == 1);
    assert(g_last_event.policy.tune_allowed == 1);
    assert(g_last_event.policy.priority == 70);
    assert(g_last_event.policy.preempt_requested == 1);
}

/* A Tier III group data grant (TD_GRANT) is a group call with its data classification, which its policy was judged
   with; a private data grant (PD_GRANT) an individual one. */
static void
test_handle_data_grant_keeps_classification(void) {
    static dsd_opts opts;
    static dsd_state state;
    struct dmr_csbk_result result = {0};
    reset_grant_spy();

    result.opcode = 52U;
    result.group_call = 1;
    result.freq_hz = 851025000L;
    result.lpcn = 7U;
    result.lcn = 0U;
    result.target = 1300U;
    result.source = 2300U;
    result.data_call = 1;
    result.policy_valid = 1;
    result.policy.tune_allowed = 1;
    result.policy.data_call = 1;
    dmr_csbk_handle(&result, &opts, &state);
    assert(g_group_calls == 1);
    assert(g_last_event.kind == DSD_CALL_KIND_DATA);
    assert(g_last_event.is_group == 1 && g_last_event.tg == 1300);
    assert(g_last_event.policy.data_call == 1);
    assert(g_last_slot == 0);

    // A private data grant is individual, with its data classification.
    reset_grant_spy();
    result.opcode = 51U;
    result.group_call = 0;
    result.policy.private_call = 1;
    dmr_csbk_handle(&result, &opts, &state);
    assert(g_indiv_calls == 1);
    assert(g_last_event.kind == DSD_CALL_KIND_DATA);
    assert(g_last_event.dst == 1300 && g_last_event.src == 2300);
    assert(g_last_event.policy.private_call == 1);
}

static void
test_handle_individual_and_ignored_paths(void) {
    static dsd_opts opts;
    static dsd_state state;
    struct dmr_csbk_result result = {0};
    reset_grant_spy();

    dmr_csbk_handle(NULL, &opts, &state);
    dmr_csbk_handle(&result, NULL, &state);
    dmr_csbk_handle(&result, &opts, NULL);
    assert(g_group_calls == 0);
    assert(g_indiv_calls == 0);

    result.opcode = 48U;
    result.freq_hz = 935000000L;
    result.lpcn = 123U;
    result.lcn = 1U;
    result.target = 77U;
    result.source = 88U;
    dmr_csbk_handle(&result, &opts, &state);
    assert(g_group_calls == 0);
    assert(g_indiv_calls == 1);
    assert(g_last_freq == 935000000L);
    assert(g_last_lpcn == 123);
    assert(g_last_slot == 1);
    assert(g_last_target == 77);
    assert(g_last_source == 88);
    assert(g_last_event.kind == DSD_CALL_KIND_PRIVATE_VOICE);
    assert(g_last_event.policy_valid == 0);

    result.opcode = 47U;
    dmr_csbk_handle(&result, &opts, &state);
    assert(g_indiv_calls == 1);
}

/* A Tier III grant's channel is its LPCN, 1..4094 (0 is CHNULL and 4095 the absolute escape, neither a channel): the
   state machine gets it with kind LPCN, or no channel (0, kind NONE). */
static void
test_handle_grant_channel_kind(void) {
    static dsd_opts opts;
    static dsd_state state;

    static const struct {
        uint16_t lpcn;
        int channel;
        uint8_t kind;
    } cases[] = {
        {1U, 1, DSD_CALL_CHANNEL_LPCN}, {12U, 12, DSD_CALL_CHANNEL_LPCN},  {4094U, 4094, DSD_CALL_CHANNEL_LPCN},
        {0U, 0, DSD_CALL_CHANNEL_NONE}, {4095U, 0, DSD_CALL_CHANNEL_NONE},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct dmr_csbk_result result = {0};
        reset_grant_spy();
        result.opcode = 49U;
        result.group_call = 1;
        result.freq_hz = 851025000L;
        result.lpcn = cases[i].lpcn;
        result.target = 1300U;
        result.source = 2300U;
        dmr_csbk_handle(&result, &opts, &state);
        assert(g_group_calls == 1);
        assert(g_last_event.lpcn == cases[i].channel);
        assert(g_last_event.channel_kind == cases[i].kind);
    }
}

/* TS 102 361-4 Tables 7.9-7.15: group {49, 50, 52, 56 outbound}, private {48, 51, 53, 54, 55}, data {51, 52, 54, 55,
   56}; 56 inbound is BS_Dwn_Act. A duplex grant's Call Direction (bit 31) says whether bits 32-55 name the caller. */
static void
test_grant_classify(void) {
    static const struct {
        int opcode;
        uint8_t group;
        uint8_t data;
    } cases[] = {
        {48, 0U, 0U}, {49, 1U, 0U}, {50, 1U, 0U}, {51, 0U, 1U}, {52, 1U, 1U},
        {53, 0U, 0U}, {54, 0U, 1U}, {55, 0U, 1U}, {56, 1U, 1U},
    };

    uint8_t bits[80] = {0};
    dmr_t3_grant_class grant;
    set_bits(bits, 32U, 24U, 0x00AB01U);
    set_bits(bits, 56U, 24U, 0x00CD02U);
    bits[31] = 1U;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const int duplex = cases[i].opcode == 53 || cases[i].opcode == 54;
        assert(dmr_t3_grant_classify(cases[i].opcode, bits, 1, &grant) == 1);
        assert(grant.group == cases[i].group && grant.data == cases[i].data);
        // Bit 31 is Call Direction only on a duplex grant (Emergency/Offset neighbours elsewhere).
        assert(grant.caller == (duplex ? 0x00AB01U : 0x00CD02U));
        assert(grant.recipient == (duplex ? 0x00CD02U : 0x00AB01U));
    }
    bits[31] = 0U;
    assert(dmr_t3_grant_classify(53, bits, 1, &grant) == 1);
    assert(grant.caller == 0x00CD02U && grant.recipient == 0x00AB01U);
    assert(dmr_t3_grant_classify(54, bits, 1, &grant) == 1);
    assert(grant.caller == 0x00CD02U && grant.recipient == 0x00AB01U);

    // Inbound 56 is BS_Dwn_Act; inbound 48-55 stay grants. Non-grant opcodes and NULL arguments are refused.
    assert(dmr_t3_grant_classify(56, bits, 0, &grant) == 0 && grant.recipient == 0U && grant.group == 0U);
    assert(dmr_t3_grant_classify(52, bits, 0, &grant) == 1 && grant.group == 1U);
    assert(dmr_t3_grant_classify(47, bits, 1, &grant) == 0);
    assert(dmr_t3_grant_classify(57, bits, 1, &grant) == 0);
    assert(dmr_t3_grant_classify(49, NULL, 1, &grant) == 0);
    assert(dmr_t3_grant_classify(49, bits, 1, NULL) == 0);
}

/* dmr_csbk_parse() takes a duplex grant's parties from its Call Direction. */
static void
test_parse_duplex_grant_direction(void) {
    uint8_t bits[80] = {0};
    uint8_t bytes[10] = {0};
    struct dmr_csbk_result result;
    bytes[0] = 0x80U | 54U;
    set_bits(bits, 32U, 24U, 501U);
    set_bits(bits, 56U, 24U, 502U);
    bits[31] = 1U;
    assert(dmr_csbk_parse(bits, bytes, &result) == 0);
    assert(result.target == 502U && result.source == 501U);
    assert(result.group_call == 0 && result.data_call == 1);
    bits[31] = 0U;
    assert(dmr_csbk_parse(bits, bytes, &result) == 0);
    assert(result.target == 501U && result.source == 502U);
}

static void
test_opcode_names_cover_grant_table(void) {
    assert(strcmp(dmr_csbk_grant_opcode_name(48U), "Private Voice Channel Grant (PV_GRANT)") == 0);
    assert(strcmp(dmr_csbk_grant_opcode_name(49U), "Talkgroup Voice Channel Grant (TV_GRANT)") == 0);
    assert(strcmp(dmr_csbk_grant_opcode_name(50U), "Broadcast Voice Channel Grant (BTV_GRANT)") == 0);
    assert(strcmp(dmr_csbk_grant_opcode_name(51U), "Private Data Channel Grant: Single Item (PD_GRANT)") == 0);
    assert(strcmp(dmr_csbk_grant_opcode_name(52U), "Talkgroup Data Channel Grant: Single Item (TD_GRANT)") == 0);
    assert(strcmp(dmr_csbk_grant_opcode_name(53U), "Duplex Private Voice Channel Grant (PV_GRANT_DX)") == 0);
    assert(strcmp(dmr_csbk_grant_opcode_name(54U), "Duplex Private Data Channel Grant (PD_GRANT_DX)") == 0);
    assert(strcmp(dmr_csbk_grant_opcode_name(55U), "Private Data Channel Grant: Multi Item (PD_GRANT)") == 0);
    assert(strcmp(dmr_csbk_grant_opcode_name(56U), "Talkgroup Data Channel Grant: Multi Item (TD_GRANT)") == 0);
    assert(strcmp(dmr_csbk_grant_opcode_name(57U), "Unknown CSBK") == 0);
}

int
main(void) {
    test_parse_rejects_null_arguments();
    test_parse_non_grant_header_only();
    test_parse_and_handle_group_grant();
    test_handle_individual_and_ignored_paths();
    test_handle_data_grant_keeps_classification();
    test_handle_grant_channel_kind();
    test_opcode_names_cover_grant_table();
    test_grant_classify();
    test_parse_duplex_grant_direction();
    DSD_FPRINTF(stdout, "DMR_CSBK_HELPERS: OK\n");
    return 0;
}
