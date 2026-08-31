// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

enum {
    TEST_HANDLER_NONE = 0,
    TEST_HANDLER_NXDN,
    TEST_HANDLER_DSTAR,
    TEST_HANDLER_DMR,
    TEST_HANDLER_X2TDMA,
    TEST_HANDLER_PROVOICE,
    TEST_HANDLER_EDACS,
    TEST_HANDLER_YSF,
    TEST_HANDLER_M17,
    TEST_HANDLER_P25P2,
    TEST_HANDLER_DPMR,
    TEST_HANDLER_P25P1,
};

static int g_called_handler = TEST_HANDLER_NONE;
/* The verdict every stub reports, so one case can drive the whole table. */
static dsd_frame_verdict g_handler_verdict = DSD_FRAME_VERDICT_PRODUCTIVE;

static dsd_frame_verdict
record_handler(int handler_id) {
    assert(g_called_handler == TEST_HANDLER_NONE);
    g_called_handler = handler_id;
    return g_handler_verdict;
}

int
dsd_dispatch_matches_nxdn(int synctype) {
    return DSD_SYNC_IS_NXDN(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_nxdn(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_NXDN);
}

int
dsd_dispatch_matches_dstar(int synctype) {
    return DSD_SYNC_IS_DSTAR(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_dstar(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_DSTAR);
}

int
dsd_dispatch_matches_dmr(int synctype) {
    return DSD_SYNC_IS_DMR(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_dmr(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_DMR);
}

int
dsd_dispatch_matches_x2tdma(int synctype) {
    return DSD_SYNC_IS_X2TDMA(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_x2tdma(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_X2TDMA);
}

int
dsd_dispatch_matches_provoice(int synctype) {
    return DSD_SYNC_IS_PROVOICE(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_provoice(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_PROVOICE);
}

int
dsd_dispatch_matches_edacs(int synctype) {
    return DSD_SYNC_IS_EDACS(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_edacs(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_EDACS);
}

int
dsd_dispatch_matches_ysf(int synctype) {
    return DSD_SYNC_IS_YSF(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_ysf(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_YSF);
}

int
dsd_dispatch_matches_m17(int synctype) {
    return DSD_SYNC_IS_M17(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_m17(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_M17);
}

int
dsd_dispatch_matches_p25p2(int synctype) {
    return DSD_SYNC_IS_P25P2(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_p25p2(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_P25P2);
}

int
dsd_dispatch_matches_dpmr(int synctype) {
    return DSD_SYNC_IS_DPMR(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_dpmr(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_DPMR);
}

int
dsd_dispatch_matches_p25p1(int synctype) {
    return DSD_SYNC_IS_P25P1(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return record_handler(TEST_HANDLER_P25P1);
}

static void
run_dispatch_case_verdict(int synctype, int expected_handler, dsd_frame_verdict verdict, int stale_verdict_in_state) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    state->synctype = synctype;
    state->rf_mod = 1;
    state->max = 100.0F;
    state->min = -50.0F;
    state->sps_hunt_last_frame_verdict = stale_verdict_in_state;
    g_called_handler = TEST_HANDLER_NONE;
    g_handler_verdict = verdict;

    processFrame(opts, state);

    assert(g_called_handler == expected_handler);
    assert(state->maxref == 80.0F);
    assert(state->minref == -40.0F);
    /* #391: the handler's verdict, recorded where getFrameSync() reads it on its next entry.
     * A synctype no handler claims leaves it productive -- the pre-#391 behaviour -- and
     * either way a verdict left over from the previous frame must not survive. */
    assert(state->sps_hunt_last_frame_verdict
           == (expected_handler == TEST_HANDLER_NONE ? DSD_FRAME_VERDICT_PRODUCTIVE : (int)verdict));
    g_handler_verdict = DSD_FRAME_VERDICT_PRODUCTIVE;
    free(state);
    free(opts);
}

static void
run_dispatch_case(int synctype, int expected_handler) {
    run_dispatch_case_verdict(synctype, expected_handler, DSD_FRAME_VERDICT_PRODUCTIVE, 0);
}

static void
check_public_handler_table(void) {
    assert(strcmp(dsd_protocol_handlers[0].name, "NXDN") == 0);
    assert(strcmp(dsd_protocol_handlers[1].name, "D-STAR") == 0);
    assert(strcmp(dsd_protocol_handlers[2].name, "DMR") == 0);
    assert(strcmp(dsd_protocol_handlers[9].name, "dPMR") == 0);
    assert(strcmp(dsd_protocol_handlers[10].name, "P25P1") == 0);
    assert(dsd_protocol_handlers[11].name == NULL);
}

/* #391: the zero value is PRODUCTIVE, which is what makes the contract safe by omission --
 * a handler with nothing to say, and a zeroed dsd_state, both read as "decoded a frame". */
static void
check_verdict_default_is_productive(void) {
    _Static_assert(DSD_FRAME_VERDICT_PRODUCTIVE == 0, "PRODUCTIVE must be the zero value");
    _Static_assert(DSD_FRAME_VERDICT_UNPRODUCTIVE != 0, "UNPRODUCTIVE must be non-zero");
    /* #400: the DSP layer includes no engine headers, so it reads these as literals out of
     * dsd_state::sps_hunt_last_frame_verdict. frame_sync_sps_hunt_note_handler_consumption()
     * recognises 2 as the proof that restarts a dwell; renumbering here without changing it
     * there turns every proof into a refusal. */
    _Static_assert(DSD_FRAME_VERDICT_PROFILE_PROVEN == 2, "the DSP layer matches PROFILE_PROVEN as the literal 2");
    /* #392: same rule for the verdict that makes a declined dispatch budget-neutral. */
    _Static_assert(DSD_FRAME_VERDICT_WITHHELD == 3, "the DSP layer matches WITHHELD as the literal 3");
}

int
main(void) {
    check_public_handler_table();
    check_verdict_default_is_productive();
    run_dispatch_case(DSD_SYNC_DMR_BS_VOICE_POS, TEST_HANDLER_DMR);
    run_dispatch_case(DSD_SYNC_DPMR_FS1_POS, TEST_HANDLER_DPMR);
    run_dispatch_case(DSD_SYNC_P25P1_POS, TEST_HANDLER_P25P1);
    run_dispatch_case(-1, TEST_HANDLER_NONE);
    run_dispatch_case_verdict(DSD_SYNC_YSF_POS, TEST_HANDLER_YSF, DSD_FRAME_VERDICT_UNPRODUCTIVE, 0);
    /* A stale UNPRODUCTIVE must not survive a productive frame, nor a frame no handler took. */
    run_dispatch_case_verdict(DSD_SYNC_YSF_POS, TEST_HANDLER_YSF, DSD_FRAME_VERDICT_PRODUCTIVE,
                              DSD_FRAME_VERDICT_UNPRODUCTIVE);
    run_dispatch_case_verdict(-1, TEST_HANDLER_NONE, DSD_FRAME_VERDICT_PRODUCTIVE, DSD_FRAME_VERDICT_UNPRODUCTIVE);
    /* #400: a proof reaches frame sync intact, and does not survive the frame that made it. */
    run_dispatch_case_verdict(DSD_SYNC_YSF_POS, TEST_HANDLER_YSF, DSD_FRAME_VERDICT_PROFILE_PROVEN, 0);
    run_dispatch_case_verdict(DSD_SYNC_YSF_POS, TEST_HANDLER_YSF, DSD_FRAME_VERDICT_UNPRODUCTIVE,
                              DSD_FRAME_VERDICT_PROFILE_PROVEN);
    /* #392: and so does a withheld frame, over any verdict the frame before it left. */
    run_dispatch_case_verdict(DSD_SYNC_YSF_POS, TEST_HANDLER_YSF, DSD_FRAME_VERDICT_WITHHELD, 0);
    run_dispatch_case_verdict(DSD_SYNC_YSF_POS, TEST_HANDLER_YSF, DSD_FRAME_VERDICT_PRODUCTIVE,
                              DSD_FRAME_VERDICT_WITHHELD);

    printf("ENGINE_PROTOCOL_DISPATCH: OK\n");
    return 0;
}
