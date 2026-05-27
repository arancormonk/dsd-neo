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
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/p25p1_heuristics.h>
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
#include <stdlib.h>
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
                            AnalogSignal* analog_signal_array, int* analog_signal_index) {
    read_word(opts, state, dodeca, 12, status_count, analog_signal_array, analog_signal_index);
    read_golay24_parity(opts, state, parity, status_count, analog_signal_array, analog_signal_index);
}

static void
tdulc_collect_golay_reliability(const AnalogSignal* sig, int reliab[24]) {
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
                             const AnalogSignal* sig, int irrecoverable_errors, int fixed_errors) {
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
                             AnalogSignal* analog_signal_array, int* analog_signal_index) {
    char parity[12];
    int fixed_errors;
    int irrecoverable_errors;

    int start_index = *analog_signal_index;
    tdulc_read_word_with_parity(opts, state, dodeca, parity, status_count, analog_signal_array, analog_signal_index);
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

    const AnalogSignal* sig = (analog_signal_array != NULL) ? &analog_signal_array[start_index] : NULL;
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
dodeca_half_reliability(const AnalogSignal* word, int half) {
    int16_t llr[6];
    int dibit_offset = half == 0 ? 0 : 3;

    for (int i = 0; i < 3; i++) {
        llr[(i * 2) + 0] = word[dibit_offset + i].llr[0];
        llr[(i * 2) + 1] = word[dibit_offset + i].llr[1];
    }
    return p25p1_llr_reliability(llr, 6);
}

static void
build_tdulc_rs_reliability(const AnalogSignal* analog_signal_array, uint8_t data_reliab[12],
                           uint8_t parity_reliab[12]) {
    const AnalogSignal* data_word = analog_signal_array + 60;
    for (int i = 0; i < 6; i++, data_word -= 12) {
        const AnalogSignal* word = data_word;
        data_reliab[(i * 2) + 0] = dodeca_half_reliability(word, 1);
        data_reliab[(i * 2) + 1] = dodeca_half_reliability(word, 0);
    }
    const AnalogSignal* parity_word = analog_signal_array + 132;
    for (int i = 0; i < 6; i++, parity_word -= 12) {
        const AnalogSignal* word = parity_word;
        parity_reliab[(i * 2) + 0] = dodeca_half_reliability(word, 1);
        parity_reliab[(i * 2) + 1] = dodeca_half_reliability(word, 0);
    }
}

/**
 * Correct the information in analog_signal_array according with the content of data, which has been
 * error corrected and should be valid.
 * \param data A sequence of 12-bit words.
 * \param count Number of words in the sequence.
 * \param analog_signal_array Pointer to a sequence of AnalogSignal elements, as many as the value of count.
 */
static void
correct_golay_dibits_12(const char* data, int count, AnalogSignal* analog_signal_array) {
    int i, j;
    int analog_signal_index;
    int dibit;
    char parity[12];

    analog_signal_index = 0;

    for (i = count - 1; i >= 0; i--) {
        for (j = 0; j < 12; j += 2) // 6 iterations -> 6 dibits
        {
            dibit = (data[i * 12 + j] << 1) | data[i * 12 + j + 1];
            analog_signal_array[analog_signal_index].corrected_dibit = dibit;

#ifdef HEURISTICS_DEBUG
            if (analog_signal_array[analog_signal_index].dibit != dibit) {
                DSD_FPRINTF(stderr, "TDULC data word corrected from %i to %i, analog value %i\n",
                            analog_signal_array[analog_signal_index].dibit, dibit,
                            analog_signal_array[analog_signal_index].value);
            }
#endif

            analog_signal_index++;
        }

        // Calculate the golay parity for the hex word
        encode_golay_24_12(data + ((size_t)i * 12), parity);

        for (j = 0; j < 12; j += 2) // 6 iterations -> 6 dibits
        {
            dibit = (parity[j] << 1) | parity[j + 1];
            analog_signal_array[analog_signal_index].corrected_dibit = dibit;

#ifdef HEURISTICS_DEBUG
            if (analog_signal_array[analog_signal_index].dibit != dibit) {
                DSD_FPRINTF(stderr, "TDULC parity corrected from %i to %i, analog value %i\n",
                            analog_signal_array[analog_signal_index].dibit, dibit,
                            analog_signal_array[analog_signal_index].value);
            }
#endif

            analog_signal_index++;
        }
    }
}

void
read_zeros(dsd_opts* opts, dsd_state* state, AnalogSignal* analog_signal_array, unsigned int length, int* status_count,
           int new_sequence) {
    char* buffer;
    unsigned int i;
    int analog_signal_index;

    analog_signal_index = 0;
    buffer = malloc(length);
    read_dibit_update_analog_data(opts, state, buffer, length, status_count, analog_signal_array, &analog_signal_index);
    free(buffer);
    if (new_sequence) {
        analog_signal_array[0].sequence_broken = 1;
    }

    for (i = 0; i < length / 2; i++) {
        analog_signal_array[i].corrected_dibit = 0;
#ifdef HEURISTICS_DEBUG
        if (analog_signal_array[i].corrected_dibit != analog_signal_array[i].dibit) {
            DSD_FPRINTF(stderr, "TDULC ending zeros corrected from %i to %i, analog value %i\n",
                        analog_signal_array[i].dibit, 0, analog_signal_array[i].value);
        }
#endif
    }

    // We know that all these bits should be zero. Use this information for the heuristics module
    P25Heuristics* heur = (state->synctype == DSD_SYNC_P25P1_NEG) ? &state->inv_p25_heuristics : &state->p25_heuristics;
    contribute_to_heuristics(state->rf_mod, heur, analog_signal_array, length / 2);
}

static void
tdulc_read_data_and_parity_words(dsd_opts* opts, dsd_state* state, char dodeca_data[6][12], char dodeca_parity[6][12],
                                 int* status_count, AnalogSignal* analog_signal_array, int* analog_signal_index) {
    for (int i = 5; i >= 0; i--) {
        read_and_correct_dodeca_word(opts, state, &(dodeca_data[i][0]), status_count, analog_signal_array,
                                     analog_signal_index);
    }
    for (int i = 5; i >= 0; i--) {
        read_and_correct_dodeca_word(opts, state, &(dodeca_parity[i][0]), status_count, analog_signal_array,
                                     analog_signal_index);
    }
}

static int
tdulc_recover_reedsolomon(dsd_state* state, char dodeca_data[6][12], char dodeca_parity[6][12],
                          const AnalogSignal* analog_signal_array) {
    swap_hex_words((char*)dodeca_data, (char*)dodeca_parity);
    int irrecoverable_errors = check_and_fix_reedsolomon_24_12_13((char*)dodeca_data, (char*)dodeca_parity);
    if (irrecoverable_errors == 1) {
        uint8_t data_reliab[12];
        uint8_t parity_reliab[12];
        build_tdulc_rs_reliability(analog_signal_array, data_reliab, parity_reliab);
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
tdulc_apply_recovery_result(dsd_state* state, P25Heuristics* heur, char dodeca_data[6][12], char dodeca_parity[6][12],
                            AnalogSignal* analog_signal_array, int irrecoverable_errors) {
    if (irrecoverable_errors == 1) {
        state->p25_p1_voice_fec_err++;
        state->debug_header_critical_errors++;
        update_error_stats(heur, 12 * 6 + 12 * 6, 7 * 4);
        return;
    }

    state->p25_p1_voice_fec_ok++;

    char fixed_parity[6 * 12];
    correct_golay_dibits_12((char*)dodeca_data, 6, analog_signal_array);
    swap_hex_words((char*)dodeca_data, (char*)dodeca_parity);
    encode_reedsolomon_24_12_13((char*)dodeca_data, fixed_parity);
    swap_hex_words((char*)dodeca_data, fixed_parity);
    correct_golay_dibits_12(fixed_parity, 6, analog_signal_array + ((size_t)6) * (6 + 6));

    analog_signal_array[0].sequence_broken = 1;
    size_t trusted = ((size_t)6) * (6 + 6) + ((size_t)6) * (6 + 6);
    contribute_to_heuristics(state->rf_mod, heur, analog_signal_array, (int)trusted);
}

static void
tdulc_finalize_tail_symbols(dsd_opts* opts, dsd_state* state, AnalogSignal* analog_signal_array, int* status_count,
                            int irrecoverable_errors) {
    read_zeros(opts, state, analog_signal_array + ((size_t)6) * (6 + 6) + ((size_t)6) * (6 + 6), 20, status_count,
               irrecoverable_errors);

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

    P25Heuristics* heur = (state->synctype == DSD_SYNC_P25P1_NEG) ? &state->inv_p25_heuristics : &state->p25_heuristics;

    state->currentslot = 0;
    p25_sm_emit_idle(opts, state, 0);

    state->p25_call_emergency[0] = 0;
    state->p25_call_priority[0] = 0;
    state->p25_call_is_packet[0] = 0;

    char dodeca_data[6][12];
    char dodeca_parity[6][12];
    AnalogSignal analog_signal_array[6 * (6 + 6) + 6 * (6 + 6) + 10] = {0};
    uint8_t LCW_bytes[9];
    uint8_t LCW_bits[72];
    int analog_signal_index = 0;
    int status_count = 21;
    tdulc_read_data_and_parity_words(opts, state, dodeca_data, dodeca_parity, &status_count, analog_signal_array,
                                     &analog_signal_index);

    int irrecoverable_errors = tdulc_recover_reedsolomon(state, dodeca_data, dodeca_parity, analog_signal_array);
    tdulc_apply_recovery_result(state, heur, dodeca_data, dodeca_parity, analog_signal_array, irrecoverable_errors);
    tdulc_finalize_tail_symbols(opts, state, analog_signal_array, &status_count, irrecoverable_errors);
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
