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

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/p25p1_heuristics.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_lfsr.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p1_check_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <stddef.h>
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

static int
get_dibit_analog_and_soft(dsd_opts* opts, dsd_state* state, int* analog, dsd_dibit_soft_t* soft) {
    int dibit = get_dibit_and_analog_signal(opts, state, analog);

    if (soft != NULL) {
        int found = 0;
        if (state->dmr_soft_p != NULL && state->dmr_soft_buf != NULL) {
            const dsd_dibit_soft_t* sp = state->dmr_soft_p - 1;
            if (sp >= state->dmr_soft_buf + 200 && sp < state->dmr_soft_buf + 1000000) {
                *soft = *sp;
                found = 1;
            }
        }
        if (!found) {
            uint8_t r = 255;
            if (state->dmr_reliab_p != NULL && state->dmr_reliab_buf != NULL) {
                const uint8_t* rp = state->dmr_reliab_p - 1;
                if (rp >= state->dmr_reliab_buf + 200 && rp < state->dmr_reliab_buf + 1000000) {
                    r = *rp;
                }
            }
            soft->reliability = r;
            soft->llr[0] = (int16_t)(((dibit >> 1) & 1) ? r : -(int)r);
            soft->llr[1] = (int16_t)((dibit & 1) ? r : -(int)r);
        }
    }

    return dibit;
}

int
read_dibit_soft(dsd_opts* opts, dsd_state* state, char* output, int* status_count, int* analog_signal,
                int* did_read_status, int* reliab, int16_t llr[2]) {
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
        if (did_read_status != NULL) {
            *did_read_status = 1;
        }
        *status_count = 1;

#ifdef TRACE_DSD
        state->debug_prefix = prev_prefix;
#endif

    } else {
        if (did_read_status != NULL) {
            *did_read_status = 0;
        }
        (*status_count)++;
    }

    dibit = get_dibit_analog_and_soft(opts, state, analog_signal, &soft);
    if (reliab != NULL) {
        *reliab = soft.reliability;
    }
    if (llr != NULL) {
        llr[0] = soft.llr[0];
        llr[1] = soft.llr[1];
    }
    output[0] = (1 & (dibit >> 1)); // bit 1
    output[1] = (1 & dibit);        // bit 0

    return dibit;
}

int
read_dibit(dsd_opts* opts, dsd_state* state, char* output, int* status_count, int* analog_signal, int* did_read_status,
           int* reliab) {
    return read_dibit_soft(opts, state, output, status_count, analog_signal, did_read_status, reliab, NULL);
}

void
read_dibit_update_analog_data(dsd_opts* opts, dsd_state* state, char* buffer, unsigned int count, int* status_count,
                              AnalogSignal* analog_signal_array, int* analog_signal_index) {
    unsigned int i;

    for (i = 0; i < count; i += 2) {
        // We read two bits on each call
        int analog_signal;
        int did_read_status;
        int reliab;
        int16_t llr[2];
        int dibit;

        dibit = read_dibit_soft(opts, state, buffer + i, status_count, &analog_signal, &did_read_status, &reliab, llr);

        if (analog_signal_array != NULL) {
            // Fill up the AnalogSignal struct
            analog_signal_array[*analog_signal_index].value = analog_signal;
            analog_signal_array[*analog_signal_index].dibit = dibit;
            analog_signal_array[*analog_signal_index].sequence_broken = did_read_status;
            analog_signal_array[*analog_signal_index].reliab = reliab;
            analog_signal_array[*analog_signal_index].llr[0] = llr[0];
            analog_signal_array[*analog_signal_index].llr[1] = llr[1];
            (*analog_signal_index)++;
        }
    }
}

void
read_word(dsd_opts* opts, dsd_state* state, char* word, unsigned int length, int* status_count,
          AnalogSignal* analog_signal_array, int* analog_signal_index) {
    read_dibit_update_analog_data(opts, state, word, length, status_count, analog_signal_array, analog_signal_index);
}

