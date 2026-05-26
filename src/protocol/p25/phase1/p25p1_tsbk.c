// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * p25p1_tsbk.c
 * P25p1 Trunking Signal Block Handler (with majority-vote over 3 reps)
 *
 * LWVMOBILE
 * 2022-10 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_12.h>
#include <dsd-neo/protocol/p25/p25_callsign.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RTLSDR
#endif

static int16_t
saturating_llr_add(int acc, int value) {
    acc += value;
    if (acc > INT16_MAX) {
        return INT16_MAX;
    }
    if (acc < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)acc;
}

enum {
    TSBK_REPETITIONS = 3,
    TSBK_DIBITS_PER_REP = 98,
    TSBK_DIBITS_WITH_STATUS = 101,
    TSBK_SOFT_BITS_PER_REP = 196,
    TSBK_BYTES_PER_BLOCK = 12,
    TSBK_BITS_PER_BLOCK = 96
};

typedef struct {
    uint8_t tsbk_dibit[TSBK_DIBITS_PER_REP];
    int16_t tsbk_llr[TSBK_SOFT_BITS_PER_REP];
    int16_t rep_llr[TSBK_REPETITIONS][TSBK_SOFT_BITS_PER_REP];
    uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK];
    uint8_t rep_bits[TSBK_REPETITIONS][TSBK_BITS_PER_BLOCK];
    uint8_t rep_bytes[TSBK_REPETITIONS][TSBK_BYTES_PER_BLOCK];
    int rep_crc[TSBK_REPETITIONS];
    int tsbk_decoded_bits[TSBK_BITS_PER_BLOCK];
} tsbk_decode_ctx_t;

static void
tsbk_prepare_frame_state(dsd_opts* opts, dsd_state* state) {
    state->p25_p1_duid_tsbk++;

    // Reset counters and buffers to avoid carryover from voice paths.
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    opts->slot_preference = 2;

    // Ensure slot index is sane when swapping protocols.
    state->currentslot = 0;

    p25_status_accum_ensure_started(state);

    // Clear stale active-channel text after a short idle gap.
    const time_t now = time(NULL);
    if ((now - state->last_active_time) > 3) {
        DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
    }
}

static void
tsbk_init_decode_ctx(tsbk_decode_ctx_t* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    for (int i = 0; i < TSBK_REPETITIONS; i++) {
        ctx->rep_crc[i] = -2;
    }
}

static void
tsbk_bits_from_bytes(const uint8_t bytes[TSBK_BYTES_PER_BLOCK], int bits[TSBK_BITS_PER_BLOCK]) {
    int bit_index = 0;
    for (int b = 0; b < TSBK_BYTES_PER_BLOCK; b++) {
        for (int bit = 0; bit < 8; bit++) {
            bits[bit_index++] = ((bytes[b] << bit) & 0x80) >> 7;
        }
    }
}

static void
tsbk_bytes_from_bits(const uint8_t bits[TSBK_BITS_PER_BLOCK], uint8_t bytes[TSBK_BYTES_PER_BLOCK]) {
    for (int i = 0; i < TSBK_BYTES_PER_BLOCK; i++) {
        int byte = 0;
        for (int x = 0; x < 8; x++) {
            byte = (byte << 1) | (bits[(i * 8) + x] & 1);
        }
        bytes[i] = (uint8_t)byte;
    }
}

static int
tsbk_select_crc_candidate(const p25_12_candidate_t candidates[P25_12_MAX_CANDIDATES], int candidate_count,
                          int default_index) {
    for (int c = 0; c < candidate_count; c++) {
        int candidate_bits[TSBK_BITS_PER_BLOCK];
        tsbk_bits_from_bytes(candidates[c].bytes, candidate_bits);
        if (crc16_lb_bridge(candidate_bits, 80) == 0) {
            return c;
        }
    }
    return default_index;
}

