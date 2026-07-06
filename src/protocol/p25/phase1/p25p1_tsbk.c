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
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
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
#include "../p25_cc_update.h"
#include "../p25_mfid90_utils.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RTLSDR
#endif

enum {
    TSBK_MAX_BLOCKS = 3,
    TSBK_DIBITS_PER_REP = 98,
    TSBK_DIBITS_WITH_STATUS = 101,
    TSBK_SOFT_BITS_PER_REP = 196,
    TSBK_BYTES_PER_BLOCK = 12,
    TSBK_BITS_PER_BLOCK = 96
};

typedef struct {
    uint8_t tsbk_dibit[TSBK_DIBITS_PER_REP];
    int16_t tsbk_llr[TSBK_SOFT_BITS_PER_REP];
    uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK];
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
    state->dmr_so = 0;
    state->p25_service_options_valid[0] = 0;

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
tsbk_decode_block(dsd_opts* opts, dsd_state* state, int* skipdibit, tsbk_decode_ctx_t* ctx) {
    tsbk_init_decode_ctx(ctx);
    tsbk_read_repetition_samples(opts, state, skipdibit, ctx->tsbk_dibit, ctx->tsbk_llr);
    tsbk_decode_repetition_bytes(ctx->tsbk_dibit, ctx->tsbk_llr, ctx->tsbk_byte);
    tsbk_bits_from_bytes(ctx->tsbk_byte, ctx->tsbk_decoded_bits);
    return crc16_lb_bridge(ctx->tsbk_decoded_bits, 80);
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
    int service_options = tsbk_byte[2];
    int channel = (tsbk_byte[3] << 8) | tsbk_byte[4];
    int sg = (tsbk_byte[5] << 8) | tsbk_byte[6];
    int source_address = (tsbk_byte[7] << 16) | (tsbk_byte[8] << 8) | tsbk_byte[9];
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Group Regroup Channel Grant\n");
    DSD_FPRINTF(stderr, "  SVC [%02X] CHAN [%04X] SG: %d SRC: %d", service_options, channel, sg, source_address);
    long int freq = process_channel_to_freq(opts, state, channel);
    char suf[32];
    p25_format_chan_suffix(state, (uint16_t)channel, -1, suf, sizeof suf);
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "MFID90 GRG Grant: %04X%s SG: %d; ",
                 channel, suf, sg);
    state->last_active_time = time(NULL);
    DSD_FPRINTF(stderr, "\n");
    if (opts->p25_trunk == 1 && freq != 0) {
        p25_sm_seed_cc_from_current_tuner_if_unknown(opts, state);
        p25_sm_on_group_grant(opts, state, channel, service_options, sg, source_address);
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
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "MFID90 GRG Upd: %04X%s SG: %d; ", ch1,
                 suf1, sg1);
    state->last_active_time = time(NULL);
    DSD_FPRINTF(stderr, "\n");
    if (opts->p25_trunk == 1 && ch1 != 0 && freq1 != 0) {
        p25_sm_seed_cc_from_current_tuner_if_unknown(opts, state);
        p25_sm_on_group_grant(opts, state, ch1, P25_SM_SVC_UNKNOWN, sg1, /*src*/ 0);
    }
    if (opts->p25_trunk == 1 && ch2 != 0 && freq2 != 0) {
        p25_sm_seed_cc_from_current_tuner_if_unknown(opts, state);
        p25_sm_on_group_grant(opts, state, ch2, P25_SM_SVC_UNKNOWN, sg2, /*src*/ 0);
    }
}

static uint16_t
tsbk_u16(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK], int offset) {
    return (uint16_t)((tsbk_byte[offset] << 8) | tsbk_byte[offset + 1]);
}

static int
tsbk_u24(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK], int offset) {
    return (tsbk_byte[offset] << 16) | (tsbk_byte[offset + 1] << 8) | tsbk_byte[offset + 2];
}

