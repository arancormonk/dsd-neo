// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * dmr_bs.c
 * DMR BS Voice Handling and Data Gathering Routines - "DMR STEREO"
 *
 * LWVMOBILE
 * 2022-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dmr_cach.h"
#include "dmr_confidence.h"
#include "dmr_mono_slot.h"
#include "dmr_vcrypt.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

typedef enum {
    DMR_BS_ACTION_CONTINUE = 0,
    DMR_BS_ACTION_SKIP,
    DMR_BS_ACTION_END,
} dmr_bs_action;

typedef struct {
    char ambe_fr[4][24];
    char ambe_fr2[4][24];
    char ambe_fr3[4][24];
    char redundancyA[36];
    char redundancyB[36];
    uint8_t m1[4][24];
    uint8_t m2[4][24];
    uint8_t m3[4][24];
    char sync[25];
    uint8_t syncdata[48];
    uint8_t emb_pdu[16];
    uint8_t dummy_bits[196];
    uint8_t cachdata[25];
    uint8_t tact_bits[7];
    uint8_t tact_okay;
    uint8_t emb_ok;
    uint8_t internalslot;
    uint8_t vc1;
    uint8_t vc2;
    uint8_t cc;
    uint8_t power;
    short skipcount;
    /* The slot of the previous burst the loop read, -1 before the first (whose TACT starts the count): bursts
       alternate slots, so this decides the next one's, not its TACT (issue #729). */
    int8_t prev_slot;
    uint8_t tact_disagree;
    uint8_t tact_agree;
    char timestr[9];
} dmr_bs_ctx;

typedef struct {
    char ambe_fr[4][24];
    char ambe_fr2[4][24];
    char ambe_fr3[4][24];
    uint8_t m1[4][24];
    uint8_t m2[4][24];
    uint8_t m3[4][24];
    uint8_t cachdata[25];
    uint8_t tact_bits[7];
    char sync[25];
    uint8_t tact_okay;
    uint8_t sync_okay;
    uint8_t internalslot;
    char timestr[9];
} dmr_bs_bootstrap_ctx;

/*
 * Mark dmrBS helper roots used by the public decoder entrypoint. CodeQL's
 * manual C/C++ database can miss this local call chain and report all children.
 */
static void DSD_ATTR_USED
init_dmr_bs_ctx(const dsd_state* state, dmr_bs_ctx* ctx, int bootstrap_slot_known) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->cc = 25;
    ctx->power = 9;
    ctx->vc1 = 7;
    ctx->vc2 = 7;
    ctx->prev_slot = -1;
    /* The bootstrap just read the slot's burst A, so the loop's first burst is the other slot's. A bootstrap that could
       not read the burst's CACH back names no slot: the loop's first burst is the slot its own TACT names. */
    if (!bootstrap_slot_known) {
        return;
    }
    if (state->currentslot == 0) {
        ctx->vc1 = 2;
        ctx->prev_slot = 0;
    } else if (state->currentslot == 1) {
        ctx->vc2 = 2;
        ctx->prev_slot = 1;
    }
}

static void
reset_dmr_bs_loop_buffers(dmr_bs_ctx* ctx) {
    DSD_MEMSET(ctx->ambe_fr, 0, sizeof(ctx->ambe_fr));
    DSD_MEMSET(ctx->ambe_fr2, 0, sizeof(ctx->ambe_fr2));
    DSD_MEMSET(ctx->ambe_fr3, 0, sizeof(ctx->ambe_fr3));
    DSD_MEMSET(ctx->emb_pdu, 0, sizeof(ctx->emb_pdu));
    DSD_MEMSET(ctx->syncdata, 0, sizeof(ctx->syncdata));
}

static int
read_dmr_bs_stream_dibit(dsd_opts* opts, dsd_state* state, int payload_index) {
    dsd_dibit_soft_t soft;
    int dibit = getDibitSoft(opts, state, &soft);
    state->dmr_stereo_payload[payload_index] = dibit;
    state->dmr_stereo_reliab[payload_index] = soft.reliability;
    return dibit;
}

static void
read_dmr_bs_ambe_segment_stream(dsd_opts* opts, dsd_state* state, char frame[4][24], int payload_offset,
                                int dibit_count, int interleave_offset, char* redundancy_out) {
    for (int i = 0; i < dibit_count; i++) {
        const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[interleave_offset + i];
        int dibit = read_dmr_bs_stream_dibit(opts, state, payload_offset + i);
        if (redundancy_out != NULL) {
            redundancy_out[i] = (char)dibit;
        }
        frame[map->high_row][map->high_col] = (1 & (dibit >> 1));
        frame[map->low_row][map->low_col] = (1 & dibit);
    }
}

static void
unpack_dmr_bs_ambe_segment_from_payload(const dsd_state* state, char frame[4][24], int payload_offset, int dibit_count,
                                        int interleave_offset) {
    for (int i = 0; i < dibit_count; i++) {
        const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[interleave_offset + i];
        int dibit = state->dmr_stereo_payload[payload_offset + i];
        frame[map->high_row][map->high_col] = (1 & (dibit >> 1));
        frame[map->low_row][map->low_col] = (1 & dibit);
    }
}

static void
read_dmr_bs_sync_segment(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    for (int i = 0; i < 24; i++) {
        int dibit = read_dmr_bs_stream_dibit(opts, state, i + 66);
        ctx->sync[i] = (dibit | 1) + 48;
        ctx->syncdata[((size_t)2 * i)] = (1 & (dibit >> 1));
        ctx->syncdata[((size_t)2 * i) + 1] = (1 & dibit);

        if (ctx->internalslot == 0 && ctx->vc1 > 1 && ctx->vc1 < 7) {
            state->dmr_embedded_signalling[ctx->internalslot][ctx->vc1 - 1][((size_t)i * 2)] = (1 & (dibit >> 1));
            state->dmr_embedded_signalling[ctx->internalslot][ctx->vc1 - 1][((size_t)i * 2) + 1] = (1 & dibit);
        }

        if (ctx->internalslot == 1 && ctx->vc2 > 1 && ctx->vc2 < 7) {
            state->dmr_embedded_signalling[ctx->internalslot][ctx->vc2 - 1][((size_t)i * 2)] = (1 & (dibit >> 1));
            state->dmr_embedded_signalling[ctx->internalslot][ctx->vc2 - 1][((size_t)i * 2) + 1] = (1 & dibit);
        }
    }
    ctx->sync[24] = 0;
}