static void
tsbk_decode_repetition_bytes(const uint8_t tsbk_dibit[TSBK_DIBITS_PER_REP],
                             const int16_t tsbk_llr[TSBK_SOFT_BITS_PER_REP], uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    p25_12_candidate_t candidates[P25_12_MAX_CANDIDATES];
    int candidate_count = p25_12_soft_llr_list(tsbk_dibit, tsbk_llr, candidates, P25_12_MAX_CANDIDATES);
    if (candidate_count > 0) {
        int selected = tsbk_select_crc_candidate(candidates, candidate_count, 0);
        DSD_MEMCPY(tsbk_byte, candidates[selected].bytes, TSBK_BYTES_PER_BLOCK);
        return;
    }
    (void)p25_12_soft_llr(tsbk_dibit, tsbk_llr, tsbk_byte);
}

static void
tsbk_read_repetition_samples(dsd_opts* opts, dsd_state* state, int* skipdibit, uint8_t tsbk_dibit[TSBK_DIBITS_PER_REP],
                             int16_t tsbk_llr[TSBK_SOFT_BITS_PER_REP]) {
    int k = 0;
    for (int i = 0; i < TSBK_DIBITS_WITH_STATUS; i++) {
        dsd_dibit_soft_t soft;
        int dibit = getDibitSoft(opts, state, &soft);
        if ((*skipdibit / 36) == 0) {
            tsbk_dibit[k] = (uint8_t)dibit;
            tsbk_llr[(k * 2) + 0] = soft.llr[0];
            tsbk_llr[(k * 2) + 1] = soft.llr[1];
            k++;
        } else {
            p25_status_accum_add(state, dibit);
            *skipdibit = 0;
        }
        (*skipdibit)++;
    }
}

static int
tsbk_collect_repetitions(dsd_opts* opts, dsd_state* state, tsbk_decode_ctx_t* ctx) {
    int skipdibit = 36 - 14;
    int reps_got = 0;

    for (int rep = 0; rep < TSBK_REPETITIONS; rep++) {
        tsbk_read_repetition_samples(opts, state, &skipdibit, ctx->tsbk_dibit, ctx->tsbk_llr);
        tsbk_decode_repetition_bytes(ctx->tsbk_dibit, ctx->tsbk_llr, ctx->tsbk_byte);

        DSD_MEMCPY(ctx->rep_llr[rep], ctx->tsbk_llr, sizeof(ctx->tsbk_llr));
        tsbk_bits_from_bytes(ctx->tsbk_byte, ctx->tsbk_decoded_bits);
        for (int i = 0; i < TSBK_BITS_PER_BLOCK; i++) {
            ctx->rep_bits[rep][i] = (uint8_t)(ctx->tsbk_decoded_bits[i] & 1);
        }
        DSD_MEMCPY(ctx->rep_bytes[rep], ctx->tsbk_byte, TSBK_BYTES_PER_BLOCK);
        ctx->rep_crc[rep] = crc16_lb_bridge(ctx->tsbk_decoded_bits, 80);

        reps_got++;
        if (((ctx->tsbk_byte[0] >> 7) & 0x1) != 0) {
            break;
        }
    }

    return reps_got;
}

static void
tsbk_build_majority_bits(uint8_t rep_bits[TSBK_REPETITIONS][TSBK_BITS_PER_BLOCK], int reps,
                         uint8_t maj_bits[TSBK_BITS_PER_BLOCK]) {
    DSD_MEMSET(maj_bits, 0, TSBK_BITS_PER_BLOCK);
    int thresh = (reps >= 2) ? ((reps + 1) / 2) : 1;
    for (int i = 0; i < TSBK_BITS_PER_BLOCK; i++) {
        int sum = 0;
        for (int r = 0; r < reps; r++) {
            sum += (int)rep_bits[r][i];
        }
        maj_bits[i] = (uint8_t)((sum >= thresh) ? 1 : 0);
    }
}