static int
tsbk_handle_standard_osp_data_channel(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    uint8_t opcode = (uint8_t)(tsbk_byte[0] & 0x3F);

    switch (opcode) {
        case 0x10: {
            uint16_t channel = tsbk_u16(tsbk_byte, 2);
            int target = tsbk_u24(tsbk_byte, 4);
            int source = tsbk_u24(tsbk_byte, 7);
            DSD_FPRINTF(stderr, "\n Individual Data Channel Grant - Obsolete");
            DSD_FPRINTF(stderr, "\n  CHAN [%04X] Target [%d] Source [%d]", channel, target, source);
            if (channel != 0) {
                (void)process_channel_to_freq(opts, state, channel);
            }
            return 1;
        }
        case 0x11: {
            uint8_t svc = tsbk_byte[2];
            uint16_t channel = tsbk_u16(tsbk_byte, 3);
            uint16_t group = tsbk_u16(tsbk_byte, 5);
            int source = tsbk_u24(tsbk_byte, 7);
            DSD_FPRINTF(stderr, "\n Group Data Channel Grant - Obsolete");
            DSD_FPRINTF(stderr, "\n  SVC [%02X] CHAN [%04X] Group [%d][%04X] Source [%d]", svc, channel, group, group,
                        source);
            if (channel != 0) {
                (void)process_channel_to_freq(opts, state, channel);
            }
            return 1;
        }
        case 0x12: {
            uint16_t channel_a = tsbk_u16(tsbk_byte, 2);
            uint16_t group_a = tsbk_u16(tsbk_byte, 4);
            uint16_t channel_b = tsbk_u16(tsbk_byte, 6);
            uint16_t group_b = tsbk_u16(tsbk_byte, 8);
            DSD_FPRINTF(stderr, "\n Group Data Channel Announcement - Obsolete");
            DSD_FPRINTF(stderr, "\n  CHAN-A [%04X] Group-A [%d][%04X] CHAN-B [%04X] Group-B [%d][%04X]", channel_a,
                        group_a, group_a, channel_b, group_b, group_b);
            if (channel_a != 0) {
                (void)process_channel_to_freq(opts, state, channel_a);
            }
            if (channel_b != 0) {
                (void)process_channel_to_freq(opts, state, channel_b);
            }
            return 1;
        }
        case 0x13: {
            uint8_t svc = tsbk_byte[2];
            uint8_t reserved = tsbk_byte[3];
            uint16_t channel_t = tsbk_u16(tsbk_byte, 4);
            uint16_t channel_r = tsbk_u16(tsbk_byte, 6);
            uint16_t group = tsbk_u16(tsbk_byte, 8);
            DSD_FPRINTF(stderr, "\n Group Data Channel Announcement Explicit - Obsolete");
            DSD_FPRINTF(stderr, "\n  SVC [%02X] RES [%02X] CHAN-T [%04X] CHAN-R [%04X] Group [%d][%04X]", svc, reserved,
                        channel_t, channel_r, group, group);
            if (channel_t != 0) {
                (void)process_channel_to_freq(opts, state, channel_t);
            }
            if (channel_r != 0) {
                (void)process_channel_to_freq(opts, state, channel_r);
            }
            return 1;
        }
        default: break;
    }
    return 0;
}

static const char*
tsbk_queued_reason_str(uint8_t code) {
    switch (code) {
        case 0x10: return "Requesting Unit Busy Other Service";
        case 0x20: return "Target Unit Busy Other Service";
        case 0x2F: return "Target Unit Queued This Call";
        case 0x30: return "Target Group Currently Active";
        case 0x40: return "Channel Resources Unavailable";
        case 0x41: return "Telephone Resources Unavailable";
        case 0x42: return "Data Resources Unavailable";
        case 0x50: return "Superseding Service Currently Active";
        default: break;
    }

    if (code <= 0x7F) {
        return "Reserved";
    }
    return "User/System Defined";
}

static const char*
tsbk_deny_reason_str(uint8_t code) {
    static const struct {
        uint8_t code;
        const char* text;
    } k_deny_reasons[] = {
        {0x10, "Requesting Unit Not Valid"},
        {0x11, "Requesting Unit Not Authorized"},
        {0x20, "Target Unit Not Valid"},
        {0x21, "Target Unit Not Authorized"},
        {0x2F, "Target Unit Refused Call"},
        {0x30, "Target Group Not Valid"},
        {0x31, "Target Group Not Authorized"},
        {0x40, "Invalid Dialing"},
        {0x41, "Telephone Number Not Authorized"},
        {0x42, "PSTN Not Valid"},
        {0x50, "Call Timeout"},
        {0x51, "Landline Terminated Call"},
        {0x52, "Subscriber Unit Terminated Call"},
        {0x5F, "Call Preempted"},
        {0x60, "Site Access Denial"},
        {0x67, "PTT Collide"},
        {0x77, "PTT Bonk"},
        {0xF0, "Call Options Not Valid For Service"},
        {0xF1, "Protection Service Option Not Valid"},
        {0xF2, "Duplex Service Option Not Valid"},
        {0xF3, "Circuit/Packet Mode Option Not Valid"},
        {0xFF, "System Does Not Support Service"},
    };

    for (size_t i = 0; i < sizeof(k_deny_reasons) / sizeof(k_deny_reasons[0]); i++) {
        if (k_deny_reasons[i].code == code) {
            return k_deny_reasons[i].text;
        }
    }

    if (code <= 0x5E) {
        return "Reserved";
    }
    return "User/System Defined";
}

