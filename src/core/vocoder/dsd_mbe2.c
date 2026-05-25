// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 *
 * AMBE/IMBE soft-demod and processing helpers
 * (consolidated and simplified handling)
 *
 * LWVMOBILE
 * 2023-07 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

//new and simplified/organized ambe and imbe handling
//moving all audio handling and decryption to seperate files for simplicity (eventually)

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <mbelib.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
save_ambe2450_x2(dsd_opts* opts, dsd_state* state, char ambe_d[49]) {
    /*
     * X2 exports both timeslots into opts->mbe_out_f, but slot-2 decode
     * error stats are tracked in errs2R.
     */
    int saved_errs2 = state->errs2;

    if (state->currentslot == 1) {
        state->errs2 = state->errs2R;
    }

    saveAmbe2450Data(opts, state, ambe_d);
    state->errs2 = saved_errs2;
}

//P25p1 IMBE 7200 or AMBE+2 EFR
static void
soft_demod_imbe7200(dsd_state* state, char imbe_fr7200[8][23], char imbe_d[88]) {
    state->errs = mbe_eccImbe7200x4400C0(imbe_fr7200);
    state->errs2 = state->errs;
    mbe_demodulateImbe7200x4400Data(imbe_fr7200);
    state->errs2 += mbe_eccImbe7200x4400Data(imbe_fr7200, imbe_d);
    state->debug_audio_errors += state->errs2;
}

//ProVoice IMBE 7100
static void
soft_demod_imbe7100(dsd_state* state, char imbe_fr7100[7][24], char imbe_d[88]) {
    state->errs = mbe_eccImbe7100x4400C0(imbe_fr7100);
    state->errs2 = state->errs;
    mbe_demodulateImbe7100x4400Data(imbe_fr7100);
    state->errs2 += mbe_eccImbe7100x4400Data(imbe_fr7100, imbe_d);
    state->debug_audio_errors += state->errs2;
}

//AMBE+2 EHR
static void
soft_demod_ambe2_ehr(dsd_state* state, char ambe2_ehr[4][24], char ambe_d[49]) {
    state->errs = mbe_eccAmbe3600x2450C0(ambe2_ehr);
    state->errs2 = state->errs;
    mbe_demodulateAmbe3600x2450Data(ambe2_ehr);
    state->errs2 += mbe_eccAmbe3600x2450Data(ambe2_ehr, ambe_d);
}

//AMBE One Shot (DSTAR)
static void
soft_demod_ambe_dstar(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], char ambe_d[49]) {
    mbe_processAmbe3600x2400Framef(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str, ambe_fr,
                                   ambe_d, state->cur_mp, state->prev_mp, state->prev_mp_enhanced, opts->uvquality);
    if (opts->floating_point == 1) {
        DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));
    } else {
        processAudio(opts, state);
    }
}

//AMBE+2 One Shot (X2-TDMA)
static void
soft_demod_ambe_x2(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], char ambe_d[49]) {
    mbe_parms* cur_mp = state->cur_mp;
    mbe_parms* prev_mp = state->prev_mp;
    mbe_parms* prev_mp_enhanced = state->prev_mp_enhanced;
    int* errs = &state->errs;
    int* errs2 = &state->errs2;
    char* err_str = state->err_str;

    if (state->currentslot == 1) {
        cur_mp = state->cur_mp2;
        prev_mp = state->prev_mp2;
        prev_mp_enhanced = state->prev_mp_enhanced2;
        errs = &state->errsR;
        errs2 = &state->errs2R;
        err_str = state->err_strR;
    }

    /*
     * Avoid mbe_processAmbe3600x2450Framef():
     * - Demodulate/ECC explicitly and then decode via mbe_processAmbe2450Dataf(), which is exported.
     */
    *errs = mbe_eccAmbe3600x2450C0(ambe_fr);
    *errs2 = *errs;
    mbe_demodulateAmbe3600x2450Data(ambe_fr);
    *errs2 += mbe_eccAmbe3600x2450Data(ambe_fr, ambe_d);
    mbe_processAmbe2450Dataf(state->audio_out_temp_buf, errs, errs2, err_str, ambe_d, cur_mp, prev_mp, prev_mp_enhanced,
                             opts->uvquality);
    if (opts->floating_point == 1) {
        DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));
    } else {
        processAudio(opts, state);
    }
}

static void
update_p25_p1_voice_err_hist(dsd_state* state) {
    int len = state->p25_p1_voice_err_hist_len > 0 ? state->p25_p1_voice_err_hist_len : 50;

    if (len > (int)sizeof(state->p25_p1_voice_err_hist)) {
        len = (int)sizeof(state->p25_p1_voice_err_hist);
    }
    state->p25_p1_voice_err_hist_len = len;

    {
        uint8_t pos = (uint8_t)state->p25_p1_voice_err_hist_pos;
        uint8_t old = state->p25_p1_voice_err_hist[pos % len];
        uint8_t val = (uint8_t)(state->errs2 & 0xFF);

        state->p25_p1_voice_err_hist[pos % len] = val;
        state->p25_p1_voice_err_hist_sum += val;
        state->p25_p1_voice_err_hist_sum -= old;
        state->p25_p1_voice_err_hist_pos = (pos + 1) % len;
    }
}