static int
tsbk_select_passing_repetition(const int rep_crc[TSBK_REPETITIONS], int reps) {
    for (int r = 0; r < reps; r++) {
        if (rep_crc[r] == 0) {
            return r;
        }
    }
    return -1;
}

static int
tsbk_try_combined_soft_decode(dsd_state* state, tsbk_decode_ctx_t* ctx, int reps) {
    int16_t combined_llr[TSBK_SOFT_BITS_PER_REP];
    DSD_MEMSET(combined_llr, 0, sizeof(combined_llr));
    for (int i = 0; i < TSBK_SOFT_BITS_PER_REP; i++) {
        int acc = 0;
        for (int r = 0; r < reps; r++) {
            acc = saturating_llr_add(acc, ctx->rep_llr[r][i]);
        }
        combined_llr[i] = (int16_t)acc;
    }

    p25_12_candidate_t candidates[P25_12_MAX_CANDIDATES];
    int candidate_count = p25_12_soft_llr_list(NULL, combined_llr, candidates, P25_12_MAX_CANDIDATES);
    int selected = tsbk_select_crc_candidate(candidates, candidate_count, -1);
    if (selected < 0) {
        return -1;
    }

    DSD_MEMCPY(ctx->tsbk_byte, candidates[selected].bytes, TSBK_BYTES_PER_BLOCK);
    state->p25_p1_soft_combined_ok++;
    return 0;
}

static int
tsbk_finalize_decode(dsd_state* state, tsbk_decode_ctx_t* ctx, int reps_got) {
    uint8_t maj_bits[TSBK_BITS_PER_BLOCK];
    int reps = (reps_got > 0) ? reps_got : 1;
    tsbk_build_majority_bits(ctx->rep_bits, reps, maj_bits);

    int sel_idx = tsbk_select_passing_repetition(ctx->rep_crc, reps);
    if (sel_idx >= 0) {
        DSD_MEMCPY(ctx->tsbk_byte, ctx->rep_bytes[sel_idx], TSBK_BYTES_PER_BLOCK);
        return 0;
    }

    if (reps > 1 && tsbk_try_combined_soft_decode(state, ctx, reps) == 0) {
        return 0;
    }

    int maj_bits_int[TSBK_BITS_PER_BLOCK];
    for (int i = 0; i < TSBK_BITS_PER_BLOCK; i++) {
        maj_bits_int[i] = (int)maj_bits[i];
    }
    int err = crc16_lb_bridge(maj_bits_int, 80);
    tsbk_bytes_from_bits(maj_bits, ctx->tsbk_byte);
    return err;
}

static void
tsbk_update_fec_counters(dsd_state* state, int err) {
    if (err == 0) {
        // Refresh CC activity on any good TSBK decode to avoid premature hunting.
        state->last_cc_sync_time = time(NULL);
        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
        state->p25_p1_fec_ok++;
#ifdef USE_RTLSDR
        dsd_rtl_stream_metrics_hook_p25p1_ber_update(1, 0);
#endif
    } else {
        state->p25_p1_fec_err++;
#ifdef USE_RTLSDR
        dsd_rtl_stream_metrics_hook_p25p1_ber_update(0, 1);
#endif
    }
}

static void
tsbk_build_mac_like_pdu(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK], unsigned long long int PDU[24]) {
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
}

