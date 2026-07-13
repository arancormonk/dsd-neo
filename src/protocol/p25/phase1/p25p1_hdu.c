// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <dsd-neo/core/bit_packing.h>

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_lfsr.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"

static int
soft_abs_i16(int16_t v) {
    return v < 0 ? -(int)v : (int)v;
}

int
read_dibit_soft(dsd_opts* opts, dsd_state* state, char* output, int* status_count, P25P1SoftDibit* soft_dibit) {
    int dibit;
    dsd_dibit_soft_t soft;

    if (*status_count == 35) {

#ifdef TRACE_DSD
        char prev_prefix = state->debug_prefix;
        state->debug_prefix = 's';
#endif

        dsd_dibit_soft_t status_soft;
        int status_dibit = getDibitSoft(opts, state, &status_soft);
        p25_status_accum_add(state, status_dibit);
        *status_count = 1;

#ifdef TRACE_DSD
        state->debug_prefix = prev_prefix;
#endif

    } else {
        (*status_count)++;
    }

    dibit = getDibitSoft(opts, state, &soft);
    if (soft_dibit != NULL) {
        soft_dibit->reliab = soft.reliability;
        soft_dibit->llr[0] = soft.llr[0];
        soft_dibit->llr[1] = soft.llr[1];
    }
    output[0] = (1 & (dibit >> 1)); // bit 1
    output[1] = (1 & dibit);        // bit 0

    return dibit;
}

void
read_dibit_update_soft_data(dsd_opts* opts, dsd_state* state, char* buffer, unsigned int count, int* status_count,
                            P25P1SoftDibit* soft_dibits, int* soft_dibit_index) {
    unsigned int i;

    for (i = 0; i < count; i += 2) {
        P25P1SoftDibit soft_dibit;

        (void)read_dibit_soft(opts, state, buffer + i, status_count, &soft_dibit);

        if (soft_dibits != NULL && soft_dibit_index != NULL) {
            soft_dibits[*soft_dibit_index] = soft_dibit;
            (*soft_dibit_index)++;
        }
    }
}

void
read_word(dsd_opts* opts, dsd_state* state, char* word, unsigned int length, int* status_count,
          P25P1SoftDibit* soft_dibits, int* soft_dibit_index) {
    read_dibit_update_soft_data(opts, state, word, length, status_count, soft_dibits, soft_dibit_index);
}

void
read_golay24_parity(dsd_opts* opts, dsd_state* state, char* parity, int* status_count, P25P1SoftDibit* soft_dibits,
                    int* soft_dibit_index) {
    read_dibit_update_soft_data(opts, state, parity, 12, status_count, soft_dibits, soft_dibit_index);
}

void
read_hamm_parity(dsd_opts* opts, dsd_state* state, char* parity, int* status_count, P25P1SoftDibit* soft_dibits,
                 int* soft_dibit_index) {
    // Read 2 dibits = read 4 bits.
    read_dibit_update_soft_data(opts, state, parity, 4, status_count, soft_dibits, soft_dibit_index);
}

/**
 * Corrects a hex (6 bit) word using the Golay 24 FEC.
 * Uses soft decode if hard decode fails and reliability info is available.
 *
 * @param state Decoder state for error tracking.
 * @param hex   The 6-bit data word (modified in place).
 * @param parity The 12-bit parity word.
 * @param soft_dibits Soft dibit array for this hex word (9 dibits: 3 data + 6 parity).
 *                    May be NULL to disable soft decode.
 */