/* A BS voice sync is the symbol-wise complement of a BS data sync, so the hunt cannot tell a voice burst from a data
   burst received at the other polarity: an inverted control channel or idle slot reads as voice sync on every burst.
   A voice superframe carries its sync on burst A alone, and an EMB on B-F, so voice sync on four adjacent bursts in a
   row (two of each slot), with no slot's EMB ever having opened its voice gate, is never voice. The polarity is the
   other one: the decoder takes it for the session (the -xr setting, which the menu toggles too) and ends the loop, and
   the hunt finds the bursts again as what they are. Left alone, the wrong polarity decoded data as voice whenever noise
   made an EMB look valid, and with a steady symbol grid it never left the loop at all (issue #729). */
#define DMR_BS_BURST_SYMBOLS      144U
#define DMR_BS_POLARITY_RUN       4U
/* How far from one burst after the last sync field the next may end and still be its neighbour: the grid's slips move
   a sync field by a few symbols, a lost burst by a whole one. */
#define DMR_BS_ADJACENT_TOLERANCE 12U

/* Counts a voice sync field into the run, which holds only syncs read on adjacent bursts. The run is the state's, not
   the loop's: identical idle bursts end the loop after two (the redundant-carrier check), and the hunt re-enters it on
   the next voice sync, so the run is counted across those entries -- the bootstrap's burst too. But a burst the
   decoder never saw breaks it: on real voice, lost bursts B leave burst A syncs that would otherwise read as a run. */
static uint8_t
dmr_bs_note_voice_sync_run(dsd_state* state, int voice_sync) {
    /* A slot whose gate opened carried EMBs that decoded: the polarity is proved, whatever the syncs look like. */
    if (!voice_sync || dmr_confidence_voice_slot_open(state, 0U) || dmr_confidence_voice_slot_open(state, 1U)) {
        state->dmr_voice_sync_run = 0;
        return 0;
    }
    const uint64_t since = state->symbol_clock - state->dmr_voice_sync_clock;
    const int adjacent = state->dmr_voice_sync_run > 0U && state->dmr_voice_sync_epoch == state->symbol_clock_epoch
                         && since >= (uint64_t)(DMR_BS_BURST_SYMBOLS - DMR_BS_ADJACENT_TOLERANCE)
                         && since <= (uint64_t)(DMR_BS_BURST_SYMBOLS + DMR_BS_ADJACENT_TOLERANCE);
    if (!adjacent) {
        state->dmr_voice_sync_run = 1;
    } else if (state->dmr_voice_sync_run < 255U) {
        state->dmr_voice_sync_run++;
    }
    state->dmr_voice_sync_clock = state->symbol_clock;
    state->dmr_voice_sync_epoch = state->symbol_clock_epoch;
    return state->dmr_voice_sync_run;
}

static int
dmr_bs_polarity_is_inverted(dsd_opts* opts, dsd_state* state, const dmr_bs_ctx* ctx) {
    if (dmr_bs_note_voice_sync_run(state, strcmp(ctx->sync, DMR_BS_VOICE_SYNC) == 0) < DMR_BS_POLARITY_RUN) {
        return 0;
    }
    opts->inverted_dmr = opts->inverted_dmr ? 0 : 1;
    state->dmr_voice_sync_run = 0;
    DSD_FPRINTF(stderr, "%s %s DMR: voice sync on every burst is data at the other polarity; decoding %s polarity%s\n",
                ctx->timestr, KYEL, opts->inverted_dmr ? "inverted" : "normal", KNRM);
    return 1;
}

/* The loop reads burst after burst without returning to the frame-sync hunt, the only place the symbol grid used to
   move, so a call long enough for the transmitter's symbol clock to drift against the sample clock walked the grid off
   the symbols until a sync was lost (issue #729). A sync word read whole is a known pattern to place the grid on; the
   EMB of bursts B-F is not, until its FEC passes, and one sync per slot superframe plus every data burst is plenty. */
static void
note_dmr_bs_sync_anchor(const dsd_state* state, const dmr_bs_ctx* ctx) {
    if (strcmp(ctx->sync, DMR_BS_VOICE_SYNC) == 0 || strcmp(ctx->sync, DMR_BS_DATA_SYNC) == 0) {
        dsd_symbol_note_sync_anchor(state, ctx->sync, 24);
    }
}

static void
extract_dmr_bs_sync_from_payload(const dsd_state* state, int payload_offset, char sync[25]) {
    for (int i = 0; i < 24; i++) {
        int dibit = state->dmr_stereo_payload[payload_offset + i];
        sync[i] = (dibit | 1) + 48;
    }
    sync[24] = 0;
}

/* A base station's bursts alternate slots, and the loop reads every one of them in turn. The TACT's slot bit is only
   Hamming(7,4)-protected, a perfect code that decodes two bit errors into a wrong codeword without a word, which would
   hand the burst to the other slot and leave this slot's VC count a burst behind -- its VC6 then never runs, nor the MI
   step and late entry with it. Alternation decides, from the burst the bootstrap read. Until two TACTs agree with it,
   two in a row against it say the start was wrong (the bootstrap's own TACT miscorrected) and move it, so a wrong
   start costs one burst; once confirmed, TACTs no longer move it, and miscorrections cost nothing. */
static uint8_t
dmr_bs_next_slot(dmr_bs_ctx* ctx) {
    const uint8_t tact_slot = ctx->tact_bits[1] != 0U ? 1U : 0U;
    if (ctx->prev_slot < 0) {
        return tact_slot;
    }
    const uint8_t expected = (uint8_t)(ctx->prev_slot ^ 1);
    if (tact_slot == expected) {
        ctx->tact_disagree = 0;
        if (ctx->tact_agree < 2U) {
            ctx->tact_agree++;
        }
        return expected;
    }
    if (ctx->tact_agree >= 2U || ++ctx->tact_disagree < 2U) {
        return expected;
    }
    ctx->tact_disagree = 0;
    return tact_slot;
}

