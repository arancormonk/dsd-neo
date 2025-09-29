// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
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

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>

#include <dsd-neo/protocol/p25/p25_lsd.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_ldu.h>

void
processLDU2(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_ldu2++;

    // Mark recent voice activity so trunk hangtime logic does not bounce
    // back to the control channel mid-call.
    state->last_vc_sync_time = time(NULL);

    //push current slot to 0, just in case swapping p2 to p1
    //or stale slot value from p2 and then decoding a pdu
    state->currentslot = 0;

    // extracts IMBE frames from LDU frame
    int i;
    uint8_t mi[73];
    char algid[9], kid[17];
    char lsd1[9], lsd2[9];
    int algidhex, kidhex;
    uint8_t lsd_hex1, lsd_hex2;
    uint8_t lowspeeddata[32];
    memset(lowspeeddata, 0, sizeof(lowspeeddata));
    unsigned long long int mihex1, mihex2, mihex3;
    int status_count;
    int lsd1_okay, lsd2_okay = 0;
    UNUSED(mihex3);

    char hex_data[16][6];  // Data in hex-words (6 bit words). A total of 16 hex words.
    char hex_parity[8][6]; // Parity of the data, again in hex-word format. A total of 12 parity hex words.

    int irrecoverable_errors;

    AnalogSignal analog_signal_array[16 * (3 + 2) + 8 * (3 + 2)];
    int analog_signal_index;

    analog_signal_index = 0;

    // we skip the status dibits that occur every 36 symbols
    // the first IMBE frame starts 14 symbols before next status
    // so we start counter at 36-14-1 = 21
    status_count = 21;

    state->p25vc = 9;

    if (opts->errorbars == 1) {
        //fprintf (stderr, "e:");
    }

    // IMBE 1
#ifdef TRACE_DSD
    state->debug_prefix_2 = '0';
#endif
    process_IMBE(opts, state, &status_count);
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }

    // IMBE 2
#ifdef TRACE_DSD
    state->debug_prefix_2 = '1';