static void
correct_hex_word(dsd_opts* opts, dsd_state* state, char* hex, char* parity, const P25P1SoftDibit* soft_dibits) {
    (void)opts;
    int fixed_errors;
    int irrecoverable_errors;
    char raw_hex[6];
    char raw_parity[12];
    DSD_MEMCPY(raw_hex, hex, sizeof(raw_hex));
    DSD_MEMCPY(raw_parity, parity, sizeof(raw_parity));

    irrecoverable_errors = check_and_fix_golay_24_6(hex, parity, &fixed_errors);

    state->debug_header_errors += fixed_errors;

    if ((irrecoverable_errors != 0 || fixed_errors > 0) && soft_dibits != NULL) {
        /* Hard decode failed or corrected low-confidence bits; try soft decode using reliability info.
         * The soft_dibits array contains 9 dibits:
         *   [0..2] = 3 dibits for 6 data bits
         *   [3..8] = 6 dibits for 12 parity bits
         * Extract per-bit reliability by taking dibit reliability for both bits.
         */
        int reliab[18];
        int idx = 0;

        /* Data bits: 3 dibits -> 6 bits */
        for (int d = 0; d < 3; d++) {
            reliab[idx++] = soft_abs_i16(soft_dibits[d].llr[0]);
            reliab[idx++] = soft_abs_i16(soft_dibits[d].llr[1]);
        }
        /* Parity bits: 6 dibits -> 12 bits */
        for (int d = 3; d < 9; d++) {
            reliab[idx++] = soft_abs_i16(soft_dibits[d].llr[0]);
            reliab[idx++] = soft_abs_i16(soft_dibits[d].llr[1]);
        }

        int soft_fixed = 0;
        char soft_hex[6];
        char soft_parity[12];
        DSD_MEMCPY(soft_hex, raw_hex, sizeof(soft_hex));
        DSD_MEMCPY(soft_parity, raw_parity, sizeof(soft_parity));
        int soft_result = check_and_fix_golay_24_6_soft(soft_hex, soft_parity, reliab, &soft_fixed);
        if (soft_result == 0) {
            DSD_MEMCPY(hex, soft_hex, sizeof(soft_hex));
            DSD_MEMCPY(parity, soft_parity, sizeof(soft_parity));
            if (irrecoverable_errors != 0) {
                state->p25_p1_soft_golay_ok++;
                state->debug_header_errors += soft_fixed;
            }
            irrecoverable_errors = 0;
        }
    }

    if (irrecoverable_errors != 0) {
        state->debug_header_critical_errors++;
    }
}

static uint8_t
rs_hex_symbol_reliability(const P25P1SoftDibit* symbol) {
    int16_t llr[6];

    for (int i = 0; i < 3; i++) {
        llr[(i * 2) + 0] = symbol[i].llr[0];
        llr[(i * 2) + 1] = symbol[i].llr[1];
    }
    return p25p1_llr_reliability(llr, 6);
}

static void
build_hdu_rs_reliability(const P25P1SoftDibit* soft_dibits, uint8_t data_reliab[20], uint8_t parity_reliab[16]) {
    for (int i = 0; i < 20; i++) {
        int soft_index = (19 - i) * (3 + 6);
        data_reliab[i] = rs_hex_symbol_reliability(soft_dibits + soft_index);
    }
    for (int i = 0; i < 16; i++) {
        int soft_index = (20 * (3 + 6)) + ((15 - i) * (3 + 6));
        parity_reliab[i] = rs_hex_symbol_reliability(soft_dibits + soft_index);
    }
}

/**
 * Reads an hex word, its parity bits and attempts to error correct it using the Golay24 algorithm.
 */
static void
read_and_correct_hex_word(dsd_opts* opts, dsd_state* state, char* hex, int* status_count, P25P1SoftDibit* soft_dibits,
                          int* soft_dibit_index) {
    char parity[12];

    /* Remember where this hex word's soft dibits start */
    int start_index = *soft_dibit_index;

    // Read the hex word
    read_word(opts, state, hex, 6, status_count, soft_dibits, soft_dibit_index);
    // Read the parity
    read_golay24_parity(opts, state, parity, status_count, soft_dibits, soft_dibit_index);

    // Use the Golay24 FEC to correct it. This call modifies the content of hex to fix it, hopefully.
    // Pass the soft dibit array starting at this hex word for soft decode support.
    const P25P1SoftDibit* hex_soft = (soft_dibits != NULL) ? &soft_dibits[start_index] : NULL;
    correct_hex_word(opts, state, hex, parity, hex_soft);
}