static int
collect_dmr_bs_cach_and_tact(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    for (int i = 0; i < 12; i++) {
        int dibit = read_dmr_bs_stream_dibit(opts, state, i);
        ctx->cachdata[dmr_cach_interleave[((size_t)i * 2)]] = (1 & (dibit >> 1));
        ctx->cachdata[dmr_cach_interleave[((size_t)i * 2) + 1]] = (1 & dibit);
    }

    for (int i = 0; i < 7; i++) {
        ctx->tact_bits[i] = ctx->cachdata[i];
    }

    ctx->tact_okay = 0;
    if (Hamming_7_4_decode(ctx->tact_bits)) {
        ctx->tact_okay = 1;
    }
    if (ctx->tact_okay != 1) {
        return 0;
    }

    ctx->internalslot = dmr_bs_next_slot(ctx);
    ctx->prev_slot = (int8_t)ctx->internalslot;
    state->currentslot = ctx->internalslot;
    return 1;
}

static void
build_dmr_bs_emb_pdu(dmr_bs_ctx* ctx) {
    for (int i = 0; i < 8; i++) {
        ctx->emb_pdu[i] = ctx->syncdata[i];
        ctx->emb_pdu[i + 8] = ctx->syncdata[i + 40];
    }
}

static int
is_dmr_bs_redundant_carrier(const dmr_bs_ctx* ctx) {
    return ctx->redundancyA[16] == ctx->redundancyB[16] && ctx->redundancyA[27] == ctx->redundancyB[27]
           && ctx->redundancyA[1] == ctx->redundancyB[1] && ctx->redundancyA[32] == ctx->redundancyB[32]
           && ctx->redundancyA[3] == ctx->redundancyB[3] && ctx->redundancyA[33] == ctx->redundancyB[33]
           && ctx->redundancyA[13] == ctx->redundancyB[13] && ctx->redundancyA[7] == ctx->redundancyB[7];
}

static void
note_dmr_bs_voice_sync(dsd_state* state, dmr_bs_ctx* ctx) {
    if (strcmp(ctx->sync, DMR_BS_VOICE_SYNC) != 0) {
        return;
    }

    if (ctx->internalslot == 0) {
        ctx->vc1 = 1;
        state->dmr_emb_err[0] = 0;
    }
    if (ctx->internalslot == 1) {
        ctx->vc2 = 1;
        state->dmr_emb_err[1] = 0;
    }
    dmr_confidence_note_voice_sync(state, ctx->internalslot);
}

static void
print_dmr_sync_polarity(const dsd_opts* opts) {
    if (opts->inverted_dmr == 0) {
        DSD_FPRINTF(stderr, "Sync: +DMR  ");
    } else {
        DSD_FPRINTF(stderr, "Sync: -DMR  ");
    }
}

static uint8_t
current_dmr_bs_vc(const dmr_bs_ctx* ctx) {
    if (ctx->internalslot == 0) {
        return ctx->vc1;
    }
    if (ctx->internalslot == 1) {
        return ctx->vc2;
    }
    return 0;
}

static int
is_dmr_bs_slot_vc6(const dmr_bs_ctx* ctx) {
    return current_dmr_bs_vc(ctx) == 6;
}

static void
increment_dmr_bs_slot_vc(dmr_bs_ctx* ctx) {
    if (ctx->internalslot == 0) {
        ctx->vc1++;
    }
    if (ctx->internalslot == 1) {
        ctx->vc2++;
    }
}

static dmr_bs_action
handle_dmr_bs_data_sync(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    if (strcmp(ctx->sync, DMR_BS_DATA_SYNC) != 0) {
        return DMR_BS_ACTION_CONTINUE;
    }

    DSD_FPRINTF(stderr, "%s ", ctx->timestr);
    print_dmr_sync_polarity(opts);

    if (ctx->internalslot == 0) {
        ctx->vc1 = 7;
        dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT1, DSD_SYNC_DMR_BS_DATA_POS);
    }
    if (ctx->internalslot == 1) {
        ctx->vc2 = 7;
        dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT2, DSD_SYNC_DMR_BS_DATA_POS);
    }

    dmr_data_sync(opts, state);
    ctx->skipcount++;
    return DMR_BS_ACTION_SKIP;
}

static dmr_bs_action
handle_dmr_bs_frame_sync_miss(const dmr_bs_ctx* ctx, uint8_t* vc1, uint8_t* vc2) {
    if (strcmp(ctx->sync, DMR_BS_DATA_SYNC) == 0) {
        return DMR_BS_ACTION_CONTINUE;
    }

    if (ctx->internalslot == 0 && *vc1 > 6) {
        char light[18];
        DSD_FPRINTF(stderr, "%s ", ctx->timestr);
        DSD_SNPRINTF(light, 18, "%s", " [SLOT1]  slot2  ");
        DSD_FPRINTF(stderr, "Sync:  DMR %s", light);
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, "| Frame Sync Err: %d", *vc1);
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_FPRINTF(stderr, "\n");
        (*vc1)++;
        return (*vc1 > 13) ? DMR_BS_ACTION_END : DMR_BS_ACTION_SKIP;
    }

    if (ctx->internalslot == 1 && *vc2 > 6) {
        char light[18];
        DSD_FPRINTF(stderr, "%s ", ctx->timestr);
        DSD_SNPRINTF(light, 18, "%s", "  slot1  [SLOT2] ");
        DSD_FPRINTF(stderr, "Sync:  DMR %s", light);
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, "| Frame Sync Err: %d", *vc2);
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_FPRINTF(stderr, "\n");
        (*vc2)++;
        return (*vc2 > 13) ? DMR_BS_ACTION_END : DMR_BS_ACTION_SKIP;
    }

    return DMR_BS_ACTION_CONTINUE;
}

