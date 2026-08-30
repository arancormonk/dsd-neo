// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for the small P25 Phase 2 and YSF dispatch wrappers.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/ysf/ysf.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_p25p2(int synctype);
dsd_frame_verdict dsd_dispatch_handle_p25p2(dsd_opts* opts, dsd_state* state);
int dsd_dispatch_matches_ysf(int synctype);
dsd_frame_verdict dsd_dispatch_handle_ysf(dsd_opts* opts, dsd_state* state);

static int g_process_p2_calls;
static int g_process_ysf_calls;

void
processP2(dsd_opts* opts, dsd_state* state) {
    assert(opts != NULL);
    assert(state != NULL);
    g_process_p2_calls++;
    state->lastp25type = 2;
}

/* The stubbed FICH verdict dsd_dispatch_handle_ysf() must pass through. */
static int g_process_ysf_result = 1;

int
processYSF(dsd_opts* opts, dsd_state* state) {
    assert(opts != NULL);
    assert(state != NULL);
    g_process_ysf_calls++;
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), "%s", " YSF          ");
    return g_process_ysf_result;
}

static void
test_p25p2_match_and_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(dsd_dispatch_matches_p25p2(DSD_SYNC_P25P2_POS));
    assert(dsd_dispatch_matches_p25p2(DSD_SYNC_P25P2_NEG));
    assert(!dsd_dispatch_matches_p25p2(DSD_SYNC_P25P1_POS));
    assert(!dsd_dispatch_matches_p25p2(DSD_SYNC_YSF_POS));

    dsd_dispatch_handle_p25p2(&opts, &state);
    assert(g_process_p2_calls == 1);
    assert(state.lastp25type == 2);
}

static void
test_ysf_match_and_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(dsd_dispatch_matches_ysf(DSD_SYNC_YSF_POS));
    assert(dsd_dispatch_matches_ysf(DSD_SYNC_YSF_NEG));
    assert(!dsd_dispatch_matches_ysf(DSD_SYNC_P25P2_POS));
    assert(!dsd_dispatch_matches_ysf(DSD_SYNC_DMR_BS_DATA_POS));

    assert(dsd_dispatch_handle_ysf(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    assert(g_process_ysf_calls == 1);
    assert(strcmp(state.fsubtype, " YSF          ") == 0);
}

/* #391: a FICH whose CRC-16 failed leaves processYSF() reading the rest of the frame on the
 * previous frame's dt/fi. Those ~460 symbols validated nothing and must buy no dwell. */
static void
test_ysf_failed_fich_reports_unproductive(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    g_process_ysf_calls = 0;
    g_process_ysf_result = 0;

    assert(dsd_dispatch_handle_ysf(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    assert(g_process_ysf_calls == 1);
    g_process_ysf_result = 1;
}

int
main(void) {
    test_p25p2_match_and_dispatch();
    test_ysf_match_and_dispatch();
    test_ysf_failed_fich_reports_unproductive();
    printf("P25_P2_YSF_SYNC_DISPATCH: OK\n");
    return 0;
}
