// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

//simplified DSTAR
void
processDSTAR(dsd_opts* opts, dsd_state* state) {
    uint8_t sd[480];
    DSD_MEMSET(sd, 0, sizeof(sd));
    int i, j;
    char ambe_fr[4][24];
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));

    //20 voice and 19 slow data frames (20th is frame sync)
    for (j = 0; j < 21; j++) {

        DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
        const int* w = dstar_interleave_w;
        const int* x = dstar_interleave_x;

        for (i = 0; i < 72; i++) {
            int dibit = getDibit(opts, state);
            ambe_fr[*w][*x] = dibit & 1;
            w++;
            x++;
        }

        soft_mbe(opts, state, NULL, ambe_fr, NULL);

        if (j != 20) {
            for (i = 0; i < 24; i++) {
                //slow data
                sd[(j * 24) + i] = (uint8_t)getDibit(opts, state);
            }
        }

        if (j == 20) {
            processDSTAR_SD(opts, state, sd);
        }

        //since we are in a long loop, use this to improve response time in ncurses
        if (dsd_opts_frontend_active(opts)) {
            dsd_telemetry_publish_both_and_redraw(opts, state);
        }

        //slot 1
        watchdog_event_history(opts, state, 0);
        watchdog_event_current(opts, state, 0);
    }

    DSD_FPRINTF(stderr, "\n");
}

void
processDSTAR_HD(dsd_opts* opts, dsd_state* state) {

    int i;
    float soft_symbols[660];

    // Capture soft symbols for soft-decision decoding
    for (i = 0; i < 660; i++) {
        getDibitAndSoftSymbol(opts, state, &soft_symbols[i]);
    }

    dstar_header_decode_soft(state, soft_symbols);
    processDSTAR(opts, state);
}
