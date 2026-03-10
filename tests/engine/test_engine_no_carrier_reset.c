// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#define DSD_NEO_MAIN
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#undef DSD_NEO_MAIN

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/frame_processing.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <stdio.h>
#include <string.h>

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

int
ui_start(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

void
ui_stop(void) {}

int
main(void) {
    int rc = 0;

    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    initOpts(&opts);
    initState(&state);

    for (int i = 0; i < 200; i++) {
        state.dmr_payload_buf[i] = 0x7F7F7F7F;
        if (state.dmr_reliab_buf != NULL) {
            state.dmr_reliab_buf[i] = 0xA5U;
        }
    }

    state.dmr_payload_p = state.dibit_buf + 321;
    if (state.dmr_reliab_buf != NULL) {
        state.dmr_reliab_p = state.dmr_reliab_buf + 321;
    }

    noCarrier(&opts, &state);

    rc |= expect_true("dmr-payload-pointer-buffer", state.dmr_payload_p == state.dmr_payload_buf + 200);
    rc |= expect_true("dmr-payload-pointer-not-dibit", state.dmr_payload_p != state.dibit_buf + 200);
    rc |= expect_true("dibit-pointer-reset", state.dibit_buf_p == state.dibit_buf + 200);

    for (int i = 0; i < 200; i++) {
        if (state.dmr_payload_buf[i] != 0) {
            fprintf(stderr, "dmr payload buf[%d] not reset: %d\n", i, state.dmr_payload_buf[i]);
            rc = 1;
            break;
        }
    }

    if (state.dmr_reliab_buf != NULL) {
        rc |= expect_true("dmr-reliab-pointer-buffer", state.dmr_reliab_p == state.dmr_reliab_buf + 200);
        for (int i = 0; i < 200; i++) {
            if (state.dmr_reliab_buf[i] != 0U) {
                fprintf(stderr, "dmr reliab buf[%d] not reset: %u\n", i, (unsigned)state.dmr_reliab_buf[i]);
                rc = 1;
                break;
            }
        }
    }

    freeState(&state);

    if (rc == 0) {
        printf("ENGINE_NO_CARRIER_RESET: OK\n");
    }
    return rc;
}
