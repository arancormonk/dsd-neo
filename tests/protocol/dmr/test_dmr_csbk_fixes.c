// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR CSBK and data header decode fixes, driven through the CSBK dispatcher dmr_cspdu() and the data header entry
 * dmr_dheader(), plus the reason tables and the C_AHOY classification they print with.
 *
 * - NACK_Rsp (TS 102 361-2 Table 7.6) carries the source before the target, and its Reason Code uses the Tier II
 *   table, which differs from the Tier III C_ACKD table (0x21).
 * - Standard CSBK meanings belong to FID 0x00 only: vendor opcodes that reuse a standard number print as
 *   manufacturer CSBKs.
 * - C_AHOY Service_Kind 13 / 14 is told apart by its whole field combination (TS 102 361-4 Tables 6.11, 6.17, 6.19,
 *   6.23, 6.27): stun vs revive vs kill, radio check vs authentication vs subscription list request.
 * - Gen_Site_Params reads its site information from BParms2 (Table 7.80), UU_Ans_Rsp is named as the spec names it,
 *   C_BCAST shows its partition bits, a UDT header shows its Emergency / UDT_Option_Flag bits (Table 6.31) and an
 *   SP_HEAD its whole 10-bit status (TS 102 361-1 Table 9.17A).
 */

#include <assert.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_reason.h"
#include "dsd-neo/core/safe_api.h"
#include "test_support.h"

static int g_failures;
static char g_out[8192];

static void
expect_contains(const char* needle, const char* tag) {
    if (strstr(g_out, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle, g_out);
        g_failures++;
    }
}

static void
expect_absent(const char* needle, const char* tag) {
    if (strstr(g_out, needle) != NULL) {
        DSD_FPRINTF(stderr, "%s: unexpected '%s' in '%s'\n", tag, needle, g_out);
        g_failures++;
    }
}

static void
expect_true(int cond, const char* tag) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: failed\n", tag);
        g_failures++;
    }
}

static void
bytes_to_bits(const uint8_t* bytes, size_t count, uint8_t* bits) {
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - (i % 8U))) & 1U);
    }
}

/* One CRC-correct CSBK through the dispatcher; @p ten holds its octets 0-9 (the rest of the 24-octet buffer the
   C_BCAST MBC parser reads stays zero). */
static void
run_csbk(dsd_opts* opts, dsd_state* state, const uint8_t ten[10]) {
    uint8_t bytes[24];
    uint8_t bits[sizeof bytes * 8U];
    DSD_MEMSET(bytes, 0, sizeof bytes);
    DSD_MEMCPY(bytes, ten, 10);
    bytes_to_bits(bytes, sizeof bytes, bits);
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_csbk_fixes") == 0);
    dmr_cspdu(opts, state, bits, bytes, 1, 0);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, g_out, sizeof g_out) == 0);
}

static void
test_nack_rsp(dsd_opts* opts, dsd_state* state) {
    /* AIF 1, Source Type 0 (BS), Service Type 0x04 (UU_V_Req), Reason 0x21, Source 100, Target 200. */
    static const uint8_t nack[10] = {0xA6, 0x00, 0x84, 0x21, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8};
    run_csbk(opts, state, nack);
    expect_contains("NACK_Rsp) - Source [100] - Target [200]", "nack source before target");
    expect_contains("Source Type: BS;", "nack source type");
    expect_contains("Service: 04;", "nack service type");
    expect_contains("Reason: 21 (Radio does not support this service or feature)", "nack tier2 reason");

    static const uint8_t nack_ms[10] = {0xA6, 0x00, 0xC4, 0x55, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8};
    run_csbk(opts, state, nack_ms);
    expect_contains("Source Type: MS;", "nack ms source type");
    expect_contains("Reason: 55;", "nack undefined reason raw");

    /* The same opcode under a vendor FID is not a NACK_Rsp. */
    static const uint8_t vendor[10] = {0xA6, 0x10, 0x84, 0x21, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8};
    run_csbk(opts, state, vendor);
    expect_absent("NACK_Rsp", "vendor op 38 not NACK_Rsp");
    expect_contains("Manufacturer CSBK: FID 10; Opcode 26;", "vendor op 38 manufacturer");
}

