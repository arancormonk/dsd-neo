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
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_cach.h"
#include "dmr_confidence.h"
#include "dmr_mono_slot.h"
#include "dmr_vcrypt.h"
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

/* The burst's CACH TACT, deinterleaved from its CACH dibits @p cach, for the slot this single-slot decoder follows
   (issue #637). */
static void
dmr_ms_note_rx_slot(dsd_state* state, const char cach[25]) {
    uint8_t bits[DMR_CACH_INTERLEAVE_BITS];
    for (int i = 0; i < 12; i++) {
        const int dibit = (unsigned char)cach[i];
        bits[dmr_cach_interleave[(size_t)i * 2]] = (uint8_t)(1 & (dibit >> 1));
        bits[dmr_cach_interleave[((size_t)i * 2) + 1]] = (uint8_t)(1 & dibit);
    }
    uint8_t tact[7];
    for (int i = 0; i < 7; i++) {
        tact[i] = bits[i];
    }
    const int ok = Hamming_7_4_decode(tact) ? 1 : 0;
    dmr_mono_slot_note(state, ok, tact[1]);
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

/* As in dmr_bs.c: an MS voice sync is the complement of an MS data sync, so a data burst at the other polarity reads
   as voice sync. A voice superframe never repeats its sync in the bursts after A: two in a row mean the polarity is the
   other one, which the decoder takes for the session before the hunt starts again (issue #729). */
static int
dmr_ms_sync_is_voice(const uint8_t syncdata[48]) {
    char sync[25];
    for (int i = 0; i < 24; i++) {
        sync[i] = (syncdata[(size_t)2 * i] != 0U) ? '3' : '1';
    }
    sync[24] = 0;
    return strcmp(sync, DMR_MS_VOICE_SYNC) == 0;
}

/* The burst's EMB (ETSI TS 102 361-1 clause 9.1.2) through the colour-code gate, as a BS voice burst's is
   (dmr_confidence.c): its power and pre-emption indicator in @p power (9 when the EMB did not decode). The EMB has no
   checksum, only QR(16,7,6) (Table B.1), whose decoder corrects two bit errors: about one random 16-bit word in four
   decodes, to any colour code. So one decode is not the carrier's colour code: only the gate's lock publishes it
   (dmr_color_code, the received access code), and a locked gate rejects another code unless it repeats. Taken from
   every decode, the noise after a carrier drop named the call's last bursts with a random colour code, which split the
   call and published their audio as a recording of no one (issue #834). REJECT for an EMB that did not decode. */
static dmr_confidence_result
dmr_ms_decode_embedded_color_code(dsd_state* state, const uint8_t syncdata[48], uint8_t emb_pdu[16], uint8_t* power) {
    *power = 9;
    for (int i = 0; i < 8; i++) {
        emb_pdu[i] = syncdata[i];
        emb_pdu[i + 8] = syncdata[i + 40];
    }
    if (!QR_16_7_6_decode(emb_pdu)) {
        return DMR_CONFIDENCE_REJECT;
    }
    const uint8_t cc = (uint8_t)((emb_pdu[0] << 3) + (emb_pdu[1] << 2) + (emb_pdu[2] << 1) + emb_pdu[3]);
    *power = emb_pdu[4];
    state->color_code = cc;
    return dmr_confidence_note_voice_burst(state, 0U, cc);
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
    processMbeFrame(opts, state, DSD_VOICE_PROTOCOL_DMR, NULL, ambe_fr, NULL);
    dsd_dmr_mix_store_frame(state, 0, (int)frame_idx);
}

/* Each frame decodes at its position in the superframe (burst @p vc), where its keystream lies (issue #729). */
static void
dmr_ms_process_audio_frames(dsd_opts* opts, dsd_state* state, dmr_ms_voice_frames* frames, uint8_t vc) {
    dmr_vcrypt_set_frame(state, 0U, vc, 0);
    dmr_ms_process_single_mbe(opts, state, frames->ambe_fr, 0);
    dmr_vcrypt_set_frame(state, 0U, vc, 1);
    dmr_ms_process_single_mbe(opts, state, frames->ambe_fr2, 1);
    dmr_vcrypt_set_frame(state, 0U, vc, 2);
    dmr_ms_process_single_mbe(opts, state, frames->ambe_fr3, 2);
    dmr_vcrypt_clear_frame(state, 0U);
}

/* The bootstrap burst's frames when its read-back is not all the stream's (dsd_state_sync_readback_invalid(): a cold
   start or an acquisition reset left no soft history to re-read what noCarrier() cleared): a frame with a dibit of the
   @p invalid oldest of the 90 read back (the CACH's 12, then the first frame's 36, then the second's first 18) is not
   decoded and plays silence; the rest decode at their positions as dmr_ms_process_audio_frames() decodes them. */
static void
dmr_ms_process_audio_frames_after(dsd_opts* opts, dsd_state* state, dmr_ms_voice_frames* frames, unsigned int invalid) {
    char (*const frame[3])[24] = {frames->ambe_fr, frames->ambe_fr2, frames->ambe_fr3};
    const unsigned int first_dibit[3] = {12U, 48U, 90U};
    for (uint8_t i = 0; i < 3U; i++) {
        if (invalid > first_dibit[i]) {
            DSD_MEMSET(state->s_l4[i], 0, sizeof(state->s_l4[i]));
            DSD_MEMSET(state->s_l4u[i], 0, sizeof(state->s_l4u[i]));
            state->s4_fresh[0][i] = 0U;
            continue;
        }
        dmr_vcrypt_set_frame(state, 0U, 1U, i);
        dmr_ms_process_single_mbe(opts, state, frame[i], i);
    }
    dmr_vcrypt_clear_frame(state, 0U);
}

static void
dmr_ms_play_voice(dsd_opts* opts, dsd_state* state) {
    if (opts->pulse_digi_out_channels != 1 && opts->pulse_digi_out_channels != 2) {
        return;
    }
    playSynthesizedVoiceSS3(opts, state);
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

/* What the voice cycle does with a burst read whole, by the voice gate (dmr_confidence.c) and its EMB's verdict @p emb
   (dmr_ms_decode_embedded_color_code()). */
typedef enum {
    DMR_MS_BURST_DECODE = 0, /* the gate is open: the burst is the call's */
    DMR_MS_BURST_HOLD,       /* the gate is closed and the EMB may still open it: nothing of the burst decodes */
    DMR_MS_BURST_END,        /* the gate is closed and the EMB gave it nothing: the cycle ends */
} dmr_ms_burst_verdict;

/* MS voice is held until the voice gate opens, as BS voice is: its colour code locked and then repeated (two EMBs that
   agree, or one after a data burst with the same code), so a sync that noise passed plays, records and publishes
   nothing (AGENTS.md "CRC/FEC provenance"; issue #834). Before it opens, a burst without a usable EMB ends the cycle,
   as a BS burst's does on a closed gate, and the hunt looks for the next sync. Once it is open, an EMB that does not
   decode or names another colour code is an EMB miss, as on BS (issue #729): one is far likelier a noisy burst or a
   miscorrection than another system, and its burst still decodes; a second in a row ends the cycle and closes the
   gate, so the noise after a carrier drop plays at most one burst into the call, and a sync noise passes after it
   finds the gate closed (TS 102 361-1 Annex G.1.1: an MS leaves My_System once the colour code can no longer be
   decoded). Only an EMB the gate accepted ends a miss streak; burst A, the superframe's sync, starts a new one. */
static dmr_ms_burst_verdict
dmr_ms_gate_burst(dsd_state* state, dmr_confidence_result emb) {
    uint8_t* miss = &state->dmr_emb_err[0];
    if (emb != DMR_CONFIDENCE_REJECT) {
        *miss = 0U;
    } else if (*miss < 0xFFU) {
        (*miss)++;
    }
    if (dmr_confidence_voice_slot_open(state, 0U) && *miss < 2U) {
        return DMR_MS_BURST_DECODE;
    }
    if (emb == DMR_CONFIDENCE_REJECT) {
        dmr_confidence_reset_slot(state, 0U);
        return DMR_MS_BURST_END;
    }
    return DMR_MS_BURST_HOLD;
}

/* A burst B to F of the call (the voice gate open): its frames, and at F the embedded LC and the reverse channel. */
static void
dmr_ms_decode_voice_burst(dsd_opts* opts, dsd_state* state, dmr_ms_voice_frames* frames, uint8_t vc, uint8_t power,
                          uint8_t dummy_bits[196]) {
    dmr_ms_copy_frames_for_late_entry(frames);
    dmr_ms_apply_keystream(state, frames);
    dmr_ms_process_audio_frames(opts, state, frames, vc);
    dmr_ms_play_voice(opts, state);

    if (vc == 6) {
        dmr_ms_handle_vc6_ops(opts, state, power, dummy_bits);
    }
    dmr_late_entry_mi_fragment(opts, state, vc, frames->m1, frames->m2, frames->m3);
}

static int
dmr_ms_advance_voice_cycle(dsd_opts* opts, dsd_state* state, uint8_t* vc) {
    dsd_mark_vc_sync(state);
    (*vc)++;
    if (*vc > 6) {
        return 0;
    }

    skipDibit(opts, state, 144); // skip to next TDMA channel
    if (dsd_telemetry_is_active()) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }

    dsd_event_sync_slot(opts, state, 0);
    dmr_sm_tick_ctx(dmr_sm_get_ctx(), opts, state); // handle hangtime/release logic
    return 1;
}

/* The other slot's time, then the next burst's CACH and first half, up to the sync the hunt reads next: the recording
   for it is armed a dibit ahead of it (dmr_ms_note_hunt_sync_anchor()). */
static void
dmr_ms_prime_next_payload(dsd_opts* opts, dsd_state* state) {
    skipDibit(opts, state, 144); // should we have two of these?
    for (int i = 0; i < 66; i++) {
        if (i == 65) {
            /* Every exit from dmrMS() lands here, a polarity flip or a cut burst's break too. Arming is harmless on
               all of them: the recording is named only by a burst the hunt accepts next as MS, and one that sees no
               such sync lapses after 64 symbols, taking any hold with it. */
            dsd_symbol_arm_hunt_sync_anchor(state);
        }
        state->dmr_stereo_payload[i] = dmr_ms_read_dibit(opts, state, 1);
    }
}

/* The sync the hunt just accepted places the symbol grid for the burst it opens and those behind it. The hunt
   re-acquires every voice superframe on its burst A (and every data burst), and its crossing loop left the grid a
   sample or two late on that sync, which bursts B to F, read on without a sync of their own, then kept: an inner
   symbol read as outer now and then on a clean signal (issue #834). The 24 dibits below dmr_payload_p are the sync,
   by sign at the polarity received, which is all the anchor reads of them. A recording armed ahead of the sync (the
   burst before it, dmr_ms_prime_next_payload() or dmr_ms_data_read_to_next_sync()) measures it; without one this
   names nothing. */
static void
dmr_ms_note_hunt_sync_anchor(const dsd_state* state) {
    if (state->dmr_payload_buf == NULL || state->dmr_payload_p == NULL
        || state->dmr_payload_p - state->dmr_payload_buf < 24) {
        return;
    }
    char sync[24];
    const int* dibit_p = state->dmr_payload_p - 24;
    for (int i = 0; i < 24; i++) {
        sync[i] = (char)((dibit_p[i] | 1) + '0');
    }
    dsd_symbol_note_sync_anchor(state, sync, 24);
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

/* Burst A's frames, read back with @p invalid dibits not the stream's (dmr_ms_process_audio_frames_after()). */
static void
dmr_ms_decode_bootstrap_frames(dsd_opts* opts, dsd_state* state, dmr_ms_voice_frames* frames, unsigned int invalid) {
    dmr_ms_copy_frames_for_late_entry(frames);
    dmr_ms_apply_keystream(state, frames);
    if (invalid == 0U) {
        dmr_ms_process_audio_frames(opts, state, frames, 1U);
    } else {
        dmr_ms_process_audio_frames_after(opts, state, frames, invalid);
    }
    dmr_ms_play_voice(opts, state);

    /* The late-entry fragments are the three frames': none when one was not read back. */
    if (invalid <= 12U) {
        dmr_late_entry_mi_fragment(opts, state, 1, frames->m1, frames->m2, frames->m3);
    }
}

void
dmrMS(dsd_opts* opts, dsd_state* state) {
    char timestr[9];
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);

    dmr_ms_voice_frames frames;
    char cachdata[25];
    uint8_t syncdata[48];
    uint8_t emb_pdu[16];
    uint8_t dummy_bits[196];
    uint8_t vc = 2;
    const uint8_t internalslot = 0;
    int voice_syncs_after_a = 0;

    DSD_MEMSET(syncdata, 0, sizeof(syncdata));
    DSD_MEMSET(emb_pdu, 0, sizeof(emb_pdu));
    DSD_MEMSET(dummy_bits, 0, sizeof(dummy_bits));

    state->currentslot = 0;

    /* The superframe's bursts belong to one carrier (issue #575): the sixth assembles the embedded LC from the four
       before it. When the carrier boundary moves the carrier count while the superframe is collected (a replay read
       adopting a recorded retune), inside a burst or in the other slot's burst skipped before one, the burst read
       across or after the move is dropped with the rest of the superframe, as on a sync loss; so is the burst an input
       interruption cut, and every burst after it, which read only erasures. */
    const uint32_t carrier_seq = state->carrier_seq;
    for (int j = 0; j < 6; j++) {
        if (dsd_state_frame_read_lost(state, carrier_seq)) {
            break;
        }
        state->dmrburstL = 16;
        dmr_sm_emit_voice_sync(opts, state, 0);

        dmr_ms_init_voice_frames(&frames);
        dmr_ms_read_cach(opts, state, cachdata);
        /* A CACH an interruption cut or a boundary moved is erasures, never slot evidence. */
        if (!dsd_state_frame_read_lost(state, carrier_seq)) {
            dmr_ms_note_rx_slot(state, cachdata);
        }

        dmr_ms_fill_ambe_from_stream(opts, state, frames.ambe_fr, 12, 36, 0);
        dmr_ms_fill_ambe_from_stream(opts, state, frames.ambe_fr2, 48, 18, 0);
        dmr_ms_collect_sync_bits(opts, state, syncdata, vc, internalslot);
        if (dsd_state_frame_read_lost(state, carrier_seq)) {
            break;
        }
        voice_syncs_after_a = dmr_ms_sync_is_voice(syncdata) ? voice_syncs_after_a + 1 : 0;
        if (voice_syncs_after_a >= 2) {
            opts->inverted_dmr = opts->inverted_dmr ? 0 : 1;
            DSD_FPRINTF(
                stderr,
                "%s %s DMR MS: voice sync after burst A is data at the other polarity; decoding %s polarity%s\n",
                timestr, KYEL, opts->inverted_dmr ? "inverted" : "normal", KNRM);
            break;
        }
        dmr_ms_fill_ambe_from_stream(opts, state, frames.ambe_fr2, 90, 18, 18);
        dmr_ms_fill_ambe_from_stream(opts, state, frames.ambe_fr3, 108, 36, 0);
        /* The EMB decodes only once the burst is read whole: one an input interruption cut is erasures, and all-zero
           bits are a valid QR(16,7,6) codeword, so it would publish colour code 0; one the carrier boundary split is half
           another carrier's, and its colour code would be noted as the new carrier's. */
        if (dsd_state_frame_read_lost(state, carrier_seq)) {
            break;
        }
        uint8_t power = 9;
        const dmr_confidence_result emb = dmr_ms_decode_embedded_color_code(state, syncdata, emb_pdu, &power);
        dmr_debug_dump_burst(opts, state, state->currentslot, 0x10);
        dmr_ms_dump_dsp_output(opts, state);

        state->dmr_ms_mode = 1;
        const dmr_ms_burst_verdict verdict = dmr_ms_gate_burst(state, emb);
        if (verdict == DMR_MS_BURST_END) {
            break;
        }
        if (verdict == DMR_MS_BURST_DECODE) {
            dmr_ms_decode_voice_burst(opts, state, &frames, vc, power, dummy_bits);
        }
        if (vc == 6) {
            dmr_vcrypt_superframe_end(state, 0U);
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
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);

    dmr_ms_voice_frames frames;
    char cachdata[25];

    dmr_ms_note_hunt_sync_anchor(state);
    dmr_ms_reset_ks_counter(state);
    dmr_ms_init_voice_frames(&frames);

    state->dmrburstL = 16;
    state->currentslot = 0;
    dmr_sm_emit_voice_sync(opts, state, 0);

    /* Read before any symbol is: how much of the read-back is not the stream's. */
    const unsigned int invalid = dsd_state_sync_readback_invalid(state);
    dmr_ms_prepare_bootstrap_payload(state);
    /* The voice sync the hunt accepted is burst A: the superframe starts here, and the MI catches up over any the
       hunt spent between this one and the last (issue #729). */
    dmr_vcrypt_superframe_start(state, 0U);
    dmr_ms_collect_bootstrap_cach(opts, state, cachdata);
    /* The bootstrap runs again at every superframe, and its buffered CACH is not the one that names the burst's slot:
       the slot is proven from the voice cycle's own bursts (dmrMS()), and forgotten at a carrier boundary. */
    dmr_mono_slot_begin(state);
    const uint32_t carrier_seq = state->carrier_seq;
    dmr_ms_decode_bootstrap_voice(opts, state, &frames);
    if (dsd_state_frame_read_lost(state, carrier_seq)) {
        /* The burst's live half came after the carrier boundary (issue #575), or an input interruption cut it: it is
           dropped, as on a sync loss, and the voice cycle does not start on it. */
        dmr_ms_clear_mode_flags(state);
        return;
    }
    if (invalid == 0U) {
        dmr_ms_dump_bootstrap_debug_burst(opts, state);
        dmr_ms_dump_dsp_output(opts, state);
    }
    dmr_ms_print_bootstrap_sync(opts, state, timestr);

    /* Burst A carries the sync, no EMB: it decodes only on a gate a call already opened (dmr_ms_gate_burst()). */
    dmr_confidence_note_voice_sync(state, 0U);
    state->dmr_emb_err[0] = 0U;
    if (dmr_confidence_voice_slot_open(state, 0U)) {
        dmr_ms_decode_bootstrap_frames(opts, state, &frames, invalid);
    }

    skipDibit(opts, state, 144); //skip to next TDMA slot
    dmrMS(opts, state);          //bootstrap into full TDMA frame
}

/* After a data burst: the other slot's time and the next burst's CACH and first half, up to its sync, with the
   recording for that sync armed a dibit ahead of it (dmr_ms_note_hunt_sync_anchor()). */
static void
dmr_ms_data_read_to_next_sync(dsd_opts* opts, dsd_state* state) {
    //should just be loaded in the dmr_payload_buffer instead now
    //but we want to read dibits so the buffer has actual good values in it
    for (int i = 0; i < 144; i++) { // 66
        (void)get_dibit_and_analog_signal(opts, state, NULL);
        state->dmr_stereo_payload[i] = 1; // set to one so first frame will fail intentionally instead of zero fill
    }
    //CACH + First Half Payload = 12 + 54
    for (int i = 0; i < 66; i++) { // 66
        if (i == 65) {
            dsd_symbol_arm_hunt_sync_anchor(state);
        }
        (void)get_dibit_and_analog_signal(opts, state, NULL);
        state->dmr_stereo_payload[i + 66] =
            1; ////set to one so first frame will fail intentionally instead of zero fill
    }
}

//simplied to a simple data collector, and then passed on to dmr_data_sync for the usual processing
void
dmrMSData(dsd_opts* opts, dsd_state* state) {
    /* Read before any symbol is: a read-back not all the stream's (dsd_state_sync_readback_invalid()) holds the burst's
       CACH and the first half of its payload and slot type, so nothing of the burst decodes. */
    const unsigned int invalid = dsd_state_sync_readback_invalid(state);
    dmr_ms_note_hunt_sync_anchor(state);

    char timestr[9];
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);

    int i;
    int dibit;
    const int* dibit_p;
    const dsd_dibit_soft_t* soft_p = NULL;
    const uint32_t carrier_seq = state->carrier_seq;

    if (state->dmr_soft_buf != NULL && state->dmr_soft_p != NULL && state->dmr_soft_p >= state->dmr_soft_buf + 90) {
        soft_p = state->dmr_soft_p - 90;
    }

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
        state->dmr_stereo_reliab[i] = soft_p != NULL ? soft_p[i].reliability : 200U;
    }

    for (i = 0; i < 54; i++) {
        dsd_dibit_soft_t soft;
        dibit = getDibitSoft(opts, state, &soft);
        if (opts->inverted_dmr == 1) {
            dibit = (dibit ^ 2) & 3;
        }
        state->dmr_stereo_payload[i + 90] = dibit;
        state->dmr_stereo_reliab[i + 90] = soft.reliability;
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

    /* Process the data, unless the carrier boundary moved the carrier count while the live half was read (issue #575):
       everything decodes only after that read, so a slot type read whole before the move would publish the colour code
       of the carrier left after the boundary forgot it. Nor when an input interruption cut the live half, which is
       erasures the reader stood in. The burst is dropped, as on a sync loss. */
    if (!dsd_state_frame_read_lost(state, carrier_seq) && invalid == 0U) {
        state->dmr_stereo = 1;
        state->dmr_ms_mode = 1;
        dmr_data_sync(opts, state);
    } else {
        DSD_FPRINTF(stderr, "\n");
    }

    state->dmr_stereo = 0;
    state->dmr_ms_mode = 0;
    state->directmode = 0; //flag off

    dmr_ms_data_read_to_next_sync(opts, state);

    /* stack buffer; no free */
}