static void
tsbk_handle_mfid90_extended_function(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int class_id = tsbk_byte[2];
    int operand = tsbk_byte[3];
    int argument = tsbk_u24(tsbk_byte, 4);
    int target = tsbk_u24(tsbk_byte, 7);

    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Extended Function Command\n");
    DSD_FPRINTF(stderr, "  Class [%02X] Operand [%02X] Arg [%06X] Target [%d]", class_id, operand, argument, target);
    if (class_id == 0x02 && operand == 0x00) {
        DSD_FPRINTF(stderr, " Create Supergroup");
    } else if (class_id == 0x02 && operand == 0x01) {
        DSD_FPRINTF(stderr, " Cancel Supergroup");
    }
    DSD_FPRINTF(stderr, "\n");
}

static void
tsbk_handle_mfid90_traffic_channel_id(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Traffic Channel ID\n");
    DSD_FPRINTF(stderr, "  MSG: %02X%02X%02X%02X%02X%02X%02X%02X\n", tsbk_byte[2], tsbk_byte[3], tsbk_byte[4],
                tsbk_byte[5], tsbk_byte[6], tsbk_byte[7], tsbk_byte[8], tsbk_byte[9]);
}

static void
tsbk_handle_mfid90_queued_deny(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK],
                               int is_deny) {
    int has_additional = ((tsbk_byte[2] & 0x80) != 0) || is_deny;
    int svc_type = tsbk_byte[2] & 0x3F;
    int reason_code = tsbk_byte[3];
    int addl_info = tsbk_u24(tsbk_byte, 4);
    int target_addr = tsbk_u24(tsbk_byte, 7);
    const char* reason_str =
        is_deny ? tsbk_deny_reason_str((uint8_t)reason_code) : tsbk_queued_reason_str((uint8_t)reason_code);

    DSD_FPRINTF(stderr, "\n MFID90 (Moto) %s Response\n", is_deny ? "Deny" : "Queued");
    DSD_FPRINTF(stderr, "  SVC [%02X] Reason [%s]", svc_type, reason_str);
    if (has_additional) {
        DSD_FPRINTF(stderr, " Addl [%06X]", addl_info);
        DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]),
                     "MOT %s Target: %d Reason: %s Info: %06X; ", is_deny ? "DENY" : "QUEUED", target_addr, reason_str,
                     addl_info);
    } else {
        DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "MOT %s Target: %d Reason: %s; ",
                     is_deny ? "DENY" : "QUEUED", target_addr, reason_str);
    }
    DSD_FPRINTF(stderr, " Target [%d]\n", target_addr);
    state->last_active_time = time(NULL);

    if (is_deny) {
        p25_sm_on_deny_response(opts, state, svc_type, reason_code, target_addr);
    } else {
        p25_sm_on_queued_response(opts, state, svc_type, reason_code, target_addr);
    }
}

static void
tsbk_handle_mfid90_ack(dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int svc_type = tsbk_byte[2] & 0x3F;
    int source = tsbk_u24(tsbk_byte, 4);
    int target = tsbk_u24(tsbk_byte, 7);

    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Acknowledge Response\n");
    DSD_FPRINTF(stderr, "  Service [%02X] Source [%d] Target [%d]\n", svc_type, source, target);
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]),
                 "MOT ACK Target: %d Source: %d Service: %02X; ", target, source, svc_type);
    state->last_active_time = time(NULL);
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
tsbk_handle_mfid90_base_station_id(const dsd_opts* opts, const dsd_state* state,
                                   const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    char cwid[9];
    uint16_t channel = 0;
    (void)p25_mfid90_base_station_id_decode(tsbk_byte, cwid, sizeof cwid, &channel);

    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Control Channel Base Station ID\n");
    DSD_FPRINTF(stderr, "  CHAN [%04X]", channel);
    if (channel != 0) {
        char suf[32];
        p25_format_chan_suffix(state, channel, -1, suf, sizeof suf);
        DSD_FPRINTF(stderr, "%s", suf);
        if (state && channel < DSD_TRUNK_CHAN_MAP_SIZE && state->trunk_chan_map[channel] > 0) {
            DSD_FPRINTF(stderr, " Freq: %.6lf MHz", (double)state->trunk_chan_map[channel] / 1000000.0);
        }
    }
    if (cwid[0] != '\0') {
        DSD_FPRINTF(stderr, " CWID: %s", cwid);
    } else {
        DSD_FPRINTF(stderr, " CWID: none");
    }
    if (opts && state && opts->frontend_display.show_p25_callsign_decode
        && (state->p2_wacn != 0 || state->p2_sysid != 0)) {
        char callsign[7];
        p25_wacn_sysid_to_callsign((uint32_t)state->p2_wacn, (uint16_t)state->p2_sysid, callsign);
        DSD_FPRINTF(stderr, " Network Callsign: %s", callsign);
    }
    DSD_FPRINTF(stderr, "\n");
}

static void
tsbk_handle_mfid90_display_raw(const char* label, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) %s\n", label);
    DSD_FPRINTF(stderr, "  MSG: %02X%02X%02X%02X%02X%02X%02X%02X\n", tsbk_byte[2], tsbk_byte[3], tsbk_byte[4],
                tsbk_byte[5], tsbk_byte[6], tsbk_byte[7], tsbk_byte[8], tsbk_byte[9]);
}

