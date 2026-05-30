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
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/mbe_api.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/ysf/ysf.h>
#include <dsd-neo/runtime/colors.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "ysf_frame.h"

static uint8_t fr_interleave[144] = {
    0,   7,   12,  19,  24,  31,  36,  43,  48,  55,  60,  67, // [  0 -  11] yellow message
    72,  79,  84,  91,  96,  103, 108, 115, 120, 127, 132,     // [ 12 -  22] yellow FEC
    139, 1,   6,   13,  18,  25,  30,  37,  42,  49,  54,  61, // [ 23 -  34] orange message
    66,  73,  78,  85,  90,  97,  102, 109, 114, 121, 126,     // [ 35 -  45] orange FEC
    133, 138, 2,   9,   14,  21,  26,  33,  38,  45,  50,  57, // [ 46 -  57] red message
    62,  69,  74,  81,  86,  93,  98,  105, 110, 117, 122,     // [ 58 -  68] red FEC
    129, 134, 141, 3,   8,   15,  20,  27,  32,  39,  44,  51, // [ 69 -  80] pink message
    56,  63,  68,  75,  80,  87,  92,  99,  104, 111, 116,     // [ 81 -  91] pink FEC
    123, 128, 135, 140, 4,   11,  16,  23,  28,  35,  40,      // [ 92 - 102] dark blue message
    47,  52,  59,  64,                                         // [103 - 106] dark blue FEC
    71,  76,  83,  88,  95,  100, 107, 112, 119, 124, 131,     // [107 - 117] light blue message
    136, 143, 5,   10,                                         // [118 - 121] light blue FEC
    17,  22,  29,  34,  41,  46,  53,  58,  65,  70,  77,      // [122 - 132] green message
    82,  89,  94,  101,                                        // [133 - 136] green FEC
    106, 113, 118, 125, 130, 137, 142,                         // [137 - 143] unprotected
};

static uint8_t pn95[512] = {
    1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1,
    1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0,
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 1,
    1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1,
    1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0,
    1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0,
    0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0,
    0, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0,
    1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 1, 0,
    0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1,
    1, 1, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0,
    0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0,
    0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0,
    1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1};

//half-rate (from NXDN)
static const int YnW[36] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                            0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2};

static const int YnX[36] = {23, 10, 22, 9, 21, 8,  20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2,
                            14, 1,  13, 0, 12, 10, 11, 9, 10, 8, 9,  7, 8,  6, 7,  5, 6,  4};

static const int YnY[36] = {0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3,
                            1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3};

static const int YnZ[36] = {5,  3, 4,  2, 3,  1, 2,  0, 1,  13, 0,  12, 22, 11, 21, 10, 20, 9,
                            19, 8, 18, 7, 17, 6, 16, 5, 15, 4,  14, 3,  13, 2,  12, 1,  11, 0};

//M = 26, depth of 4; -- from DSDcc
static const int vd2Interleave[104] = {
    0,  26, 52, 78, 1,  27, 53, 79, 2,  28, 54, 80, 3,  29,  55, 81, 4,  30,  56, 82, 5,  31,  57, 83, 6,  32,
    58, 84, 7,  33, 59, 85, 8,  34, 60, 86, 9,  35, 61, 87,  10, 36, 62, 88,  11, 37, 63, 89,  12, 38, 64, 90,
    13, 39, 65, 91, 14, 40, 66, 92, 15, 41, 67, 93, 16, 42,  68, 94, 17, 43,  69, 95, 18, 44,  70, 96, 19, 45,
    71, 97, 20, 46, 72, 98, 21, 47, 73, 99, 22, 48, 74, 100, 23, 49, 75, 101, 24, 50, 76, 102, 25, 51, 77, 103};