static void
tsbk_handle_mfid90_regroup_add_del(dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK], int add_cmd) {
    int sg = (tsbk_byte[2] << 8) | tsbk_byte[3];
    int ga1 = (tsbk_byte[4] << 8) | tsbk_byte[5];
    int ga2 = (tsbk_byte[6] << 8) | tsbk_byte[7];
    int ga3 = (tsbk_byte[8] << 8) | tsbk_byte[9];
    const char* action = add_cmd ? "Add" : "Delete";
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Group Regroup %s Command\n", action);
    DSD_FPRINTF(stderr, "  SG: %d", sg);
    if (ga1 != 0) {
        DSD_FPRINTF(stderr, " GA1: %d", ga1);
        if (add_cmd) {
            p25_patch_add_wgid(state, sg, ga1);
        } else {
            p25_patch_remove_wgid(state, sg, ga1);
        }
    }
    if (ga2 != 0) {
        DSD_FPRINTF(stderr, " GA2: %d", ga2);
        if (add_cmd) {
            p25_patch_add_wgid(state, sg, ga2);
        } else {
            p25_patch_remove_wgid(state, sg, ga2);
        }
    }
    if (ga3 != 0) {
        DSD_FPRINTF(stderr, " GA3: %d", ga3);
        if (add_cmd) {
            p25_patch_add_wgid(state, sg, ga3);
        } else {
            p25_patch_remove_wgid(state, sg, ga3);
        }
    }
    DSD_FPRINTF(stderr, "\n");
    if (add_cmd) {
        p25_patch_update(state, sg, /*is_patch*/ 1, /*active*/ 1);
    }
}

static void
tsbk_handle_mfid90_grant(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int channel = (tsbk_byte[3] << 8) | tsbk_byte[4];
    int sg = (tsbk_byte[5] << 8) | tsbk_byte[6];
    int source = (tsbk_byte[7] << 16) | (tsbk_byte[8] << 8) | tsbk_byte[9];
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Group Regroup Channel Grant\n");
    DSD_FPRINTF(stderr, "  CHAN [%04X] SG: %d SRC: %d", channel, sg, source);
    long int freq = process_channel_to_freq(opts, state, channel);
    char suf[32];
    p25_format_chan_suffix(state, (uint16_t)channel, -1, suf, sizeof suf);
    DSD_SPRINTF(state->active_channel[0], "MFID90 GRG Grant: %04X%s SG: %d; ", channel, suf, sg);
    state->last_active_time = time(NULL);
    DSD_FPRINTF(stderr, "\n");
    if (opts->p25_trunk == 1 && freq != 0) {
        p25_sm_on_group_grant(opts, state, channel, /*svc*/ 0, sg, source);
    }
}

static void
tsbk_handle_mfid90_grant_update(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int ch1 = (tsbk_byte[2] << 8) | tsbk_byte[3];
    int sg1 = (tsbk_byte[4] << 8) | tsbk_byte[5];
    int ch2 = (tsbk_byte[6] << 8) | tsbk_byte[7];
    int sg2 = (tsbk_byte[8] << 8) | tsbk_byte[9];
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Group Regroup Channel Grant Update\n");
    DSD_FPRINTF(stderr, "  CH1 [%04X] SG1: %d  CH2 [%04X] SG2: %d", ch1, sg1, ch2, sg2);
    long int freq1 = (ch1 != 0) ? process_channel_to_freq(opts, state, ch1) : 0;
    long int freq2 = (ch2 != 0) ? process_channel_to_freq(opts, state, ch2) : 0;
    char suf1[32], suf2[32];
    p25_format_chan_suffix(state, (uint16_t)ch1, -1, suf1, sizeof suf1);
    p25_format_chan_suffix(state, (uint16_t)ch2, -1, suf2, sizeof suf2);
    DSD_SPRINTF(state->active_channel[0], "MFID90 GRG Upd: %04X%s SG: %d; ", ch1, suf1, sg1);
    state->last_active_time = time(NULL);
    DSD_FPRINTF(stderr, "\n");
    if (opts->p25_trunk == 1 && ch1 != 0 && freq1 != 0) {
        p25_sm_on_group_grant(opts, state, ch1, /*svc*/ 0, sg1, /*src*/ 0);
    }
    if (opts->p25_trunk == 1 && ch2 != 0 && freq2 != 0) {
        p25_sm_on_group_grant(opts, state, ch2, /*svc*/ 0, sg2, /*src*/ 0);
    }
}

