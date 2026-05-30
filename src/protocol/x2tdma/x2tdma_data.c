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

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/x2tdma/x2tdma.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "x2tdma_frame.h"

static int
x2tdma_read_dibit(const dsd_opts* opts, int** dibit_p) {
    int dibit = **dibit_p;
    (*dibit_p)++;
    if (opts->inverted_x2tdma == 1) {
        dibit ^= 2;
    }
    return dibit;
}

static void
x2tdma_update_slot_light_from_cach(dsd_state* state, int dibit) {
    state->currentslot = (1 & (dibit >> 1)); // bit 1
    if (state->currentslot == 0) {
        state->slot0light[0] = '[';
        state->slot0light[6] = ']';
        state->slot1light[0] = ' ';
        state->slot1light[6] = ' ';
    } else {
        state->slot1light[0] = '[';
        state->slot1light[6] = ']';
        state->slot0light[0] = ' ';
        state->slot0light[6] = ' ';
    }
}

static void
x2tdma_read_cach(const dsd_opts* opts, dsd_state* state, int** dibit_p, char cachdata[13]) {
    for (int i = 0; i < 12; i++) {
        int dibit = x2tdma_read_dibit(opts, dibit_p);
        cachdata[i] = dibit;
        if (i == 2) {
            x2tdma_update_slot_light_from_cach(state, dibit);
        }
    }
    cachdata[12] = 0;
}

#ifdef X2TDMA_DUMP
static void
x2tdma_dump_dibit_bits(const char* dibits, int dibit_count) {
    int k = 0;
    char bits[49];

    for (int i = 0; i < dibit_count; i++) {
        int dibit = dibits[i];
        bits[k] = (1 & (dibit >> 1)) + 48; // bit 1
        k++;
        bits[k] = (1 & dibit) + 48; // bit 0
        k++;
    }
    bits[dibit_count * 2] = 0;
    DSD_FPRINTF(stderr, "%s ", bits);
}
#endif

static void
x2tdma_read_slot_type(const dsd_opts* opts, int** dibit_p, char cc[4], char bursttype[5]) {
    int dibit = x2tdma_read_dibit(opts, dibit_p);
    cc[0] = (1 & (dibit >> 1)) + 48; // bit 1
    cc[1] = (1 & dibit) + 48;        // bit 0

    dibit = x2tdma_read_dibit(opts, dibit_p);
    cc[2] = (1 & (dibit >> 1)) + 48; // bit 1
    (void)(1 & dibit);               // bit 0 (unused)

    dibit = x2tdma_read_dibit(opts, dibit_p);
    bursttype[0] = (1 & (dibit >> 1)) + 48; // bit 1
    bursttype[1] = (1 & dibit) + 48;        // bit 0

    dibit = x2tdma_read_dibit(opts, dibit_p);
    bursttype[2] = (1 & (dibit >> 1)) + 48; // bit 1
    bursttype[3] = (1 & dibit) + 48;        // bit 0
}

static void
x2tdma_set_fsubtype_from_bursttype(dsd_state* state, const char bursttype[5]) {
    static const struct {
        const char* bursttype;
        const char* subtype;
    } bursttype_map[] = {
        {"0000", " PI Header    "}, {"0001", " VOICE Header "}, {"0010", " TLC          "}, {"0011", " CSBK         "},
        {"0100", " MBC Header   "}, {"0101", " MBC          "}, {"0110", " DATA Header  "}, {"0111", " RATE 1/2 DATA"},
        {"1000", " RATE 3/4 DATA"}, {"1001", " Slot idle    "}, {"1010", " Rate 1 DATA  "},
    };

    for (int i = 0; i < (int)(sizeof bursttype_map / sizeof bursttype_map[0]); i++) {
        if (strcmp(bursttype, bursttype_map[i].bursttype) == 0) {
            DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), "%s", bursttype_map[i].subtype);
            return;
        }
    }

    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), "              ");
}

static void
x2tdma_read_sync(const dsd_opts* opts, int** dibit_p, char sync[25], char syncdata[25]) {
    for (int i = 0; i < 24; i++) {
        int dibit = x2tdma_read_dibit(opts, dibit_p);
        syncdata[i] = dibit;
        sync[i] = (dibit | 1) + 48;
    }
    sync[24] = 0;
    syncdata[24] = 0;
}

static void
x2tdma_mark_slot_on_data_sync(dsd_state* state, const char sync[25]) {
    if (dsd_x2tdma_sync_is_data(sync)) {
        if (state->currentslot == 0) {
            DSD_SNPRINTF(state->slot0light, sizeof state->slot0light, "%s", "[slot0]");
        } else {
            DSD_SNPRINTF(state->slot1light, sizeof state->slot1light, "%s", "[slot1]");
        }
    }
}

static void
x2tdma_print_burst_type_status(const dsd_opts* opts, const dsd_state* state, const char bursttype[5]) {
    if (opts->errorbars == 1) {
        if (strcmp(state->fsubtype, "              ") == 0) {
            DSD_FPRINTF(stderr, " Unknown burst type: %s\n", bursttype);
        } else {
            DSD_FPRINTF(stderr, "%s\n", state->fsubtype);
        }
    }
}

void
processX2TDMAdata(dsd_opts* opts, dsd_state* state) {
    int* dibit_p;
    char sync[25];
    char syncdata[25];
    char cachdata[13];
    char cc[4];
    int aiei;
    char bursttype[5];
    UNUSED4(syncdata, cachdata, cc, aiei);

    cc[3] = 0;
    bursttype[4] = 0;
    dibit_p = state->dibit_buf_p - 90;

    // CACH
    x2tdma_read_cach(opts, state, &dibit_p, cachdata);

#ifdef X2TDMA_DUMP
    x2tdma_dump_dibit_bits(cachdata, 12);
#endif

    // current slot
    dibit_p += 49;

    // slot type
    x2tdma_read_slot_type(opts, &dibit_p, cc, bursttype);

    // parity bit
    dibit_p++;

    x2tdma_set_fsubtype_from_bursttype(state, bursttype);

    // signaling data or sync
    x2tdma_read_sync(opts, &dibit_p, sync, syncdata);

#ifdef X2TDMA_DUMP
    x2tdma_dump_dibit_bits(syncdata, 24);
#endif

    x2tdma_mark_slot_on_data_sync(state, sync);

    if (opts->errorbars == 1) {
        DSD_FPRINTF(stderr, "%s %s ", state->slot0light, state->slot1light);
    }

    // current slot second half, cach, next slot 1st half
    skipDibit(opts, state, 120);

    x2tdma_print_burst_type_status(opts, state, bursttype);
}