static void
ysf_dch_decode_csd1(dsd_state* state, const char dch_bytes[20], uint8_t cm) {
    char string2[11];

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

    DSD_MEMSET(state->ysf_tgt, 0, sizeof(state->ysf_tgt));
    DSD_MEMCPY(state->ysf_tgt, dch_bytes, 10);
    state->ysf_tgt[10] = '\0';

    DSD_MEMSET(state->ysf_src, 0, sizeof(state->ysf_src));
    DSD_MEMCPY(state->ysf_src, dch_bytes + 10, 10);
    state->ysf_src[10] = '\0';
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

    DSD_MEMCPY(state->ysf_upl, dch_bytes, 10);
    state->ysf_upl[10] = '\0';

    state->ysf_dnl[10] = '\0';
    DSD_MEMCPY(state->ysf_dnl, dch_bytes + 10, 10);
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

static void
ysf_dch_decode(dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm, uint8_t input[]) {
    //TODO: Per Call WAV files using these strings
    int i;
    char dch_bytes[20];
    DSD_MEMSET(dch_bytes, 0, sizeof(dch_bytes));

    UNUSED(bt);

    for (i = 0; i < 20; i++) {
        dch_bytes[i] = (char)ConvertBitIntoBytes(&input[(size_t)i * 8u], 8);
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
ysf_store_text_field(const char* label, const char* src, size_t len, char* dst, const char* suffix) {
    ysf_print_text_field(label, src, len, suffix);
    ysf_copy_text_field(dst, src, len);
}

static void
ysf_dch_decode2_destination(dsd_state* state, const char dch_bytes[20], uint8_t cm) {
    if (cm != 1) {
        ysf_print_text_field("DST: ", dch_bytes, 10, " ");
    } else {
        ysf_print_text_field("DST RID: ", dch_bytes, 5, " ");
        ysf_print_text_field("SRC RID: ", dch_bytes + 5, 5, " ");
    }
    ysf_copy_text_field(state->ysf_tgt, dch_bytes, 10);
}

static void
ysf_dch_decode2_remarks(const char* label1, char* dst1, const char* label2, char* dst2, const char dch_bytes[20]) {
    ysf_store_text_field(label1, dch_bytes, 5, dst1, " ");
    ysf_store_text_field(label2, dch_bytes + 5, 5, dst2, " ");
}

static void
ysf_dch_decode2(dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm, uint8_t input[]) {
    char dch_bytes[20];
    DSD_MEMSET(dch_bytes, 0, sizeof(dch_bytes));

    UNUSED3(bn, bt, ft);

    for (int i = 0; i < 10; i++) {
        dch_bytes[i] = (char)ConvertBitIntoBytes(&input[(size_t)i * 8u], 8);
    }

    switch (fn) {
        case 0: ysf_dch_decode2_destination(state, dch_bytes, cm); break;
        case 1: ysf_store_text_field("SRC: ", dch_bytes, 10, state->ysf_src, ""); break;
        case 2: ysf_store_text_field("U/L: ", dch_bytes, 10, state->ysf_upl, ""); break;
        case 3: ysf_store_text_field("D/L: ", dch_bytes, 10, state->ysf_dnl, ""); break;
        case 4: ysf_dch_decode2_remarks("RM1: ", state->ysf_rm1, "RM2: ", state->ysf_rm2, dch_bytes); break;
        case 5: ysf_dch_decode2_remarks("RM3: ", state->ysf_rm3, "RM4: ", state->ysf_rm4, dch_bytes); break;
        default: break;
    }
}

static inline uint16_t
crc16ysf(const uint8_t buf[], int len) {
    uint32_t poly = (1 << 12) + (1 << 5) + (1 << 0);
    uint32_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t bit = buf[i] & 1;
        crc = ((crc << 1) | bit) & 0x1ffff;
        if (crc & 0x10000) {
            crc = (crc & 0xffff) ^ poly;
        }
    }
    crc = crc ^ 0xffff;
    return crc & 0xffff;
}

//modified version of nxdn_deperm_facch1 -- this one for V/D Type 2 CC DCH (100 dibit version)
static int
ysf_conv_dch2(const dsd_opts* opts, dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
              uint8_t input[]) {

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

    uint16_t crc = crc16ysf(trellis_buf, 96);
    if (crc != 0) {
        err = -2; // crc failure
    }

    for (i = 0; i < 80; i++) {
        trellis_buf[i] = trellis_buf[i] ^ pn95[i];
    }

    //reload after de-whitening
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    for (i = 0; i < 12; i++) {
        m_data[i] = (uint8_t)ConvertBitIntoBytes(&trellis_buf[(size_t)i * 8u], 8);
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
static int
ysf_conv_dch(const dsd_opts* opts, dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
             uint8_t input[]) {
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

    uint16_t crc = crc16ysf(trellis_buf, 176);
    if (crc != 0) {
        err = -2; // crc failure
    }

    for (i = 0; i < 160; i++) {
        trellis_buf[i] = trellis_buf[i] ^ pn95[i];
    }

    //reload after de-whitening
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    for (i = 0; i < 22; i++) {
        m_data[i] = (uint8_t)ConvertBitIntoBytes(&trellis_buf[(size_t)i * 8u], 8);
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
static int
ysf_conv_fich(uint8_t input[], uint8_t dest[32], uint32_t* v_error_out) {
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

    uint16_t crc = crc16ysf(fich_bits, 48);
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
        const int *w = YnW, *x = YnX, *y = YnY, *z = YnZ;

        //debug
        // DSD_FPRINTF(stderr, " DBUF = ");

        for (i = 0; i < 36; i++) {

            //debug
            // DSD_FPRINTF(stderr, "%d", dbuf[(start*36)+i]);

            uint8_t b1 = dbuf[(start * 36) + i] >> 1;
            uint8_t b2 = dbuf[(start * 36) + i] & 1;

            //should all be loaded back to back
            ambe_fr[*w][*x] = (char)b1;
            ambe_fr[*y][*z] = (char)b2;

            w++;
            x++;
            y++;
            z++;
        }

        processMbeFrame(opts, state, NULL, ambe_fr, NULL);

        if (opts->floating_point == 0) {
            // processAudio(opts, state); //needed here? -- nothign to test it with

            if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1) {
                writeSynthesizedVoice(opts, state);
            }

            if (opts->pulse_digi_out_channels == 1) {
                playSynthesizedVoiceMS(opts, state);
            }

            if (opts->pulse_digi_out_channels == 2) {
                playSynthesizedVoiceSS(opts, state);
            }
        }

        if (opts->floating_point == 1) //float audio is really quiet now (look into it)
        {

            DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));

            if (opts->pulse_digi_out_channels == 1) {
                playSynthesizedVoiceFM(opts, state);
            }

            if (opts->pulse_digi_out_channels == 2) {
                playSynthesizedVoiceFS(opts, state);
            }
        }
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

        if (opts->pulse_digi_out_channels == 1) {
            playSynthesizedVoiceMS(opts, state);
        }

        if (opts->pulse_digi_out_channels == 2) {
            playSynthesizedVoiceSS(opts, state);
        }
        return;
    }

    DSD_MEMCPY(state->f_l, state->audio_out_temp_buf, sizeof(state->f_l));

    if (opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }

    if (opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }
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
        fichrawdibits[i] = getDibit(opts, state);
    }

    info->err = ysf_conv_fich(fichrawdibits, info->fich_decode, &info->v_error);
    if (info->err == 0) {
        info->fi = (uint8_t)ConvertBitIntoBytes(&info->fich_decode[0], 2);
        info->cm = (uint8_t)ConvertBitIntoBytes(&info->fich_decode[4], 2);
        info->bn = (uint8_t)ConvertBitIntoBytes(&info->fich_decode[6], 2);
        info->bt = (uint8_t)ConvertBitIntoBytes(&info->fich_decode[8], 2);
        info->fn = (uint8_t)ConvertBitIntoBytes(&info->fich_decode[10], 3);
        info->ft = (uint8_t)ConvertBitIntoBytes(&info->fich_decode[13], 3);
        info->mr = (uint8_t)ConvertBitIntoBytes(&info->fich_decode[18], 3);
        info->vp = info->fich_decode[21];
        info->dt = (uint8_t)ConvertBitIntoBytes(&info->fich_decode[22], 2);
        info->st = info->fich_decode[24];
        info->sc = (uint8_t)ConvertBitIntoBytes(&info->fich_decode[25], 7);

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
            DSD_FPRINTF(stderr, "[%02X]", (uint8_t)ConvertBitIntoBytes(&info->fich_decode[(size_t)i * 8], 8));
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
            dbuf[(i * 36) + j] = getDibit(opts, state);
        }
        for (int j = 0; j < 36; j++) {
            vbuf[(i * 36) + j] = getDibit(opts, state);
        }
    }

    ysf_ehr(opts, state, vbuf, 0, 4);
    ysf_conv_dch(opts, state, info->bn, info->bt, info->fn, info->ft, info->cm, dbuf);
}