static void
tsbk_handle_mfid90_scan_marker(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int mk = (tsbk_byte[2] >> 4) & 0x0F;
    int ms = tsbk_byte[3];
    int value = (tsbk_byte[4] << 8) | tsbk_byte[5];
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Scan Marker Broadcast\n");
    DSD_FPRINTF(stderr, "  MK: %d MS: %d Value: %d\n", mk, ms, value);
}

static void
tsbk_handle_mfid90_emergency(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int source = (tsbk_byte[7] << 16) | (tsbk_byte[8] << 8) | tsbk_byte[9];
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Emergency Alarm Activation\n");
    DSD_FPRINTF(stderr, "  Source: %d", source);
    DSD_FPRINTF(stderr, " %s** EMERGENCY **%s\n", KRED, KYEL);
}

static void
tsbk_handle_mfid90_system_info(const dsd_opts* opts, const dsd_state* state,
                               const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) System Information (BSI)\n");
    DSD_FPRINTF(stderr, "  Data: %02X %02X %02X %02X %02X %02X %02X %02X", tsbk_byte[2], tsbk_byte[3], tsbk_byte[4],
                tsbk_byte[5], tsbk_byte[6], tsbk_byte[7], tsbk_byte[8], tsbk_byte[9]);
    if (opts->show_p25_callsign_decode && (state->p2_wacn != 0 || state->p2_sysid != 0)) {
        char callsign[7];
        p25_wacn_sysid_to_callsign((uint32_t)state->p2_wacn, (uint16_t)state->p2_sysid, callsign);
        DSD_FPRINTF(stderr, " [Callsign: %s]", callsign);
    }
    DSD_FPRINTF(stderr, "\n");
}

