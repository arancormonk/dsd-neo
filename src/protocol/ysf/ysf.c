// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * ysf.c
 * Yaesu Fusion Decoder (WIP)
 *
 * Bits of code and ideas from DSDcc, Osmocom OP25, gr-ysf, Munaut sprinkled in
 *
 * LWVMOBILE
 * 2023-07 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/tuner_owner.h>

#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/ysf/ysf.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <mbelib-neo/mbelib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "ysf_frame.h"
#include "ysf_internal.h"

/* Adds the callsigns a verified DCH named to the slot's active YSF call. An ended one is not observed again; but when
   they name another party than its own, the call mode it was in is forgotten (ysf_call_cm), so a terminator that
   carries them is not that call's own (issue #888). */
static void
ysf_enrich_identity(dsd_state* state, uint8_t cm, const char* source, const char* target, const char* uplink,
                    const char* downlink) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || !DSD_SYNC_IS_YSF(call.protocol)
        || call.phase == DSD_CALL_PHASE_IDLE) {
        return;
    }
    const dsd_call_observation observation = {
        .protocol = call.protocol,
        .slot = 0U,
        .kind = call.kind == DSD_CALL_KIND_DATA ? DSD_CALL_KIND_DATA
                : cm == 3U                      ? DSD_CALL_KIND_PRIVATE_VOICE
                : cm == 0U                      ? DSD_CALL_KIND_GROUP_VOICE
                                                : DSD_CALL_KIND_VOICE,
    };
    dsd_call_observation enriched = observation;
    DSD_SNPRINTF(enriched.source_text, sizeof(enriched.source_text), "%s", source ? source : "");
    DSD_SNPRINTF(enriched.target_text, sizeof(enriched.target_text), "%s", target ? target : "");
    DSD_SNPRINTF(enriched.route_text[0], sizeof(enriched.route_text[0]), "%s", uplink ? uplink : "");
    DSD_SNPRINTF(enriched.route_text[1], sizeof(enriched.route_text[1]), "%s", downlink ? downlink : "");
    if (call.phase != DSD_CALL_PHASE_ACTIVE) {
        if (dsd_call_snapshot_contradicted_by(&call, &enriched)) {
            state->ysf_call_cm = 9U;
        }
        return;
    }
    (void)dsd_call_state_observe(state, &enriched, DSD_CALL_BOUNDARY_CONTINUE);
}

static void
ysf_dch_decode_csd1(dsd_state* state, const char dch_bytes[20], uint8_t cm) {
    char target[11];
    char string2[11];

    DSD_MEMCPY(target, dch_bytes, 10);
    target[10] = '\0';

    if (cm != 1) {
        char string1[11];
        DSD_MEMCPY(string1, dch_bytes, 10);
        string1[10] = '\0';
        DSD_FPRINTF(stderr, "DST: ");
        DSD_FPRINTF(stderr, "%s ", string1);
    } else {
        char rem1[6];
        char rem2[6];
        DSD_MEMCPY(rem1, dch_bytes, 5);
        rem1[5] = '\0';
        DSD_FPRINTF(stderr, "DST RID: ");
        DSD_FPRINTF(stderr, "%s ", rem1);

        DSD_MEMCPY(rem2, dch_bytes + 5, 5);
        rem2[5] = '\0';
        DSD_FPRINTF(stderr, "SRC RID: ");
        DSD_FPRINTF(stderr, "%s ", rem2);
    }

    DSD_MEMCPY(string2, dch_bytes + 10, 10);
    string2[10] = '\0';
    DSD_FPRINTF(stderr, "SRC: ");
    DSD_FPRINTF(stderr, "%s ", string2);

    ysf_enrich_identity(state, cm, string2, target, NULL, NULL);
}

/* CSD2: the downlink repeater's callsign, then the uplink's (Yaesu "Amateur Radio Digital Standards" Table 4-2, as
   MMDVMHost and DSDcc read it). */
static void
ysf_dch_decode_csd2(dsd_state* state, const char dch_bytes[20]) {
    char downlink[11];
    char uplink[11];

    DSD_MEMCPY(downlink, dch_bytes, 10);
    downlink[10] = '\0';
    DSD_FPRINTF(stderr, "D/L: ");
    DSD_FPRINTF(stderr, "%s ", downlink);

    DSD_MEMCPY(uplink, dch_bytes + 10, 10);
    uplink[10] = '\0';
    DSD_FPRINTF(stderr, "U/L: ");
    DSD_FPRINTF(stderr, "%s ", uplink);

    ysf_enrich_identity(state, state->ysf_cm, NULL, NULL, uplink, downlink);
}

static void
ysf_dch_decode_text(dsd_state* state, const char dch_bytes[20], uint8_t fn, uint8_t ft) {
    if (fn == 0) {
        DSD_MEMSET(state->ysf_txt, 0, sizeof(state->ysf_txt));
    }

    if (fn < 20) {
        for (int i = 0; i < 20; i++) {
            char C = dch_bytes[i];
            state->ysf_txt[fn][i] = (C > 0x19 && C < 0x7F) ? C : 0x20;
        }
    }

    if (fn == ft && dsd_ysf_event_text_should_print(state)) {
        DSD_FPRINTF(stderr, " %s", state->event_history_s[0].Event_History_Items[0].text_message);
    }
}

void
ysf_dch_decode(dsd_state* state, uint8_t field, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
               const uint8_t input[160]) {
    //TODO: Per Call WAV files using these strings
    int i;
    char dch_bytes[20];
    DSD_MEMSET(dch_bytes, 0, sizeof(dch_bytes));

    UNUSED(bt);

    for (i = 0; i < 20; i++) {
        dch_bytes[i] = (char)convert_bits_into_output(&input[(size_t)i * 8u], 8);
    }

    switch (field) { // the caller names the field, so fn is left to sort the text messages found in here
        case 0: ysf_dch_decode_csd1(state, dch_bytes, cm); break;
        case 1: ysf_dch_decode_csd2(state, dch_bytes); break;
        case 2: ysf_dch_decode_text(state, dch_bytes, fn, ft); break;
        default: break;
    }
}