/* Counts an EMB that did not decode, or named another colour code, on @p slot; returns the misses in a row. */
static uint8_t
note_dmr_bs_emb_miss(dsd_state* state, uint8_t slot) {
    uint8_t* miss = &state->dmr_emb_err[slot];
    if (*miss < 0xFF) {
        (*miss)++;
    }
    return *miss;
}

/* A burst on a slot whose voice gate is still closed: not decoded, and the loop ends without a usable EMB or sync. */
static dmr_bs_action
skip_dmr_bs_closed_gate_burst(dsd_state* state, dmr_bs_ctx* ctx) {
    if (ctx->emb_ok != 1 && strcmp(ctx->sync, DMR_BS_VOICE_SYNC) != 0) {
        dmr_confidence_reset_slot(state, ctx->internalslot);
        return DMR_BS_ACTION_END;
    }
    increment_dmr_bs_slot_vc(ctx);
    ctx->tact_okay = 0;
    ctx->emb_ok = 0;
    if (ctx->vc1 > 14 || ctx->vc2 > 14) {
        return DMR_BS_ACTION_END;
    }
    return DMR_BS_ACTION_SKIP;
}

static dmr_bs_action
check_dmr_bs_emb_and_confidence(dsd_state* state, dmr_bs_ctx* ctx) {
    ctx->emb_ok = QR_16_7_6_decode(ctx->emb_pdu) ? 1 : 0;

    if (ctx->emb_ok == 1) {
        ctx->cc = (uint8_t)((ctx->emb_pdu[0] << 3) + (ctx->emb_pdu[1] << 2) + (ctx->emb_pdu[2] << 1) + ctx->emb_pdu[3]);
        ctx->power = ctx->emb_pdu[4];
        state->color_code = ctx->cc;
    } else if (strcmp(ctx->sync, DMR_BS_VOICE_SYNC) != 0) {
        if (note_dmr_bs_emb_miss(state, ctx->internalslot) >= 2) {
            return DMR_BS_ACTION_END;
        }
    } else {
        state->dmr_emb_err[ctx->internalslot] = 0;
    }

    int voice_gate_open = dmr_confidence_voice_slot_open(state, ctx->internalslot);
    if (ctx->emb_ok == 1) {
        dmr_confidence_result confidence = dmr_confidence_note_voice_burst(state, ctx->internalslot, ctx->cc);
        if (confidence == DMR_CONFIDENCE_REJECT) {
            ctx->emb_ok = 0;
            /* On a slot whose gate is open, one EMB naming another colour code is far likelier a QR(16,7,6)
               miscorrection of a noisy burst than another system: it counts as the EMB miss it is, and only a second
               in a row ends the loop, as two undecodable EMBs do. Ending on the first dropped the rest of the
               superframe, and the gate's re-qualification the next one's first bursts (issue #729). */
            if (note_dmr_bs_emb_miss(state, ctx->internalslot) >= 2 || !voice_gate_open) {
                dmr_confidence_reset_slot(state, ctx->internalslot);
                return DMR_BS_ACTION_END;
            }
        } else {
            /* Only an EMB the colour-code check accepted ends a miss streak. */
            state->dmr_emb_err[ctx->internalslot] = 0;
        }
        voice_gate_open = dmr_confidence_voice_slot_open(state, ctx->internalslot);
    }

    return voice_gate_open == 0 ? skip_dmr_bs_closed_gate_burst(state, ctx) : DMR_BS_ACTION_CONTINUE;
}

static uint8_t
prepare_dmr_bs_voice_slot(dsd_opts* opts, dsd_state* state, const dmr_bs_ctx* ctx, char light[18], char polarity[3]) {
    uint8_t vc = 0;

    if (ctx->internalslot == 0) {
        state->dmrburstL = 16;
        vc = ctx->vc1;
        DSD_SNPRINTF(light, 18, "%s", " [SLOT1]  slot2  ");
        dmr_sm_emit_voice_sync(opts, state, 0);
    } else {
        state->dmrburstR = 16;
        vc = ctx->vc2;
        DSD_SNPRINTF(light, 18, "%s", "  slot1  [SLOT2] ");
        dmr_sm_emit_voice_sync(opts, state, 1);
    }

    if (opts->inverted_dmr == 0) {
        DSD_SNPRINTF(polarity, 3, "%s", "+");
    } else {
        DSD_SNPRINTF(polarity, 3, "%s", "-");
    }

    return vc;
}

static void
handle_dmr_bs_slot_vc6_pre_link(dsd_opts* opts, dsd_state* state, const dmr_bs_ctx* ctx) {
    if (ctx->internalslot == 0 && ctx->vc1 == 6) {
        if (state->payload_algid == 0x02) {
            dmr_vcrypt_hytera_vc6_step(state, 0U);
        }
        DSD_FPRINTF(stderr, "\n");
        dmr_data_burst_handler(opts, state, (uint8_t*)ctx->dummy_bits, 0xEB, NULL);
    }

    if (ctx->internalslot == 1 && ctx->vc2 == 6) {
        if (state->payload_algidR == 0x02) {
            dmr_vcrypt_hytera_vc6_step(state, 1U);
        }
        DSD_FPRINTF(stderr, "\n");
        dmr_data_burst_handler(opts, state, (uint8_t*)ctx->dummy_bits, 0xEB, NULL);
    }
}

static void
copy_dmr_bs_voice_frames(uint8_t m1[4][24], uint8_t m2[4][24], uint8_t m3[4][24], char ambe_fr[4][24],
                         char ambe_fr2[4][24], char ambe_fr3[4][24]) {
    DSD_MEMCPY(m1, ambe_fr, sizeof(uint8_t[4][24]));
    DSD_MEMCPY(m2, ambe_fr2, sizeof(uint8_t[4][24]));
    DSD_MEMCPY(m3, ambe_fr3, sizeof(uint8_t[4][24]));
}

