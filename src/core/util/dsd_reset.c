// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) || defined(__clang__)
#define DSD_ATTR_UNUSED_FN __attribute__((unused))
#else
#define DSD_ATTR_UNUSED_FN
#endif

static void DSD_ATTR_UNUSED_FN resetState(dsd_state* state);
static void DSD_ATTR_UNUSED_FN reset_dibit_buffer(dsd_state* state);

static void
reset_primary_buffers(dsd_state* state) {
    //Dibit Buffer -- Free Allocated Memory
    // free (state->dibit_buf);

    //Dibit Buffer -- Memset/Init/Allocate Memory
    // state->dibit_buf = malloc (sizeof (int) * 1000000);

    state->dibit_buf_p = state->dibit_buf + 200;
    DSD_MEMSET(state->dibit_buf, 0, sizeof(int) * 200);
    state->repeat = 0; //repeat frame?

    //Audio Buffer -- Free Allocated Memory
    free(state->audio_out_buf);
    free(state->audio_out_float_buf);
    free(state->audio_out_bufR);
    free(state->audio_out_float_bufR);

    //slot 1
    state->audio_out_float_buf = malloc(sizeof(float) * 1000000);
    state->audio_out_buf = malloc(sizeof(short) * 1000000);

    DSD_MEMSET(state->audio_out_buf, 0, 100 * sizeof(short));
    state->audio_out_buf_p = state->audio_out_buf + 100;

    DSD_MEMSET(state->audio_out_float_buf, 0, 100 * sizeof(float));
    state->audio_out_float_buf_p = state->audio_out_float_buf + 100;

    state->audio_out_idx = 0;
    state->audio_out_idx2 = 0;
    state->audio_out_temp_buf_p = state->audio_out_temp_buf;

    //slot 2
    state->audio_out_bufR = malloc(sizeof(short) * 1000000);
    state->audio_out_float_bufR = malloc(sizeof(float) * 1000000);

    DSD_MEMSET(state->audio_out_bufR, 0, 100 * sizeof(short));
    state->audio_out_buf_pR = state->audio_out_bufR + 100;

    DSD_MEMSET(state->audio_out_float_bufR, 0, 100 * sizeof(float));
    state->audio_out_float_buf_pR = state->audio_out_float_bufR + 100;

    state->audio_out_idxR = 0;
    state->audio_out_idx2R = 0;
    state->audio_out_temp_buf_pR = state->audio_out_temp_bufR;
}

static void
reset_dmr_buffers(dsd_state* state) {
    // DMR reliability buffer (parallel to dmr_payload_buf)
    if (state->dmr_reliab_buf) {
        dsd_aligned_free(state->dmr_reliab_buf);
        state->dmr_reliab_buf = NULL;
    }
    state->dmr_reliab_buf = (uint8_t*)dsd_aligned_alloc(64, 1000000 * sizeof(uint8_t));
    if (state->dmr_reliab_buf) {
        DSD_MEMSET(state->dmr_reliab_buf, 0, 200 * sizeof(uint8_t));
        state->dmr_reliab_p = state->dmr_reliab_buf + 200;
    } else {
        state->dmr_reliab_p = NULL;
    }
    if (state->dmr_soft_buf) {
        dsd_aligned_free(state->dmr_soft_buf);
        state->dmr_soft_buf = NULL;
    }
    state->dmr_soft_buf = (dsd_dibit_soft_t*)dsd_aligned_alloc(64, 1000000 * sizeof(dsd_dibit_soft_t));
    if (state->dmr_soft_buf) {
        DSD_MEMSET(state->dmr_soft_buf, 0, 200 * sizeof(dsd_dibit_soft_t));
        state->dmr_soft_p = state->dmr_soft_buf + 200;
    } else {
        state->dmr_soft_p = NULL;
    }
}

static void
reset_dmr_sample_history(dsd_state* state) {
    // DMR sample history buffer reset (resample-on-sync support)
    // Note: Buffer allocation preserved; only reset indices.
    if (state->dmr_sample_history && state->dmr_sample_history_size > 0) {
        DSD_MEMSET(state->dmr_sample_history, 0, sizeof(float) * state->dmr_sample_history_size);
    }
    state->dmr_sample_history_head = 0;
    state->dmr_sample_history_count = 0;
}