static void
ysf_copy_text_field(char* dst, const char* src, size_t len) {
    DSD_MEMCPY(dst, src, len);
    dst[len] = '\0';
}

static void
ysf_print_text_field(const char* label, const char* src, size_t len, const char* suffix) {
    char text[11];
    ysf_copy_text_field(text, src, len);
    DSD_FPRINTF(stderr, "%s", label);
    DSD_FPRINTF(stderr, "%s%s", text, suffix);
}

static void
ysf_dch_decode2_destination(dsd_state* state, const char dch_bytes[20], uint8_t cm) {
    char target[11];
    if (cm != 1) {
        ysf_print_text_field("DST: ", dch_bytes, 10, " ");
    } else {
        ysf_print_text_field("DST RID: ", dch_bytes, 5, " ");
        ysf_print_text_field("SRC RID: ", dch_bytes + 5, 5, " ");
    }
    ysf_copy_text_field(target, dch_bytes, 10);
    ysf_enrich_identity(state, cm, NULL, target, NULL, NULL);
}

static void
ysf_dch_decode2_remarks(const char* label1, char* dst1, const char* label2, char* dst2, const char dch_bytes[20]) {
    ysf_print_text_field(label1, dch_bytes, 5, " ");
    ysf_copy_text_field(dst1, dch_bytes, 5);
    ysf_print_text_field(label2, dch_bytes + 5, 5, " ");
    ysf_copy_text_field(dst2, dch_bytes + 5, 5);
}

void
ysf_dch_decode2(dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm, const uint8_t input[80]) {
    char dch_bytes[20];
    DSD_MEMSET(dch_bytes, 0, sizeof(dch_bytes));

    UNUSED3(bn, bt, ft);

    for (int i = 0; i < 10; i++) {
        dch_bytes[i] = (char)convert_bits_into_output(&input[(size_t)i * 8u], 8);
    }

    switch (fn) {
        case 0: ysf_dch_decode2_destination(state, dch_bytes, cm); break;
        case 1: {
            char source_text[11];
            ysf_print_text_field("SRC: ", dch_bytes, 10, "");
            ysf_copy_text_field(source_text, dch_bytes, 10);
            ysf_enrich_identity(state, cm, source_text, NULL, NULL, NULL);
            break;
        }
        /* The downlink repeater in frame 2, the uplink in frame 3 (Table 4-4). */
        case 2: {
            char downlink[11];
            ysf_print_text_field("D/L: ", dch_bytes, 10, "");
            ysf_copy_text_field(downlink, dch_bytes, 10);
            ysf_enrich_identity(state, cm, NULL, NULL, NULL, downlink);
            break;
        }
        case 3: {
            char uplink[11];
            ysf_print_text_field("U/L: ", dch_bytes, 10, "");
            ysf_copy_text_field(uplink, dch_bytes, 10);
            ysf_enrich_identity(state, cm, NULL, NULL, uplink, NULL);
            break;
        }
        case 4: ysf_dch_decode2_remarks("RM1: ", state->ysf_rm1, "RM2: ", state->ysf_rm2, dch_bytes); break;
        case 5: ysf_dch_decode2_remarks("RM3: ", state->ysf_rm3, "RM4: ", state->ysf_rm4, dch_bytes); break;
        default: break;
    }
}

uint16_t
ysf_crc16(const uint8_t bits[], int len) {
    uint32_t poly = (1 << 12) + (1 << 5) + (1 << 0);
    uint32_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t bit = bits[i] & 1;
        crc = ((crc << 1) | bit) & 0x1ffff;
        if (crc & 0x10000) {
            crc = (crc & 0xffff) ^ poly;
        }
    }
    crc = crc ^ 0xffff;
    return crc & 0xffff;
}

/* Content a YSF CRC passed, as a scan hears it (issue #834, #890 F3): PROVEN, except a test frame's (FI 3, Yaesu
   "Amateur Radio Digital Standards" Rev 1.02, FICH) on a row of the ysf class, a typed -Y row or a ysf-conventional
   trunk-scan target: for it a test transmission proves the YSF profile and no traffic, so it advances neither the -Y
   content nor a quiet check's activity. An untyped row hears it as channel activity, as before. @p fi: the frame's
   checked FI. */
static void
ysf_note_checked_content(const dsd_opts* opts, dsd_state* state, uint32_t fi) {
    if (fi == 3U && dsd_scan_mode_active(state) == DSD_SCAN_MODE_YSF) {
        return;
    }
    dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
}

//modified version of nxdn_deperm_facch1 -- this one for V/D Type 2 CC DCH (100 dibit version)
int
ysf_conv_dch2(const dsd_opts* opts, dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
              uint8_t input[100]) {

    int i, j, err;
    uint8_t trellis_buf[100];
    uint8_t m_data[100];
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    err = 0;

    //dibit de-interleave block length M = 9 dibits and depth N = 20
    uint8_t buf[100];
    DSD_MEMSET(buf, 0, sizeof(buf));
    for (i = 0; i < 20; i++) {
        for (j = 0; j < 5; j++) {
            buf[j + (i * 5)] = input[i + (j * 20)];
        }
    }

    uint32_t v_error = dsd_ysf_soft_viterbi_decode(buf, 100U, 13U, 8U, 96U, trellis_buf, m_data);

    uint16_t crc = ysf_crc16(trellis_buf, 96);
    if (crc != 0) {
        err = -2; // crc failure
    }

    dsd_ysf_dewhiten_bits(trellis_buf, 80U);

    //reload after de-whitening
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    for (i = 0; i < 12; i++) {
        m_data[i] = (uint8_t)convert_bits_into_output(&trellis_buf[(size_t)i * 8u], 8);
    }

    //decode the callsign, etc, found in the DCH when no errors
    if (err == 0) {
        ysf_dch_decode2(state, bn, bt, fn, ft, cm, trellis_buf);
        /* Reported once the callsign is on the call: a signal search reads it from there when it hears this. */
        dsd_signal_search_note_evidence(state, "YSF", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        ysf_note_checked_content(opts, state, state->ysf_fi);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "DCH2 (CRC ERR) ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " D2-Ve: %1.1f; ", (float)v_error / (float)0xFFFF);
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "DCH2: ");
        for (i = 0; i < 12; i++) {
            DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
        }
    }

    return err;
}

