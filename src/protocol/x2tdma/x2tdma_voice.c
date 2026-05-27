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

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/x2tdma/x2tdma.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

typedef struct {
    char ambe_fr[4][24];
    char ambe_fr2[4][24];
    char ambe_fr3[4][24];
    char sync[25];
    char syncdata[25];
    char lcformat[9];
    char mfid[9];
    char lcinfo[57];
    char cachdata[13];
    char mi[73];
    int mutecurrentslot;
    int eeei;
    int aiei;
    int msMode;
} x2tdma_voice_ctx;

static int
x2tdma_read_slot_dibit(dsd_opts* opts, dsd_state* state, int j, int** dibit_p) {
    int dibit;

    if (j > 0) {
        return getDibit(opts, state);
    }

    dibit = **dibit_p;
    (*dibit_p)++;
    if (opts->inverted_x2tdma == 1) {
        dibit = (dibit ^ 2);
    }
    return dibit;
}

static void
x2tdma_skip_prev_half(dsd_opts* opts, dsd_state* state, int j, int** dibit_p) {
    int i;

    for (i = 0; i < 54; i++) {
        if (j > 0) {
            (void)getDibit(opts, state);
        } else {
            (*dibit_p)++;
        }
    }
}

static void
x2tdma_update_currentslot_from_cach(dsd_state* state, int dibit) {
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
x2tdma_read_cach_from_slot(dsd_opts* opts, dsd_state* state, int j, int** dibit_p, char cachdata[13]) {
    int i;

    for (i = 0; i < 12; i++) {
        cachdata[i] = x2tdma_read_slot_dibit(opts, state, j, dibit_p);
        if (i == 2) {
            x2tdma_update_currentslot_from_cach(state, cachdata[i]);
        }
    }
    cachdata[12] = 0;
}

static void
x2tdma_read_cach_from_stream(dsd_opts* opts, dsd_state* state, char cachdata[13]) {
    int i;

    for (i = 0; i < 12; i++) {
        cachdata[i] = getDibit(opts, state);
    }
    cachdata[12] = 0;
}

static void
x2tdma_fill_ambe_from_slot(dsd_opts* opts, dsd_state* state, int j, int** dibit_p, char frame[4][24], int start,
                           int count) {
    int i;
    int dibit;
    const int* w = x2tdma_ambe_interleave_w + start;
    const int* x = x2tdma_ambe_interleave_x + start;
    const int* y = x2tdma_ambe_interleave_y + start;
    const int* z = x2tdma_ambe_interleave_z + start;

    for (i = 0; i < count; i++) {
        dibit = x2tdma_read_slot_dibit(opts, state, j, dibit_p);
        frame[*w][*x] = (1 & (dibit >> 1)); // bit 1
        frame[*y][*z] = (1 & dibit);        // bit 0
        w++;
        x++;
        y++;
        z++;
    }
}

static void
x2tdma_fill_ambe_from_stream(dsd_opts* opts, dsd_state* state, char frame[4][24], int start, int count) {
    int i;
    int dibit;
    const int* w = x2tdma_ambe_interleave_w + start;
    const int* x = x2tdma_ambe_interleave_x + start;
    const int* y = x2tdma_ambe_interleave_y + start;
    const int* z = x2tdma_ambe_interleave_z + start;

    for (i = 0; i < count; i++) {
        dibit = getDibit(opts, state);
        frame[*w][*x] = (1 & (dibit >> 1)); // bit 1
        frame[*y][*z] = (1 & dibit);        // bit 0
        w++;
        x++;
        y++;
        z++;
    }
}

static void
x2tdma_read_sync_from_slot(dsd_opts* opts, dsd_state* state, int j, int** dibit_p, char sync[25], char syncdata[25]) {
    for (int i = 0; i < 24; i++) {
        int dibit = x2tdma_read_slot_dibit(opts, state, j, dibit_p);
        syncdata[i] = dibit;
        sync[i] = (dibit | 1) + 48;
    }
    sync[24] = 0;
    syncdata[24] = 0;
}

static void
x2tdma_read_sync_from_stream(dsd_opts* opts, dsd_state* state, char sync[25], char syncdata[25]) {
    for (int i = 0; i < 24; i++) {
        int dibit = getDibit(opts, state);
        syncdata[i] = dibit;
        sync[i] = (dibit | 1) + 48;
    }
    sync[24] = 0;
    syncdata[24] = 0;
}

#ifdef X2TDMA_DUMP
static void
x2tdma_dump_dibits(const char dibits[], int count) {
    int i;
    int k = 0;
    int dibit;
    char bits[49];

    for (i = 0; i < count; i++) {
        dibit = dibits[i];
        bits[k] = (1 & (dibit >> 1)) + 48; // bit 1
        k++;
        bits[k] = (1 & dibit) + 48; // bit 0
        k++;
    }

    bits[k] = 0;
    DSD_FPRINTF(stderr, "%s ", bits);
}
#endif

static void
x2tdma_update_mute_and_lights(x2tdma_voice_ctx* ctx, dsd_state* state) {
    if ((strcmp(ctx->sync, X2TDMA_BS_DATA_SYNC) == 0) || (strcmp(ctx->sync, X2TDMA_MS_DATA_SYNC) == 0)) {
        ctx->mutecurrentslot = 1;
        if (state->currentslot == 0) {
            DSD_SNPRINTF(state->slot0light, sizeof state->slot0light, "%s", "[slot0]");
        } else {
            DSD_SNPRINTF(state->slot1light, sizeof state->slot1light, "%s", "[slot1]");
        }
    } else if ((strcmp(ctx->sync, X2TDMA_BS_VOICE_SYNC) == 0) || (strcmp(ctx->sync, X2TDMA_MS_VOICE_SYNC) == 0)) {
        ctx->mutecurrentslot = 0;
        if (state->currentslot == 0) {
            DSD_SNPRINTF(state->slot0light, sizeof state->slot0light, "%s", "[SLOT0]");
        } else {
            DSD_SNPRINTF(state->slot1light, sizeof state->slot1light, "%s", "[SLOT1]");
        }
    }
}

static void
x2tdma_update_ms_mode(x2tdma_voice_ctx* ctx) {
    if ((strcmp(ctx->sync, X2TDMA_MS_VOICE_SYNC) == 0) || (strcmp(ctx->sync, X2TDMA_MS_DATA_SYNC) == 0)) {
        ctx->msMode = 1;
    }
}

static void
x2tdma_decode_signal_j1(x2tdma_voice_ctx* ctx) {
    ctx->eeei = (1 & ctx->syncdata[1]);        // bit 0
    ctx->aiei = (1 & (ctx->syncdata[2] >> 1)); // bit 1

    if ((ctx->eeei == 0) && (ctx->aiei == 0)) {
        ctx->lcformat[0] = (1 & (ctx->syncdata[4] >> 1)) + 48;  // bit 1
        ctx->mfid[3] = (1 & ctx->syncdata[4]) + 48;             // bit 0
        ctx->lcinfo[6] = (1 & (ctx->syncdata[5] >> 1)) + 48;    // bit 1
        ctx->lcinfo[16] = (1 & ctx->syncdata[5]) + 48;          // bit 0
        ctx->lcinfo[26] = (1 & (ctx->syncdata[6] >> 1)) + 48;   // bit 1
        ctx->lcinfo[36] = (1 & ctx->syncdata[6]) + 48;          // bit 0
        ctx->lcinfo[46] = (1 & (ctx->syncdata[7] >> 1)) + 48;   // bit 1
        ctx->lcformat[1] = (1 & (ctx->syncdata[8] >> 1)) + 48;  // bit 1
        ctx->mfid[4] = (1 & ctx->syncdata[8]) + 48;             // bit 0
        ctx->lcinfo[7] = (1 & (ctx->syncdata[9] >> 1)) + 48;    // bit 1
        ctx->lcinfo[17] = (1 & ctx->syncdata[9]) + 48;          // bit 0
        ctx->lcinfo[27] = (1 & (ctx->syncdata[10] >> 1)) + 48;  // bit 1
        ctx->lcinfo[37] = (1 & ctx->syncdata[10]) + 48;         // bit 0
        ctx->lcinfo[47] = (1 & (ctx->syncdata[11] >> 1)) + 48;  // bit 1
        ctx->lcformat[2] = (1 & (ctx->syncdata[12] >> 1)) + 48; // bit 1
        ctx->mfid[5] = (1 & ctx->syncdata[12]) + 48;            // bit 0
        ctx->lcinfo[8] = (1 & (ctx->syncdata[13] >> 1)) + 48;   // bit 1
        ctx->lcinfo[18] = (1 & ctx->syncdata[13]) + 48;         // bit 0
        ctx->lcinfo[28] = (1 & (ctx->syncdata[14] >> 1)) + 48;  // bit 1
        ctx->lcinfo[38] = (1 & ctx->syncdata[14]) + 48;         // bit 0
        ctx->lcinfo[48] = (1 & (ctx->syncdata[15] >> 1)) + 48;  // bit 1
        ctx->lcformat[3] = (1 & (ctx->syncdata[16] >> 1)) + 48; // bit 1
        ctx->mfid[6] = (1 & ctx->syncdata[16]) + 48;            // bit 0
        ctx->lcinfo[9] = (1 & (ctx->syncdata[17] >> 1)) + 48;   // bit 1
        ctx->lcinfo[19] = (1 & ctx->syncdata[17]) + 48;         // bit 0
        ctx->lcinfo[29] = (1 & (ctx->syncdata[18] >> 1)) + 48;  // bit 1
        ctx->lcinfo[39] = (1 & ctx->syncdata[18]) + 48;         // bit 0
        ctx->lcinfo[49] = (1 & (ctx->syncdata[19] >> 1)) + 48;  // bit 1
    } else {
        ctx->mi[0] = (1 & (ctx->syncdata[4] >> 1)) + 48;   // bit 1
        ctx->mi[11] = (1 & ctx->syncdata[4]) + 48;         // bit 0
        ctx->mi[22] = (1 & (ctx->syncdata[5] >> 1)) + 48;  // bit 1
        ctx->mi[32] = (1 & ctx->syncdata[5]) + 48;         // bit 0
        ctx->mi[42] = (1 & (ctx->syncdata[6] >> 1)) + 48;  // bit 1
        ctx->mi[52] = (1 & ctx->syncdata[6]) + 48;         // bit 0
        ctx->mi[62] = (1 & (ctx->syncdata[7] >> 1)) + 48;  // bit 1
        ctx->mi[1] = (1 & (ctx->syncdata[8] >> 1)) + 48;   // bit 1
        ctx->mi[12] = (1 & ctx->syncdata[8]) + 48;         // bit 0
        ctx->mi[23] = (1 & (ctx->syncdata[9] >> 1)) + 48;  // bit 1
        ctx->mi[33] = (1 & ctx->syncdata[9]) + 48;         // bit 0
        ctx->mi[43] = (1 & (ctx->syncdata[10] >> 1)) + 48; // bit 1
        ctx->mi[53] = (1 & ctx->syncdata[10]) + 48;        // bit 0
        ctx->mi[63] = (1 & (ctx->syncdata[11] >> 1)) + 48; // bit 1
        ctx->mi[2] = (1 & (ctx->syncdata[12] >> 1)) + 48;  // bit 1
        ctx->mi[13] = (1 & ctx->syncdata[12]) + 48;        // bit 0
        ctx->mi[24] = (1 & (ctx->syncdata[13] >> 1)) + 48; // bit 1
        ctx->mi[34] = (1 & ctx->syncdata[13]) + 48;        // bit 0
        ctx->mi[44] = (1 & (ctx->syncdata[14] >> 1)) + 48; // bit 1
        ctx->mi[54] = (1 & ctx->syncdata[14]) + 48;        // bit 0
        ctx->mi[64] = (1 & (ctx->syncdata[15] >> 1)) + 48; // bit 1
        ctx->mi[3] = (1 & (ctx->syncdata[16] >> 1)) + 48;  // bit 1
        ctx->mi[14] = (1 & ctx->syncdata[16]) + 48;        // bit 0
        ctx->mi[25] = (1 & (ctx->syncdata[17] >> 1)) + 48; // bit 1
        ctx->mi[35] = (1 & ctx->syncdata[17]) + 48;        // bit 0
        ctx->mi[45] = (1 & (ctx->syncdata[18] >> 1)) + 48; // bit 1
        ctx->mi[55] = (1 & ctx->syncdata[18]) + 48;        // bit 0
        ctx->mi[65] = (1 & (ctx->syncdata[19] >> 1)) + 48; // bit 1
    }
}

static void
x2tdma_decode_signal_j2(x2tdma_voice_ctx* ctx) {
    if ((ctx->eeei == 0) && (ctx->aiei == 0)) {
        ctx->lcformat[4] = (1 & (ctx->syncdata[4] >> 1)) + 48;  // bit 1
        ctx->mfid[7] = (1 & ctx->syncdata[4]) + 48;             // bit 0
        ctx->lcinfo[10] = (1 & (ctx->syncdata[5] >> 1)) + 48;   // bit 1
        ctx->lcinfo[20] = (1 & ctx->syncdata[5]) + 48;          // bit 0
        ctx->lcinfo[30] = (1 & (ctx->syncdata[6] >> 1)) + 48;   // bit 1
        ctx->lcinfo[40] = (1 & ctx->syncdata[6]) + 48;          // bit 0
        ctx->lcinfo[50] = (1 & (ctx->syncdata[7] >> 1)) + 48;   // bit 1
        ctx->lcformat[5] = (1 & (ctx->syncdata[8] >> 1)) + 48;  // bit 1
        ctx->lcinfo[0] = (1 & ctx->syncdata[8]) + 48;           // bit 0
        ctx->lcinfo[11] = (1 & (ctx->syncdata[9] >> 1)) + 48;   // bit 1
        ctx->lcinfo[21] = (1 & ctx->syncdata[9]) + 48;          // bit 0
        ctx->lcinfo[31] = (1 & (ctx->syncdata[10] >> 1)) + 48;  // bit 1
        ctx->lcinfo[41] = (1 & ctx->syncdata[10]) + 48;         // bit 0
        ctx->lcinfo[51] = (1 & (ctx->syncdata[11] >> 1)) + 48;  // bit 1
        ctx->lcformat[6] = (1 & (ctx->syncdata[12] >> 1)) + 48; // bit 1
        ctx->lcinfo[1] = (1 & ctx->syncdata[12]) + 48;          // bit 0
        ctx->lcinfo[12] = (1 & (ctx->syncdata[13] >> 1)) + 48;  // bit 1
        ctx->lcinfo[22] = (1 & ctx->syncdata[13]) + 48;         // bit 0
        ctx->lcinfo[32] = (1 & (ctx->syncdata[14] >> 1)) + 48;  // bit 1
        ctx->lcinfo[42] = (1 & ctx->syncdata[14]) + 48;         // bit 0
        ctx->lcinfo[52] = (1 & (ctx->syncdata[15] >> 1)) + 48;  // bit 1
        ctx->lcformat[7] = (1 & (ctx->syncdata[16] >> 1)) + 48; // bit 1
        ctx->lcinfo[2] = (1 & ctx->syncdata[16]) + 48;          // bit 0
        ctx->lcinfo[13] = (1 & (ctx->syncdata[17] >> 1)) + 48;  // bit 1
        ctx->lcinfo[23] = (1 & ctx->syncdata[17]) + 48;         // bit 0
        ctx->lcinfo[33] = (1 & (ctx->syncdata[18] >> 1)) + 48;  // bit 1
        ctx->lcinfo[43] = (1 & ctx->syncdata[18]) + 48;         // bit 0
        ctx->lcinfo[53] = (1 & (ctx->syncdata[19] >> 1)) + 48;  // bit 1
    } else {
        ctx->mi[4] = (1 & (ctx->syncdata[4] >> 1)) + 48;   // bit 1
        ctx->mi[15] = (1 & ctx->syncdata[4]) + 48;         // bit 0
        ctx->mi[26] = (1 & (ctx->syncdata[5] >> 1)) + 48;  // bit 1
        ctx->mi[36] = (1 & ctx->syncdata[5]) + 48;         // bit 0
        ctx->mi[46] = (1 & (ctx->syncdata[6] >> 1)) + 48;  // bit 1
        ctx->mi[56] = (1 & ctx->syncdata[6]) + 48;         // bit 0
        ctx->mi[66] = (1 & (ctx->syncdata[7] >> 1)) + 48;  // bit 1
        ctx->mi[5] = (1 & (ctx->syncdata[8] >> 1)) + 48;   // bit 1
        ctx->mi[16] = (1 & ctx->syncdata[8]) + 48;         // bit 0
        ctx->mi[27] = (1 & (ctx->syncdata[9] >> 1)) + 48;  // bit 1
        ctx->mi[37] = (1 & ctx->syncdata[9]) + 48;         // bit 0
        ctx->mi[47] = (1 & (ctx->syncdata[10] >> 1)) + 48; // bit 1
        ctx->mi[57] = (1 & ctx->syncdata[10]) + 48;        // bit 0
        ctx->mi[67] = (1 & (ctx->syncdata[11] >> 1)) + 48; // bit 1
        ctx->mi[6] = (1 & (ctx->syncdata[12] >> 1)) + 48;  // bit 1
        ctx->mi[17] = (1 & ctx->syncdata[12]) + 48;        // bit 0
        ctx->mi[28] = (1 & (ctx->syncdata[13] >> 1)) + 48; // bit 1
        ctx->mi[38] = (1 & ctx->syncdata[13]) + 48;        // bit 0
        ctx->mi[48] = (1 & (ctx->syncdata[14] >> 1)) + 48; // bit 1
        ctx->mi[58] = (1 & ctx->syncdata[14]) + 48;        // bit 0
        ctx->mi[68] = (1 & (ctx->syncdata[15] >> 1)) + 48; // bit 1
        ctx->mi[7] = (1 & (ctx->syncdata[16] >> 1)) + 48;  // bit 1
        ctx->mi[18] = (1 & ctx->syncdata[16]) + 48;        // bit 0
        ctx->mi[29] = (1 & (ctx->syncdata[17] >> 1)) + 48; // bit 1
        ctx->mi[39] = (1 & ctx->syncdata[17]) + 48;        // bit 0
        ctx->mi[49] = (1 & (ctx->syncdata[18] >> 1)) + 48; // bit 1
        ctx->mi[59] = (1 & ctx->syncdata[18]) + 48;        // bit 0
        ctx->mi[69] = (1 & (ctx->syncdata[19] >> 1)) + 48; // bit 1
    }
}

static void
x2tdma_decode_signal_j3(x2tdma_voice_ctx* ctx, dsd_state* state) {
    int burstd = (1 & ctx->syncdata[1]); // bit 0

    state->algid[0] = (1 & (ctx->syncdata[4] >> 1)) + 48; // bit 1
    state->algid[1] = (1 & ctx->syncdata[4]) + 48;        // bit 0
    state->algid[2] = (1 & (ctx->syncdata[5] >> 1)) + 48; // bit 1
    state->algid[3] = (1 & ctx->syncdata[5]) + 48;        // bit 0
    if (burstd == 0) {
        state->algid[4] = (1 & (ctx->syncdata[8] >> 1)) + 48; // bit 1
        state->algid[5] = (1 & ctx->syncdata[8]) + 48;        // bit 0
        state->algid[6] = (1 & (ctx->syncdata[9] >> 1)) + 48; // bit 1
        state->algid[7] = (1 & ctx->syncdata[9]) + 48;        // bit 0

        state->keyid[0] = (1 & (ctx->syncdata[10] >> 1)) + 48;  // bit 1
        state->keyid[1] = (1 & ctx->syncdata[10]) + 48;         // bit 0
        state->keyid[2] = (1 & (ctx->syncdata[11] >> 1)) + 48;  // bit 1
        state->keyid[3] = (1 & ctx->syncdata[11]) + 48;         // bit 0
        state->keyid[4] = (1 & (ctx->syncdata[12] >> 1)) + 48;  // bit 1
        state->keyid[5] = (1 & ctx->syncdata[12]) + 48;         // bit 0
        state->keyid[6] = (1 & (ctx->syncdata[13] >> 1)) + 48;  // bit 1
        state->keyid[7] = (1 & ctx->syncdata[13]) + 48;         // bit 0
        state->keyid[8] = (1 & (ctx->syncdata[14] >> 1)) + 48;  // bit 1
        state->keyid[9] = (1 & ctx->syncdata[14]) + 48;         // bit 0
        state->keyid[10] = (1 & (ctx->syncdata[15] >> 1)) + 48; // bit 1
        state->keyid[11] = (1 & ctx->syncdata[15]) + 48;        // bit 0
        state->keyid[12] = (1 & (ctx->syncdata[16] >> 1)) + 48; // bit 1
        state->keyid[13] = (1 & ctx->syncdata[16]) + 48;        // bit 0
        state->keyid[14] = (1 & (ctx->syncdata[17] >> 1)) + 48; // bit 1
        state->keyid[15] = (1 & ctx->syncdata[17]) + 48;        // bit 0
    } else {
        DSD_SNPRINTF(state->algid, sizeof(state->algid), "________");
        DSD_SNPRINTF(state->keyid, sizeof(state->keyid), "________________");
    }
}

static void
x2tdma_decode_signal_j4(x2tdma_voice_ctx* ctx) {
    if ((ctx->eeei == 0) && (ctx->aiei == 0)) {
        ctx->mfid[0] = (1 & (ctx->syncdata[4] >> 1)) + 48;     // bit 1
        ctx->lcinfo[3] = (1 & ctx->syncdata[4]) + 48;          // bit 0
        ctx->lcinfo[14] = (1 & (ctx->syncdata[5] >> 1)) + 48;  // bit 1
        ctx->lcinfo[24] = (1 & ctx->syncdata[5]) + 48;         // bit 0
        ctx->lcinfo[34] = (1 & (ctx->syncdata[6] >> 1)) + 48;  // bit 1
        ctx->lcinfo[44] = (1 & ctx->syncdata[6]) + 48;         // bit 0
        ctx->lcinfo[54] = (1 & (ctx->syncdata[7] >> 1)) + 48;  // bit 1
        ctx->mfid[1] = (1 & (ctx->syncdata[8] >> 1)) + 48;     // bit 1
        ctx->lcinfo[4] = (1 & ctx->syncdata[8]) + 48;          // bit 0
        ctx->lcinfo[15] = (1 & (ctx->syncdata[9] >> 1)) + 48;  // bit 1
        ctx->lcinfo[25] = (1 & ctx->syncdata[9]) + 48;         // bit 0
        ctx->lcinfo[35] = (1 & (ctx->syncdata[10] >> 1)) + 48; // bit 1
        ctx->lcinfo[45] = (1 & ctx->syncdata[10]) + 48;        // bit 0
        ctx->lcinfo[55] = (1 & (ctx->syncdata[11] >> 1)) + 48; // bit 1
        ctx->mfid[2] = (1 & (ctx->syncdata[12] >> 1)) + 48;    // bit 1
        ctx->lcinfo[5] = (1 & ctx->syncdata[12]) + 48;         // bit 0
    } else {
        ctx->mi[8] = (1 & (ctx->syncdata[4] >> 1)) + 48;   // bit 1
        ctx->mi[19] = (1 & ctx->syncdata[4]) + 48;         // bit 0
        ctx->mi[30] = (1 & (ctx->syncdata[5] >> 1)) + 48;  // bit 1
        ctx->mi[40] = (1 & ctx->syncdata[5]) + 48;         // bit 0
        ctx->mi[50] = (1 & (ctx->syncdata[6] >> 1)) + 48;  // bit 1
        ctx->mi[60] = (1 & ctx->syncdata[6]) + 48;         // bit 0
        ctx->mi[70] = (1 & (ctx->syncdata[7] >> 1)) + 48;  // bit 1
        ctx->mi[9] = (1 & (ctx->syncdata[8] >> 1)) + 48;   // bit 1
        ctx->mi[20] = (1 & ctx->syncdata[8]) + 48;         // bit 0
        ctx->mi[31] = (1 & (ctx->syncdata[9] >> 1)) + 48;  // bit 1
        ctx->mi[41] = (1 & ctx->syncdata[9]) + 48;         // bit 0
        ctx->mi[51] = (1 & (ctx->syncdata[10] >> 1)) + 48; // bit 1
        ctx->mi[61] = (1 & ctx->syncdata[10]) + 48;        // bit 0
        ctx->mi[71] = (1 & (ctx->syncdata[11] >> 1)) + 48; // bit 1
        ctx->mi[10] = (1 & (ctx->syncdata[12] >> 1)) + 48; // bit 1
        ctx->mi[21] = (1 & ctx->syncdata[12]) + 48;        // bit 0
    }
}

static void
x2tdma_decode_signaling(int j, x2tdma_voice_ctx* ctx, dsd_state* state) {
    switch (j) {
        case 1: x2tdma_decode_signal_j1(ctx); break;
        case 2: x2tdma_decode_signal_j2(ctx); break;
        case 3: x2tdma_decode_signal_j3(ctx, state); break;
        case 4: x2tdma_decode_signal_j4(ctx); break;
        default: break;
    }
}

static void
x2tdma_process_voice_frames(dsd_opts* opts, dsd_state* state, x2tdma_voice_ctx* ctx) {
    x2tdma_fill_ambe_from_stream(opts, state, ctx->ambe_fr2, 18, 18);

    if (ctx->mutecurrentslot == 0) {
        if (state->firstframe == 1) {
            // We don't know if anything before first sync after no carrier is valid.
            state->firstframe = 0;
        } else {
            soft_mbe(opts, state, NULL, ctx->ambe_fr, NULL);
            soft_mbe(opts, state, NULL, ctx->ambe_fr2, NULL);
        }
    }

    x2tdma_fill_ambe_from_stream(opts, state, ctx->ambe_fr3, 0, 36);
    if (ctx->mutecurrentslot == 0) {
        soft_mbe(opts, state, NULL, ctx->ambe_fr3, NULL);
    }
}

static void
x2tdma_update_next_slot_lights(const x2tdma_voice_ctx* ctx, dsd_state* state) {
    if ((strcmp(ctx->sync, X2TDMA_BS_DATA_SYNC) == 0) || (ctx->msMode == 1)) {
        if (state->currentslot == 0) {
            DSD_SNPRINTF(state->slot1light, sizeof(state->slot1light), " slot1 ");
        } else {
            DSD_SNPRINTF(state->slot0light, sizeof(state->slot0light), " slot0 ");
        }
    } else if (strcmp(ctx->sync, X2TDMA_BS_VOICE_SYNC) == 0) {
        if (state->currentslot == 0) {
            DSD_SNPRINTF(state->slot1light, sizeof(state->slot1light), " SLOT1 ");
        } else {
            DSD_SNPRINTF(state->slot0light, sizeof(state->slot0light), " SLOT0 ");
        }
    }
}

static void
x2tdma_process_slot_iteration(dsd_opts* opts, dsd_state* state, x2tdma_voice_ctx* ctx, int j, int** dibit_p) {
    x2tdma_skip_prev_half(opts, state, j, dibit_p);
    x2tdma_read_cach_from_slot(opts, state, j, dibit_p, ctx->cachdata);

#ifdef X2TDMA_DUMP
    x2tdma_dump_dibits(ctx->cachdata, 12);
#endif

    x2tdma_fill_ambe_from_slot(opts, state, j, dibit_p, ctx->ambe_fr, 0, 36);
    x2tdma_fill_ambe_from_slot(opts, state, j, dibit_p, ctx->ambe_fr2, 0, 18);

    x2tdma_read_sync_from_slot(opts, state, j, dibit_p, ctx->sync, ctx->syncdata);
    x2tdma_update_mute_and_lights(ctx, state);
    x2tdma_update_ms_mode(ctx);

    if ((j == 0) && (opts->errorbars == 1)) {
        DSD_FPRINTF(stderr, "%s %s  VOICE e:", state->slot0light, state->slot1light);
    }

#ifdef X2TDMA_DUMP
    x2tdma_dump_dibits(ctx->syncdata, 24);
#endif

    x2tdma_decode_signaling(j, ctx, state);
    x2tdma_process_voice_frames(opts, state, ctx);

    x2tdma_read_cach_from_stream(opts, state, ctx->cachdata);
#ifdef X2TDMA_DUMP
    x2tdma_dump_dibits(ctx->cachdata, 12);
#endif

    skipDibit(opts, state, 54);
    x2tdma_read_sync_from_stream(opts, state, ctx->sync, ctx->syncdata);
    x2tdma_update_next_slot_lights(ctx, state);

#ifdef X2TDMA_DUMP
    x2tdma_dump_dibits(ctx->syncdata, 24);
#endif

    if (j == 5) {
        skipDibit(opts, state, 54);
        skipDibit(opts, state, 12);
        skipDibit(opts, state, 54);
    }
}

void
processX2TDMAvoice(dsd_opts* opts, dsd_state* state) {
    // extracts AMBE frames from X2TDMA frame
    int j;
    int* dibit_p;
    x2tdma_voice_ctx ctx;

    ctx.lcformat[8] = 0;
    ctx.mfid[8] = 0;
    ctx.lcinfo[56] = 0;
    ctx.mutecurrentslot = 0;
    ctx.msMode = 0;

    dibit_p = state->dibit_buf_p - 144;
    for (j = 0; j < 6; j++) {
        x2tdma_process_slot_iteration(opts, state, &ctx, j, &dibit_p);
    }

    if (opts->errorbars == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    if (ctx.mutecurrentslot == 0) {
        if (opts->p25enc == 1) {
            int algidhex = strtol(state->algid, NULL, 2);
            int kidhex = strtol(state->keyid, NULL, 2);
            DSD_FPRINTF(stderr, "mi: %s algid: $%x kid: $%x\n", ctx.mi, algidhex, kidhex);
        }
    }
}
