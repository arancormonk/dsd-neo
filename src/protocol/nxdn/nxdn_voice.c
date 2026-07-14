// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

//Originally found at - https://github.com/LouisErigHerve/dsd
//Modified for use in DSD-FME

#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/nxdn/nxdn_voice.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
nxdn_voice(dsd_opts* opts, dsd_state* state, int voice, uint8_t dbuf[182], const uint8_t* dbuf_reliab) {
    int i;
    int start = 0, stop = 0;
    char ambe_fr[4][24];
    dsd_vocoder_soft_bit ambe_soft_fr[4][24];

    //these conditions will determine our starting and stopping value for voice
    //i.e. voice in first, voice in second, or voice in both
    if (voice == 1 || voice == 3) {
        start = 0;
    }
    if (voice == 1) {
        stop = 2;
    }
    if (voice == 2) {
        start = 2;
    }
    if (voice == 2 || voice == 3) {
        stop = 4;
    }

    for (; start < stop; start++) {
        DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
        DSD_MEMSET(ambe_soft_fr, 0, sizeof(ambe_soft_fr));

        for (i = 0; i < DSD_AMBE_2450_DIBITS; i++) {
            const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[i];
            int buf_idx = i + 38 + start * 36;

            //skip 8 lich and 30 sacch dibits already in buffer plus 36 times start position
            ambe_fr[map->high_row][map->high_col] = dbuf[buf_idx] >> 1;
            ambe_fr[map->low_row][map->low_col] = dbuf[buf_idx] & 1;

            if (dbuf_reliab != NULL) {
                ambe_soft_fr[map->high_row][map->high_col].bit = (uint8_t)(ambe_fr[map->high_row][map->high_col] & 1);
                ambe_soft_fr[map->high_row][map->high_col].reliability = dbuf_reliab[buf_idx];
                ambe_soft_fr[map->low_row][map->low_col].bit = (uint8_t)(ambe_fr[map->low_row][map->low_col] & 1);
                ambe_soft_fr[map->low_row][map->low_col].reliability = dbuf_reliab[buf_idx];
            }
        }
        if (dbuf_reliab != NULL) {
            processMbeFrameSoft(opts, state, NULL, ambe_soft_fr, NULL);
        } else {
            processMbeFrame(opts, state, NULL, ambe_fr, NULL);
        }

        DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));

        if (opts->floating_point == 0) {
            playSynthesizedVoiceMS(opts, state);
        }
        if (opts->floating_point == 1) {
            playSynthesizedVoiceFM(opts, state);
        }
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }
}
