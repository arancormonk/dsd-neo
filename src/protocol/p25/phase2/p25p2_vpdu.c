// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * p25p2_vpdu.c
 * Phase 2 Variable PDU (and TSBK PDU) Handling
 *
 * LWVMOBILE
 * 2022-10 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p2_mac_parse.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../p25_cc_update.h"
#include "../p25_extended_function.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static inline void dsd_append(char* dst, size_t dstsz, const char* src);

#if defined(__GNUC__) || defined(__clang__)
#define VPDU_MAYBE_UNUSED __attribute__((unused))
#define VPDU_LABEL_UNUSED __attribute__((unused))
#else
#define VPDU_MAYBE_UNUSED
#define VPDU_LABEL_UNUSED
#endif

// Expose MAC helpers for tests and diagnostics.

static void
p25p2_vpdu_print_group_label(const dsd_state* state, uint32_t id) {
    char name[50];
    if (id != 0U && dsd_tg_policy_lookup_label(state, id, NULL, 0, name, sizeof(name))) {
        DSD_FPRINTF(stderr, " [%s]", name);
    }
}

static int
p25p2_sccb_matches_current_site(const dsd_state* state, int rfssid, int siteid) {
    if (!state) {
        return 0;
    }
    if (state->p2_rfssid != 0 && rfssid != (int)state->p2_rfssid) {
        return 0;
    }
    if (state->p2_siteid != 0 && siteid != (int)state->p2_siteid) {
        return 0;
    }
    return 1;
}

static int
p25p2_sccb_implicit_channel_b_valid(int bridged_p1, int channel1, int channel2, int sysclass2) {
    if (channel2 == channel1 || channel2 == 0xFFFF) {
        return 0;
    }
    if (bridged_p1) {
        return channel2 != 0;
    }
    return sysclass2 != 0;
}

static void
p25p2_seed_secondary_lcn_fallback(dsd_state* state, int rfssid, int siteid, const long* freqs, int count) {
    if (!state || !freqs || count <= 0 || !p25p2_sccb_matches_current_site(state, rfssid, siteid)) {
        return;
    }

    for (int i = 0; i < count && i < 2; i++) {
        long f = freqs[i];
        if (f <= 0) {
            continue;
        }

        int exists = 0;
        for (int slot = 0; slot < 3; slot++) {
            if (state->trunk_lcn_freq[slot] == f) {
                exists = 1;
                break;
            }
        }
        if (exists) {
            continue;
        }

        for (int slot = 1; slot < 3; slot++) {
            if (state->trunk_lcn_freq[slot] == 0) {
                state->trunk_lcn_freq[slot] = f;
                if (state->lcn_freq_count < slot + 1) {
                    state->lcn_freq_count = slot + 1;
                }
                break;
            }
        }
    }
}

/**
 * @brief Resolve a P25 Algorithm ID to a human-readable name.
 *
 * Common APCO P25 ALGIDs used by the voice/ESS paths:
 *   0x80 = unencrypted, 0x81 = DES-OFB, 0x84 = AES-256,
 *   0x89 = AES-128-OFB, 0x9F = DES-XL, 0xAA = ADP/RC4
 *
 * @param algid The 8-bit algorithm identifier.
 * @return Static string with algorithm name, or NULL if unrecognized.
 */
static const char*
p25_algid_name(uint8_t algid) {
    switch (algid) {
        case 0x80: return "UNENCRYPTED";
        case 0x81: return "DES-OFB";
        case 0x82: return "2-KEY 3DES";
        case 0x83: return "3-KEY 3DES";
        case 0x84: return "AES-256";
        case 0x85: return "AES-128";
        case 0x88: return "AES-CBC";
        case 0x89: return "AES-128-OFB";
        case 0x9F: return "DES-XL";
        case 0xAA: return "ADP/RC4";
        case 0xAF: return "AES-256-GCM";
        default: return NULL;
    }
}

static uint64_t
p25_mac_get_bits(const unsigned long long int MAC[24], int len_a, int start_bit, int bit_count) {
    uint64_t value = 0;
    for (int i = 0; i < bit_count; i++) {
        int bit = start_bit + i;
        int octet = 1 + len_a + (bit / 8);
        int shift = 7 - (bit % 8);
        value = (value << 1) | ((uint64_t)(MAC[octet] >> shift) & 0x1ULL);
    }
    return value;
}

static int
p25_is_leap_year(int year) {
    if ((year % 400) == 0) {
        return 1;
    }
    if ((year % 100) == 0) {
        return 0;
    }
    return (year % 4) == 0;
}

static int
p25_utc_fields_in_range(int year, int month, int day, int hours, int minutes, int seconds) {
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59 || seconds < 0 || seconds > 59) {
        return 0;
    }
    return 1;
}

static int
p25_utc_day_is_valid(int year, int month, int day) {
    static const int days_in_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = days_in_month[month - 1];
    if (month == 2 && p25_is_leap_year(year)) {
        max_day = 29;
    }
    return day <= max_day;
}

static int64_t
p25_days_from_civil_date(int year, int month, int day) {
    int64_t y = year;
    unsigned m = (unsigned)month;
    unsigned d = (unsigned)day;
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp = (m > 2) ? (m - 3U) : (m + 9U);
    unsigned doy = (153U * mp + 2U) / 5U + d - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static time_t
p25_utc_time_from_local_fields(int year, int month, int day, int hours, int minutes, int seconds, int offset_minutes) {
    if (!p25_utc_fields_in_range(year, month, day, hours, minutes, seconds)) {
        return (time_t)-1;
    }
    if (!p25_utc_day_is_valid(year, month, day)) {
        return (time_t)-1;
    }

    int64_t days = p25_days_from_civil_date(year, month, day);
    int64_t total = days * 86400 + (int64_t)hours * 3600 + (int64_t)minutes * 60 + seconds;
    total -= (int64_t)offset_minutes * 60;
    return (time_t)total;
}

/**
 * @brief Resolve a Queued Response reason code to a human-readable string.
 *
 * Reason labels mirror sdrtrunk's QueuedResponseReason mapping.
 *
 * @param code The 8-bit reason code from octet 3.
 * @return Static string describing the reason.
 */
static const char*
p25_que_reason_str(uint8_t code) {
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

/**
 * @brief Resolve a Deny Response reason code to a human-readable string.
 *
 * Reason labels mirror sdrtrunk's DenyReason mapping.
 *
 * @param code The 8-bit reason code from octet 3.
 * @return Static string describing the reason.
 */
static const char*
p25_deny_reason_str(uint8_t code) {
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

static int
p25p2_mac_policy_flag(int svc_bits, int policy_override, int bit) {
    if (policy_override >= 0) {
        return policy_override ? 1 : 0;
    }
    if (svc_bits < 0) {
        return 0;
    }
    return (svc_bits & bit) ? 1 : 0;
}

static int
p25p2_mac_group_enc_for_policy(const dsd_opts* opts, const dsd_state* state, int group, int svc_bits,
                               int policy_encrypted_override) {
    int enc_for_policy = p25p2_mac_policy_flag(svc_bits, policy_encrypted_override, 0x40);
    if (!(enc_for_policy && policy_encrypted_override < 0 && opts->trunk_tune_enc_calls == 0)) {
        return enc_for_policy;
    }
    if (p25_patch_tg_key_is_clear(state, group) || p25_patch_sg_key_is_clear(state, group)) {
        return 0;
    }
    return enc_for_policy;
}

static int
p25p2_mac_group_policy_allows(const dsd_opts* opts, const dsd_state* state, int group, int source, int enc_for_policy,
                              int data_for_policy, dsd_tg_policy_decision* decision) {
    int rc = dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)group, (uint32_t)source, enc_for_policy,
                                               data_for_policy, DSD_TG_POLICY_HOLD_COMPAT_GRANT, decision);
    return (rc == 0 && decision->tune_allowed);
}

/* Emit a compact JSON line for a P25 Phase 2 MAC PDU when enabled. */
static void
p25p2_emit_mac_json_if_enabled(const dsd_state* state, int xch_type, uint8_t mfid, uint8_t opcode, int slot, int len_b,
                               int len_c, const char* summary) {
    const dsdneoRuntimeConfig* rc = dsd_neo_get_config();
    if (!rc || !rc->pdu_json_enable) {
        return;
    }

    /* xch_type: 0 FACCH, 1 SACCH; prefer LCCH label when flagged */
    const char* xch = (state && state->p2_is_lcch) ? "LCCH" : (xch_type == 1 ? "SACCH" : "FACCH");

    /* Minimal summary sanitization (drop quotes) to keep JSON valid */
    char sum[80];
    sum[0] = '\0';
    if (summary && summary[0] != '\0') {
        int j = 0;
        for (int i = 0; summary[i] != '\0' && j < (int)sizeof(sum) - 1; i++) {
            char ch = summary[i];
            if (ch == '"') {
                continue;
            }
            sum[j++] = ch;
        }
        sum[j] = '\0';
    }

    time_t ts = time(NULL);
    DSD_FPRINTF(stderr,
                "{\"ts\":%ld,\"proto\":\"p25\",\"mac\":1,\"xch\":\"%s\",\"mfid\":%u,\"op\":%u,\"slot\":%d,\"slot1\":%d,"
                "\"lenB\":%d,\"lenC\":%d,\"summary\":\"%s\"}\n",
                (long)ts, xch, (unsigned)mfid, (unsigned)opcode, slot, slot + 1, len_b, len_c, sum);
}

/* Centralized helper for MAC-based group grants; currently a thin wrapper
 * over the Tier II/III trunking state machine so future refactors can
 * route per-opcode behavior through a single surface. */
static void
p25p2_mac_handle(const struct p25p2_mac_result* res, dsd_opts* opts, dsd_state* state, int channel, int svc_bits,
                 int group, int source, int policy_encrypted_override, int policy_data_override, int emit_enc_lockout) {
    dsd_tg_policy_decision decision;
    (void)res;
    if (!opts || !state) {
        return;
    }
    if (opts->p25_trunk != 1) {
        return;
    }

    int enc_for_policy = p25p2_mac_group_enc_for_policy(opts, state, group, svc_bits, policy_encrypted_override);
    int data_for_policy = p25p2_mac_policy_flag(svc_bits, policy_data_override, 0x10);
    if (!p25p2_mac_group_policy_allows(opts, state, group, source, enc_for_policy, data_for_policy, &decision)) {
        if (emit_enc_lockout && (decision.block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED)) {
            p25_emit_enc_lockout_once(opts, state, 0, group, svc_bits);
        }
        return;
    }

    p25_sm_on_group_grant(opts, state, channel, svc_bits, group, source);
}

static void
p25p2_mac_handle_indiv(const struct p25p2_mac_result* res, dsd_opts* opts, dsd_state* state, int channel, int svc_bits,
                       int target, int source, int policy_encrypted_override, int policy_data_override) {
    dsd_tg_policy_decision decision;
    int enc_for_policy = 0;
    int data_for_policy = 0;
    (void)res;
    if (!opts || !state) {
        return;
    }
    if (opts->p25_trunk != 1) {
        return;
    }
    enc_for_policy = p25p2_mac_policy_flag(svc_bits, policy_encrypted_override, 0x40);
    data_for_policy = p25p2_mac_policy_flag(svc_bits, policy_data_override, 0x10);
    if (dsd_tg_policy_evaluate_private_call(opts, state, (uint32_t)source, (uint32_t)target, enc_for_policy,
                                            data_for_policy, DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                            DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
            != 0
        || !decision.tune_allowed) {
        return;
    }
    p25_sm_on_indiv_grant(opts, state, channel, svc_bits, target, source);
}

static inline void
p25_set_playback_vc_freq(const dsd_opts* opts, dsd_state* state, long int freq) {
    if (!opts || !state || opts->p25_trunk != 0) {
        return;
    }

    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        state->p25_vc_freq[0] = freq;
    } else {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    }
}

static inline void
p25_set_mfid90_active_channel_single(dsd_state* state, int channel, int group) {
    if (!state) {
        return;
    }
    char suffix[32];
    p25_format_chan_suffix(state, (uint16_t)channel, -1, suffix, sizeof(suffix));
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "MFID90 Active Ch: %04X%s SG: %d; ",
                 channel, suffix, group);
    state->last_active_time = time(NULL);
}

static inline void
p25_set_mfid90_active_channel_update(dsd_state* state, int channel1, int group1, int channel2, int group2) {
    if (!state) {
        return;
    }

    if (channel2 != channel1 && channel2 != 0 && channel2 != 0xFFFF) {
        char suffix1[32];
        char suffix2[32];
        p25_format_chan_suffix(state, (uint16_t)channel1, -1, suffix1, sizeof(suffix1));
        p25_format_chan_suffix(state, (uint16_t)channel2, -1, suffix2, sizeof(suffix2));
        DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]),
                     "MFID90 Active Ch: %04X%s SG: %d; Ch: %04X%s SG: %d; ", channel1, suffix1, group1, channel2,
                     suffix2, group2);
    } else {
        p25_set_mfid90_active_channel_single(state, channel1, group1);
        return;
    }

    state->last_active_time = time(NULL);
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    int type;
    unsigned long long int* mac;
    struct p25p2_mac_result* mac_res;
    int len_a;
    int len_b;
    int len_c;
    int slot;
    int skip_rest;
    int end_pdu;
    int iter_idx;
} p25p2_vpdu_ctx;

static void p25p2_vpdu_emit_json(const p25p2_vpdu_ctx* ctx);
static void p25p2_vpdu_lcch_signal_update(p25p2_vpdu_ctx* ctx);
static int p25p2_vpdu_validate_len_and_warn(const p25p2_vpdu_ctx* ctx);
static void p25p2_vpdu_dispatch_blocks(p25p2_vpdu_ctx* ctx);
static int p25p2_vpdu_advance_segment(p25p2_vpdu_ctx* ctx);

static void
p25p2_vpdu_emit_json(const p25p2_vpdu_ctx* ctx) {
    uint8_t mfid = (uint8_t)ctx->mac[2];
    uint8_t opcode = (uint8_t)ctx->mac[1];
    const char* tag = NULL;
    switch (opcode) {
        case 0x0: tag = "SIGNAL"; break;
        case 0x1: tag = "PTT"; break;
        case 0x2: tag = "END"; break;
        case 0x3: tag = "IDLE"; break;
        case 0x4: tag = "ACTIVE"; break;
        case 0x6: tag = "HANGTIME"; break;
        default: tag = "MAC"; break;
    }
    p25p2_emit_mac_json_if_enabled(ctx->state, ctx->type, mfid, opcode, ctx->slot, ctx->len_b, ctx->len_c, tag);
}

static void
p25p2_vpdu_lcch_signal_update(p25p2_vpdu_ctx* ctx) {
    if (ctx->state->p2_is_lcch != 1) {
        return;
    }
    if (ctx->slot != 0) {
        return;
    }
    if (ctx->type == 0 || ctx->type == 1) {
        ctx->state->dmrburstL = 30;
    }
}

static int
p25p2_vpdu_seen_unknown_len(uint8_t mfid, uint8_t opcode) {
    static struct {
        uint8_t mfid;
        uint8_t opcode;
    } seen[32];

    static int seen_count = 0;
    for (int i = 0; i < seen_count; i++) {
        if (seen[i].mfid == mfid && seen[i].opcode == opcode) {
            return 1;
        }
    }
    if (seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) {
        seen[seen_count].mfid = mfid;
        seen[seen_count].opcode = opcode;
        seen_count++;
    }
    return 0;
}

static int
p25p2_vpdu_validate_len_and_warn(const p25p2_vpdu_ctx* ctx) {
    if (ctx->len_b != 0) {
        return 1;
    }
    uint8_t mfid = (uint8_t)ctx->mac[2];
    uint8_t opcode = (uint8_t)ctx->mac[1];
    if (p25p2_vpdu_seen_unknown_len(mfid, opcode)) {
        return 0;
    }
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr,
                "\n P25p2 MAC length unknown/unsupported: MFID=%02X OPCODE=%02X (len=0, MCO=%d). Please report.\n",
                mfid, opcode, (int)(ctx->mac[1] & 0x3F));
    DSD_FPRINTF(stderr, "%s", KNRM);
    return 0;
}

static int
p25p2_vpdu_channel_is_valid(int channel) {
    return channel != 0 && channel != 0xFFFF;
}

static int
p25p2_vpdu_can_tune(const dsd_opts* opts, dsd_state* state, long int freq) {
    if (!opts || !state || opts->p25_trunk != 1 || opts->p25_is_tuned != 0 || freq == 0) {
        return 0;
    }
    if (state->trunk_cc_freq > 0 || state->p2_is_lcch == 1 || DSD_SYNC_IS_P25P1(state->synctype)) {
        p25_sm_seed_cc_from_current_tuner_if_unknown(opts, state);
    }
    return state->p25_cc_freq != 0;
}

static void
p25p2_vpdu_update_playback_if_match(const dsd_opts* opts, dsd_state* state, int group, long int freq) {
    if (opts->p25_trunk != 0) {
        return;
    }
    if (group == state->lasttg || group == state->lasttgR) {
        p25_set_playback_vc_freq(opts, state, freq);
    }
}

static void
p25p2_vpdu_print_svc_payload(const dsd_opts* opts, int svc) {
    if (opts->payload != 1) {
        return;
    }
    if (svc & 0x20) {
        DSD_FPRINTF(stderr, " Duplex");
    }
    if (svc & 0x10) {
        DSD_FPRINTF(stderr, " Packet");
    } else {
        DSD_FPRINTF(stderr, " Circuit");
    }
    if (svc & 0x8) {
        DSD_FPRINTF(stderr, " R");
    }
    DSD_FPRINTF(stderr, " Priority %d", svc & 0x7);
}

static void
p25p2_vpdu_apply_svc_slot_state(const dsd_opts* opts, dsd_state* state, int slot_idx, int svc, int set_packet_bit) {
    state->p25_call_emergency[slot_idx] = (uint8_t)((svc & 0x80) ? 1 : 0);
    if (set_packet_bit) {
        state->p25_call_is_packet[slot_idx] = (uint8_t)((svc & 0x10) ? 1 : 0);
    }
    state->p25_call_priority[slot_idx] = (uint8_t)((opts->payload == 1) ? (svc & 0x7) : 0);
}

static void
p25p2_vpdu_print_svc_with_slot_state(const dsd_opts* opts, dsd_state* state, int slot_idx, int svc,
                                     int set_packet_bit) {
    if (svc & 0x80) {
        DSD_FPRINTF(stderr, " Emergency");
    }
    if (svc & 0x40) {
        DSD_FPRINTF(stderr, " Encrypted");
    }
    p25p2_vpdu_apply_svc_slot_state(opts, state, slot_idx, svc, set_packet_bit);
    p25p2_vpdu_print_svc_payload(opts, svc);
}

