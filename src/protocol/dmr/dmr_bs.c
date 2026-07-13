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

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dmr_confidence.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

typedef enum {
    DMR_BS_ACTION_CONTINUE = 0,
    DMR_BS_ACTION_SKIP,
    DMR_BS_ACTION_END,
} dmr_bs_action;

static const int dmr_bs_cach_interleave[24] = {
    0, 7, 8, 9, 1, 10, 11, 12, 2, 13, 14, 15, 3, 16, 4, 17, 18, 19, 5, 20, 21, 22, 6, 23,
};

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
init_dmr_bs_ctx(const dsd_state* state, dmr_bs_ctx* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->cc = 25;
    ctx->power = 9;
    ctx->vc1 = 7;
    ctx->vc2 = 7;
    if (state->currentslot == 0) {
        ctx->vc1 = 2;
    } else if (state->currentslot == 1) {
        ctx->vc2 = 2;
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

static void
read_dmr_bs_ambe_segment_stream(dsd_opts* opts, dsd_state* state, char frame[4][24], int payload_offset,
                                int dibit_count, int interleave_offset, char* redundancy_out) {
    const int *w = dmr_ambe_interleave_w + interleave_offset, *x = dmr_ambe_interleave_x + interleave_offset,
              *y = dmr_ambe_interleave_y + interleave_offset, *z = dmr_ambe_interleave_z + interleave_offset;
    for (int i = 0; i < dibit_count; i++) {
        int dibit = get_dibit_and_analog_signal(opts, state, NULL);
        state->dmr_stereo_payload[payload_offset + i] = dibit;
        if (redundancy_out != NULL) {
            redundancy_out[i] = (char)dibit;
        }
        frame[*w][*x] = (1 & (dibit >> 1));
        frame[*y][*z] = (1 & dibit);
        w++;
        x++;
        y++;
        z++;
    }
}

static void
unpack_dmr_bs_ambe_segment_from_payload(const dsd_state* state, char frame[4][24], int payload_offset, int dibit_count,
                                        int interleave_offset) {
    const int *w = dmr_ambe_interleave_w + interleave_offset, *x = dmr_ambe_interleave_x + interleave_offset,
              *y = dmr_ambe_interleave_y + interleave_offset, *z = dmr_ambe_interleave_z + interleave_offset;
    for (int i = 0; i < dibit_count; i++) {
        int dibit = state->dmr_stereo_payload[payload_offset + i];
        frame[*w][*x] = (1 & (dibit >> 1));
        frame[*y][*z] = (1 & dibit);
        w++;
        x++;
        y++;
        z++;
    }
}

static void
read_dmr_bs_sync_segment(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    for (int i = 0; i < 24; i++) {
        int dibit = get_dibit_and_analog_signal(opts, state, NULL);
        state->dmr_stereo_payload[i + 66] = dibit;
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

static void
extract_dmr_bs_sync_from_payload(const dsd_state* state, int payload_offset, char sync[25]) {
    for (int i = 0; i < 24; i++) {
        int dibit = state->dmr_stereo_payload[payload_offset + i];
        sync[i] = (dibit | 1) + 48;
    }
    sync[24] = 0;
}

static int
collect_dmr_bs_cach_and_tact(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    for (int i = 0; i < 12; i++) {
        int dibit = get_dibit_and_analog_signal(opts, state, NULL);
        state->dmr_stereo_payload[i] = dibit;
        ctx->cachdata[dmr_bs_cach_interleave[((size_t)i * 2)]] = (1 & (dibit >> 1));
        ctx->cachdata[dmr_bs_cach_interleave[((size_t)i * 2) + 1]] = (1 & dibit);
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
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    }
    if (ctx->internalslot == 1) {
        ctx->vc2 = 7;
        if (opts->mbe_out_fR != NULL) {
            closeMbeOutFileR(opts, state);
        }
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

static dmr_bs_action
check_dmr_bs_emb_and_confidence(dsd_state* state, dmr_bs_ctx* ctx) {
    ctx->emb_ok = QR_16_7_6_decode(ctx->emb_pdu) ? 1 : 0;

    if (ctx->emb_ok == 1) {
        state->dmr_emb_err[ctx->internalslot] = 0;
        ctx->cc = (uint8_t)((ctx->emb_pdu[0] << 3) + (ctx->emb_pdu[1] << 2) + (ctx->emb_pdu[2] << 1) + ctx->emb_pdu[3]);
        ctx->power = ctx->emb_pdu[4];
        state->color_code = ctx->cc;
    } else {
        if (strcmp(ctx->sync, DMR_BS_VOICE_SYNC) != 0) {
            uint8_t* miss = &state->dmr_emb_err[ctx->internalslot];
            if (*miss < 0xFF) {
                (*miss)++;
            }
            if (*miss >= 2) {
                return DMR_BS_ACTION_END;
            }
        } else {
            state->dmr_emb_err[ctx->internalslot] = 0;
        }
    }

    int voice_gate_open = dmr_confidence_voice_slot_open(state, ctx->internalslot);
    if (ctx->emb_ok == 1) {
        dmr_confidence_result confidence = dmr_confidence_note_voice_burst(state, ctx->internalslot, ctx->cc);
        if (confidence == DMR_CONFIDENCE_REJECT) {
            ctx->emb_ok = 0;
            dmr_confidence_reset_slot(state, ctx->internalslot);
            return DMR_BS_ACTION_END;
        }
        voice_gate_open = dmr_confidence_voice_slot_open(state, ctx->internalslot);
    }

    if (voice_gate_open == 0) {
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

    return DMR_BS_ACTION_CONTINUE;
}

static uint8_t
prepare_dmr_bs_voice_slot(dsd_opts* opts, dsd_state* state, const dmr_bs_ctx* ctx, char light[18], char polarity[3]) {
    uint8_t vc = 0;

    if (ctx->internalslot == 0) {
        state->dmrburstL = 16;
        vc = ctx->vc1;
        DSD_SNPRINTF(light, 18, "%s", " [SLOT1]  slot2  ");
        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
            openMbeOutFile(opts, state);
        }
        dmr_sm_emit_voice_sync(opts, state, 0);
    } else {
        state->dmrburstR = 16;
        vc = ctx->vc2;
        DSD_SNPRINTF(light, 18, "%s", "  slot1  [SLOT2] ");
        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_fR == NULL)) {
            openMbeOutFileR(opts, state);
        }
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
            hytera_enhanced_alg_refresh(state);
        }
        DSD_FPRINTF(stderr, "\n");
        dmr_data_burst_handler(opts, state, (uint8_t*)ctx->dummy_bits, 0xEB, NULL);
    }

    if (ctx->internalslot == 1 && ctx->vc2 == 6) {
        if (state->payload_algidR == 0x02) {
            hytera_enhanced_alg_refresh(state);
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
        DSD_MEMCPY(state->f_l4[frame_index], state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf));
        DSD_MEMCPY(state->s_l4[frame_index], state->s_l, sizeof(state->s_l));
        DSD_MEMCPY(state->s_l4u[frame_index], state->s_lu, sizeof(state->s_lu));
    } else {
        DSD_MEMCPY(state->f_r4[frame_index], state->audio_out_temp_bufR, sizeof(state->audio_out_temp_bufR));
        DSD_MEMCPY(state->s_r4[frame_index], state->s_r, sizeof(state->s_r));
        DSD_MEMCPY(state->s_r4u[frame_index], state->s_ru, sizeof(state->s_ru));
    }
}

static void
process_dmr_bs_voice_frame(dsd_opts* opts, dsd_state* state, uint8_t internalslot, char frame[4][24],
                           uint8_t frame_index) {
    processMbeFrame(opts, state, NULL, frame, NULL);
    store_dmr_bs_processed_audio(state, internalslot, frame_index);
}

static void
process_dmr_bs_three_voice_frames(dsd_opts* opts, dsd_state* state, uint8_t internalslot, char ambe_fr[4][24],
                                  char ambe_fr2[4][24], char ambe_fr3[4][24]) {
    process_dmr_bs_voice_frame(opts, state, internalslot, ambe_fr, 0);
    process_dmr_bs_voice_frame(opts, state, internalslot, ambe_fr2, 1);
    process_dmr_bs_voice_frame(opts, state, internalslot, ambe_fr3, 2);
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
run_dmr_bs_slot_vc6_sbrc(const dsd_opts* opts, dsd_state* state, const dmr_bs_ctx* ctx) {
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

static void
update_dmr_bs_sync_times_if_tuned(const dsd_opts* opts, dsd_state* state) {
    if (opts->p25_is_tuned == 1 || opts->trunk_is_tuned == 1) {
        time_t now = time(NULL);
        double nowm = dsd_time_now_monotonic_s();
        state->last_vc_sync_time = now;
        state->last_vc_sync_time_m = nowm;
        state->last_cc_sync_time = now;
        state->last_cc_sync_time_m = nowm;
    }
}

static void
collect_dmr_bs_late_entry_if_needed(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    if (opts->dmr_le != 2) {
        dmr_late_entry_mi_fragment(opts, state, current_dmr_bs_vc(ctx) % 7, ctx->m1, ctx->m2, ctx->m3);
    }
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

#ifdef PRINT_AMBE72
    ambe2_codeword_print_i(opts, ctx->ambe_fr);
    ambe2_codeword_print_i(opts, ctx->ambe_fr2);
    ambe2_codeword_print_i(opts, ctx->ambe_fr3);
#endif

    process_dmr_bs_three_voice_frames(opts, state, ctx->internalslot, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);

    if (opts->use_dsp_output == 1) {
        write_dmr_bs_dsp_output(opts, state, ctx->internalslot);
    }

    run_dmr_bs_slot_vc6_sbrc(opts, state, ctx);
    (void)dmr_cach(opts, state, ctx->cachdata);

    if (opts->payload == 0) {
        DSD_FPRINTF(stderr, "\n");
    }

    run_dmr_bs_slot_vc6_post_voice(opts, state, ctx);
    collect_dmr_bs_late_entry_if_needed(opts, state, ctx);
    increment_dmr_bs_slot_vc(ctx);
    update_dmr_bs_sync_times_if_tuned(opts, state);

    ctx->tact_okay = 0;
    ctx->emb_ok = 0;
    if (ctx->vc1 > 14 || ctx->vc2 > 14) {
        return DMR_BS_ACTION_END;
    }

    return DMR_BS_ACTION_SKIP;
}

#ifdef RC_TESTING
static dmr_bs_action
handle_dmr_bs_reverse_channel_testing(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    if ((strcmp(ctx->sync, DMR_BS_DATA_SYNC) == 0) || (strcmp(ctx->sync, DMR_BS_VOICE_SYNC) == 0)) {
        return DMR_BS_ACTION_CONTINUE;
    }

    unsigned char SlotType[20];
    DSD_MEMSET(SlotType, 0, sizeof(SlotType));

    uint8_t k = 61;
    for (uint8_t i = 0; i < 5; i++) {
        SlotType[(i * 2) + 0] = (state->dmr_stereo_payload[k + 0] >> 1) & 1;
        SlotType[(i * 2) + 1] = (state->dmr_stereo_payload[k++] >> 0) & 1;
    }

    k = 90;
    for (uint8_t i = 0; i < 5; i++) {
        SlotType[(i * 2) + 10] = (state->dmr_stereo_payload[k + 0] >> 1) & 1;
        SlotType[(i * 2) + 11] = (state->dmr_stereo_payload[k++] >> 0) & 1;
    }

    if (!(QR_16_7_6_decode(ctx->emb_pdu) && ctx->emb_pdu[4] && ctx->tact_okay == 1)) {
        return DMR_BS_ACTION_CONTINUE;
    }

    int slot_type_ok = Golay_20_8_decode(SlotType);
    if (slot_type_ok) {
        DSD_FPRINTF(stderr, "%s ", ctx->timestr);
        if (opts->inverted_dmr == 0) {
            DSD_FPRINTF(stderr, "Sync: +RC   ");
        } else {
            DSD_FPRINTF(stderr, "Sync: -RC   ");
        }
        dmr_data_sync(opts, state);
    }

    for (int i = 0; i < 48; i++) {
        state->dmr_embedded_signalling[ctx->internalslot][5][i] = ctx->syncdata[i];
    }

    dmr_sbrc(opts, state, ctx->emb_pdu[4]);
    ctx->emb_ok = 1;

    beeper(opts, state, ctx->internalslot, 40, 86, 3);
    beeper(opts, state, ctx->internalslot, 80, 86, 3);

    watchdog_event_datacall(opts, state, 0, 0, "DMR Reverse Channel P/PI Indicator On (FEC Okay);", ctx->internalslot);
    push_event_history(&state->event_history_s[ctx->internalslot]);
    init_event_history(&state->event_history_s[ctx->internalslot], 0, 1);

    if (slot_type_ok) {
        ctx->skipcount++;
        return DMR_BS_ACTION_SKIP;
    }

    return DMR_BS_ACTION_CONTINUE;
}
#endif

static dmr_bs_action
run_dmr_bs_post_skip(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    if (dmr_confidence_any_voice_open(state) && ctx->internalslot == 1 && opts->floating_point == 1
        && opts->pulse_digi_rate_out == 8000) {
        playSynthesizedVoiceFS3(opts, state);
    }

    if (dmr_confidence_any_voice_open(state) && ctx->internalslot == 1 && opts->floating_point == 0
        && opts->pulse_digi_rate_out == 8000) {
        playSynthesizedVoiceSS3(opts, state);
    }

    if (ctx->skipcount > 3) {
        ctx->tact_okay = 1;
        ctx->emb_ok = 1;
        return DMR_BS_ACTION_END;
    }

    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }

    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
    watchdog_event_history(opts, state, 1);
    watchdog_event_current(opts, state, 1);
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

    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if (opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }

    DSD_MEMSET(state->f_l4, 0.0f, sizeof(state->f_l4));
    DSD_MEMSET(state->f_r4, 0.0f, sizeof(state->f_r4));
    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));

    if (ctx->tact_okay != 1 || ctx->emb_ok != 1) {
        DSD_FPRINTF(stderr, "%s ", ctx->timestr);
        DSD_FPRINTF(stderr, "Sync:  DMR                  ");
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "| VOICE CACH/EMB ERR");
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_FPRINTF(stderr, "\n");
        dmr_refresh_algids_on_error(opts, state);
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
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, ctx->timestr, sizeof ctx->timestr);
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
        ctx->cachdata[dmr_bs_cach_interleave[((size_t)i * 2)]] = (1 & (dibit >> 1));
        ctx->cachdata[dmr_bs_cach_interleave[((size_t)i * 2) + 1]] = (1 & dibit);
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
prepare_dmr_bs_bootstrap_slot_output(dsd_opts* opts, dsd_state* state, uint8_t internalslot, char light[18],
                                     char polarity[3]) {
    if (internalslot == 0) {
        DSD_SNPRINTF(light, 18, "%s", " [SLOT1]  slot2  ");
        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
            openMbeOutFile(opts, state);
        }
    } else {
        DSD_SNPRINTF(light, 18, "%s", "  slot1  [SLOT2] ");
        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_fR == NULL)) {
            openMbeOutFileR(opts, state);
        }
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
    prepare_dmr_bs_bootstrap_slot_output(opts, state, ctx->internalslot, light, polarity);

    DSD_FPRINTF(stderr, "%s ", ctx->timestr);
    if (state->dmr_color_code != 16) {
        DSD_FPRINTF(stderr, "Sync: %sDMR %s| Color Code=%02d | VC1*", polarity, light, state->dmr_color_code);
    } else {
        DSD_FPRINTF(stderr, "Sync: %sDMR %s| Color Code=XX | VC1*", polarity, light);
    }

    copy_dmr_bs_voice_frames(ctx->m1, ctx->m2, ctx->m3, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);
    apply_dmr_bs_voice_keystream(state, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);

#ifdef PRINT_AMBE72
    ambe2_codeword_print_i(opts, ctx->ambe_fr);
    ambe2_codeword_print_i(opts, ctx->ambe_fr2);
    ambe2_codeword_print_i(opts, ctx->ambe_fr3);
#endif

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    process_dmr_bs_three_voice_frames(opts, state, ctx->internalslot, ctx->ambe_fr, ctx->ambe_fr2, ctx->ambe_fr3);

    if (opts->dmr_le != 2) {
        dmr_late_entry_mi_fragment(opts, state, 1, ctx->m1, ctx->m2, ctx->m3);
    }

    (void)dmr_cach(opts, state, ctx->cachdata);
    if (opts->payload == 0) {
        DSD_FPRINTF(stderr, "\n");
    }

    if (opts->p25_is_tuned == 1 || opts->trunk_is_tuned == 1) {
        state->last_vc_sync_time = time(NULL);
        state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
    }

    if (ctx->internalslot == 1 && opts->floating_point == 1 && opts->pulse_digi_rate_out == 8000) {
        playSynthesizedVoiceFS3(opts, state);
    }

    if (ctx->internalslot == 1 && opts->floating_point == 0 && opts->pulse_digi_rate_out == 8000) {
        playSynthesizedVoiceSS3(opts, state);
    }
}

