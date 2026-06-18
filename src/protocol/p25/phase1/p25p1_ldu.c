// SPDX-License-Identifier: ISC
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/p25/p25p1_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef TRACE_DSD
#include <dsd-neo/platform/file_compat.h>
#endif

static int
soft_abs_i16(int16_t v) {
    return v < 0 ? -(int)v : (int)v;
}

#ifdef TRACE_DSD
static void
debug_write_label_imbe(dsd_state* state, unsigned int cc, int bitindex, char bit) {
    if (state->debug_label_imbe_file == NULL) {
        state->debug_label_imbe_file = dsd_fopen_private("pp_label_imbe.txt", "w");
    }
    if (state->debug_label_imbe_file != NULL) {
        const float left = (state->debug_sample_index - 10) / 48000.0F;
        const float right = (state->debug_sample_index) / 48000.0F;
        DSD_FPRINTF(state->debug_label_imbe_file, "%f\t%f\tC_%i[%i]=%c\n", left, right, cc, bitindex, (bit + '0'));
    }
}
#endif

/**
 * Logs the IMBE's c0-c6 words.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static void
debug_log_imbe(char imbe_fr[8][23]) {
    int i, j;
    DSD_FPRINTF(stderr, "    ");

    for (j = 0; j < 4; j++) {
        DSD_FPRINTF(stderr, "{");
        for (i = 0; i < 23; i++) {
            if (i > 0) {
                DSD_FPRINTF(stderr, ", ");
            }
            DSD_FPRINTF(stderr, "%c", (imbe_fr[j][i] + '0'));
        }
        DSD_FPRINTF(stderr, "}; ");
    }

    for (j = 4; j < 7; j++) {
        DSD_FPRINTF(stderr, "{");
        for (i = 0; i < 15; i++) {
            if (i > 0) {
                DSD_FPRINTF(stderr, ", ");
            }
            DSD_FPRINTF(stderr, "%c", (imbe_fr[j][i] + '0'));
        }
        DSD_FPRINTF(stderr, "}; ");
    }

    DSD_FPRINTF(stderr, "\n");
}

static void
maybe_read_midframe_status_symbol(dsd_opts* opts, dsd_state* state, int* status_count) {
    if (*status_count == 35) {
        // Record the mid-frame status symbol for advisory source classification.
#ifdef TRACE_DSD
        state->debug_prefix = 's';
#endif
        {
            dsd_dibit_soft_t status_soft;
            int ss = getDibitSoft(opts, state, &status_soft);
            p25_status_accum_add(state, ss);
        }
        *status_count = 1;

#ifdef TRACE_DSD
        state->debug_prefix = 'I';
#endif
        return;
    }
    (*status_count)++;
}

static void
store_imbe_dibit(char imbe_fr[8][23], dsd_vocoder_soft_bit imbe_soft_fr[8][23], const int* w, const int* x,
                 const int* y, const int* z, int dibit, const dsd_dibit_soft_t* soft) {
    imbe_fr[*w][*x] = (1 & (dibit >> 1)); // bit 1
    imbe_fr[*y][*z] = (1 & dibit);        // bit 0
    imbe_soft_fr[*w][*x] = dsd_vocoder_soft_bit_from_hard_llr(imbe_fr[*w][*x], soft->llr[0]);
    imbe_soft_fr[*y][*z] = dsd_vocoder_soft_bit_from_hard_llr(imbe_fr[*y][*z], soft->llr[1]);
}

#ifdef TRACE_DSD
static void
debug_write_imbe_c0_bits(dsd_state* state, const int* w, const int* x, const int* y, const int* z, int dibit) {
    if (*w == 0) {
        debug_write_label_imbe(state, 0, *x, (1 & (dibit >> 1)));
    }
    if (*y == 0) {
        debug_write_label_imbe(state, 0, *z, (1 & dibit));
    }
}
#endif

static void
update_ldu_encryption_flag(dsd_state* state) {
    // ALGID 0x00 is unknown until HDU/LDU2 confirms clear 0x80.
    state->dmr_encL = (state->payload_algid != 0x80) ? 1 : 0;
}

static int
is_non_standard_c0_word(const char c0_word[23]) {
    static const char non_standard_word[23] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0,
    };
    for (unsigned int i = 0; i < 23; i++) {
        if (c0_word[i] != non_standard_word[i]) {
            return 0;
        }
    }
    return 1;
}

static void
process_imbe_or_skip_non_standard(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23],
                                  dsd_vocoder_soft_bit imbe_soft_fr[8][23]) {
    if (is_non_standard_c0_word(imbe_fr[0])) {
        // Skip this value; it will otherwise look like an erroneous IMBE.
        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, "\n IMBE Non-standard c0 detected, skipped;");
        }
        return;
    }
    processMbeFrameSoft(opts, state, imbe_soft_fr, NULL, NULL);
}

void
process_IMBE(dsd_opts* opts, dsd_state* state, int* status_count) {
    int j, status;
    char imbe_fr[8][23];
    dsd_vocoder_soft_bit imbe_soft_fr[8][23];
    const int *w, *x, *y, *z;
    UNUSED(status);

    DSD_MEMSET(imbe_fr, 0, sizeof(imbe_fr));
    DSD_MEMSET(imbe_soft_fr, 0, sizeof(imbe_soft_fr));

    w = p25p1_imbe_interleave_w;
    x = p25p1_imbe_interleave_x;
    y = p25p1_imbe_interleave_y;
    z = p25p1_imbe_interleave_z;

#ifdef TRACE_DSD
    state->debug_prefix = 'I';
#endif

    for (j = 0; j < 72; j++) {
        maybe_read_midframe_status_symbol(opts, state, status_count);
        dsd_dibit_soft_t soft;
        int dibit = getDibitSoft(opts, state, &soft);
        store_imbe_dibit(imbe_fr, imbe_soft_fr, w, x, y, z, dibit, &soft);

#ifdef TRACE_DSD
        debug_write_imbe_c0_bits(state, w, x, y, z, dibit);
#endif

        w++;
        x++;
        y++;
        z++;
    }

#ifdef TRACE_DSD
    state->debug_prefix = '\0';
#endif
    update_ldu_encryption_flag(state);

    // state->p25kid == 0 || opts->unmute_encrypted_p25 == 1
    // Check for a non-standard c0 transmitted
    // This is explained here: https://github.com/szechyjs/dsd/issues/24

    /* //this is what is observed on the sigid wiki example (matches above pattern)
    IMBE 000F920291AD6F06540980 err = [3] [A]
    IMBE 000F920294816F06540980 err = [3] [A]
    IMBE 000E1C0294816F06540980 err = [3] [B]

    //this is what is observed on the 'hole' examples (not a match, TODO: Examine imbe_fr add this?)
    IMBE FC00000000000000000300 err = [2] [B]
    IMBE FC00000000000000000300 err = [2] [B]
    IMBE FC00000000000000000300 err = [2] [B]

  */

    process_imbe_or_skip_non_standard(opts, state, imbe_fr, imbe_soft_fr);
}