static void
apply_dmr_bs_voice_keystream(dsd_state* state, char ambe_fr[4][24], char ambe_fr2[4][24], char ambe_fr3[4][24]) {
    if (state->tyt_bp == 1) {
        tyt16_ambe2_codeword_keystream(state, ambe_fr, 0);
        tyt16_ambe2_codeword_keystream(state, ambe_fr2, 1);
        tyt16_ambe2_codeword_keystream(state, ambe_fr3, 0);
    }

    if (state->csi_ee == 1) {
        csi72_ambe2_codeword_keystream(state, ambe_fr);
        csi72_ambe2_codeword_keystream(state, ambe_fr2);
        csi72_ambe2_codeword_keystream(state, ambe_fr3);
    }
}

static void
store_dmr_bs_processed_audio(dsd_state* state, uint8_t internalslot, uint8_t frame_index) {
    if (internalslot == 0) {
        DSD_MEMCPY(state->s_l4[frame_index], state->s_l, sizeof(state->s_l));
        DSD_MEMCPY(state->s_l4u[frame_index], state->s_lu, sizeof(state->s_lu));
    } else {
        DSD_MEMCPY(state->s_r4[frame_index], state->s_r, sizeof(state->s_r));
        DSD_MEMCPY(state->s_r4u[frame_index], state->s_ru, sizeof(state->s_ru));
    }
}

static void
process_dmr_bs_voice_frame(dsd_opts* opts, dsd_state* state, uint8_t internalslot, char frame[4][24],
                           uint8_t frame_index) {
    processMbeFrame(opts, state, DSD_VOICE_PROTOCOL_DMR, NULL, frame, NULL);
    store_dmr_bs_processed_audio(state, internalslot, frame_index);
}

/* Each frame decodes at its position in the superframe (burst @p vc, 1..6), which is where every DMR voice cipher's
   keystream for it lies, whichever bursts before it the gate skipped or the loop lost (issue #729). */
static void
process_dmr_bs_three_voice_frames(dsd_opts* opts, dsd_state* state, uint8_t internalslot, uint8_t vc,
                                  char ambe_fr[4][24], char ambe_fr2[4][24], char ambe_fr3[4][24]) {
    dmr_vcrypt_set_frame(state, internalslot, vc, 0);
    process_dmr_bs_voice_frame(opts, state, internalslot, ambe_fr, 0);
    dmr_vcrypt_set_frame(state, internalslot, vc, 1);
    process_dmr_bs_voice_frame(opts, state, internalslot, ambe_fr2, 1);
    dmr_vcrypt_set_frame(state, internalslot, vc, 2);
    process_dmr_bs_voice_frame(opts, state, internalslot, ambe_fr3, 2);
    dmr_vcrypt_clear_frame(state, internalslot);
}

static void
write_dmr_bs_dsp_output(const dsd_opts* opts, const dsd_state* state, uint8_t internalslot) {
    FILE* pFile = dsd_fopen_private(opts->dsp_out_file, "a");
    if (pFile == NULL) {
        return;
    }

    DSD_FPRINTF(pFile, "\n%d 98 ", internalslot + 1);
    for (int i = 0; i < 6; i++) {
        int cach_byte =
            (state->dmr_stereo_payload[((size_t)i * 2)] << 2) | state->dmr_stereo_payload[((size_t)i * 2) + 1];
        DSD_FPRINTF(pFile, "%X", cach_byte);
    }

    DSD_FPRINTF(pFile, "\n%d 10 ", internalslot + 1);
    for (int i = 6; i < 72; i++) {
        int dsp_byte =
            (state->dmr_stereo_payload[((size_t)i * 2)] << 2) | state->dmr_stereo_payload[((size_t)i * 2) + 1];
        DSD_FPRINTF(pFile, "%X", dsp_byte);
    }

    fclose(pFile);
}

static void
reset_dmr_bs_slot_keystream_counters(dsd_state* state, uint8_t internalslot) {
    state->static_ks_counter[internalslot] = 0;
    state->vertex_ks_counter[internalslot] = 0;
    state->vertex_ks_active_idx[internalslot] = -1;
    state->vertex_ks_warned[internalslot] = 0;
}

static void
run_dmr_bs_slot_vc6_sbrc(dsd_opts* opts, dsd_state* state, const dmr_bs_ctx* ctx) {
    if (is_dmr_bs_slot_vc6(ctx)) {
        dmr_sbrc(opts, state, ctx->power);
    }
}

static void
run_dmr_bs_slot_vc6_post_voice(dsd_opts* opts, dsd_state* state, const dmr_bs_ctx* ctx) {
    if (is_dmr_bs_slot_vc6(ctx)) {
        dmr_alg_refresh(opts, state);
        reset_dmr_bs_slot_keystream_counters(state, ctx->internalslot);
    }
}

/* A followed voice channel's activity: the carrier is alive (the control channel's stamp), and the slot's voice holds
   the channel only while the receive policy admits it (issue #637), so a carrier only rejected traffic holds is let go
   at its hangtime even when a CSBK path followed it without the state machine. */
static void
update_dmr_bs_sync_times_if_tuned(const dsd_opts* opts, dsd_state* state, int slot) {
    if (opts->trunk_is_tuned == 1) {
        time_t now = dsd_decode_time();
        double nowm = dsd_decode_now_mono_s();
        if (dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, slot)) {
            state->last_vc_sync_time = now;
            state->last_vc_sync_time_m = nowm;
        }
        state->last_cc_sync_time = now;
        state->last_cc_sync_time_m = nowm;
    }
}

// dmr_late_entry_mi_fragment() skips a slot whose call is Hytera Enhanced (issue #657).
static void
collect_dmr_bs_late_entry(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    dmr_late_entry_mi_fragment(opts, state, current_dmr_bs_vc(ctx) % 7, ctx->m1, ctx->m2, ctx->m3);
}