static void
test_c_ackd_tier3_reason(dsd_opts* opts, dsd_state* state) {
    /* C_ACKD, Reason 0x21 at bits 23-30, target 123, source REGI. Tier III 0x21 is Perm_User_Refused. */
    uint8_t ack[10] = {0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7B, 0xFF, 0xFE, 0xC6};
    ack[2] = (uint8_t)(0x21U >> 7);
    ack[3] = (uint8_t)((0x21U << 1) & 0xFFU);
    run_csbk(opts, state, ack);
    expect_contains("C_ACKD", "c_ackd printed");
    expect_contains("Reason: 21 (Not authorised for this user (permanent))", "c_ackd tier3 reason");

    ack[2] = (uint8_t)(0x62U >> 7);
    ack[3] = (uint8_t)((0x62U << 1) & 0xFFU);
    run_csbk(opts, state, ack);
    expect_contains("Reason: 62 (Registration accepted)", "c_ackd reg accepted");
    expect_contains("REGI", "c_ackd regi gateway");
}

static void
test_vendor_opcodes_not_standard(dsd_opts* opts, dsd_state* state) {
    static const struct {
        uint8_t op;
        uint8_t fid;
        const char* standard;
        const char* manufacturer;
    } cases[] = {
        {31U, 0x10U, " C_RAND ", "Manufacturer CSBK: FID 10; Opcode 1F;"},
        {42U, 0x10U, " P_MAINT", "Manufacturer CSBK: FID 10; Opcode 2A;"},
        {28U, 0x06U, " C_AHOY", "Manufacturer CSBK: FID 06; Opcode 1C;"},
        {32U, 0x68U, " C_ACKD", "Manufacturer CSBK: FID 68; Opcode 20;"},
        {32U, 0x08U, " C_ACKD", "Manufacturer CSBK: FID 08; Opcode 20;"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t pdu[10] = {0};
        pdu[0] = (uint8_t)(0x80U | cases[i].op);
        pdu[1] = cases[i].fid;
        run_csbk(opts, state, pdu);
        expect_absent(cases[i].standard, "vendor opcode not printed as standard");
        expect_contains(cases[i].manufacturer, "vendor opcode printed as manufacturer");
    }

    /* Opcode 56 on MS data is BS_Dwn_Act only under FID 0x00 (elsewhere it is a grant opcode, run for any FID). */
    static const uint8_t bs_dwn_vendor[10] = {0xB8, 0x10};
    static const uint8_t bs_dwn_standard[10] = {0xB8, 0x00};
    const int saved_synctype = state->synctype;
    state->synctype = DSD_SYNC_DMR_MS_DATA;
    run_csbk(opts, state, bs_dwn_vendor);
    expect_absent("BS_Dwn_Act", "vendor op 56 on MS data not BS_Dwn_Act");
    expect_contains("Manufacturer CSBK: FID 10; Opcode 38;", "vendor op 56 on MS data printed as manufacturer");
    run_csbk(opts, state, bs_dwn_standard);
    expect_contains("BS_Dwn_Act", "standard op 56 on MS data");
    expect_absent("Manufacturer CSBK", "standard op 56 not manufacturer");
    state->synctype = saved_synctype;

    /* FID 0x00 keeps the standard meaning. */
    static const uint8_t c_rand[10] = {0x9F, 0x00};
    run_csbk(opts, state, c_rand);
    expect_contains(" C_RAND ", "standard C_RAND");
    static const uint8_t p_maint[10] = {0xAA, 0x00};
    run_csbk(opts, state, p_maint);
    expect_contains(" P_MAINT", "standard P_MAINT");
}

static void
run_ahoy(dsd_opts* opts, dsd_state* state, uint8_t flag, uint8_t gi, uint8_t kind, uint32_t source) {
    uint8_t pdu[10] = {0x9C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7B, 0x00, 0x00, 0x00};
    pdu[2] = flag & 1U;
    pdu[3] = (uint8_t)(((gi & 1U) << 6) | (kind & 0x0FU));
    pdu[7] = (uint8_t)(source >> 16);
    pdu[8] = (uint8_t)(source >> 8);
    pdu[9] = (uint8_t)source;
    run_csbk(opts, state, pdu);
}

static void
test_c_ahoy_kinds(dsd_opts* opts, dsd_state* state) {
    run_ahoy(opts, state, 0U, 0U, 13U, 0xFFFECCU);
    expect_contains("Stun Command;", "ahoy stun");
    expect_absent("Revive", "ahoy stun is not revive");
    expect_contains("Target: 123;", "ahoy stun target");
    expect_contains("STUNI", "ahoy stun gateway");

    run_ahoy(opts, state, 1U, 0U, 13U, 0xFFFECCU);
    expect_contains("Revive Command;", "ahoy revive");
    expect_absent("Stun Command", "ahoy revive is not stun");

    run_ahoy(opts, state, 0U, 0U, 13U, 0xFFFECFU);
    expect_contains("Kill Command;", "ahoy kill");
    expect_contains("KILLI", "ahoy kill gateway");

    run_ahoy(opts, state, 0U, 0U, 14U, 0xFFFECAU);
    expect_contains("Radio Check;", "ahoy radio check");
    expect_absent("Authentication Challenge", "ahoy radio check is not authentication");

    run_ahoy(opts, state, 0U, 0U, 14U, 0x123456U);
    expect_contains("Authentication Challenge;", "ahoy authentication");
    expect_contains("Challenge: 123456;", "ahoy challenge shown as a value");
    expect_absent("Source: 1193046", "ahoy challenge never shown as a source identity");

    run_ahoy(opts, state, 1U, 0U, 14U, 0xFFFED7U);
    expect_contains("Talkgroup Subscription List Request;", "ahoy subscription request");
    expect_contains("TATTSI", "ahoy subscription gateway");
}

static int
classify(uint8_t kind, uint8_t flag, uint8_t group, uint32_t source) {
    const dmr_ahoy_fields f = {kind, flag, group, 123U, source};
    return dmr_ahoy_classify(&f);
}

static void
test_ahoy_classify_table(void) {
    expect_true(classify(13U, 0U, 0U, 0xFFFECCU) == DMR_AHOY_STUN, "classify stun");
    expect_true(classify(13U, 1U, 0U, 0xFFFECCU) == DMR_AHOY_REVIVE, "classify revive");
    expect_true(classify(13U, 0U, 0U, 0xFFFECFU) == DMR_AHOY_KILL, "classify kill");
    expect_true(classify(13U, 1U, 0U, 0xFFFECFU) == DMR_AHOY_UNRECOGNISED, "kill has no flag-1 form");
    expect_true(classify(13U, 0U, 1U, 0xFFFECCU) == DMR_AHOY_UNRECOGNISED, "stun of a talkgroup is not a stun");
    expect_true(classify(13U, 0U, 0U, 0xFFFED6U) == DMR_AHOY_UNRECOGNISED, "DGNA is not a stun");
    expect_true(classify(14U, 0U, 0U, 0xFFFECAU) == DMR_AHOY_RADIO_CHECK, "classify radio check");
    expect_true(classify(14U, 0U, 1U, 0xFFFECAU) == DMR_AHOY_RADIO_CHECK, "classify talkgroup radio check");
    expect_true(classify(14U, 0U, 0U, 0x000000U) == DMR_AHOY_AUTHENTICATION, "classify lowest challenge");
    expect_true(classify(14U, 0U, 0U, 0xFFFCDFU) == DMR_AHOY_AUTHENTICATION, "classify highest challenge");
    expect_true(classify(14U, 0U, 0U, 0xFFFCE0U) == DMR_AHOY_UNRECOGNISED, "above the challenge range");
    expect_true(classify(14U, 0U, 0U, 0xFFFEC6U) == DMR_AHOY_UNRECOGNISED, "REGI source is no challenge");
    expect_true(classify(14U, 1U, 0U, 0xFFFED7U) == DMR_AHOY_SUBSCRIPTION_REQ, "classify subscription request");
    expect_true(classify(14U, 1U, 0U, 0x123456U) == DMR_AHOY_UNRECOGNISED, "flag 1 needs TATTSI");
    expect_true(classify(0U, 0U, 0U, 0x000064U) == DMR_AHOY_OTHER_SERVICE, "voice radio check is another service");
    expect_true(dmr_ahoy_classify(NULL) == DMR_AHOY_UNRECOGNISED, "classify null");
}

static void
test_reason_tables(void) {
    const char* t3 = dmr_tier3_reason_text(0x21U);
    const char* t2 = dmr_tier2_nack_reason_text(0x21U);
    expect_true(t3 != NULL && t2 != NULL && strcmp(t3, t2) != 0, "0x21 differs between Tier II and Tier III");
    expect_true(dmr_tier2_nack_reason_text(0x20U) == NULL, "tier2 defines only 0x21");
    expect_true(dmr_tier3_reason_text(0x2AU) != NULL && strstr(dmr_tier3_reason_text(0x2AU), "refused") != NULL,
                "tier3 reg refused");
    expect_true(dmr_tier3_reason_text(0x2BU) != NULL && strstr(dmr_tier3_reason_text(0x2BU), "denied") != NULL,
                "tier3 reg denied");
    expect_true(dmr_tier3_reason_text(0xA1U) != NULL, "tier3 queued-for-busy is 0xA1");
    expect_true(dmr_tier3_reason_text(0xAAU) == NULL, "0xAA is not queued-for-busy");
    expect_true(dmr_tier3_reason_text(0x16U) != NULL, "tier3 MS duplex not supported from the clause text");
    expect_true(dmr_tier3_reason_text(0xE0U) != NULL, "tier3 wait");
    expect_true(dmr_tier3_reason_text(0x3EU) == NULL, "tier3 undefined value");
    expect_true(dmr_tier3_reason_class_of(0x62U) == DMR_TIER3_REASON_ACK, "class ack");
    expect_true(dmr_tier3_reason_class_of(0x2BU) == DMR_TIER3_REASON_NACK, "class nack");
    expect_true(dmr_tier3_reason_class_of(0xA0U) == DMR_TIER3_REASON_QACK, "class qack");
    expect_true(dmr_tier3_reason_class_of(0xE0U) == DMR_TIER3_REASON_WACK, "class wack");
}

static void
test_cosmetic_labels(dsd_opts* opts, dsd_state* state) {
    /* UU_Ans_Rsp (TS 102 361-2 Table 7.5c). */
    static const uint8_t uu_ans[10] = {0x85, 0x00, 0x00, 0x20, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8};
    run_csbk(opts, state, uu_ans);
    expect_contains("(UU_Ans_Rsp)", "op 5 named UU_Ans_Rsp");
    expect_absent("UU_Ans_Req", "op 5 not UU_Ans_Req");

    /* C_BCAST Gen_Site_Params (type 7): BParms1 all ones (reserved), BParms2 = Current Site Information 0x40
       (hibernating), reserved 0x00, Network Information 0x80 (Reg and TG Subscription = 1). PAR = 2. */
    uint8_t bcast[10] = {0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x80};
    /* Announcement type 7 at bits 16-20, BParms1 (14 ones) at bits 21-34. */
    bcast[2] = (uint8_t)((7U << 3) | 0x07U);
    bcast[3] = 0xFFU;
    bcast[4] = 0xE0U;
    /* System code (14 bits at 40) 0x0123, PAR (bits 54-55) = 2. */
    bcast[5] = (uint8_t)(0x0123U >> 6);
    bcast[6] = (uint8_t)(((0x0123U << 2) & 0xFCU) | 0x02U);
    opts->payload = 1;
    run_csbk(opts, state, bcast);
    opts->payload = 0;
    expect_contains("Gen_Site_Params", "gen site params printed");
    expect_contains("Hibernating: 1;", "hibernating flag from BParms2");
    expect_contains("Current Site Info: 40;", "current site information from BParms2");
    expect_contains("Network Info: 80;", "network information from BParms2");
    expect_contains("Reg TG Subscription: 1", "reg and tg subscription flag");
    expect_contains("SYS: 0123;", "system code convention kept");
    expect_contains("PAR: 2;", "partition bits shown");
}

/* A data header (12 octets with its CRC, which the caller vouches for) through dmr_dheader(). */
static void
run_dheader(dsd_opts* opts, dsd_state* state, const uint8_t ten[10]) {
    uint8_t bytes[12];
    uint8_t bits[sizeof bytes * 8U];
    DSD_MEMSET(bytes, 0, sizeof bytes);
    DSD_MEMCPY(bytes, ten, 10);
    bytes_to_bits(bytes, sizeof bytes, bits);
    state->currentslot = 0;
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_dheader_fixes") == 0);
    dmr_dheader(opts, state, bytes, bits, 1, 0);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, g_out, sizeof g_out) == 0);
}

