// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for P25 Phase 1 sync constants and dispatch routing.
 */

#include <mbelib-neo/mbelib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_p25p1(int synctype);
void dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state);

static uint8_t g_test_duid = 0x3U;
static int g_check_result = NID_OK;
static int g_new_nac = 0x293;
static int g_error_count;
static int g_last_observed_nac;
static int g_open_mbe_calls;
static int g_close_mbe_calls;
static int g_close_mbe_r_calls;
static int g_resume_calls;
static int g_status_classify_calls;

static void
reset_stub_state(void) {
    g_test_duid = 0x3U;
    g_check_result = NID_OK;
    g_new_nac = 0x293;
    g_error_count = 0;
    g_last_observed_nac = -1;
    g_open_mbe_calls = 0;
    g_close_mbe_calls = 0;
    g_close_mbe_r_calls = 0;
    g_resume_calls = 0;
    g_status_classify_calls = 0;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    if (out_soft != NULL) {
        out_soft->reliability = 255;
        out_soft->llr[0] = -255;
        out_soft->llr[1] = -255;
    }
    return 0;
}

struct p25p1_nid_result
p25p1_nid_decode(const char bch_code[63], const uint8_t reliab63[63], int observed_nac, unsigned char parity,
                 uint8_t parity_reliab) {
    (void)bch_code;
    (void)reliab63;
    (void)parity;
    (void)parity_reliab;
    g_last_observed_nac = observed_nac;
    struct p25p1_nid_result result = {(enum NidResult)g_check_result, g_new_nac, g_test_duid, g_error_count};
    return result;
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    ++g_open_mbe_calls;
    opts->mbe_out_f = stdout;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    ++g_close_mbe_calls;
    opts->mbe_out_f = NULL;
}

void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    ++g_close_mbe_r_calls;
    opts->mbe_out_fR = NULL;
}

void
resumeScan(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    ++g_resume_calls;
}

void
mbe_initMbeParms(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    (void)cur_mp;
    (void)prev_mp;
    (void)prev_mp_enhanced;
}

void
p25_status_accum_reset(dsd_state* state) {
    (void)state;
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    (void)dibit_value;
}

void
p25_status_accum_classify(dsd_state* state) {
    (void)state;
    ++g_status_classify_calls;
}

void
processHDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lastp25type = 2;
}

void
processLDU1(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lastp25type = 1;
}

void
processLDU2(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lastp25type = 2;
}

void
processTDULC(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lastp25type = 0;
}

void
processTDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lastp25type = 0;
}

void
processTSBK(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lastp25type = 3;
}

void
processMPDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lastp25type = 4;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(P25P1_SYNC) == 25U, "P25P1_SYNC length");
    _Static_assert(sizeof(INV_P25P1_SYNC) == 25U, "INV_P25P1_SYNC length");
}

static void
test_synctype_helpers(void) {
    _Static_assert(DSD_SYNC_IS_P25P1(DSD_SYNC_P25P1_POS), "P25P1 positive synctype");
    _Static_assert(DSD_SYNC_IS_P25P1(DSD_SYNC_P25P1_NEG), "P25P1 negative synctype");
    _Static_assert(DSD_SYNC_IS_P25(DSD_SYNC_P25P1_POS), "P25P1 positive is P25");
    _Static_assert(DSD_SYNC_IS_P25(DSD_SYNC_P25P1_NEG), "P25P1 negative is P25");
    assert(dsd_dispatch_matches_p25p1(DSD_SYNC_P25P1_POS));
    assert(dsd_dispatch_matches_p25p1(DSD_SYNC_P25P1_NEG));

    _Static_assert(!DSD_SYNC_IS_P25P1(DSD_SYNC_P25P2_POS), "P25P2 is not P25P1");
    _Static_assert(!DSD_SYNC_IS_P25P1(DSD_SYNC_DMR_BS_DATA_POS), "DMR BS data is not P25P1");
    _Static_assert(!DSD_SYNC_IS_P25P1(DSD_SYNC_NXDN_POS), "NXDN is not P25P1");
    assert(!dsd_dispatch_matches_p25p1(DSD_SYNC_P25P2_POS));
    assert(!dsd_dispatch_matches_p25p1(DSD_SYNC_DMR_BS_DATA_POS));
    assert(!dsd_dispatch_matches_p25p1(DSD_SYNC_NXDN_POS));
}

static void
test_simple_tdu_dispatch_defaults(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    g_test_duid = 0x3U;
    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.nac == 0x293);
    assert(state.lastp25type == 0);
    assert(strcmp(state.fsubtype, " TDU          ") == 0);
}

static const char k_valid_embedded_gps[] = "GPS: 41.12345N 087.12345W (41.12345, -87.12345) Current Fix";

static void
seed_stale_data_call_display(dsd_state* state) {
    DSD_SNPRINTF(state->dmr_embedded_gps[0], sizeof(state->dmr_embedded_gps[0]), "%s", k_valid_embedded_gps);
    DSD_SNPRINTF(state->dmr_lrrp_gps[0], sizeof(state->dmr_lrrp_gps[0]),
                 "Data Call: Mobile Radio Statistics; SAP:24; LLID: 393226; ");
}