static dmr_bs_action
process_dmr_bs_voice_burst(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    dmr_bs_action action = check_dmr_bs_emb_and_confidence(state, ctx);
    if (action != DMR_BS_ACTION_CONTINUE) {
        return action;
    }

    dmr_debug_dump_burst(opts, state, ctx->internalslot, 0x10);
    ctx->skipcount = 0;
    DSD_FPRINTF(stderr, "%s ", ctx->timestr);

    char polarity[3];
    char light[18];
    uint8_t vc = prepare_dmr_bs_voice_slot(opts, state, ctx, light, polarity);

    if (state->dmr_color_code != 16) {
        DSD_FPRINTF(stderr, "Sync: %sDMR %s| Color Code=%02d | VC%d ", polarity, light, state->dmr_color_code, vc);
    } else {
        DSD_FPRINTF(stderr, "Sync: %sDMR %s| Color Code=XX | VC%d ", polarity, light, vc);
    }

    handle_dmr_bs_slot_vc6_pre_link(opts, state, ctx);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    copy_dmr_bs_voice_frames(ctx->m1, ctx->m2, ctx->m3, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);
    apply_dmr_bs_voice_keystream(state, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);

    process_dmr_bs_three_voice_frames(opts, state, ctx->internalslot, vc, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);

    if (opts->use_dsp_output == 1) {
        write_dmr_bs_dsp_output(opts, state, ctx->internalslot);
    }

    run_dmr_bs_slot_vc6_sbrc(opts, state, ctx);
    (void)dmr_cach(opts, state, ctx->cachdata);

    if (opts->payload == 0) {
        DSD_FPRINTF(stderr, "\n");
    }

    run_dmr_bs_slot_vc6_post_voice(opts, state, ctx);
    collect_dmr_bs_late_entry(opts, state, ctx);
    if (vc == 6U) {
        dmr_vcrypt_superframe_end(state, ctx->internalslot);
    }
    increment_dmr_bs_slot_vc(ctx);
    update_dmr_bs_sync_times_if_tuned(opts, state, ctx->internalslot);

    ctx->tact_okay = 0;
    ctx->emb_ok = 0;
    if (ctx->vc1 > 14 || ctx->vc2 > 14) {
        return DMR_BS_ACTION_END;
    }

    return DMR_BS_ACTION_SKIP;
}

static dmr_bs_action
run_dmr_bs_post_skip(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    if (dmr_confidence_any_voice_open(state) && ctx->internalslot == 1 && opts->pulse_digi_rate_out == 8000) {
        playSynthesizedVoiceSS3(opts, state);
    }

    if (ctx->skipcount > 3) {
        ctx->tact_okay = 1;
        ctx->emb_ok = 1;
        return DMR_BS_ACTION_END;
    }

    if (dsd_telemetry_is_active()) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }

    dsd_event_sync_slot(opts, state, 0);
    dsd_event_sync_slot(opts, state, 1);
    dmr_sm_tick_ctx(dmr_sm_get_ctx(), opts, state);

    return DMR_BS_ACTION_CONTINUE;
}

static void DSD_ATTR_USED
finalize_dmr_bs(dsd_opts* opts, dsd_state* state, const dmr_bs_ctx* ctx) {
    state->dmr_stereo = 0;
    state->errs = 0;
    state->errs2 = 0;
    state->errs2R = 0;
    state->errs2 = 0;

    dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_BOTH, DSD_SYNC_DMR_BS_DATA_POS);

    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    dsd_audio_dmr_mix_media_discard(0);
    dsd_audio_dmr_mix_media_discard(1);

    if (ctx->tact_okay != 1 || ctx->emb_ok != 1) {
        DSD_FPRINTF(stderr, "%s ", ctx->timestr);
        DSD_FPRINTF(stderr, "Sync:  DMR                  ");
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "| VOICE CACH/EMB ERR");
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_FPRINTF(stderr, "\n");
        dmr_reset_blocks(opts, state);
    }

    state->static_ks_counter[0] = 0;
    state->static_ks_counter[1] = 0;
    state->vertex_ks_counter[0] = 0;
    state->vertex_ks_counter[1] = 0;
    state->vertex_ks_active_idx[0] = -1;
    state->vertex_ks_active_idx[1] = -1;
    state->vertex_ks_warned[0] = 0;
    state->vertex_ks_warned[1] = 0;
    state->dmr_emb_err[0] = 0;
    state->dmr_emb_err[1] = 0;
    dmr_confidence_reset(state);
}

static void
init_dmr_bs_bootstrap_ctx(dmr_bs_bootstrap_ctx* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->sync_okay = 1;
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COLON, ctx->timestr,
                                    sizeof ctx->timestr);
}

static void
seed_dmr_bs_bootstrap_payload(const dsd_opts* opts, dsd_state* state) {
    const int* dibit_p = state->dmr_payload_p - 90;

    for (int i = 0; i < 90; i++) {
        int dibit = *dibit_p;
        dibit_p++;
        if (opts->inverted_dmr == 1) {
            dibit = (dibit ^ 2) & 3;
        }
        state->dmr_stereo_payload[i] = dibit;
    }
}

static int
decode_dmr_bs_bootstrap_cach_and_tact(dsd_state* state, dmr_bs_bootstrap_ctx* ctx) {
    for (int i = 0; i < 12; i++) {
        int dibit = state->dmr_stereo_payload[i];
        ctx->cachdata[dmr_cach_interleave[((size_t)i * 2)]] = (1 & (dibit >> 1));
        ctx->cachdata[dmr_cach_interleave[((size_t)i * 2) + 1]] = (1 & dibit);
    }

    for (int i = 0; i < 7; i++) {
        ctx->tact_bits[i] = ctx->cachdata[i];
    }

    ctx->tact_okay = 0;
    if (Hamming_7_4_decode(ctx->tact_bits)) {
        ctx->tact_okay = 1;
    }
    if (ctx->tact_okay != 1) {
        return 0;
    }

    ctx->internalslot = ctx->tact_bits[1];
    state->currentslot = ctx->internalslot;
    dmr_confidence_note_voice_sync(state, ctx->internalslot);
    reset_dmr_bs_slot_keystream_counters(state, ctx->internalslot);
    return 1;
}

static int
collect_dmr_bs_bootstrap_prefetched_voice(const dsd_state* state, dmr_bs_bootstrap_ctx* ctx) {
    unpack_dmr_bs_ambe_segment_from_payload(state, ctx->ambe_fr, 12, 36, 0);
    unpack_dmr_bs_ambe_segment_from_payload(state, ctx->ambe_fr2, 48, 18, 0);
    extract_dmr_bs_sync_from_payload(state, 66, ctx->sync);

    if (strcmp(ctx->sync, DMR_BS_VOICE_SYNC) != 0) {
        ctx->sync_okay = 0;
        return 0;
    }

    return 1;
}