void
read_golay24_parity(dsd_opts* opts, dsd_state* state, char* parity, int* status_count,
                    AnalogSignal* analog_signal_array, int* analog_signal_index) {
    read_dibit_update_analog_data(opts, state, parity, 12, status_count, analog_signal_array, analog_signal_index);
}

void
read_hamm_parity(dsd_opts* opts, dsd_state* state, char* parity, int* status_count, AnalogSignal* analog_signal_array,
                 int* analog_signal_index) {
    // Read 2 dibits = read 4 bits.
    read_dibit_update_analog_data(opts, state, parity, 4, status_count, analog_signal_array, analog_signal_index);
}

/**
 * Corrects a hex (6 bit) word using the Golay 24 FEC.
 * Uses soft decode if hard decode fails and reliability info is available.
 *
 * @param state Decoder state for error tracking.
 * @param hex   The 6-bit data word (modified in place).
 * @param parity The 12-bit parity word.
 * @param analog_signal_array AnalogSignal array for this hex word (9 dibits: 3 data + 6 parity).
 *                            May be NULL to disable soft decode.
 */
static void
correct_hex_word(dsd_opts* opts, dsd_state* state, char* hex, char* parity, const AnalogSignal* analog_signal_array) {
    (void)opts;
    int fixed_errors;
    int irrecoverable_errors;
    char raw_hex[6];
    char raw_parity[12];
    DSD_MEMCPY(raw_hex, hex, sizeof(raw_hex));
    DSD_MEMCPY(raw_parity, parity, sizeof(raw_parity));

    irrecoverable_errors = check_and_fix_golay_24_6(hex, parity, &fixed_errors);

    state->debug_header_errors += fixed_errors;

    if ((irrecoverable_errors != 0 || fixed_errors > 0) && analog_signal_array != NULL) {
        /* Hard decode failed or corrected low-confidence bits; try soft decode using reliability info.
         * The analog_signal_array contains 9 dibits:
         *   [0..2] = 3 dibits for 6 data bits
         *   [3..8] = 6 dibits for 12 parity bits
         * Extract per-bit reliability by taking dibit reliability for both bits.
         */
        int reliab[18];
        int idx = 0;

        /* Data bits: 3 dibits -> 6 bits */
        for (int d = 0; d < 3; d++) {
            reliab[idx++] = soft_abs_i16(analog_signal_array[d].llr[0]);
            reliab[idx++] = soft_abs_i16(analog_signal_array[d].llr[1]);
        }
        /* Parity bits: 6 dibits -> 12 bits */
        for (int d = 3; d < 9; d++) {
            reliab[idx++] = soft_abs_i16(analog_signal_array[d].llr[0]);
            reliab[idx++] = soft_abs_i16(analog_signal_array[d].llr[1]);
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
rs_hex_symbol_reliability(const AnalogSignal* symbol) {
    int16_t llr[6];

    for (int i = 0; i < 3; i++) {
        llr[(i * 2) + 0] = symbol[i].llr[0];
        llr[(i * 2) + 1] = symbol[i].llr[1];
    }
    return p25p1_llr_reliability(llr, 6);
}

static void
build_hdu_rs_reliability(const AnalogSignal* analog_signal_array, uint8_t data_reliab[20], uint8_t parity_reliab[16]) {
    for (int i = 0; i < 20; i++) {
        int analog_index = (19 - i) * (3 + 6);
        data_reliab[i] = rs_hex_symbol_reliability(analog_signal_array + analog_index);
    }
    for (int i = 0; i < 16; i++) {
        int analog_index = (20 * (3 + 6)) + ((15 - i) * (3 + 6));
        parity_reliab[i] = rs_hex_symbol_reliability(analog_signal_array + analog_index);
    }
}

/**
 * Reads an hex word, its parity bits and attempts to error correct it using the Golay24 algorithm.
 */
static void
read_and_correct_hex_word(dsd_opts* opts, dsd_state* state, char* hex, int* status_count,
                          AnalogSignal* analog_signal_array, int* analog_signal_index) {
    char parity[12];

    /* Remember where this hex word's analog signals start */
    int start_index = *analog_signal_index;

    // Read the hex word
    read_word(opts, state, hex, 6, status_count, analog_signal_array, analog_signal_index);
    // Read the parity
    read_golay24_parity(opts, state, parity, status_count, analog_signal_array, analog_signal_index);

    // Use the Golay24 FEC to correct it. This call modifies the content of hex to fix it, hopefully.
    // Pass the analog signal array starting at this hex word for soft decode support.
    const AnalogSignal* hex_analog = (analog_signal_array != NULL) ? &analog_signal_array[start_index] : NULL;
    correct_hex_word(opts, state, hex, parity, hex_analog);
}

/**
 * Uses the information from a corrected sequence of hex words to update the AnalogSignal data.
 * The proper Golay 24 parity is calculated from the corrected hex word so we can also fix the Golay parity
 * that we read originally from the signal.
 * \param corrected_hex_data Pointer to a sequence of hex words that has been error corrected and therefore
 * we trust it's correct. Typically this are hex words that has been decoded successfully using a
 * Reed-Solomon variant.
 * \param hex_count The number of hex words in the sequence.
 * \param analog_signal_array A pointer to the AnalogSignal information for the sequence of hex words.
 */
static void
correct_golay_dibits_6(const char* corrected_hex_data, int hex_count, AnalogSignal* analog_signal_array) {
    int i, j;
    int analog_signal_index;
    int dibit;
    char parity[12];

    analog_signal_index = 0;

    for (i = hex_count - 1; i >= 0; i--) {
        for (j = 0; j < 6; j += 2) // 3 iterations -> 3 dibits
        {
            // Given the bits, calculates the dibit
            dibit = (corrected_hex_data[i * 6 + j] << 1) | corrected_hex_data[i * 6 + j + 1];
            // Now we know the dibit we should have read from the signal
            analog_signal_array[analog_signal_index].corrected_dibit = dibit;

#ifdef HEURISTICS_DEBUG
            if (analog_signal_array[analog_signal_index].dibit != dibit) {
                DSD_FPRINTF(stderr, "HDU data word corrected from %i to %i, analog value %i\n",
                            analog_signal_array[analog_signal_index].dibit, dibit,
                            analog_signal_array[analog_signal_index].value);
            }
#endif

            analog_signal_index++;
        }

        // Calculate the Golay 24 parity for the corrected hex word
        ptrdiff_t off = (ptrdiff_t)i * 6;
        encode_golay_24_6(corrected_hex_data + off, parity);

        // Now we know the parity we should have read from the signal. Use this information
        for (j = 0; j < 12; j += 2) // 6 iterations -> 6 dibits
        {
            // Given the bits, calculates the dibit
            dibit = (parity[j] << 1) | parity[j + 1];
            // Now we know the dibit we should have read from the signal
            analog_signal_array[analog_signal_index].corrected_dibit = dibit;

#ifdef HEURISTICS_DEBUG
            if (analog_signal_array[analog_signal_index].dibit != dibit) {
                DSD_FPRINTF(stderr, "HDU parity corrected from %i to %i, analog value %i\n",
                            analog_signal_array[analog_signal_index].dibit, dibit,
                            analog_signal_array[analog_signal_index].value);
            }
#endif

            analog_signal_index++;
        }
    }
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
hdu_read_and_fec(dsd_opts* opts, dsd_state* state, P25Heuristics* heur, char hex_data[20][6], char hex_parity[16][6],
                 AnalogSignal analog_signal_array[20 * (3 + 6) + 16 * (3 + 6)], int* status_count,
                 int* analog_signal_index) {
    char hex[6];

    for (int i = 19; i >= 0; i--) {
        read_and_correct_hex_word(opts, state, hex, status_count, analog_signal_array, analog_signal_index);
        for (int j = 0; j < 6; j++) {
            hex_data[i][j] = hex[j];
        }
    }

    for (int i = 15; i >= 0; i--) {
        read_and_correct_hex_word(opts, state, hex, status_count, analog_signal_array, analog_signal_index);
        for (int j = 0; j < 6; j++) {
            hex_parity[i][j] = hex[j];
        }
    }
    analog_signal_array[0].sequence_broken = 1;

    int irrecoverable_errors = check_and_fix_redsolomon_36_20_17((char*)hex_data, (char*)hex_parity);
    if (irrecoverable_errors != 0) {
        uint8_t data_reliab[20];
        uint8_t parity_reliab[16];
        build_hdu_rs_reliability(analog_signal_array, data_reliab, parity_reliab);
        if (p25p1_rs_36_20_17_soft_reliability((char*)hex_data, (char*)hex_parity, data_reliab, parity_reliab) == 0) {
            state->p25_p1_soft_rs_ok++;
            irrecoverable_errors = 0;
        }
    }

    if (irrecoverable_errors != 0) {
        state->p25_p1_voice_fec_err++;
        state->debug_header_critical_errors++;
        update_error_stats(heur, 20 * 6 + 16 * 6, 9 * 4);
        return irrecoverable_errors;
    }

    state->p25_p1_voice_fec_ok++;
    state->last_vc_sync_time = time(NULL);
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();

    char fixed_parity[16 * 6];
    correct_golay_dibits_6((char*)hex_data, 20, analog_signal_array);
    encode_reedsolomon_36_20_17((char*)hex_data, fixed_parity);
    ptrdiff_t hoff = (ptrdiff_t)20 * (3 + 6);
    correct_golay_dibits_6(fixed_parity, 16, analog_signal_array + hoff);
    contribute_to_heuristics(state->rf_mod, heur, analog_signal_array, 20 * (3 + 6) + 16 * (3 + 6));
    return 0;
}

static void
hdu_record_enc_lockout(dsd_opts* opts, dsd_state* state, int ttg) {
    if (ttg == 0) {
        return;
    }

    char lockout_name_buf[50];
    const char* lockout_name = "ENC LO";
    dsd_tg_policy_entry lockout_entry;
    if (dsd_tg_policy_lookup_label(state, (uint32_t)ttg, NULL, 0, lockout_name_buf, sizeof(lockout_name_buf))) {
        lockout_name = lockout_name_buf;
    }
    if (dsd_tg_policy_make_exact_entry((uint32_t)ttg, "DE", lockout_name, DSD_TG_POLICY_SOURCE_ENC_LOCKOUT,
                                       &lockout_entry)
            != 0
        || dsd_tg_policy_upsert_exact(state, &lockout_entry, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) != 0) {
        return;
    }

    DSD_SNPRINTF(state->event_history_s[0].Event_History_Items[0].internal_str,
                 sizeof(state->event_history_s[0].Event_History_Items[0].internal_str),
                 "Target: %d; has been locked out; Encryption Lock Out Enabled.", ttg);
    dsd_p25_optional_hook_watchdog_event_current(opts, state, 0);
    if (opts->event_out_file[0] != 0) {
        dsd_p25_optional_hook_write_event_to_log_file(opts, state, 0, /*swrite*/ 0,
                                                      state->event_history_s[0].Event_History_Items[0].event_string);
    }
    dsd_p25_optional_hook_push_event_history(&state->event_history_s[0]);
    dsd_p25_optional_hook_init_event_history(&state->event_history_s[0], 0, 1);
}

static void
hdu_maybe_enc_lockout(dsd_opts* opts, dsd_state* state) {
    if (!(opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && opts->trunk_tune_enc_calls == 0)) {
        return;
    }

    int alg = state->payload_algid;
    int have_key = 0;
    if (((alg == 0xAA || alg == 0x81 || alg == 0x9F) && state->R != 0)
        || ((alg == 0x84 || alg == 0x89) && state->aes_key_loaded[0] == 1)) {
        have_key = 1;
    }
    int enc_suspect = (alg != 0 && alg != 0x80 && have_key == 0);
    if (!enc_suspect) {
        return;
    }

    state->payload_algid = 0;
    state->payload_keyid = 0;
    state->payload_miP = 0ULL;

    hdu_record_enc_lockout(opts, state, state->lasttg);

    DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], "%s", "                     ");
    DSD_FPRINTF(stderr, " No Enc Following on P25p1 Trunking (HDU); Return to CC; \n");
    state->p25_sm_force_release = 1;
    p25_sm_on_release(opts, state);
}

static void
hdu_apply_unmute_policy(dsd_opts* opts, const dsd_state* state) {
    if (state->R != 0
        && (state->payload_algid == 0xAA || state->payload_algid == 0x81 || state->payload_algid == 0x9F)) {
        DSD_FPRINTF(stderr, " Key: %s", DSD_SECRET_REDACTED);
        opts->unmute_encrypted_p25 = 1;
        return;
    }

    if ((state->payload_algid == 0x84 || state->payload_algid == 0x89) && state->aes_key_loaded[0] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, "Key: %s ", DSD_SECRET_REDACTED);
        DSD_FPRINTF(stderr, "%s ", KNRM);
        opts->unmute_encrypted_p25 = 1;
        return;
    }

    if (state->payload_algid != 0 && state->payload_algid != 0x80) {
        opts->unmute_encrypted_p25 = 0;
    }
}

static void
hdu_handle_good_decode(dsd_opts* opts, dsd_state* state, int algidhex, int kidhex, unsigned long long int mihex1,
                       unsigned long long int mihex2, unsigned long long int mihex3) {
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " HDU  ALG ID: 0x%02X KEY ID: 0x%04X MI: 0x%08llX%08llX", algidhex, kidhex, mihex1, mihex2);
    state->payload_algid = algidhex;
    state->payload_keyid = kidhex;
    if (mihex3) {
        DSD_FPRINTF(stderr, "-%02llX", mihex3);
    }
    hdu_apply_unmute_policy(opts, state);
    DSD_FPRINTF(stderr, "%s", KNRM);
    state->payload_miP = (mihex1 << 32) | (mihex2);

    if (state->payload_algid != 0x80 && state->payload_algid != 0x0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " ENC");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    DSD_FPRINTF(stderr, "\n");

    if (state->payload_algid == 0x84 || state->payload_algid == 0x89) {
        LFSR128(state);
        DSD_FPRINTF(stderr, "\n");
    }

    state->xl_is_hdu = 1;
    hdu_maybe_enc_lockout(opts, state);
}