//modified version of nxdn_deperm_facch1 -- this one for Full Rate, Type 1 CC, Headers and Terminators DCH (180 dibit version)
int
ysf_conv_dch(const dsd_opts* opts, dsd_state* state, uint8_t field, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
             uint8_t input[180]) {
    int i, j, err;
    uint8_t trellis_buf[190];
    uint8_t m_data[100];
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    err = 0;

    //dibit de-interleave block length M = 9 dibits and depth N = 20
    uint8_t buf[180];
    DSD_MEMSET(buf, 0, sizeof(buf));
    for (i = 0; i < 20; i++) { //20*9 = 180
        for (j = 0; j < 9; j++) {
            buf[j + (i * 9)] = input[i + (j * 20)];
        }
    }

    uint32_t v_error = dsd_ysf_soft_viterbi_decode(buf, 180U, 23U, 8U, 176U, trellis_buf, m_data);

    uint16_t crc = ysf_crc16(trellis_buf, 176);
    if (crc != 0) {
        err = -2; // crc failure
    }

    dsd_ysf_dewhiten_bits(trellis_buf, 160U);

    //reload after de-whitening
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    for (i = 0; i < 22; i++) {
        m_data[i] = (uint8_t)convert_bits_into_output(&trellis_buf[(size_t)i * 8u], 8);
    }

    //decode the callsign, etc, found in the DCH when no errors
    if (err == 0) {
        ysf_dch_decode(state, field, bt, fn, ft, cm, trellis_buf);
        /* A header's or terminator's callsigns arrive here, after its FICH reported: tell a signal search again once
         * they are on the call, so the frame that carries them is enough for the search to name the stations. */
        dsd_signal_search_note_evidence(state, "YSF", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        ysf_note_checked_content(opts, state, state->ysf_fi);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "DCH (CRC ERR) ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " D1-Ve: %1.1f; ", (float)v_error / (float)0xFFFF);
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "DCH1: ");
        for (i = 0; i < 22; i++) {
            DSD_FPRINTF(stderr, "[%02X]", m_data[i]);
        }
    }

    return err;
}

//modified version of nxdn_deperm_facch1
int
ysf_conv_fich(const uint8_t input[100], uint8_t dest[32], uint32_t* v_error_out) {
    if (input == NULL || dest == NULL) {
        return -1;
    }

    int i, j, err;
    uint8_t trellis_buf[100];
    uint8_t m_data[100];
    DSD_MEMSET(trellis_buf, 0, sizeof(trellis_buf));
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    err = 0;

    //dibit de-interleave block length M = 5 dibits and depth N = 10
    uint8_t buf[100];
    DSD_MEMSET(buf, 0, sizeof(buf));
    for (i = 0; i < 20; i++) {
        for (j = 0; j < 5; j++) {
            buf[j + (i * 5)] = input[i + (j * 20)];
        }
    }

    uint32_t v_error = dsd_ysf_soft_viterbi_decode(buf, 100U, 13U, 8U, 96U, trellis_buf, m_data);
    if (v_error_out != NULL) {
        *v_error_out = v_error;
    }

    uint8_t fich_bits[12 * 4];
    uint8_t temp_b[24];
    bool g[4];

    // run golay 24_12 error correction
    for (i = 0; i < 4; i++) {
        DSD_MEMSET(temp_b, 0, sizeof(temp_b));
        g[i] = false;

        for (j = 0; j < 24; j++) {
            temp_b[j] = (char)trellis_buf[(i * 24) + j];
        }

        g[i] = Golay_24_12_decode(temp_b);
        if (!g[i]) {
            err = -1;
        }

        for (j = 0; j < 24; j++) {
            trellis_buf[(i * 24) + j] = (uint8_t)temp_b[j];
        }
    }

    //load corrected bits
    for (i = 0; i < 12; i++) {
        fich_bits[(12 * 0) + i] = trellis_buf[i + 0];
        fich_bits[(12 * 1) + i] = trellis_buf[i + 24];
        fich_bits[(12 * 2) + i] = trellis_buf[i + 48];
        fich_bits[(12 * 3) + i] = trellis_buf[i + 72];
    }

    uint16_t crc = ysf_crc16(fich_bits, 48);
    if (crc != 0) {
        err = -2; // crc failure
    }

    DSD_MEMCPY(dest, fich_bits, 32); //copy minus the crc16
    return err;
}

static void
ysf_ehr(dsd_opts* opts, dsd_state* state, uint8_t dbuf[180], int start, int stop) {
    int i;
    char ambe_fr[4][24];
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));

    int st = state->synctype;
    state->synctype = DSD_SYNC_NXDN_POS;
    /* The frames decode and play through NXDN's decoder under its synctype; the receive policy judges YSF's. */
    const dsd_receive_context receive = dsd_receive_context_begin(state, DSD_SYNC_YSF_POS);

    for (; start < stop; start++) {
        //debug
        // DSD_FPRINTF(stderr, " DBUF = ");

        for (i = 0; i < DSD_AMBE_2450_DIBITS; i++) {
            const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[i];

            //debug
            // DSD_FPRINTF(stderr, "%d", dbuf[(start*36)+i]);

            uint8_t b1 = dbuf[(start * 36) + i] >> 1;
            uint8_t b2 = dbuf[(start * 36) + i] & 1;

            //should all be loaded back to back
            ambe_fr[map->high_row][map->high_col] = (char)b1;
            ambe_fr[map->low_row][map->low_col] = (char)b2;
        }

        processMbeFrame(opts, state, DSD_VOICE_PROTOCOL_YSF, NULL, ambe_fr, NULL);

        // The vocoder already wrote this frame's per-call WAV block behind its record gate.
        dsd_play_synthesized_voice(opts, state);
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    dsd_receive_context_end(state, receive);
    state->synctype = st;
}

