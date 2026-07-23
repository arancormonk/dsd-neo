// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for DMR sync constants and dispatch routing.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_dmr(int synctype);
void dsd_dispatch_handle_dmr(dsd_opts* opts, dsd_state* state);

static int bs_bootstrap_calls;
static int close_left_calls;
static int close_right_calls;
static int data_sync_calls;
static int ms_bootstrap_calls;
static int ms_data_calls;
static int open_left_calls;

static void
reset_calls(void) {
    bs_bootstrap_calls = 0;
    close_left_calls = 0;
    close_right_calls = 0;
    data_sync_calls = 0;
    ms_bootstrap_calls = 0;
    ms_data_calls = 0;
    open_left_calls = 0;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    open_left_calls++;
    opts->mbe_out_f = stdout;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    close_left_calls++;
    opts->mbe_out_f = NULL;
}

void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    close_right_calls++;
    opts->mbe_out_fR = NULL;
}

void
dmrBSBootstrap(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    bs_bootstrap_calls++;
}

void
dmrMSBootstrap(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    ms_bootstrap_calls++;
}

void
dmrMSData(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    ms_data_calls++;
}

void
dmr_data_sync(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    data_sync_calls++;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(DMR_BS_DATA_SYNC) == 25U, "DMR_BS_DATA_SYNC length");
    _Static_assert(sizeof(DMR_BS_VOICE_SYNC) == 25U, "DMR_BS_VOICE_SYNC length");
    _Static_assert(sizeof(DMR_MS_DATA_SYNC) == 25U, "DMR_MS_DATA_SYNC length");
    _Static_assert(sizeof(DMR_MS_VOICE_SYNC) == 25U, "DMR_MS_VOICE_SYNC length");
    _Static_assert(sizeof(DMR_RESERVED_SYNC) == 25U, "DMR_RESERVED_SYNC length");
    _Static_assert(sizeof(DMR_DIRECT_MODE_TS1_DATA_SYNC) == 25U, "DMR_DIRECT_MODE_TS1_DATA_SYNC length");
    _Static_assert(sizeof(DMR_DIRECT_MODE_TS1_VOICE_SYNC) == 25U, "DMR_DIRECT_MODE_TS1_VOICE_SYNC length");
    _Static_assert(sizeof(DMR_DIRECT_MODE_TS2_DATA_SYNC) == 25U, "DMR_DIRECT_MODE_TS2_DATA_SYNC length");
    _Static_assert(sizeof(DMR_DIRECT_MODE_TS2_VOICE_SYNC) == 25U, "DMR_DIRECT_MODE_TS2_VOICE_SYNC length");
}

static void
test_synctype_helpers(void) {
    for (int synctype = DSD_SYNC_DMR_BS_DATA_POS; synctype <= DSD_SYNC_DMR_BS_DATA_NEG; synctype++) {
        assert(DSD_SYNC_IS_DMR_BS(synctype));
        assert(DSD_SYNC_IS_DMR(synctype));
        assert(dsd_dispatch_matches_dmr(synctype));
    }

    _Static_assert(DSD_SYNC_IS_DMR_MS(DSD_SYNC_DMR_MS_VOICE), "DMR MS voice synctype");
    _Static_assert(DSD_SYNC_IS_DMR_MS(DSD_SYNC_DMR_MS_DATA), "DMR MS data synctype");
    _Static_assert(DSD_SYNC_IS_DMR_MS(DSD_SYNC_DMR_RC_DATA), "DMR RC data synctype");
    _Static_assert(DSD_SYNC_IS_DMR(DSD_SYNC_DMR_MS_VOICE), "DMR MS voice is DMR");
    _Static_assert(DSD_SYNC_IS_DMR(DSD_SYNC_DMR_MS_DATA), "DMR MS data is DMR");
    _Static_assert(DSD_SYNC_IS_DMR(DSD_SYNC_DMR_RC_DATA), "DMR RC data is DMR");
    assert(dsd_dispatch_matches_dmr(DSD_SYNC_DMR_MS_VOICE));
    assert(dsd_dispatch_matches_dmr(DSD_SYNC_DMR_MS_DATA));
    assert(dsd_dispatch_matches_dmr(DSD_SYNC_DMR_RC_DATA));

    _Static_assert(!DSD_SYNC_IS_DMR(DSD_SYNC_X2TDMA_DATA_POS), "X2-TDMA data is not DMR");
    _Static_assert(!DSD_SYNC_IS_DMR(DSD_SYNC_NXDN_POS), "NXDN is not DMR");
    assert(!dsd_dispatch_matches_dmr(DSD_SYNC_X2TDMA_DATA_POS));
    assert(!dsd_dispatch_matches_dmr(DSD_SYNC_NXDN_POS));
}

