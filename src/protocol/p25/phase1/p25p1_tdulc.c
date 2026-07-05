// SPDX-License-Identifier: ISC
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

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p1_check_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
soft_abs_i16(int16_t v) {
    return v < 0 ? -(int)v : (int)v;
}

// Uncomment for some verbose debug info
//#define TDULC_DEBUG

/**
 * Reverse the order of bits in a 12-bit word. We need this to accommodate to the expected bit order in
 * some algorithms.
 * \param dodeca The 12-bit word to reverse.
 */
static void
swap_hex_words_bits(char* dodeca) {
    int j;
    for (j = 0; j < 6; j++) {
        unsigned char swap;
        swap = (unsigned char)dodeca[j];
        dodeca[j] = dodeca[j + 6];
        dodeca[j + 6] = swap;
    }
}

/**
 * Reverse the order of bits in a sequence of six pairs of 12-bit words and their parities.
 * \param dodeca_data Pointer to the start of the 12-bit words sequence.
 * \param dodeca_parity Pointer to the parities sequence.
 */
static void
swap_hex_words(char* dodeca_data, char* dodeca_parity) {
    int i;
    for (i = 0; i < 6; i++) {
        swap_hex_words_bits(dodeca_data + ((size_t)i * 12));
        swap_hex_words_bits(dodeca_parity + ((size_t)i * 12));
    }
}

static void
tdulc_read_word_with_parity(dsd_opts* opts, dsd_state* state, char* dodeca, char parity[12], int* status_count,
                            P25P1SoftDibit* soft_dibits, int* soft_dibit_index) {
    read_word(opts, state, dodeca, 12, status_count, soft_dibits, soft_dibit_index);
    read_golay24_parity(opts, state, parity, status_count, soft_dibits, soft_dibit_index);
}

static void
tdulc_collect_golay_reliability(const P25P1SoftDibit* sig, int reliab[24]) {
    int idx = 0;
    for (int d = 0; d < 6; d++) {
        reliab[idx++] = soft_abs_i16(sig[d].llr[0]);
        reliab[idx++] = soft_abs_i16(sig[d].llr[1]);
    }
    for (int d = 6; d < 12; d++) {
        reliab[idx++] = soft_abs_i16(sig[d].llr[0]);
        reliab[idx++] = soft_abs_i16(sig[d].llr[1]);
    }
}

static int
tdulc_try_soft_recover_golay(dsd_state* state, char* dodeca, const char raw_dodeca[12], const char raw_parity[12],
                             const P25P1SoftDibit* sig, int irrecoverable_errors, int fixed_errors) {
    if ((irrecoverable_errors == 0 && fixed_errors == 0) || sig == NULL) {
        return irrecoverable_errors;
    }

    int reliab[24];
    int soft_fixed = 0;
    char soft_dodeca[12];
    char soft_parity[12];
    tdulc_collect_golay_reliability(sig, reliab);
    DSD_MEMCPY(soft_dodeca, raw_dodeca, sizeof(soft_dodeca));
    DSD_MEMCPY(soft_parity, raw_parity, sizeof(soft_parity));
    int soft_result = check_and_fix_golay_24_12_soft(soft_dodeca, soft_parity, reliab, &soft_fixed);
    if (soft_result != 0) {
        return irrecoverable_errors;
    }

    DSD_MEMCPY(dodeca, soft_dodeca, sizeof(soft_dodeca));
    if (irrecoverable_errors != 0) {
        state->p25_p1_soft_golay_ok++;
        state->debug_header_errors += soft_fixed;
    }
    return 0;
}

/**
 * Read a dodeca (12-bit) word, its parity bits and attempts to error correct it using the Golay(24,12) algorithm.
 * Uses soft decode if hard decode fails and reliability info is available.
 */
