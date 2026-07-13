// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * dmr_ms.c
 * DMR MS/Simplex/Direct Mode Voice Handling and Data Gathering Routines
 *
 * LWVMOBILE
 * 2022-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

typedef struct dmr_ms_voice_frames {
    char ambe_fr[4][24];
    char ambe_fr2[4][24];
    char ambe_fr3[4][24];
    char ambe_fr4[4][24];
    uint8_t m1[4][24];
    uint8_t m2[4][24];
    uint8_t m3[4][24];
} dmr_ms_voice_frames;

static void
dmr_ms_reset_ks_counter(dsd_state* state) {
    state->static_ks_counter[0] = 0;
    state->vertex_ks_counter[0] = 0;
    state->vertex_ks_active_idx[0] = -1;
    state->vertex_ks_warned[0] = 0;
}

static int
dmr_ms_apply_inversion(const dsd_opts* opts, int dibit, int mask_after_xor) {
    if (opts->inverted_dmr == 1) {
        dibit ^= 2;
        if (mask_after_xor != 0) {
            dibit &= 3;
        }
    }
    return dibit;
}

static int
dmr_ms_read_dibit(dsd_opts* opts, dsd_state* state, int mask_after_xor) {
    int dibit = get_dibit_and_analog_signal(opts, state, NULL);
    return dmr_ms_apply_inversion(opts, dibit, mask_after_xor);
}

static void
dmr_ms_store_ambe_dibit(char ambe_fr[4][24], int index, int dibit) {
    const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[index];
    ambe_fr[map->high_row][map->high_col] = (1 & (dibit >> 1)); // bit 1
    ambe_fr[map->low_row][map->low_col] = (1 & dibit);          // bit 0
}

static void
dmr_ms_init_voice_frames(dmr_ms_voice_frames* frames) {
    DSD_MEMSET(frames->ambe_fr, 0, sizeof(frames->ambe_fr));
    DSD_MEMSET(frames->ambe_fr2, 0, sizeof(frames->ambe_fr2));
    DSD_MEMSET(frames->ambe_fr3, 0, sizeof(frames->ambe_fr3));
}

static void
dmr_ms_read_cach(dsd_opts* opts, dsd_state* state, char cachdata[25]) {
    for (int i = 0; i < 12; i++) {
        int dibit = dmr_ms_read_dibit(opts, state, 1);
        cachdata[i] = (char)dibit;
        state->dmr_stereo_payload[i] = dibit;
    }
}

static uint8_t
dmr_ms_decode_tact(const char cachdata[25], uint8_t tact_bits[7]) {
    for (int i = 0; i < 7; i++) {
        tact_bits[i] = (uint8_t)cachdata[i];
    }
    return Hamming_7_4_decode(tact_bits) ? 1 : 0;
}

static void
dmr_ms_fill_ambe_from_stream(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], int payload_offset, int dibit_count,
                             int interleave_offset) {
    for (int i = 0; i < dibit_count; i++) {
        int dibit = dmr_ms_read_dibit(opts, state, 1);
        state->dmr_stereo_payload[payload_offset + i] = dibit;
        dmr_ms_store_ambe_dibit(ambe_fr, interleave_offset + i, dibit);
    }
}

static void
dmr_ms_fill_ambe_from_payload(const dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], int payload_offset,
                              int dibit_count, int interleave_offset, int write_back) {
    for (int i = 0; i < dibit_count; i++) {
        int dibit = dmr_ms_apply_inversion(opts, state->dmr_stereo_payload[payload_offset + i], 1);
        if (write_back != 0) {
            state->dmr_stereo_payload[payload_offset + i] = dibit;
        }
        dmr_ms_store_ambe_dibit(ambe_fr, interleave_offset + i, dibit);
    }
}

static void
dmr_ms_collect_sync_bits(dsd_opts* opts, dsd_state* state, uint8_t syncdata[48], uint8_t vc, uint8_t internalslot) {
    for (int i = 0; i < 24; i++) {
        int dibit = dmr_ms_read_dibit(opts, state, 0);
        state->dmr_stereo_payload[i + 66] = dibit;
        syncdata[((size_t)2 * i)] = (1 & (dibit >> 1)); // bit 1
        syncdata[((size_t)2 * i) + 1] = (1 & dibit);    // bit 0
        if (vc > 1) {
            state->dmr_embedded_signalling[internalslot][vc - 1][((size_t)i * 2)] = (1 & (dibit >> 1)); // bit 1
            state->dmr_embedded_signalling[internalslot][vc - 1][((size_t)i * 2) + 1] = (1 & dibit);    // bit 0
        }
    }
}