static void
play_synthesized_voice_by_output(dsd_opts* opts, dsd_state* state) {
    if (opts->floating_point == 0) {
        if (opts->pulse_digi_out_channels == 1) {
            playSynthesizedVoiceMS(opts, state);
        }
        if (opts->pulse_digi_out_channels == 2) {
            playSynthesizedVoiceSS(opts, state);
        }
    }

    if (opts->floating_point == 1) {
        if (opts->pulse_digi_out_channels == 1) {
            playSynthesizedVoiceFM(opts, state);
        }
        if (opts->pulse_digi_out_channels == 2) {
            playSynthesizedVoiceFS(opts, state);
        }
    }
}

static void
handle_soft_mbe_p25p1(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char imbe_d[88]) {
    soft_demod_imbe7200(state, imbe_fr, imbe_d);
    mbe_processImbe4400Dataf(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str, imbe_d,
                             state->cur_mp, state->prev_mp, state->prev_mp_enhanced, opts->uvquality);
    update_p25_p1_voice_err_hist(state);
}

static void
handle_soft_mbe_provoice(dsd_opts* opts, dsd_state* state, char imbe7100_fr[7][24], char imbe_d[88]) {
    soft_demod_imbe7100(state, imbe7100_fr, imbe_d);
    mbe_convertImbe7100to7200(imbe_d);
    mbe_processImbe4400Dataf(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str, imbe_d,
                             state->cur_mp, state->prev_mp, state->prev_mp_enhanced, opts->uvquality);
}

static void
handle_soft_mbe_dstar(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], char ambe_d[49]) {
    soft_demod_ambe_dstar(opts, state, ambe_fr, ambe_d);
    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, ambe_d);
    }

    play_synthesized_voice_by_output(opts, state);

    if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1) {
        writeSynthesizedVoice(opts, state);
    }

    if (opts->mbe_out_f != NULL) {
        saveAmbe2450Data(opts, state, ambe_d);
    }
}

static void
handle_soft_mbe_x2(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], char ambe_d[49]) {
    soft_demod_ambe_x2(opts, state, ambe_fr, ambe_d);
    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, ambe_d);
    }

    play_synthesized_voice_by_output(opts, state);

    if (opts->wav_out_f != NULL) {
        writeSynthesizedVoice(opts, state);
    }

    if (opts->mbe_out_f != NULL) {
        save_ambe2450_x2(opts, state, ambe_d);
    }
}

static void
handle_soft_mbe_ambe2_ehr(dsd_opts* opts, dsd_state* state, char ambe_fr[4][24], char ambe_d[49]) {
    soft_demod_ambe2_ehr(state, ambe_fr, ambe_d);
    if (dsd_frame_detail_enabled(opts)) {
        PrintAMBEData(opts, state, ambe_d);
    }

    if (state->currentslot == 0) {
        mbe_processAmbe2450Dataf(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str, ambe_d,
                                 state->cur_mp, state->prev_mp, state->prev_mp_enhanced, opts->uvquality);
    } else if (state->currentslot == 1) {
        mbe_processAmbe2450Dataf(state->audio_out_temp_bufR, &state->errsR, &state->errs2R, state->err_strR, ambe_d,
                                 state->cur_mp2, state->prev_mp2, state->prev_mp_enhanced2, opts->uvquality);
    }
}

void
soft_mbe(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    char ambe_d[49];
    char imbe_d[88];

    DSD_MEMSET(ambe_d, 0, sizeof(ambe_d));
    DSD_MEMSET(imbe_d, 0, sizeof(imbe_d));

    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        handle_soft_mbe_p25p1(opts, state, imbe_fr, imbe_d);
    } else if (DSD_SYNC_IS_PROVOICE(state->synctype)) {
        handle_soft_mbe_provoice(opts, state, imbe7100_fr, imbe_d);
    } else if (state->synctype == DSD_SYNC_DSTAR_VOICE_POS || state->synctype == DSD_SYNC_DSTAR_VOICE_NEG) {
        handle_soft_mbe_dstar(opts, state, ambe_fr, ambe_d);
    } else if (DSD_SYNC_IS_X2TDMA(state->synctype)) {
        handle_soft_mbe_x2(opts, state, ambe_fr, ambe_d);
    } else {
        handle_soft_mbe_ambe2_ehr(opts, state, ambe_fr, ambe_d);
    }
}