// Uncomment this line for verbose information on the error correction of the LDU bits.
//#define LDU_DEBUG

static int
has_valid_ldu_read_inputs(const dsd_opts* opts, const dsd_state* state, const char* hex, const int* status_count,
                          const int* soft_dibit_index) {
    return opts != NULL && state != NULL && hex != NULL && status_count != NULL && soft_dibit_index != NULL;
}

static void
copy_hex_and_parity_bits(char all_bits[10], const char hex[6], const char parity[4]) {
    DSD_MEMCPY(all_bits, hex, 6);
    DSD_MEMCPY(all_bits + 6, parity, 4);
}

static void
build_soft_hamming_inputs(char bits[10], int reliab[10], const char raw_hex[6], const char raw_parity[4],
                          const P25P1SoftDibit* soft_dibits, int start_index) {
    int idx = 0;

    // Data bits: 3 dibits -> 6 bits.
    for (int d = 0; d < 3; d++) {
        bits[idx] = raw_hex[(d * 2) + 0];
        reliab[idx++] = soft_abs_i16(soft_dibits[start_index + d].llr[0]);
        bits[idx] = raw_hex[(d * 2) + 1];
        reliab[idx++] = soft_abs_i16(soft_dibits[start_index + d].llr[1]);
    }
    // Parity bits: 2 dibits -> 4 bits.
    for (int d = 0; d < 2; d++) {
        bits[idx] = raw_parity[(d * 2) + 0];
        reliab[idx++] = soft_abs_i16(soft_dibits[start_index + 3 + d].llr[0]);
        bits[idx] = raw_parity[(d * 2) + 1];
        reliab[idx++] = soft_abs_i16(soft_dibits[start_index + 3 + d].llr[1]);
    }
}