static int
tsbk_mfid90_data_channel_valid(uint16_t channel) {
    return (((channel >> 12) & 0x0F) != 0x0F) && ((channel & 0x0FFF) != 0x0FFF);
}

static void
tsbk_handle_mfid90_tdma_data_channel(const dsd_opts* opts, dsd_state* state,
                                     const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    uint16_t downlink = tsbk_u16(tsbk_byte, 4);
    uint16_t uplink = tsbk_u16(tsbk_byte, 6);
    int downlink_valid = tsbk_mfid90_data_channel_valid(downlink);
    int uplink_valid = tsbk_mfid90_data_channel_valid(uplink);
    long int downlink_freq = downlink_valid ? process_channel_to_freq(opts, state, downlink) : 0;
    long int uplink_freq = uplink_valid ? process_channel_to_freq(opts, state, uplink) : 0;

    DSD_FPRINTF(stderr, "\n MFID90 (Moto) TDMA Data Channel\n");
    if (downlink_valid) {
        char suffix[32];
        p25_format_chan_suffix(state, downlink, -1, suffix, sizeof suffix);
        DSD_FPRINTF(stderr, "  DL [%04X]%s", downlink, suffix);
        if (downlink_freq > 0) {
            DSD_FPRINTF(stderr, " Freq: %.6lf MHz", (double)downlink_freq / 1000000.0);
        }
    }
    if (uplink_valid) {
        char suffix[32];
        p25_format_chan_suffix(state, uplink, -1, suffix, sizeof suffix);
        DSD_FPRINTF(stderr, "%sUL [%04X]%s", downlink_valid ? " " : "  ", uplink, suffix);
        if (uplink_freq > 0) {
            DSD_FPRINTF(stderr, " Freq: %.6lf MHz", (double)uplink_freq / 1000000.0);
        }
    }
    if (!downlink_valid && !uplink_valid) {
        DSD_FPRINTF(stderr, "  Not Active");
        DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "MOT TDMA Data: Not Active; ");
    } else if (downlink_valid && uplink_valid) {
        char downlink_suffix[32], uplink_suffix[32];
        p25_format_chan_suffix(state, downlink, -1, downlink_suffix, sizeof downlink_suffix);
        p25_format_chan_suffix(state, uplink, -1, uplink_suffix, sizeof uplink_suffix);
        DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "MOT TDMA Data: DL %04X%s UL %04X%s; ",
                     downlink, downlink_suffix, uplink, uplink_suffix);
    } else {
        uint16_t channel = downlink_valid ? downlink : uplink;
        char suffix[32];
        p25_format_chan_suffix(state, channel, -1, suffix, sizeof suffix);
        DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "MOT TDMA Data: %04X%s; ", channel,
                     suffix);
    }
    state->last_active_time = time(NULL);
    DSD_FPRINTF(stderr, "\n");
}

static int
tsbk_handle_mfid90_regroup_grants(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK],
                                  int opcode) {
    switch (opcode) {
        case 0x00: tsbk_handle_mfid90_regroup_add_del(state, tsbk_byte, 1); break;
        case 0x01: tsbk_handle_mfid90_regroup_add_del(state, tsbk_byte, 0); break;
        case 0x02: tsbk_handle_mfid90_grant(opts, state, tsbk_byte); break;
        case 0x03: tsbk_handle_mfid90_grant_update(opts, state, tsbk_byte); break;
        case 0x04: tsbk_handle_mfid90_extended_function(tsbk_byte); break;
        case 0x05: tsbk_handle_mfid90_traffic_channel_id(tsbk_byte); break;
        default: return 0;
    }
    return 1;
}

static int
tsbk_handle_mfid90_responses(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK],
                             int opcode) {
    switch (opcode) {
        case 0x06: tsbk_handle_mfid90_queued_deny(opts, state, tsbk_byte, /*is_deny*/ 0); break;
        case 0x07: tsbk_handle_mfid90_queued_deny(opts, state, tsbk_byte, /*is_deny*/ 1); break;
        case 0x08: tsbk_handle_mfid90_ack(state, tsbk_byte); break;
        case 0x09: tsbk_handle_mfid90_scan_marker(tsbk_byte); break;
        case 0x0A: tsbk_handle_mfid90_emergency(tsbk_byte); break;
        case 0x0B: tsbk_handle_mfid90_base_station_id(opts, state, tsbk_byte); break;
        default: return 0;
    }
    return 1;
}

static void
tsbk_handle_mfid90_misc(const dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK],
                        int opcode) {
    switch (opcode) {
        case 0x0E: tsbk_handle_mfid90_display_raw("Control Channel Planned Shutdown", tsbk_byte); break;
        case 0x0F: tsbk_handle_mfid90_display_raw("Opcode 15", tsbk_byte); break;
        case 0x16: tsbk_handle_mfid90_tdma_data_channel(opts, state, tsbk_byte); break;
        default: break;
    }
}