static dmr_bs_action DSD_ATTR_USED
process_dmr_bs_iteration(dsd_opts* opts, dsd_state* state, dmr_bs_ctx* ctx) {
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, ctx->timestr, sizeof ctx->timestr);
    reset_dmr_bs_loop_buffers(ctx);

    if (!collect_dmr_bs_cach_and_tact(opts, state, ctx)) {
        return DMR_BS_ACTION_END;
    }

    read_dmr_bs_ambe_segment_stream(opts, state, ctx->ambe_fr, 12, 36, 0, ctx->redundancyA);
    if (is_dmr_bs_redundant_carrier(ctx)) {
        return DMR_BS_ACTION_END;
    }
    DSD_MEMCPY(ctx->redundancyB, ctx->redundancyA, sizeof(ctx->redundancyA));

    read_dmr_bs_ambe_segment_stream(opts, state, ctx->ambe_fr2, 48, 18, 0, NULL);
    read_dmr_bs_sync_segment(opts, state, ctx);
    build_dmr_bs_emb_pdu(ctx);
    read_dmr_bs_ambe_segment_stream(opts, state, ctx->ambe_fr2, 90, 18, 18, NULL);
    read_dmr_bs_ambe_segment_stream(opts, state, ctx->ambe_fr3, 108, 36, 0, NULL);

    note_dmr_bs_voice_sync(state, ctx);

    dmr_bs_action action = handle_dmr_bs_data_sync(opts, state, ctx);
    if (action != DMR_BS_ACTION_CONTINUE) {
        goto HANDLE_ACTION;
    }