static void
reset_sync_tracking_state(dsd_state* state) {
    int i;

    //Sync
    state->center = 0.0f;
    state->jitter = -1;
    state->synctype = DSD_SYNC_NONE;
    state->min = -4.0f;
    state->max = 4.0f;
    state->lmid = 0.0f;
    state->umid = 0.0f;
    state->minref = -3.2f;
    state->maxref = 3.2f;

    state->lastsample = 0.0f;
    for (i = 0; i < 128; i++) {
        state->sbuf[i] = 0.0f;
    }
    state->sidx = 0;
    for (i = 0; i < 1024; i++) {
        state->maxbuf[i] = 4.0f;
    }
    for (i = 0; i < 1024; i++) {
        state->minbuf[i] = -4.0f;
    }

    state->midx = 0;
    dsd_state_invalidate_minmax_sums(state);
    state->symbolcnt = 0;
    state->symbolc = 0;
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN;
    state->symbol_replay_header_checked = 0;
    state->symbol_replay_has_soft = 0;
    state->symbol_replay_soft.reliability = 0;
    state->symbol_replay_soft.llr[0] = 0;
    state->symbol_replay_soft.llr[1] = 0;
    state->symbol_replay_soft_symbol = 0.0f;

    /* Reset M17 polarity auto-detection: 0=unknown */
    state->m17_polarity = 0;
    state->m17_bert_locked = 0;
    state->m17_bert_lfsr = 1;
    state->m17_bert_lock_count = 0;
    state->m17_bert_window_bits = 0;
    state->m17_bert_window_errors = 0;
    state->m17_bert_bits = 0;
    state->m17_bert_errors = 0;
    state->m17_bert_resyncs = 0;

    /* Reset multi-rate SPS hunting state */
    state->sps_hunt_counter = 0;
    state->sps_hunt_idx = 0;

    state->lastsynctype = DSD_SYNC_NONE;
    state->lastp25type = 0;
    state->offset = 0;
    state->carrier = 0;
}

static void
reset_error_histograms(dsd_state* state) {
    //Reset Voice Errors in C0 and C1 (or remaining Codewords in IMBE)
    state->errs = 0;
    state->errs2 = 0;
    state->errsR = 0;
    state->errs2R = 0;

    // Reset debug accumulators so UI counters reflect current tune
    state->debug_audio_errors = 0;
    state->debug_audio_errorsR = 0;
    state->debug_header_errors = 0;
    state->debug_header_critical_errors = 0;

    // Initialize P25p1 voice avg error histogram
    DSD_MEMSET(state->p25_p1_voice_err_hist, 0, sizeof(state->p25_p1_voice_err_hist));
    state->p25_p1_voice_err_hist_len = 50; // default short window
    state->p25_p1_voice_err_hist_pos = 0;
    state->p25_p1_voice_err_hist_sum = 0;

    // Initialize P25p2 voice avg error histogram (per slot)
    DSD_MEMSET(state->p25_p2_voice_err_hist, 0, sizeof(state->p25_p2_voice_err_hist));
    state->p25_p2_voice_err_hist_len = 50;
    state->p25_p2_voice_err_hist_pos[0] = 0;
    state->p25_p2_voice_err_hist_pos[1] = 0;
    state->p25_p2_voice_err_hist_sum[0] = 0;
    state->p25_p2_voice_err_hist_sum[1] = 0;
    DSD_MEMSET(state->ess_b, 0, sizeof(state->ess_b));
    DSD_MEMSET(state->ess_b_llr, 0, sizeof(state->ess_b_llr));
    state->fourv_counter[0] = 0;
    state->fourv_counter[1] = 0;
}

static void
reset_misc_runtime_state(dsd_state* state) {
    //Misc -- may not be needed
    state->optind = 0;
    state->numtdulc = 0;
    state->firstframe = 0;

    //unsure if these are still used or ever were used,
    // DSD_MEMSET(state->aout_max_buf, 0, sizeof (float) * 200);
}