static void
prepare_dmr_bs_bootstrap_slot_output(const dsd_opts* opts, uint8_t internalslot, char light[18], char polarity[3]) {
    if (internalslot == 0) {
        DSD_SNPRINTF(light, 18, "%s", " [SLOT1]  slot2  ");
    } else {
        DSD_SNPRINTF(light, 18, "%s", "  slot1  [SLOT2] ");
    }

    if (opts->inverted_dmr == 0) {
        DSD_SNPRINTF(polarity, 3, "%s", "+");
    } else {
        DSD_SNPRINTF(polarity, 3, "%s", "-");
    }
}

static void
process_dmr_bs_bootstrap_voice_if_open(dsd_opts* opts, dsd_state* state, dmr_bs_bootstrap_ctx* ctx) {
    dmr_alg_reset(opts, state);
    if (!dmr_confidence_voice_slot_open(state, ctx->internalslot)) {
        return;
    }

    char polarity[3];
    char light[18];
    prepare_dmr_bs_bootstrap_slot_output(opts, ctx->internalslot, light, polarity);

    DSD_FPRINTF(stderr, "%s ", ctx->timestr);
    if (state->dmr_color_code != 16) {
        DSD_FPRINTF(stderr, "Sync: %sDMR %s| Color Code=%02d | VC1*", polarity, light, state->dmr_color_code);
    } else {
        DSD_FPRINTF(stderr, "Sync: %sDMR %s| Color Code=XX | VC1*", polarity, light);
    }

    copy_dmr_bs_voice_frames(ctx->m1, ctx->m2, ctx->m3, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);
    apply_dmr_bs_voice_keystream(state, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    process_dmr_bs_three_voice_frames(opts, state, ctx->internalslot, 1U, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);

    dmr_late_entry_mi_fragment(opts, state, 1, ctx->m1, ctx->m2, ctx->m3);

    (void)dmr_cach(opts, state, ctx->cachdata);
    if (opts->payload == 0) {
        DSD_FPRINTF(stderr, "\n");
    }

    if (opts->trunk_is_tuned == 1 && dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, ctx->internalslot)) {
        state->last_vc_sync_time = dsd_decode_time();
        state->last_vc_sync_time_m = dsd_decode_now_mono_s();
    }

    if (ctx->internalslot == 1 && opts->pulse_digi_rate_out == 8000) {
        playSynthesizedVoiceSS3(opts, state);
    }
}

/* The carrier boundary moved state->carrier_seq while a burst was read (issue #575): the burst straddles two carriers,
   and what it carries from the carrier left (the first AMBE frames, the EMB colour code, a CACH fragment that may
   complete a short LC, the embedded LC the sixth burst assembles) would decode after the boundary ended that carrier's
   calls. Or an input interruption cut the burst, and the rest of it is erasures the reader stood in. Nothing in it
   decodes; what earlier bursts gathered (data blocks, CACH fragments) goes, as on a sync loss. */
static void
drop_dmr_bs_carrier_left(dsd_opts* opts, dsd_state* state) {
    dmr_reset_blocks(opts, state);
}

static dmr_bs_action DSD_ATTR_USED
process_dmr_bs_iteration(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COLON, ctx->timestr,
                                    sizeof ctx->timestr);
    reset_dmr_bs_loop_buffers(ctx);
    const uint32_t carrier_seq = state->carrier_seq;

    if (!collect_dmr_bs_cach_and_tact(opts, state, ctx)) {
        return DMR_BS_ACTION_END;
    }

    read_dmr_bs_ambe_segment_stream(opts, state, ctx->ambe_fr, 12, 36, 0, ctx->redundancyA);
    if (is_dmr_bs_redundant_carrier(ctx)) {
        return DMR_BS_ACTION_END;
    }
    DSD_MEMCPY(ctx->redundancyB, ctx->redundancyA, sizeof(ctx->redundancyA));

    /* The sync field follows these 18 dibits: the symbol grid records from the last of them, a symbol ahead of the sync
       word for its shifts to read into, for note_dmr_bs_sync_anchor(). */
    read_dmr_bs_ambe_segment_stream(opts, state, ctx->ambe_fr2, 48, 17, 0, NULL);
    dsd_symbol_arm_sync_anchor(state);
    read_dmr_bs_ambe_segment_stream(opts, state, ctx->ambe_fr2, 65, 1, 17, NULL);
    read_dmr_bs_sync_segment(opts, state, ctx);
    if (dmr_bs_polarity_is_inverted(opts, state, ctx)) {
        /* Not a decode failure: the hunt starts again at the polarity the bursts proved. */
        ctx->tact_okay = 1;
        ctx->emb_ok = 1;
        return DMR_BS_ACTION_END;
    }
    note_dmr_bs_sync_anchor(state, ctx);
    if (strcmp(ctx->sync, DMR_BS_VOICE_SYNC) == 0) {
        /* Burst A starts the slot's next superframe: catch the MI up over any the loop did not see. */
        dmr_vcrypt_superframe_start(state, ctx->internalslot);
    }
    build_dmr_bs_emb_pdu(ctx);
    read_dmr_bs_ambe_segment_stream(opts, state, ctx->ambe_fr2, 90, 18, 18, NULL);
    read_dmr_bs_ambe_segment_stream(opts, state, ctx->ambe_fr3, 108, 36, 0, NULL);
    if (dsd_state_frame_read_lost(state, carrier_seq)) {
        drop_dmr_bs_carrier_left(opts, state);
        /* A carrier left, or a cut burst, is not a CACH/EMB decode failure. */
        ctx->tact_okay = 1;
        ctx->emb_ok = 1;
        return DMR_BS_ACTION_END;
    }

    note_dmr_bs_voice_sync(state, ctx);

    dmr_bs_action action = handle_dmr_bs_data_sync(opts, state, ctx);
    if (action != DMR_BS_ACTION_CONTINUE) {
        goto HANDLE_ACTION;
    }

    action = handle_dmr_bs_frame_sync_miss(ctx, &ctx->vc1, &ctx->vc2);
    if (action != DMR_BS_ACTION_CONTINUE) {
        goto HANDLE_ACTION;
    }

    if (strcmp(ctx->sync, DMR_BS_DATA_SYNC) != 0) {
        action = process_dmr_bs_voice_burst(opts, state, ctx);
        if (action != DMR_BS_ACTION_CONTINUE) {
            goto HANDLE_ACTION;
        }
    }

    action = DMR_BS_ACTION_SKIP;