static void
read_and_correct_dodeca_word(dsd_opts* opts, dsd_state* state, char* dodeca, int* status_count,
                             P25P1SoftDibit* soft_dibits, int* soft_dibit_index) {
    char parity[12];
    int fixed_errors;
    int irrecoverable_errors;

    int start_index = *soft_dibit_index;
    tdulc_read_word_with_parity(opts, state, dodeca, parity, status_count, soft_dibits, soft_dibit_index);
    char raw_dodeca[12];
    char raw_parity[12];
    DSD_MEMCPY(raw_dodeca, dodeca, sizeof(raw_dodeca));
    DSD_MEMCPY(raw_parity, parity, sizeof(raw_parity));

#ifdef TDULC_DEBUG
    DSD_FPRINTF(stderr, "[");
    for (int i = 0; i < 12; i++) {
        DSD_FPRINTF(stderr, "%c", (dodeca[i] == 1) ? 'X' : ' ');
    }
    DSD_FPRINTF(stderr, "-");
    for (int i = 0; i < 12; i++) {
        DSD_FPRINTF(stderr, "%c", (parity[i] == 1) ? 'X' : ' ');
    }
    DSD_FPRINTF(stderr, "]");
#endif

    // Use extended golay to error correct the dodeca word
    irrecoverable_errors = check_and_fix_golay_24_12(dodeca, parity, &fixed_errors);

    state->debug_header_errors += fixed_errors;

    const P25P1SoftDibit* sig = (soft_dibits != NULL) ? &soft_dibits[start_index] : NULL;
    irrecoverable_errors =
        tdulc_try_soft_recover_golay(state, dodeca, raw_dodeca, raw_parity, sig, irrecoverable_errors, fixed_errors);

    if (irrecoverable_errors != 0) {
        state->debug_header_critical_errors++;
    }

#ifdef TDULC_DEBUG
    DSD_FPRINTF(stderr, " -> [");
    for (int i = 0; i < 12; i++) {
        DSD_FPRINTF(stderr, "%c", (dodeca[i] == 1) ? 'X' : ' ');
    }
    DSD_FPRINTF(stderr, "]");
    if (irrecoverable_errors == 0) {
        if (fixed_errors > 0) {
            DSD_FPRINTF(stderr, " fixed!");
        }
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " IRRECOVERABLE");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
    DSD_FPRINTF(stderr, "\n");
#endif
}

static uint8_t
dodeca_half_reliability(const P25P1SoftDibit* word, int half) {
    int16_t llr[6];
    int dibit_offset = half == 0 ? 0 : 3;

    for (int i = 0; i < 3; i++) {
        llr[(i * 2) + 0] = word[dibit_offset + i].llr[0];
        llr[(i * 2) + 1] = word[dibit_offset + i].llr[1];
    }
    return p25p1_llr_reliability(llr, 6);
}

static void
build_tdulc_rs_reliability(const P25P1SoftDibit* soft_dibits, uint8_t data_reliab[12], uint8_t parity_reliab[12]) {
    const P25P1SoftDibit* data_word = soft_dibits + 60;
    for (int i = 0; i < 6; i++, data_word -= 12) {
        const P25P1SoftDibit* word = data_word;
        data_reliab[(i * 2) + 0] = dodeca_half_reliability(word, 1);
        data_reliab[(i * 2) + 1] = dodeca_half_reliability(word, 0);
    }
    const P25P1SoftDibit* parity_word = soft_dibits + 132;
    for (int i = 0; i < 6; i++, parity_word -= 12) {
        const P25P1SoftDibit* word = parity_word;
        parity_reliab[(i * 2) + 0] = dodeca_half_reliability(word, 1);
        parity_reliab[(i * 2) + 1] = dodeca_half_reliability(word, 0);
    }
}

void
read_zeros(dsd_opts* opts, dsd_state* state, unsigned int length, int* status_count) {
    for (unsigned int i = 0; i < length; i += 2) {
        if (*status_count == 35) {
            dsd_dibit_soft_t status_soft;
            int status_dibit = getDibitSoft(opts, state, &status_soft);
            p25_status_accum_add(state, status_dibit);
            *status_count = 1;
        } else {
            (*status_count)++;
        }

        dsd_dibit_soft_t soft;
        (void)getDibitSoft(opts, state, &soft);
    }
}

static void
tdulc_read_data_and_parity_words(dsd_opts* opts, dsd_state* state, char dodeca_data[6][12], char dodeca_parity[6][12],
                                 int* status_count, P25P1SoftDibit* soft_dibits, int* soft_dibit_index) {
    for (int i = 5; i >= 0; i--) {
        read_and_correct_dodeca_word(opts, state, &(dodeca_data[i][0]), status_count, soft_dibits, soft_dibit_index);
    }
    for (int i = 5; i >= 0; i--) {
        read_and_correct_dodeca_word(opts, state, &(dodeca_parity[i][0]), status_count, soft_dibits, soft_dibit_index);
    }
}

