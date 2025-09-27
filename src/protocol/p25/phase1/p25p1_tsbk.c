// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * p25p1_tsbk.c
 * P25p1 Trunking Signal Block Handler (with majority-vote over 3 reps)
 *
 * LWVMOBILE
 * 2022-10 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/dsd.h>
#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

void
processTSBK(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_tsbk++;

    // Reset counters and buffers to avoid carryover from voice paths
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
    memset(state->s_l4, 0, sizeof(state->s_l4));
    memset(state->s_r4, 0, sizeof(state->s_r4));
    opts->slot_preference = 2;

    // Ensure slot index is sane when swapping protocols
    state->currentslot = 0;

    // Clear stale Active Channel messages after idle
    const time_t now = time(NULL);
    if ((now - state->last_active_time) > 3) {
        memset(state->active_channel, 0, sizeof(state->active_channel));
    }

    // Buffers
    uint8_t tsbk_dibit[98];
    memset(tsbk_dibit, 0, sizeof(tsbk_dibit));

    uint8_t tsbk_byte[12]; // per repetition
    memset(tsbk_byte, 0, sizeof(tsbk_byte));

    // Store decoded 96 bits from each of the 3 repetitions
    uint8_t rep_bits[3][96];
    memset(rep_bits, 0, sizeof(rep_bits));
    uint8_t rep_bytes[3][12];
    memset(rep_bytes, 0, sizeof(rep_bytes));
    int rep_crc[3] = {-2, -2, -2};

    unsigned long long int PDU[24];
    memset(PDU, 0, sizeof(PDU));

    int tsbk_decoded_bits[96];
    memset(tsbk_decoded_bits, 0, sizeof(tsbk_decoded_bits));

    int i, j, k, x;
    int err = -2; // CRC16 result (majority)

    int protectbit = 0;
    int MFID = 0xFF;

    // Status-dibit skipping state
    int skipdibit = 36 - 14;
    int status_count = 1;
    int dibit_count = 0;
    UNUSED(status_count);
    UNUSED(dibit_count);

    // Collect up to 3 repetitions of 101 dibits (with status dibits interlaced)
    int reps_got = 0;
    for (j = 0; j < 3; j++) {
        k = 0;
        for (i = 0; i < 101; i++) {
            int dibit = getDibit(opts, state);
            if ((skipdibit / 36) == 0) {
                dibit_count++;
                tsbk_dibit[k++] = (uint8_t)dibit;
            } else {
                skipdibit = 0;
                status_count++;
            }
            skipdibit++;
        }

        // 1/2-rate decode this repetition
        (void)p25_12(tsbk_dibit, tsbk_byte);

        // Convert decoded bytes into a 96-bit MSB-first vector
        k = 0;
        for (i = 0; i < 12; i++) {
            for (x = 0; x < 8; x++) {
                tsbk_decoded_bits[k++] = ((tsbk_byte[i] << x) & 0x80) >> 7;
            }
        }
        for (i = 0; i < 96; i++) {
            rep_bits[j][i] = (uint8_t)(tsbk_decoded_bits[i] & 1);
        }
        memcpy(rep_bytes[j], tsbk_byte, 12);

        // Compute per-repetition CRC16 over first 80 bits for later selection
        rep_crc[j] = crc16_lb_bridge(tsbk_decoded_bits, 80);

        reps_got++;

        // If this repetition indicates Last Block, further reps typically stop.
        // Use what we have for majority to avoid blending with the next message.
        int lb_rep = (tsbk_byte[0] >> 7) & 0x1;
        if (lb_rep) {
            break;
        }
    }

    // Majority-vote across available repetitions (1..3)
    uint8_t maj_bits[96];
    memset(maj_bits, 0, sizeof(maj_bits));
    int reps = (reps_got > 0) ? reps_got : 1;
    int thresh = (reps >= 2) ? ((reps + 1) / 2) : 1;
    for (i = 0; i < 96; i++) {
        int sum = 0;
        for (int r = 0; r < reps; r++) {
            sum += (int)rep_bits[r][i];
        }
        maj_bits[i] = (uint8_t)((sum >= thresh) ? 1 : 0);
    }

    // Select best repetition: prefer any CRC-pass rep; otherwise fall back to majority
    int sel_idx = -1;
    for (int r = 0; r < reps; r++) {
        if (rep_crc[r] == 0) {
            sel_idx = r;
            break;
        }
    }
    if (sel_idx >= 0) {
        // Use passing repetition
        memcpy(tsbk_byte, rep_bytes[sel_idx], 12);
        err = 0;
    } else {
        // No rep passed; compute CRC on majority and use majority bytes
        int maj_bits_int[96];
        for (i = 0; i < 96; i++) {
            maj_bits_int[i] = (int)maj_bits[i];
        }
        err = crc16_lb_bridge(maj_bits_int, 80);
        // Rebuild bytes from majority bits for downstream parsing
        for (i = 0; i < 12; i++) {
            int byte = 0;
            for (x = 0; x < 8; x++) {
                byte = (byte << 1) | (maj_bits[(i * 8) + x] & 1);
            }
            tsbk_byte[i] = (uint8_t)byte;
        }
    }

    // Update FEC counters once per message
    if (err == 0) {
        state->p25_p1_fec_ok++;
#ifdef USE_RTLSDR
        rtl_stream_p25p1_ber_update(1, 0);
#endif
    } else {
        state->p25_p1_fec_err++;
#ifdef USE_RTLSDR
        rtl_stream_p25p1_ber_update(0, 1);
#endif
    }

    // Basic field extraction
    MFID = tsbk_byte[1];
    protectbit = (tsbk_byte[0] >> 6) & 0x1;
    /* lb not needed here: only used intra-repetition for early break */

    // Prepare a MAC-like PDU form when applicable
    PDU[0] = 0x07; // P25p1 TSBK DUID
    PDU[1] = tsbk_byte[0] & 0x3F;
    PDU[2] = tsbk_byte[2];
    PDU[3] = tsbk_byte[3];
    PDU[4] = tsbk_byte[4];
    PDU[5] = tsbk_byte[5];
    PDU[6] = tsbk_byte[6];
    PDU[7] = tsbk_byte[7];
    PDU[8] = tsbk_byte[8];
    PDU[9] = tsbk_byte[9];
    PDU[10] = 0; // strip CRC for vPDU search
    PDU[11] = 0;
    PDU[1] ^= 0x40; // flip to match MAC_PDU flavor (3D -> 7D)

    // Downstream handling on majority-decoded frame
    if (MFID < 0x2 && protectbit == 0 && err == 0 && PDU[1] != 0x7B) {
        fprintf(stderr, "%s", KYEL);
        process_MAC_VPDU(opts, state, 0, PDU);
        fprintf(stderr, "%s", KNRM);
    } else if (MFID == 0xA4 && protectbit == 0 && err == 0) {
        // Harris regrouping summaries
        if ((tsbk_byte[0] & 0x3F) == 0x30) {
            int sg = (tsbk_byte[3] << 8) | tsbk_byte[4];
            int key = (tsbk_byte[5] << 8) | tsbk_byte[6];
            int add = (tsbk_byte[7] << 16) | (tsbk_byte[8] << 8) | tsbk_byte[9];
            int tga = tsbk_byte[2] >> 5;
            int ssn = tsbk_byte[2] & 0x1F;
            fprintf(stderr, "%s", KYEL);
            fprintf(stderr, "\n MFID A4 (Harris) Group Regroup Explicit Encryption Command\n");
            if ((tga & 0x2) == 2) {
                fprintf(stderr, "  SG: %d; KEY: %04X; WGID: %d; ", sg, key, add);
                p25_patch_add_wgid(state, sg, add);
            } else {
                fprintf(stderr, "  SG: %d; KEY: %04X; WUID: %d; ", sg, key, add);
                p25_patch_add_wuid(state, sg, (uint32_t)add);
            }
            fprintf(stderr, (tga & 0x4) ? " Simulselect" : " Patch");
            fprintf(stderr, (tga & 0x1) ? " Active;" : " Inactive;");
            fprintf(stderr, " SSN: %02d \n", ssn);
            int is_patch = ((tga & 0x4) == 0) ? 1 : 0;
            int active = (tga & 0x1) ? 1 : 0;
            p25_patch_update(state, sg, is_patch, active);
            p25_patch_set_kas(state, sg, -1, -1, ssn);
        }
    } else if (protectbit == 0 && err == 0 && (tsbk_byte[0] & 0x3F) == 0x3B) {
        // Network Status Broadcast (Abbreviated)
        long int wacn = (tsbk_byte[3] << 12) | (tsbk_byte[4] << 4) | (tsbk_byte[5] >> 4);
        int sysid = ((tsbk_byte[5] & 0xF) << 8) | tsbk_byte[6];
        int channel = (tsbk_byte[7] << 8) | tsbk_byte[8];
        fprintf(stderr, "%s", KYEL);
        fprintf(stderr, "\n Network Status Broadcast TSBK - Abbreviated \n");
        fprintf(stderr, "  WACN [%05lX] SYSID [%03X] NAC [%03llX]", wacn, sysid, state->p2_cc);
        state->p25_cc_freq = process_channel_to_freq(opts, state, channel);
        long neigh[1] = {state->p25_cc_freq};
        p25_sm_on_neighbor_update(opts, state, neigh, 1);
        state->p25_cc_is_tdma = 0;
        if (state->trunk_lcn_freq[0] == 0 || state->trunk_lcn_freq[0] != state->p25_cc_freq) {
            state->trunk_lcn_freq[0] = state->p25_cc_freq;
        }
        if (state->p2_hardset == 0) {
            state->p2_wacn = wacn;
            state->p2_sysid = sysid;
        }
        p25_confirm_idens_for_current_site(state);
    }

    fprintf(stderr, "%s", KNRM);
    fprintf(stderr, "\n");

    // When on a CC, rotate the symbol out file every hour, if enabled
    rotate_symbol_out_file(opts, state);
}
