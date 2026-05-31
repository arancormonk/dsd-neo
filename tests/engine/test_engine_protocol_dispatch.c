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

static void
record_handler(int handler_id) {
    assert(g_called_handler == TEST_HANDLER_NONE);
    g_called_handler = handler_id;
}

int
dsd_dispatch_matches_nxdn(int synctype) {
    return DSD_SYNC_IS_NXDN(synctype);
}

void
dsd_dispatch_handle_nxdn(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_NXDN);
}

int
dsd_dispatch_matches_dstar(int synctype) {
    return DSD_SYNC_IS_DSTAR(synctype);
}

void
dsd_dispatch_handle_dstar(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_DSTAR);
}

int
dsd_dispatch_matches_dmr(int synctype) {
    return DSD_SYNC_IS_DMR(synctype);
}

void
dsd_dispatch_handle_dmr(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_DMR);
}

int
dsd_dispatch_matches_x2tdma(int synctype) {
    return DSD_SYNC_IS_X2TDMA(synctype);
}

void
dsd_dispatch_handle_x2tdma(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_X2TDMA);
}

int
dsd_dispatch_matches_provoice(int synctype) {
    return DSD_SYNC_IS_PROVOICE(synctype);
}

void
dsd_dispatch_handle_provoice(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_PROVOICE);
}

int
dsd_dispatch_matches_edacs(int synctype) {
    return DSD_SYNC_IS_EDACS(synctype);
}

void
dsd_dispatch_handle_edacs(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_EDACS);
}

int
dsd_dispatch_matches_ysf(int synctype) {
    return DSD_SYNC_IS_YSF(synctype);
}

void
dsd_dispatch_handle_ysf(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_YSF);
}

int
dsd_dispatch_matches_m17(int synctype) {
    return DSD_SYNC_IS_M17(synctype);
}

void
dsd_dispatch_handle_m17(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_M17);
}

int
dsd_dispatch_matches_p25p2(int synctype) {
    return DSD_SYNC_IS_P25P2(synctype);
}

void
dsd_dispatch_handle_p25p2(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_P25P2);
}

int
dsd_dispatch_matches_dpmr(int synctype) {
    return DSD_SYNC_IS_DPMR(synctype);
}

void
dsd_dispatch_handle_dpmr(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_DPMR);
}

int
dsd_dispatch_matches_p25p1(int synctype) {
    return DSD_SYNC_IS_P25P1(synctype);
}

void
dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    record_handler(TEST_HANDLER_P25P1);
}

static void
run_dispatch_case(int synctype, int expected_handler) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);
    state->synctype = synctype;
    state->rf_mod = 1;
    state->max = 100.0F;
    state->min = -50.0F;
    g_called_handler = TEST_HANDLER_NONE;

    processFrame(opts, state);

    assert(g_called_handler == expected_handler);
    assert(state->maxref == 80.0F);
    assert(state->minref == -40.0F);
    free(state);
    free(opts);
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

int
main(void) {
    check_public_handler_table();
    run_dispatch_case(DSD_SYNC_DMR_BS_VOICE_POS, TEST_HANDLER_DMR);
    run_dispatch_case(DSD_SYNC_DPMR_FS1_POS, TEST_HANDLER_DPMR);
    run_dispatch_case(DSD_SYNC_P25P1_POS, TEST_HANDLER_P25P1);
    run_dispatch_case(-1, TEST_HANDLER_P25P1);

    printf("ENGINE_PROTOCOL_DISPATCH: OK\n");
    return 0;
}