static int
tdulc_recover_reedsolomon(dsd_state* state, char dodeca_data[6][12], char dodeca_parity[6][12],
                          const P25P1SoftDibit* soft_dibits) {
    swap_hex_words((char*)dodeca_data, (char*)dodeca_parity);
    int irrecoverable_errors = check_and_fix_reedsolomon_24_12_13((char*)dodeca_data, (char*)dodeca_parity);
    if (irrecoverable_errors == 1) {
        uint8_t data_reliab[12];
        uint8_t parity_reliab[12];
        build_tdulc_rs_reliability(soft_dibits, data_reliab, parity_reliab);
        if (p25p1_rs_24_12_13_soft_reliability((char*)dodeca_data, (char*)dodeca_parity, data_reliab, parity_reliab)
            == 0) {
            state->p25_p1_soft_rs_ok++;
            irrecoverable_errors = 0;
        }
    }
    swap_hex_words((char*)dodeca_data, (char*)dodeca_parity);
    return irrecoverable_errors;
}

static void
tdulc_apply_recovery_result(dsd_state* state, int irrecoverable_errors) {
    if (irrecoverable_errors == 1) {
        state->p25_p1_voice_fec_err++;
        state->debug_header_critical_errors++;
        return;
    }

    state->p25_p1_voice_fec_ok++;
}

static void
tdulc_finalize_tail_symbols(dsd_opts* opts, dsd_state* state, int* status_count) {
    read_zeros(opts, state, 20, status_count);

    state->p25_p1_last_tdu = time(NULL);
    state->p25_p1_last_tdu_m = dsd_time_now_monotonic_s();
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
    if (*status_count != 35) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "*** SYNC ERROR\n");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    dsd_dibit_soft_t status_soft;
    int ss = getDibitSoft(opts, state, &status_soft);
    p25_status_accum_add(state, ss);
}

static void
tdulc_build_lcw_payload(const char dodeca_data[6][12], uint8_t LCW_bytes[9], uint8_t LCW_bits[72]) {
    DSD_MEMSET(LCW_bytes, 0, 9);
    for (int bit_idx = 0; bit_idx < 72; bit_idx++) {
        int word = 5 - (bit_idx / 12);
        int bit_in_word = bit_idx % 12;
        uint8_t bit = (uint8_t)(dodeca_data[word][bit_in_word] & 0x01);
        LCW_bits[bit_idx] = bit;
        LCW_bytes[bit_idx / 8] |= (uint8_t)(bit << (7 - (bit_idx % 8)));
    }
}

static void
tdulc_print_lcw_payload(const dsd_opts* opts, const uint8_t LCW_bytes[9]) {
    if (opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, " P25 LCW Payload ");
    for (int i = 0; i < 9; i++) {
        DSD_FPRINTF(stderr, "[%02X]", LCW_bytes[i]);
    }
    DSD_FPRINTF(stderr, "%s\n", KNRM);
}

void
processTDULC(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_tdulc++;
    p25_status_accum_ensure_started(state);

    state->currentslot = 0;
    p25_sm_emit_idle(opts, state, 0);

    state->p25_call_emergency[0] = 0;
    state->p25_call_priority[0] = 0;
    state->p25_call_is_packet[0] = 0;
    state->dmr_so = 0;
    state->p25_service_options_valid[0] = 0;

    char dodeca_data[6][12];
    char dodeca_parity[6][12];
    P25P1SoftDibit soft_dibits[6 * (6 + 6) + 6 * (6 + 6)] = {0};
    uint8_t LCW_bytes[9];
    uint8_t LCW_bits[72];
    int soft_dibit_index = 0;
    int status_count = 21;
    tdulc_read_data_and_parity_words(opts, state, dodeca_data, dodeca_parity, &status_count, soft_dibits,
                                     &soft_dibit_index);

    int irrecoverable_errors = tdulc_recover_reedsolomon(state, dodeca_data, dodeca_parity, soft_dibits);
    tdulc_apply_recovery_result(state, irrecoverable_errors);
    tdulc_finalize_tail_symbols(opts, state, &status_count);
    tdulc_build_lcw_payload((const char (*)[12])dodeca_data, LCW_bytes, LCW_bits);

    if (irrecoverable_errors == 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        p25_lcw(opts, state, LCW_bits, 0);
        DSD_FPRINTF(stderr, "%s", KNRM);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " LCW FEC ERR ");
        DSD_FPRINTF(stderr, "%s\n", KNRM);
    }
    tdulc_print_lcw_payload(opts, LCW_bytes);
    DSD_SNPRINTF(state->call_string[0], sizeof(state->call_string[0]), "%s", "                     ");
    DSD_SNPRINTF(state->call_string[1], sizeof(state->call_string[1]), "%s", "                     ");
    if (opts->floating_point == 1) {
        state->aout_gain = opts->audio_gain;
    }
    p25_status_accum_classify(state, opts);
}
