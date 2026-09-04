// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused coverage for DMR CSBK parse/table helper contracts.
 */

#include "dsd-neo/core/safe_api.h"

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/dmr/dmr_csbk_parse.h>
#include <dsd-neo/protocol/dmr/dmr_csbk_tables.h>
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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_sm_emit_group_grant_slot(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int slot, int tg, int src) {
    (void)opts;
    (void)state;
    g_group_calls++;
    g_last_freq = freq_hz;
    g_last_lpcn = lpcn;
    g_last_slot = slot;
    g_last_target = tg;
    g_last_source = src;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_sm_emit_indiv_grant_slot(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int slot, int dst, int src) {
    (void)opts;
    (void)state;
    g_indiv_calls++;
    g_last_freq = freq_hz;
    g_last_lpcn = lpcn;
    g_last_slot = slot;
    g_last_target = dst;
    g_last_source = src;
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

    dmr_csbk_handle(&result, &opts, &state);
    assert(g_group_calls == 1);
    assert(g_indiv_calls == 0);
    assert(g_last_freq == 851012500L);
    assert(g_last_lpcn == 0x2A5);
    assert(g_last_slot == 1);
    assert(g_last_target == 0x123456);
    assert(g_last_source == 0x654321);
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

    result.opcode = 47U;
    dmr_csbk_handle(&result, &opts, &state);
    assert(g_indiv_calls == 1);
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
    test_opcode_names_cover_grant_table();
    DSD_FPRINTF(stdout, "DMR_CSBK_HELPERS: OK\n");
    return 0;
}