#endif
    process_IMBE(opts, state, &status_count);
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }

    // Read data after IMBE 2
    read_and_correct_hex_word(opts, state, &(hex_data[15][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[14][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[13][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[12][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    {
        size_t idx = (size_t)0 * 5u;
        analog_signal_array[idx].sequence_broken = 1;
    }

    // IMBE 3
#ifdef TRACE_DSD
    state->debug_prefix_2 = '2';
#endif
    process_IMBE(opts, state, &status_count);
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }

    // Read data after IMBE 3
    read_and_correct_hex_word(opts, state, &(hex_data[11][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[10][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[9][0]), &status_count, analog_signal_array, &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[8][0]), &status_count, analog_signal_array, &analog_signal_index);
    {
        size_t idx = (size_t)4 * 5u;
        analog_signal_array[idx].sequence_broken = 1;
    }

    // IMBE 4
#ifdef TRACE_DSD
    state->debug_prefix_2 = '3';
#endif
    process_IMBE(opts, state, &status_count);
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }

    // Read data after IMBE 4
    read_and_correct_hex_word(opts, state, &(hex_data[7][0]), &status_count, analog_signal_array, &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[6][0]), &status_count, analog_signal_array, &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[5][0]), &status_count, analog_signal_array, &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[4][0]), &status_count, analog_signal_array, &analog_signal_index);
    {
        size_t idx = (size_t)8 * 5u;
        analog_signal_array[idx].sequence_broken = 1;
    }

    // IMBE 5
#ifdef TRACE_DSD
    state->debug_prefix_2 = '4';
#endif
    process_IMBE(opts, state, &status_count);
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }

    // Read data after IMBE 5
    read_and_correct_hex_word(opts, state, &(hex_data[3][0]), &status_count, analog_signal_array, &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[2][0]), &status_count, analog_signal_array, &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[1][0]), &status_count, analog_signal_array, &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_data[0][0]), &status_count, analog_signal_array, &analog_signal_index);
    {
        size_t idx = (size_t)12 * 5u;
        analog_signal_array[idx].sequence_broken = 1;
    }

    // Early extract of ALGID/KID/MI after IMBE 5 so we can gate audio for the
    // remainder of this LDU if encrypted. Policy: allow clear (ALGID 0/0x80).
    // For encrypted payloads, allow audio only when a key is present for a
    // recognized algorithm:
    //  - RC4 (0xAA), DES-OFB (0x81), DES-XL (0x9F): require R != 0
    //  - AES-256 (0x84), AES-128 (0x89): require aes_key_loaded[slot] == 1
    // If keys are absent, keep audio muted and (if trunking ENC lockout is
    // enabled) return to CC early. Keep prints later; just set state here.
    if (state->payload_algid == 0) {
        char algid_b[9] = {0};
        char kid_b[17] = {0};
        char mi_b[73] = {0};

        // Build MI bits from hex_data[15..4]
        mi_b[0] = hex_data[15][0] + '0';
        mi_b[1] = hex_data[15][1] + '0';
        mi_b[2] = hex_data[15][2] + '0';
        mi_b[3] = hex_data[15][3] + '0';
        mi_b[4] = hex_data[15][4] + '0';
        mi_b[5] = hex_data[15][5] + '0';

        mi_b[6] = hex_data[14][0] + '0';
        mi_b[7] = hex_data[14][1] + '0';
        mi_b[8] = hex_data[14][2] + '0';
        mi_b[9] = hex_data[14][3] + '0';
        mi_b[10] = hex_data[14][4] + '0';
        mi_b[11] = hex_data[14][5] + '0';

        mi_b[12] = hex_data[13][0] + '0';
        mi_b[13] = hex_data[13][1] + '0';
        mi_b[14] = hex_data[13][2] + '0';
        mi_b[15] = hex_data[13][3] + '0';
        mi_b[16] = hex_data[13][4] + '0';
        mi_b[17] = hex_data[13][5] + '0';

        mi_b[18] = hex_data[12][0] + '0';
        mi_b[19] = hex_data[12][1] + '0';
        mi_b[20] = hex_data[12][2] + '0';
        mi_b[21] = hex_data[12][3] + '0';
        mi_b[22] = hex_data[12][4] + '0';
        mi_b[23] = hex_data[12][5] + '0';

        mi_b[24] = hex_data[11][0] + '0';
        mi_b[25] = hex_data[11][1] + '0';
        mi_b[26] = hex_data[11][2] + '0';
        mi_b[27] = hex_data[11][3] + '0';
        mi_b[28] = hex_data[11][4] + '0';
        mi_b[29] = hex_data[11][5] + '0';

        mi_b[30] = hex_data[10][0] + '0';
        mi_b[31] = hex_data[10][1] + '0';
        mi_b[32] = hex_data[10][2] + '0';
        mi_b[33] = hex_data[10][3] + '0';
        mi_b[34] = hex_data[10][4] + '0';
        mi_b[35] = hex_data[10][5] + '0';

        mi_b[36] = hex_data[9][0] + '0';
        mi_b[37] = hex_data[9][1] + '0';
        mi_b[38] = hex_data[9][2] + '0';
        mi_b[39] = hex_data[9][3] + '0';
        mi_b[40] = hex_data[9][4] + '0';
        mi_b[41] = hex_data[9][5] + '0';

        mi_b[42] = hex_data[8][0] + '0';
        mi_b[43] = hex_data[8][1] + '0';
        mi_b[44] = hex_data[8][2] + '0';
        mi_b[45] = hex_data[8][3] + '0';
        mi_b[46] = hex_data[8][4] + '0';
        mi_b[47] = hex_data[8][5] + '0';

        mi_b[48] = hex_data[7][0] + '0';
        mi_b[49] = hex_data[7][1] + '0';
        mi_b[50] = hex_data[7][2] + '0';
        mi_b[51] = hex_data[7][3] + '0';
        mi_b[52] = hex_data[7][4] + '0';
        mi_b[53] = hex_data[7][5] + '0';

        mi_b[54] = hex_data[6][0] + '0';
        mi_b[55] = hex_data[6][1] + '0';
        mi_b[56] = hex_data[6][2] + '0';
        mi_b[57] = hex_data[6][3] + '0';
        mi_b[58] = hex_data[6][4] + '0';
        mi_b[59] = hex_data[6][5] + '0';

        mi_b[60] = hex_data[5][0] + '0';
        mi_b[61] = hex_data[5][1] + '0';
        mi_b[62] = hex_data[5][2] + '0';
        mi_b[63] = hex_data[5][3] + '0';
        mi_b[64] = hex_data[5][4] + '0';
        mi_b[65] = hex_data[5][5] + '0';

        mi_b[66] = hex_data[4][0] + '0';
        mi_b[67] = hex_data[4][1] + '0';
        mi_b[68] = hex_data[4][2] + '0';
        mi_b[69] = hex_data[4][3] + '0';
        mi_b[70] = hex_data[4][4] + '0';
        mi_b[71] = hex_data[4][5] + '0';

        // Build ALGID bits
        algid_b[0] = hex_data[3][0] + '0';
        algid_b[1] = hex_data[3][1] + '0';
        algid_b[2] = hex_data[3][2] + '0';
        algid_b[3] = hex_data[3][3] + '0';
        algid_b[4] = hex_data[3][4] + '0';
        algid_b[5] = hex_data[3][5] + '0';
        algid_b[6] = hex_data[2][0] + '0';
        algid_b[7] = hex_data[2][1] + '0';

        // Build KID bits
        kid_b[0] = hex_data[2][2] + '0';
        kid_b[1] = hex_data[2][3] + '0';
        kid_b[2] = hex_data[2][4] + '0';
        kid_b[3] = hex_data[2][5] + '0';
        kid_b[4] = hex_data[1][0] + '0';
        kid_b[5] = hex_data[1][1] + '0';
        kid_b[6] = hex_data[1][2] + '0';
        kid_b[7] = hex_data[1][3] + '0';
        kid_b[8] = hex_data[1][4] + '0';
        kid_b[9] = hex_data[1][5] + '0';
        kid_b[10] = hex_data[0][0] + '0';
        kid_b[11] = hex_data[0][1] + '0';
        kid_b[12] = hex_data[0][2] + '0';
        kid_b[13] = hex_data[0][3] + '0';
        kid_b[14] = hex_data[0][4] + '0';
        kid_b[15] = hex_data[0][5] + '0';

        int algid_early = (int)strtol(algid_b, NULL, 2);
        int kid_early = (int)strtol(kid_b, NULL, 2);
        unsigned long long mihex1_e = (unsigned long long)ConvertBitIntoBytes((unsigned char*)&mi_b[0], 32);
        unsigned long long mihex2_e = (unsigned long long)ConvertBitIntoBytes((unsigned char*)&mi_b[32], 32);

        // Do NOT persist early ALGID/KID/MI into state here; bits are not
        // yet fully FEC-corrected and can cause false ENC classification.
        // Defer assignments until after FEC below where algidhex/kidhex/MI
        // are computed from corrected words.
        (void)algid_early;
        (void)kid_early;
        (void)mihex1_e;
        (void)mihex2_e;

        // Establish mute/unmute policy early (no prints here)
        if (state->R != 0 && (algid_early == 0xAA || algid_early == 0x81 || algid_early == 0x9F)) {
            opts->unmute_encrypted_p25 = 1; // RC4/DES/DES-XL with key
        } else if (algid_early == 0x84 || algid_early == 0x89) {
            // AES: defer unmute unless key already known loaded; keep muted by default
            // opts->unmute_encrypted_p25 remains as-is
        } else if (algid_early != 0 && algid_early != 0x80) {
            opts->unmute_encrypted_p25 = 0;
        }

        // Do NOT apply ENC lockout before FEC confirmation; early bits can be
        // wrong and spuriously trigger lockout/misclassification. The proper
        // ENC lockout is handled later after parity/FEC steps.
    }

    // IMBE 6