/**
 * The important method that processes a full P25 HD unit.
 */
void
processHDU(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_hdu++;

    // Start status-symbol collection unless the dispatcher already did so for this data unit.
    p25_status_accum_ensure_started(state);

    P25Heuristics* heur = (state->synctype == DSD_SYNC_P25P1_NEG) ? &state->inv_p25_heuristics : &state->p25_heuristics;

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

    AnalogSignal analog_signal_array[20 * (3 + 6) + 16 * (3 + 6)] = {0};
    int analog_signal_index;

    analog_signal_index = 0;

    // we skip the status dibits that occur every 36 symbols
    // the next status symbol comes in 14 dibits from here
    // so we start counter at 36-14-1 = 21
    status_count = 21;

    irrecoverable_errors = hdu_read_and_fec(opts, state, heur, hex_data, hex_parity, analog_signal_array, &status_count,
                                            &analog_signal_index);

    // Now put the corrected data on the DSD structures

    hdu_extract_mi_algid_kid((const char (*)[6])hex_data, mi, algid, kid);

    uint32_t kid_parsed = 0;
    state->p25kid = (dsd_parse_binary_u32_n(kid, 16, &kid_parsed) == 0) ? (int)kid_parsed : 0;
    hdu_consume_trailing_dibits_and_status(opts, state);

    uint32_t algid_parsed = 0;
    algidhex = (dsd_parse_binary_u32_n(algid, 8, &algid_parsed) == 0) ? (int)algid_parsed : 0;
    kidhex = (dsd_parse_binary_u32_n(kid, 16, &kid_parsed) == 0) ? (int)kid_parsed : 0;
    mihex1 = (unsigned long long int)ConvertBitIntoBytes(&mi[0], 32);
    mihex2 = (unsigned long long int)ConvertBitIntoBytes(&mi[32], 32);
    mihex3 = (unsigned long long int)ConvertBitIntoBytes(&mi[64], 8);

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