static void
hdu_extract_mi_algid_kid(const char hex_data[20][6], uint8_t mi[73], char algid[9], char kid[17]) {
    size_t mi_pos = 0;
    for (int row = 19; row >= 8; row--) {
        for (int bit = 0; bit < 6; bit++) {
            mi[mi_pos++] = (uint8_t)(hex_data[row][bit] + '0');
        }
    }
    mi[72] = 0;

    size_t alg_pos = 0;
    for (int bit = 2; bit < 6; bit++) {
        algid[alg_pos++] = (char)(hex_data[6][bit] + '0');
    }
    for (int bit = 0; bit < 4; bit++) {
        algid[alg_pos++] = (char)(hex_data[5][bit] + '0');
    }
    algid[8] = 0;

    size_t kid_pos = 0;
    kid[kid_pos++] = (char)(hex_data[5][4] + '0');
    kid[kid_pos++] = (char)(hex_data[5][5] + '0');
    for (int row = 4; row >= 3; row--) {
        for (int bit = 0; bit < 6; bit++) {
            kid[kid_pos++] = (char)(hex_data[row][bit] + '0');
        }
    }
    kid[kid_pos++] = (char)(hex_data[2][0] + '0');
    kid[kid_pos++] = (char)(hex_data[2][1] + '0');
    kid[16] = 0;
}

static void
hdu_consume_trailing_dibits_and_status(dsd_opts* opts, dsd_state* state) {
    for (int i = 0; i < 5; i++) {
        dsd_dibit_soft_t skip_soft;
        (void)getDibitSoft(opts, state, &skip_soft);
    }

    dsd_dibit_soft_t status_soft;
    int ss = getDibitSoft(opts, state, &status_soft);
    p25_status_accum_add(state, ss);
}

static int
hdu_read_and_fec(dsd_opts* opts, dsd_state* state, char hex_data[20][6], char hex_parity[16][6],
                 P25P1SoftDibit soft_dibits[20 * (3 + 6) + 16 * (3 + 6)], int* status_count, int* soft_dibit_index) {
    char hex[6];

    for (int i = 19; i >= 0; i--) {
        read_and_correct_hex_word(opts, state, hex, status_count, soft_dibits, soft_dibit_index);
        for (int j = 0; j < 6; j++) {
            hex_data[i][j] = hex[j];
        }
    }

    for (int i = 15; i >= 0; i--) {
        read_and_correct_hex_word(opts, state, hex, status_count, soft_dibits, soft_dibit_index);
        for (int j = 0; j < 6; j++) {
            hex_parity[i][j] = hex[j];
        }
    }

    int irrecoverable_errors = check_and_fix_redsolomon_36_20_17((char*)hex_data, (char*)hex_parity);
    if (irrecoverable_errors != 0) {
        uint8_t data_reliab[20];
        uint8_t parity_reliab[16];
        build_hdu_rs_reliability(soft_dibits, data_reliab, parity_reliab);
        if (p25p1_rs_36_20_17_soft_reliability((char*)hex_data, (char*)hex_parity, data_reliab, parity_reliab) == 0) {
            state->p25_p1_soft_rs_ok++;
            irrecoverable_errors = 0;
        }
    }

    if (irrecoverable_errors != 0) {
        state->p25_p1_voice_fec_err++;
        state->debug_header_critical_errors++;
        return irrecoverable_errors;
    }

    state->p25_p1_voice_fec_ok++;
    state->last_vc_sync_time = time(NULL);
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();

    return 0;
}

static void
hdu_maybe_enc_lockout(dsd_opts* opts, dsd_state* state, int algid, int keyid, uint64_t mi) {
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE1, 0, algid, keyid, mi, state->lasttg);
}

