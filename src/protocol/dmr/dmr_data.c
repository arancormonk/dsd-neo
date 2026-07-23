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
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_cach.h"
#include "dmr_confidence.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RADIO
#endif

typedef struct dmr_data_sync_ctx_s {
    dsd_opts* opts;
    dsd_state* state;
    int* dibit_p;
    dsd_dibit_soft_t* soft_p;
    char sync[25];
    char syncdata[48];
    uint8_t cachdata[25];
    uint8_t info[196];
    uint8_t rel98[98];
    unsigned char SlotType[20];
    uint8_t burst;
    unsigned int SlotTypeOk;
    int cach_okay;
    int confidence_pending;
    int confidence_reject;
} dmr_data_sync_ctx;

static void
dmr_data_sync_init_ctx(dmr_data_sync_ctx* ctx, dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->opts = opts;
    ctx->state = state;
    ctx->dibit_p = state->dmr_payload_p - 90;
    ctx->soft_p = NULL;
    if (state->dmr_soft_buf && state->dmr_soft_p) {
        ctx->soft_p = state->dmr_soft_p - 90;
    }
    ctx->cach_okay = -1;
}

static int
dmr_data_read_cached_dibit(dmr_data_sync_ctx* ctx, int stereo_idx, int advance_rel, uint8_t* rel_value,
                           uint8_t rel_default) {
    int dibit;
    uint8_t reliability = rel_default;

    if (ctx->state->dmr_stereo == 1) {
        dibit = (int)ctx->state->dmr_stereo_payload[stereo_idx];
        reliability = ctx->state->dmr_stereo_reliab[stereo_idx];
    } else {
        dibit = *ctx->dibit_p;
        ctx->dibit_p++;
        if (advance_rel != 0 && ctx->soft_p != NULL) {
            reliability = ctx->soft_p->reliability;
            ctx->soft_p++;
        }
        if (ctx->opts->inverted_dmr == 1) {
            dibit ^= 2;
        }
        ctx->state->dmr_stereo_payload[stereo_idx] = dibit;
        if (advance_rel != 0) {
            ctx->state->dmr_stereo_reliab[stereo_idx] = reliability;
        }
    }
    if (advance_rel != 0 && rel_value != NULL) {
        *rel_value = reliability;
    }
    return dibit;
}

static int
dmr_data_read_live_dibit(dmr_data_sync_ctx* ctx, int stereo_idx, uint8_t* rel_value) {
    int dibit;
    if (ctx->state->dmr_stereo == 0) {
        dsd_dibit_soft_t soft;
        dibit = getDibitSoft(ctx->opts, ctx->state, &soft);
        ctx->state->dmr_stereo_payload[stereo_idx] = dibit;
        ctx->state->dmr_stereo_reliab[stereo_idx] = soft.reliability;
        if (rel_value != NULL) {
            *rel_value = soft.reliability;
        }
    } else {
        dibit = (int)ctx->state->dmr_stereo_payload[stereo_idx];
        if (rel_value != NULL) {
            *rel_value = ctx->state->dmr_stereo_reliab[stereo_idx];
        }
    }
    return dibit;
}

static int
dmr_data_collect_cach(dmr_data_sync_ctx* ctx) {
    uint8_t tact_bits[7];
    int i;
    int dibit;

    for (i = 0; i < 12; i++) {
        dibit = dmr_data_read_cached_dibit(ctx, i, 1, NULL, 0);
        ctx->cachdata[dmr_cach_interleave[((size_t)i * 2)]] = (uint8_t)(1 & (dibit >> 1));
        ctx->cachdata[dmr_cach_interleave[((size_t)i * 2) + 1]] = (uint8_t)(1 & dibit);
    }

    for (i = 0; i < 7; i++) {
        tact_bits[i] = ctx->cachdata[i];
    }

    if (!Hamming_7_4_decode(tact_bits)) {
        ctx->cach_okay = -1;
        ctx->SlotTypeOk = 0;
        return 0;
    }

    ctx->cach_okay = 1;
    ctx->state->currentslot = tact_bits[1];
    if (ctx->state->dmr_ms_mode == 1) {
        ctx->state->currentslot = 0;
    }
    return 1;
}

static void
dmr_data_collect_first_half(dmr_data_sync_ctx* ctx) {
    int i;
    int dibit;
    for (i = 0; i < 49; i++) {
        dibit = dmr_data_read_cached_dibit(ctx, i + 12, 1, &ctx->rel98[i], 200);
        ctx->info[((size_t)2) * i] = (uint8_t)(1 & (dibit >> 1));
        ctx->info[((size_t)2 * i) + 1] = (uint8_t)(1 & dibit);
    }
}