static uint8_t
dmr_ms_decode_embedded_color_code(dsd_state* state, const uint8_t syncdata[48], uint8_t emb_pdu[16]) {
    uint8_t power = 9; // power and pre-emption indicator
    for (int i = 0; i < 8; i++) {
        emb_pdu[i] = syncdata[i];
        emb_pdu[i + 8] = syncdata[i + 40];
    }
    if (QR_16_7_6_decode(emb_pdu)) {
        uint8_t cc = (uint8_t)((emb_pdu[0] << 3) + (emb_pdu[1] << 2) + (emb_pdu[2] << 1) + emb_pdu[3]);
        power = emb_pdu[4];
        state->dmr_color_code = state->color_code = cc;
    }
    return power;
}

static void
dmr_ms_dump_dsp_output(const dsd_opts* opts, const dsd_state* state) {
    if (opts->use_dsp_output != 1) {
        return;
    }

    FILE* pFile = dsd_fopen_private(opts->dsp_out_file, "a");
    if (pFile == NULL) {
        return;
    }

    DSD_FPRINTF(pFile, "\n%d 10 ", state->currentslot + 1); // 0x10 for "voice burst", forced to slot 1
    for (int i = 6; i < 72; i++)                            // 33 bytes, no CACH
    {
        int dsp_byte =
            (state->dmr_stereo_payload[((size_t)i * 2)] << 2) | state->dmr_stereo_payload[((size_t)i * 2) + 1];
        DSD_FPRINTF(pFile, "%X", dsp_byte);
    }
    fclose(pFile);
}

static void
dmr_ms_copy_frames_for_late_entry(dmr_ms_voice_frames* frames) {
    DSD_MEMCPY(frames->ambe_fr4, frames->ambe_fr2, sizeof(frames->ambe_fr2));
    DSD_MEMCPY(frames->m1, frames->ambe_fr, sizeof(frames->m1));
    DSD_MEMCPY(frames->m2, frames->ambe_fr2, sizeof(frames->m2));
    DSD_MEMCPY(frames->m3, frames->ambe_fr3, sizeof(frames->m3));
}

static void
dmr_ms_apply_keystream(dsd_state* state, dmr_ms_voice_frames* frames) {
    if (state->tyt_bp == 1) {
        tyt16_ambe2_codeword_keystream(state, frames->ambe_fr, 0);
        tyt16_ambe2_codeword_keystream(state, frames->ambe_fr2, 1);
        tyt16_ambe2_codeword_keystream(state, frames->ambe_fr3, 0);
    }
    if (state->csi_ee == 1) {
        csi72_ambe2_codeword_keystream(state, frames->ambe_fr);
        csi72_ambe2_codeword_keystream(state, frames->ambe_fr2);
        csi72_ambe2_codeword_keystream(state, frames->ambe_fr3);
    }
}

static void
dmr_ms_process_single_mbe(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], size_t frame_idx) {
    processMbeFrame(opts, state, NULL, ambe_fr, NULL);
    DSD_MEMCPY(state->f_l4[frame_idx], state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf));
    DSD_MEMCPY(state->s_l4[frame_idx], state->s_l, sizeof(state->s_l));
    DSD_MEMCPY(state->s_l4u[frame_idx], state->s_lu, sizeof(state->s_lu));
}

static void
dmr_ms_process_audio_frames(dsd_opts* opts, dsd_state* state, dmr_ms_voice_frames* frames) {
    dmr_ms_process_single_mbe(opts, state, frames->ambe_fr, 0);
    dmr_ms_process_single_mbe(opts, state, frames->ambe_fr2, 1);
    dmr_ms_process_single_mbe(opts, state, frames->ambe_fr3, 2);
}

static void
dmr_ms_play_voice(dsd_opts* opts, dsd_state* state) {
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS3(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS3(opts, state);
    }
}

static void
dmr_ms_handle_vc6_ops(dsd_opts* opts, dsd_state* state, uint8_t power, uint8_t dummy_bits[196]) {
    if (state->payload_algid == 0x02) {
        hytera_enhanced_alg_refresh(state);
    }
    dmr_data_burst_handler(opts, state, dummy_bits, 0xEB, NULL);
    dmr_sbrc(opts, state, power);
    DSD_FPRINTF(stderr, "\n");
    dmr_alg_refresh(opts, state);
}

static int
dmr_ms_advance_voice_cycle(dsd_opts* opts, dsd_state* state, uint8_t* vc) {
    dsd_mark_vc_sync(state);
    (*vc)++;
    if (*vc > 6) {
        return 0;
    }

    skipDibit(opts, state, 144); // skip to next TDMA channel
    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }

    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
    dmr_sm_tick_ctx(dmr_sm_get_ctx(), opts, state); // handle hangtime/release logic
    return 1;
}

static void
dmr_ms_prime_next_payload(dsd_opts* opts, dsd_state* state) {
    skipDibit(opts, state, 144); // should we have two of these?
    for (int i = 0; i < 66; i++) {
        state->dmr_stereo_payload[i] = dmr_ms_read_dibit(opts, state, 1);
    }
}