static void
tsbk_handle_mfid90(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int opcode = tsbk_byte[0] & 0x3F;
    DSD_FPRINTF(stderr, "%s", KYEL);
    if (!tsbk_handle_mfid90_regroup_grants(opts, state, tsbk_byte, opcode)
        && !tsbk_handle_mfid90_responses(opts, state, tsbk_byte, opcode)) {
        tsbk_handle_mfid90_misc(opts, state, tsbk_byte, opcode);
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

static uint32_t
tsbk_wacn_from_24(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    return ((uint32_t)tsbk_byte[3] << 12) | ((uint32_t)tsbk_byte[4] << 4) | ((uint32_t)tsbk_byte[5] >> 4);
}

static uint16_t
tsbk_sys_from_44(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    return (uint16_t)(((uint16_t)(tsbk_byte[5] & 0x0F) << 8) | (uint16_t)tsbk_byte[6]);
}

static void
tsbk_isp_print_src_tgt(const char* label, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK], const char* target_label) {
    int target = tsbk_u24(tsbk_byte, 4);
    int source = tsbk_u24(tsbk_byte, 7);
    DSD_FPRINTF(stderr, "\n %s (ISP protected/inbound)", label);
    DSD_FPRINTF(stderr, " FM [%d] %s [%d]", source, target_label ? target_label : "TO", target);
}

static void
tsbk_isp_print_group_request(const char* label, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK], int has_service) {
    int svc = tsbk_byte[2];
    int group = tsbk_u16(tsbk_byte, 5);
    int source = tsbk_u24(tsbk_byte, 7);
    DSD_FPRINTF(stderr, "\n %s (ISP protected/inbound)", label);
    DSD_FPRINTF(stderr, " FM [%d] Group [%d][%04X]", source, group, group);
    if (has_service) {
        DSD_FPRINTF(stderr, " SVC [%02X]", svc);
    }
}

static void
tsbk_isp_print_status(const char* label, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK], int has_status) {
    int target = tsbk_u24(tsbk_byte, 4);
    int source = tsbk_u24(tsbk_byte, 7);
    DSD_FPRINTF(stderr, "\n %s (ISP protected/inbound)", label);
    DSD_FPRINTF(stderr, " FM [%d] TO [%d]", source, target);
    if (has_status) {
        DSD_FPRINTF(stderr, " UNIT STATUS [%02X] USER STATUS [%02X]", tsbk_byte[2], tsbk_byte[3]);
    }
}

static void
tsbk_isp_print_wacn_sys_src(const char* label, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    uint32_t wacn = tsbk_wacn_from_24(tsbk_byte);
    uint16_t sysid = tsbk_sys_from_44(tsbk_byte);
    int source = tsbk_u24(tsbk_byte, 7);
    DSD_FPRINTF(stderr, "\n %s (ISP protected/inbound)", label);
    DSD_FPRINTF(stderr, " FM [%d] WACN [%05X] SYSID [%03X]", source, wacn, sysid);
}

static int
tsbk_handle_isp_service_messages(uint8_t opcode, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    switch (opcode) {
        case 0x00: tsbk_isp_print_group_request("Group Voice Service Request", tsbk_byte, 1); return 1;
        case 0x04:
            tsbk_isp_print_src_tgt("Unit-to-Unit Voice Service Request", tsbk_byte, "TO");
            DSD_FPRINTF(stderr, " SVC [%02X]", tsbk_byte[2]);
            return 1;
        case 0x05:
            tsbk_isp_print_src_tgt("Unit-to-Unit Answer Response", tsbk_byte, "TO");
            DSD_FPRINTF(stderr, " SVC [%02X] RESPONSE [%02X]", tsbk_byte[2], tsbk_byte[3]);
            return 1;
        case 0x08:
            tsbk_isp_print_src_tgt("Telephone Interconnect Explicit Dial Request", tsbk_byte, "TO");
            DSD_FPRINTF(stderr, " SVC [%02X]", tsbk_byte[2]);
            return 1;
        case 0x09:
            tsbk_isp_print_src_tgt("Telephone Interconnect PSTN Request", tsbk_byte, "TO");
            DSD_FPRINTF(stderr, " SVC [%02X]", tsbk_byte[2]);
            return 1;
        case 0x0A:
            tsbk_isp_print_src_tgt("Telephone Interconnect Answer Response", tsbk_byte, "TO");
            DSD_FPRINTF(stderr, " SVC [%02X] RESPONSE [%02X]", tsbk_byte[2], tsbk_byte[3]);
            return 1;
        case 0x10:
            tsbk_isp_print_src_tgt("Individual Data Service Request", tsbk_byte, "TO");
            DSD_FPRINTF(stderr, " SVC [%02X]", tsbk_byte[2]);
            return 1;
        case 0x11: tsbk_isp_print_group_request("Group Data Service Request", tsbk_byte, 1); return 1;
        case 0x12:
            DSD_FPRINTF(stderr, "\n SNDCP Data Channel Request (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%d] DSO [%02X] DAC [%04X]", tsbk_u24(tsbk_byte, 7), tsbk_byte[2],
                        tsbk_u16(tsbk_byte, 3));
            return 1;
        case 0x13:
            DSD_FPRINTF(stderr, "\n SNDCP Data Page Response (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%d] DSO [%02X] RESPONSE [%02X] DAC [%04X]", tsbk_u24(tsbk_byte, 7), tsbk_byte[2],
                        tsbk_byte[3], tsbk_u16(tsbk_byte, 4));
            return 1;
        case 0x14:
            DSD_FPRINTF(stderr, "\n SNDCP Reconnect Request (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%d] DSO [%02X] DAC [%04X] DATA_TO_SEND [%d]", tsbk_u24(tsbk_byte, 7),
                        tsbk_byte[2], tsbk_u16(tsbk_byte, 3), (tsbk_byte[5] & 0x80) ? 1 : 0);
            return 1;
        default: return 0;
    }
}