static void
test_udt_header_flags(dsd_opts* opts, dsd_state* state) {
    /* G/I 0, A 1, Emergency 1, UDT_Option_Flag 0, DPF 0000; SAP 0, UDT format 4; target 100, source 200; UAB 0. */
    static const uint8_t emergency[10] = {0x60, 0x04, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8, 0x00, 0x1A};
    run_dheader(opts, state, emergency);
    expect_contains("Unified Data Transport (UDT)", "udt header printed");
    expect_contains(" - Emergency", "udt emergency bit printed");
    expect_contains(" - Option 0", "udt option flag printed");

    static const uint8_t option[10] = {0x10, 0x04, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8, 0x00, 0x1A};
    run_dheader(opts, state, option);
    expect_absent(" - Emergency", "udt without emergency");
    expect_contains(" - Option 1", "udt option flag set");
}

/* The assembled UDT (header + appended blocks) through the block assembler, as test_dmr_udt_utf16_text.c drives it:
   its decode line shows the header's Emergency bit too. */
static void
test_udt_decoder_emergency(dsd_opts* opts, dsd_state* state, uint8_t octet0, int want_emergency) {
    state->currentslot = 0;
    state->data_header_valid[0] = 1;
    state->data_header_blocks[0] = 2;
    state->data_block_counter[0] = 2;
    state->data_byte_ctr[0] = 0;
    state->data_conf_data[0] = 0;
    state->data_header_format[0] = 0;
    state->data_header_sap[0] = 0;
    state->data_p_head[0] = 0;
    state->udt_uab_reserved[0] = 0;
    state->data_block_crc_valid[0][0] = 1;
    DSD_MEMSET(state->dmr_pdu_sf[0], 0, sizeof(state->dmr_pdu_sf[0]));
    state->dmr_pdu_sf[0][0] = octet0;
    state->dmr_pdu_sf[0][1] = 0x07; /* UTF-16 text */
    state->dmr_pdu_sf[0][12] = 0x00;
    state->dmr_pdu_sf[0][13] = 0x41;
    uint8_t block[12];
    DSD_MEMCPY(block, &state->dmr_pdu_sf[0][24], sizeof block);
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_udt_emergency") == 0);
    dmr_block_assembler(opts, state, block, 12, 0x06, 3);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, g_out, sizeof g_out) == 0);
    expect_contains("UDT ", "udt decoder line");
    if (want_emergency) {
        expect_contains("UDT Emergency; ", "udt decoder emergency");
    } else {
        expect_absent("Emergency", "udt decoder without emergency");
    }
}