#ifdef TRACE_DSD
    state->debug_prefix_2 = '5';
#endif
    process_IMBE(opts, state, &status_count);
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }

    // Read data after IMBE 6
    read_and_correct_hex_word(opts, state, &(hex_parity[7][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_parity[6][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_parity[5][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_parity[4][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    analog_signal_array[((size_t)16) * 5].sequence_broken = 1;

    // IMBE 7
#ifdef TRACE_DSD
    state->debug_prefix_2 = '6';
#endif
    process_IMBE(opts, state, &status_count);
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }

    // Read data after IMBE 7
    read_and_correct_hex_word(opts, state, &(hex_parity[3][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_parity[2][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_parity[1][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    read_and_correct_hex_word(opts, state, &(hex_parity[0][0]), &status_count, analog_signal_array,
                              &analog_signal_index);
    analog_signal_array[((size_t)20) * 5].sequence_broken = 1;

    // IMBE 8
#ifdef TRACE_DSD
    state->debug_prefix_2 = '7';
#endif
    process_IMBE(opts, state, &status_count);
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }

    // Read data after IMBE 8: LSD (low speed data)
    {
        char lsd[8];
        char cyclic_parity[8];

        for (i = 0; i <= 6; i += 2) {
            read_dibit(opts, state, lsd + i, &status_count, NULL, NULL);
        }
        for (i = 0; i <= 6; i += 2) {
            read_dibit(opts, state, cyclic_parity + i, &status_count, NULL, NULL);
        }
        lsd_hex1 = 0;
        for (i = 0; i < 8; i++) {
            lsd_hex1 = lsd_hex1 << 1;
            lsd1[i] = lsd[i] + '0';
            lsd_hex1 |= (uint8_t)lsd[i];
            lowspeeddata[i + 0] = (uint8_t)lsd[i];
            lowspeeddata[i + 8] = (uint8_t)cyclic_parity[i];
        }

        for (i = 0; i <= 6; i += 2) {
            read_dibit(opts, state, lsd + i, &status_count, NULL, NULL);
        }
        for (i = 0; i <= 6; i += 2) {
            read_dibit(opts, state, cyclic_parity + i, &status_count, NULL, NULL);
        }
        lsd_hex2 = 0;
        for (i = 0; i < 8; i++) {
            lsd_hex2 = lsd_hex2 << 1;
            lsd2[i] = lsd[i] + '0';
            lsd_hex2 |= (uint8_t)lsd[i];
            lowspeeddata[i + 16] = (uint8_t)lsd[i];
            lowspeeddata[i + 24] = (uint8_t)cyclic_parity[i];
        }

        // TODO: error correction of the LSD bytes... <--THIS!
        // TODO: do something useful with the LSD bytes... <--THIS!

        state->dropL += 2; //need to skip 2 here for the LSD bytes
        //same for octet counter
        state->octet_counter += 2;
    }

    // IMBE 9
#ifdef TRACE_DSD
    state->debug_prefix_2 = '8';
#endif
    process_IMBE(opts, state, &status_count);
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    }
    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    }
    if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }

    //set vc counter to 0
    state->p25vc = 0;

    //reset dropbytes -- skip first 11 for next LCW
    state->dropL = 267;
    //reset octet counter
    state->octet_counter = 0;

    if (opts->errorbars == 1) {
        fprintf(stderr, "\n");
    }

    if (opts->p25status == 1) {
        fprintf(stderr, "lsd1: %s lsd2: %s\n", lsd1, lsd2);
    }

    // trailing status symbol
    {
        int status;
        status = getDibit(opts, state) + '0';
        // TODO: do something useful with the status bits...
        UNUSED(status);
    }

    // Error correct the hex_data using Reed-Solomon hex_parity
    irrecoverable_errors = check_and_fix_reedsolomon_24_16_9((char*)hex_data, (char*)hex_parity);
    if (irrecoverable_errors == 1) {
        state->debug_header_critical_errors++;

        // We can correct (9-1)/2 = 4 errors. If we failed, it means that there were more than 4 errors in
        // these 12+12 words. But take into account that each hex word was already error corrected with
        // Hamming(10,6,3), which can correct 1 bits on each sequence of (6+4) bits. We could say that there
        // were 5 errors of 2 bits.
        update_error_stats(&state->p25_heuristics, 12 * 6 + 12 * 6, 5 * 2);
    } else {
        // Same comments as in processHDU. See there.

        char fixed_parity[8 * 6];

        // Correct the dibits that we read according with hex_data values
        correct_hamming_dibits((char*)hex_data, 16, analog_signal_array);

        // Generate again the Reed-Solomon parity
        encode_reedsolomon_24_16_9((char*)hex_data, fixed_parity);

        // Correct the dibits that we read according with the fixed parity values
        correct_hamming_dibits(fixed_parity, 8, analog_signal_array + ((size_t)16) * (3 + 2));

        // Once corrected, contribute this information to the heuristics module
        contribute_to_heuristics(state->rf_mod, &(state->p25_heuristics), analog_signal_array,
                                 16 * (3 + 2) + 8 * (3 + 2));
    }

#ifdef HEURISTICS_DEBUG
    fprintf(stderr, "(audio errors, header errors, critical header errors) (%i,%i,%i)\n", state->debug_audio_errors,
            state->debug_header_errors, state->debug_header_critical_errors);
#endif

    // Now put the corrected data into the DSD structures

    mi[72] = 0;
    algid[8] = 0;
    kid[16] = 0;
    lsd1[8] = 0;
    lsd2[8] = 0;

    mi[0] = hex_data[15][0] + '0';
    mi[1] = hex_data[15][1] + '0';
    mi[2] = hex_data[15][2] + '0';
    mi[3] = hex_data[15][3] + '0';
    mi[4] = hex_data[15][4] + '0';
    mi[5] = hex_data[15][5] + '0';

    mi[6] = hex_data[14][0] + '0';
    mi[7] = hex_data[14][1] + '0';
    mi[8] = hex_data[14][2] + '0';
    mi[9] = hex_data[14][3] + '0';
    mi[10] = hex_data[14][4] + '0';
    mi[11] = hex_data[14][5] + '0';

    mi[12] = hex_data[13][0] + '0';
    mi[13] = hex_data[13][1] + '0';
    mi[14] = hex_data[13][2] + '0';
    mi[15] = hex_data[13][3] + '0';
    mi[16] = hex_data[13][4] + '0';
    mi[17] = hex_data[13][5] + '0';

    mi[18] = hex_data[12][0] + '0';
    mi[19] = hex_data[12][1] + '0';
    mi[20] = hex_data[12][2] + '0';
    mi[21] = hex_data[12][3] + '0';
    mi[22] = hex_data[12][4] + '0';
    mi[23] = hex_data[12][5] + '0';

    mi[24] = hex_data[11][0] + '0';
    mi[25] = hex_data[11][1] + '0';
    mi[26] = hex_data[11][2] + '0';
    mi[27] = hex_data[11][3] + '0';
    mi[28] = hex_data[11][4] + '0';
    mi[29] = hex_data[11][5] + '0';

    mi[30] = hex_data[10][0] + '0';
    mi[31] = hex_data[10][1] + '0';
    mi[32] = hex_data[10][2] + '0';
    mi[33] = hex_data[10][3] + '0';
    mi[34] = hex_data[10][4] + '0';
    mi[35] = hex_data[10][5] + '0';

    mi[36] = hex_data[9][0] + '0';
    mi[37] = hex_data[9][1] + '0';
    mi[38] = hex_data[9][2] + '0';
    mi[39] = hex_data[9][3] + '0';
    mi[40] = hex_data[9][4] + '0';
    mi[41] = hex_data[9][5] + '0';

    mi[42] = hex_data[8][0] + '0';
    mi[43] = hex_data[8][1] + '0';
    mi[44] = hex_data[8][2] + '0';
    mi[45] = hex_data[8][3] + '0';
    mi[46] = hex_data[8][4] + '0';
    mi[47] = hex_data[8][5] + '0';

    mi[48] = hex_data[7][0] + '0';
    mi[49] = hex_data[7][1] + '0';
    mi[50] = hex_data[7][2] + '0';
    mi[51] = hex_data[7][3] + '0';
    mi[52] = hex_data[7][4] + '0';
    mi[53] = hex_data[7][5] + '0';

    mi[54] = hex_data[6][0] + '0';
    mi[55] = hex_data[6][1] + '0';
    mi[56] = hex_data[6][2] + '0';
    mi[57] = hex_data[6][3] + '0';
    mi[58] = hex_data[6][4] + '0';
    mi[59] = hex_data[6][5] + '0';

    mi[60] = hex_data[5][0] + '0';
    mi[61] = hex_data[5][1] + '0';
    mi[62] = hex_data[5][2] + '0';
    mi[63] = hex_data[5][3] + '0';
    mi[64] = hex_data[5][4] + '0';
    mi[65] = hex_data[5][5] + '0';

    mi[66] = hex_data[4][0] + '0';
    mi[67] = hex_data[4][1] + '0';
    mi[68] = hex_data[4][2] + '0';
    mi[69] = hex_data[4][3] + '0';
    mi[70] = hex_data[4][4] + '0';
    mi[71] = hex_data[4][5] + '0';

    algid[0] = hex_data[3][0] + '0';
    algid[1] = hex_data[3][1] + '0';
    algid[2] = hex_data[3][2] + '0';
    algid[3] = hex_data[3][3] + '0';
    algid[4] = hex_data[3][4] + '0';
    algid[5] = hex_data[3][5] + '0';

    algid[6] = hex_data[2][0] + '0';
    algid[7] = hex_data[2][1] + '0';
    kid[0] = hex_data[2][2] + '0';
    kid[1] = hex_data[2][3] + '0';
    kid[2] = hex_data[2][4] + '0';
    kid[3] = hex_data[2][5] + '0';

    kid[4] = hex_data[1][0] + '0';
    kid[5] = hex_data[1][1] + '0';
    kid[6] = hex_data[1][2] + '0';
    kid[7] = hex_data[1][3] + '0';
    kid[8] = hex_data[1][4] + '0';
    kid[9] = hex_data[1][5] + '0';

    kid[10] = hex_data[0][0] + '0';
    kid[11] = hex_data[0][1] + '0';
    kid[12] = hex_data[0][2] + '0';
    kid[13] = hex_data[0][3] + '0';
    kid[14] = hex_data[0][4] + '0';
    kid[15] = hex_data[0][5] + '0';

    algidhex = strtol(algid, NULL, 2);
    kidhex = strtol(kid, NULL, 2);
    mihex1 = (unsigned long long int)ConvertBitIntoBytes(&mi[0], 32);
    mihex2 = (unsigned long long int)ConvertBitIntoBytes(&mi[32], 32);
    mihex3 = (unsigned long long int)ConvertBitIntoBytes(&mi[64], 8);

    //NOTE: LSD is also encrypted if voice is encrypted, so let's just zip it for now
    if (state->payload_algid != 0x80) {
        lsd_hex1 = 0;
        lsd_hex2 = 0;
    }

    // LSD FEC (16,8) — correct single-bit errors in full codeword
    lsd1_okay = p25_lsd_fec_16x8(lowspeeddata + 0);
    lsd2_okay = p25_lsd_fec_16x8(lowspeeddata + 16);

    if (irrecoverable_errors == 0) {
        fprintf(stderr, "%s", KYEL);
        fprintf(stderr, " LDU2 ALG ID: 0x%02X KEY ID: 0x%04X MI: 0x%08llX%08llX", algidhex, kidhex, mihex1, mihex2);
        state->payload_algid = algidhex;
        state->payload_keyid = kidhex;
        if (mihex3) {
            fprintf(stderr, "-%02llX", mihex3);
        }
        if (state->R != 0
            && (state->payload_algid == 0xAA || state->payload_algid == 0x81 || state->payload_algid == 0x9F)) {
            fprintf(stderr, " Key: %010llX", state->R);
            opts->unmute_encrypted_p25 = 1;
        }
        /* //going to want to update this for OFB, CBC, and ECB
      { 0x84, "AES-256-OFB" },
      { 0x85, "AES-128-ECB"},
      { 0x88, "AES-CBC"},
      { 0x89, "AES-128-OFB"},
    */
        else if ((state->payload_algid == 0x84 || state->payload_algid == 0x89) && state->aes_key_loaded[0] == 1) {
            fprintf(stderr, "\n ");
            fprintf(stderr, "%s", KYEL);
            fprintf(stderr, "Key: %016llX %016llX ", state->A1[0], state->A2[0]);
            if (state->payload_algid == 0x84) {
                fprintf(stderr, "%016llX %016llX", state->A3[0], state->A4[0]);
            }
            fprintf(stderr, "%s ", KNRM);
            opts->unmute_encrypted_p25 = 1;
        } else if (state->payload_algid != 0 && state->payload_algid != 0x80) {
            //may want to mute this again, or may not want to
            opts->unmute_encrypted_p25 = 0;
        }
        fprintf(stderr, "%s", KNRM);
        //only use 64 MSB, trailing 8 bits aren't used, so no mihex3
        state->payload_miP = (mihex1 << 32) | (mihex2);

        if (state->payload_algid != 0x80 && state->payload_algid != 0x0) {
            fprintf(stderr, "%s", KRED);
            fprintf(stderr, " ENC");
            fprintf(stderr, "%s", KNRM);
        }
    } else {
        fprintf(stderr, "%s", KRED);
        fprintf(stderr, " LDU2 FEC ERR ");
        fprintf(stderr, "%s", KNRM);
    }

    if (opts->payload == 1) {
        //view Low Speed Data
        fprintf(stderr, "%s", KCYN);
        fprintf(stderr, "    LSD: %02X %02X ", lsd_hex1, lsd_hex2);
        if ((lsd_hex1 > 0x19) && (lsd_hex1 < 0x7F) && (lsd1_okay == 1)) {
            fprintf(stderr, "(%c", lsd_hex1);
        } else {
            fprintf(stderr, "( ");
        }
        if ((lsd_hex2 > 0x19) && (lsd_hex2 < 0x7F) && (lsd2_okay == 1)) {
            fprintf(stderr, "%c)", lsd_hex2);
        } else {
            fprintf(stderr, " )");
        }
        if (lsd1_okay == 0) {
            fprintf(stderr, " L1 ERR");
        }
        if (lsd2_okay == 0) {
            fprintf(stderr, " L2 ERR");
        }
        fprintf(stderr, "%s", KNRM);
    }

    fprintf(stderr, "\n");

    //TEST: Store LSD into array if 0x02 0x08 (opcode and len?)
    int k = 0;
    if (state->dmr_alias_format[0] == 0x02) {
        k = state->data_block_counter[0];
        if ((lsd_hex1 > 0x19) && (lsd_hex1 < 0x7F) && (lsd1_okay == 1)) {
            state->dmr_alias_block_segment[0][0][k / 4][k % 4] = (char)lsd_hex1;
        }
        // else state->dmr_alias_block_segment[0][0][k/4][k%4] = 0x20;
        k++;
        if ((lsd_hex2 > 0x19) && (lsd_hex2 < 0x7F) && (lsd2_okay == 1)) {
            state->dmr_alias_block_segment[0][0][k / 4][k % 4] = (char)lsd_hex2;
        }
        // else state->dmr_alias_block_segment[0][0][k/4][k%4] = 0x20;
        k++;
        state->data_block_counter[0] = k;
    }

    //reset format, len, counter.
    if (lsd_hex1 == 0x02 && lsd1_okay == 1 && lsd2_okay == 1) {
        state->dmr_alias_format[0] = 0x02;
        if (lsd_hex2 > 8) {
            lsd_hex2 = 8; //sanity check
        }
        state->dmr_alias_block_len[0] = lsd_hex2;
        state->data_block_counter[0] = 0;
    }

    if ((k >= state->dmr_alias_block_len[0]) && (state->dmr_alias_format[0] == 0x02)) {
        //storage for completed string
        char str[16];
        int wr = 0;
        int tsrc = state->lastsrc;
        int z = 0;
        k = 0;
        for (i = 0; i < 16; i++) {
            str[i] = 0;
        }

        //print out what we've gathered
        fprintf(stderr, "%s", KCYN);
        fprintf(stderr, " LSD Soft ID: ");
        for (i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                fprintf(stderr, "%c", state->dmr_alias_block_segment[0][0][i][j]);
                if (state->dmr_alias_block_segment[0][0][i][j] != 0) {
                    str[k++] = state->dmr_alias_block_segment[0][0][i][j];
                }
            }
        }

        //debug
        // fprintf (stderr, " STR: %s", str);

        //assign to tg name string, but only if not trunking (this could clash with ENC LO functionality)
        if (tsrc
            != 0) //&& opts->p25_trunk == 0 //should never get here if enc, should be zeroed out, but could potentially slip if HDU is missed and offchance of 02 opcode
        {
            for (unsigned int x = 0; x < state->group_tally; x++) {
                if (state->group_array[x].groupNumber == (unsigned long)tsrc) {
                    wr = 1; //already in there, so no need to assign it
                    z = x;
                    break;
                }
            }

            //if not already in there, so save it there now
            if (wr == 0) {
                state->group_array[state->group_tally].groupNumber = tsrc;
                // Only mark as encrypted if ALG is known non-clear (not 0x80 and not 0)
                if (state->payload_algid != 0x80 && state->payload_algid != 0 && opts->trunk_tune_enc_calls == 0
                    && state->R == 0) {
                    sprintf(state->group_array[state->group_tally].groupMode, "%s", "DE");
                } else {
                    sprintf(state->group_array[state->group_tally].groupMode, "%s", "D");
                }
                sprintf(state->group_array[state->group_tally].groupName, "%s", str);
                state->group_tally++;
            }

            //if its in there, but doesn't match (bad/partial decode)
            else if (strcmp(str, state->group_array[z].groupName) != 0) {
                state->group_array[z].groupNumber = tsrc;
                // Only mark as encrypted if ALG is known non-clear (not 0x80 and not 0)
                if (state->payload_algid != 0x80 && state->payload_algid != 0 && opts->trunk_tune_enc_calls == 0
                    && state->R == 0) {
                    sprintf(state->group_array[state->group_tally].groupMode, "%s", "DE");
                } else {
                    sprintf(state->group_array[state->group_tally].groupMode, "%s", "D");
                }
                sprintf(state->group_array[z].groupName, "%s", str);
            }
        }

        fprintf(stderr, "%s", KNRM);
        fprintf(stderr, "\n");

        //reset values
        state->dmr_alias_format[0] = 0;
        state->data_block_counter[0] = 0;
        state->dmr_alias_block_len[0] = 0;
        // memset (state->dmr_alias_block_segment, 0, sizeof(state->dmr_alias_block_segment));
    }

    //run LFSR on the MI if we have irrecoverable errors here
    if (irrecoverable_errors && state->payload_algid != 0x80 && state->payload_keyid != 0 && state->payload_miP != 0) {
        LFSRP(state);
        fprintf(stderr, "\n");
    }

    //expand 64-bit MI to 128-bit for AES
    if (state->payload_algid == 0x84 || state->payload_algid == 0x89) {
        LFSR128(state);
        fprintf(stderr, "\n");
    }

    //xl, we need to know if the ESS is from HDU, or LDU2
    state->xl_is_hdu = 0;

#define P25p1_ENC_LO //disable if this behavior is detremental
#ifdef P25p1_ENC_LO
    //If trunking and tuning ENC calls is disabled, lock out and go back to CC
    int enc_lo = 1;
    int ttg = state->lasttg; //checking to a valid TG will help make sure we have a good LDU1 LCW or HDU first
    if (irrecoverable_errors == 0 && state->payload_algid != 0x80 && state->payload_algid != 0 && opts->p25_trunk == 1
        && opts->p25_is_tuned == 1 && opts->trunk_tune_enc_calls == 0) {
        //NOTE: This may still cause an issue IF we havent' loaded the key yet from keyloader
        if (state->payload_algid == 0xAA && state->R != 0) {
            enc_lo = 0;
        }
        // else if (future condition) enc_lo = 0;
        // else if (future condition) enc_lo = 0;

        //if this is locked out by conditions above, then write it into the TG mode if we have a TG value assigned
        if (enc_lo == 1 && ttg != 0) {
            unsigned int xx = 0;
            int enc_wr = 0;
            for (xx = 0; xx < state->group_tally; xx++) {
                if (state->group_array[xx].groupNumber == (unsigned long)ttg) {
                    enc_wr = 1; //already in there, so no need to assign it
                    break;
                }
            }

            //if not already in there, so save it there now
            if (enc_wr == 0) {
                state->group_array[state->group_tally].groupNumber = ttg;
                sprintf(state->group_array[state->group_tally].groupMode, "%s", "DE");
                sprintf(state->group_array[state->group_tally].groupName, "%s",
                        "ENC LO"); //was xx and not state->group_tally
                state->group_tally++;
            }

            //run a watchdog here so we can update this with the crypto variables and ENC LO
            if (ttg != 0 && enc_wr == 0) //
            {
                snprintf(state->event_history_s[0].Event_History_Items[0].internal_str,
                         sizeof state->event_history_s[0].Event_History_Items[0].internal_str,
                         "Target: %d; has been locked out; Encryption Lock Out Enabled.", ttg);
                watchdog_event_current(opts, state, 0);
                Event_History_I* eh = &state->event_history_s[0];
                if (strncmp(eh->Event_History_Items[1].internal_str, eh->Event_History_Items[0].internal_str,
                            sizeof eh->Event_History_Items[0].internal_str)
                    != 0) {
                    if (opts->event_out_file[0] != '\0') {
                        write_event_to_log_file(opts, state, 0, /*swrite*/ 0, eh->Event_History_Items[0].event_string);
                    }
                    push_event_history(eh);
                    init_event_history(eh, 0, 1);
                }
            }

            //return to the control channel
            fprintf(stderr, " No Enc Following on P25p1 Trunking; Return to CC; \n");
            p25_sm_on_release(opts, state);
        }
    }
#endif //P25p1_ENC_LO
}

//LFSR code courtesy of https://github.com/mattames/LFSR/
void
LFSRP(dsd_state* state) {
    //rework for P2 TDMA support
    unsigned long long int lfsr = 0;
    if (state->currentslot == 0) {
        lfsr = state->payload_miP;
    }

    if (state->currentslot == 1) {
        lfsr = state->payload_miN;
    }

    int cnt = 0;
    for (cnt = 0; cnt < 64; cnt++) {
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1
        unsigned long long int bit =
            ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1;
        lfsr = (lfsr << 1) | (bit);
    }

    if (state->currentslot == 0) {
        state->payload_miP = lfsr;
    }

    if (state->currentslot == 1) {
        state->payload_miN = lfsr;
    }

    //print current ENC identifiers already known and new calculated MI
    fprintf(stderr, "%s", KYEL);
    if (state->currentslot == 0) {
        fprintf(stderr, "\n LDU2/ESS_B FEC ERR - ALG: 0x%02X KEY ID: 0x%04X LFSR MI: 0x%016llX", state->payload_algid,
                state->payload_keyid, state->payload_miP);
    }
    if (state->currentslot == 1) {
        fprintf(stderr, "\n LDU2/ESS_B FEC ERR - ALG: 0x%02X KEY ID: 0x%04X LFSR MI: 0x%016llX", state->payload_algidR,
                state->payload_keyidR, state->payload_miN);
    }
    fprintf(stderr, "%s", KNRM);
}

void
LFSR128(dsd_state* state) {
    //generate a 128-bit IV from a 64-bit IV for AES blocks
    unsigned long long int lfsr = 0;

    int slot = state->currentslot;

    if (state->currentslot == 0) {
        lfsr = state->payload_miP;
    } else {
        lfsr = state->payload_miN;
    }

    //start packing aes_iv
    if (slot == 0) {
        state->aes_iv[0] = (lfsr >> 56) & 0xFF;
        state->aes_iv[1] = (lfsr >> 48) & 0xFF;
        state->aes_iv[2] = (lfsr >> 40) & 0xFF;
        state->aes_iv[3] = (lfsr >> 32) & 0xFF;
        state->aes_iv[4] = (lfsr >> 24) & 0xFF;
        state->aes_iv[5] = (lfsr >> 16) & 0xFF;
        state->aes_iv[6] = (lfsr >> 8) & 0xFF;
        state->aes_iv[7] = (lfsr >> 0) & 0xFF;
    }
    if (slot == 1) {
        state->aes_ivR[0] = (lfsr >> 56) & 0xFF;
        state->aes_ivR[1] = (lfsr >> 48) & 0xFF;
        state->aes_ivR[2] = (lfsr >> 40) & 0xFF;
        state->aes_ivR[3] = (lfsr >> 32) & 0xFF;
        state->aes_ivR[4] = (lfsr >> 24) & 0xFF;
        state->aes_ivR[5] = (lfsr >> 16) & 0xFF;
        state->aes_ivR[6] = (lfsr >> 8) & 0xFF;
        state->aes_ivR[7] = (lfsr >> 0) & 0xFF;
    }

    int cnt = 0;
    int x = 64;
    unsigned long long int bit;
    //polynomial P(x) = 1 + X15 + X27 + X38 + X46 + X62 + X64
    for (cnt = 0; cnt < 64; cnt++) {
        //63,61,45,37,27,14
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1
        bit = ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1;
        lfsr = (lfsr << 1) | bit;

        //continue packing aes_iv
        if (slot == 0) {
            state->aes_iv[x / 8] = (state->aes_iv[x / 8] << 1) + bit;
        }
        if (slot == 1) {
            state->aes_ivR[x / 8] = (state->aes_ivR[x / 8] << 1) + bit;
        }
        x++;
    }

    if (state->currentslot == 0) {
        fprintf(stderr, "%s", KYEL);
        if (state->dmrburstL != 27) { //if not LDU2
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "     ");
        fprintf(stderr, " ALG ID: 0x%02X KEY ID: 0x%04X MI(128): 0x", state->payload_algid, state->payload_keyid);
        for (x = 0; x < 16; x++) {
            fprintf(stderr, "%02X", state->aes_iv[x]);
        }
        fprintf(stderr, "%s", KNRM);
        // fprintf (stderr, "\n");
    }

    if (state->currentslot == 1) {
        fprintf(stderr, "%s", KYEL);
        if (state->dmrburstL != 27) { //if not LDU2, shouldn't matter on P25p2 for the second slot
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "     ");
        fprintf(stderr, " ALG ID: 0x%02X KEY ID: 0x%04X MI(128): 0x", state->payload_algidR, state->payload_keyidR);
        for (x = 0; x < 16; x++) {
            fprintf(stderr, "%02X", state->aes_ivR[x]);
        }
        fprintf(stderr, "%s", KNRM);
        // fprintf (stderr, "\n");
    }
}
