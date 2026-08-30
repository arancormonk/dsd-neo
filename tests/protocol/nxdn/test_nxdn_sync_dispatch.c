// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for NXDN sync constants and dispatch routing.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <stdio.h>

int dsd_dispatch_matches_nxdn(int synctype);
dsd_frame_verdict dsd_dispatch_handle_nxdn(dsd_opts* opts, dsd_state* state);

static int frame_calls;

/* The stubbed CRC confirmation dsd_dispatch_handle_nxdn() must pass through. */
static int frame_confirmed = 1;

int
nxdn_frame(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    frame_calls++;
    return frame_confirmed;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(NXDN_MS_DATA_SYNC) == 19U, "NXDN_MS_DATA_SYNC length");
    _Static_assert(sizeof(INV_NXDN_MS_DATA_SYNC) == 19U, "INV_NXDN_MS_DATA_SYNC length");
    _Static_assert(sizeof(NXDN_BS_DATA_SYNC) == 19U, "NXDN_BS_DATA_SYNC length");
    _Static_assert(sizeof(INV_NXDN_BS_DATA_SYNC) == 19U, "INV_NXDN_BS_DATA_SYNC length");
    _Static_assert(sizeof(NXDN_MS_VOICE_SYNC) == 19U, "NXDN_MS_VOICE_SYNC length");
    _Static_assert(sizeof(INV_NXDN_MS_VOICE_SYNC) == 19U, "INV_NXDN_MS_VOICE_SYNC length");
    _Static_assert(sizeof(NXDN_BS_VOICE_SYNC) == 19U, "NXDN_BS_VOICE_SYNC length");
    _Static_assert(sizeof(INV_NXDN_BS_VOICE_SYNC) == 19U, "INV_NXDN_BS_VOICE_SYNC length");
    _Static_assert(sizeof(NXDN_FSW) == 11U, "NXDN_FSW length");
    _Static_assert(sizeof(INV_NXDN_FSW) == 11U, "INV_NXDN_FSW length");
    _Static_assert(sizeof(NXDN_PANDFSW) == 20U, "NXDN_PANDFSW length");
    _Static_assert(sizeof(INV_NXDN_PANDFSW) == 20U, "INV_NXDN_PANDFSW length");
}

static void
test_synctype_helpers(void) {
    _Static_assert(DSD_SYNC_IS_NXDN(DSD_SYNC_NXDN_POS), "NXDN positive synctype");
    _Static_assert(DSD_SYNC_IS_NXDN(DSD_SYNC_NXDN_NEG), "NXDN negative synctype");
    assert(dsd_dispatch_matches_nxdn(DSD_SYNC_NXDN_POS));
    assert(dsd_dispatch_matches_nxdn(DSD_SYNC_NXDN_NEG));

    _Static_assert(!DSD_SYNC_IS_NXDN(DSD_SYNC_DPMR_FS1_POS), "dPMR is not NXDN");
    _Static_assert(!DSD_SYNC_IS_NXDN(DSD_SYNC_DSTAR_HD_NEG), "D-STAR header is not NXDN");
    assert(!dsd_dispatch_matches_nxdn(DSD_SYNC_DPMR_FS1_POS));
    assert(!dsd_dispatch_matches_nxdn(DSD_SYNC_DSTAR_HD_NEG));
}

static void
test_dispatch_calls_frame(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    frame_calls = 0;

    state.synctype = DSD_SYNC_NXDN_POS;
    assert(dsd_dispatch_handle_nxdn(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    state.synctype = DSD_SYNC_NXDN_NEG;
    assert(dsd_dispatch_handle_nxdn(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);

    assert(frame_calls == 2);
}

/* #391: NXDN's sync word and LICH are weak enough that noise clears both (#398), so an
 * unconfirmed frame's 182 symbols must not buy the SPS hunt's dwell. */
static void
test_unconfirmed_frame_reports_unproductive(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    frame_calls = 0;
    frame_confirmed = 0;

    state.synctype = DSD_SYNC_NXDN_POS;
    assert(dsd_dispatch_handle_nxdn(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    assert(frame_calls == 1);
    frame_confirmed = 1;
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_dispatch_calls_frame();
    test_unconfirmed_frame_reports_unproductive();
    printf("NXDN_SYNC_DISPATCH: OK\n");
    return 0;
}