static void
hdu_report_decryption_key(const dsd_opts* opts, const dsd_state* state) {
    if (state->p25_crypto_state[0] == DSD_P25_CRYPTO_DECRYPTABLE
        && (state->payload_algid == 0xAA || state->payload_algid == 0x81 || state->payload_algid == 0x9F)) {
        const unsigned int key_width = (state->payload_algid == 0xAA) ? 10U : 16U;
        char key_text[17];
        DSD_FPRINTF(stderr, " Key: %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->R, key_width, 0));
        return;
    }

    if (state->p25_crypto_state[0] == DSD_P25_CRYPTO_DECRYPTABLE
        && (state->payload_algid == 0x83 || state->payload_algid == 0x84 || state->payload_algid == 0x89)) {
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "%s", KYEL);
        const unsigned long long segments[4] = {state->A1[0], state->A2[0], state->A3[0], state->A4[0]};
        char key_text[68];
        const unsigned int segment_count =
            (state->payload_algid == 0x83) ? 3U : ((state->payload_algid == 0x84) ? 4U : 2U);
        DSD_FPRINTF(
            stderr, "Key: %s ",
            dsd_secret_format_u64_segments(key_text, sizeof key_text, opts->show_keys, segments, segment_count));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
}

static void
hdu_handle_good_decode(dsd_opts* opts, dsd_state* state, int algidhex, int kidhex, unsigned long long int mihex1,
                       unsigned long long int mihex2, unsigned long long int mihex3) {
    const uint64_t mi = (mihex1 << 32) | mihex2;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " HDU  ALG ID: 0x%02X KEY ID: 0x%04X MI: 0x%08llX%08llX", algidhex, kidhex, mihex1, mihex2);
    if (mihex3) {
        DSD_FPRINTF(stderr, "-%02llX", mihex3);
    }

    if (algidhex != 0x80 && algidhex != 0x0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " ENC");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    DSD_FPRINTF(stderr, "\n");

    state->xl_is_hdu = 1;
    hdu_maybe_enc_lockout(opts, state, algidhex, kidhex, mi);
    hdu_report_decryption_key(opts, state);
    DSD_FPRINTF(stderr, "%s", KNRM);

    if (state->payload_algid == 0x84 || state->payload_algid == 0x89) {
        LFSR128(state);
        DSD_FPRINTF(stderr, "\n");
    }
}

/**
 * The important method that processes a full P25 HD unit.
 */
void
processHDU(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_hdu++;

    // Start status-symbol collection unless the dispatcher already did so for this data unit.
    p25_status_accum_ensure_started(state);

    // Defer last_vc_sync_time refresh until after FEC success to avoid
    // extending hangtime due to false HDU decodes during signal loss.

    //push current slot to 0, just in case swapping p2 to p1
    //or stale slot value from p2 and then decoding a pdu
    state->currentslot = 0;

    uint8_t mi[73];
    char algid[9], kid[17];
    int algidhex, kidhex;
    int status_count;
    unsigned long long int mihex1, mihex2, mihex3;
    char hex_data[20][6];   // Data in hex-words (6 bit words). A total of 20 hex words.
    char hex_parity[16][6]; // Parity of the data, again in hex-word format. A total of 16 parity hex words.

    int irrecoverable_errors;

    P25P1SoftDibit soft_dibits[20 * (3 + 6) + 16 * (3 + 6)] = {0};
    int soft_dibit_index;

    soft_dibit_index = 0;

    // we skip the status dibits that occur every 36 symbols
    // the next status symbol comes in 14 dibits from here
    // so we start counter at 36-14-1 = 21
    status_count = 21;

    irrecoverable_errors =
        hdu_read_and_fec(opts, state, hex_data, hex_parity, soft_dibits, &status_count, &soft_dibit_index);

    // Now put the corrected data on the DSD structures

    hdu_extract_mi_algid_kid((const char (*)[6])hex_data, mi, algid, kid);

    uint32_t kid_parsed = 0;
    state->p25kid = (dsd_parse_binary_u32_n(kid, 16, &kid_parsed) == 0) ? (int)kid_parsed : 0;
    hdu_consume_trailing_dibits_and_status(opts, state);

    uint32_t algid_parsed = 0;
    algidhex = (dsd_parse_binary_u32_n(algid, 8, &algid_parsed) == 0) ? (int)algid_parsed : 0;
    kidhex = (dsd_parse_binary_u32_n(kid, 16, &kid_parsed) == 0) ? (int)kid_parsed : 0;
    mihex1 = (unsigned long long int)convert_bits_into_output(&mi[0], 32);
    mihex2 = (unsigned long long int)convert_bits_into_output(&mi[32], 32);
    mihex3 = (unsigned long long int)convert_bits_into_output(&mi[64], 8);

    //reset dropbytes - skip first 11 for LCW
    state->dropL = 267;

    //set vc counter to 0
    state->p25vc = 0;

    if (irrecoverable_errors == 0) {
        hdu_handle_good_decode(opts, state, algidhex, kidhex, mihex1, mihex2, mihex3);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " HDU FEC ERR \n");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    // Classify accumulated status symbols and set advisory AFC gate flag.
    p25_status_accum_classify(state, opts);

    //reset gain
    if (opts->floating_point == 1) {
        state->aout_gain = opts->audio_gain;
    }
}
