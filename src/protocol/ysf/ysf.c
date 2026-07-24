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

#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/ysf/ysf.h>
#include <dsd-neo/runtime/colors.h>
#include <mbelib-neo/mbelib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "ysf_frame.h"
#include "ysf_internal.h"

static void
ysf_enrich_identity(dsd_state* state, uint8_t cm, const char* source, const char* target, const char* uplink,
                    const char* downlink) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || !DSD_SYNC_IS_YSF(call.protocol)) {
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

static void
ysf_dch_decode_csd2(dsd_state* state, const char dch_bytes[20]) {
    char string1[11];
    char string2[11];

    DSD_MEMCPY(string1, dch_bytes, 10);
    string1[10] = '\0';
    DSD_FPRINTF(stderr, "U/L: ");
    DSD_FPRINTF(stderr, "%s ", string1);

    DSD_MEMCPY(string2, dch_bytes + 10, 10);
    string2[10] = '\0';
    DSD_FPRINTF(stderr, "D/L: ");
    DSD_FPRINTF(stderr, "%s ", string2);

    ysf_enrich_identity(state, state->ysf_cm, NULL, NULL, string1, string2);
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
ysf_dch_decode(dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm, const uint8_t input[160]) {
    //TODO: Per Call WAV files using these strings
    int i;
    char dch_bytes[20];
    DSD_MEMSET(dch_bytes, 0, sizeof(dch_bytes));

    UNUSED(bt);

    for (i = 0; i < 20; i++) {
        dch_bytes[i] = (char)convert_bits_into_output(&input[(size_t)i * 8u], 8);
    }

    switch (bn) { //using bn here so we can use the frame number for sorting the text messages found in here
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
        case 2: {
            char uplink[11];
            ysf_print_text_field("U/L: ", dch_bytes, 10, "");
            ysf_copy_text_field(uplink, dch_bytes, 10);
            ysf_enrich_identity(state, cm, NULL, NULL, uplink, NULL);
            break;
        }
        case 3: {
            char downlink[11];
            ysf_print_text_field("D/L: ", dch_bytes, 10, "");
            ysf_copy_text_field(downlink, dch_bytes, 10);
            ysf_enrich_identity(state, cm, NULL, NULL, NULL, downlink);
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
ysf_conv_dch(const dsd_opts* opts, dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
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
        ysf_dch_decode(state, bn, bt, fn, ft, cm, trellis_buf);
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

        processMbeFrame(opts, state, NULL, ambe_fr, NULL);

        if (opts->floating_point == 0) {
            // processAudio(opts, state); //needed here? -- nothign to test it with

            if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1) {
                writeSynthesizedVoice(opts, state);
            }
        }

        if (opts->floating_point == 1) //float audio is really quiet now (look into it)
        {

            DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));
        }
        dsd_play_synthesized_voice(opts, state);
    }

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

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
} ysf_fich_info;

static void
ysf_emit_audio_from_temp(dsd_opts* opts, dsd_state* state, bool run_process_audio) {
    if (opts->floating_point == 0) {
        if (run_process_audio) {
            processAudio(opts, state);
        }

        if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1) {
            writeSynthesizedVoice(opts, state);
        }

    } else {
        DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));
    }
    dsd_play_synthesized_voice(opts, state);
}

static void
ysf_parse_fich(dsd_opts* opts, dsd_state* state, ysf_fich_info* info, uint8_t* last_dt, uint8_t* last_fi) {
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
    info->sc = 69;
    info->v_error = UINT32_MAX;

    for (int i = 0; i < 100; i++) {
        fichrawdibits[i] = get_dibit_and_analog_signal(opts, state, NULL);
    }

    info->err = ysf_conv_fich(fichrawdibits, info->fich_decode, &info->v_error);
    if (info->err == 0) {
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

        *last_dt = info->dt;
        *last_fi = info->fi;
        return;
    }

    info->dt = *last_dt;
    info->fi = *last_fi;
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
    if (info->st && info->sc != 69) {
        DSD_FPRINTF(stderr, "SQL ");
        DSD_FPRINTF(stderr, "CODE: %03d ", info->sc);
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

static void
ysf_handle_vd_type1(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    uint8_t dbuf[190];
    uint8_t vbuf[190];
    DSD_MEMSET(dbuf, 0, sizeof(dbuf));
    DSD_MEMSET(vbuf, 0, sizeof(vbuf));

    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 36; j++) {
            dbuf[(i * 36) + j] = get_dibit_and_analog_signal(opts, state, NULL);
        }
        for (int j = 0; j < 36; j++) {
            vbuf[(i * 36) + j] = get_dibit_and_analog_signal(opts, state, NULL);
        }
    }

    ysf_ehr(opts, state, vbuf, 0, 4);
    ysf_conv_dch(opts, state, info->bn, info->bt, info->fn, info->ft, info->cm, dbuf);
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
            state->errs = ((result.flags & MBE_PROCESS_FLAG_C0_VALID) != 0u) ? result.c0_errors : result.total_errors;
            state->errs2 = result.total_errors;
            mbe_formatProcessResult(state->err_str, sizeof(state->err_str), &result);
        }

        if (dsd_frame_detail_enabled(opts)) {
            PrintAMBEData(opts, state, ambe_d);
        }

        ysf_emit_audio_from_temp(opts, state, true);
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