static void
tsbk_handle_mfid90(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int opcode = tsbk_byte[0] & 0x3F;
    DSD_FPRINTF(stderr, "%s", KYEL);
    switch (opcode) {
        case 0x00: tsbk_handle_mfid90_regroup_add_del(state, tsbk_byte, 1); break;
        case 0x01: tsbk_handle_mfid90_regroup_add_del(state, tsbk_byte, 0); break;
        case 0x02: tsbk_handle_mfid90_grant(opts, state, tsbk_byte); break;
        case 0x03: tsbk_handle_mfid90_grant_update(opts, state, tsbk_byte); break;
        case 0x09: tsbk_handle_mfid90_scan_marker(tsbk_byte); break;
        case 0x0A: tsbk_handle_mfid90_emergency(tsbk_byte); break;
        case 0x0B: tsbk_handle_mfid90_system_info(opts, state, tsbk_byte); break;
        default: break;
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
tsbk_handle_mfid_a4(dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    if ((tsbk_byte[0] & 0x3F) != 0x30) {
        return;
    }

    int sg = (tsbk_byte[3] << 8) | tsbk_byte[4];
    int key = (tsbk_byte[5] << 8) | tsbk_byte[6];
    int add = (tsbk_byte[7] << 16) | (tsbk_byte[8] << 8) | tsbk_byte[9];
    int tga = tsbk_byte[2] >> 5;
    int ssn = tsbk_byte[2] & 0x1F;
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n MFID A4 (Harris) Group Regroup Explicit Encryption Command\n");
    if ((tga & 0x2) == 2) {
        DSD_FPRINTF(stderr, "  SG: %d; KEY ID: %04X; WGID: %d; ", sg, key, add);
        p25_patch_add_wgid(state, sg, add);
    } else {
        DSD_FPRINTF(stderr, "  SG: %d; KEY ID: %04X; WUID: %d; ", sg, key, add);
        p25_patch_add_wuid(state, sg, (uint32_t)add);
    }
    DSD_FPRINTF(stderr, (tga & 0x4) ? " Simulselect" : " Patch");
    DSD_FPRINTF(stderr, (tga & 0x1) ? " Active;" : " Inactive;");
    DSD_FPRINTF(stderr, " SSN: %02d \n", ssn);
    int is_patch = ((tga & 0x4) == 0) ? 1 : 0;
    int active = (tga & 0x1) ? 1 : 0;
    p25_patch_update(state, sg, is_patch, active);
    p25_patch_set_kas(state, sg, key, -1, ssn);
}

static void
tsbk_handle_isp_messages(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int opcode = tsbk_byte[0] & 0x3F;
    if (opcode != 0x27) {
        return;
    }

    int group = (tsbk_byte[5] << 8) | tsbk_byte[6];
    int source = (tsbk_byte[7] << 16) | (tsbk_byte[8] << 8) | tsbk_byte[9];
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Emergency Alarm Request (ISP)\n");
    DSD_FPRINTF(stderr, "  Source: %d Group: %d", source, group);
    DSD_FPRINTF(stderr, " %s** EMERGENCY **%s", KRED, KYEL);
    DSD_FPRINTF(stderr, "\n");
}

static void
tsbk_handle_network_status(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    long int wacn = (tsbk_byte[3] << 12) | (tsbk_byte[4] << 4) | (tsbk_byte[5] >> 4);
    int sysid = ((tsbk_byte[5] & 0xF) << 8) | tsbk_byte[6];
    int channel = (tsbk_byte[7] << 8) | tsbk_byte[8];
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Network Status Broadcast TSBK - Abbreviated \n");
    DSD_FPRINTF(stderr, "  WACN [%05lX] SYSID [%03X] NAC [%03llX]", wacn, sysid, state->p2_cc);
    if (opts->show_p25_callsign_decode) {
        char callsign[7];
        p25_wacn_sysid_to_callsign((uint32_t)wacn, (uint16_t)sysid, callsign);
        DSD_FPRINTF(stderr, " [%s]", callsign);
    }
    state->p25_cc_freq = process_channel_to_freq(opts, state, channel);
    const long neigh[1] = {state->p25_cc_freq};
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

static void
tsbk_dispatch_message(dsd_opts* opts, dsd_state* state, const tsbk_decode_ctx_t* ctx, int err, int MFID, int protectbit,
                      unsigned long long int PDU[24]) {
    if (err != 0) {
        return;
    }

    if (protectbit == 1) {
        if (MFID < 0x2) {
            tsbk_handle_isp_messages(ctx->tsbk_byte);
        }
        return;
    }

    if (MFID < 0x2 && PDU[1] != 0x7B) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        process_MAC_VPDU(opts, state, 0, PDU);
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    if (MFID == 0x90) {
        tsbk_handle_mfid90(opts, state, ctx->tsbk_byte);
        return;
    }
    if (MFID == 0xA4) {
        tsbk_handle_mfid_a4(state, ctx->tsbk_byte);
        return;
    }
    if ((ctx->tsbk_byte[0] & 0x3F) == 0x3B) {
        tsbk_handle_network_status(opts, state, ctx->tsbk_byte);
    }
}

void
processTSBK(dsd_opts* opts, dsd_state* state) {
    tsbk_decode_ctx_t ctx;
    unsigned long long int PDU[24];
    DSD_MEMSET(PDU, 0, sizeof(PDU));
    tsbk_prepare_frame_state(opts, state);
    tsbk_init_decode_ctx(&ctx);
    int reps_got = tsbk_collect_repetitions(opts, state, &ctx);
    int err = tsbk_finalize_decode(state, &ctx, reps_got);
    tsbk_update_fec_counters(state, err);

    int MFID = ctx.tsbk_byte[1];
    int protectbit = (ctx.tsbk_byte[0] >> 6) & 0x1;
    tsbk_build_mac_like_pdu(ctx.tsbk_byte, PDU);
    tsbk_dispatch_message(opts, state, &ctx, err, MFID, protectbit, PDU);

    DSD_FPRINTF(stderr, "%s", KNRM);
    DSD_FPRINTF(stderr, "\n");

    p25_status_accum_classify(state, opts);

    // When on a CC, rotate the symbol out file every hour, if enabled
    rotate_symbol_out_file(opts, state);
}