static int
apply_soft_hamming_fix(dsd_state* state, char hex[6], char parity[4], const char hard_bits[10], const char raw_hex[6],
                       const char raw_parity[4], int hard_error_count, const P25P1SoftDibit* soft_dibits,
                       int start_index) {
    char bits[10];
    int reliab[10];
    char corrected[10];

    build_soft_hamming_inputs(bits, reliab, raw_hex, raw_parity, soft_dibits, start_index);
    int soft_result = hamming_10_6_3_soft(bits, reliab, corrected);
    if (soft_result == 2) {
        return hard_error_count;
    }

    if (hard_error_count == 2 || memcmp(corrected, hard_bits, sizeof(corrected)) != 0) {
        state->p25_p1_soft_hamming_ok++;
    }
    for (int i = 0; i < 6; i++) {
        hex[i] = corrected[i];
    }
    for (int i = 0; i < 4; i++) {
        parity[i] = corrected[6 + i];
    }
    return soft_result; // 0 or 1
}

static void
update_ldu_header_error_counts(dsd_state* state, int error_count) {
    if (error_count == 1) {
        state->debug_header_errors++;
    } else if (error_count == 2) {
        state->debug_header_critical_errors++;
    }
}

#ifdef LDU_DEBUG
static void
debug_print_raw_ldu_hamming(const char hex[6], const char parity[4]) {
    DSD_FPRINTF(stderr, "[");
    for (int i = 0; i < 6; i++) {
        DSD_FPRINTF(stderr, "%c", (hex[i] == 1) ? 'X' : ' ');
    }
    DSD_FPRINTF(stderr, "-");
    for (int i = 0; i < 4; i++) {
        DSD_FPRINTF(stderr, "%c", (parity[i] == 1) ? 'X' : ' ');
    }
    DSD_FPRINTF(stderr, "]");
}

static void
debug_print_fixed_ldu_hamming(const char hex[6], int error_count) {
    DSD_FPRINTF(stderr, " -> [");
    for (int i = 0; i < 6; i++) {
        DSD_FPRINTF(stderr, "%c", (hex[i] == 1) ? 'X' : ' ');
    }
    DSD_FPRINTF(stderr, "]");
    if (error_count == 1) {
        DSD_FPRINTF(stderr, " fixed!");
    } else if (error_count == 2) {
        DSD_FPRINTF(stderr, " IRRECOVERABLE");
    }
    DSD_FPRINTF(stderr, "\n");
}
#endif

void
read_and_correct_hex_word(dsd_opts* opts, dsd_state* state, char* hex, int* status_count, P25P1SoftDibit* soft_dibits,
                          int* soft_dibit_index) {
    if (!has_valid_ldu_read_inputs(opts, state, hex, status_count, soft_dibit_index)) {
        return;
    }

    char parity[4];
    int error_count;
    /* Remember where this hex word's soft dibits start */
    int start_index = *soft_dibit_index;

    // Read the hex word
    read_word(opts, state, hex, 6, status_count, soft_dibits, soft_dibit_index);
    // Read the parity
    read_hamm_parity(opts, state, parity, status_count, soft_dibits, soft_dibit_index);
    char raw_hex[6];
    char raw_parity[4];
    DSD_MEMCPY(raw_hex, hex, sizeof(raw_hex));
    DSD_MEMCPY(raw_parity, parity, sizeof(raw_parity));

#ifdef LDU_DEBUG
    debug_print_raw_ldu_hamming(hex, parity);
#endif

    // Use Hamming to error correct the hex word
    error_count = check_and_fix_hamming_10_6_3(hex, parity);
    int hard_error_count = error_count;
    char hard_bits[10];
    copy_hex_and_parity_bits(hard_bits, hex, parity);

    if (soft_dibits != NULL && (error_count == 1 || error_count == 2)) {
        /* Hard decode failed or corrected a low-confidence bit; try soft decode using reliability info. */
        error_count = apply_soft_hamming_fix(state, hex, parity, hard_bits, raw_hex, raw_parity, hard_error_count,
                                             soft_dibits, start_index);
    }

    update_ldu_header_error_counts(state, error_count);

#ifdef LDU_DEBUG
    debug_print_fixed_ldu_hamming(hex, error_count);
#endif
}

uint8_t
p25p1_hamming_rs_symbol_reliability(const P25P1SoftDibit* symbol) {
    int16_t llr[6];

    if (symbol == NULL) {
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        llr[(i * 2) + 0] = symbol[i].llr[0];
        llr[(i * 2) + 1] = symbol[i].llr[1];
    }
    return p25p1_llr_reliability(llr, 6);
}

void
debug_ldu_header(const dsd_state* state) {
#ifdef TRACE_DSD
    float s = state->debug_sample_index / 48000.0F;
    DSD_FPRINTF(stderr, "Start of LDU at sample %f\n", s);
#else
    UNUSED(state);
#endif
}