static void
ysf_read_type2_vech_bits(dsd_opts* opts, dsd_state* state, uint8_t vech_bits[104]) {
    int k = 0;
    for (int j = 0; j < 52; j++) {
        int dibit = getDibit(opts, state);
        uint8_t b1 = (uint8_t)((dibit >> 1) & 1);
        uint8_t b2 = (uint8_t)(dibit & 1);
        uint8_t msb = (uint8_t)vd2Interleave[k++];
        uint8_t lsb = (uint8_t)vd2Interleave[k++];

        vech_bits[msb] = b1 ^ pn95[msb];
        vech_bits[lsb] = b2 ^ pn95[lsb];
    }
}

static void
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
            dbuf[d++] = getDibit(opts, state);
        }

        DSD_MEMSET(vech_bits, 0, sizeof(vech_bits));
        state->errs = 0;
        state->errs2 = 0;

        ysf_read_type2_vech_bits(opts, state, vech_bits);
        ysf_build_type2_ambe(vech_bits, temp, ambe_d);

        state->errs2 = vech_bits[103];
        state->debug_audio_errors += state->errs2;

        (void)dsd_mbe_process_ambe2450_dataf(state->audio_out_temp_buf, &state->errs, &state->errs2, state->err_str,
                                             sizeof(state->err_str), ambe_d, state->cur_mp, state->prev_mp,
                                             state->prev_mp_enhanced, NULL);

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
                dbuf[(bank * 36) + j] = getDibit(opts, state);
            } else {
                skipDibit(opts, state, 1);
            }
        }
    }
}