typedef struct {
    uint8_t fi;
    uint8_t cm;
    uint8_t bn;
    uint8_t bt;
    uint8_t fn;
    uint8_t ft;
    uint8_t mr;
    uint8_t vp;
    uint8_t dt;
    uint8_t st;
    uint8_t sc;
    int err;
    uint32_t v_error;
    uint8_t fich_decode[32];
    /* The carrier count (state->carrier_seq) the frame began on (issue #575). */
    uint32_t carrier_seq;
} ysf_fich_info;

/* Whether the carrier boundary moved the carrier count since the frame began (issue #575): a replay read adopting a
   recorded retune. A payload decodes only once read, so a data channel or voice channel read whole before the move
   would decode after the boundary ended the carrier left's call: from there nothing of the frame decodes or publishes,
   as on a sync loss. Nor once an input interruption cut the frame, whose rest is erasures the reader stood in. */
static bool
ysf_carrier_left(const dsd_state* state, const ysf_fich_info* info) {
    return dsd_state_frame_read_lost(state, info->carrier_seq) != 0;
}

// V/D2 decodes straight through mbelib, so its frames never reach the vocoder's
// per-call WAV write (-P); they are written here under the same record policy, on
// slot 0 where YSF publishes its call. Frames that went through processMbeFrame()
// were already written there.
static void
ysf_write_bypassed_call_wav(const dsd_opts* opts, dsd_state* state) {
    int allow = 0;
    if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1
        && dsd_audio_record_policy_gate_slot(opts, state, 0, &allow) == 0 && allow) {
        writeSynthesizedVoice(opts, state);
    }
}

// V/D2's MBE capture (-d), for the same reason and under the same record policy as its per-call WAV: the frame never
// reaches the vocoder's capture stage (issue #661).
static void
ysf_capture_bypassed_frame(dsd_opts* opts, dsd_state* state, const char ambe_d[49]) {
    int allow = 0;
    if (dsd_mbe_out_enabled(opts, 0) && dsd_audio_record_policy_gate_slot(opts, state, 0, &allow) == 0 && allow) {
        dsd_mbe_out_write(opts, state, 0, DSD_MBE_OUT_FORMAT_AMBE, ambe_d);
    }
}

// A frame that bypassed the vocoder (V/D2) runs the gain stage here, in either output format (issue #652), as @p media
// (YSF voice is never encrypted); one that went through it was already gained there.
static void
ysf_emit_audio_from_temp(dsd_opts* opts, dsd_state* state, bool bypassed_vocoder, dsd_voice_media media) {
    if (bypassed_vocoder) {
        const dsd_voice_frame_info info = {(uint8_t)media, 1U,
                                           (uint8_t)(dsd_receive_current_allows(opts, state, 0) != 0), 0U};
        dsd_voice_chain_run(opts, state, 0, state->audio_out_temp_buf, &info);
        dsd_voice_stage_short_left(opts, state);
        ysf_write_bypassed_call_wav(opts, state);
    }
    dsd_play_synthesized_voice(opts, state);
}

/* A checked FICH's call to trunk scan's policy table (issue #890): header (FI 0) and communication (FI 1) frames
   are a call, a terminator (FI 2) and a test (FI 3) frame none. YSF names its stations by callsign, so the identity
   is its kind alone: DT 1 is data (V/D type 1 and 2 and full-rate voice are voice), and the call mode (Yaesu "Amateur
   Radio Digital Standards" Rev 1.02, section 4 (4), Table 4-1, CM) says 00 Group/CQ mode (a group call) and 11
   Individual mode (a private one); 01 Radio ID mode, which addresses a destination radio ID (Table 4-2, CSD1 with
   CM=1) and is neither, and 10 (reserve) leave the party unknown. YSF carries no encryption. No DG-ID is read (a
   decided limit). */
static void
ysf_report_scan_activity(const dsd_opts* opts, const dsd_state* state, const ysf_fich_info* info) {
    if (info->fi > 1U) {
        return;
    }
    dsd_trunk_scan_activity_identity identity;
    DSD_MEMSET(&identity, 0, sizeof(identity));
    identity.known = DSD_SCAN_ACTIVITY_KNOWN_DATA | DSD_SCAN_ACTIVITY_KNOWN_ENCRYPTED;
    identity.data_call = (uint8_t)(info->dt == 1U);
    if (info->cm == 0U || info->cm == 3U) {
        identity.is_private = (uint8_t)(info->cm == 3U);
        identity.known |= DSD_SCAN_ACTIVITY_KNOWN_PARTY;
    }
    const int protocol = DSD_SYNC_IS_YSF(state->synctype) ? state->synctype : DSD_SYNC_YSF_POS;
    dsd_trunk_scan_hook_digital_conventional_activity(opts, state, protocol, -1, &identity);
}