#ifdef RC_TESTING
    action = handle_dmr_bs_reverse_channel_testing(opts, state, ctx);
    if (action != DMR_BS_ACTION_CONTINUE) {
        goto HANDLE_ACTION;
    }
#endif

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

void
dmrBS(dsd_opts* opts, dsd_state* state) {
    dmr_bs_ctx ctx;
    init_dmr_bs_ctx(state, &ctx);

    DSD_SNPRINTF(state->slot1light, sizeof(state->slot1light), " slot1 ");
    DSD_SNPRINTF(state->slot2light, sizeof(state->slot2light), " slot2 ");

    state->color_code_ok = 0;
    state->dmr_emb_err[0] = 0;
    state->dmr_emb_err[1] = 0;

    while (1) {
        if (exitflag == 1) {
            dsd_request_shutdown(opts, state);
            break;
        }
        if (process_dmr_bs_iteration(opts, state, &ctx) == DMR_BS_ACTION_END) {
            break;
        }
    }

    finalize_dmr_bs(opts, state, &ctx);
}

//Process buffered half frame and 2nd half and then jump to full BS decoding
void
dmrBSBootstrap(dsd_opts* opts, dsd_state* state) {
    dmr_bs_bootstrap_ctx ctx;
    init_dmr_bs_bootstrap_ctx(&ctx);

    seed_dmr_bs_bootstrap_payload(opts, state);

    if (!decode_dmr_bs_bootstrap_cach_and_tact(state, &ctx)) {
        goto END;
    }

    if (!collect_dmr_bs_bootstrap_prefetched_voice(state, &ctx)) {
        goto END;
    }

    read_dmr_bs_ambe_segment_stream(opts, state, ctx.ambe_fr2, 90, 18, 18, NULL);
    read_dmr_bs_ambe_segment_stream(opts, state, ctx.ambe_fr3, 108, 36, 0, NULL);

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
        dmr_refresh_algids_on_error(opts, state);
        dmr_reset_blocks(opts, state);
    }
}