static int
tsbk_handle_isp_status_control_messages(uint8_t opcode, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    switch (opcode) {
        case 0x18: tsbk_isp_print_status("Status Update Request", tsbk_byte, 1); return 1;
        case 0x19: tsbk_isp_print_status("Status Query Response", tsbk_byte, 1); return 1;
        case 0x1A: tsbk_isp_print_status("Status Query Request", tsbk_byte, 0); return 1;
        case 0x1C:
            tsbk_isp_print_src_tgt("Message Update Request", tsbk_byte, "TO");
            DSD_FPRINTF(stderr, " SHORT DATA [%04X]", tsbk_u16(tsbk_byte, 2));
            return 1;
        case 0x1F: tsbk_isp_print_src_tgt("Call Alert Request", tsbk_byte, "TO"); return 1;
        case 0x20:
            tsbk_isp_print_src_tgt("Unit Acknowledge Response", tsbk_byte, "TO");
            DSD_FPRINTF(stderr, " ACK SVC [%02X]", tsbk_byte[2] & 0x3F);
            return 1;
        case 0x23:
            DSD_FPRINTF(stderr, "\n Cancel Service Request (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%d] VALID [%d] SVC [%02X] REASON [%02X] INFO [%06X]", tsbk_u24(tsbk_byte, 7),
                        (tsbk_byte[2] & 0x80) ? 1 : 0, tsbk_byte[2] & 0x3F, tsbk_byte[3], tsbk_u24(tsbk_byte, 4));
            return 1;
        case 0x24:
            DSD_FPRINTF(stderr, "\n Extended Function Response (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%d] FUNC [%04X] ARG [%06X]", tsbk_u24(tsbk_byte, 7), tsbk_u16(tsbk_byte, 2),
                        tsbk_u24(tsbk_byte, 4));
            return 1;
        case 0x27: {
            int group = tsbk_u16(tsbk_byte, 5);
            int source = tsbk_u24(tsbk_byte, 7);
            DSD_FPRINTF(stderr, "\n Emergency Alarm Request (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " Source [%d] Group [%d][%04X]", source, group, group);
            DSD_FPRINTF(stderr, " %s** EMERGENCY **%s", KRED, KYEL);
            return 1;
        }
        default: return 0;
    }
}

static int
tsbk_handle_isp_registration_messages(uint8_t opcode, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    switch (opcode) {
        case 0x28: {
            int sysid = ((tsbk_byte[3] & 0x0F) << 8) | tsbk_byte[4];
            int group = tsbk_u16(tsbk_byte, 5);
            int source = tsbk_u24(tsbk_byte, 7);
            DSD_FPRINTF(stderr, "\n Group Affiliation Request (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%d] SYSID [%03X] Group [%d][%04X]", source, sysid, group, group);
            return 1;
        }
        case 0x29:
            DSD_FPRINTF(stderr, "\n Group Affiliation Query Response (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%d] Announcement Group [%d][%04X] Group [%d][%04X]", tsbk_u24(tsbk_byte, 7),
                        tsbk_u16(tsbk_byte, 3), tsbk_u16(tsbk_byte, 3), tsbk_u16(tsbk_byte, 5), tsbk_u16(tsbk_byte, 5));
            return 1;
        case 0x2B: tsbk_isp_print_wacn_sys_src("Unit De-Registration Request", tsbk_byte); return 1;
        case 0x2C:
            tsbk_isp_print_wacn_sys_src("Unit Registration Request", tsbk_byte);
            DSD_FPRINTF(stderr, " EMERGENCY [%d] CAPABILITY [%02X]", (tsbk_byte[2] & 0x80) ? 1 : 0,
                        tsbk_byte[2] & 0x7F);
            return 1;
        case 0x2D:
            DSD_FPRINTF(stderr, "\n Location Registration Request (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%d] EMERGENCY [%d] CAPABILITY [%02X] LRA [%02X] Group [%d][%04X]",
                        tsbk_u24(tsbk_byte, 7), (tsbk_byte[2] & 0x80) ? 1 : 0, tsbk_byte[2] & 0x7F, tsbk_byte[4],
                        tsbk_u16(tsbk_byte, 5), tsbk_u16(tsbk_byte, 5));
            return 1;
        case 0x30: tsbk_isp_print_wacn_sys_src("Protection Parameter Request", tsbk_byte); return 1;
        case 0x32: tsbk_isp_print_wacn_sys_src("Identifier/Frequency Band Update Request", tsbk_byte); return 1;
        default: return 0;
    }
}

static int
tsbk_handle_isp_auth_roaming_messages(uint8_t opcode, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    switch (opcode) {
        case 0x2E:
        case 0x2F:
            tsbk_isp_print_src_tgt(opcode == 0x2E ? "Authentication Query (obsolete)"
                                                  : "Authentication Response (obsolete)",
                                   tsbk_byte, "TO");
            return 1;
        case 0x36: tsbk_isp_print_src_tgt("Roaming Address Request", tsbk_byte, "TO"); return 1;
        case 0x37:
            tsbk_isp_print_wacn_sys_src("Roaming Address Response", tsbk_byte);
            DSD_FPRINTF(stderr, " MSN [%d] FINAL [%d]", tsbk_byte[2] & 0x0F, (tsbk_byte[2] & 0x80) ? 1 : 0);
            return 1;
        case 0x38:
        case 0x39:
        case 0x3A:
        case 0x3B:
            DSD_FPRINTF(stderr, "\n Authentication Message (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " OP [%02X] SRC [%d] DATA [%02X%02X%02X%02X%02X%02X%02X%02X]", opcode,
                        tsbk_u24(tsbk_byte, 7), tsbk_byte[2], tsbk_byte[3], tsbk_byte[4], tsbk_byte[5], tsbk_byte[6],
                        tsbk_byte[7], tsbk_byte[8], tsbk_byte[9]);
            return 1;
        default: return 0;
    }
}

static void
tsbk_isp_print_unsupported(uint8_t opcode, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    DSD_FPRINTF(stderr, "\n Unsupported ISP opcode (protected/inbound) OP [%02X]", opcode);
    DSD_FPRINTF(stderr, " DATA [%02X%02X%02X%02X%02X%02X%02X%02X]", tsbk_byte[2], tsbk_byte[3], tsbk_byte[4],
                tsbk_byte[5], tsbk_byte[6], tsbk_byte[7], tsbk_byte[8], tsbk_byte[9]);
}

static void
tsbk_handle_isp_messages(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    uint8_t opcode = (uint8_t)(tsbk_byte[0] & 0x3F);
    DSD_FPRINTF(stderr, "%s", KYEL);

    /*
     * Standard protected TSBKs carry inbound ISP messages. Field offsets below
     * mirror SDRTrunk's phase1/message/tsbk/standard/isp classes. These are
     * subscriber-to-network requests/responses, so this path logs metadata only
     * and intentionally does not retune or feed the outbound grant state machine.
     */
    int handled = tsbk_handle_isp_service_messages(opcode, tsbk_byte);
    if (!handled) {
        handled = tsbk_handle_isp_status_control_messages(opcode, tsbk_byte);
    }
    if (!handled) {
        handled = tsbk_handle_isp_registration_messages(opcode, tsbk_byte);
    }
    if (!handled) {
        handled = tsbk_handle_isp_auth_roaming_messages(opcode, tsbk_byte);
    }
    if (!handled) {
        tsbk_isp_print_unsupported(opcode, tsbk_byte);
    }
    DSD_FPRINTF(stderr, "\n");
}

static void
tsbk_handle_mfid90_isp_messages(const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    uint8_t opcode = (uint8_t)(tsbk_byte[0] & 0x3F);
    DSD_FPRINTF(stderr, "%s", KYEL);

    switch (opcode) {
        case 0x00: {
            uint8_t svc = tsbk_byte[2];
            uint16_t sg = tsbk_u16(tsbk_byte, 5);
            uint32_t source = (uint32_t)tsbk_u24(tsbk_byte, 7);
            DSD_FPRINTF(stderr, "\n MFID90 (Moto) Group Regroup Voice Request (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%u] SG [%u][%04X] SVC [%02X]", source, sg, sg, svc);
            break;
        }
        case 0x01: {
            uint16_t function = tsbk_u16(tsbk_byte, 2);
            uint32_t argument = (uint32_t)tsbk_u24(tsbk_byte, 4);
            uint32_t source = (uint32_t)tsbk_u24(tsbk_byte, 7);
            DSD_FPRINTF(stderr, "\n MFID90 (Moto) Extended Function Response (ISP protected/inbound)");
            DSD_FPRINTF(stderr, " FM [%u] FUNC [%04X] ARG [%06X]", source, function, argument);
            break;
        }
        default:
            DSD_FPRINTF(stderr, "\n Unsupported MFID90 ISP opcode (protected/inbound) OP [%02X]", opcode);
            DSD_FPRINTF(stderr, " DATA [%02X%02X%02X%02X%02X%02X%02X%02X]", tsbk_byte[2], tsbk_byte[3], tsbk_byte[4],
                        tsbk_byte[5], tsbk_byte[6], tsbk_byte[7], tsbk_byte[8], tsbk_byte[9]);
            break;
    }

    DSD_FPRINTF(stderr, "\n%s", KNRM);
}

static void
tsbk_handle_network_status(dsd_opts* opts, dsd_state* state, const uint8_t tsbk_byte[TSBK_BYTES_PER_BLOCK]) {
    int lra = tsbk_byte[2];
    long int wacn = (tsbk_byte[3] << 12) | (tsbk_byte[4] << 4) | (tsbk_byte[5] >> 4);
    int sysid = ((tsbk_byte[5] & 0xF) << 8) | tsbk_byte[6];
    int channel = (tsbk_byte[7] << 8) | tsbk_byte[8];
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Network Status Broadcast TSBK - Abbreviated \n");
    DSD_FPRINTF(stderr, "  LRA [%02X] WACN [%05lX] SYSID [%03X] NAC [%03llX]", lra, wacn, sysid, state->p2_cc);
    if (opts->frontend_display.show_p25_callsign_decode) {
        char callsign[7];
        p25_wacn_sysid_to_callsign((uint32_t)wacn, (uint16_t)sysid, callsign);
        DSD_FPRINTF(stderr, " [%s]", callsign);
    }
    long int cc_freq = process_channel_to_freq(opts, state, channel);
    int accepted_cc = p25_cc_update_primary_from_network_status(opts, state, cc_freq);
    const int cc_metadata_allowed = accepted_cc || !p25_cc_update_is_voice_tuned(opts);
    if (cc_metadata_allowed) {
        if (state->p2_hardset == 0) {
            (void)p25_update_system_identity(state, (unsigned long long)wacn, (unsigned long long)sysid);
        }

        p25_store_site_lra(state, (uint8_t)lra);
        state->p25_cc_is_tdma = 0;
    }
    if (accepted_cc) {
        const long neigh[1] = {state->p25_cc_freq};
        p25_sm_on_neighbor_update(opts, state, neigh, 1);
        if (state->trunk_lcn_freq[0] == 0 || state->trunk_lcn_freq[0] != state->p25_cc_freq) {
            state->trunk_lcn_freq[0] = state->p25_cc_freq;
        }
        p25_confirm_idens_for_current_site(state);
    } else if (cc_freq > 0) {
        DSD_FPRINTF(stderr, "\n  P25 TSBK NET_STS: ignoring CC update while voice-tuned (freq=%ld)", cc_freq);
    } else {
        DSD_FPRINTF(stderr, "\n  P25 TSBK NET_STS: ignoring invalid channel->freq (CHAN-T=%04X)", channel);
    }
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
        } else if (MFID == 0x90) {
            tsbk_handle_mfid90_isp_messages(ctx->tsbk_byte);
        }
        return;
    }

    if (MFID < 0x2 && PDU[1] != 0x7B) {
        if (tsbk_handle_standard_osp_data_channel(opts, state, ctx->tsbk_byte)) {
            return;
        }
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
    tsbk_prepare_frame_state(opts, state);

    int skipdibit = 36 - 14;
    for (int block = 0; block < TSBK_MAX_BLOCKS; block++) {
        tsbk_decode_ctx_t ctx;
        unsigned long long int PDU[24];
        DSD_MEMSET(PDU, 0, sizeof(PDU));

        int err = tsbk_decode_block(opts, state, &skipdibit, &ctx);
        tsbk_update_fec_counters(state, err);

        int MFID = ctx.tsbk_byte[1];
        int protectbit = (ctx.tsbk_byte[0] >> 6) & 0x1;
        int last_block = (ctx.tsbk_byte[0] >> 7) & 0x1;
        tsbk_build_mac_like_pdu(ctx.tsbk_byte, PDU);
        tsbk_dispatch_message(opts, state, &ctx, err, MFID, protectbit, PDU);

        if (last_block) {
            break;
        }
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
    DSD_FPRINTF(stderr, "\n");

    p25_status_accum_classify(state, opts);

    // When on a CC, rotate the symbol out file every hour, if enabled
    rotate_symbol_out_file(opts, state);
}