static void
ysf_decode_full_rate_voice_slot(dsd_opts* opts, dsd_state* state) {
    uint8_t imbe_raw[144];
    uint8_t imbe_vch[144];
    char imbe_fr[8][23];
    int synctype = state->synctype;

    DSD_MEMSET(imbe_raw, 0, sizeof(imbe_raw));
    DSD_MEMSET(imbe_vch, 0, sizeof(imbe_vch));
    DSD_MEMSET(imbe_fr, 0, sizeof(imbe_fr));

    ysf_read_full_rate_imbe_raw(opts, state, imbe_raw);
    dsd_ysf_unpack_full_rate_imbe(imbe_raw, imbe_vch, imbe_fr);

    state->synctype = DSD_SYNC_P25P1_POS;
    processMbeFrame(opts, state, imbe_fr, NULL, NULL);
    state->synctype = synctype;

    ysf_emit_audio_from_temp(opts, state, false);
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
        ysf_decode_full_rate_voice_slot(opts, state);
    }

    if (is_csd3) {
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

static void
ysf_update_call_lifecycle(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    if ((info->err == 0 && (info->fi == 0U || info->fi == 1U)) || ysf_is_fallback_voice_frame(info)) {
        const int protocol = DSD_SYNC_IS_YSF(state->synctype) ? state->synctype : DSD_SYNC_YSF_POS;
        const dsd_call_observation observation = {
            .protocol = protocol,
            .slot = 0U,
            .kind = info->dt == 1U ? DSD_CALL_KIND_DATA : ysf_call_kind(info->cm),
        };
        const dsd_call_boundary boundary = info->fi == 0U ? DSD_CALL_BOUNDARY_BEGIN : DSD_CALL_BOUNDARY_CONTINUE;
        if (dsd_call_state_observe(state, &observation, boundary) > 0) {
            dsd_event_sync_slot(opts, state, 0U);
        }
    }
}

static void
ysf_end_call_lifecycle(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    if (info->err == 0 && info->fi == 2U && dsd_call_state_end(state, 0U, 0.0) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

static void
ysf_dispatch_payload(dsd_opts* opts, dsd_state* state, const ysf_fich_info* info) {
    if (info->fi == 1U && info->dt == 0U) {
        ysf_handle_vd_type1(opts, state, info);
    }
    if (info->fi == 1U && info->dt == 2U) {
        ysf_handle_vd_type2(opts, state, info);
    }
    if (info->fi == 1U && info->dt == 3U) {
        ysf_handle_full_rate_voice(opts, state, info);
    }
    if (info->dt == 1U || info->fi == 0U || info->fi == 2U) {
        ysf_handle_full_rate_data(opts, state, info);
    }
}

void
processYSF(dsd_opts* opts, dsd_state* state) {
    static uint8_t last_dt;
    static uint8_t last_fi;
    ysf_fich_info info;

    ysf_parse_fich(opts, state, &info, &last_dt, &last_fi);
    ysf_print_fich_summary(opts, &info);

    ysf_update_call_lifecycle(opts, state, &info);
    ysf_dispatch_payload(opts, state, &info);
    ysf_end_call_lifecycle(opts, state, &info);

    DSD_FPRINTF(stderr, "%s", KNRM);
    DSD_FPRINTF(stderr, "\n");
} //end processYSF
