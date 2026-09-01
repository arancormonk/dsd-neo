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
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <stdio.h>

int dsd_dispatch_matches_nxdn(int synctype);
dsd_frame_verdict dsd_dispatch_handle_nxdn(dsd_opts* opts, dsd_state* state);

static int frame_calls;

/* The stubbed nxdn_frame() answer dsd_dispatch_handle_nxdn() must act on: 0 unconfirmed,
 * 1 confirmed, 2 confirmed by a CRC this frame carried itself. */
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

/* #445: a frame that passes a CRC of its own records the profile it was read on, so the weaker
 * matchers waiting on 4800/4 can tell that a 2400-baud transmission was live a moment ago. The
 * verdict is deliberately not changed by it -- reporting PROFILE_PROVEN measured worse, see
 * dispatch_nxdn.c -- so recording the proof and acting on it stay separate. */
static void
test_dispatch_records_the_profile_a_crc_passed_on(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_NXDN_POS;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;

    /* A frame carrying its own CRC stamps the profile, and still reports the verdict it always
     * did: the hunt's dwell accounting is untouched by this. */
    frame_confirmed = 2;
    state.symbolcnt = 1000U;
    assert(dsd_dispatch_handle_nxdn(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    assert(state.profile_proof_valid == 1);
    assert(state.profile_proof_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4);
    assert(state.profile_proof_symbolcnt == 1000U);

    /* A confirmed frame that carried no CRC of its own leaves the stamp alone, so a run of them
     * cannot hold the guard open on a transmission that has stopped proving. */
    frame_confirmed = 1;
    state.symbolcnt = 1192U;
    assert(dsd_dispatch_handle_nxdn(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    assert(state.profile_proof_symbolcnt == 1000U);

    /* So does an unconfirmed one, which is what the noise between transmissions produces. */
    frame_confirmed = 0;
    state.symbolcnt = 1384U;
    assert(dsd_dispatch_handle_nxdn(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    assert(state.profile_proof_symbolcnt == 1000U);

    /* The next real pass moves it, wherever the hunt has got to by then. */
    frame_confirmed = 2;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.symbolcnt = 5000U;
    assert(dsd_dispatch_handle_nxdn(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    assert(state.profile_proof_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.profile_proof_symbolcnt == 5000U);

    /* A zeroed state records nothing, which is what the valid flag is for: index 0 is a real
     * profile and symbol 0 a real count. */
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_NXDN_POS;
    frame_confirmed = 0;
    assert(dsd_dispatch_handle_nxdn(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    assert(state.profile_proof_valid == 0);

    frame_confirmed = 1;
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_dispatch_calls_frame();
    test_unconfirmed_frame_reports_unproductive();
    test_dispatch_records_the_profile_a_crc_passed_on();
    printf("NXDN_SYNC_DISPATCH: OK\n");
    return 0;
}