static void
expect_stale_data_call_display_cleared(uint8_t duid, unsigned int expected_burst, const char* expected_subtype) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    seed_stale_data_call_display(&state);

    g_test_duid = duid;
    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.dmrburstL == expected_burst);
    assert(strcmp(state.dmr_embedded_gps[0], k_valid_embedded_gps) == 0);
    assert(state.dmr_lrrp_gps[0][0] == '\0');
    assert(strcmp(state.fsubtype, expected_subtype) == 0);
}

static void
test_p25p1_dispatch_clears_stale_data_call_display(void) {
    static const struct {
        uint8_t duid;
        unsigned int expected_burst;
        const char* expected_subtype;
    } cases[] = {
        {0x0U, 25, " HDU          "}, {0x5U, 26, " LDU1         "}, {0xAU, 27, " LDU2         "},
        {0xFU, 28, " TDULC        "}, {0x3U, 28, " TDU          "}, {0x7U, 29, " TSBK         "},
        {0xCU, 29, " MPDU         "}, {0xFFU, 0, "              "},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        expect_stale_data_call_display_cleared(cases[i].duid, cases[i].expected_burst, cases[i].expected_subtype);
    }
}

static void
expect_observed_nac(const char* label, unsigned long long p2_cc, int p2_hardset, int nac, int expected_observed_nac) {
    (void)label;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    state.p2_cc = p2_cc;
    state.p2_hardset = p2_hardset;
    state.nac = nac;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_last_observed_nac == expected_observed_nac);
}

static void
test_p25p1_observed_nac_priority(void) {
    expect_observed_nac("hardset-p2-cc", 0x123ULL, 1, 0x234, 0x123);
    expect_observed_nac("decoded-nac", 0x123ULL, 0, 0x234, 0x234);
    expect_observed_nac("fallback-p2-cc", 0x456ULL, 0, 0, 0x456);
    expect_observed_nac("invalid-values", 0xFFFULL, 0, 0xFFF, 0);
}

static void
test_p25p1_nid_correction_and_failure_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    g_check_result = NID_PARITY_OVERRIDE;
    g_error_count = 3;
    g_new_nac = 0x321;
    g_test_duid = 0x3U;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.nid_corrections_total == 3U);
    assert(state.nid_parity_overrides == 1U);
    assert(state.nac == 0x321);
    assert(state.p2_cc == 0x321ULL);
    assert(state.debug_header_errors == 2U);
    assert(state.nid_failures_total == 0U);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    g_test_duid = 0x5U;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.nid_failures_total == 1U);
    assert(state.debug_header_critical_errors == 1U);
    assert(state.lastp25type == 0);
    assert(strcmp(state.fsubtype, "              ") == 0);
    assert(g_status_classify_calls == 1);
}

static void
test_p25p1_nac_update_guards(void) {
    static const int invalid_nacs[] = {0, 0xFFF};

    for (size_t i = 0; i < sizeof(invalid_nacs) / sizeof(invalid_nacs[0]); i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_stub_state();

        state.synctype = DSD_SYNC_P25P1_POS;
        state.nac = 0x2AA;
        state.p2_cc = 0x2AAULL;
        g_new_nac = invalid_nacs[i];
        g_error_count = 2;
        g_test_duid = 0x0U;

        dsd_dispatch_handle_p25p1(&opts, &state);

        assert(g_last_observed_nac == 0x2AA);
        assert(state.nac == 0x2AA);
        assert(state.p2_cc == 0x2AAULL);
        assert(state.nid_corrections_total == 2U);
        assert(state.debug_header_errors == 1U);
        assert(state.nid_failures_total == 0U);
    }

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x234;
    state.p2_cc = 0x123ULL;
    state.p2_hardset = 1;
    g_new_nac = 0x321;
    g_error_count = 1;
    g_test_duid = 0x0U;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_last_observed_nac == 0x123);
    assert(state.nac == 0x321);
    assert(state.p2_cc == 0x123ULL);
    assert(state.nid_corrections_total == 1U);
    assert(state.debug_header_errors == 1U);
    assert(state.nid_failures_total == 0U);
}

static void
test_p25p1_mbe_output_and_resume_side_effects(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "captures");
    opts.mbe_out_f = (FILE*)0x1;
    g_test_duid = 0x0U;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_close_mbe_calls == 1);
    assert(g_open_mbe_calls == 0);
    assert(opts.mbe_out_f == NULL);
    assert(state.lastp25type == 2);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "captures");
    opts.mbe_out_f = (FILE*)0x1;
    opts.mbe_out_fR = (FILE*)0x2;
    opts.resume = 1;
    g_test_duid = 0x7U;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_close_mbe_calls == 1);
    assert(g_close_mbe_r_calls == 1);
    assert(g_resume_calls == 1);
    assert(opts.mbe_out_f == NULL);
    assert(opts.mbe_out_fR == NULL);
    assert(state.lastp25type == 3);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    opts.resume = 1;
    state.numtdulc = 1;
    g_test_duid = 0xFU;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_resume_calls == 1);
    assert(state.numtdulc == 2);
    assert(state.lastp25type == 0);
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_simple_tdu_dispatch_defaults();
    test_p25p1_dispatch_clears_stale_data_call_display();
    test_p25p1_observed_nac_priority();
    test_p25p1_nid_correction_and_failure_state();
    test_p25p1_nac_update_guards();
    test_p25p1_mbe_output_and_resume_side_effects();
    printf("P25_P1_SYNC_DISPATCH: OK\n");
    return 0;
}
