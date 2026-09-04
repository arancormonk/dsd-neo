// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
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
#include "dstar_confirm.h"

//simplified DSTAR
int
processDSTAR(dsd_opts* opts, dsd_state* state) {
    uint8_t sd[480];
    DSD_MEMSET(sd, 0, sizeof(sd));
    int i, j;
    char ambe_fr[4][24];
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));

    /* The superframe sits behind an exact 24-symbol sync word, which is the only thing about
     * it that can be checked from here; a CRC-16 in the slow data below may yet prove it
     * outright. Weak on its own, so it has to repeat -- see dstar_confirm.h. */
    dstar_confirm_begin_frame(state);
    dstar_confirm_note_evidence(state, DSTAR_EVIDENCE_WEAK);

    //20 voice and 19 slow data frames (20th is frame sync)
    for (j = 0; j < 21; j++) {

        DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
        const int* w = dstar_interleave_w;
        const int* x = dstar_interleave_x;

        for (i = 0; i < 72; i++) {
            int dibit = get_dibit_and_analog_signal(opts, state, NULL);
            ambe_fr[*w][*x] = dibit & 1;
            w++;
            x++;
        }

        processMbeFrame(opts, state, NULL, ambe_fr, NULL);
        dsd_play_synthesized_voice(opts, state);

        if (j != 20) {
            for (i = 0; i < 24; i++) {
                //slow data
                sd[(j * 24) + i] = (uint8_t)get_dibit_and_analog_signal(opts, state, NULL);
            }
        }

        if (j == 20) {
            processDSTAR_SD(opts, state, sd);
        }

        //since we are in a long loop, use this to improve response time in ncurses
        if (dsd_telemetry_is_active()) {
            dsd_telemetry_publish_both_and_redraw(opts, state);
        }

        //slot 1
        dsd_event_sync_slot(opts, state, 0);
    }

    DSD_FPRINTF(stderr, "\n");

    dstar_confirm_end_frame(state);
    return dstar_confirm_is_confirmed(state);
}

int
processDSTAR_HD(dsd_opts* opts, dsd_state* state) {

    int i;
    float soft_symbols[660];

    // Capture soft symbols for soft-decision decoding
    for (i = 0; i < 660; i++) {
        getDibitAndSoftSymbol(opts, state, &soft_symbols[i]);
    }

    const int header_ok = dstar_header_decode_soft(state, soft_symbols);
    if (header_ok) {
        /* A CRC-16/X.25 over 39 octets: proof on its own, and it opens the transmission the
         * voice superframe below belongs to. */
        dstar_confirm_note_evidence(state, DSTAR_EVIDENCE_STRONG);
    }
    (void)processDSTAR(opts, state);
    return header_ok;
}