static void
dmr_data_collect_slot_type_prefix(dmr_data_sync_ctx* ctx) {
    int dibit;
    dibit = dmr_data_read_cached_dibit(ctx, 61, 1, NULL, 0);
    ctx->SlotType[0] = (unsigned char)(1 & (dibit >> 1));
    ctx->SlotType[1] = (unsigned char)(1 & dibit);

    dibit = dmr_data_read_cached_dibit(ctx, 62, 1, NULL, 0);
    ctx->SlotType[2] = (unsigned char)(1 & (dibit >> 1));
    ctx->SlotType[3] = (unsigned char)(1 & dibit);

    dibit = dmr_data_read_cached_dibit(ctx, 63, 1, NULL, 0);
    ctx->SlotType[4] = (unsigned char)(1 & (dibit >> 1));
    ctx->SlotType[5] = (unsigned char)(1 & dibit);

    dibit = dmr_data_read_cached_dibit(ctx, 64, 1, NULL, 0);
    ctx->SlotType[6] = (unsigned char)(1 & (dibit >> 1));
    ctx->SlotType[7] = (unsigned char)(1 & dibit);

    dibit = dmr_data_read_cached_dibit(ctx, 65, 0, NULL, 0);
    ctx->SlotType[8] = (unsigned char)(1 & (dibit >> 1));
    ctx->SlotType[9] = (unsigned char)(1 & dibit);
}

static void
dmr_data_collect_sync(dmr_data_sync_ctx* ctx) {
    int i;
    int dibit;
    for (i = 0; i < 24; i++) {
        dibit = dmr_data_read_cached_dibit(ctx, i + 66, 1, NULL, 0);
        ctx->syncdata[((size_t)2) * i] = (char)(1 & (dibit >> 1));
        ctx->syncdata[((size_t)2 * i) + 1] = (char)(1 & dibit);
        ctx->sync[i] = (char)((dibit | 1) + 48);
    }
    ctx->sync[24] = 0;

    if (strcmp(ctx->sync, DMR_BS_DATA_SYNC) == 0) {
        if (ctx->state->currentslot == 0) {
            DSD_SNPRINTF(ctx->state->slot1light, sizeof(ctx->state->slot1light), "[slot1]");
            DSD_SNPRINTF(ctx->state->slot2light, sizeof(ctx->state->slot2light), " slot2 ");
        } else {
            DSD_SNPRINTF(ctx->state->slot1light, sizeof(ctx->state->slot1light), " slot1 ");
            DSD_SNPRINTF(ctx->state->slot2light, sizeof(ctx->state->slot2light), "[slot2]");
        }
    } else if (strcmp(ctx->sync, DMR_DIRECT_MODE_TS1_DATA_SYNC) == 0) {
        ctx->state->currentslot = 0;
        DSD_SNPRINTF(ctx->state->slot1light, sizeof(ctx->state->slot1light), "[sLoT1]");
        DSD_SNPRINTF(ctx->state->slot2light, sizeof(ctx->state->slot2light), "[DMODE]");
    } else if (strcmp(ctx->sync, DMR_DIRECT_MODE_TS2_DATA_SYNC) == 0) {
        ctx->state->currentslot = 1;
        DSD_SNPRINTF(ctx->state->slot1light, sizeof(ctx->state->slot1light), "[DMODE]");
        DSD_SNPRINTF(ctx->state->slot2light, sizeof(ctx->state->slot2light), "[sLoT2]");
    }

    if (ctx->state->dmr_ms_mode == 0) {
        DSD_FPRINTF(stderr, "%s %s ", ctx->state->slot1light, ctx->state->slot2light);
    }
}