static void
p25p2_vpdu_print_svc_no_state(const dsd_opts* opts, int svc) {
    if (svc & 0x80) {
        DSD_FPRINTF(stderr, " Emergency");
    }
    if (svc & 0x40) {
        DSD_FPRINTF(stderr, " Encrypted");
    }
    p25p2_vpdu_print_svc_payload(opts, svc);
}

static void
p25p2_vpdu_store_slot_svc(dsd_state* state, int slot, int svc) {
    if ((slot & 1) == 0) {
        state->dmr_so = (uint16_t)svc;
        state->p25_service_options_valid[0] = 1;
    } else {
        state->dmr_soR = (uint16_t)svc;
        state->p25_service_options_valid[1] = 1;
    }
}

static int
p25p2_vpdu_u16(const unsigned long long int* mac, int idx) {
    return (int)((mac[idx] << 8) | mac[idx + 1]);
}

static int
p25p2_vpdu_u24(const unsigned long long int* mac, int idx) {
    return (int)((mac[idx] << 16) | (mac[idx + 1] << 8) | mac[idx + 2]);
}

static int
p25p2_vpdu_fqid_wacn(const unsigned long long int* mac, int idx) {
    return (int)((mac[idx] << 12) | (mac[idx + 1] << 4) | ((mac[idx + 2] & 0xF0) >> 4));
}

static int
p25p2_vpdu_fqid_sysid(const unsigned long long int* mac, int idx) {
    return (int)(((mac[idx + 2] & 0x0F) << 8) | mac[idx + 3]);
}

static void
p25p2_vpdu_set_active_group_single(dsd_state* state, int channel, int group) {
    char suffix[32];
    p25_format_chan_suffix(state, (uint16_t)channel, -1, suffix, sizeof suffix);
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active Ch: %04X%s TG: %d; ", channel,
                 suffix, group);
    state->last_active_time = time(NULL);
}

static void
p25p2_vpdu_set_active_group_pair(dsd_state* state, int channel1, int group1, int channel2, int group2) {
    if (channel2 != channel1 && p25p2_vpdu_channel_is_valid(channel2)) {
        char suffix1[32];
        char suffix2[32];
        p25_format_chan_suffix(state, (uint16_t)channel1, -1, suffix1, sizeof suffix1);
        p25_format_chan_suffix(state, (uint16_t)channel2, -1, suffix2, sizeof suffix2);
        DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]),
                     "Active Ch: %04X%s TG: %d; Ch: %04X%s TG: %d; ", channel1, suffix1, group1, channel2, suffix2,
                     group2);
        state->last_active_time = time(NULL);
        return;
    }
    p25p2_vpdu_set_active_group_single(state, channel1, group1);
}

static void
p25p2_vpdu_set_active_group_triple(dsd_state* state, int channel1, int group1, int channel2, int group2, int channel3,
                                   int group3) {
    char suffix1[32];
    char suffix2[32];
    char suffix3[32];
    p25_format_chan_suffix(state, (uint16_t)channel1, -1, suffix1, sizeof suffix1);
    p25_format_chan_suffix(state, (uint16_t)channel2, -1, suffix2, sizeof suffix2);
    p25_format_chan_suffix(state, (uint16_t)channel3, -1, suffix3, sizeof suffix3);
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]),
                 "Active Ch: %04X%s TG: %d; Ch: %04X%s TG: %d; Ch: %04X%s TG: %d; ", channel1, suffix1, group1,
                 channel2, suffix2, group2, channel3, suffix3, group3);
    state->last_active_time = time(NULL);
}

typedef struct {
    int channel;
    int group;
    long int freq;
    int svc_bits;
} p25p2_vpdu_group_candidate;

typedef struct {
    int policy_encrypted_override;
    int policy_data_override;
    int emit_enc_lockout;
    int stop_on_tune;
} p25p2_vpdu_candidate_policy;

static int
p25p2_vpdu_try_group_candidate(const struct p25p2_mac_result* mac_res, dsd_opts* opts, dsd_state* state,
                               const p25p2_vpdu_group_candidate* candidate, const p25p2_vpdu_candidate_policy* policy) {
    int tuned = 0;
    p25p2_vpdu_print_group_label(state, (uint32_t)candidate->group);
    if (p25p2_vpdu_can_tune(opts, state, candidate->freq)) {
        p25p2_mac_handle(mac_res, opts, state, candidate->channel, candidate->svc_bits, candidate->group, /*src*/ 0,
                         policy->policy_encrypted_override, policy->policy_data_override, policy->emit_enc_lockout);
        tuned = (policy->stop_on_tune && opts->p25_is_tuned != 0) ? 1 : 0;
    }
    p25p2_vpdu_update_playback_if_match(opts, state, candidate->group, candidate->freq);
    return tuned;
}

static void
p25p2_vpdu_clear_slot_banner(dsd_state* state, int slot) {
    if (slot == 0) {
        DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], "%s", "                     ");
    } else {
        DSD_SNPRINTF(state->call_string[1], sizeof state->call_string[1], "%s", "                     ");
    }
}

static void
p25p2_vpdu_gate_slot_audio(dsd_state* state, int slot) {
    state->p25_p2_audio_allowed[slot] = 0;
    p25_p2_audio_ring_reset(state, slot);
}

static double
p25p2_vpdu_cfg_mac_hold_s(const dsd_state* state, double fallback) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (state && state->p25_cfg_mac_hold_s > 0.0) {
        return state->p25_cfg_mac_hold_s;
    }
    if (cfg && cfg->p25_mac_hold_is_set) {
        return cfg->p25_mac_hold_s;
    }
    return fallback;
}

static double
p25p2_vpdu_cfg_voice_hold_s(double fallback) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->p25_voice_hold_is_set) {
        return cfg->p25_voice_hold_s;
    }
    return fallback;
}

static double
p25p2_vpdu_cfg_vc_grace_s(const dsd_state* state, double fallback) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (state->p25_cfg_vc_grace_s > 0.0) {
        return state->p25_cfg_vc_grace_s;
    }
    if (cfg && cfg->p25_vc_grace_is_set) {
        return cfg->p25_vc_grace_s;
    }
    return fallback;
}

static double
p25p2_vpdu_elapsed_s(double mono_stamp, time_t wall_stamp, double nowm, double noww) {
    if (mono_stamp > 0.0) {
        return nowm - mono_stamp;
    }
    if (wall_stamp != 0) {
        return noww - (double)wall_stamp;
    }
    return 1e9;
}

static int
p25p2_vpdu_recent_voice_active(const dsd_state* state, double voice_hold_s) {
    double nowm = dsd_time_now_monotonic_s();
    double noww = (double)time(NULL);
    double dt = p25p2_vpdu_elapsed_s(state->last_vc_sync_time_m, state->last_vc_sync_time, nowm, noww);
    return dt <= voice_hold_s;
}

static int
p25p2_vpdu_other_slot_audio_with_history(const dsd_state* state, int slot, double mac_hold_s, double voice_hold_s) {
    int other_slot = slot ^ 1;
    double nowm = dsd_time_now_monotonic_s();
    double noww = (double)time(NULL);
    double dt_mac = p25p2_vpdu_elapsed_s(state->p25_p2_last_mac_active_m[other_slot],
                                         state->p25_p2_last_mac_active[other_slot], nowm, noww);
    int recent_voice = p25p2_vpdu_recent_voice_active(state, voice_hold_s);
    int recent_mac = (state->p25_p2_last_mac_active[other_slot] != 0 && dt_mac <= mac_hold_s) ? 1 : 0;
    return state->p25_p2_audio_allowed[other_slot] || (state->p25_p2_audio_ring_count[other_slot] > 0) || recent_mac
           || recent_voice;
}

static int
p25p2_vpdu_force_release_after_grace(dsd_opts* opts, dsd_state* state) {
    double vc_grace = p25p2_vpdu_cfg_vc_grace_s(state, 0.75);
    double nowm = dsd_time_now_monotonic_s();
    double dt_since_tune = (state->p25_last_vc_tune_time_m > 0.0) ? (nowm - state->p25_last_vc_tune_time_m) : 1e9;
    if (dt_since_tune < vc_grace) {
        return 0;
    }
    state->p25_sm_force_release = 1;
    p25_sm_on_release(opts, state);
    return 1;
}

static void
p25p2_vpdu_mark_enc_lockout(dsd_opts* opts, dsd_state* state, int slot, int talkgroup) {
    if (talkgroup == 0 || p25_patch_tg_key_is_clear(state, talkgroup) || p25_patch_sg_key_is_clear(state, talkgroup)) {
        return;
    }
    p25_emit_enc_lockout_once(opts, state, (uint8_t)slot, talkgroup, /*svc_bits*/ 0);
    state->p25_p2_enc_lockout_muted[slot & 1] = 1;
}

static void
p25p2_vpdu_set_group_call_banner(dsd_state* state, int slot, int svc) {
    DSD_SNPRINTF(state->call_string[slot], sizeof(state->call_string[slot]), "   Group ");
    if (svc & 0x80) {
        dsd_append(state->call_string[slot], sizeof state->call_string[slot], " Emergency  ");
    } else if (svc & 0x40) {
        dsd_append(state->call_string[slot], sizeof state->call_string[slot], " Encrypted  ");
    } else {
        dsd_append(state->call_string[slot], sizeof state->call_string[slot], "            ");
    }
}

static void
p25p2_vpdu_set_private_call_banner(dsd_state* state, int slot, int svc) {
    DSD_SNPRINTF(state->call_string[slot], sizeof(state->call_string[slot]), " Private ");
    if (svc & 0x80) {
        dsd_append(state->call_string[slot], sizeof state->call_string[slot], " Emergency  ");
    } else if (svc & 0x40) {
        dsd_append(state->call_string[slot], sizeof state->call_string[slot], " Encrypted  ");
    } else {
        dsd_append(state->call_string[slot], sizeof state->call_string[slot], "            ");
    }
}

static void
p25p2_vpdu_update_group_last_ids(dsd_state* state, int slot, int talkgroup, int source) {
    if (slot == 0) {
        state->lasttg = talkgroup;
        if (source != 0) {
            state->lastsrc = source;
            state->generic_talker_alias[0][0] = '\0';
            state->generic_talker_alias_src[0] = 0;
        }
        return;
    }
    state->lasttgR = talkgroup;
    if (source != 0) {
        state->lastsrcR = source;
        state->generic_talker_alias[1][0] = '\0';
        state->generic_talker_alias_src[1] = 0;
    }
}

static void
p25p2_vpdu_update_private_last_ids(dsd_state* state, int slot, int talkgroup, int source) {
    if (slot == 0) {
        state->lasttg = talkgroup;
        if (source != 0) {
            state->lastsrc = source;
            if (state->generic_talker_alias_src[0] != (uint32_t)source) {
                state->generic_talker_alias[0][0] = '\0';
                state->generic_talker_alias_src[0] = 0;
            }
        }
        return;
    }
    state->lasttgR = talkgroup;
    if (source != 0) {
        state->lastsrcR = source;
        if (state->generic_talker_alias_src[1] != (uint32_t)source) {
            state->generic_talker_alias[1][0] = '\0';
            state->generic_talker_alias_src[1] = 0;
        }
    }
}

static void
p25p2_vpdu_handle_group_voice_enc_fallback(dsd_opts* opts, dsd_state* state, int slot, int talkgroup) {
    double mac_hold = p25p2_vpdu_cfg_mac_hold_s(state, 0.75);
    double voice_hold = p25p2_vpdu_cfg_voice_hold_s(0.6);
    int other_audio = 0;

    p25p2_vpdu_mark_enc_lockout(opts, state, slot, talkgroup);
    p25p2_vpdu_gate_slot_audio(state, slot);
    other_audio = p25p2_vpdu_other_slot_audio_with_history(state, slot, mac_hold, voice_hold);
    if (!other_audio) {
        DSD_FPRINTF(stderr, " No Enc Following on P25p2 Trunking (VCH SVC ENC); ");
        if (p25p2_vpdu_force_release_after_grace(opts, state)) {
            DSD_FPRINTF(stderr, "Return to CC; \n");
        } else {
            DSD_FPRINTF(stderr, "Defer (VC grace); stay on VC. \n");
        }
        return;
    }
    DSD_FPRINTF(stderr, " No Enc Following on P25p2 Trunking (VCH SVC ENC); Other slot active; stay on VC. \n");
    p25p2_vpdu_clear_slot_banner(state, slot);
}

static void
p25p2_vpdu_handle_unit_voice_enc_fallback(dsd_opts* opts, dsd_state* state, int slot, int talkgroup) {
    double mac_hold = p25p2_vpdu_cfg_mac_hold_s(state, 0.75);
    double voice_hold = p25p2_vpdu_cfg_voice_hold_s(0.6);
    int other_audio = 0;

    p25p2_vpdu_mark_enc_lockout(opts, state, slot, talkgroup);
    p25p2_vpdu_gate_slot_audio(state, slot);
    other_audio = p25p2_vpdu_other_slot_audio_with_history(state, slot, mac_hold, voice_hold);
    if (!other_audio) {
        DSD_FPRINTF(stderr, " No Enc Following on P25p2 Trunking (VCH SVC ENC); ");
        if (p25p2_vpdu_force_release_after_grace(opts, state)) {
            DSD_FPRINTF(stderr, "Return to CC; \n");
        } else {
            DSD_FPRINTF(stderr, "Defer (VC grace); stay on VC. \n");
        }
        return;
    }
    DSD_FPRINTF(stderr, " No Enc Following on P25p2 Trunking (VCH SVC ENC); Other slot active; stay on VC. \n");
    p25p2_vpdu_clear_slot_banner(state, slot);
}

static long int
p25p2_vpdu_block07_print_entry(const dsd_opts* opts, dsd_state* state, int svc, int channel_t, int channel_r, int group,
                               int slot_idx) {
    long int freq_t = 0;
    DSD_FPRINTF(stderr, "\n  SVC [%02X] CHAN-T [%04X] CHAN-R [%04X] Group [%d][%04X]", svc, channel_t, channel_r, group,
                group);
    p25p2_vpdu_print_svc_with_slot_state(opts, state, slot_idx, svc, /*set_packet_bit*/ 0);
    freq_t = process_channel_to_freq(opts, state, channel_t);
    if (p25p2_vpdu_channel_is_valid(channel_r)) {
        (void)process_channel_to_freq(opts, state, channel_r);
    }
    return freq_t;
}

typedef struct {
    int svc;
    int reserved;
    int channelt;
    int channelr;
    int group;
    int source;
    int set_packet_bit;
    int store_slot_svc;
    const char* label;
} p25p2_group_explicit_grant;

static void
p25p2_vpdu_handle_group_explicit_grant(const struct p25p2_mac_result* mac_res, dsd_opts* opts, dsd_state* state,
                                       const p25p2_group_explicit_grant* grant) {
    int slot_idx = state->currentslot & 1;
    long int freq_t = 0;

    DSD_FPRINTF(stderr, "\n");
    p25p2_vpdu_print_svc_with_slot_state(opts, state, slot_idx, grant->svc, grant->set_packet_bit);
    DSD_FPRINTF(stderr, " %s", grant->label);
    DSD_FPRINTF(stderr, "\n  SVC [%02X]", grant->svc);
    if (grant->reserved >= 0) {
        DSD_FPRINTF(stderr, " RES [%02X]", grant->reserved);
    }
    DSD_FPRINTF(stderr, " CHAN-T [%04X] CHAN-R [%04X] Group [%d][%04X]", grant->channelt, grant->channelr, grant->group,
                grant->group);
    if (grant->source > 0) {
        DSD_FPRINTF(stderr, " Source [%d]", grant->source);
    }

    freq_t = process_channel_to_freq(opts, state, grant->channelt);
    if (p25p2_vpdu_channel_is_valid(grant->channelr)) {
        (void)process_channel_to_freq(opts, state, grant->channelr);
    }
    if (grant->store_slot_svc) {
        p25p2_vpdu_store_slot_svc(state, slot_idx, grant->svc);
    }
    p25p2_vpdu_set_active_group_single(state, grant->channelt, grant->group);
    p25p2_vpdu_print_group_label(state, (uint32_t)grant->group);

    if (p25p2_vpdu_can_tune(opts, state, freq_t)) {
        p25p2_mac_handle(mac_res, opts, state, grant->channelt, grant->svc, grant->group, grant->source,
                         /*policy_encrypted*/ -1,
                         /*policy_data*/ -1, /*emit_enc_lockout*/ 1);
    }
    p25p2_vpdu_update_playback_if_match(opts, state, grant->group, freq_t);
}

static int
p25p2_vpdu_groups_clear_for_enc(const dsd_state* state, const int* groups, int count) {
    for (int i = 0; i < count; i++) {
        if (p25_patch_tg_key_is_clear(state, groups[i]) || p25_patch_sg_key_is_clear(state, groups[i])) {
            return 1;
        }
    }
    return 0;
}

static int
p25p2_vpdu_block07_enc_blocked(const dsd_opts* opts, const dsd_state* state, int svc1, int svc2, int group1,
                               int group2) {
    const int groups[2] = {group1, group2};
    if (!(svc1 & 0x40) || !(svc2 & 0x40) || opts->trunk_tune_enc_calls != 0) {
        return 0;
    }
    return !p25p2_vpdu_groups_clear_for_enc(state, groups, 2);
}

static void
p25p2_vpdu_block07_try_candidates(const struct p25p2_mac_result* mac_res, dsd_opts* opts, dsd_state* state,
                                  const p25p2_vpdu_group_candidate candidates[2]) {
    int loop = (candidates[0].channel == candidates[1].channel) ? 1 : 2;
    const p25p2_vpdu_candidate_policy policy = {-1, -1, 1, 0};

    for (int j = 0; j < loop; j++) {
        (void)p25p2_vpdu_try_group_candidate(mac_res, opts, state, &candidates[j], &policy);
    }
}

static void
p25p2_vpdu_block08_print_entry(const dsd_opts* opts, dsd_state* state, int index, int channel, int group, int svc,
                               long int* out_freq) {
    DSD_FPRINTF(stderr, "\n  Channel %d [%04X] Group %d [%d][%04X]", index, channel, index, group, group);
    p25p2_vpdu_print_svc_no_state(opts, svc);
    *out_freq = process_channel_to_freq(opts, state, channel);
}

static int
p25p2_vpdu_block08_enc_blocked(const dsd_opts* opts, const dsd_state* state, int so1, int so2, int so3, int group1,
                               int group2, int group3) {
    const int groups[3] = {group1, group2, group3};
    if (!(so1 & 0x40) || !(so2 & 0x40) || !(so3 & 0x40) || opts->trunk_tune_enc_calls != 0) {
        return 0;
    }
    return !p25p2_vpdu_groups_clear_for_enc(state, groups, 3);
}