static void
ysf_read_full_rate_imbe_raw(dsd_opts* opts, dsd_state* state, uint8_t imbe_raw[144]) {
    for (int j = 0; j < 72; j++) {
        int dibit = getDibit(opts, state);
        imbe_raw[(j * 2) + 0] = (uint8_t)((dibit >> 1) & 1);
        imbe_raw[(j * 2) + 1] = (uint8_t)(dibit & 1);
    }
}

static void
ysf_unpack_full_rate_imbe(const uint8_t imbe_raw[144], uint8_t imbe_vch[144], char imbe_fr[8][23]) {
    int k = 0;

    for (int j = 0; j < 144; j++) {
        imbe_vch[j] = imbe_raw[fr_interleave[j]];
    }

    for (int n = 0; n < 4; n++) {
        for (int m = 22; m >= 0; m--) {
            imbe_fr[n][m] = (char)imbe_vch[k++];
        }
    }
    for (int n = 4; n < 7; n++) {
        for (int m = 14; m >= 0; m--) {
            imbe_fr[n][m] = (char)imbe_vch[k++];
        }
    }
    for (int m = 6; m >= 0; m--) {
        imbe_fr[7][m] = (char)imbe_vch[k++];
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
    ysf_unpack_full_rate_imbe(imbe_raw, imbe_vch, imbe_fr);

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
            dbuf_fr[i % 2][((i / 2) * 36) + j] = getDibit(opts, state);
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

void
processYSF(dsd_opts* opts, dsd_state* state) {
    static uint8_t last_dt;
    static uint8_t last_fi;
    ysf_fich_info info;

    ysf_parse_fich(opts, state, &info, &last_dt, &last_fi);
    ysf_print_fich_summary(opts, &info);

    if (info.fi == 1 && info.dt == 0) {
        ysf_handle_vd_type1(opts, state, &info);
    }

    if (info.fi == 1 && info.dt == 2) {
        ysf_handle_vd_type2(opts, state, &info);
    }

    if (info.fi == 1 && info.dt == 3) {
        ysf_handle_full_rate_voice(opts, state, &info);
    }

    if (info.dt == 1 || info.fi == 0 || info.fi == 2) {
        ysf_handle_full_rate_data(opts, state, &info);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
    DSD_FPRINTF(stderr, "\n");
} //end processYSF