static int
dmr_data_collect_slot_type_suffix(dmr_data_sync_ctx* ctx) {
    int i;
    int dibit;
    for (i = 0; i < 5; i++) {
        dibit = dmr_data_read_live_dibit(ctx, i + 90, NULL);
        ctx->SlotType[(i * 2) + 10] = (unsigned char)(1 & (dibit >> 1));
        ctx->SlotType[(i * 2) + 11] = (unsigned char)(1 & dibit);
    }

    if (!Golay_20_8_decode(ctx->SlotType)) {
        ctx->SlotTypeOk = 0;
        return 0;
    }

    ctx->SlotTypeOk = 1;
    ctx->state->color_code =
        (ctx->SlotType[0] << 3) + (ctx->SlotType[1] << 2) + (ctx->SlotType[2] << 1) + ctx->SlotType[3];
    ctx->state->color_code_ok = ctx->SlotTypeOk;
    ctx->burst =
        (uint8_t)((ctx->SlotType[4] << 3) + (ctx->SlotType[5] << 2) + (ctx->SlotType[6] << 1) + ctx->SlotType[7]);

    if (ctx->state->dmr_ms_mode == 0 && ctx->opts->dmr_mono == 0) {
        dmr_confidence_result confidence =
            dmr_confidence_note_data_burst(ctx->state, ctx->state->color_code, ctx->burst);
        if (confidence == DMR_CONFIDENCE_REJECT) {
            ctx->confidence_reject = 1;
        } else if (confidence != DMR_CONFIDENCE_LOCKED && ctx->burst != 9U) {
            ctx->confidence_pending = 1;
        }
    } else {
        ctx->state->dmr_color_code = ctx->state->color_code;
    }

    if (ctx->state->currentslot == 0) {
        ctx->state->dmrburstL = ctx->burst;
    }
    if (ctx->state->currentslot == 1) {
        ctx->state->dmrburstR = ctx->burst;
    }
    return 1;
}

static void
dmr_data_collect_second_half(dmr_data_sync_ctx* ctx) {
    int i;
    int dibit;
    for (i = 0; i < 49; i++) {
        dibit = dmr_data_read_live_dibit(ctx, i + 95, &ctx->rel98[i + 49]);
        ctx->info[(2 * i) + 98] = (uint8_t)(1 & (dibit >> 1));
        ctx->info[(2 * i) + 99] = (uint8_t)(1 & dibit);
    }
}

static void
dmr_data_dispatch_burst(dmr_data_sync_ctx* ctx) {
    if (ctx->confidence_reject != 0) {
        ctx->SlotTypeOk = 0;
        return;
    }
    if (ctx->confidence_pending != 0) {
        DSD_FPRINTF(stderr, "\n");
        return;
    }

    if (ctx->burst == 6 || ctx->burst == 7 || ctx->burst == 8 || ctx->burst == 10) {
        dmr_sm_emit_data_sync(ctx->opts, ctx->state, ctx->state->currentslot);
    }
    dmr_data_burst_handler(ctx->opts, ctx->state, ctx->info, ctx->burst, ctx->rel98);
    if (ctx->state->dmr_ms_mode == 0 && ctx->opts->dmr_mono == 0) {
        (void)dmr_cach(ctx->opts, ctx->state, ctx->cachdata);
    }
    DSD_FPRINTF(stderr, "\n");
}

static void
dmr_data_finalize(dsd_opts* opts, dsd_state* state, unsigned int SlotTypeOk, int cach_okay) {
    if (SlotTypeOk == 0 || cach_okay != 1) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "| CACH/Burst FEC ERR");
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_FPRINTF(stderr, "\n");
        dmr_reset_blocks(opts, state);
    }

    if (state->dmr_stereo == 0) {
        skipDibit(opts, state, 12 + 49 + 5);
    }

    if (opts->trunk_enable == 1 && opts->trunk_is_tuned == 1 && state->is_con_plus == 1) {
        int clear = 0;
        if (state->dmrburstL == 9 && state->dmrburstR == 9) {
            clear = 1;
        }
        if (clear == 1) {
            state->last_cc_sync_time = 0;
            state->last_cc_sync_time_m = 0.0;
            state->last_vc_sync_time = 0;
            state->last_vc_sync_time_m = 0.0;
        }
    }
}

void
dmr_data_sync(dsd_opts* opts, dsd_state* state) {
    dmr_data_sync_ctx ctx;
    dmr_data_sync_init_ctx(&ctx, opts, state);
    char debug_line[192];
    debug_line[0] = '\0';

    if (dmr_data_collect_cach(&ctx)) {
        dmr_data_collect_first_half(&ctx);
        dmr_data_collect_slot_type_prefix(&ctx);
        dmr_data_collect_sync(&ctx);
        if (dmr_data_collect_slot_type_suffix(&ctx)) {
            dmr_data_collect_second_half(&ctx);
            if (opts->dmr_debug_burst != 0) {
                (void)dmr_debug_format_burst(debug_line, sizeof(debug_line), state, state->currentslot, ctx.burst);
            }
            dmr_data_dispatch_burst(&ctx);
        }
    }

    dmr_data_finalize(opts, state, ctx.SlotTypeOk, ctx.cach_okay);
    if (debug_line[0] != '\0') {
        DSD_FPRINTF(stderr, "%s\n", debug_line);
    }
}