static void
p25p2_vpdu_block08_try_candidates(const struct p25p2_mac_result* mac_res, dsd_opts* opts, dsd_state* state,
                                  const int* channels, const int* groups, const long int* freqs, const int* svcs) {
    for (int j = 0; j < 3; j++) {
        p25p2_vpdu_group_candidate candidate = {channels[j], groups[j], freqs[j], svcs[j]};
        const p25p2_vpdu_candidate_policy policy = {-1, -1, 1, 1};
        int tuned = p25p2_vpdu_try_group_candidate(mac_res, opts, state, &candidate, &policy);
        if (tuned) {
            break;
        }
    }
}

//MAC PDU 3-bit Opcodes BBAC (8.4.1) p 123:
//0 - reserved //1 - Mac PTT //2 - Mac End PTT //3 - Mac Idle //4 - Mac Active
//5 - reserved //6 - Mac Hangtime //7 - reserved //Mac PTT BBAC p80

//TODO: Check for Non standard MFIDs first MAC[1], then set len on the MAC[2] if
//the result from the len table is 0 (had to manually enter a few observed values from Harris)
static void
p25p2_vpdu_iter_block_01(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xA3 && MAC[2 + len_a] == 0x90) {
        int mfid = MAC[2 + len_a];
        UNUSED(mfid);
        int svc = MAC[4 + len_a];
        int channel = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int sgroup = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int source = (MAC[9 + len_a] << 16) | (MAC[10 + len_a] << 8) | MAC[11 + len_a];
        int slot_idx = state->currentslot & 1;
        long int freq = 0;
        DSD_FPRINTF(stderr, "\n MFID90 Group Regroup Channel Grant - Implicit");
        p25p2_vpdu_print_svc_with_slot_state(opts, state, slot_idx, svc, /*set_packet_bit*/ 1);
        DSD_FPRINTF(stderr, "\n  SVC [%02X] CHAN [%04X] Group [%d][%04X] Source [%d]", svc, channel, sgroup, sgroup,
                    source);
        freq = process_channel_to_freq(opts, state, channel);

        //add active channel to string for ncurses display
        p25_set_mfid90_active_channel_single(state, channel, sgroup);
        p25p2_vpdu_store_slot_svc(state, slot_idx, svc);

        p25p2_vpdu_print_group_label(state, (uint32_t)sgroup);

        if (p25p2_vpdu_can_tune(opts, state, freq)) {
            p25p2_mac_handle(&mac_res, opts, state, channel, svc, sgroup, source, /*policy_encrypted*/ -1,
                             /*policy_data*/ -1, /*emit_enc_lockout*/ 1);
        }
        // If playing back files, and we still want to see what freqs are in use in the ncurses terminal
        //might only want to do these on a grant update, and not a grant by itself?
        p25_set_playback_vc_freq(opts, state, freq);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_02(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xA4 && MAC[2 + len_a] == 0x90) {
        int mfid = MAC[2 + len_a];
        int svc = MAC[4 + len_a];
        int channel = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int channelr = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int sgroup = (MAC[9 + len_a] << 8) | MAC[10 + len_a];
        int source = (MAC[11 + len_a] << 16) | (MAC[12 + len_a] << 8) | MAC[13 + len_a];
        int slot_idx = state->currentslot & 1;
        long int freq = 0;
        UNUSED(mfid);
        DSD_FPRINTF(stderr, "\n MFID90 Group Regroup Channel Grant - Explicit");
        p25p2_vpdu_print_svc_with_slot_state(opts, state, slot_idx, svc, /*set_packet_bit*/ 1);
        DSD_FPRINTF(stderr, "\n  SVC [%02X] CHAN-T [%04X] CHAN-R [%04X] Group [%d][%04X] Source [%d]", svc, channel,
                    channelr, sgroup, sgroup, source);
        freq = process_channel_to_freq(opts, state, channel);
        if (p25p2_vpdu_channel_is_valid(channelr)) {
            (void)process_channel_to_freq(opts, state, channelr);
        }

        //add active channel to string for ncurses display
        p25_set_mfid90_active_channel_single(state, channel, sgroup);
        p25p2_vpdu_store_slot_svc(state, slot_idx, svc);

        p25p2_vpdu_print_group_label(state, (uint32_t)sgroup);

        if (p25p2_vpdu_can_tune(opts, state, freq)) {
            p25p2_mac_handle(&mac_res, opts, state, channel, svc, sgroup, source, /*policy_encrypted*/ -1,
                             /*policy_data*/ -1, /*emit_enc_lockout*/ 1);
        }
        // If playing back files, and we still want to see what freqs are in use in the ncurses terminal
        //might only want to do these on a grant update, and not a grant by itself?
        if (opts->p25_trunk == 0) {
            if (sgroup == state->lasttg || sgroup == state->lasttgR) {
                p25_set_playback_vc_freq(opts, state, freq);
            }
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_03(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xA5 && MAC[2 + len_a] == 0x90) {
        int channel1 = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int group1 = (MAC[6 + len_a] << 8) | MAC[7 + len_a];
        int channel2 = (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        int group2 = (MAC[10 + len_a] << 8) | MAC[11 + len_a];
        long int freq1 = 0;
        long int freq2 = 0;

        DSD_FPRINTF(stderr, "\n MFID90 Group Regroup Channel Grant Update");
        DSD_FPRINTF(stderr, "\n  Channel 1 [%04X] Group 1 [%d][%04X]", channel1, group1, group1);
        freq1 = process_channel_to_freq(opts, state, channel1);
        if (channel2 != channel1 && p25p2_vpdu_channel_is_valid(channel2)) {
            DSD_FPRINTF(stderr, "\n  Channel 2 [%04X] Group 2 [%d][%04X]", channel2, group2, group2);
            freq2 = process_channel_to_freq(opts, state, channel2);
        }

        p25_set_mfid90_active_channel_update(state, channel1, group1, channel2, group2);

        if (opts->trunk_tune_group_calls == 0) {
            ctx->skip_rest = 1;
            goto BLOCK_END;
        }

        int loop = (channel1 == channel2) ? 1 : 2;
        for (int j = 0; j < loop; j++) {
            int tunable_chan = (j == 0) ? channel1 : channel2;
            int tunable_group = (j == 0) ? group1 : group2;
            long int tunable_freq = (j == 0) ? freq1 : freq2;
            p25p2_vpdu_group_candidate candidate = {tunable_chan, tunable_group, tunable_freq, P25_SM_SVC_UNKNOWN};
            p25p2_vpdu_candidate_policy policy = {-1, 0, 0, 1};
            int tuned = p25p2_vpdu_try_group_candidate(&mac_res, opts, state, &candidate, &policy);
            if (tuned) {
                break;
            }
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_04(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x40) {
        int svc = MAC[2 + len_a];
        int channel = (MAC[3 + len_a] << 8) | MAC[4 + len_a];
        int group = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int source = (MAC[7 + len_a] << 16) | (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        int slot_idx = state->currentslot & 1;
        long int freq = 0;

        DSD_FPRINTF(stderr, "\n");
        p25p2_vpdu_print_svc_with_slot_state(opts, state, slot_idx, svc, /*set_packet_bit*/ 1);
        DSD_FPRINTF(stderr, " Group Voice Channel Grant");
        DSD_FPRINTF(stderr, "\n  SVC [%02X] CHAN [%04X] Group [%d] Source [%d]", svc, channel, group, source);
        freq = process_channel_to_freq(opts, state, channel);
        p25p2_vpdu_store_slot_svc(state, slot_idx, svc);
        p25p2_vpdu_set_active_group_single(state, channel, group);
        p25p2_vpdu_print_group_label(state, (uint32_t)group);

        if (p25p2_vpdu_can_tune(opts, state, freq)) {
            p25p2_mac_handle(&mac_res, opts, state, channel, svc, group, source, /*policy_encrypted*/ -1,
                             /*policy_data*/ -1, /*emit_enc_lockout*/ 1);
        }
        p25p2_vpdu_update_playback_if_match(opts, state, group, freq);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_05(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x48 || MAC[1 + len_a] == 0x49 || MAC[1 + len_a] == 0xC8 || MAC[1 + len_a] == 0xC9) {
        int k = (MAC[len_a] == 0x07) ? 0 : 1;
        int svc = MAC[2 + len_a + k];
        int channel = (MAC[3 + len_a + k] << 8) | MAC[4 + len_a + k];
        int timer = (MAC[5 + len_a + k] << 8) | MAC[6 + len_a + k];
        uint32_t target = (uint32_t)((MAC[7 + len_a + k] << 16) | (MAC[8 + len_a + k] << 8) | MAC[9 + len_a + k]);
        int slot_idx = state->currentslot & 1;
        long int freq = 0;

        if (MAC[1 + len_a] & 0x80) {
            timer = (MAC[8 + len_a] << 8) | MAC[9 + len_a];
            target = (uint32_t)((MAC[10 + len_a] << 16) | (MAC[11 + len_a] << 8) | MAC[12 + len_a]);
        }

        DSD_FPRINTF(stderr, "\n");
        p25p2_vpdu_print_svc_with_slot_state(opts, state, slot_idx, svc, /*set_packet_bit*/ 1);
        DSD_FPRINTF(stderr, " Telephone Interconnect Voice Channel Grant");
        if (MAC[1 + len_a] & 0x01) {
            DSD_FPRINTF(stderr, " Update");
        }
        DSD_FPRINTF(stderr, (MAC[1 + len_a] & 0x80) ? " Explicit" : " Implicit");
        DSD_FPRINTF(stderr, "\n  CHAN: %04X; Timer: %f Seconds; Target: %d;", channel, (float)timer * 0.1f, target);
        freq = process_channel_to_freq(opts, state, channel);
        p25p2_vpdu_store_slot_svc(state, slot_idx, svc);

        if (p25p2_vpdu_channel_is_valid(channel)) {
            char suffix[32];
            p25_format_chan_suffix(state, (uint16_t)channel, -1, suffix, sizeof suffix);
            DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active Tele Ch: %04X%s TGT: %u; ",
                         channel, suffix, target);
        }
        state->last_active_time = time(NULL);

        p25p2_vpdu_print_group_label(state, target);
        if (p25p2_vpdu_can_tune(opts, state, freq)) {
            p25p2_mac_handle_indiv(&mac_res, opts, state, channel, svc, (int)target, /*src*/ 0,
                                   /*policy_encrypted*/ -1, /*policy_data*/ -1);
        }
        if (opts->p25_trunk == 0
            && ((uint32_t)target == (uint32_t)state->lasttg || (uint32_t)target == (uint32_t)state->lasttgR)) {
            p25_set_playback_vc_freq(opts, state, freq);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_06(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x44 || MAC[1 + len_a] == 0x46 || MAC[1 + len_a] == 0xC4) {
        int opcode = (int)MAC[1 + len_a];
        int channel = (MAC[2 + len_a] << 8) | MAC[3 + len_a];
        int target = (MAC[4 + len_a] << 16) | (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int source = (MAC[7 + len_a] << 16) | (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        unsigned long long int src_suid = 0;
        long int freq = 0;

        DSD_FPRINTF(stderr, "\n Unit to Unit Channel Grant");
        if (opcode == 0x46) {
            DSD_FPRINTF(stderr, " Update");
        }
        if (opcode == 0xC4) {
            DSD_FPRINTF(stderr, " Extended");
        }
        DSD_FPRINTF(stderr, "\n  CHAN: %04X; SRC: %d; TGT: %d; ", channel, source, target);
        if (opcode == 0xC4) {
            DSD_FPRINTF(stderr, "SUID: %08llX-%08d; ", src_suid >> 24, source);
        }
        freq = process_channel_to_freq(opts, state, channel);

        char suffix[32];
        p25_format_chan_suffix(state, (uint16_t)channel, -1, suffix, sizeof suffix);
        DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active Ch: %04X%s TGT: %d; ", channel,
                     suffix, target);
        state->last_active_time = time(NULL);

        if (opts->trunk_tune_private_calls == 0) {
            ctx->skip_rest = 1;
            goto BLOCK_END;
        }

        p25p2_vpdu_print_group_label(state, (uint32_t)source);
        if (source != target) {
            p25p2_vpdu_print_group_label(state, (uint32_t)target);
        }

        if (p25p2_vpdu_can_tune(opts, state, freq)) {
            int policy_encrypted = (opts->trunk_tune_enc_calls == 0) ? 1 : 0;
            p25p2_mac_handle_indiv(&mac_res, opts, state, channel, P25_SM_SVC_UNKNOWN, target, source, policy_encrypted,
                                   /*policy_data*/ 0);
        }
        p25p2_vpdu_update_playback_if_match(opts, state, target, freq);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_07(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x25) {
        int svc1 = MAC[2 + len_a];
        int channelt1 = (MAC[3 + len_a] << 8) | MAC[4 + len_a];
        int channelr1 = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int group1 = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int svc2 = MAC[9 + len_a];
        int channelt2 = (MAC[10 + len_a] << 8) | MAC[11 + len_a];
        int channelr2 = (MAC[12 + len_a] << 8) | MAC[13 + len_a];
        int group2 = (MAC[14 + len_a] << 8) | MAC[15 + len_a];
        long int freq1t = 0;
        long int freq1r = 0;
        long int freq2t = 0;
        UNUSED(freq1r);

        DSD_FPRINTF(stderr, "\n Group Voice Channel Grant Update Multiple - Explicit");
        freq1t = p25p2_vpdu_block07_print_entry(opts, state, svc1, channelt1, channelr1, group1, /*slot_idx*/ 0);
        freq2t = p25p2_vpdu_block07_print_entry(opts, state, svc2, channelt2, channelr2, group2, /*slot_idx*/ 1);

        {
            char suffix1[32];
            char suffix2[32];
            p25_format_chan_suffix(state, (uint16_t)channelt1, -1, suffix1, sizeof suffix1);
            p25_format_chan_suffix(state, (uint16_t)channelt2, -1, suffix2, sizeof suffix2);
            DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]),
                         "Active Ch: %04X%s TG: %d; Ch: %04X%s TG: %d; ", channelt1, suffix1, group1, channelt2,
                         suffix2, group2);
        }
        state->last_active_time = time(NULL);

        if (opts->trunk_tune_group_calls == 0) {
            ctx->skip_rest = 1;
            goto BLOCK_END;
        }
        if (p25p2_vpdu_block07_enc_blocked(opts, state, svc1, svc2, group1, group2)) {
            ctx->skip_rest = 1;
            goto BLOCK_END;
        }
        p25p2_vpdu_group_candidate candidates[2] = {{channelt1, group1, freq1t, svc1},
                                                    {channelt2, group2, freq2t, svc2}};
        p25p2_vpdu_block07_try_candidates(&mac_res, opts, state, candidates);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_08(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x05) {
        int so1 = MAC[2 + len_a];
        int channel1 = (MAC[3 + len_a] << 8) | MAC[4 + len_a];
        int group1 = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int so2 = MAC[7 + len_a];
        int channel2 = (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        int group2 = (MAC[10 + len_a] << 8) | MAC[11 + len_a];
        int so3 = MAC[12 + len_a];
        int channel3 = (MAC[13 + len_a] << 8) | MAC[14 + len_a];
        int group3 = (MAC[15 + len_a] << 8) | MAC[16 + len_a];
        long int freq1 = 0;
        long int freq2 = 0;
        long int freq3 = 0;

        DSD_FPRINTF(stderr, "\n Group Voice Channel Grant Update Multiple - Implicit");
        p25p2_vpdu_block08_print_entry(opts, state, 1, channel1, group1, so1, &freq1);

        if (channel2 != channel1 && p25p2_vpdu_channel_is_valid(channel2)) {
            p25p2_vpdu_block08_print_entry(opts, state, 2, channel2, group2, so2, &freq2);
        }
        if (channel3 != channel2 && p25p2_vpdu_channel_is_valid(channel3)) {
            p25p2_vpdu_block08_print_entry(opts, state, 3, channel3, group3, so3, &freq3);
        }

        p25p2_vpdu_set_active_group_triple(state, channel1, group1, channel2, group2, channel3, group3);
        if (opts->trunk_tune_group_calls == 0) {
            ctx->skip_rest = 1;
            goto BLOCK_END;
        }
        if (p25p2_vpdu_block08_enc_blocked(opts, state, so1, so2, so3, group1, group2, group3)) {
            ctx->skip_rest = 1;
            goto BLOCK_END;
        }

        const int channels[3] = {channel1, channel2, channel3};
        const int groups[3] = {group1, group2, group3};
        const long int freqs[3] = {freq1, freq2, freq3};
        const int svcs[3] = {so1, so2, so3};
        p25p2_vpdu_block08_try_candidates(&mac_res, opts, state, channels, groups, freqs, svcs);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_09(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x42) {
        int channel1 = (MAC[2 + len_a] << 8) | MAC[3 + len_a];
        int group1 = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int channel2 = (MAC[6 + len_a] << 8) | MAC[7 + len_a];
        int group2 = (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        long int freq1 = 0;
        long int freq2 = 0;

        DSD_FPRINTF(stderr, "\n Group Voice Channel Grant Update - Implicit");
        DSD_FPRINTF(stderr, "\n  Channel 1 [%04X] Group 1 [%d][%04X]", channel1, group1, group1);
        freq1 = process_channel_to_freq(opts, state, channel1);
        if (channel2 != channel1 && p25p2_vpdu_channel_is_valid(channel2)) {
            DSD_FPRINTF(stderr, "\n  Channel 2 [%04X] Group 2 [%d][%04X]", channel2, group2, group2);
            freq2 = process_channel_to_freq(opts, state, channel2);
        }

        p25p2_vpdu_set_active_group_pair(state, channel1, group1, channel2, group2);
        if (opts->trunk_tune_group_calls == 0) {
            ctx->skip_rest = 1;
            goto BLOCK_END;
        }

        int loop = (channel1 == channel2) ? 1 : 2;
        for (int j = 0; j < loop; j++) {
            int tunable_chan = (j == 0) ? channel1 : channel2;
            int tunable_group = (j == 0) ? group1 : group2;
            long int tunable_freq = (j == 0) ? freq1 : freq2;
            p25p2_vpdu_group_candidate candidate = {tunable_chan, tunable_group, tunable_freq, P25_SM_SVC_UNKNOWN};
            const p25p2_vpdu_candidate_policy policy = {-1, 0, 0, 1};
            int tuned = p25p2_vpdu_try_group_candidate(&mac_res, opts, state, &candidate, &policy);
            if (tuned) {
                break;
            }
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_10(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x43) {
        int svc = MAC[2 + len_a];
        int reserved = MAC[3 + len_a];
        int channelt = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int channelr = (MAC[6 + len_a] << 8) | MAC[7 + len_a];
        int group = (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        const p25p2_group_explicit_grant grant = {
            .svc = svc,
            .reserved = reserved,
            .channelt = channelt,
            .channelr = channelr,
            .group = group,
            .source = 0,
            .set_packet_bit = 0,
            .store_slot_svc = 0,
            .label = "Group Voice Channel Grant Update - Explicit",
        };
        p25p2_vpdu_handle_group_explicit_grant(&mac_res, opts, state, &grant);
    }

    if (MAC[1 + len_a] == 0xC0) {
        int svc = MAC[2 + len_a];
        int channelt = (MAC[3 + len_a] << 8) | MAC[4 + len_a];
        int channelr = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int group = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int source = (MAC[9 + len_a] << 16) | (MAC[10 + len_a] << 8) | MAC[11 + len_a];
        const p25p2_group_explicit_grant grant = {
            .svc = svc,
            .reserved = -1,
            .channelt = channelt,
            .channelr = channelr,
            .group = group,
            .source = source,
            .set_packet_bit = 1,
            .store_slot_svc = 1,
            .label = "Group Voice Channel Grant - Explicit",
        };
        p25p2_vpdu_handle_group_explicit_grant(&mac_res, opts, state, &grant);
    }

    if (MAC[1 + len_a] == 0xC3) {
        int svc = MAC[2 + len_a];
        int channelt = (MAC[3 + len_a] << 8) | MAC[4 + len_a];
        int channelr = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int group = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        const p25p2_group_explicit_grant grant = {
            .svc = svc,
            .reserved = -1,
            .channelt = channelt,
            .channelr = channelr,
            .group = group,
            .source = 0,
            .set_packet_bit = 0,
            .store_slot_svc = 0,
            .label = "Group Voice Channel Grant Update - Explicit",
        };
        p25p2_vpdu_handle_group_explicit_grant(&mac_res, opts, state, &grant);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_11(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x54) {
        DSD_FPRINTF(stderr, "\n SNDCP Data Channel Grant - Explicit");
        int dso = MAC[2 + len_a];
        int channelt = (MAC[3 + len_a] << 8) | MAC[4 + len_a];
        int channelr = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int target = (MAC[7 + len_a] << 16) | (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        DSD_FPRINTF(stderr, "\n  DSO: %02X; CHAN-T: %04X; CHAN-R: %04X; Target: %d;", dso, channelt, channelr, target);

        p25p2_vpdu_print_group_label(state, (uint32_t)target);

        long int freq = process_channel_to_freq(opts, state, channelt);

        //add active channel to string for ncurses display
        {
            char suf_dat[32];
            p25_format_chan_suffix(state, (uint16_t)channelt, -1, suf_dat, sizeof suf_dat);
            DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active Data Ch: %04X%s TGT: %d; ",
                         channelt, suf_dat, target);
        }
        state->last_active_time = time(NULL);

        if (p25p2_vpdu_can_tune(opts, state, freq)) {
            const int policy_encrypted = (opts->trunk_tune_enc_calls == 0) ? 1 : 0;
            p25p2_mac_handle_indiv(&mac_res, opts, state, channelt, P25_SM_SVC_UNKNOWN, (int)target, /*src*/ 0,
                                   policy_encrypted, /*policy_data*/ 1);
        }
        if (opts->p25_trunk == 0) {
            if (target == state->lasttg || target == state->lasttgR) {
                //P1 FDMA
                if (DSD_SYNC_IS_P25P1(state->synctype)) {
                    state->p25_vc_freq[0] = freq;
                }
                //P2 TDMA
                else {
                    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
                }
            }
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_12(p25p2_vpdu_ctx* ctx) {
    const dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x55) {
        DSD_FPRINTF(stderr, "\n SNDCP Data Page Request ");
        int dso = MAC[2 + len_a];
        int dac = (MAC[3 + len_a] << 8) | MAC[4 + len_a];
        int target = (MAC[5 + len_a] << 16) | (MAC[6 + len_a] << 8) | MAC[7 + len_a];
        //P25p1 TSBK is shifted slightly on these two values
        if (DSD_SYNC_IS_P25P1(state->synctype)) {
            dac = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
            target = (MAC[7 + len_a] << 16) | (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        }
        DSD_FPRINTF(stderr, "\n  DSO: %02X; DAC: %02X; Target: %d;", dso, dac, target);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_13(p25p2_vpdu_ctx* ctx) {
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x56) {
        int ch1 = (MAC[2 + len_a] << 8) | MAC[3 + len_a];
        int ch2 = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        DSD_FPRINTF(stderr, "\n SNDCP (P1 TSBK) CH1 [%04X] CH2 [%04X]", ch1, ch2);
        // Minimal handling: just log channels, no frequency conversion per OP25 approach
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_14(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xD6) {
        DSD_FPRINTF(stderr, "\n SNDCP Data Channel Announcement ");
        int aa = (MAC[2 + len_a] >> 7) & 1;
        int ra = (MAC[2 + len_a] >> 6) & 1;
        int dso = MAC[2 + len_a];
        int channelt = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int channelr = (MAC[6 + len_a] << 8) | MAC[7 + len_a];
        int dac = (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        DSD_FPRINTF(stderr, "\n  AA: %d; RA: %d; DSO: %02X; DAC: %02X; CHAN-T: %04X; CHAN-R: %04X;", aa, ra, dso, dac,
                    channelt, channelr);
        long int freq = 0;
        UNUSED(freq);
        if (channelt != 0) {
            (void)process_channel_to_freq(opts, state, channelt);
        }
        if (channelr != 0) {
            (void)process_channel_to_freq(opts, state, channelr);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_15(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x81 && MAC[2 + len_a] == 0x90) {
        int wg_len = MAC[3 + len_a] & 0x3F; // length in bytes of workgroup list
        int sg = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        DSD_FPRINTF(stderr, "\n MFID90 (Moto) Group Regroup Add Command\n");
        DSD_FPRINTF(stderr, "  SG: %d", sg);
        // Workgroup IDs start at byte 6, each is 16 bits
        int num_wg = (wg_len >= 2) ? ((wg_len - 2) / 2) : 0; // subtract sg bytes, divide by 2
        for (int wi = 0; wi < num_wg && (6 + len_a + wi * 2 + 1) < 24; wi++) {
            int wg = (MAC[6 + len_a + wi * 2] << 8) | MAC[6 + len_a + wi * 2 + 1];
            if (wg != 0) {
                DSD_FPRINTF(stderr, " WG%d: %d", wi + 1, wg);
                p25_patch_add_wgid(state, sg, wg);
            }
        }
        DSD_FPRINTF(stderr, "\n");
        p25_patch_update(state, sg, /*is_patch*/ 1, /*active*/ 1);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_16(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x89 && MAC[2 + len_a] == 0x90) {
        int wg_len = MAC[3 + len_a] & 0x3F;
        int sg = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        DSD_FPRINTF(stderr, "\n MFID90 (Moto) Group Regroup Delete Command\n");
        DSD_FPRINTF(stderr, "  SG: %d", sg);
        int num_wg = (wg_len >= 2) ? ((wg_len - 2) / 2) : 0;
        for (int wi = 0; wi < num_wg && (6 + len_a + wi * 2 + 1) < 24; wi++) {
            int wg = (MAC[6 + len_a + wi * 2] << 8) | MAC[6 + len_a + wi * 2 + 1];
            if (wg != 0) {
                DSD_FPRINTF(stderr, " WG%d: %d", wi + 1, wg);
                p25_patch_remove_wgid(state, sg, wg);
            }
        }
        DSD_FPRINTF(stderr, "\n");
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_17(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x83 && MAC[2 + len_a] == 0x90) {
        int svc = MAC[3 + len_a];
        int sg = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int channel = (MAC[6 + len_a] << 8) | MAC[7 + len_a];
        int slot_idx = state->currentslot & 1;
        DSD_FPRINTF(stderr, "\n MFID90 (Moto) Group Regroup Voice Channel Update\n");
        p25p2_vpdu_print_svc_with_slot_state(opts, state, slot_idx, svc, /*set_packet_bit*/ 1);
        DSD_FPRINTF(stderr, "  SVC [%02X] SG: %d CHAN [%04X]", svc, sg, channel);
        long int freq = process_channel_to_freq(opts, state, channel);
        char suf[32];
        p25_format_chan_suffix(state, (uint16_t)channel, -1, suf, sizeof suf);
        DSD_SNPRINTF(state->active_channel[slot], sizeof(state->active_channel[slot]),
                     "MFID90 GRG VCH Upd: %04X%s SG: %d; ", channel, suf, sg);
        p25p2_vpdu_store_slot_svc(state, slot_idx, svc);
        state->last_active_time = time(NULL);
        DSD_FPRINTF(stderr, "\n");
        // Route through SM for tuning consideration
        if (opts->p25_trunk == 1 && channel != 0 && freq != 0) {
            p25p2_mac_handle(&mac_res, opts, state, channel, svc, sg, /*src*/ 0,
                             /*policy_encrypted*/ -1, /*policy_data*/ -1, /*emit_enc_lockout*/ 1);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_19(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1] == 0x91 && MAC[2] == 0x90) {
        uint8_t len = MAC[3]; //this indication is correct, 0x11, or 17 octets including opcode and mfid
        uint8_t mac_bits[24 * 8];
        DSD_MEMSET(mac_bits, 0, sizeof(mac_bits));
        uint8_t bytes[24];
        DSD_MEMSET(bytes, 0, sizeof(bytes));
        for (int bi = 0; bi < 24; bi++) {
            bytes[bi] = (uint8_t)MAC[bi];
        }
        unpack_byte_array_into_bit_array(bytes + 1, mac_bits, len);
        DSD_FPRINTF(stderr, "\n MFID90 (Moto) Talker Alias Header");
        apx_embedded_alias_header_phase2(opts, state, state->currentslot, mac_bits);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_20(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1] == 0x95 && MAC[2] == 0x90) {
        uint8_t len = MAC[3]; //this indication is correct, 0x11, or 17 octets including opcode and mfid
        uint8_t mac_bits[24 * 8];
        DSD_MEMSET(mac_bits, 0, sizeof(mac_bits));
        uint8_t bytes[24];
        DSD_MEMSET(bytes, 0, sizeof(bytes));
        for (int bi = 0; bi < 24; bi++) {
            bytes[bi] = (uint8_t)MAC[bi];
        }
        unpack_byte_array_into_bit_array(bytes + 1, mac_bits, len);
        DSD_FPRINTF(stderr, "\n MFID90 (Moto) Talker Alias Blocks");
        apx_embedded_alias_blocks_phase2(opts, state, state->currentslot, mac_bits);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_21(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x78) {
        int TWV = MAC[2 + len_a]; //TWUID Validity
        int SSA = (MAC[3 + len_a] << 16) | (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int SSS = (MAC[6 + len_a] << 16) | (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int RPL = MAC[9 + len_a];
        DSD_FPRINTF(stderr, "\n System Service Broadcast - Abbreviated \n");
        DSD_FPRINTF(stderr, "  TWV: %02X SSA: %06X; SSS: %06X; RPL: %02X", TWV, SSA, SSS, RPL);
        p25_store_system_service_broadcast(state, (uint32_t)SSA, (uint32_t)SSS, (uint8_t)RPL);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_22(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x7A) {
        int lra = MAC[2 + len_a];
        int lsysid = ((MAC[3 + len_a] & 0xF) << 8) | MAC[4 + len_a];
        int rfssid = MAC[5 + len_a];
        int siteid = MAC[6 + len_a];
        int channel = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int sysclass = MAC[9 + len_a];
        DSD_FPRINTF(stderr, "\n RFSS Status Broadcast - Implicit \n");
        DSD_FPRINTF(stderr, "  LRA [%02X] SYSID [%03X] RFSS ID [%03d] SITE ID [%03d] CHAN [%04X] SSC [%02X] ", lra,
                    lsysid, rfssid, siteid, channel, sysclass);
        process_channel_to_freq(opts, state, channel);

        p25_store_site_lra(state, (uint8_t)lra);
        p25_store_site_network_active(state, (uint8_t)((MAC[3 + len_a] & 0x10U) != 0U));
        state->p2_siteid = siteid;
        state->p2_rfssid = rfssid;
        // Promote any matching IDENs to trusted on site identification
        p25_confirm_idens_for_current_site(state);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_23(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xFA) {
        int lra = MAC[2 + len_a];
        int lsysid = ((MAC[3 + len_a] & 0xF) << 8) | MAC[4 + len_a];
        int rfssid = MAC[5 + len_a];
        int siteid = MAC[6 + len_a];
        int channelt = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int channelr = (MAC[9 + len_a] << 8) | MAC[10 + len_a];
        int sysclass = MAC[11 + len_a];
        DSD_FPRINTF(stderr, "\n RFSS Status Broadcast - Explicit \n");
        DSD_FPRINTF(
            stderr,
            "  LRA [%02X] SYSID [%03X] RFSS ID [%03d] SITE ID [%03d]\n  CHAN-T [%04X] CHAN-R [%02X] SSC [%02X] ", lra,
            lsysid, rfssid, siteid, channelt, channelr, sysclass);
        process_channel_to_freq(opts, state, channelt);
        process_channel_to_freq(opts, state, channelr);

        p25_store_site_lra(state, (uint8_t)lra);
        p25_store_site_network_active(state, (uint8_t)((MAC[3 + len_a] & 0x10U) != 0U));
        state->p2_siteid = siteid;
        state->p2_rfssid = rfssid;
        p25_confirm_idens_for_current_site(state);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_24(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] != 0xA0 && MAC[1 + len_a] != 0xAC && MAC[1 + len_a] != 0xB0 && MAC[2 + len_a] == 0xA4) {
        // 6.2.36 Manufacturer Specific regarding octet 3 as len
        int len = MAC[3 + len_a] & 0x3F;

        //sanity check that we don't exceed the max MAC array size
        if (len > 24) {
            len = 24; //should never exceed this len, but just in case it does
        }

        //Harris "Talker" Alias
        if (MAC[1 + len_a] == 0xA8) {
            DSD_FPRINTF(stderr, "\n MFID A4 (Harris); VCH %d;", slot);
            uint8_t bytes[24];
            DSD_MEMSET(bytes, 0, sizeof(bytes));
            for (int8_t bi = 0; bi < 24; bi++) {
                bytes[bi] = (uint8_t)MAC[bi];
            }
            l3h_embedded_alias_decode(opts, state, slot, len, bytes);
        }

        else if (MAC[1 + len_a]
                 == 0x81) //speculative based on the EDACS message that is also flushed with all F hex values
        {
            DSD_FPRINTF(stderr, "\n MFID A4 (Harris) Group Regroup Bitmap: ");
            for (i = 4; i <= len; i++) {
                DSD_FPRINTF(stderr, "%02llX", MAC[i + len_a]);
            }
        }

        else {
            int res = MAC[3 + len_a] >> 6;
            DSD_FPRINTF(stderr, "\n MFID A4 (Harris); Res: %d; Len: %d; Opcode: %02llX; ", res, len,
                        MAC[1 + len_a] & 0x3F); //first two bits are the b0 and b1
            for (i = 4; i <= len; i++) {
                DSD_FPRINTF(stderr, "%02llX", MAC[i + len_a]);
            }
        }

        //assign here so we don't read an extra opcode value, like MAC Release on FL-DCC-1 (0x31 opcode)
        len_b = len;
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_25(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[len_a + 1] == 0x80 && MAC[len_a + 2] != 0xA4 && MAC[len_a + 2] != 0x90) {
        int unk1 =
            MAC[len_a
                + 1]; //assuming this is the octet set for the 'manufacturer specific' message, may only be the MSBit
        int unk2 = MAC[len_a + 2]; //This field is observed as 0xAA, unknown if this is an opcode, or other MFID
        int mfid =
            MAC[len_a + 3]; //This is where the 0xA4 (Harris) Identifier is found in this message, as opposed to +2
        int len = MAC[len_a + 4] & 0x3F;
        ; //0x11 or 17 dec sounds reasonable, but cannot verify
        DSD_FPRINTF(stderr, "\n MFID %02X (Harris); Len: %d; Opcode: %02X/%02X;", mfid, len, unk1, unk2);

        //convert bytes to bits, may move this up top
        uint8_t mac_bits[24 * 8];
        DSD_MEMSET(mac_bits, 0, sizeof(mac_bits));
        int l, x, z = 0;
        for (l = 0; l < 24; l++) {
            for (x = 0; x < 8; x++) {
                mac_bits[z++] = (((uint8_t)MAC[l] << x) & 0x80) >> 7;
            }
        }

        int tsrc = 0;
        if (slot == 0 && state->lastsrc != 0) {
            tsrc = state->lastsrc;
        }
        if (slot == 1 && state->lastsrcR != 0) {
            tsrc = state->lastsrcR;
        }

        nmea_harris(opts, state, mac_bits + 0, tsrc, slot); //new

        len_b = 17;
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_26(p25p2_vpdu_ctx* ctx) {
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[len_a + 1] == 0xB5 && MAC[len_a + 2] == 0xD8) {
        uint8_t mfid = MAC[len_a + 2];
        uint16_t sc =
            (MAC[len_a + 4] << 8)
            + MAC[len_a + 5]; //its possible that not all bits in these two bytes are slots, similar to sync_bcst
        uint8_t len =
            MAC[len_a + 3] & 0x3F; //5 on this opcode (including opcode, mfid, len, and two bytes for slot count)

        //confirmed, this is the same as the sync_bcst slot counter value up to 8000 (0x1F40)
        sc &= 0x1FFF;

        DSD_FPRINTF(stderr, "\n MFID %02X (Tait); Len: %d; Micro Slot Counter: %04X;", mfid, len, sc);

        len_b = 5;
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_27(p25p2_vpdu_ctx* ctx) {
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[len_a + 1] != 0xB5 && MAC[len_a + 2] == 0xD8) {
        uint8_t mfid = MAC[len_a + 2];
        uint8_t len = MAC[len_a + 3] & 0x3F;
        DSD_FPRINTF(stderr, "\n MFID %02X (Tait); Len: %d; Opcode: %02llX;", mfid, len, MAC[len_a + 1]);

        //sanity check
        if (len > 24) {
            len = 24;
        }

        //dump entire payload
        DSD_FPRINTF(stderr, " Payload: ");
        for (i = 4; i < len; i++) {
            DSD_FPRINTF(stderr, "%02llX", MAC[i + len_a]);
        }

        len_b = len;
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_28(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x70) {
        //NOTE: I've observed the minute value on Harris (Duke) does not work the same (rolls over every ~3 minutes)
        //as it does on a Moto system (actual minute value), may want to expose and check mm or mc bits (AABC-D Page 199-200)
        DSD_FPRINTF(stderr, "\n Synchronization Broadcast");
        int us = (MAC[3 + len_a] >> 3) & 0x1;  //synced or unsynced FDMA to TDMA
        int ist = (MAC[3 + len_a] >> 2) & 0x1; //IST bit tells us if time is realiable, synced to external source
        int mm = (MAC[3 + len_a] >> 1) & 0x1;  //Minute / Microslot Boundary Unlocked
        int mc = (((MAC[3 + len_a] >> 0) & 0x1) << 1) + (((MAC[4 + len_a] >> 7) & 0x1) << 0); //Minute Correction
        int vl = (MAC[4 + len_a] >> 6) & 0x1; //Local Time Offset if Valid
        int ltoff = (MAC[4 + len_a] & 0x3F);
        int year = MAC[5 + len_a] >> 1;
        int month = ((MAC[5 + len_a] & 0x1) << 3) | (MAC[6 + len_a] >> 5);
        int day = (MAC[6 + len_a] & 0x1F);
        int min = ((MAC[7 + len_a] & 0x7) << 3) | (MAC[8 + len_a] >> 5);
        int slots = ((MAC[8 + len_a] & 0x1F) << 8) | MAC[9 + len_a];
        int sign = (ltoff & 0x20) >> 5;
        float offhour = 0;

        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, "\n");
            if (us) {
                DSD_FPRINTF(stderr, " Unsynchronized Slots;");
            }
            if (ist) {
                DSD_FPRINTF(stderr, " External System Time Sync;");
            }
            if (mm) {
                DSD_FPRINTF(stderr, " Minute / Microslots Boundary Unlocked;"); //just a rolling counter
            }
            if (mc) {
                DSD_FPRINTF(stderr, " Minute Correction: +%.01f ms;", (float)mc * 2.5f);
            }
            if (vl) {
                DSD_FPRINTF(stderr, " Local Time Offset Valid;");
            }
        }

        //calculate local time (on system) by looking at offset and subtracting 30 minutes increments, or divide by 2 for hourly
        if (sign == 1) {
            offhour = -((float)(ltoff & 0x1F) / 2.0f);
        } else {
            offhour = ((float)(ltoff & 0x1F) / 2.0f);
        }

        int seconds = slots / 135; //very rough estimation, but may be close enough for grins
        if (seconds > 59) {
            seconds = 59; //sanity check for rounding error
        }

        if (year != 0) // If time is synced in this PDU
        {
            int hour = MAC[7 + len_a] >> 3;
            DSD_FPRINTF(stderr, "\n  Date: 20%02d.%02d.%02d Time: %02d:%02d:%02d UTC", year, month, day, hour, min,
                        seconds);
            if (offhour != 0) { //&& vl == 1
                DSD_FPRINTF(stderr, "\n  Local Time Offset: %.01f Hours;", offhour);
            }
        }
        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, "\n US: %d; IST: %d; MM: %d; MC: %d; VL: %d; Sync Slots: %d; ", us, ist, mm, mc, vl,
                        slots);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_29(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x74) {
        struct p25p2_iden_update up = {0};
        (void)p25p2_mac_decode_iden_vuhf(MAC, 2 + len_a, &up);
        state->p25_chan_iden = up.iden;
        int iden = state->p25_chan_iden;
        int bw_vu = up.bw_vu;
        int trans_off = up.trans_off;
        int chan_spac = up.chan_spac;
        long int base_freq = up.base_freq;

        // Validate that base_freq actually falls in VHF/UHF range (warning only)
        if (!p25_is_vhf_uhf_base_freq(base_freq)) {
            DSD_FPRINTF(stderr, "\n  WARNING: 0x74 IDEN_UP_VU base_freq %08lX outside VHF/UHF range", base_freq);
        }

        p25_invalidate_chan_map_for_iden(state, iden);

        // Write to FDMA IDEN entry
        {
            p25_iden_entry_t* e = &state->p25_iden_fdma[iden];
            e->base_freq = base_freq;
            e->chan_type = 1; // FDMA default
            e->chan_spac = chan_spac;
            e->trans_off = trans_off;
            e->bw_vu = (uint8_t)bw_vu;
            e->trust = (state->p25_cc_freq != 0 && opts->p25_is_tuned == 0) ? 2 : 1;
            e->populated = 1;
            e->wacn = state->p2_wacn;
            e->sysid = state->p2_sysid;
            e->rfss = state->p2_rfssid;
            e->site = state->p2_siteid;
            state->p25_chan_tdma_explicit[iden] |= 1; // bit0 = has FDMA/non-TDMA entry
        }
        p25_resolve_pending_announcements(opts, state);

        DSD_FPRINTF(stderr, "\n Identifier Update UHF/VHF\n");
        DSD_FPRINTF(stderr,
                    "  Channel Identifier [%01X] BW [%01X] Transmit Offset [%04X]\n  Channel Spacing [%03X] Base "
                    "Frequency [%08lX] [%09ld]",
                    state->p25_chan_iden, bw_vu, trans_off, chan_spac, base_freq, base_freq * 5);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_30(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x7D) {
        struct p25p2_iden_update up = {0};
        (void)p25p2_mac_decode_iden_standard(MAC, 2 + len_a, &up);
        state->p25_chan_iden = up.iden;
        int iden = state->p25_chan_iden;
        long int base_freq = up.base_freq;
        int bw = up.bandwidth;
        int trans_off = up.trans_off;
        int chan_spac = up.chan_spac;

        p25_invalidate_chan_map_for_iden(state, iden);

        // Write to FDMA IDEN entry
        {
            p25_iden_entry_t* e = &state->p25_iden_fdma[iden];
            e->base_freq = base_freq;
            e->chan_type = 1; // FDMA default
            e->chan_spac = chan_spac;
            e->trans_off = trans_off;
            e->bw_vu = 0;
            e->trust = (state->p25_cc_freq != 0 && opts->p25_is_tuned == 0) ? 2 : 1;
            e->populated = 1;
            e->wacn = state->p2_wacn;
            e->sysid = state->p2_sysid;
            e->rfss = state->p2_rfssid;
            e->site = state->p2_siteid;
            state->p25_chan_tdma_explicit[iden] |= 1; // bit0 = has FDMA/non-TDMA entry
        }
        p25_resolve_pending_announcements(opts, state);

        DSD_FPRINTF(stderr, "\n Identifier Update (8.3.1.23)\n");
        DSD_FPRINTF(stderr,
                    "  Channel Identifier [%01X] BW [%01X] Transmit Offset [%04X]\n  Channel Spacing [%03X] Base "
                    "Frequency [%08lX] [%09ld]",
                    state->p25_chan_iden, bw, trans_off, chan_spac, base_freq, base_freq * 5);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_31(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x73) {
        struct p25p2_iden_update up = {0};
        (void)p25p2_mac_decode_iden_tdma(MAC, 2 + len_a, &up);
        state->p25_chan_iden = up.iden;
        int iden = state->p25_chan_iden;
        int chan_type = up.chan_type;
        int trans_off = up.trans_off;
        int chan_spac = up.chan_spac;
        long int base_freq = up.base_freq;

        p25_invalidate_chan_map_for_iden(state, iden);

        // Route by ChannelType using the shared slot denominator table; types 3-15 are TDMA.
        {
            int is_tdma = p25_channel_type_is_tdma(chan_type);
            p25_iden_entry_t* e = is_tdma ? &state->p25_iden_tdma[iden] : &state->p25_iden_fdma[iden];
            e->base_freq = base_freq;
            e->chan_type = chan_type; // from MAC payload (4-bit)
            e->chan_spac = chan_spac;
            e->trans_off = trans_off;
            e->trust = (state->p25_cc_freq != 0 && opts->p25_is_tuned == 0) ? 2 : 1;
            e->populated = 1;
            e->wacn = state->p2_wacn;
            e->sysid = state->p2_sysid;
            e->rfss = state->p2_rfssid;
            e->site = state->p2_siteid;
            state->p25_chan_tdma_explicit[iden] |= is_tdma ? 2 : 1;
        }
        p25_resolve_pending_announcements(opts, state);

        DSD_FPRINTF(stderr, "\n Identifier Update for TDMA - Abbreviated\n");
        DSD_FPRINTF(stderr,
                    "  Channel Identifier [%01X] Channel Type [%01X] Transmit Offset [%04X]\n  Channel Spacing [%03X] "
                    "Base Frequency [%08lX] [%09ld]",
                    state->p25_chan_iden, chan_type, trans_off, chan_spac, base_freq, base_freq * 5);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_32(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xF3) {
        struct p25p2_iden_update up = {0};
        (void)p25p2_mac_decode_iden_tdma(MAC, 3 + len_a, &up);
        state->p25_chan_iden = up.iden;
        int iden = state->p25_chan_iden;
        int chan_type = up.chan_type;
        int trans_off = up.trans_off;
        int chan_spac = up.chan_spac;
        long int base_freq = up.base_freq;
        int lwacn = (MAC[11 + len_a] << 12) | (MAC[12 + len_a] << 4) | ((MAC[13 + len_a] & 0xF0) >> 4);
        int lsysid = ((MAC[13 + len_a] & 0xF) << 8) | MAC[14 + len_a];

        p25_invalidate_chan_map_for_iden(state, iden);

        // Route by ChannelType using the shared slot denominator table; types 3-15 are TDMA.
        {
            int is_tdma = p25_channel_type_is_tdma(chan_type);
            p25_iden_entry_t* e = is_tdma ? &state->p25_iden_tdma[iden] : &state->p25_iden_fdma[iden];
            e->base_freq = base_freq;
            e->chan_type = chan_type; // from MAC payload (4-bit)
            e->chan_spac = chan_spac;
            e->trans_off = trans_off;
            e->trust = (state->p25_cc_freq != 0 && opts->p25_is_tuned == 0
                        && state->p2_wacn == (unsigned long long)lwacn && state->p2_sysid == (unsigned long long)lsysid)
                           ? 2
                           : 1;
            e->populated = 1;
            e->wacn = (unsigned long long)lwacn;   // from extended payload
            e->sysid = (unsigned long long)lsysid; // from extended payload
            e->rfss = state->p2_rfssid;
            e->site = state->p2_siteid;
            state->p25_chan_tdma_explicit[iden] |= is_tdma ? 2 : 1;
        }
        p25_resolve_pending_announcements(opts, state);

        DSD_FPRINTF(stderr, "\n Identifier Update for TDMA - Extended\n");
        DSD_FPRINTF(stderr,
                    "  Channel Identifier [%01X] Channel Type [%01X] Transmit Offset [%04X]\n  Channel Spacing [%03X] "
                    "Base Frequency [%08lX] [%09ld]",
                    state->p25_chan_iden, chan_type, trans_off, chan_spac, base_freq, base_freq * 5);
        DSD_FPRINTF(stderr, "\n  WACN [%04X] SYSID [%04X]", lwacn, lsysid);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_33(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xE9) {
        int rfssid = MAC[2 + len_a];
        int siteid = MAC[3 + len_a];
        int channelt = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int channelr = (MAC[6 + len_a] << 8) | MAC[7 + len_a];
        int sysclass = MAC[8 + len_a];

        // state->p2_is_lcch == 1
        DSD_FPRINTF(stderr, "\n Secondary Control Channel Broadcast - Explicit\n");
        DSD_FPRINTF(stderr, "  RFSS [%03d] SITE ID [%03d] CHAN-T [%04X] CHAN-R [%04X] SSC [%02X]", rfssid, siteid,
                    channelt, channelr, sysclass);

        (void)process_channel_to_freq(opts, state, channelr);
        (void)p25_announce_secondary_cc_channel(opts, state, (uint16_t)channelt, (uint8_t)rfssid, (uint8_t)siteid,
                                                (uint8_t)sysclass);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_34(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x79) {
        int bridged_p1 = (MAC[len_a] == 0x07);
        int rfssid = MAC[2 + len_a];
        int siteid = MAC[3 + len_a];
        int channel1 = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int sysclass1 = MAC[6 + len_a];
        int channel2 = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int sysclass2 = MAC[9 + len_a];
        int channel2_valid = p25p2_sccb_implicit_channel_b_valid(bridged_p1, channel1, channel2, sysclass2);
        long int freq1 = 0;
        long int freq2 = 0;
        // state->p2_is_lcch == 1
        DSD_FPRINTF(stderr, "\n Secondary Control Channel Broadcast - Implicit\n");
        DSD_FPRINTF(stderr, "  RFSS[%03d] SITE ID [%03d] CHAN1 [%04X] SSC [%02X] CHAN2 [%04X] SSC [%02X]", rfssid,
                    siteid, channel1, sysclass1, channel2, sysclass2);

        freq1 = process_channel_to_freq(opts, state, channel1);
        if (channel2_valid) {
            freq2 = process_channel_to_freq(opts, state, channel2);
        }
        (void)p25_announce_secondary_cc_channel(opts, state, (uint16_t)channel1, (uint8_t)rfssid, (uint8_t)siteid,
                                                (uint8_t)sysclass1);
        if (channel2_valid) {
            (void)p25_announce_secondary_cc_channel(opts, state, (uint16_t)channel2, (uint8_t)rfssid, (uint8_t)siteid,
                                                    (uint8_t)sysclass2);
        }
        const long scc_freqs[2] = {freq1, freq2};

        //place the cc freq into the list at index 0 if 0 is empty so we can hunt for rotating CCs without user LCN list
        p25p2_seed_secondary_lcn_fallback(state, rfssid, siteid, scc_freqs, channel2_valid ? 2 : 1);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_35(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x80 && MAC[2 + len_a] == 0x90) {

        int svc = MAC[3 + len_a];
        int gr = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int src = (MAC[6 + len_a] << 16) | (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        DSD_FPRINTF(stderr, "\n VCH %d - Super Group %d SRC %d ", slot + 1, gr, src);
        p25p2_vpdu_print_svc_with_slot_state(opts, state, slot, svc, /*set_packet_bit*/ 0);
        DSD_FPRINTF(stderr, "MFID90 Group Regroup Voice");
        state->gi[slot] = 0;
        p25p2_vpdu_store_slot_svc(state, slot, svc);
        p25p2_vpdu_set_group_call_banner(state, slot, svc);
        // Treat observed Super Group activity as an active patch (vendor-specific signaling may differ)
        p25_patch_update(state, gr, /*is_patch*/ 1, /*active*/ 1);
        p25p2_vpdu_update_group_last_ids(state, slot, gr, src);

        if ((svc & 0x40) && opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && opts->trunk_tune_enc_calls == 0) {
            p25p2_vpdu_handle_group_voice_enc_fallback(opts, state, slot, gr);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_36(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xA0 && MAC[2 + len_a] == 0x90) {

        int svc = MAC[4 + len_a];
        int gr = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
        int src = (MAC[7 + len_a] << 16) | (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        DSD_FPRINTF(stderr, "\n VCH %d - Super Group %d SRC %d ", slot + 1, gr, src);
        p25p2_vpdu_print_svc_with_slot_state(opts, state, slot, svc, /*set_packet_bit*/ 0);
        DSD_FPRINTF(stderr, "MFID90 Group Regroup Voice");
        state->gi[slot] = 0;
        p25p2_vpdu_store_slot_svc(state, slot, svc);
        p25p2_vpdu_set_group_call_banner(state, slot, svc);
        p25_patch_update(state, gr, /*is_patch*/ 1, /*active*/ 1);

        uint32_t mfid90_wacn = (MAC[10 + len_a] << 16) | (MAC[11 + len_a] << 8) | (MAC[12 + len_a] & 0xF0);
        mfid90_wacn >>= 4;
        uint16_t mfid90_sys = (uint16_t)(((MAC[12 + len_a] & 0x0F) << 8) | MAC[13 + len_a]);
        DSD_FPRINTF(stderr, " EXT - FQSUID: %05X:%03X.%d", mfid90_wacn, mfid90_sys, src);

        p25p2_vpdu_update_group_last_ids(state, slot, gr, src);
        if (src != 0 && gr != 0) {
            p25_ga_add(state, (uint32_t)src, (uint16_t)gr);
        }
        if ((svc & 0x40) && opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && opts->trunk_tune_enc_calls == 0) {
            p25p2_vpdu_handle_group_voice_enc_fallback(opts, state, slot, gr);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_37(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xB0 && MAC[2 + len_a] == 0xA4) //&& MAC[2+len_a] == 0xA4
    {
        int len_grg = MAC[3 + len_a] & 0x3F; //MFID Len in Octets
        int tga = MAC[4 + len_a] >> 5;       //3 bit TGA values from GRG_Options
        int ssn = MAC[4 + len_a] & 0x1F;     //5 bit SSN from from GRG_Options

        DSD_FPRINTF(stderr, "\n MFID A4 (Harris) Group Regroup Explicit Encryption Command\n");
        if ((tga & 4) == 4) {
            DSD_FPRINTF(stderr, " Simulselect"); //one-way regroup
        } else {
            DSD_FPRINTF(stderr, " Patch"); //two-way regroup
        }
        if (tga & 1) {
            DSD_FPRINTF(stderr, " Active;"); //activated
        } else {
            DSD_FPRINTF(stderr, " Inactive;"); //deactivated
        }

        DSD_FPRINTF(stderr, " SSN: %02d;", ssn);

        if ((tga & 0x2) == 2) //group WGID to supergroup
        {
            int sg = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
            int key = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
            int alg = MAC[9 + len_a];
            int t1 = (MAC[10 + len_a] << 8) | MAC[11 + len_a];
            int t2 = (MAC[12 + len_a] << 8) | MAC[13 + len_a];
            int t3 = (MAC[14 + len_a] << 8) | MAC[15 + len_a];
            int t4 = (MAC[16 + len_a] << 8) | MAC[17 + len_a];
            UNUSED4(t1, t2, t3, t4);
            DSD_FPRINTF(stderr, " SG: %d; KEY ID: %04X; ALG: %02X;\n  ", sg, key, alg);
            int a = 0;
            int wgid = 0;

            for (int wi = 10; wi <= len_grg;) {
                //failsafe to prevent oob array
                if ((wi + len_a) > 20) {
                    ctx->end_pdu = 1;
                    goto BLOCK_END;
                }
                wgid = (MAC[10 + len_a + a] << 8) | MAC[11 + len_a + a];
                DSD_FPRINTF(stderr, "WGID: %d; ", wgid);
                p25_patch_add_wgid(state, sg, wgid);
                a = a + 2;
                wi = wi + 2;
            }

            // Update patch tracker for this SG (two-way patch if bit4 of TGA is 0)
            int is_patch = ((tga & 0x4) == 0) ? 1 : 0;
            int active = (tga & 0x1) ? 1 : 0;
            p25_patch_update(state, sg, is_patch, active);
            p25_patch_set_kas(state, sg, key, alg, ssn);

        }

        else if ((tga & 0x2) == 0) //individual WUID to supergroup
        {
            int sg = (MAC[5 + len_a] << 8) | MAC[6 + len_a];
            int key = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
            int t1 = (MAC[9 + len_a] << 16) | (MAC[10 + len_a] << 8) | MAC[11 + len_a];
            int t2 = (MAC[12 + len_a] << 16) | (MAC[13 + len_a] << 8) | MAC[14 + len_a];
            int t3 = (MAC[15 + len_a] << 16) | (MAC[16 + len_a] << 8) | MAC[17 + len_a];
            DSD_FPRINTF(stderr, "  SG: %d KEY ID: %04X", sg, key);
            DSD_FPRINTF(stderr, " WUID: %d; WUID: %d; WUID: %d; ", t1, t2, t3);
            p25_patch_add_wuid(state, sg, (uint32_t)t1);
            p25_patch_add_wuid(state, sg, (uint32_t)t2);
            p25_patch_add_wuid(state, sg, (uint32_t)t3);

            // Update patch tracker
            int is_patch = ((tga & 0x4) == 0) ? 1 : 0;
            int active = (tga & 0x1) ? 1 : 0;
            p25_patch_update(state, sg, is_patch, active);
            p25_patch_set_kas(state, sg, key, -1, ssn);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_38(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xEC && MAC[0] != 0x07) {
        int res = (MAC[3 + len_a] >> 2) & 0x3F;
        int RV = (MAC[2 + len_a] >> 0) & 0x3;
        int src = (MAC[8 + len_a] << 16) | (MAC[9 + len_a] << 8) | MAC[10 + len_a];
        int uwacn = (MAC[4 + len_a] << 12) | (MAC[5 + len_a] << 4) | ((MAC[6 + len_a] & 0xF0) >> 4);
        int usys = ((MAC[6 + len_a] & 0xF) << 8) | MAC[7 + len_a];
        DSD_FPRINTF(stderr, "\n Unit Registration Response - WACN: %05X; SYS: %03X; SRC: %d", uwacn, usys, src);
        if (res) {
            DSD_FPRINTF(stderr, " RES: %d;", res);
        }
        if (RV == 0) {
            DSD_FPRINTF(stderr, " REG_ACCEPT;");
            // Track affiliated RID
            p25_aff_register(state, (uint32_t)src);
        }
        if (RV == 1) {
            DSD_FPRINTF(stderr, " REG_FAIL;"); //RFSS was unable to verify
        }
        if (RV == 2) {
            DSD_FPRINTF(stderr, " REG_DENY;"); //Not allowed at this location
        }
        if (RV == 3) {
            DSD_FPRINTF(stderr, " REG_REFUSED;"); //WUID invalid but re-register after a user stimulus
        }
        DSD_FPRINTF(stderr, " - Extended;");
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_handle_location_registration_response(dsd_state* state, const unsigned long long int* MAC, int len_a) {
    int k = 1; // vPDU offset
    if (MAC[len_a] == 0x07) {
        k = 0; // TSBK offset
    }

    int res = (MAC[2 + len_a + k] >> 2) & 0x3F;
    int rv = MAC[2 + len_a + k] & 0x3;
    int group = (MAC[3 + len_a + k] << 8) | MAC[4 + len_a + k];
    int rfss = MAC[5 + len_a + k];
    int site = MAC[6 + len_a + k];
    int target = (MAC[7 + len_a + k] << 16) | (MAC[8 + len_a + k] << 8) | MAC[9 + len_a + k];

    DSD_FPRINTF(stderr, "\n Location Registration Response");
    DSD_FPRINTF(stderr, "\n  GROUP: %d RFSS: %03d SITE: %03d TARGET: %d", group, rfss, site, target);
    if (res != 0) {
        DSD_FPRINTF(stderr, " RES: %d;", res);
    }

    switch (rv) {
        case 0:
            DSD_FPRINTF(stderr, " REG_ACCEPT;");
            p25_aff_register(state, (uint32_t)target);
            p25_ga_add(state, (uint32_t)target, (uint16_t)group);
            break;
        case 1: DSD_FPRINTF(stderr, " REG_FAIL;"); break;
        case 2: DSD_FPRINTF(stderr, " REG_DENY;"); break;
        case 3: DSD_FPRINTF(stderr, " REG_REFUSED;"); break;
        default: break;
    }
}

static void
p25p2_vpdu_iter_block_39(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x6B) {
        p25p2_vpdu_handle_location_registration_response(state, MAC, len_a);
    }

    if (MAC[1 + len_a] == 0x6C) {
        /*
    			Unit Registration Response TSBK
    			P25 PDU Payload
    				[07][6C][01][EC][72][70][EC][72][70][EC][00][00]
    				[00][00][00][00][00][00][00][00][00][00][00][00]
    			*/
        int k = 1; //vPDU is one octet higher than TSBK
        if (MAC[len_a] == 0x07) {
            k = 0;
        }
        int res = (MAC[2 + len_a + k] >> 6) & 0x3;
        int RV = (MAC[2 + len_a + k] >> 4) & 0x3;
        int usite = ((MAC[2 + len_a + k] & 0xF) << 8) | MAC[3 + len_a + k];
        int sid = (MAC[4 + len_a + k] << 16) | (MAC[5 + len_a + k] << 8) | MAC[6 + len_a + k];
        int src = (MAC[7 + len_a + k] << 16) | (MAC[8 + len_a + k] << 8) | MAC[9 + len_a + k];
        DSD_FPRINTF(stderr, "\n Unit Registration Response - SITE: %03X SRC_ID: %d SRC: %d", usite, sid, src);
        if (res) {
            DSD_FPRINTF(stderr, " RES: %d;", res);
        }
        if (RV == 0) {
            DSD_FPRINTF(stderr, " REG_ACCEPT;");
            // Track affiliated RID
            p25_aff_register(state, (uint32_t)src);
        }
        if (RV == 1) {
            DSD_FPRINTF(stderr, " REG_FAIL;"); //RFSS was unable to verify
        }
        if (RV == 2) {
            DSD_FPRINTF(stderr, " REG_DENY;"); //Not allowed at this location
        }
        if (RV == 3) {
            DSD_FPRINTF(stderr, " REG_REFUSED;"); //WUID invalid but re-register after a user stimulus
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_40(p25p2_vpdu_ctx* ctx) {
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x6D && MAC[0 + len_a] != 0x07) {
        int k = 2; //vPDU is two octets higher than TSBK
        if (MAC[len_a] == 0x07) {
            k = 0;
        }
        int src = (MAC[2 + len_a + k] << 16) | (MAC[3 + len_a + k] << 8) | MAC[4 + len_a + k];
        int tgt = (MAC[5 + len_a + k] << 16) | (MAC[6 + len_a + k] << 8) | MAC[7 + len_a + k];
        DSD_FPRINTF(stderr, "\n Unit Registration - SRC: %d; TGT: %d;", src, tgt);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_41(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x6F || MAC[1 + len_a] == 0xEF) {
        int src = (MAC[7 + len_a] << 16) | (MAC[8 + len_a] << 8) | MAC[9 + len_a];
        int uwacn = (MAC[3 + len_a] << 12) | (MAC[4 + len_a] << 4) | ((MAC[5 + len_a] & 0xF0) >> 4);
        int usys = ((MAC[5 + len_a] & 0xF) << 8) | MAC[6 + len_a];
        DSD_FPRINTF(stderr, "\n Unit Deregistration Acknowlegement - WACN: %05X; SYS: %03X; SRC: %d", uwacn, usys, src);
        if (MAC[1 + len_a] == 0xEF) {
            DSD_FPRINTF(stderr, " - Extended;");
        }
        // Remove RID from affiliation table
        p25_aff_deregister(state, (uint32_t)src);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_42(p25p2_vpdu_ctx* ctx) {
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x71 || MAC[1 + len_a] == 0xF1) {
        DSD_FPRINTF(stderr, "\n Authentication Demand;");
        if (MAC[1 + len_a] == 0xF1) {
            DSD_FPRINTF(stderr, " - Extended;");
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_43(p25p2_vpdu_ctx* ctx) {
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x72 || MAC[1 + len_a] == 0xF2) {
        DSD_FPRINTF(stderr, "\n Authentication FNE Response;");
        if (MAC[1 + len_a] == 0xF2) {
            DSD_FPRINTF(stderr, " - Extended;");
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_44(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x31) {
        int uf = (MAC[2 + len_a] >> 7) & 1;
        int ca = (MAC[2 + len_a] >> 6) & 1;
        int resr1 = MAC[2 + len_a] & 0x1F;
        int add = (MAC[3 + len_a] << 16) | (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int resr2 = MAC[6 + len_a] >> 4;
        int cc = ((MAC[6 + len_a] & 0xF) << 8) | MAC[7 + len_a];
        int eslot = slot;
        double mac_hold = p25p2_vpdu_cfg_mac_hold_s(state, 0.75);
        double voice_hold = p25p2_vpdu_cfg_voice_hold_s(0.75);
        int other_audio = 0;

        DSD_FPRINTF(stderr, "\n MAC Release:  ");
        DSD_FPRINTF(stderr, uf ? "Forced; " : "Unforced; ");
        DSD_FPRINTF(stderr, ca ? "Audio Preemption; " : "Call Preemption; ");
        DSD_FPRINTF(stderr, "RES1: %d; ", resr1);
        DSD_FPRINTF(stderr, "RES2: %d; ", resr2);
        DSD_FPRINTF(stderr, "TGT: %d; ", add);
        DSD_FPRINTF(stderr, "CC: %03X; ", cc);

        p25p2_vpdu_gate_slot_audio(state, eslot);
        state->p25_p2_enc_lockout_muted[eslot & 1] = 0;
        other_audio = p25p2_vpdu_other_slot_audio_with_history(state, eslot, mac_hold, voice_hold);
        if (!other_audio) {
            (void)p25p2_vpdu_force_release_after_grace(opts, state);
        } else {
            p25p2_vpdu_clear_slot_banner(state, eslot);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_45(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x1 || MAC[1 + len_a] == 0x21) {
        int svc = MAC[2 + len_a];
        int gr = (MAC[3 + len_a] << 8) | MAC[4 + len_a];
        int src = (MAC[5 + len_a] << 16) | (MAC[6 + len_a] << 8) | MAC[7 + len_a];
        int slot_idx = state->currentslot & 1;
        unsigned long long int src_suid = 0;

        if (MAC[1 + len_a] == 0x21) {
            src_suid = (MAC[8 + len_a] << 48ULL) | (MAC[9 + len_a] << 40ULL) | (MAC[10 + len_a] << 32ULL)
                       | (MAC[11 + len_a] << 24ULL) | (MAC[12 + len_a] << 16ULL) | (MAC[13 + len_a] << 8ULL)
                       | (MAC[14 + len_a] << 0ULL);
            src = src_suid & 0xFFFFFF;
        }

        DSD_FPRINTF(stderr, "\n VCH %d - TG: %d; SRC: %d; ", slot + 1, gr, src);
        state->p25_p2_last_mac_active[slot] = time(NULL);
        if (MAC[1 + len_a] == 0x21) {
            DSD_FPRINTF(stderr, "SUID: %08llX-%08d; ", src_suid >> 24, src);
        }

        p25p2_vpdu_print_svc_with_slot_state(opts, state, slot_idx, svc, /*set_packet_bit*/ 0);
        DSD_FPRINTF(stderr, " Group Voice");
        state->gi[slot] = 0;
        p25p2_vpdu_store_slot_svc(state, slot, svc);
        p25p2_vpdu_set_group_call_banner(state, slot, svc);
        DSD_FPRINTF(stderr, (MAC[1 + len_a] == 0x21) ? " - Extended " : " - Abbreviated ");
        p25p2_vpdu_update_group_last_ids(state, slot, gr, src);

        if ((svc & 0x40) && opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && opts->trunk_tune_enc_calls == 0) {
            p25p2_vpdu_handle_group_voice_enc_fallback(opts, state, slot, gr);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_46(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x2 || MAC[1 + len_a] == 0x22) {
        int svc = MAC[2 + len_a];
        int gr = (MAC[3 + len_a] << 16) | (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int src = (MAC[6 + len_a] << 16) | (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        unsigned long long int src_suid = 0;

        if (MAC[1 + len_a] == 0x22) {
            src_suid = (MAC[9 + len_a] << 48ULL) | (MAC[10 + len_a] << 40ULL) | (MAC[11 + len_a] << 32ULL)
                       | (MAC[12 + len_a] << 24ULL) | (MAC[13 + len_a] << 16ULL) | (MAC[14 + len_a] << 8ULL)
                       | (MAC[15 + len_a] << 0ULL);
            src = src_suid & 0xFFFFFF;
        }

        DSD_FPRINTF(stderr, "\n VCH %d - TGT: %d; SRC %d; ", slot + 1, gr, src);
        state->p25_p2_last_mac_active[slot] = time(NULL);
        if (MAC[1 + len_a] == 0x22) {
            DSD_FPRINTF(stderr, "SUID: %08llX-%08d; ", src_suid >> 24, src);
        }

        p25p2_vpdu_print_svc_no_state(opts, svc);
        DSD_FPRINTF(stderr, " Unit to Unit Voice");
        state->gi[slot] = 1;
        p25p2_vpdu_store_slot_svc(state, slot, svc);
        p25p2_vpdu_set_private_call_banner(state, slot, svc);
        p25p2_vpdu_update_private_last_ids(state, slot, gr, src);

        if ((svc & 0x40) && opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && opts->trunk_tune_enc_calls == 0) {
            p25p2_vpdu_handle_unit_voice_enc_fallback(opts, state, slot, gr);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_apply_nsb_identity(dsd_state* state, int lwacn, int lsysid, int lcolorcode) {
    if (!state || state->p2_hardset != 0) {
        return;
    }
    if (lwacn == 0 && lsysid == 0) {
        return;
    }
    if (p25_update_system_identity(state, (unsigned long long)lwacn, (unsigned long long)lsysid)) {
        state->p2_cc = lcolorcode;
    }
}

static void
p25p2_vpdu_store_nsb_identity_metadata(dsd_state* state, int lwacn, int lsysid, int lcolorcode) {
    p25p2_vpdu_apply_nsb_identity(state, lwacn, lsysid, lcolorcode);
}

static void
p25p2_vpdu_note_nsb_system_tdma(dsd_state* state) {
    if (state) {
        state->p25_sys_is_tdma = 1; // system carries Phase 2 voice (TDMA present)
    }
}

static void
p25p2_vpdu_accept_nsb_cc(dsd_opts* opts, dsd_state* state, int lwacn, int lsysid, int lcolorcode, int seed_lcn0) {
    const long neigh[1] = {state->p25_cc_freq};
    p25_sm_on_neighbor_update(opts, state, neigh, 1);
    p25p2_vpdu_note_nsb_system_tdma(state);
    state->p25_cc_is_tdma = 1; // TDMA control channel (QPSK, 6000 sym/s)

    // Only update system identity and potentially reset IDEN tables when values
    // are sane (non-zero) and we have a valid frequency mapping.
    p25p2_vpdu_apply_nsb_identity(state, lwacn, lsysid, lcolorcode);

    if (seed_lcn0 && (state->trunk_lcn_freq[0] == 0 || state->trunk_lcn_freq[0] != state->p25_cc_freq)) {
        state->trunk_lcn_freq[0] = state->p25_cc_freq;
    }
    p25_confirm_idens_for_current_site(state);
}

static void
p25p2_vpdu_log_rejected_nsb_cc(const char* label, long freq, int channel) {
    if (freq > 0) {
        DSD_FPRINTF(stderr, "\n  %s: ignoring CC update while voice-tuned (freq=%ld)", label, freq);
    } else {
        DSD_FPRINTF(stderr, "\n  %s: ignoring invalid channel->freq (CHAN-T=%04X)", label, channel);
    }
}

static void
p25p2_vpdu_iter_block_47(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x7B) {
        int lra = MAC[2 + len_a];
        int lwacn = (MAC[3 + len_a] << 12) | (MAC[4 + len_a] << 4) | ((MAC[5 + len_a] & 0xF0) >> 4);
        int lsysid = ((MAC[5 + len_a] & 0xF) << 8) | MAC[6 + len_a];
        int channel = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int sysclass = MAC[9 + len_a];
        int lcolorcode = ((MAC[10 + len_a] & 0xF) << 8) | MAC[11 + len_a];
        UNUSED(sysclass);
        DSD_FPRINTF(stderr, "\n Network Status Broadcast - Abbreviated \n");
        DSD_FPRINTF(stderr, "  LRA [%02X] WACN [%05X] SYSID [%03X] NAC [%03X] CHAN-T [%04X]", lra, lwacn, lsysid,
                    lcolorcode, channel);
        long int cc_freq = process_channel_to_freq(opts, state, channel);
        p25p2_vpdu_note_nsb_system_tdma(state);
        int accepted_cc = p25_cc_update_primary_from_network_status(opts, state, cc_freq);
        const int cc_metadata_allowed = accepted_cc || !p25_cc_update_is_voice_tuned(opts);
        if (cc_metadata_allowed) {
            p25p2_vpdu_store_nsb_identity_metadata(state, lwacn, lsysid, lcolorcode);
            p25_store_site_lra(state, (uint8_t)lra);
        }
        if (accepted_cc) {
            p25p2_vpdu_accept_nsb_cc(opts, state, lwacn, lsysid, lcolorcode, 1);
        } else {
            p25p2_vpdu_log_rejected_nsb_cc("P25 NSB", cc_freq, channel);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_48(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xFB) {
        int lra = MAC[2 + len_a];
        int lwacn = (MAC[3 + len_a] << 12) | (MAC[4 + len_a] << 4) | ((MAC[5 + len_a] & 0xF0) >> 4);
        int lsysid = ((MAC[5 + len_a] & 0xF) << 8) | MAC[6 + len_a];
        int channelt = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int channelr = (MAC[9 + len_a] << 8) | MAC[10 + len_a];
        int sysclass = MAC[9 + len_a];
        int lcolorcode = ((MAC[12 + len_a] & 0xF) << 8) | MAC[13 + len_a];
        UNUSED(sysclass);
        DSD_FPRINTF(stderr, "\n Network Status Broadcast - Extended \n");
        DSD_FPRINTF(stderr, "  LRA [%02X] WACN [%05X] SYSID [%03X] NAC [%03X] CHAN-T [%04X] CHAN-R [%04X]", lra, lwacn,
                    lsysid, lcolorcode, channelt, channelr);
        long int nf1 = process_channel_to_freq(opts, state, channelt);
        (void)process_channel_to_freq(opts, state, channelr);
        p25p2_vpdu_note_nsb_system_tdma(state);
        int accepted_cc = p25_cc_update_primary_from_network_status(opts, state, nf1);
        const int cc_metadata_allowed = accepted_cc || !p25_cc_update_is_voice_tuned(opts);
        if (cc_metadata_allowed) {
            p25p2_vpdu_store_nsb_identity_metadata(state, lwacn, lsysid, lcolorcode);
            p25_store_site_lra(state, (uint8_t)lra);
        }
        if (accepted_cc) {
            p25p2_vpdu_accept_nsb_cc(opts, state, lwacn, lsysid, lcolorcode, 0);
        } else {
            p25p2_vpdu_log_rejected_nsb_cc("P25 NSB-EXT", nf1, channelt);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_49(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x7C || (MAC[1 + len_a] == 0x7E && MAC[len_a] == 0x07)) {
        /* Bridged P1 0x3C/0x3E adjacency carries CFVA/reserved here, not SYSID. */
        int bridged_p1_adj = (MAC[len_a] == 0x07 && (MAC[1 + len_a] == 0x7C || MAC[1 + len_a] == 0x7E));
        int uncoordinated = (MAC[1 + len_a] == 0x7E);
        int lra = MAC[2 + len_a];
        int cfva = MAC[3 + len_a] >> 4;
        int lsysid = bridged_p1_adj ? 0 : (((MAC[3 + len_a] & 0xF) << 8) | MAC[4 + len_a]);
        int rfssid = MAC[5 + len_a];
        int siteid = MAC[6 + len_a];
        int channelt = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int sysclass = MAC[9 + len_a];
        DSD_FPRINTF(stderr, "\n Adjacent Status Broadcast%s - Abbreviated\n",
                    uncoordinated ? " Uncoordinated Band Plan" : "");
        DSD_FPRINTF(stderr, "  LRA [%02X] RFSS[%03d] SITE [%03d] SYSID [%03X] CHAN-T [%04X] SSC [%02X]\n  ", lra,
                    rfssid, siteid, lsysid, channelt, sysclass);
        if (cfva & 0x8) {
            DSD_FPRINTF(stderr, " Conventional");
        }
        if (cfva & 0x4) {
            DSD_FPRINTF(stderr, " Failure Condition");
        }
        if (cfva & 0x2) {
            DSD_FPRINTF(stderr, " Up to Date (Correct)");
        } else {
            DSD_FPRINTF(stderr, " Last Known");
        }
        if (cfva & 0x1) {
            DSD_FPRINTF(stderr, " Valid RFSS Connection Active");
        }
        const p25_neighbor_channel_announcement_t announcement = {
            .channel = (uint16_t)channelt,
            .sysid = (uint16_t)lsysid,
            .rfss = (uint8_t)rfssid,
            .site = (uint8_t)siteid,
            .lra = (uint8_t)lra,
            .cfva = (uint8_t)cfva,
            .lra_valid = 1U,
            .cfva_valid = 1U,
        };
        (void)p25_announce_neighbor_channel_ex(opts, state, &announcement);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_50(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xFC) {
        int lra = MAC[2 + len_a];
        int cfva = MAC[3 + len_a] >> 4;
        int lsysid = ((MAC[3 + len_a] & 0xF) << 8) | MAC[4 + len_a];
        int rfssid = MAC[5 + len_a];
        int siteid = MAC[6 + len_a];
        int channelt = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int channelr = (MAC[9 + len_a] << 8) | MAC[10 + len_a];
        int sysclass = MAC[11 + len_a]; //need to re-check this
        DSD_FPRINTF(stderr, "\n Adjacent Status Broadcast - Extended\n");
        DSD_FPRINTF(stderr,
                    "  LRA [%02X] RFSS[%03d] SITE [%03d] SYSID [%03X] CHAN-T [%04X] CHAN-R [%04X] SSC [%02X]\n  ", lra,
                    rfssid, siteid, lsysid, channelt, channelr, sysclass);
        if (cfva & 0x8) {
            DSD_FPRINTF(stderr, " Conventional");
        }
        if (cfva & 0x4) {
            DSD_FPRINTF(stderr, " Failure Condition");
        }
        if (cfva & 0x2) {
            DSD_FPRINTF(stderr, " Up to Date (Correct)");
        } else {
            DSD_FPRINTF(stderr, " Last Known");
        }
        if (cfva & 0x1) {
            DSD_FPRINTF(stderr, " Valid RFSS Connection Active");
        }
        (void)process_channel_to_freq(opts, state, channelr);
        const p25_neighbor_channel_announcement_t announcement = {
            .channel = (uint16_t)channelt,
            .sysid = (uint16_t)lsysid,
            .rfss = (uint8_t)rfssid,
            .site = (uint8_t)siteid,
            .lra = (uint8_t)lra,
            .cfva = (uint8_t)cfva,
            .lra_valid = 1U,
            .cfva_valid = 1U,
        };
        (void)p25_announce_neighbor_channel_ex(opts, state, &announcement);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_51(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0xFE) {
        int lra = MAC[2 + len_a];
        int cfva = MAC[3 + len_a] >> 4;
        int lsysid = ((MAC[3 + len_a] & 0xF) << 8) | MAC[4 + len_a];
        int rfssid = MAC[5 + len_a];
        int siteid = MAC[6 + len_a];
        int channelt = (MAC[7 + len_a] << 8) | MAC[8 + len_a];
        int channelr = (MAC[9 + len_a] << 8) | MAC[10 + len_a];
        int sysclass = MAC[11 + len_a];
        int lwacn = (MAC[12 + len_a] << 12) | (MAC[13 + len_a] << 4) | ((MAC[14 + len_a] & 0xF0) >> 4);
        DSD_FPRINTF(stderr, "\n Adjacent Status Broadcast - Extended Explicit\n");
        DSD_FPRINTF(stderr,
                    "  LRA [%02X] RFSS[%03d] SITE [%03d] SYSID [%03X] CHAN-T [%04X] CHAN-R [%04X] SSC [%02X] WACN "
                    "[%05X]\n  ",
                    lra, rfssid, siteid, lsysid, channelt, channelr, sysclass, lwacn);
        if (cfva & 0x8) {
            DSD_FPRINTF(stderr, " Conventional");
        }
        if (cfva & 0x4) {
            DSD_FPRINTF(stderr, " Failure Condition");
        }
        if (cfva & 0x2) {
            DSD_FPRINTF(stderr, " Up to Date (Correct)");
        } else {
            DSD_FPRINTF(stderr, " Last Known");
        }
        if (cfva & 0x1) {
            DSD_FPRINTF(stderr, " Valid RFSS Connection Active");
        }
        (void)process_channel_to_freq(opts, state, channelr);
        const p25_neighbor_channel_announcement_t announcement = {
            .channel = (uint16_t)channelt,
            .wacn = (uint32_t)lwacn,
            .sysid = (uint16_t)lsysid,
            .rfss = (uint8_t)rfssid,
            .site = (uint8_t)siteid,
            .lra = (uint8_t)lra,
            .cfva = (uint8_t)cfva,
            .wacn_valid = 1U,
            .lra_valid = 1U,
            .cfva_valid = 1U,
        };
        (void)p25_announce_neighbor_channel_ex(opts, state, &announcement);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_52(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x68) {
        int k = 1; // vPDU offset
        if (MAC[len_a] == 0x07) {
            k = 0; // TSBK offset
        }
        int lg = (MAC[2 + len_a + k] >> 7) & 0x1;
        int gav = MAC[2 + len_a + k] & 0x3;
        int aga = (MAC[3 + len_a + k] << 8) | MAC[4 + len_a + k];
        int ga = (MAC[5 + len_a + k] << 8) | MAC[6 + len_a + k];
        int ta = (MAC[7 + len_a + k] << 16) | (MAC[8 + len_a + k] << 8) | MAC[9 + len_a + k];
        DSD_FPRINTF(stderr, "\n Group Affiliation Response");
        DSD_FPRINTF(stderr, "\n  LG: %d GAV: %d AGA: %d GA: %d TA: %d", lg, gav, aga, ga, ta);
        // Track RID affiliation (TA is the target address / unit ID)
        if (gav == 0) { // Accepted
            p25_aff_register(state, (uint32_t)ta);
            // Also track group affiliation (RID -> TG mapping)
            p25_ga_add(state, (uint32_t)ta, (uint32_t)ga);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_53(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x64) {
        uint8_t class_id = (uint8_t)MAC[2 + len_a];
        uint8_t operand = (uint8_t)MAC[3 + len_a];
        uint32_t argument = (uint32_t)((MAC[4 + len_a] << 16) | (MAC[5 + len_a] << 8) | MAC[6 + len_a]);
        uint32_t target = (uint32_t)((MAC[7 + len_a] << 16) | (MAC[8 + len_a] << 8) | MAC[9 + len_a]);

        DSD_FPRINTF(stderr, "\n Extended Function Command - Abbreviated");
        DSD_FPRINTF(stderr, "\n  Class: %02X Operand: %02X Arg/Src: %06X Target: %u", class_id, operand, argument,
                    target);
        if (class_id == 0) {
            DSD_FPRINTF(stderr, " %s", p25_extended_function_class0_operand_label(operand));
            if (p25_extended_function_operand_is_ack(operand)) {
                DSD_FPRINTF(stderr, " Ack");
            }
        } else {
            DSD_FPRINTF(stderr, " Other Command");
        }
        DSD_FPRINTF(stderr, ";");
    }

    if (MAC[1 + len_a] == 0x69) {
        int rfssid = MAC[2 + len_a];
        int siteid = MAC[3 + len_a];
        int channelt = (MAC[4 + len_a] << 8) | MAC[5 + len_a];
        int channelr = (MAC[6 + len_a] << 8) | MAC[7 + len_a];
        int sysclass = MAC[8 + len_a];
        DSD_FPRINTF(stderr, "\n Secondary Control Channel Broadcast - Explicit (from P1 TSBK)\n");
        DSD_FPRINTF(stderr, "  RFSS [%03d] SITE ID [%03d] CHAN-T [%04X] CHAN-R [%04X] SSC [%02X]", rfssid, siteid,
                    channelt, channelr, sysclass);
        long int sccf = process_channel_to_freq(opts, state, channelt);
        (void)process_channel_to_freq(opts, state, channelr);
        // Add to CC candidate list for hunting
        if (sccf > 0 && p25p2_sccb_matches_current_site(state, rfssid, siteid) && state->trunk_lcn_freq[1] == 0) {
            state->trunk_lcn_freq[1] = sccf;
            state->lcn_freq_count = 2;
        } else if (sccf > 0 && p25p2_sccb_matches_current_site(state, rfssid, siteid) && state->trunk_lcn_freq[2] == 0
                   && sccf != state->trunk_lcn_freq[1]) {
            state->trunk_lcn_freq[2] = sccf;
            state->lcn_freq_count = 3;
        }
        (void)p25_announce_secondary_cc_channel(opts, state, (uint16_t)channelt, (uint8_t)rfssid, (uint8_t)siteid,
                                                (uint8_t)sysclass);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_54(p25p2_vpdu_ctx* ctx) {
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x30 && MAC[2 + len_a] != 0xA4 && MAC[2 + len_a] != 0x90) {
        // Exclude MFID messages (0xA4 = Harris, 0x90 = Moto) which use same opcode
        int ta = (MAC[2 + len_a] << 16) | (MAC[3 + len_a] << 8) | MAC[4 + len_a];
        int rf = (MAC[5 + len_a] >> 4) & 0xF;
        int ber = MAC[5 + len_a] & 0xF;
        DSD_FPRINTF(stderr, "\n Power Control Signal Quality");
        DSD_FPRINTF(stderr, "\n  Target Address: %d RF: 0x%X BER: 0x%X", ta, rf, ber);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_55(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x7F && MAC[len_a] == 0x07) {
        int algid = (int)MAC[4 + len_a];
        int kid = (int)((MAC[5 + len_a] << 8) | MAC[6 + len_a]);
        int target = (int)((MAC[7 + len_a] << 16) | (MAC[8 + len_a] << 8) | MAC[9 + len_a]);

        const char* alg_name = p25_algid_name((uint8_t)algid);

        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, "\n Protection Parameter Update");
        DSD_FPRINTF(stderr, "\n  ALGID [%02X]", algid);
        if (alg_name) {
            DSD_FPRINTF(stderr, " (%s)", alg_name);
        }
        DSD_FPRINTF(stderr, " KID [%04X]", kid);
        DSD_FPRINTF(stderr, " Target [%d]", target);
        DSD_FPRINTF(stderr, "%s", KNRM);

        state->p25_prot_algid = (uint8_t)algid;
        state->p25_prot_kid = (uint16_t)kid;
        state->p25_prot_valid = 1;
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static int
p25p2_vpdu_lto_offset_minutes(int valid, int sign, int magnitude) {
    if (!valid) {
        return 0;
    }
    return sign ? -magnitude : magnitude;
}

static void
p25p2_vpdu_iter_block_56(p25p2_vpdu_ctx* ctx) {
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x75) {
        int bridged_p1 = (MAC[len_a] == 0x07);
        int vd = (int)p25_mac_get_bits(MAC, len_a, 8, 1);
        int vt = (int)p25_mac_get_bits(MAC, len_a, 9, 1);
        int vl = (int)p25_mac_get_bits(MAC, len_a, 10, 1);

        int year = 0, month = 0, day = 0;
        int hours = 0, minutes = 0, seconds = 0;
        int lto_sign = 0, lto_mag = 0;

        if (vd) {
            month = (int)p25_mac_get_bits(MAC, len_a, 24, 4);
            day = (int)p25_mac_get_bits(MAC, len_a, 28, 5);
            year = (int)p25_mac_get_bits(MAC, len_a, 33, 13);
        }
        if (vt) {
            hours = (int)p25_mac_get_bits(MAC, len_a, 48, 5);
            minutes = (int)p25_mac_get_bits(MAC, len_a, 53, 6);
            seconds = (int)p25_mac_get_bits(MAC, len_a, 59, 6);
        }
        if (vl) {
            if (bridged_p1) {
                int raw_lto = (int)p25_mac_get_bits(MAC, len_a, 12, 12);
                lto_sign = (raw_lto >> 11) & 0x1;
                lto_mag = raw_lto & 0x7FF;
            } else {
                lto_sign = (int)p25_mac_get_bits(MAC, len_a, 11, 1);
                lto_mag = (int)p25_mac_get_bits(MAC, len_a, 12, 12);
            }
        }

        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, "\n Time and Date Announcement");
        DSD_FPRINTF(stderr, "\n  ");
        if (vd) {
            DSD_FPRINTF(stderr, "%04d-%02d-%02d ", year, month, day);
        }
        if (vt) {
            DSD_FPRINTF(stderr, "%02d:%02d:%02d ", hours, minutes, seconds);
        }
        if (vl) {
            int lto_total_minutes = lto_mag;
            int lto_hours = lto_total_minutes / 60;
            int lto_mins = lto_total_minutes % 60;
            DSD_FPRINTF(stderr, "UTC%c%02d:%02d", lto_sign ? '-' : '+', lto_hours, lto_mins);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);

        const int offset_minutes = p25p2_vpdu_lto_offset_minutes(vl, lto_sign, lto_mag);

        // Store UTC time_t if both date and time are valid.
        if (vd && vt) {
            time_t t = p25_utc_time_from_local_fields(year, month, day, hours, minutes, seconds, offset_minutes);
            if (t != (time_t)-1) {
                state->p25_sys_time = t;
                state->p25_sys_time_valid = 1;
            }
        }

        if (vl) {
            state->p25_sys_time_offset = (int16_t)offset_minutes;
            state->p25_sys_time_offset_valid = 1;
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_57(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    UNUSED4(type, mac_res, len_c, slot);

    if (MAC[1 + len_a] == 0x61 || MAC[1 + len_a] == 0x67) {
        int is_deny = (MAC[1 + len_a] == 0x67);
        int has_addl_info = ((MAC[2 + len_a] & 0x80) != 0);
        int svc_type = (int)(MAC[2 + len_a] & 0x3F);
        int reason_code = (int)MAC[3 + len_a];
        int addl_info = (int)((MAC[4 + len_a] << 16) | (MAC[5 + len_a] << 8) | MAC[6 + len_a]);
        int target_addr = (int)((MAC[7 + len_a] << 16) | (MAC[8 + len_a] << 8) | MAC[9 + len_a]);

        const char* reason_str =
            is_deny ? p25_deny_reason_str((uint8_t)reason_code) : p25_que_reason_str((uint8_t)reason_code);

        DSD_FPRINTF(stderr, "\n %s Response", is_deny ? "Deny" : "Queued");
        DSD_FPRINTF(stderr, "\n  SVC [%02X] Reason [%s]", svc_type, reason_str);
        if (has_addl_info) {
            DSD_FPRINTF(stderr, " Addl [%06X]", addl_info);
        }
        DSD_FPRINTF(stderr, " Target [%d]", target_addr);

        if (has_addl_info) {
            DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0],
                         "%s Target: %d Reason: %s Info: %06X; ", is_deny ? "DENY" : "QUEUED", target_addr, reason_str,
                         addl_info);
        } else {
            DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0], "%s Target: %d Reason: %s; ",
                         is_deny ? "DENY" : "QUEUED", target_addr, reason_str);
        }
        state->last_active_time = time(NULL);

        // Notify the trunking state machine
        if (is_deny) {
            p25_sm_on_deny_response(opts, state, svc_type, reason_code, target_addr);
        } else {
            p25_sm_on_queued_response(opts, state, svc_type, reason_code, target_addr);
        }
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_58(p25p2_vpdu_ctx* ctx) {
    const dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    int opcode = (int)MAC[1 + len_a];
    UNUSED4(type, mac_res, len_c, slot);

    if (opcode == 0x41) {
        int svc = (int)MAC[2 + len_a];
        int group = p25p2_vpdu_u16(MAC, 3 + len_a);
        int source = p25p2_vpdu_u24(MAC, 5 + len_a);

        DSD_FPRINTF(stderr, "\n Group Voice Service Request");
        DSD_FPRINTF(stderr, "\n  SVC [%02X] Group [%d][%04X] Source [%d]", svc, group, group, source);
        p25p2_vpdu_print_svc_no_state(opts, svc);
        p25p2_vpdu_print_group_label(state, (uint32_t)group);
        if (source != 0 && group != 0) {
            p25_ga_add(state, (uint32_t)source, (uint16_t)group);
        }
    }

    if (opcode == 0x45 || opcode == 0xC5) {
        int svc = (int)MAC[2 + len_a];
        int target = p25p2_vpdu_u24(MAC, 3 + len_a);

        DSD_FPRINTF(stderr, "\n Unit-to-Unit Answer Request");
        if (opcode == 0xC5) {
            int src_wacn = p25p2_vpdu_fqid_wacn(MAC, 6 + len_a);
            int src_sys = p25p2_vpdu_fqid_sysid(MAC, 6 + len_a);
            int source = p25p2_vpdu_u24(MAC, 10 + len_a);
            DSD_FPRINTF(stderr, " - Extended");
            DSD_FPRINTF(stderr, "\n  SVC [%02X] Target [%d] Source [%05X:%03X.%d]", svc, target, src_wacn, src_sys,
                        source);
        } else {
            int source = p25p2_vpdu_u24(MAC, 6 + len_a);
            DSD_FPRINTF(stderr, " - Abbreviated");
            DSD_FPRINTF(stderr, "\n  SVC [%02X] Target [%d] Source [%d]", svc, target, source);
        }
        p25p2_vpdu_print_svc_no_state(opts, svc);
    }

    if (opcode == 0x4A) {
        static const char hex[] = "0123456789ABCDEF";
        char digits[11];
        int d = 0;
        for (int b = 2; b <= 6; b++) {
            digits[d++] = hex[(MAC[b + len_a] >> 4) & 0x0F];
            digits[d++] = hex[MAC[b + len_a] & 0x0F];
        }
        digits[d] = '\0';

        int target = (int)((MAC[7 + len_a] >> 4) & 0x0F);
        DSD_FPRINTF(stderr, "\n Telephone Interconnect Answer Request");
        DSD_FPRINTF(stderr, "\n  Target [%d] Digits [%s]", target, digits);
    }

    if (opcode == 0x58) {
        int unit_status = (int)MAC[3 + len_a];
        int user_status = (int)MAC[4 + len_a];
        int target = p25p2_vpdu_u24(MAC, 5 + len_a);
        int source = p25p2_vpdu_u24(MAC, 8 + len_a);

        DSD_FPRINTF(stderr, "\n Status Update - Abbreviated");
        DSD_FPRINTF(stderr, "\n  Target [%d] Source [%d] Unit [%02X] User [%02X]", target, source, unit_status,
                    user_status);
        DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0],
                     "STATUS Target: %d Source: %d Unit: %02X User: %02X; ", target, source, unit_status, user_status);
        state->last_active_time = time(NULL);
    }

    if (opcode == 0x5A || opcode == 0x5F || opcode == 0x6A) {
        int target = p25p2_vpdu_u24(MAC, 2 + len_a);
        int source = p25p2_vpdu_u24(MAC, 5 + len_a);
        const char* label = (opcode == 0x5A)   ? "Status Query"
                            : (opcode == 0x5F) ? "Call Alert"
                                               : "Group Affiliation Query";

        DSD_FPRINTF(stderr, "\n %s - Abbreviated", label);
        DSD_FPRINTF(stderr, "\n  Target [%d] Source [%d]", target, source);
        DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0], "%s Target: %d Source: %d; ", label,
                     target, source);
        state->last_active_time = time(NULL);
    }

    if (opcode == 0x5C) {
        int message = p25p2_vpdu_u16(MAC, 3 + len_a);
        int target = p25p2_vpdu_u24(MAC, 5 + len_a);
        int source = p25p2_vpdu_u24(MAC, 8 + len_a);

        DSD_FPRINTF(stderr, "\n Message Update - Abbreviated");
        DSD_FPRINTF(stderr, "\n  Target [%d] Source [%d] Message [%04X]", target, source, message);
        DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0],
                     "MSG Target: %d Source: %d Message: %04X; ", target, source, message);
        state->last_active_time = time(NULL);
    }

    if (opcode == 0x60) {
        int has_addl_info = ((MAC[2 + len_a] & 0x80) != 0);
        int has_extended_addr = ((MAC[2 + len_a] & 0x40) != 0);
        int svc_type = (int)(MAC[2 + len_a] & 0x3F);
        int target = p25p2_vpdu_u24(MAC, 7 + len_a);

        DSD_FPRINTF(stderr, "\n Acknowledge Response FNE - Abbreviated");
        DSD_FPRINTF(stderr, "\n  Service [%02X] Target [%d]", svc_type, target);
        if (has_addl_info && has_extended_addr) {
            int target_wacn = (int)((MAC[3 + len_a] << 12) | (MAC[4 + len_a] << 4) | (MAC[5 + len_a] >> 4));
            int target_sys = (int)(((MAC[5 + len_a] & 0x0F) << 8) | MAC[6 + len_a]);
            DSD_FPRINTF(stderr, " FQTarget [%05X:%03X.%d]", target_wacn, target_sys, target);
        } else if (has_addl_info) {
            int source = p25p2_vpdu_u24(MAC, 4 + len_a);
            DSD_FPRINTF(stderr, " Source [%d]", source);
        }
        DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0], "ACK Target: %d Service: %02X; ",
                     target, svc_type);
        state->last_active_time = time(NULL);
    }

    if (opcode == 0x76) {
        int stack_op = (int)MAC[3 + len_a];
        int target_wacn = p25p2_vpdu_fqid_wacn(MAC, 4 + len_a);
        int target_sys = p25p2_vpdu_fqid_sysid(MAC, 4 + len_a);
        int target = p25p2_vpdu_u24(MAC, 8 + len_a);

        DSD_FPRINTF(stderr, "\n Roaming Address Command");
        DSD_FPRINTF(stderr, "\n  StackOp [%02X] Target [%05X:%03X.%d]", stack_op, target_wacn, target_sys, target);
    }

    if (opcode == 0x77) {
        int last = ((MAC[3 + len_a] & 0x80) != 0);
        int sequence = (int)(MAC[3 + len_a] & 0x0F);
        int target = p25p2_vpdu_u24(MAC, 4 + len_a);
        int source_wacn = p25p2_vpdu_fqid_wacn(MAC, 7 + len_a);
        int source_sys = p25p2_vpdu_fqid_sysid(MAC, 7 + len_a);
        int source = p25p2_vpdu_u24(MAC, 11 + len_a);

        DSD_FPRINTF(stderr, "\n Roaming Address Update");
        DSD_FPRINTF(stderr, "\n  Target [%d] Source [%05X:%03X.%d] Seq [%d]%s", target, source_wacn, source_sys, source,
                    sequence, last ? " Last" : "");
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_iter_block_59(p25p2_vpdu_ctx* ctx) {
    dsd_opts* opts VPDU_MAYBE_UNUSED = ctx->opts;
    dsd_state* state VPDU_MAYBE_UNUSED = ctx->state;
    int type = ctx->type;
    const unsigned long long int* MAC = ctx->mac;
    struct p25p2_mac_result mac_res VPDU_MAYBE_UNUSED = *ctx->mac_res;
    int len_a VPDU_MAYBE_UNUSED = ctx->len_a;
    int len_b = ctx->len_b;
    int len_c VPDU_MAYBE_UNUSED = ctx->len_c;
    int slot VPDU_MAYBE_UNUSED = ctx->slot;
    int i = ctx->iter_idx;
    int opcode = (int)MAC[1 + len_a];
    int mfid = (int)MAC[2 + len_a];
    UNUSED4(type, len_c, slot, i);

    if (mfid == 0x90 && (opcode == 0xA6 || opcode == 0xA7)) {
        int is_deny = (opcode == 0xA7);
        int has_addl_info = ((MAC[4 + len_a] & 0x80) != 0);
        int svc_type = (int)(MAC[4 + len_a] & 0x3F);
        int reason_code = (int)MAC[5 + len_a];
        int addl_info = p25p2_vpdu_u24(MAC, 6 + len_a);
        int target_addr = p25p2_vpdu_u24(MAC, 9 + len_a);
        const char* reason_str =
            is_deny ? p25_deny_reason_str((uint8_t)reason_code) : p25_que_reason_str((uint8_t)reason_code);

        DSD_FPRINTF(stderr, "\n Motorola %s Response", is_deny ? "Deny" : "Queued");
        DSD_FPRINTF(stderr, "\n  SVC [%02X] Reason [%s]", svc_type, reason_str);
        if (has_addl_info) {
            DSD_FPRINTF(stderr, " Addl [%06X]", addl_info);
        }
        DSD_FPRINTF(stderr, " Target [%d]", target_addr);

        if (has_addl_info) {
            DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0],
                         "MOT %s Target: %d Reason: %s Info: %06X; ", is_deny ? "DENY" : "QUEUED", target_addr,
                         reason_str, addl_info);
        } else {
            DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0], "MOT %s Target: %d Reason: %s; ",
                         is_deny ? "DENY" : "QUEUED", target_addr, reason_str);
        }
        state->last_active_time = time(NULL);

        if (is_deny) {
            p25_sm_on_deny_response(opts, state, svc_type, reason_code, target_addr);
        } else {
            p25_sm_on_queued_response(opts, state, svc_type, reason_code, target_addr);
        }
    }

    if (mfid == 0x90 && opcode == 0xA8) {
        int svc_type = (int)(MAC[4 + len_a] & 0x3F);
        int source = p25p2_vpdu_u24(MAC, 5 + len_a);
        int target = p25p2_vpdu_u24(MAC, 8 + len_a);

        DSD_FPRINTF(stderr, "\n Motorola Acknowledge Response");
        DSD_FPRINTF(stderr, "\n  Service [%02X] Source [%d] Target [%d]", svc_type, source, target);
        DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0],
                     "MOT ACK Target: %d Source: %d Service: %02X; ", target, source, svc_type);
        state->last_active_time = time(NULL);
    }

    if (mfid == 0x90 && opcode == 0x84) {
        int function = p25p2_vpdu_u16(MAC, 4 + len_a);
        int class_id = (function >> 8) & 0xFF;
        int operand = function & 0xFF;
        int argument = p25p2_vpdu_u24(MAC, 6 + len_a);
        int target = p25p2_vpdu_u24(MAC, 9 + len_a);

        DSD_FPRINTF(stderr, "\n Motorola Group Regroup Extended Function Command");
        DSD_FPRINTF(stderr, "\n  Class [%02X] Operand [%02X] Arg [%06X] Target [%d]", class_id, operand, argument,
                    target);
        if (class_id == 0) {
            DSD_FPRINTF(stderr, " %s", p25_extended_function_class0_operand_label((uint8_t)operand));
        }
    }

    if (mfid == 0x90 && opcode == 0x8B) {
        DSD_FPRINTF(stderr, "\n Motorola TDMA Data Channel");
        int printed = 0;
        const int channel_offsets[] = {5, 8, 11, 14};
        for (int c = 0; c < (int)(sizeof(channel_offsets) / sizeof(channel_offsets[0])); c++) {
            int channel = p25p2_vpdu_u16(MAC, channel_offsets[c] + len_a);
            if (!p25p2_vpdu_channel_is_valid(channel)) {
                continue;
            }
            long int freq = process_channel_to_freq(opts, state, channel);
            DSD_FPRINTF(stderr, "%s CH%d [%04X]", printed ? "" : "\n ", c + 1, channel);
            if (freq > 0) {
                DSD_FPRINTF(stderr, " [%09ld]", freq);
            }
            printed = 1;
        }
        if (!printed) {
            DSD_FPRINTF(stderr, " Not Active");
        }
    }

    if (mfid == 0xA4 && (opcode == 0xA0 || opcode == 0xAC)) {
        int channel = p25p2_vpdu_u16(MAC, 5 + len_a);
        int target = p25p2_vpdu_u24(MAC, 7 + len_a);
        int source = (opcode == 0xAC) ? p25p2_vpdu_u24(MAC, 10 + len_a) : 0;
        long int freq = process_channel_to_freq(opts, state, channel);
        char suffix[32];

        DSD_FPRINTF(stderr, "\n L3Harris %s Data Channel Grant", opcode == 0xAC ? "Unit-to-Unit" : "Private");
        DSD_FPRINTF(stderr, "\n  CHAN [%04X] Target [%d]", channel, target);
        if (source != 0) {
            DSD_FPRINTF(stderr, " Source [%d]", source);
        }

        p25_format_chan_suffix(state, (uint16_t)channel, -1, suffix, sizeof suffix);
        if (source != 0) {
            DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0],
                         "Harris Data Ch: %04X%s TGT: %d SRC: %d; ", channel, suffix, target, source);
        } else {
            DSD_SNPRINTF(state->active_channel[0], sizeof state->active_channel[0], "Harris Data Ch: %04X%s TGT: %d; ",
                         channel, suffix, target);
        }
        state->last_active_time = time(NULL);

        if (p25p2_vpdu_can_tune(opts, state, freq)) {
            int policy_encrypted = (opts->trunk_tune_enc_calls == 0) ? 1 : 0;
            p25p2_mac_handle_indiv(&mac_res, opts, state, channel, P25_SM_SVC_UNKNOWN, target, source, policy_encrypted,
                                   /*policy_data*/ 1);
        }
        p25p2_vpdu_update_playback_if_match(opts, state, target, freq);
    }

    if (len_b < 0) {
        goto BLOCK_END;
    }
BLOCK_END:
    VPDU_LABEL_UNUSED;

    ctx->len_b = len_b;
    ctx->iter_idx = i;
}

static void
p25p2_vpdu_dispatch_blocks(p25p2_vpdu_ctx* ctx) {
    typedef void (*p25p2_vpdu_handler_fn)(p25p2_vpdu_ctx*);
    static const p25p2_vpdu_handler_fn handlers[] = {
        p25p2_vpdu_iter_block_01, p25p2_vpdu_iter_block_02, p25p2_vpdu_iter_block_03, p25p2_vpdu_iter_block_04,
        p25p2_vpdu_iter_block_05, p25p2_vpdu_iter_block_06, p25p2_vpdu_iter_block_07, p25p2_vpdu_iter_block_08,
        p25p2_vpdu_iter_block_09, p25p2_vpdu_iter_block_10, p25p2_vpdu_iter_block_11, p25p2_vpdu_iter_block_12,
        p25p2_vpdu_iter_block_13, p25p2_vpdu_iter_block_14, p25p2_vpdu_iter_block_15, p25p2_vpdu_iter_block_16,
        p25p2_vpdu_iter_block_17, p25p2_vpdu_iter_block_19, p25p2_vpdu_iter_block_20, p25p2_vpdu_iter_block_21,
        p25p2_vpdu_iter_block_22, p25p2_vpdu_iter_block_23, p25p2_vpdu_iter_block_24, p25p2_vpdu_iter_block_25,
        p25p2_vpdu_iter_block_26, p25p2_vpdu_iter_block_27, p25p2_vpdu_iter_block_28, p25p2_vpdu_iter_block_29,
        p25p2_vpdu_iter_block_30, p25p2_vpdu_iter_block_31, p25p2_vpdu_iter_block_32, p25p2_vpdu_iter_block_33,
        p25p2_vpdu_iter_block_34, p25p2_vpdu_iter_block_35, p25p2_vpdu_iter_block_36, p25p2_vpdu_iter_block_37,
        p25p2_vpdu_iter_block_38, p25p2_vpdu_iter_block_39, p25p2_vpdu_iter_block_40, p25p2_vpdu_iter_block_41,
        p25p2_vpdu_iter_block_42, p25p2_vpdu_iter_block_43, p25p2_vpdu_iter_block_44, p25p2_vpdu_iter_block_45,
        p25p2_vpdu_iter_block_46, p25p2_vpdu_iter_block_47, p25p2_vpdu_iter_block_48, p25p2_vpdu_iter_block_49,
        p25p2_vpdu_iter_block_50, p25p2_vpdu_iter_block_51, p25p2_vpdu_iter_block_52, p25p2_vpdu_iter_block_53,
        p25p2_vpdu_iter_block_54, p25p2_vpdu_iter_block_55, p25p2_vpdu_iter_block_56, p25p2_vpdu_iter_block_57,
        p25p2_vpdu_iter_block_58, p25p2_vpdu_iter_block_59,
    };
    const size_t handler_count = sizeof(handlers) / sizeof(handlers[0]);
    for (size_t h = 0; h < handler_count; h++) {
        if (ctx->end_pdu || ctx->skip_rest) {
            return;
        }
        handlers[h](ctx);
    }
}

static int
p25p2_vpdu_advance_segment(p25p2_vpdu_ctx* ctx) {
    if ((ctx->len_b + ctx->len_c) < 24 && ctx->len_c != 0) {
        ctx->len_a = ctx->len_b;
        return 1;
    }
    return 0;
}

void
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int mac[24]) {
    unsigned long long int mac_octets[24] = {0};
    for (int bi = 0; bi < 24; bi++) {
        mac_octets[bi] = mac[bi] & 0xFFu;
    }
    const unsigned long long int* MAC = mac_octets;
    //handle variable content MAC PDUs (Active, Idle, Hangtime, or Signal)
    //use type to specify SACCH or FACCH, so we know if we should invert the currentslot when assigning ids etc

    //b values - 0 = Unique TDMA Message,  1 Phase 1 OSP/ISP abbreviated
    // 2 = Manufacturer Message, 3 Phase 1 OSP/ISP extended/explicit

    struct p25p2_mac_result mac_res;
    if (p25p2_mac_parse(type, MAC, &mac_res) != 0) {
        return;
    }

    p25p2_vpdu_ctx ctx = {
        .opts = opts,
        .state = state,
        .type = type,
        .mac = mac_octets,
        .mac_res = &mac_res,
        .len_a = mac_res.len_a,
        .len_b = mac_res.len_b,
        .len_c = mac_res.len_c,
        .slot = (type == 1) ? ((state->currentslot ^ 1) & 1) : state->currentslot,
        .skip_rest = 0,
        .end_pdu = 0,
        .iter_idx = 0,
    };

    p25p2_vpdu_emit_json(&ctx);
    p25p2_vpdu_lcch_signal_update(&ctx);
    if (!p25p2_vpdu_validate_len_and_warn(&ctx)) {
        ctx.end_pdu = 1;
    }

    for (ctx.iter_idx = 0; !ctx.end_pdu && ctx.iter_idx < 2; ctx.iter_idx++) {
        ctx.skip_rest = 0;
        p25p2_vpdu_dispatch_blocks(&ctx);
        if (ctx.end_pdu) {
            break;
        }
        if (!p25p2_vpdu_advance_segment(&ctx)) {
            break;
        }
    }

    state->p2_is_lcch = 0;
    //debug printing
    if (opts->payload == 1 && MAC[1] != 0) //print only if not a null type //&& MAC[1] != 0 //&& MAC[2] != 0
    {
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, "\n P25 PDU Payload\n  ");
        for (int bi = 0; bi < 24; bi++) {
            DSD_FPRINTF(stderr, "[%02llX]", MAC[bi]);
            if (bi == 11) {
                DSD_FPRINTF(stderr, "\n  ");
            }
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

// Local bounded append helper (reused pattern across modules)
static inline void
dsd_append(char* dst, size_t dstsz, const char* src) {
    if (!dst || !src || dstsz == 0) {
        return;
    }
    size_t len = strlen(dst);
    if (len >= dstsz) {
        return;
    }
    DSD_SNPRINTF(dst + len, dstsz - len, "%s", src);
}