static void
reset_p25_ber_fec_counters(dsd_state* state) {
    /* Reset P25 BER/FEC counters so UI reflects fresh conditions after retune */
    state->p25_p1_fec_ok = 0;
    state->p25_p1_fec_err = 0;
    state->p25_p1_voice_fec_ok = 0;
    state->p25_p1_voice_fec_err = 0;
    state->p25_p1_duid_hdu = 0;
    state->p25_p1_duid_ldu1 = 0;
    state->p25_p1_duid_ldu2 = 0;
    state->p25_p1_duid_tdu = 0;
    state->p25_p1_duid_tdulc = 0;
    state->p25_p1_duid_tsbk = 0;
    state->p25_p1_duid_mpdu = 0;
    state->p25_p2_rs_facch_ok = 0;
    state->p25_p2_rs_facch_err = 0;
    state->p25_p2_rs_facch_corr = 0;
    state->p25_p2_rs_sacch_ok = 0;
    state->p25_p2_rs_sacch_err = 0;
    state->p25_p2_rs_sacch_corr = 0;
    state->p25_p2_rs_ess_ok = 0;
    state->p25_p2_rs_ess_err = 0;
    state->p25_p2_rs_ess_corr = 0;
    state->p25_p2_soft_erasure_ok = 0;
    state->p25_p1_soft_hamming_ok = 0;
    state->p25_p1_soft_golay_ok = 0;
    state->p25_p1_soft_rs_ok = 0;
    state->p25_p2_soft_ess_ok = 0;
    state->p25_p2_soft_ess_max_depth = 0;
    state->p25_p1_soft_combined_ok = 0;
}

static void
reset_p25_tables_and_hints(dsd_state* state) {
    // Reset P25 affiliation table
    state->p25_aff_count = 0;
    DSD_MEMSET(state->p25_aff_rid, 0, sizeof(state->p25_aff_rid));
    DSD_MEMSET(state->p25_aff_last_seen, 0, sizeof(state->p25_aff_last_seen));

    // Reset P25 CC/system TDMA hints
    state->p25_cc_is_tdma = 2;
    state->p25_sys_is_tdma = 0;
    state->p25_p2_active_slot = -1;
    state->p25_vc_cqpsk_pref = -1;
    state->p25_vc_cqpsk_override = -1;

    // Reset P25 Group Affiliation table
    state->p25_ga_count = 0;
    DSD_MEMSET(state->p25_ga_rid, 0, sizeof(state->p25_ga_rid));
    DSD_MEMSET(state->p25_ga_tg, 0, sizeof(state->p25_ga_tg));
    DSD_MEMSET(state->p25_ga_last_seen, 0, sizeof(state->p25_ga_last_seen));
}

//fixed the memory leak, but now random segfaults occur -- double free or corruption (out) or (!prev)
static void DSD_ATTR_UNUSED_FN
resetState(dsd_state* state) {
    reset_primary_buffers(state);
    reset_dmr_buffers(state);
    reset_dmr_sample_history(state);
    reset_sync_tracking_state(state);
    reset_error_histograms(state);
    reset_misc_runtime_state(state);
    reset_p25_ber_fec_counters(state);
    reset_p25_tables_and_hints(state);
}

//simple function to reset the dibit buffer
static void DSD_ATTR_UNUSED_FN
reset_dibit_buffer(dsd_state* state) {
    //Dibit Buffer -- Free Allocated Memory
    // free (state->dibit_buf);

    //Dibit Buffer -- Memset/Init/Allocate Memory
    // state->dibit_buf = malloc (sizeof (int) * 1000000);

    state->dibit_buf_p = state->dibit_buf + 200;
    DSD_MEMSET(state->dibit_buf, 0, sizeof(int) * 200);
    if (state->dmr_reliab_buf) {
        state->dmr_reliab_p = state->dmr_reliab_buf + 200;
        DSD_MEMSET(state->dmr_reliab_buf, 0, 200 * sizeof(uint8_t));
    }
    if (state->dmr_soft_buf) {
        state->dmr_soft_p = state->dmr_soft_buf + 200;
        DSD_MEMSET(state->dmr_soft_buf, 0, 200 * sizeof(dsd_dibit_soft_t));
    }
}