static void
ysf_parse_fich(dsd_opts* opts, dsd_state* state, ysf_fich_info* info) {
    uint8_t fichrawdibits[100];
    DSD_MEMSET(fichrawdibits, 0, sizeof(fichrawdibits));
    DSD_MEMSET(info->fich_decode, 0, sizeof(info->fich_decode));

    info->fi = 9;
    info->cm = 9;
    info->bn = 9;
    info->bt = 9;
    info->fn = 9;
    info->ft = 9;
    info->mr = 9;
    info->vp = 9;
    info->dt = 9;
    info->st = 9;
    info->sc = 0;
    info->v_error = UINT32_MAX;

    for (int i = 0; i < 100; i++) {
        fichrawdibits[i] = get_dibit_and_analog_signal(opts, state, NULL);
    }
    /* A carrier boundary split the FICH (issue #575), or an input interruption cut it (a Signal search left the
       frame): the rest of it belongs to another carrier or is erasures the reader stood in, so it decodes nothing, not
       even the transmission's evidence, and processYSF() drops the frame. */
    if (dsd_state_frame_read_lost(state, info->carrier_seq)) {
        info->err = -1;
        return;
    }

    info->err = ysf_conv_fich(fichrawdibits, info->fich_decode, &info->v_error);
    if (info->err == 0) {
        dsd_signal_search_note_evidence(state, "YSF", 0, 0, NULL);
        dsd_signal_search_note_call(state, 0U);
        info->fi = (uint8_t)convert_bits_into_output(&info->fich_decode[0], 2);
        info->cm = (uint8_t)convert_bits_into_output(&info->fich_decode[4], 2);
        info->bn = (uint8_t)convert_bits_into_output(&info->fich_decode[6], 2);
        info->bt = (uint8_t)convert_bits_into_output(&info->fich_decode[8], 2);
        info->fn = (uint8_t)convert_bits_into_output(&info->fich_decode[10], 3);
        info->ft = (uint8_t)convert_bits_into_output(&info->fich_decode[13], 3);
        info->mr = (uint8_t)convert_bits_into_output(&info->fich_decode[18], 3);
        info->vp = info->fich_decode[21];
        info->dt = (uint8_t)convert_bits_into_output(&info->fich_decode[22], 2);
        info->st = info->fich_decode[24];
        info->sc = (uint8_t)convert_bits_into_output(&info->fich_decode[25], 7);

        state->ysf_dt = info->dt;
        state->ysf_fi = info->fi;
        state->ysf_cm = info->cm;
        /* FICH Golay(24,12) and CRC-16 passed: activity to a scan quiet check (issue #834), and a call to trunk scan. */
        ysf_note_checked_content(opts, state, info->fi);
        ysf_report_scan_activity(opts, state, info);
        return;
    }

    /* A failed FICH lays the frame out as the last one that verified, which is this transmission's only once one of its
       own FICHs verified (ysf_fich_confirmed, cleared with the carrier): before that it is an earlier transmission's
       layout, or none, and a sync word matched on noise after a V/D call played that call's layout as voice and opened
       a call. Such a frame lays nothing out, and the hunt reads on through it. The layout is the decoder state's own
       (state->ysf_dt/ysf_fi, written above and reset with the flag), so each carrier falls back to its own. */
    if (state->ysf_fich_confirmed == 0) {
        return;
    }
    info->dt = state->ysf_dt;
    info->fi = state->ysf_fi;
}

static void
ysf_print_fich_type(const ysf_fich_info* info) {
    if (info->dt == 0 && info->err == 0) {
        DSD_FPRINTF(stderr, "V/D1 ");
    }
    if (info->dt == 1 && info->err == 0) {
        DSD_FPRINTF(stderr, "DATA ");
    }
    if (info->dt == 2 && info->err == 0) {
        DSD_FPRINTF(stderr, "V/D2 ");
    }
    if (info->dt == 3 && info->err == 0) {
        DSD_FPRINTF(stderr, "VWFR ");
    }
}

static void
ysf_print_fich_call_mode(const ysf_fich_info* info) {
    if (info->cm == 0) {
        DSD_FPRINTF(stderr, "Group/CQ ");
    }
    if (info->cm == 3) {
        DSD_FPRINTF(stderr, "Private  ");
    }
    if (info->cm == 1) {
        DSD_FPRINTF(stderr, "RID Mode ");
    }
    if (info->cm == 2) {
        DSD_FPRINTF(stderr, "Res: 2   ");
    }
}

static void
ysf_print_fich_path_and_frame(const ysf_fich_info* info) {
    if (info->vp == 0) {
        DSD_FPRINTF(stderr, "-Simplex ");
    }
    if (info->vp == 1) {
        DSD_FPRINTF(stderr, "Repeater ");
    }

    if (info->mr > 2 && info->mr < 7) {
        DSD_FPRINTF(stderr, "Res: %03d ", info->mr);
    }

    if (info->fi == 0 && info->err == 0) {
        DSD_FPRINTF(stderr, "HC ");
    }
    if (info->fi == 1 && info->err == 0) {
        DSD_FPRINTF(stderr, "CC ");
    }
    if (info->fi == 2 && info->err == 0) {
        DSD_FPRINTF(stderr, "TC ");
    }
    if (info->fi == 3 && info->err == 0) {
        DSD_FPRINTF(stderr, "XX ");
    }
}

static void
ysf_print_fich_sql(const ysf_fich_info* info) {
    char text[32];
    if (dsd_ysf_format_fich_sql(info->err, info->st, info->sc, text, sizeof text) > 0U) {
        DSD_FPRINTF(stderr, "%s", text);
    }
}