static void
dmr_ms_clear_mode_flags(dsd_state* state) {
    state->dmr_stereo = 0;
    state->dmr_ms_mode = 0;
    state->directmode = 0;
}

static void
dmr_ms_prepare_bootstrap_payload(dsd_state* state) {
    const int* dibit_p = state->dmr_payload_p - 90;
    for (int i = 0; i < 90; i++) {
        state->dmr_stereo_payload[i] = *dibit_p;
        dibit_p++;
    }
}

static void
dmr_ms_collect_bootstrap_cach(const dsd_opts* opts, const dsd_state* state, char cachdata[25]) {
    for (int i = 0; i < 12; i++) {
        int dibit = dmr_ms_apply_inversion(opts, state->dmr_stereo_payload[i], 1);
        cachdata[i] = (char)dibit;
    }
}

static void
dmr_ms_decode_bootstrap_voice(dsd_opts* opts, dsd_state* state, dmr_ms_voice_frames* frames) {
    dmr_ms_fill_ambe_from_payload(opts, state, frames->ambe_fr, 12, 36, 0, 1);
    dmr_ms_fill_ambe_from_payload(opts, state, frames->ambe_fr2, 48, 18, 0, 0);
    dmr_ms_fill_ambe_from_stream(opts, state, frames->ambe_fr2, 90, 18, 18);
    dmr_ms_fill_ambe_from_stream(opts, state, frames->ambe_fr3, 108, 36, 0);
}

static void
dmr_ms_dump_bootstrap_debug_burst(const dsd_opts* opts, const dsd_state* state) {
    if (opts == NULL || state == NULL || opts->dmr_debug_burst == 0) {
        return;
    }

    int debug_payload[144];
    DSD_MEMCPY(debug_payload, state->dmr_stereo_payload, sizeof(debug_payload));
    for (int i = 48; i < 90; i++) {
        debug_payload[i] = dmr_ms_apply_inversion(opts, debug_payload[i], 1);
    }

    char line[192];
    if (dmr_debug_format_burst_payload(line, sizeof(line), debug_payload, (uint8_t)state->currentslot, 0x10) == 0U) {
        return;
    }
    DSD_FPRINTF(stderr, "%s\n", line);
}

static void
dmr_ms_print_bootstrap_sync(const dsd_opts* opts, dsd_state* state, const char timestr[9]) {
    const char* sign = (opts->inverted_dmr == 0) ? "+" : "-";
    if (state->dmr_color_code != 16) {
        DSD_FPRINTF(stderr, "%s Sync: %sDMR MS/DM MODE/MONO | Color Code=%02d | VC* \n", timestr, sign,
                    state->dmr_color_code);
    } else {
        DSD_FPRINTF(stderr, "%s Sync: %sDMR MS/DM MODE/MONO | Color Code=XX | VC* \n", timestr, sign);
    }
}

void
dmrMS(dsd_opts* opts, dsd_state* state) {
    char timestr[9];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    UNUSED(timestr);

    dmr_ms_voice_frames frames;
    char cachdata[25];
    uint8_t tact_bits[7];
    uint8_t syncdata[48];
    uint8_t emb_pdu[16];
    uint8_t dummy_bits[196];
    uint8_t vc = 2;
    const uint8_t internalslot = 0;

    DSD_MEMSET(syncdata, 0, sizeof(syncdata));
    DSD_MEMSET(emb_pdu, 0, sizeof(emb_pdu));
    DSD_MEMSET(dummy_bits, 0, sizeof(dummy_bits));

    state->currentslot = 0;

    for (int j = 0; j < 6; j++) {
        state->dmrburstL = 16;
        dmr_sm_emit_voice_sync(opts, state, 0);

        dmr_ms_init_voice_frames(&frames);
        dmr_ms_read_cach(opts, state, cachdata);
        (void)dmr_ms_decode_tact(cachdata, tact_bits);

        dmr_ms_fill_ambe_from_stream(opts, state, frames.ambe_fr, 12, 36, 0);
        dmr_ms_fill_ambe_from_stream(opts, state, frames.ambe_fr2, 48, 18, 0);
        dmr_ms_collect_sync_bits(opts, state, syncdata, vc, internalslot);
        uint8_t power = dmr_ms_decode_embedded_color_code(state, syncdata, emb_pdu);
        dmr_ms_fill_ambe_from_stream(opts, state, frames.ambe_fr2, 90, 18, 18);
        dmr_ms_fill_ambe_from_stream(opts, state, frames.ambe_fr3, 108, 36, 0);
        dmr_debug_dump_burst(opts, state, state->currentslot, 0x10);
        dmr_ms_dump_dsp_output(opts, state);

        state->dmr_ms_mode = 1;
        dmr_ms_copy_frames_for_late_entry(&frames);
        dmr_ms_apply_keystream(state, &frames);
        dmr_ms_process_audio_frames(opts, state, &frames);
        dmr_ms_play_voice(opts, state);

        if (vc == 6) {
            dmr_ms_handle_vc6_ops(opts, state, power, dummy_bits);
        }
        if (opts->dmr_le != 2) {
            dmr_late_entry_mi_fragment(opts, state, vc, frames.m1, frames.m2, frames.m3);
        }
        if (!dmr_ms_advance_voice_cycle(opts, state, &vc)) {
            break;
        }
    }

    dmr_ms_prime_next_payload(opts, state);
    dmr_ms_clear_mode_flags(state);
    dmr_ms_reset_ks_counter(state);
}

