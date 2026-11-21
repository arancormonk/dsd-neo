// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include "m17_confirm.h"
#include "m17_state_ext.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
m17_confirm_reset_signature(dsd_state* state) {
    if (!state) {
        return;
    }
    DSD_MEMSET(state->m17_signature_digest, 0, sizeof(state->m17_signature_digest));
    DSD_MEMSET(state->m17_signature, 0, sizeof(state->m17_signature));
    state->m17_signature_received_mask = 0U;
    state->m17_signature_complete = 0U;
    state->m17_signature_bad_sequence = 0U;
    state->m17_signature_verification_status = 0U;
    state->m17_signature_voice_next = 0U;
    state->m17_signature_voice_gap = 0U;
    state->m17_tx_ended = 0U;
}

void
m17_confirm_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    /* The confirmation. */
    state->m17_confirmed = 0;
    state->m17_confirm_weak_streak = 0;
    state->m17_confirm_frame_evidence = 0;
    /* The decoded link setup: no stream type, so nothing plays, marks media or names a signature. */
    state->m17_lsf_checked = 0U;
    state->m17_lsf_stream_mode = 0U;
    state->m17_str_dt = (uint8_t)M17_STREAM_TYPE_UNKNOWN;
    state->m17_can = 0U;
    state->m17_enc = 0U;
    state->m17_enc_st = 0U;
    state->m17_payload_decrypted = 0U;
    DSD_MEMSET(state->m17_meta, 0, sizeof(state->m17_meta));
    state->m17_signature_advertised = 0U;
    /* The raw LSF bits and the LICH chunks held in them, with what they were heard under. */
    DSD_MEMSET(state->m17_lsf, 0, sizeof(state->m17_lsf));
    DSD_MEMSET(&state->m17_lich, 0, sizeof(state->m17_lich));
    /* The packet being assembled. */
    DSD_MEMSET(state->m17_pkt, 0, sizeof(state->m17_pkt));
    state->m17_pbc_ct = 0U;
    state->m17_pkt_gap = 0U;
    /* The BERT receiver. */
    state->m17_bert_locked = 0U;
    state->m17_bert_lfsr = 1U;
    state->m17_bert_lock_count = 0U;
    state->m17_bert_window_bits = 0U;
    state->m17_bert_window_errors = 0U;
    state->m17_bert_bits = 0U;
    state->m17_bert_errors = 0U;
    state->m17_bert_resyncs = 0U;
    /* The 1600 bit/s arbitrary data and the text META being assembled. */
    m17_state_ext* ext = m17_state_ext_find(state);
    if (ext) {
        DSD_MEMSET(&ext->adata, 0, sizeof(ext->adata));
        DSD_MEMSET(&ext->text_meta, 0, sizeof(ext->text_meta));
    }
    /* The network stream. */
    state->m17_ip_stream = DSD_M17_IP_STREAM_NONE;
    state->m17_ip_stream_sid = 0U;
    state->m17_ip_stream_last_fn = 0U;
    state->m17_ip_stream_src = 0U;
    state->m17_ip_stream_signed = 0U;
    /* The signature, and that the transmission ended. */
    m17_confirm_reset_signature(state);
}

void
m17_confirm_begin_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    state->m17_confirm_frame_evidence = 0;
}

void
m17_confirm_note_evidence(const dsd_opts* opts, dsd_state* state, m17_evidence evidence) {
    if (!state) {
        return;
    }
    if (evidence == M17_EVIDENCE_STRONG) {
        state->m17_confirm_frame_evidence = (uint8_t)M17_EVIDENCE_STRONG;
        state->m17_confirm_weak_streak = 0;
        state->m17_confirmed = 1;
        dsd_signal_search_note_evidence(state, "M17", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
        return;
    }
    if (evidence != M17_EVIDENCE_WEAK || state->m17_confirm_frame_evidence != 0) {
        /* Several clean LICH reports in one frame are one frame's worth of evidence. */
        return;
    }
    state->m17_confirm_frame_evidence = (uint8_t)M17_EVIDENCE_WEAK;
    if (state->m17_confirm_weak_streak < 0xFFU) {
        state->m17_confirm_weak_streak++;
    }
    if (state->m17_confirm_weak_streak >= M17_CONFIRM_WEAK_OBSERVES) {
        state->m17_confirmed = 1;
        dsd_signal_search_note_evidence(state, "M17", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
        return;
    }
    /* A weak frame whose streak the next frame must complete: the quiet check waits for that frame. */
    dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PARTIAL, M17_CONFIRM_PARTIAL_HOLD_MS);
}

void
m17_confirm_end_frame(dsd_state* state) {
    if (!state) {
        return;
    }
    if (state->m17_confirm_frame_evidence == 0) {
        state->m17_confirm_weak_streak = 0;
    }
}

void
m17_confirm_note_unchecked_frame(const dsd_opts* opts, dsd_state* state) {
    if (!state || state->m17_confirm_frame_evidence != 0) {
        return;
    }
    dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PARTIAL, M17_CONFIRM_PARTIAL_HOLD_MS);
}

int
m17_confirm_is_confirmed(const dsd_state* state) {
    return state && state->m17_confirmed != 0;
}