static void
ysf_print_fich_errors(const ysf_fich_info* info) {
    if (info->err != 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "FICH ");
        if (info->err == -1) {
            DSD_FPRINTF(stderr, "(FEC ERR) ");
        }
        if (info->err == -2) {
            DSD_FPRINTF(stderr, "(CRC ERR) ");
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static void
ysf_print_fich_payload(const dsd_opts* opts, const ysf_fich_info* info) {
    if (opts->payload == 1) {
        if (info->v_error != UINT32_MAX) {
            DSD_FPRINTF(stderr, " F-Ve: %1.1f; ", (float)info->v_error / (float)0xFFFF);
        }
        DSD_FPRINTF(stderr, " FICH: ");
        for (int i = 0; i < 4; i++) {
            DSD_FPRINTF(stderr, "[%02X]", (uint8_t)convert_bits_into_output(&info->fich_decode[(size_t)i * 8], 8));
        }
    }
}

static void
ysf_print_fich_summary(const dsd_opts* opts, const ysf_fich_info* info) {
    ysf_print_fich_type(info);
    ysf_print_fich_call_mode(info);
    ysf_print_fich_path_and_frame(info);
    ysf_print_fich_sql(info);
    DSD_FPRINTF(stderr, "FN: %d/%d ", info->fn + 1, info->ft + 1);
    ysf_print_fich_errors(info);
    ysf_print_fich_payload(opts, info);
}

/* A V/D type 1 frame carries five DCH/VCH pairs, one 20 ms AMBE frame per VCH: 100 ms of voice (issue #660). */
enum { YSF_VD1_CHANNEL_PAIRS = 5 };

/* The DCH field of a V/D type 1 frame, as ysf_dch_decode() selects one: named by the frame's number, not its block's.
   Frame 0 carries CSD1 (destination and source), frame 1 CSD2 (the downlink and uplink repeaters); the later frames
   carry no callsign this decoder reads (3: none). Read by the block number, 0 in a voice call, every frame was CSD1,
   so a repeater's callsigns became the call's endpoints and began another call part-way (issue #888). */
static uint8_t
ysf_vd_type1_dch_field(uint8_t fn) {
    return fn <= 1U ? fn : 3U;
}

static void
ysf_handle_vd_type1(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    uint8_t dbuf[190];
    uint8_t vbuf[190];
    DSD_MEMSET(dbuf, 0, sizeof(dbuf));
    DSD_MEMSET(vbuf, 0, sizeof(vbuf));

    for (int i = 0; i < YSF_VD1_CHANNEL_PAIRS; i++) {
        for (int j = 0; j < 36; j++) {
            dbuf[(i * 36) + j] = get_dibit_and_analog_signal(opts, state, NULL);
        }
        for (int j = 0; j < 36; j++) {
            vbuf[(i * 36) + j] = get_dibit_and_analog_signal(opts, state, NULL);
        }
    }
    if (ysf_carrier_left(state, info)) {
        return;
    }

    ysf_ehr(opts, state, vbuf, 0, YSF_VD1_CHANNEL_PAIRS);
    ysf_conv_dch(opts, state, ysf_vd_type1_dch_field(info->fn), info->bt, info->fn, info->ft, info->cm, dbuf);
}

static void
ysf_read_type2_vech_bits(dsd_opts* opts, dsd_state* state, uint8_t vech_bits[104]) {
    int k = 0;
    for (int j = 0; j < 52; j++) {
        int dibit = get_dibit_and_analog_signal(opts, state, NULL);
        uint8_t b1 = (uint8_t)((dibit >> 1) & 1);
        uint8_t b2 = (uint8_t)(dibit & 1);
        uint8_t msb = dsd_ysf_vd2_interleave_index((size_t)k++);
        uint8_t lsb = dsd_ysf_vd2_interleave_index((size_t)k++);

        vech_bits[msb] = b1 ^ dsd_ysf_pn95_bit(msb);
        vech_bits[lsb] = b2 ^ dsd_ysf_pn95_bit(lsb);
    }
}

void
ysf_build_type2_ambe(const uint8_t vech_bits[104], uint8_t temp[512], char ambe_d[49]) {
    static const uint8_t majority[8] = {0, 0, 0, 1, 0, 1, 1, 1};
    int l = 0;

    DSD_MEMSET(temp, 0, 512);
    for (int j = 0; j < 81; j++) {
        if (j % 3 == 2) {
            temp[l] = majority[(vech_bits[j - 2] << 2) | (vech_bits[j - 1] << 1) | vech_bits[j]];
            l++;
        }
    }

    for (int j = 0; j < 22; j++) {
        temp[j + 27] = vech_bits[j + 81];
    }

    for (int j = 0; j < 49; j++) {
        ambe_d[j] = (char)temp[j];
    }
}

static void
ysf_handle_vd_type2(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    uint8_t dbuf[190];
    uint8_t vech_bits[104];
    uint8_t temp[512];
    char ambe_d[49];
    int d = 0;

    DSD_MEMSET(dbuf, 0, sizeof(dbuf));
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 20; j++) {
            dbuf[d++] = get_dibit_and_analog_signal(opts, state, NULL);
        }

        DSD_MEMSET(vech_bits, 0, sizeof(vech_bits));
        state->errs = 0;
        state->errs2 = 0;

        ysf_read_type2_vech_bits(opts, state, vech_bits);
        /* A voice block read across the carrier boundary is dropped with the rest of the frame, as V/D1 drops it. */
        if (ysf_carrier_left(state, info)) {
            return;
        }
        ysf_build_type2_ambe(vech_bits, temp, ambe_d);

        state->errs2 = vech_bits[103];
        state->debug_audio_errors += state->errs2;

        (void)dsd_call_state_update_media(state, 0U, 1, 0.0);
        mbe_process_result result;
        mbe_initProcessResult(&result);
        result.total_errors = state->errs2;
        result.protected_errors = result.total_errors;
        int ret = mbe_processAmbe2450Dataf(state->audio_out_temp_buf, &result, ambe_d, state->cur_mp, state->prev_mp,
                                           state->prev_mp_enhanced);
        if (ret < 0) {
            mbe_synthesizeSilencef(state->audio_out_temp_buf);
            state->errs = 0;
            state->errs2 = 0;
            state->err_str[0] = '\0';
        } else {
            dsd_mbe_float_to_pcm16_scale(state->audio_out_temp_buf);
            state->errs = ((result.flags & MBE_PROCESS_FLAG_C0_VALID) != 0u) ? result.c0_errors : result.total_errors;
            state->errs2 = result.total_errors;
            mbe_formatProcessResult(state->err_str, sizeof(state->err_str), &result);
        }
        /* What mbelib-neo made of the block, not the mark before it, is the scan's voice credit (issue #890), for
           traffic the receive policy admits. */
        if (dsd_voice_media_of_mbe(ret, result.flags) == DSD_VOICE_MEDIA_SPEECH
            && dsd_scan_uses_checked_media(opts, state) && dsd_receive_current_allows(opts, state, 0)) {
            (void)dsd_call_state_note_checked_media(state, 0U, DSD_SYNC_YSF_POS, 0.0);
        }

        if (dsd_frame_detail_enabled(opts)) {
            PrintAMBEData(opts, state, ambe_d);
        }

        ysf_capture_bypassed_frame(opts, state, ambe_d);
        ysf_emit_audio_from_temp(opts, state, true, dsd_voice_media_of_mbe(ret, result.flags));
    }
    if (ysf_carrier_left(state, info)) {
        return;
    }

    ysf_conv_dch2(opts, state, info->bn, info->bt, info->fn, info->ft, info->cm, dbuf);
}