static void
test_sp_head_status(dsd_opts* opts, dsd_state* state) {
    /* DPF 14 with no appended blocks (SP_HEAD); SP 1, DP 2; Status/Precoded (bits 70-79) = 1000 = 0x3E8. */
    uint8_t sp_head[10] = {0x0E, 0xA0, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8, 0x00, 0x00};
    sp_head[8] = (uint8_t)((1U << 5) | (2U << 2) | (0x3E8U >> 8));
    sp_head[9] = (uint8_t)(0x3E8U & 0xFFU);
    run_dheader(opts, state, sp_head);
    expect_contains("[SP_HEAD]", "sp_head printed");
    expect_contains("S/P 1000", "sp_head 10-bit status");
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);

    test_nack_rsp(opts, state);
    test_c_ackd_tier3_reason(opts, state);
    test_vendor_opcodes_not_standard(opts, state);
    test_c_ahoy_kinds(opts, state);
    test_ahoy_classify_table();
    test_reason_tables();
    test_cosmetic_labels(opts, state);
    test_udt_header_flags(opts, state);
    test_sp_head_status(opts, state);

    /* The UDT text path writes into event history, which the app allocates at init. */
    state->event_history_s = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    assert(state->event_history_s != NULL);
    for (int i = 0; i < 2; i++) {
        init_event_history(&state->event_history_s[i], 0, 255);
    }
    test_udt_decoder_emergency(opts, state, 0x20U, 1);
    test_udt_decoder_emergency(opts, state, 0x00U, 0);

    dsd_state_ext_free_all(state);
    free(state->event_history_s);
    free(state);
    free(opts);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DMR_CSBK_FIXES: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "DMR_CSBK_FIXES: PASS\n");
    return 0;
}