HANDLE_ACTION:
    if (action == DMR_BS_ACTION_SKIP) {
        return run_dmr_bs_post_skip(opts, state, ctx);
    }
    if (action == DMR_BS_ACTION_END) {
        return DMR_BS_ACTION_END;
    }

    return DMR_BS_ACTION_CONTINUE;
}

static void
dmr_bs_run(dsd_opts* opts, dsd_state* state, int bootstrap_slot_known) {
    dmr_bs_ctx ctx;
    init_dmr_bs_ctx(state, &ctx, bootstrap_slot_known);

    DSD_SNPRINTF(state->slot1light, sizeof(state->slot1light), " slot1 ");
    DSD_SNPRINTF(state->slot2light, sizeof(state->slot2light), " slot2 ");

    state->color_code_ok = 0;
    state->dmr_emb_err[0] = 0;
    state->dmr_emb_err[1] = 0;

    while (1) {
        if (dsd_exitflag_load() == 1) {
            dsd_request_shutdown(opts, state);
            break;
        }
        /* The engine owns scan advances, and the frame loop applies the commands that move the receiver or change what
         * it decodes (a scan advance, hold or avoid among them): a carrier that keys for minutes would otherwise hold
         * them off until it drops (issue #637). Unwind at a burst boundary before either runs, so finalization cannot
         * clear the next target's decoder/audio state. Other commands wait for the carrier to end. */
        if (dsd_frame_sync_hook_scan_visit_should_yield(opts, state) || dsd_runtime_controls_need_frame_loop()) {
            /* An intentional scan departure is not a CACH/EMB decode failure. */
            ctx.tact_okay = 1;
            ctx.emb_ok = 1;
            break;
        }
        if (process_dmr_bs_iteration(opts, state, &ctx) == DMR_BS_ACTION_END) {
            break;
        }
    }

    finalize_dmr_bs(opts, state, &ctx);
}

void
dmrBS(dsd_opts* opts, dsd_state* state) {
    dmr_bs_run(opts, state, 1);
}

/* The burst's read-back is not all the stream's (dsd_state_sync_readback_invalid(): a cold start or an acquisition
   reset left no soft history to re-read what noCarrier() cleared). Its CACH, which names the burst's slot, is among
   what is not, so nothing of the burst decodes: its second half is read past and the loop takes the next burst, which
   it reads whole. */
static void
dmr_bs_bootstrap_past_unread_burst(dsd_opts* opts, dsd_state* state) {
    char frame[4][24];
    const uint32_t carrier_seq = state->carrier_seq;
    read_dmr_bs_ambe_segment_stream(opts, state, frame, 90, 18, 18, NULL);
    read_dmr_bs_ambe_segment_stream(opts, state, frame, 108, 36, 0, NULL);
    if (dsd_state_frame_read_lost(state, carrier_seq)) {
        drop_dmr_bs_carrier_left(opts, state);
        dmr_confidence_reset(state);
        return;
    }
    dmr_bs_run(opts, state, 0);
}

//Process buffered half frame and 2nd half and then jump to full BS decoding
void
dmrBSBootstrap(dsd_opts* opts, dsd_state* state) {
    dmr_bs_bootstrap_ctx ctx;
    init_dmr_bs_bootstrap_ctx(&ctx);
    /* Two-slot decoding: each burst's own slot is the receive slot (issue #637). */
    dmr_mono_slot_forget(state);

    if (dsd_state_sync_readback_invalid(state) != 0U) {
        dmr_bs_bootstrap_past_unread_burst(opts, state);
        return;
    }

    seed_dmr_bs_bootstrap_payload(opts, state);

    if (!decode_dmr_bs_bootstrap_cach_and_tact(state, &ctx)) {
        goto END;
    }

    if (!collect_dmr_bs_bootstrap_prefetched_voice(state, &ctx)) {
        goto END;
    }
    /* The voice sync the hunt just accepted is burst A: the slot's superframe starts here (issue #729), and the
       burst is one of the polarity run's. */
    dmr_vcrypt_superframe_start(state, ctx.internalslot);
    (void)dmr_bs_note_voice_sync_run(state, 1);

    dmr_sm_emit_voice_sync(opts, state, ctx.internalslot);

    const uint32_t carrier_seq = state->carrier_seq;
    read_dmr_bs_ambe_segment_stream(opts, state, ctx.ambe_fr2, 90, 18, 18, NULL);
    read_dmr_bs_ambe_segment_stream(opts, state, ctx.ambe_fr3, 108, 36, 0, NULL);
    if (dsd_state_frame_read_lost(state, carrier_seq)) {
        /* The burst's second half came after the carrier boundary (issue #575), or an input interruption cut it: the
           burst is dropped, as in the loop. */
        drop_dmr_bs_carrier_left(opts, state);
        dmr_confidence_reset(state);
        return;
    }

    if (opts->use_dsp_output == 1) {
        write_dmr_bs_dsp_output(opts, state, ctx.internalslot);
    }

    dmr_debug_dump_burst(opts, state, ctx.internalslot, 0x10);
    process_dmr_bs_bootstrap_voice_if_open(opts, state, &ctx);
    dmrBS(opts, state);

END:
    if (ctx.tact_okay != 1 || ctx.sync_okay != 1) {
        dmr_confidence_reset(state);
        DSD_FPRINTF(stderr, "%s ", ctx.timestr);
        DSD_FPRINTF(stderr, "Sync:  DMR                  ");
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "| VOICE CACH/SYNC ERR");
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_FPRINTF(stderr, "\n");
        dmr_reset_blocks(opts, state);
    }
}