static void
test_branding_update(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_mfid = 0x68;
    dsd_dispatch_handle_dmr(&opts, &state);
    assert(strcmp(state.dmr_branding, "  Hytera") == 0);

    state.dmr_mfid = 0x58;
    dsd_dispatch_handle_dmr(&opts, &state);
    assert(strcmp(state.dmr_branding, "    Tait") == 0);

    state.dmr_mfid = 0x10;
    dsd_dispatch_handle_dmr(&opts, &state);
    assert(strcmp(state.dmr_branding, "    Tait") == 0);
}

static void
test_bs_voice_routes_to_mono_bootstrap(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "out");
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dsd_dispatch_handle_dmr(&opts, &state);

    assert(strcmp(state.fsubtype, " VOICE        ") == 0);
    assert(strcmp(state.slot1light, " slot1 ") == 0);
    assert(strcmp(state.slot2light, " slot2 ") == 0);
    assert(state.nac == 0);
    assert(open_left_calls == 1);
    assert(ms_bootstrap_calls == 1);
    assert(bs_bootstrap_calls == 0);
}

static void
test_ms_voice_routes_to_single_slot_bootstrap(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    opts.dmr_mono = 1;
    opts.dmr_stereo = 1;
    state.dmr_stereo = 1;
    state.synctype = DSD_SYNC_DMR_MS_VOICE;
    dsd_dispatch_handle_dmr(&opts, &state);

    assert(strcmp(state.slot1light, " slot1 ") == 0);
    assert(strcmp(state.slot2light, " slot2 ") == 0);
    assert(ms_bootstrap_calls == 1);
    assert(bs_bootstrap_calls == 0);
    assert(state.dmr_stereo == 0);
}

static void
test_trunked_bs_voice_in_mono_mode_falls_back_to_bs_bootstrap(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "out");
    opts.dmr_mono = 1;
    opts.dmr_stereo = 1;
    opts.trunk_enable = 1;
    state.dmr_stereo = 1;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_NEG;
    dsd_dispatch_handle_dmr(&opts, &state);

    assert(strcmp(state.slot1light, " slot1 ") == 0);
    assert(strcmp(state.slot2light, " slot2 ") == 0);
    assert(open_left_calls == 1);
    assert(bs_bootstrap_calls == 1);
    assert(ms_bootstrap_calls == 0);
    assert(state.dmr_stereo == 1);
}

static void
test_stereo_voice_routes_by_synctype(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    opts.dmr_stereo = 1;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_NEG;
    dsd_dispatch_handle_dmr(&opts, &state);
    assert(state.dmr_stereo == 1);
    assert(bs_bootstrap_calls == 1);
    assert(ms_bootstrap_calls == 0);

    reset_calls();
    state.synctype = DSD_SYNC_DMR_MS_VOICE;
    dsd_dispatch_handle_dmr(&opts, &state);
    assert(state.dmr_stereo == 1);
    assert(bs_bootstrap_calls == 0);
    assert(ms_bootstrap_calls == 1);
}

static void
test_ms_data_routes_to_ms_data(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    opts.mbe_out_f = stdout;
    opts.mbe_out_fR = stderr;
    state.synctype = DSD_SYNC_DMR_RC_DATA;
    dsd_dispatch_handle_dmr(&opts, &state);

    assert(ms_data_calls == 1);
    assert(data_sync_calls == 0);
    assert(close_left_calls == 1);
    assert(close_right_calls == 1);
}

static void
test_bs_data_routes_to_data_sync(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    opts.dmr_stereo = 1;
    opts.mbe_out_f = stdout;
    opts.mbe_out_fR = stderr;
    state.synctype = DSD_SYNC_DMR_BS_DATA_NEG;
    state.err_str[0] = 'E';

    dsd_dispatch_handle_dmr(&opts, &state);

    assert(state.dmr_stereo == 0);
    assert(strcmp(state.slot1light, " slot1 ") == 0);
    assert(strcmp(state.slot2light, " slot2 ") == 0);
    assert(data_sync_calls == 1);
    assert(ms_data_calls == 0);
    assert(close_left_calls == 1);
    assert(close_right_calls == 1);
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_branding_update();
    test_bs_voice_routes_to_mono_bootstrap();
    test_ms_voice_routes_to_single_slot_bootstrap();
    test_trunked_bs_voice_in_mono_mode_falls_back_to_bs_bootstrap();
    test_stereo_voice_routes_by_synctype();
    test_ms_data_routes_to_ms_data();
    test_bs_data_routes_to_data_sync();
    printf("DMR_SYNC_DISPATCH: OK\n");
    return 0;
}