static bool
ysf_is_full_rate_csd3(const ysf_fich_info* info) {
    return info->ft == 1 && info->fn == 0;
}

static void
ysf_collect_full_rate_csd3_dch(dsd_opts* opts, dsd_state* state, uint8_t dbuf[190]) {
    for (int bank = 0; bank < 6; bank++) {
        for (int j = 0; j < 36; j++) {
            if (bank != 5) {
                dbuf[(bank * 36) + j] = get_dibit_and_analog_signal(opts, state, NULL);
            } else {
                skipDibit(opts, state, 1);
            }
        }
    }
}

static void
ysf_read_full_rate_imbe_raw(dsd_opts* opts, dsd_state* state, uint8_t imbe_raw[144]) {
    for (int j = 0; j < 72; j++) {
        int dibit = get_dibit_and_analog_signal(opts, state, NULL);
        imbe_raw[(j * 2) + 0] = (uint8_t)((dibit >> 1) & 1);
        imbe_raw[(j * 2) + 1] = (uint8_t)(dibit & 1);
    }
}

static bool
ysf_decode_full_rate_voice_slot(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    uint8_t imbe_raw[144];
    uint8_t imbe_vch[144];
    char imbe_fr[8][23];
    int synctype = state->synctype;

    DSD_MEMSET(imbe_raw, 0, sizeof(imbe_raw));
    DSD_MEMSET(imbe_vch, 0, sizeof(imbe_vch));
    DSD_MEMSET(imbe_fr, 0, sizeof(imbe_fr));

    ysf_read_full_rate_imbe_raw(opts, state, imbe_raw);
    /* A voice slot read across the carrier boundary is dropped with the rest of the frame, as V/D1 drops it. */
    if (ysf_carrier_left(state, info)) {
        return false;
    }
    dsd_ysf_unpack_full_rate_imbe(imbe_raw, imbe_vch, imbe_fr);

    state->synctype = DSD_SYNC_P25P1_POS;
    processMbeFrame(opts, state, DSD_VOICE_PROTOCOL_YSF, imbe_fr, NULL, NULL);
    state->synctype = synctype;

    // The media is unused here: the vocoder told the chain about this frame.
    ysf_emit_audio_from_temp(opts, state, false, DSD_VOICE_MEDIA_NONE);
    return true;
}

static void
ysf_handle_full_rate_voice(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    uint8_t dbuf[190];
    bool is_csd3 = ysf_is_full_rate_csd3(info);
    int voice_slots = is_csd3 ? 2 : 5;

    DSD_MEMSET(dbuf, 0, sizeof(dbuf));
    if (is_csd3) {
        ysf_collect_full_rate_csd3_dch(opts, state, dbuf);
    }

    for (int i = 0; i < voice_slots; i++) {
        if (!ysf_decode_full_rate_voice_slot(opts, state, info)) {
            return;
        }
    }

    if (is_csd3 && !ysf_carrier_left(state, info)) {
        ysf_conv_dch(opts, state, 2, info->bt, info->fn, info->ft, info->cm, dbuf);
    }
}

static void
ysf_handle_full_rate_data(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    uint8_t dbuf_fr[2][180];
    DSD_MEMSET(dbuf_fr, 0, sizeof(dbuf_fr));

    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 36; j++) {
            dbuf_fr[i % 2][((i / 2) * 36) + j] = get_dibit_and_analog_signal(opts, state, NULL);
        }
    }
    if (ysf_carrier_left(state, info)) {
        return;
    }

    for (int i = 0; i < 2; i++) {
        if (info->fi == 0 || info->fi == 2) {
            ysf_conv_dch(opts, state, (uint8_t)i, info->bt, info->fn, info->ft, info->cm, dbuf_fr[i]);
            continue;
        }

        ysf_conv_dch(opts, state, 2, info->bt, (uint8_t)(info->fn * 2 + i), (uint8_t)(info->ft * 2), info->cm,
                     dbuf_fr[i]);
    }
}

static dsd_call_kind
ysf_call_kind(uint8_t call_mode) {
    if (call_mode == 3U) {
        return DSD_CALL_KIND_PRIVATE_VOICE;
    }
    if (call_mode == 0U) {
        return DSD_CALL_KIND_GROUP_VOICE;
    }
    return DSD_CALL_KIND_VOICE;
}

static bool
ysf_is_fallback_voice_frame(const ysf_fich_info* info) {
    return info->err != 0 && info->fi == 1U && (info->dt == 0U || info->dt == 2U || info->dt == 3U);
}

/* The kind of call the frame's FICH names: data by its data type, else by its call mode. */
static dsd_call_kind
ysf_frame_call_kind(const ysf_fich_info* info) {
    return info->dt == 1U ? DSD_CALL_KIND_DATA : ysf_call_kind(info->cm);
}