//collect buffered 1st half and get 2nd half voice payload and then jump to full MS Voice decoding.
void
dmrMSBootstrap(dsd_opts* opts, dsd_state* state) {
    char timestr[9];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);

    dmr_ms_voice_frames frames;
    char cachdata[25];

    dmr_ms_reset_ks_counter(state);
    dmr_ms_init_voice_frames(&frames);

    state->dmrburstL = 16;
    state->currentslot = 0;
    dmr_sm_emit_voice_sync(opts, state, 0);

    dmr_ms_prepare_bootstrap_payload(state);
    dmr_ms_collect_bootstrap_cach(opts, state, cachdata);
    dmr_ms_decode_bootstrap_voice(opts, state, &frames);
    dmr_ms_dump_bootstrap_debug_burst(opts, state);
    dmr_ms_dump_dsp_output(opts, state);
    dmr_ms_print_bootstrap_sync(opts, state, timestr);

    dmr_ms_copy_frames_for_late_entry(&frames);
    dmr_ms_apply_keystream(state, &frames);
    dmr_ms_process_audio_frames(opts, state, &frames);
    dmr_ms_play_voice(opts, state);

    if (opts->dmr_le != 2) {
        dmr_late_entry_mi_fragment(opts, state, 1, frames.m1, frames.m2, frames.m3);
    }

    skipDibit(opts, state, 144); //skip to next TDMA slot
    dmrMS(opts, state);          //bootstrap into full TDMA frame
}

//simplied to a simple data collector, and then passed on to dmr_data_sync for the usual processing
void
dmrMSData(dsd_opts* opts, dsd_state* state) {

    char timestr[9];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);

    int i;
    int dibit;
    const int* dibit_p;

    //CACH + First Half Payload + Sync = 12 + 54 + 24
    dibit_p = state->dmr_payload_p - 90;
    for (i = 0; i < 90; i++) //90
    {
        dibit = *dibit_p;
        dibit_p++;
        if (opts->inverted_dmr == 1) {
            dibit = (dibit ^ 2) & 3;
        }
        state->dmr_stereo_payload[i] = dibit;
    }

    for (i = 0; i < 54; i++) {
        dibit = get_dibit_and_analog_signal(opts, state, NULL);
        if (opts->inverted_dmr == 1) {
            dibit = (dibit ^ 2) & 3;
        }
        state->dmr_stereo_payload[i + 90] = dibit;
    }

    DSD_FPRINTF(stderr, "%s ", timestr);
    if (opts->inverted_dmr == 0) {
        DSD_FPRINTF(stderr, "Sync: +DMR MS/DM MODE/MONO ");
    } else {
        DSD_FPRINTF(stderr, "Sync: -DMR MS/DM MODE/MONO ");
    }
    if (state->dmr_color_code != 16) {
        DSD_FPRINTF(stderr, "| Color Code=%02d ", state->dmr_color_code);
    } else {
        DSD_FPRINTF(stderr, "| Color Code=XX ");
    }

    DSD_SNPRINTF(state->slot1light, sizeof(state->slot1light), "%s", "");
    DSD_SNPRINTF(state->slot2light, sizeof(state->slot2light), "%s", "");

    //process data
    state->dmr_stereo = 1;
    state->dmr_ms_mode = 1;

    dmr_data_sync(opts, state);

    state->dmr_stereo = 0;
    state->dmr_ms_mode = 0;
    state->directmode = 0; //flag off

    //should just be loaded in the dmr_payload_buffer instead now
    //but we want to read dibits so the buffer has actual good values in it
    for (i = 0; i < 144; i++) { // 66
        (void)get_dibit_and_analog_signal(opts, state, NULL);
        state->dmr_stereo_payload[i] = 1; // set to one so first frame will fail intentionally instead of zero fill
    }
    //CACH + First Half Payload = 12 + 54
    for (i = 0; i < 66; i++) { // 66
        (void)get_dibit_and_analog_signal(opts, state, NULL);
        state->dmr_stereo_payload[i + 66] =
            1; ////set to one so first frame will fail intentionally instead of zero fill
    }

    /* stack buffer; no free */
}
