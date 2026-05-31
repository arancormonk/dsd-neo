// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for P25 Phase 1 sync constants and dispatch routing.
 */

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
#include <mbelib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_p25p1(int synctype);
void dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state);

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

int
getDibitWithReliability(dsd_opts* opts, dsd_state* state, uint8_t* out_reliability) {
    (void)opts;
    (void)state;
    if (out_reliability != NULL) {
        *out_reliability = 255;
    }
    return 0;
}

int
check_NID_with_observed_nac_soft(const char* bch_code, const uint8_t* reliab63, int observed_nac, int* new_nac,
                                 char* new_duid, unsigned char parity, uint8_t parity_reliab, int* error_count) {
    (void)bch_code;
    (void)reliab63;
    (void)observed_nac;
    (void)parity;
    (void)parity_reliab;
    if (new_nac != NULL) {
        *new_nac = 0x293;
    }
    if (new_duid != NULL) {
        new_duid[0] = '0';
        new_duid[1] = '3';
        new_duid[2] = '\0';
    }
    if (error_count != NULL) {
        *error_count = 0;
    }
    return NID_OK;
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    opts->mbe_out_f = stdout;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    opts->mbe_out_f = NULL;
}

void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    opts->mbe_out_fR = NULL;
}

void
resumeScan(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
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
p25_status_accum_classify(dsd_state* state, const dsd_opts* opts) {
    (void)state;
    (void)opts;
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

    state.synctype = DSD_SYNC_P25P1_POS;
    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.nac == 0x293);
    assert(state.lastp25type == 0);
    assert(strcmp(state.fsubtype, " TDU          ") == 0);
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_simple_tdu_dispatch_defaults();
    printf("P25_P1_SYNC_DISPATCH: OK\n");
    return 0;
}
