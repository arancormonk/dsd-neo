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

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/colors.h>
#include <mbelib-neo/mbelib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_cach.h"
#include "dmr_confidence.h"
#include "dmr_mono_slot.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

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
    /* The carrier count (state->carrier_seq) the burst began on (issue #575). */
    uint32_t carrier_seq;
    /* The slot (DSD_DMR_MONO_RX_TS1/TS2) a checked TACT names for a standalone burst the single-slot output folds onto
       lane 0; DSD_DMR_MONO_RX_NONE otherwise (issue #637). */
    uint8_t folded_slot;
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
    ctx->carrier_seq = state->carrier_seq;
}

/* Whether the carrier boundary moved the carrier count since the burst began (issue #575), or an input interruption
   cut the burst (dsd_state_frame_read_lost()). */
static int
dmr_data_carrier_left(const dmr_data_sync_ctx* ctx) {
    return dsd_state_frame_read_lost(ctx->state, ctx->carrier_seq);
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
        /* The single-slot decoder's one lane keeps the burst's own slot for the receive policy (issue #637). */
        dmr_mono_slot_note(ctx->state, 1, tact_bits[1]);
        ctx->state->currentslot = 0;
    } else if (ctx->opts->dmr_mono == 1 && ctx->state->dmr_stereo == 0) {
        /* A standalone burst the single-slot output folds onto lane 0 (dmr_flco_ctx_init()) is judged by its own CACH
           slot for its dispatch: dmr_data_sync() set the voice lane's proof aside and restores it after. */
        ctx->folded_slot = (uint8_t)(DSD_DMR_MONO_RX_TS1 + (tact_bits[1] & 1U));
        ctx->state->dmr_mono_rx_slot = ctx->folded_slot;
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
    /* An input interruption cut the slot type (a Signal search left the burst): its live half is erasures the reader
       stood in. Or the carrier boundary moved while it was read (a replay read adopting a recorded retune), so it
       straddles two carriers. Either way nothing of it decodes, neither the colour code (the received identity) nor
       the confidence and search evidence it would feed. dmr_data_sync() reads on and drops the burst at its carrier
       check, which this fails too; a cut burst is no FEC error, so the slot type does not report one. */
    if (dmr_data_carrier_left(ctx)) {
        ctx->SlotTypeOk = 1;
        return 1;
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

    if (ctx->state->dmr_ms_mode == 0) {
        dmr_confidence_result confidence = dmr_confidence_note_data_burst(ctx->state, ctx->state->color_code);
        if (confidence == DMR_CONFIDENCE_REJECT) {
            ctx->confidence_reject = 1;
        } else if (confidence != DMR_CONFIDENCE_LOCKED && ctx->burst != 9U) {
            ctx->confidence_pending = 1;
        }
    } else {
        /* MS mode has no confidence lock: the slot type's checked colour code is the carrier's. */
        ctx->state->dmr_color_code = ctx->state->color_code;
        dsd_rxid_note_access(ctx->state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE,
                             (uint16_t)ctx->state->color_code);
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
    if (ctx->state->dmr_ms_mode == 0) {
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

/* The converse of dmr_bs_polarity_is_inverted() (issue #729): a BS voice sync received at the other polarity reads as a
   data sync, so a voice call at the wrong polarity arrives as a data burst on every slot's burst A whose slot type never
   decodes. Its first AMBE frame proves it: at the other polarity it passes the AMBE FEC with at most one correction,
   which noise or a data burst does about once in ten thousand. Two such bursts with no data burst decoding between
   them take the other polarity for the session, as the menu's toggle and -xr do; the hunt then finds the call. */
static int
dmr_data_burst_is_voice_inverted(const dmr_data_sync_ctx* ctx) {
    if (ctx->state->dmr_ms_mode != 0 || strcmp(ctx->sync, DMR_BS_DATA_SYNC) != 0) {
        return 0;
    }
    char frame[4][24];
    DSD_MEMSET(frame, 0, sizeof(frame));
    for (int i = 0; i < 36; i++) {
        const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[i];
        const int dibit = ctx->state->dmr_stereo_payload[12 + i] ^ 2;
        frame[map->high_row][map->high_col] = (char)(1 & (dibit >> 1));
        frame[map->low_row][map->low_col] = (char)(1 & dibit);
    }
    char ambe_d[49];
    mbe_process_result result;
    DSD_MEMSET(&result, 0, sizeof(result));
    if (mbe_decodeAmbe3600x2450Frame((const char (*)[24])frame, ambe_d, &result) < 0) {
        return 0;
    }
    return result.total_errors <= 1;
}

static void
dmr_data_note_polarity_evidence(dsd_opts* opts, dsd_state* state, const dmr_data_sync_ctx* ctx) {
    if (ctx->SlotTypeOk != 0 && ctx->cach_okay == 1) {
        state->dmr_inverted_voice_evidence = 0;
        return;
    }
    if (ctx->cach_okay != 1 || !dmr_data_burst_is_voice_inverted(ctx)) {
        return;
    }
    if (++state->dmr_inverted_voice_evidence < 2U) {
        return;
    }
    state->dmr_inverted_voice_evidence = 0;
    opts->inverted_dmr = opts->inverted_dmr ? 0 : 1;
    DSD_FPRINTF(stderr, "%s DMR: data bursts that are voice at the other polarity; decoding %s polarity%s\n", KYEL,
                opts->inverted_dmr ? "inverted" : "normal", KNRM);
}

/* Whether the burst was a voice LC header, folded onto lane 0, that the lane's call took (issue #637): the call is active
   on the header's slot and this dispatch observed it (a new epoch, or the epoch @p before held moved on). That call is
   the one the lane carries next, so the proof the previous call left must not end it. A header the LC check refused
   observed nothing, and a PI header does not open the call. */
static int
dmr_data_header_opened_lane_call(const dsd_state* state, const dmr_data_sync_ctx* ctx,
                                 const dsd_call_snapshot* before) {
    if (ctx->burst != 1U || ctx->folded_slot == DSD_DMR_MONO_RX_NONE) {
        return 0;
    }
    dsd_call_snapshot after;
    if (dsd_call_state_get(state, 0U, &after) <= 0 || after.phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    const uint8_t slot = (uint8_t)(ctx->folded_slot - DSD_DMR_MONO_RX_TS1 + 1U);
    return after.receive_identity.slot == slot && (after.epoch != before->epoch || after.revision != before->revision);
}

/* After a standalone burst (issue #637): what one the receive policy rejects or is still checking left in its lane's
   position cache (an LRRP fix, a data call line) goes before the voice lane's proof @p mono_slot / @p mono_candidate
   comes back, since the terminal judges that lane by the proof, not by the slot the burst was heard on. The proof comes
   back only onto the carrier it was proven on (a boundary during the burst, a replay retune, forgot it for good), and a
   voice LC header the lane's call @p lane_call took applies the mono-slot rule on top of it. */
static void
dmr_data_restore_voice_lane(const dsd_opts* opts, dsd_state* state, const dmr_data_sync_ctx* ctx, uint8_t mono_slot,
                            uint8_t mono_candidate, const dsd_call_snapshot* lane_call) {
    if (state->carrier_seq != ctx->carrier_seq) {
        return;
    }
    if (ctx->cach_okay == 1 && !dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_DATA_POS, state->currentslot & 1)) {
        state->dmr_lrrp_gps[state->currentslot & 1][0] = '\0';
    }
    state->dmr_mono_rx_slot = mono_slot;
    state->dmr_mono_rx_candidate = mono_candidate;
    if (dmr_data_header_opened_lane_call(state, ctx, lane_call)) {
        dmr_mono_slot_note(state, 1, (unsigned int)(ctx->folded_slot - DSD_DMR_MONO_RX_TS1));
    }
}

void
dmr_data_sync(dsd_opts* opts, dsd_state* state) {
    dmr_data_sync_ctx ctx;
    dmr_data_sync_init_ctx(&ctx, opts, state);
    char debug_line[192];
    debug_line[0] = '\0';
    /* The receive slot (issue #637). A standalone data burst is decoded as a two-slot burst, by its CACH's own slot, so
       it is judged by that slot and not by the one the single-slot decoder proved for its voice lane, which is kept for
       the voice that follows. A data burst the single-slot decoder reads (MS mode) takes its slot as its voice does: a
       direct-mode sync word names it, a repeater's CACH proves it, and otherwise it stays Checking. */
    const int standalone = state->dmr_ms_mode != 1;
    const uint8_t mono_slot = state->dmr_mono_rx_slot;
    const uint8_t mono_candidate = state->dmr_mono_rx_candidate;
    /* The lane's call before the burst, for a header the single-slot output folds onto it (issue #637). */
    dsd_call_snapshot lane_call;
    DSD_MEMSET(&lane_call, 0, sizeof(lane_call));
    if (standalone && opts->dmr_mono == 1 && state->dmr_stereo == 0) {
        (void)dsd_call_state_get(state, 0U, &lane_call);
    }
    if (standalone) {
        dmr_mono_slot_forget(state);
    } else {
        dmr_mono_slot_begin(state);
    }

    if (dmr_data_collect_cach(&ctx)) {
        dmr_data_collect_first_half(&ctx);
        dmr_data_collect_slot_type_prefix(&ctx);
        dmr_data_collect_sync(&ctx);
        if (dmr_data_collect_slot_type_suffix(&ctx)) {
            dmr_data_collect_second_half(&ctx);
            if (dmr_data_carrier_left(&ctx)) {
                /* The carrier boundary moved while the burst's live half was read (issue #575): the CACH read before
                   it may complete a short LC of the carrier left, which would decode after the boundary ended that
                   carrier's calls. Or an input interruption cut the live half, which is erasures the reader stood in.
                   Nothing in the burst dispatches, and what earlier bursts gathered goes, as on a sync loss; neither
                   is a burst FEC error. */
                dmr_reset_blocks(opts, state);
            } else {
                if (opts->dmr_debug_burst != 0) {
                    (void)dmr_debug_format_burst(debug_line, sizeof(debug_line), state, state->currentslot, ctx.burst);
                }
                dmr_data_dispatch_burst(&ctx);
            }
        }
    }

    dmr_data_finalize(opts, state, ctx.SlotTypeOk, ctx.cach_okay);
    dmr_data_note_polarity_evidence(opts, state, &ctx);
    if (standalone) {
        dmr_data_restore_voice_lane(opts, state, &ctx, mono_slot, mono_candidate, &lane_call);
    }
    if (debug_line[0] != '\0') {
        DSD_FPRINTF(stderr, "%s\n", debug_line);
    }
}