static void
ysf_update_call_lifecycle(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    if (info->err == 0 && (info->fi == 0U || info->fi == 1U)) {
        state->ysf_call_cm = info->cm;
    }
    if ((info->err == 0 && (info->fi == 0U || info->fi == 1U)) || ysf_is_fallback_voice_frame(info)) {
        const int protocol = DSD_SYNC_IS_YSF(state->synctype) ? state->synctype : DSD_SYNC_YSF_POS;
        const dsd_call_observation observation = {
            .protocol = protocol,
            .slot = 0U,
            .kind = ysf_frame_call_kind(info),
        };
        const dsd_call_boundary boundary = info->fi == 0U ? DSD_CALL_BOUNDARY_BEGIN : DSD_CALL_BOUNDARY_CONTINUE;
        if (dsd_call_state_observe(state, &observation, boundary) > 0) {
            dsd_event_sync_slot(opts, state, 0U);
        }
    }
}

static void
ysf_end_call_lifecycle(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    // A FICH-verified communication terminator (FI=2) is positive over-the-air end evidence, so
    // the event layer can keep an audible epoch whose callsigns never decoded; EXPLICIT would be
    // indistinguishable from an engine retune and drop that row. It is the YSF call's own terminator, the one end
    // that lets its row take the rdio fallback talkgroup (issue #888), only when its verified call mode is the call's
    // (ysf_call_cm): a group call's end on a private or Radio ID call's terminator, or on one whose callsigns named
    // another caller of a faded call, is a terminator end, but not the call's own.
    if (info->err != 0 || info->fi != 2U || ysf_carrier_left(state, info)) {
        return;
    }
    const int ended =
        info->cm == state->ysf_call_cm
            ? dsd_call_state_end_own_terminator(state, 0U, 0.0, DSD_SYNC_YSF_POS, ysf_frame_call_kind(info))
            : dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_TERMINATOR);
    if (ended > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

/* Returns whether a handler read the payload. */
static bool
ysf_dispatch_payload(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    bool read = false;
    if (info->fi == 1U && info->dt == 0U) {
        ysf_handle_vd_type1(opts, state, info);
        read = true;
    }
    if (info->fi == 1U && info->dt == 2U) {
        ysf_handle_vd_type2(opts, state, info);
        read = true;
    }
    if (info->fi == 1U && info->dt == 3U) {
        ysf_handle_full_rate_voice(opts, state, info);
        read = true;
    }
    if (info->dt == 1U || info->fi == 0U || info->fi == 2U) {
        ysf_handle_full_rate_data(opts, state, info);
        read = true;
    }
    return read;
}

/* The frame sync the hunt just accepted places the symbol grid for the frame behind it. The hunt's crossing loop leaves
   the grid where the sync's crossings put it, and the inner symbols among them sit their crossings off the symbol
   boundaries: clean frames were read two samples early, where the root-raised-cosine eye is half shut. The previous
   frame armed a recording at its end (ysf_arm_next_sync_anchor()), so its first sample is the sync's first; the word is
   named from its second symbol, which leaves the measurement its sample of margin before the first centre. */
static void
ysf_note_sync_anchor(const dsd_state* state) {
    const char* sync = state->synctype == DSD_SYNC_YSF_NEG ? INV_FUSION_SYNC_DIBITS : FUSION_SYNC_DIBITS;
    dsd_symbol_note_walked_sync_anchor(state, sync + 1, 19);
}

/* A frame whose payload was read ends where the next one's sync begins: record it for ysf_note_sync_anchor(). One whose
   FICH named no payload leaves the hunt to read through it. */
static void
ysf_arm_next_sync_anchor(dsd_state* state, bool payload_read) {
    if (payload_read) {
        dsd_symbol_arm_hunt_sync_anchor(state);
    }
}

int
processYSF(dsd_opts* opts, dsd_state* state) {
    ysf_fich_info info;

    ysf_note_sync_anchor(state);
    info.carrier_seq = state->carrier_seq;
    ysf_parse_fich(opts, state, &info);
    if (dsd_state_frame_read_lost(state, info.carrier_seq)) {
        /* The FICH was split or cut: no call observation on the fallback dt/fi, and no payload laid out on them. The
           frame is dropped, as on a sync loss. */
        DSD_FPRINTF(stderr, "\n");
        return 0;
    }
    ysf_print_fich_summary(opts, &info);
    if (info.err == 0 && !ysf_carrier_left(state, &info)) {
        /* Confirmed as soon as the FICH verifies, so this frame's own voice is captured (issue #661); the verdict
           below reads the flag as before. */
        state->ysf_fich_confirmed = 1;
        /* A verified header or terminator bounds the transmission: its capture ends here, even when the terminator
           of the one before it was lost. */
        if (info.fi == 0U || info.fi == 2U) {
            dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT1, DSD_SYNC_YSF_POS);
        }
    }

    ysf_update_call_lifecycle(opts, state, &info);
    const bool payload_read = ysf_dispatch_payload(opts, state, &info);
    ysf_end_call_lifecycle(opts, state, &info);
    /* A payload a carrier boundary cut ends nowhere near the next word: the hunt resumes inside the new carrier. */
    ysf_arm_next_sync_anchor(state, payload_read && !ysf_carrier_left(state, &info));

    DSD_FPRINTF(stderr, "%s", KNRM);
    DSD_FPRINTF(stderr, "\n");

    /* Sticky per transmission, the shape nxdn_confirm_is_confirmed() uses. A FICH failure on
     * its own says nothing decoded only until one FICH has passed: after that the fallback
     * dt/fi come from a frame that did check out, ysf_dispatch_payload() lays the rest of the
     * frame out on them, and ysf_handle_vd_type2() synthesizes and plays voice from it. A
     * frame that produced audio must not tell the SPS hunt it validated nothing (#391).
     * noCarrier() clears the flag with the other per-transmission YSF state.
     *
     * A frame the carrier boundary split confirms nothing and reports nothing decoded (issue #575): its FICH was the
     * carrier left's, the boundary restarted the verdict, and the payload was dropped. */
    if (ysf_carrier_left(state, &info)) {
        return 0;
    }
    if (info.err == 0) {
        state->ysf_fich_confirmed = 1;
    }
    return state->ysf_fich_confirmed != 0 ? 1 : 0;
} //end processYSF
