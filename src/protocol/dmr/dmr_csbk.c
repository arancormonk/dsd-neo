// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * dmr_csbk.c
 * DMR Control Signal Data PDU (CSBK, MBC) Handler and Related Functions
 *
 * Portions of Connect+ code reworked from Boatbod OP25
 * Source: https://github.com/boatbod/op25/blob/master/op25/gr-op25_repeater/lib/dmr_slot.cc
 *
 * Portions of Capacity+ code reworked from Eric Cottrell
 * Source: https://github.com/LinuxSheeple-E/dsd/blob/Feature/DMRECC/dmr_csbk.c
 *
 * LWVMOBILE
 * 2023-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_csbk_parse.h>
#include <dsd-neo/protocol/dmr/dmr_csbk_tables.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define PCLEAR_TUNE_AWAY //disable if slower return is preferred

static void
dmr_csbk_print_group_label(const dsd_state* state, uint32_t id) {
    char name[50];
    if (id != 0U && dsd_tg_policy_lookup_label(state, id, NULL, 0, name, sizeof(name))) {
        DSD_FPRINTF(stderr, " [%s]", name);
    }
}

// Safe append helper: appends src to dst within dstsz, NUL-terminating
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

// Format a short suffix indicating TDMA slot for DMR UI, e.g., " (TDMA S1)".
static inline void
dmr_format_chan_suffix(int slot_index, char* out, size_t outsz) {
    if (!out || outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (slot_index < 0) {
        return;
    }
    // Display slots as 1-based (S1/S2) to match UI conventions
    DSD_SNPRINTF(out, outsz, " (TDMA S%d)", (slot_index % 2) + 1);
}

static int
dmr_policy_tune_allowed(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                        int is_group_call, int data_call, dsd_tg_policy_decision* out_decision) {
    dsd_tg_policy_decision decision;
    int rc = 0;
    DSD_MEMSET(&decision, 0, sizeof(decision));

    if (is_group_call) {
        rc = dsd_tg_policy_evaluate_group_call(opts, state, target, source, 0, data_call,
                                               DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision);
    } else {
        rc = dsd_tg_policy_evaluate_private_call(opts, state, source, target, 0, data_call,
                                                 DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                 DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision);
    }
    if (out_decision) {
        *out_decision = decision;
    }
    return rc == 0 && decision.tune_allowed;
}

static const char*
dmr_policy_block_reason_label(uint32_t block_reasons) {
    if (block_reasons & DSD_TG_POLICY_BLOCK_HOLD) {
        return "hold";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED) {
        return "private-disabled";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_GROUP_DISABLED) {
        return "group-disabled";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_DATA_DISABLED) {
        return "data-disabled";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED) {
        return "enc-disabled";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) {
        return "allowlist";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_MODE) {
        return "mode";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_AUDIO) {
        return "audio";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_RECORD) {
        return "record";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_STREAM) {
        return "stream";
    }
    return "policy";
}

static void
dmr_policy_log_block(const dsd_opts* opts, int is_group_call, uint32_t target, uint32_t source,
                     const dsd_tg_policy_decision* decision) {
    if (!opts || !decision || opts->verbose < 1) {
        return;
    }
    if (decision->block_reasons == DSD_TG_POLICY_BLOCK_NONE) {
        return;
    }
    DSD_FPRINTF(stderr, "\n DMR %s grant blocked (%s): target=%u source=%u;", is_group_call ? "group" : "private",
                dmr_policy_block_reason_label(decision->block_reasons), target, source);
}

static void dmr_gateway_identifier(uint32_t source, uint32_t target);
static void dmr_decode_syscode(dsd_opts* opts, dsd_state* state, uint8_t* cs_pdu_bits, int csbk_fid, int type);

enum { DMR_T3_MAX_LCN = 4094 };

typedef struct {
    int first_lcn;
    int last_lcn;
    long first_freq;
    long long sum_df;
    long long sum_dl;
    int anchors;
} dmr_heuristic_anchor_stats;

static void
dmr_heuristic_collect_anchor_stats(const dsd_state* state, dmr_heuristic_anchor_stats* stats) {
    int prev_lcn = 0;
    long prev_freq = 0;
    int have_prev = 0;

    if (!state || !stats) {
        return;
    }

    DSD_MEMSET(stats, 0, sizeof(*stats));
    for (int l = 1; l <= DMR_T3_MAX_LCN; l++) {
        long f = state->trunk_chan_map[l];
        if (f == 0) {
            continue;
        }
        if (stats->first_lcn == 0) {
            stats->first_lcn = l;
            stats->first_freq = f;
        }
        stats->last_lcn = l;
        stats->anchors++;
        if (have_prev) {
            int dl = l - prev_lcn;
            if (dl > 0) {
                long df = f - prev_freq;
                stats->sum_dl += dl;
                stats->sum_df += df;
            }
        }
        prev_lcn = l;
        prev_freq = f;
        have_prev = 1;
    }
}

static long
dmr_heuristic_estimate_step(const dmr_heuristic_anchor_stats* stats) {
    if (!stats || stats->anchors < 2 || stats->sum_dl <= 0) {
        return 0;
    }
    double slope = (double)stats->sum_df / (double)stats->sum_dl;
    long step = (long)llround(slope / 125.0) * 125L;
    return (step > 0) ? step : 0;
}

static int
dmr_heuristic_validate_model(const dsd_state* state, const dmr_heuristic_anchor_stats* stats, long step) {
    long max_err = 0;

    if (!state || !stats || step <= 0) {
        return 0;
    }
    for (int l = 1; l <= DMR_T3_MAX_LCN; l++) {
        long f = state->trunk_chan_map[l];
        if (f == 0) {
            continue;
        }
        long model = stats->first_freq + (long)(l - stats->first_lcn) * step;
        long err = labs(f - model);
        if (err > max_err) {
            max_err = err;
        }
    }
    return max_err <= 500;
}

static int
dmr_heuristic_fill_gaps(dsd_state* state, const dmr_heuristic_anchor_stats* stats, long step) {
    int filled = 0;

    if (!state || !stats || step <= 0) {
        return 0;
    }
    for (int l = stats->first_lcn; l <= stats->last_lcn; l++) {
        if (state->trunk_chan_map[l] != 0) {
            continue;
        }
        long f = stats->first_freq + (long)(l - stats->first_lcn) * step;
        if (f <= 0) {
            continue;
        }
        dsd_state_set_trunk_chan_freq(state, (uint32_t)l, f);
        filled++;
    }
    return filled;
}

static void
dmr_heuristic_report_fill(dsd_opts* opts, dsd_state* state, const dmr_heuristic_anchor_stats* stats, long step,
                          int filled) {
    char msg[160];
    double mhz0;
    double step_khz;
    int prev_alert;

    if (!opts || !state || !stats || filled <= 0) {
        return;
    }

    mhz0 = (double)stats->first_freq / 1000000.0;
    step_khz = (double)step / 1000.0;
    DSD_SNPRINTF(msg, sizeof(msg), "DMR TIII: Heuristic filled %d LCNs (%0.3f kHz step) from %04d@%0.6f MHz;", filled,
                 step_khz, stats->first_lcn, mhz0);
    prev_alert = opts->call_alert;
    opts->call_alert = 0;
    watchdog_event_datacall(opts, state, 0xFFFFFF, 0xFFFFFF, msg, 0);
    opts->call_alert = prev_alert;
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
}

// Attempt to fill missing LCNs heuristically from learned anchors.
static void
dmr_try_heuristic_fill(dsd_opts* opts, dsd_state* state) {
    dmr_heuristic_anchor_stats stats;
    long step;
    int filled;

    if (!opts || !state) {
        return;
    }
    if (!opts->dmr_t3_heuristic_fill) {
        return; // opt-in only
    }

    dmr_heuristic_collect_anchor_stats(state, &stats);
    step = dmr_heuristic_estimate_step(&stats);
    if (step <= 0) {
        return;
    }
    if (!dmr_heuristic_validate_model(state, &stats, step)) {
        return;
    }
    filled = dmr_heuristic_fill_gaps(state, &stats, step);
    dmr_heuristic_report_fill(opts, state, &stats, step, filled);
}

// Conservative auto-learn helper: record LCN -> frequency if empty and announce in event history
static inline void
dmr_learn_chan_map(dsd_opts* opts, dsd_state* state, uint16_t lpcn, long int freq) {
    if (!state) {
        return;
    }
    // DMR Tier III logical/physical channel numbers are 12-bit (1..4094 typical)
    if (lpcn == 0 || lpcn >= 0xFFFF) {
        return;
    }
    if (freq <= 0) {
        return;
    }
    if (state->trunk_chan_map[lpcn] != 0) {
        return;
    }

    dsd_state_set_trunk_chan_freq(state, lpcn, freq);
    // Mark provenance: trusted if learned while on CC for current site, else unconfirmed
    if (lpcn < 0x1000) {
        uint8_t trust = 1;
        if (state->trunk_cc_freq != 0 && opts && opts->trunk_is_tuned == 0) {
            trust = 2;
        }
        state->dmr_lcn_trust[lpcn] = trust;
    }

    // Emit a one-line event so users can see learning progress in ncurses
    if (opts) {
        char msg[160];
        // Print in MHz with 6 decimal places
        double mhz = (double)freq / 1000000.0;
        DSD_SNPRINTF(msg, sizeof(msg), "DMR TIII: Learned LCN %04u -> %010.6f MHz;", lpcn, mhz);
        int prev_alert = opts->call_alert;
        opts->call_alert = 0; // suppress beeper for system-status events
        watchdog_event_datacall(opts, state, 0xFFFFFF, 0xFFFFFF, msg, 0);
        opts->call_alert = prev_alert;
        watchdog_event_history(opts, state, 0);
        watchdog_event_current(opts, state, 0);
    }

    // Try heuristic gap fill after learning a new anchor
    dmr_try_heuristic_fill(opts, state);
}

static int
dmr_cspdu_pf0_is_data_grant_opcode(int csbk_o) {
    return csbk_o == 51 || csbk_o == 52 || csbk_o == 54 || csbk_o == 55 || csbk_o == 56;
}

static int
dmr_cspdu_pf0_should_skip_call(const dsd_opts* opts, int* csbk_o, int* data_call) {
    if (opts->trunk_tune_group_calls == 0 && (*csbk_o == 49 || *csbk_o == 50)) {
        return 1;
    }

    if (*csbk_o == 50) {
        *csbk_o = 49;
    }

    *data_call = dmr_cspdu_pf0_is_data_grant_opcode(*csbk_o);
    if (*data_call && opts->trunk_tune_data_calls == 1) {
        *csbk_o = 49;
    }

    return (opts->trunk_tune_private_calls == 0 && *csbk_o != 49) ? 1 : 0;
}

static void
dmr_cspdu_pf0_update_slot_call_string(dsd_state* state, int slot, int csbk_o, int data_call, int emergency) {
    DSD_SPRINTF(state->call_string[slot], " Trunked ");
    if (csbk_o == 49 || csbk_o == 50) {
        DSD_SPRINTF(state->call_string[slot], "   Group ");
    } else if (!data_call) {
        DSD_SPRINTF(state->call_string[slot], " Private ");
    }
    if (emergency && !data_call) {
        dsd_append(state->call_string[slot], sizeof state->call_string[slot], " Emergency  ");
    } else {
        dsd_append(state->call_string[slot], sizeof state->call_string[slot], "            ");
    }
}

static void
dmr_cspdu_pf0_update_slot_grant_state(dsd_state* state, int slot, uint32_t target, uint32_t source, int csbk_o,
                                      int data_call, int emergency) {
    if (slot == 0) {
        state->lasttg = target;
        state->lastsrc = source;
    } else if (slot == 1) {
        state->lasttgR = target;
        state->lastsrcR = source;
    } else {
        return;
    }
    dmr_cspdu_pf0_update_slot_call_string(state, slot, csbk_o, data_call, emergency);
}

static uint16_t
dmr_cspdu_pf0_parse_absolute_grant(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], long int* freq) {
    uint8_t mbc_cdeftype = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[112], 4);
    unsigned long long int mbc_cdefparms = (unsigned long long int)ConvertBitIntoBytes(&cs_pdu_bits[118], 58);
    if (mbc_cdeftype != 0) {
        DSD_FPRINTF(stderr, "\n  MBC Channel Grant - Unknown Parms: %015llX", mbc_cdefparms);
        return 0;
    }

    uint16_t mbc_lpchannum = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[118], 12);
    uint16_t mbc_abs_rx_int = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[153], 10);
    uint16_t mbc_abs_rx_step = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[163], 13);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "  RX APCN: %04d; RX INT: %d; RX STEP: %d;", mbc_lpchannum, mbc_abs_rx_int, mbc_abs_rx_step);
    *freq = (mbc_abs_rx_int * 1000000L) + (mbc_abs_rx_step * 125L);
    dmr_learn_chan_map(opts, state, mbc_lpchannum, *freq);
    return mbc_lpchannum;
}

static void
dmr_cspdu_pf0_set_active_channel(dsd_state* state, uint8_t lcn, uint16_t channel, uint32_t target) {
    char suf[24];
    dmr_format_chan_suffix(lcn, suf, sizeof suf);
    DSD_SPRINTF(state->active_channel[lcn], "Active Ch: %04X%s TG: %d; ", channel, suf, target);
}

static void
dmr_cspdu_pf0_print_channel_kind(uint16_t lpchannum) {
    if (lpchannum == 0) {
        DSD_FPRINTF(stderr, " - Invalid Channel");
    } else if (lpchannum == 0xFFF) {
        DSD_FPRINTF(stderr, " - Absolute");
    } else {
        DSD_FPRINTF(stderr, " - Logical");
    }
}

static uint16_t
dmr_cspdu_pf0_resolve_frequency(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint16_t lpchannum,
                                long int* freq) {
    if (lpchannum == 0xFFF) {
        return dmr_cspdu_pf0_parse_absolute_grant(opts, state, cs_pdu_bits, freq);
    }
    if (lpchannum != 0) {
        *freq = state->trunk_chan_map[lpchannum];
    }
    return 0;
}

static void
dmr_cspdu_pf0_print_frequency(uint16_t lpchannum, long int freq) {
    if (freq != 0) {
        DSD_FPRINTF(stderr, "\n  Frequency: %.6lf MHz", (double)freq / 1000000.0);
    } else if (lpchannum != 0 && lpchannum != 0xFFF) {
        DSD_FPRINTF(stderr, "\n  Frequency Not Found in Channel Map;");
    }
}

static void
dmr_cspdu_pf0_update_active_channels(dsd_state* state, uint8_t lcn, uint16_t lpchannum, uint16_t mbc_lpchannum,
                                     uint32_t target) {
    if (lpchannum != 0 && lpchannum != 0xFFF) {
        dmr_cspdu_pf0_set_active_channel(state, lcn, lpchannum, target);
    } else if (lpchannum == 0xFFF) {
        dmr_cspdu_pf0_set_active_channel(state, lcn, mbc_lpchannum, target);
    }
}

static int
dmr_cspdu_pf0_prepare_dispatch(const dsd_opts* opts, dsd_state* state, int* csbk_o, int* data_call, long int freq,
                               uint32_t target) {
    if (dmr_cspdu_pf0_should_skip_call(opts, csbk_o, data_call)) {
        return 0;
    }
    if (!(*csbk_o == 48 || *csbk_o == 49 || *csbk_o == 50 || *csbk_o == 53)) {
        return 0;
    }
    if (state->tg_hold != 0 && state->tg_hold == target) {
        state->last_vc_sync_time = 0;
        state->last_vc_sync_time_m = 0.0;
    }
    if (opts->trunk_enable == 0 && freq != 0) {
        state->trunk_vc_freq[0] = freq;
        state->trunk_vc_freq[1] = freq;
    }
    return 1;
}

static void
dmr_cspdu_pf0_try_dispatch_grant(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_o,
                                 int data_call, uint8_t lcn, int emergency, uint16_t lpchannum, long int freq,
                                 uint32_t target, uint32_t source) {
    if (state->trunk_cc_freq == 0 || opts->trunk_enable != 1 || freq == 0) {
        return;
    }

    const int is_group_call = (csbk_o == 49 || csbk_o == 50) ? 1 : 0;
    dsd_tg_policy_decision policy_decision;
    int policy_allowed =
        dmr_policy_tune_allowed(opts, state, target, source, is_group_call, data_call, &policy_decision);
    if (!policy_allowed) {
        dmr_policy_log_block(opts, is_group_call, target, source, &policy_decision);
        return;
    }

    dmr_csbk_print_group_label(state, target);
    if (lcn == 0 && data_call == 0) {
        dmr_cspdu_pf0_update_slot_grant_state(state, 0, target, source, csbk_o, data_call, emergency);
    }
    if (lcn == 1 && data_call == 0) {
        dmr_cspdu_pf0_update_slot_grant_state(state, 1, target, source, csbk_o, data_call, emergency);
    }

    struct dmr_csbk_result res;
    if (dmr_csbk_parse(cs_pdu_bits, cs_pdu, &res) != 0) {
        return;
    }
    res.freq_hz = freq;
    res.lpcn = lpchannum;
    res.target = target;
    res.source = source;
    dmr_csbk_handle(&res, opts, state);
}

static void
dmr_cspdu_pf0_handle_grants(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_o) {
    if (csbk_o < 48 || csbk_o > 56) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    long int freq = 0;
    if (!(csbk_o == 56 && state->synctype == DSD_SYNC_DMR_MS_DATA)) {
        DSD_FPRINTF(stderr, " %s", dmr_csbk_grant_opcode_name((uint8_t)csbk_o));
    }

    uint16_t lpchannum = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 12);
    dmr_cspdu_pf0_print_channel_kind(lpchannum);

    uint16_t pluschannum = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 13) + 1;
    uint8_t lcn = cs_pdu_bits[28];
    uint8_t st2 = cs_pdu_bits[30];
    uint32_t target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[32], 24);
    uint32_t source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "  LPCN: %04d; TS: %d; LPCN+TS: %04d; Target: %08d - Source: %08d ", lpchannum, lcn + 1,
                pluschannum, target, source);
    if (st2) {
        DSD_FPRINTF(stderr, "Emergency; ");
    }
    dmr_gateway_identifier(source, target);

    uint16_t mbc_lpchannum = dmr_cspdu_pf0_resolve_frequency(opts, state, cs_pdu_bits, lpchannum, &freq);
    dmr_cspdu_pf0_print_frequency(lpchannum, freq);

    dmr_cspdu_pf0_update_active_channels(state, lcn, lpchannum, mbc_lpchannum, target);
    state->last_active_time = time(NULL);

    int data_call = 0;
    if (!dmr_cspdu_pf0_prepare_dispatch(opts, state, &csbk_o, &data_call, freq, target)) {
        return;
    }

    dmr_cspdu_pf0_try_dispatch_grant(opts, state, cs_pdu_bits, cs_pdu, csbk_o, data_call, lcn, st2, lpchannum, freq,
                                     target, source);
}

static void
dmr_cspdu_pf0_move_resolve_freq(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint16_t* move_lpcn,
                                long int* move_freq) {
    if (*move_lpcn == 0xFFF) {
        uint8_t mbc_cdeftype = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[112], 4);
        if (mbc_cdeftype == 0) {
            uint16_t mbc_lpchannum = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[118], 12);
            uint16_t mbc_abs_rx_int = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[153], 10);
            uint16_t mbc_abs_rx_step = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[163], 13);
            *move_lpcn = mbc_lpchannum;
            *move_freq = (mbc_abs_rx_int * 1000000L) + (mbc_abs_rx_step * 125L);
            dmr_learn_chan_map(opts, state, *move_lpcn, *move_freq);
        }
        return;
    }
    if (*move_lpcn != 0) {
        *move_freq = state->trunk_chan_map[*move_lpcn];
    }
}

static void
dmr_cspdu_pf0_move_update_slot_state(dsd_state* state, int tslot, uint32_t mv_target, uint32_t mv_source) {
    if (mv_target == 0) {
        return;
    }
    if (tslot == 0) {
        state->lasttg = mv_target;
        state->lastsrc = mv_source;
        if (state->gi[0] == 0) {
            DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], "   Group  Move      ");
        } else if (state->gi[0] == 1) {
            DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], " Private  Move      ");
        } else {
            DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], " Trunked  Move      ");
        }
        return;
    }

    state->lasttgR = mv_target;
    state->lastsrcR = mv_source;
    if (state->gi[1] == 0) {
        DSD_SNPRINTF(state->call_string[1], sizeof state->call_string[1], "   Group  Move      ");
    } else if (state->gi[1] == 1) {
        DSD_SNPRINTF(state->call_string[1], sizeof state->call_string[1], " Private  Move      ");
    } else {
        DSD_SNPRINTF(state->call_string[1], sizeof state->call_string[1], " Trunked  Move      ");
    }
}

static void
dmr_cspdu_pf0_move_debounce_slot(dsd_state* state, int tslot) {
    if (tslot == 0) {
        state->dmrburstL = 16;
        state->dmrburstR = 9;
        state->active_channel[1][0] = '\0';
        state->call_string[1][0] = '\0';
        return;
    }

    state->dmrburstR = 16;
    state->dmrburstL = 9;
    state->active_channel[0][0] = '\0';
    state->call_string[0][0] = '\0';
}

static void
dmr_cspdu_pf0_handle_move(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    if (csbk_o != 57 || cs_pdu_bits == NULL) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Move (C_MOVE) ");

    long int move_freq = 0;
    uint16_t move_lpcn = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 12);
    uint8_t move_ts = cs_pdu_bits[28];
    uint32_t mv_target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[32], 24);
    uint32_t mv_source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);
    int tslot = (int)(move_ts & 1);
    char suf[24];

    UNUSED(move_ts);
    dmr_cspdu_pf0_move_resolve_freq(opts, state, cs_pdu_bits, &move_lpcn, &move_freq);
    dmr_cspdu_pf0_move_update_slot_state(state, tslot, mv_target, mv_source);

    dmr_format_chan_suffix(tslot, suf, sizeof suf);
    if (move_lpcn != 0) {
        DSD_SNPRINTF(state->active_channel[tslot], sizeof state->active_channel[tslot], "Active Ch: %04X%s TG: %u; ",
                     move_lpcn, suf, mv_target);
        state->last_active_time = time(NULL);
    }

    dmr_cspdu_pf0_move_debounce_slot(state, tslot);

    if (opts->trunk_enable == 1 && state->trunk_cc_freq != 0 && opts->trunk_is_tuned == 1
        && (move_freq > 0 || (move_lpcn > 0 && move_lpcn < 0xFFFF))) {
        dmr_sm_emit_group_grant(opts, state, move_freq, move_lpcn, mv_target, mv_source);
    }
}

static void
dmr_cspdu_pf0_handle_aloha(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if (csbk_o != 25) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    dmr_decode_syscode(opts, state, cs_pdu_bits, csbk_fid, 0);

    if (opts->use_rigctl == 1 && opts->trunk_is_tuned == 0) {
        long int ccfreq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
        }
    }
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->trunk_is_tuned == 0) {
        long int ccfreq = (long int)opts->rtlsdr_center_freq;
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
        }
    }
    if (opts->trunk_is_tuned == 0) {
        rotate_symbol_out_file(opts, state);
    }
}

static void
dmr_cspdu_pf0_handle_p_maint(dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    UNUSED(state);
    if (csbk_o != 42 || cs_pdu_bits == NULL) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " P_MAINT -");
    uint16_t pm_res1 = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 12);
    uint8_t pm_kind = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[28], 3);
    uint8_t pm_res2 = cs_pdu_bits[31];
    uint32_t pm_target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[32], 24);
    uint32_t pm_source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);

    if (pm_kind == 0) {
        DSD_FPRINTF(stderr, "Disconnect; ");
    } else {
        DSD_FPRINTF(stderr, " Res Kind: %02X", pm_kind);
    }
    if (pm_res1) {
        DSD_FPRINTF(stderr, "Res A: %03X", pm_res1);
    }
    if (pm_res2) {
        DSD_FPRINTF(stderr, "Res B: 1");
    }
    DSD_FPRINTF(stderr, "Target: %d; Source: %d; ", pm_target, pm_source);
    dmr_gateway_identifier(pm_source, pm_target);
}

static void
dmr_cspdu_pf0_handle_ackvit(int csbk_o) {
    if (csbk_o != 30) {
        return;
    }
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " C_ACKVIT (Ackvitation/Authorization) ");
}

static void
dmr_cspdu_pf0_handle_acks(dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if (csbk_fid == 0x10 || !(csbk_o == 32 || csbk_o == 33 || csbk_o == 34 || csbk_o == 35)) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    if (csbk_o == 32) {
        DSD_FPRINTF(stderr, " C_ACKD Outbound TSCC; ");
    } else if (csbk_o == 33) {
        DSD_FPRINTF(stderr, " C_ACKU Inbound TSCC; ");
    } else if (csbk_o == 34) {
        DSD_FPRINTF(stderr, " P_ACKD Outbound Payload; ");
    } else {
        DSD_FPRINTF(stderr, " P_ACKU Inbound Payload; ");
    }

    uint8_t response_info = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 7);
    uint8_t reason_code = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[23], 8);
    uint8_t ack_res1 = cs_pdu_bits[31];
    uint32_t ack_target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[32], 24);
    uint32_t ack_source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);

    DSD_FPRINTF(stderr, "Response: %02X; Reason: %02X; ", response_info, reason_code);
    if (ack_res1) {
        DSD_FPRINTF(stderr, " Res: %d", ack_res1);
    }
    DSD_FPRINTF(stderr, "Target: %d; Source: %d; ", ack_target, ack_source);
    dmr_gateway_identifier(ack_source, ack_target);
    UNUSED(state);
}

static void
dmr_cspdu_pf0_handle_c_rand(int csbk_o) {
    if (csbk_o != 31) {
        return;
    }
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " C_RAND ");
}

static void
dmr_cspdu_pf0_print_tier2_target_source(const char* label, uint8_t cs_pdu_bits[]) {
    uint32_t target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[32], 24);
    uint32_t source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", label);
    DSD_FPRINTF(stderr, "Target [%d] - Source [%d] ", target, source);
}

static void
dmr_cspdu_pf0_handle_tier2_simple(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    if (csbk_o == 4) {
        dmr_cspdu_pf0_print_tier2_target_source(" Unit to Unit Voice Service Request (UU_V_Req) - ", cs_pdu_bits);
        return;
    }
    if (csbk_o == 5) {
        dmr_cspdu_pf0_print_tier2_target_source(" Unit to Unit Voice Service Answer Response (UU_Ans_Req) - ",
                                                cs_pdu_bits);
        return;
    }
    if (csbk_o == 7) {
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, " Channel Timing CSBK (CT_CSBK) ");
        return;
    }
    if (csbk_o == 38) {
        dmr_cspdu_pf0_print_tier2_target_source(" Negative Acknowledgement Response (NACK_Rsp) - ", cs_pdu_bits);
        return;
    }
    if (csbk_o == 56 && state->synctype == DSD_SYNC_DMR_MS_DATA) {
        dmr_cspdu_pf0_print_tier2_target_source(" BS Outbound Activation (BS_Dwn_Act) - ", cs_pdu_bits);
    }
    UNUSED2(opts, state);
}

static void
dmr_cspdu_pf0_ahoy_service_text(uint8_t svc_kind, const char** print_text, const char** append_text) {
    static const struct {
        uint8_t kind;
        const char* print;
        const char* append;
    } map[] = {
        {0, "Voice Call ", "Voice Call; "},
        {1, "Voice Call ", "Voice Call; "},
        {2, "Packet Data Call ", "Packet Data Call; "},
        {3, "Packet Data Call ", "Packet Data Call; "},
        {4, "UDT Short Data Call ", "UDT Short Data Call; "},
        {5, "UDT Short Data Call ", "UDT Short Data Call; "},
        {6, "UDT Short Data Polling Service ", "UDT Short Data Polling Service; "},
        {7, "Status Transport Service ", "Status Transport Service; "},
        {8, "Call Diversion Service ", "Call Diversion Service; "},
        {9, "Call Answer Service ", "Call Answer Service; "},
        {10, "Full Duplex Voice Call ", "Full Duplex Voice Call; "},
        {11, "Full Duplex Packet Data Call ", "Full Duplex Packet Data Call; "},
        {12, "Reserved ", "Reserved; "},
        {13, "Supplimentary Service (Stun/Revive/Kill/Auth): ", "Supplimentary Service (Stun/Revive/Kill/Auth); "},
        {14, "Registration/Authentication ", "Registration/Authentication; "},
        {15, "Cancel Call Service ", "Cancel Call Service; "},
    };

    *print_text = NULL;
    *append_text = NULL;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].kind == svc_kind) {
            *print_text = map[i].print;
            *append_text = map[i].append;
            return;
        }
    }
}

static void
dmr_cspdu_pf0_handle_c_ahoy(dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if (csbk_o != 28) {
        return;
    }

    uint16_t svc_opt = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 7);
    uint8_t svc_flag = cs_pdu_bits[23];
    uint8_t als_flag = cs_pdu_bits[24];
    uint8_t ahoy_gi = cs_pdu_bits[25];
    uint8_t ahoy_bf = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[26], 2);
    uint8_t svc_kind = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[28], 4);
    uint32_t ahoy_target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[32], 24);
    uint32_t ahoy_source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);
    char ahoy_str[200];
    const char* svc_print = NULL;
    const char* svc_append = NULL;

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " C_AHOY - ");
    UNUSED3(ahoy_bf, svc_flag, als_flag);
    DSD_MEMSET(ahoy_str, 0, sizeof(ahoy_str));
    DSD_SPRINTF(ahoy_str, "AHOY TGT: %d; SRC: %d; ", ahoy_target, ahoy_source);

    DSD_FPRINTF(stderr, ahoy_gi == 0 ? "Private " : "Group ");
    dsd_append(ahoy_str, sizeof ahoy_str, ahoy_gi == 0 ? "Private; " : "Group; ");
    state->gi[state->currentslot] = ahoy_gi ^ 1;

    DSD_FPRINTF(stderr, "FID: %02X SVC: %02X ", csbk_fid, svc_opt);
    dmr_cspdu_pf0_ahoy_service_text(svc_kind, &svc_print, &svc_append);
    if (svc_print != NULL) {
        DSD_FPRINTF(stderr, "%s", svc_print);
    }

    DSD_FPRINTF(stderr, "Target: %d; Source: %d; ", ahoy_target, ahoy_source);
    if (svc_append != NULL) {
        dsd_append(ahoy_str, sizeof ahoy_str, svc_append);
    }
    dmr_gateway_identifier(ahoy_source, ahoy_target);
}

static void
dmr_cspdu_pf0_handle_preamble(const dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    if (csbk_o != 61 || cs_pdu_bits == NULL) {
        return;
    }

    uint8_t content = cs_pdu_bits[16];
    uint8_t gi = cs_pdu_bits[17];
    uint8_t res = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[18], 6);
    uint8_t blocks = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[24], 8);
    uint32_t target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[32], 24);
    uint32_t source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Preamble CSBK - ");
    UNUSED2(res, blocks);
    DSD_FPRINTF(stderr, gi == 0 ? "Individual " : "Group ");
    DSD_FPRINTF(stderr, content == 0 ? "CSBK - " : "Data - ");

    if (strcmp(state->dmr_branding_sub, "XPT ") == 0) {
        target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[40], 16);
        source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[64], 16);
        if (gi == 0) {
            uint8_t target_hash[24];
            for (int i = 0; i < 16; i++) {
                target_hash[i] = cs_pdu_bits[40 + i];
            }
            uint8_t tg_hash = crc8(target_hash, 16);
            DSD_FPRINTF(stderr, "Source: %d - Target: %d - Hash: %d ", source, target, tg_hash);
        } else {
            DSD_FPRINTF(stderr, "Source: %d - Target: %d ", source, target);
        }
    } else if (strcmp(state->dmr_branding_sub, "Cap+ ") == 0) {
        if (gi == 0) {
            target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[40], 16);
        }
        source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[64], 16);
        int rest = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[60], 4);
        DSD_FPRINTF(stderr, "Source: %d - Target: %d - Rest LSN: %d", source, target, rest);
    } else {
        DSD_FPRINTF(stderr, "Source: %d - Target: %d ", source, target);
    }

    if (opts->trunk_enable == 1 && opts->trunk_tune_data_calls == 1) {
        if (state->currentslot == 0) {
            state->dmrburstL = 6;
        } else {
            state->dmrburstR = 6;
        }
    }
}

static void
dmr_cspdu_pf0_p_protect_mark_slot(dsd_state* state, int is_group_call) {
    if (state->currentslot == 0) {
        state->dmrburstL = 1;
    } else {
        state->dmrburstR = 1;
    }
    state->gi[state->currentslot] = is_group_call ? 0 : 1;
}

static const char*
dmr_cspdu_pf0_p_protect_kind_label(uint8_t p_kind) {
    switch (p_kind) {
        case 0: return " Disable Target PTT (DIS_PTT)";
        case 1: return " Enable Target PTT (EN_PTT)";
        case 2: return " Call Hangtime (ILLEGALLY_PARKED)";
        case 3: return " Enable Target MS PTT (EN_PTT_ONE_MS)";
        default: return "";
    }
}

static void
dmr_cspdu_pf0_handle_p_protect(const dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    if (csbk_o != 47 || cs_pdu_bits == NULL) {
        return;
    }

    uint16_t reserved = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 12);
    uint8_t p_kind = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[28], 3);
    uint8_t gi = cs_pdu_bits[31];
    uint32_t target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[32], 24);
    uint32_t source = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Protect (P_PROTECT) -");
    UNUSED(reserved);
    DSD_FPRINTF(stderr, gi ? " Group" : " Private");
    DSD_FPRINTF(stderr, "%s", dmr_cspdu_pf0_p_protect_kind_label(p_kind));
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "  Source: %08d; Target: %08d; ", source, target);
    dmr_gateway_identifier(source, target);

    if (opts->trunk_enable != 1) {
        return;
    }
    if (p_kind == 2) {
        if (state->trunk_cc_freq != 0 && opts->trunk_is_tuned == 1) {
            state->last_vc_sync_time = time(NULL);
            state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
            if (opts->verbose > 2) {
                DSD_FPRINTF(stderr, " Hold VC (hangtime advisory) ");
            }
        }
        return;
    }
    if (gi && opts->trunk_tune_group_calls == 1) {
        dmr_cspdu_pf0_p_protect_mark_slot(state, 1);
    }
    if (!gi && opts->trunk_tune_private_calls == 1) {
        dmr_cspdu_pf0_p_protect_mark_slot(state, 0);
    }
}

static int
dmr_cspdu_pf0_p_clear_from_voice(const dsd_state* state) {
    int clear = 0;
    if (state->currentslot == 0 && (state->dmrburstR != 16 && state->dmrburstR != 0 && state->dmrburstR != 1)) {
        clear = 2;
    }
    if (state->currentslot == 1 && (state->dmrburstL != 16 && state->dmrburstL != 0 && state->dmrburstL != 1)) {
        clear = 3;
    }
    return clear;
}

static int
dmr_cspdu_pf0_p_clear_from_data(const dsd_opts* opts, const dsd_state* state, int clear) {
    if (opts->trunk_tune_data_calls == 1) {
        if (state->currentslot == 0
            && (state->dmrburstR == 6 || state->dmrburstR == 7 || state->dmrburstR == 8 || state->dmrburstR == 10)) {
            clear = 21;
        }
        if (state->currentslot == 1
            && (state->dmrburstL == 6 || state->dmrburstL == 7 || state->dmrburstL == 8 || state->dmrburstL == 10)) {
            clear = 22;
        }
    }
    return clear;
}

static int
dmr_cspdu_pf0_p_clear_from_hold(const dsd_state* state, int clear) {
    if (state->currentslot == 0 && state->tg_hold == (uint32_t)state->lasttg && state->tg_hold != 0) {
        clear = 4;
    }
    if (state->currentslot == 1 && state->tg_hold == (uint32_t)state->lasttgR && state->tg_hold != 0) {
        clear = 5;
    }
    return clear;
}

static int
dmr_cspdu_pf0_p_clear_compute(const dsd_opts* opts, const dsd_state* state) {
    int clear = dmr_cspdu_pf0_p_clear_from_voice(state);
    clear = dmr_cspdu_pf0_p_clear_from_data(opts, state, clear);
    clear = dmr_cspdu_pf0_p_clear_from_hold(state, clear);
    return clear;
}

static void
dmr_cspdu_pf0_p_clear_mark_slots_idle(dsd_state* state) {
    if (state->currentslot == 0) {
        state->dmrburstL = 9;
        state->dmrburstR = 9;
        state->call_string[0][0] = '\0';
        state->active_channel[0][0] = '\0';
        return;
    }
    state->dmrburstR = 9;
    state->dmrburstL = 9;
    state->call_string[1][0] = '\0';
    state->active_channel[1][0] = '\0';
}

static void
dmr_cspdu_pf0_p_clear_log_fid_special(const dsd_state* state, int clear, int csbk_fid, int pslot, int oslot,
                                      int* handled) {
    *handled = 1;
    switch (csbk_fid) {
        case 255:
            if (clear) {
                DSD_FPRINTF(stderr,
                            " Slot %d No Encrypted Call Trunking; Slot %d Free; Request return to CC; SM decides "
                            "(may defer by hangtime/activity); ",
                            pslot, oslot);
            } else {
                DSD_FPRINTF(stderr,
                            " Slot %d No Encrypted Call Trunking; Slot %d Busy; Suggest remain on VC; SM decides "
                            "(may defer by hangtime/activity);",
                            pslot, oslot);
            }
            return;
        case 254:
            if (clear) {
                DSD_FPRINTF(stderr,
                            " Cap+ Rest LSN Change: %d; Slot %d Free; Slot %d Free; Request return to Rest LSN; SM "
                            "decides (may defer by hangtime/activity);",
                            state->dmr_rest_channel, pslot, oslot);
            } else {
                DSD_FPRINTF(stderr,
                            " Cap+ Rest LSN Change: %d; Slot %d Free; Slot %d Busy; Suggest remain on LSN; SM "
                            "decides (may defer by hangtime/activity);",
                            state->dmr_rest_channel, pslot, oslot);
            }
            return;
        case 253:
            if (clear) {
                DSD_FPRINTF(stderr,
                            " Cap+ Rest LSN Change: %d; No CSBK Channel Activity; Request return to Rest LSN; SM "
                            "decides (may defer by hangtime/activity);",
                            state->dmr_rest_channel);
            } else {
                DSD_FPRINTF(stderr,
                            " Cap+ Rest LSN Change: %d; CSBK Channel Activity; Suggest remain on LSN; SM decides "
                            "(may defer by hangtime/activity);",
                            state->dmr_rest_channel);
            }
            return;
        case 12:
            if (clear) {
                DSD_FPRINTF(stderr,
                            " Con+ Slot %d Termination: Slot %d Clear or Control CSBK; SM decides (may defer by "
                            "hangtime/activity);",
                            pslot, oslot);
            } else {
                DSD_FPRINTF(stderr,
                            " Con+ Slot %d Termination: Slot %d Busy Voice or Data Call; SM decides (may defer by "
                            "hangtime/activity);",
                            pslot, oslot);
            }
            return;
        default: *handled = 0; return;
    }
}

static int
dmr_cspdu_pf0_p_clear_log_generic(const dsd_state* state, int clear, int pslot, int oslot) {
    if (!clear) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear; Slot %d Busy; Suggest remain on VC; SM decides (may defer by "
                    "hangtime/activity);",
                    pslot, oslot);
    } else if (clear == 1) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear; Slot %d Idle; Request return to CC; SM decides (may defer by "
                    "hangtime/activity);",
                    pslot, oslot);
    } else if (clear == 2 || clear == 3) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear; Slot %d Free; Request return to CC; SM decides (may defer by "
                    "hangtime/activity);",
                    pslot, oslot);
    } else if (clear == 4 || clear == 5) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear w/ TG Hold %d; Slot %d Activity Override; Force return to CC; SM "
                    "decides (honors force); ",
                    pslot, state->tg_hold, oslot);
    } else if (clear == 21 || clear == 22) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear; Slot %d Data; Suggest remain on DC; SM decides (may defer by "
                    "hangtime/activity);",
                    pslot, oslot);
        clear = 0;
    }
    return clear;
}

static int
dmr_cspdu_pf0_p_clear_log_status(const dsd_state* state, int clear, int csbk_fid, int pslot, int oslot) {
    int handled = 0;
    dmr_cspdu_pf0_p_clear_log_fid_special(state, clear, csbk_fid, pslot, oslot, &handled);
    if (!handled) {
        clear = dmr_cspdu_pf0_p_clear_log_generic(state, clear, pslot, oslot);
    }
    return clear;
}

static void
dmr_cspdu_pf0_p_clear_emit_release(dsd_opts* opts, dsd_state* state, int clear) {
    if (clear == 1 || clear == 2 || clear == 3 || clear == 4 || clear == 5) {
        state->trunk_sm_force_release = 1;
    }
    if (state->trunk_cc_freq != 0 && opts->trunk_is_tuned == 1) {
        watchdog_event_current(opts, state, 0);
        watchdog_event_current(opts, state, 1);
        dmr_sm_emit_release(opts, state, -1);
    }
}

static void
dmr_cspdu_pf0_handle_p_clear(dsd_opts* opts, dsd_state* state, int csbk_o, int csbk_fid) {
    int clear;
    int pslot;
    int oslot;

    if (csbk_o != 46) {
        return;
    }

    pslot = state->currentslot + 1;
    oslot = ((state->currentslot ^ 1) & 1) + 1;
    clear = dmr_cspdu_pf0_p_clear_compute(opts, state);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Clear (P_CLEAR) ");
#ifdef PCLEAR_TUNE_AWAY
    if (opts->trunk_enable != 1) {
        return;
    }
    dmr_cspdu_pf0_p_clear_mark_slots_idle(state);
    clear = dmr_cspdu_pf0_p_clear_log_status(state, clear, csbk_fid, pslot, oslot);
    dmr_cspdu_pf0_p_clear_emit_release(opts, state, clear);
#else
    UNUSED(clear);
    UNUSED(pslot);
    UNUSED(oslot);
#endif
}

typedef struct {
    uint8_t a_type;
    uint16_t bparms1;
    uint8_t bpbits1[14];
    uint8_t reg_req;
    uint8_t backoff;
    uint16_t syscode;
    uint32_t bparms2;
    uint8_t bpbits2[24];
    uint8_t mbc_csbko;
    uint8_t mbc_res;
    uint8_t mbc_cc;
    uint8_t mbc_cdeftype;
    uint8_t mbc_res2;
    unsigned long long mbc_cdefparms;
    uint16_t a_channel;
} dmr_cspdu_pf0_c_bcast_fields;

typedef struct {
    uint16_t lpchannum;
    uint16_t abs_tx_int;
    uint16_t abs_tx_step;
    uint16_t abs_rx_int;
    uint16_t abs_rx_step;
    long freqt;
    long freqr;
} dmr_cspdu_pf0_c_bcast_abs_freqs;

static void
dmr_cspdu_pf0_c_bcast_parse(const uint8_t cs_pdu_bits[], dmr_cspdu_pf0_c_bcast_fields* f) {
    f->a_type = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 5);
    f->bparms1 = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[21], 14);
    for (int i = 0; i < 14; i++) {
        f->bpbits1[i] = cs_pdu_bits[21 + i];
    }

    f->reg_req = cs_pdu_bits[35];
    f->backoff = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[36], 4);
    f->syscode = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[40], 14);
    f->bparms2 = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);
    for (int i = 0; i < 24; i++) {
        f->bpbits2[i] = cs_pdu_bits[56 + i];
    }

    f->mbc_csbko = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[98], 6);
    f->mbc_res = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[104], 4);
    f->mbc_cc = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[108], 4);
    f->mbc_cdeftype = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[112], 4);
    f->mbc_res2 = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[116], 2);
    f->mbc_cdefparms = (unsigned long long)ConvertBitIntoBytes(&cs_pdu_bits[118], 58);

    f->a_channel = (uint16_t)ConvertBitIntoBytes(&f->bpbits2[12], 12);
}

static void
dmr_cspdu_pf0_c_bcast_print_type(uint8_t a_type) {
    static const char* const k_labels[8] = {" Announce/Withdraw TSCC (Ann_WD_TSCC)",
                                            " Specify Call Timer Parameters (CallTimer_Parms)",
                                            " Vote Now Advice (Vote_Now)",
                                            " Broadcast Local Time (Local_Time)",
                                            " Mass Registration (MassReg)",
                                            " Announce Logical Channel/Frequency Relationship (Chan_Freq)",
                                            " Adjacent Site Information (Adjacent_Site)",
                                            " General Site Parameters (Gen_Site_Params)"};

    if (a_type < 8U) {
        DSD_FPRINTF(stderr, "%s", k_labels[a_type]);
        return;
    }
    if (a_type < 0x1EU) {
        DSD_FPRINTF(stderr, " Reserved: %02X", a_type);
        return;
    }
    DSD_FPRINTF(stderr, " Manufacturer Specific: %02X", a_type);
}

static void
dmr_cspdu_pf0_c_bcast_parse_abs_freqs(const uint8_t cs_pdu_bits[], dmr_cspdu_pf0_c_bcast_abs_freqs* abs_freqs) {
    abs_freqs->lpchannum = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[118], 12);
    abs_freqs->abs_tx_int = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[130], 10);
    abs_freqs->abs_tx_step = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[140], 13);
    abs_freqs->abs_rx_int = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[153], 10);
    abs_freqs->abs_rx_step = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[163], 13);
    abs_freqs->freqr = (abs_freqs->abs_rx_int * 1000000L) + (abs_freqs->abs_rx_step * 125L);
    abs_freqs->freqt = (abs_freqs->abs_tx_int * 1000000L) + (abs_freqs->abs_tx_step * 125L);
}

static void
dmr_cspdu_pf0_c_bcast_print_abs_freqs(const dmr_cspdu_pf0_c_bcast_abs_freqs* abs_freqs, int apcn) {
    DSD_FPRINTF(stderr, apcn ? "\n APCN: %04d;" : "\n LPCN: %04d;", abs_freqs->lpchannum);
    DSD_FPRINTF(stderr, " RX Base: %d; RX Step: %d; RX Freq: %ld;", abs_freqs->abs_rx_int * 1000000,
                abs_freqs->abs_rx_step * 125, abs_freqs->freqr);
    DSD_FPRINTF(stderr, "\n            ");
    DSD_FPRINTF(stderr, " TX Base: %d; TX Step: %d; TX Freq: %ld;", abs_freqs->abs_tx_int * 1000000,
                abs_freqs->abs_tx_step * 125, abs_freqs->freqt);
}

static void
dmr_cspdu_pf0_c_bcast_print_unknown_cdef(const dmr_cspdu_pf0_c_bcast_fields* f, int include_cc) {
    DSD_FPRINTF(stderr, "\n Unknown CDEFType: %X; CDEFParms: %015llX", f->mbc_cdeftype, f->mbc_cdefparms);
    DSD_FPRINTF(stderr, " MBC Op: %02X;", f->mbc_csbko);
    if (include_cc) {
        DSD_FPRINTF(stderr, " CC: %d;", f->mbc_cc);
    }
    DSD_FPRINTF(stderr, " RES1: %X;", f->mbc_res);
    DSD_FPRINTF(stderr, " RES2: %X;", f->mbc_res2);
}

static void
dmr_cspdu_pf0_c_bcast_track_freq(dsd_state* state, long freqr) {
    state->trunk_lcn_freq[state->lcn_freq_count++ % 25] = freqr;
    if (state->lcn_freq_count > 25) {
        state->lcn_freq_count = 25;
    }
}

static void
dmr_cspdu_pf0_c_bcast_maybe_store_channel(dsd_opts* opts, dsd_state* state, uint16_t a_channel, long freqr) {
    if (a_channel == 0 || a_channel == 0xFFF || freqr == 0 || state->trunk_chan_map[a_channel] != 0) {
        return;
    }
    dsd_state_set_trunk_chan_freq(state, (uint32_t)a_channel, freqr);
    dmr_cspdu_pf0_c_bcast_track_freq(state, freqr);
    const long cand[1] = {freqr};
    dmr_sm_on_neighbor_update(opts, state, cand, 1);
}

static void
dmr_cspdu_pf0_c_bcast_print_add_remove(uint8_t flag) {
    if (flag == 0) {
        DSD_FPRINTF(stderr, " Add;");
    }
    if (flag == 1) {
        DSD_FPRINTF(stderr, " Remove;");
    }
}

static void
dmr_cspdu_pf0_c_bcast_try_switch_tscc(dsd_opts* opts, dsd_state* state, long f1, long f2, uint8_t ch1_flag,
                                      uint8_t ch2_flag) {
    if (opts->trunk_enable != 1 || opts->trunk_is_tuned != 0 || state->trunk_cc_freq == 0) {
        return;
    }

    long cur = state->trunk_cc_freq;
    long next = 0;
    if (cur == f1 && ch1_flag == 1 && ch2_flag == 0 && f2 > 0) {
        next = f2;
    } else if (cur == f2 && ch2_flag == 1 && ch1_flag == 0 && f1 > 0) {
        next = f1;
    }

    if (next > 0 && next != cur) {
        state->trunk_cc_freq = next;
        dsd_trunk_tuning_hook_return_to_cc(opts, state);
        DSD_FPRINTF(stderr, "\n Switched to announced TSCC: %.6lf MHz\n", (double)next / 1000000.0);
    }
}

static void
dmr_cspdu_pf0_c_bcast_handle_ann_wd_tscc(dsd_opts* opts, dsd_state* state, const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint8_t ann_res = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[0], 4);
    uint8_t cc_ch1 = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[4], 4);
    uint8_t cc_ch2 = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[8], 4);
    uint8_t ch1_flag = f->bpbits1[12];
    uint8_t ch2_flag = f->bpbits1[13];
    uint16_t bcast_ch1 = (uint16_t)ConvertBitIntoBytes(&f->bpbits2[0], 12);
    uint16_t bcast_ch2 = (uint16_t)ConvertBitIntoBytes(&f->bpbits2[12], 12);

    DSD_FPRINTF(stderr, "\n");
    if (ann_res) {
        DSD_FPRINTF(stderr, " Res: %X;", ann_res);
    }
    DSD_FPRINTF(stderr, " LPCN CH1: %d; CC: %d;", bcast_ch1, cc_ch1);
    dmr_cspdu_pf0_c_bcast_print_add_remove(ch1_flag);
    DSD_FPRINTF(stderr, " LPCN CH2: %d; CC: %d;", bcast_ch2, cc_ch2);
    dmr_cspdu_pf0_c_bcast_print_add_remove(ch2_flag);

    long f1 = 0;
    long f2 = 0;
    if (bcast_ch1 > 0 && bcast_ch1 < 0xFFFF) {
        f1 = state->trunk_chan_map[bcast_ch1];
    }
    if (bcast_ch2 > 0 && bcast_ch2 < 0xFFFF) {
        f2 = state->trunk_chan_map[bcast_ch2];
    }

    long cand[2];
    int ccount = 0;
    if (f1 > 0) {
        cand[ccount++] = f1;
    }
    if (f2 > 0 && f2 != f1) {
        cand[ccount++] = f2;
    }
    if (ccount > 0) {
        dmr_sm_on_neighbor_update(opts, state, cand, ccount);
    }

    dmr_cspdu_pf0_c_bcast_try_switch_tscc(opts, state, f1, f2, ch1_flag, ch2_flag);
}

static void
dmr_cspdu_pf0_c_bcast_handle_call_timer(const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint16_t t_emerg_timer = (uint16_t)ConvertBitIntoBytes(&f->bpbits1[0], 9);
    uint8_t t_packet_timer = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[9], 5);
    uint16_t t_msms_timer = (uint16_t)ConvertBitIntoBytes(&f->bpbits2[0], 12);
    uint16_t t_msline_timer = (uint16_t)ConvertBitIntoBytes(&f->bpbits2[12], 12);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Timers - Emergency: %d; Packet: %d; MS-MS: %d; Line: %d; ", t_emerg_timer, t_packet_timer,
                t_msms_timer, t_msline_timer);
}

static const char*
dmr_cspdu_pf0_c_bcast_weekday_label(uint8_t dofw) {
    static const char* const k_days[] = {"",          "Sunday",   "Monday", "Tuesday",
                                         "Wednesday", "Thursday", "Friday", "Saturday"};
    return (dofw < (sizeof(k_days) / sizeof(k_days[0]))) ? k_days[dofw] : NULL;
}

static int
dmr_cspdu_pf0_c_bcast_offset_minutes(uint8_t lt_off_fr) {
    switch (lt_off_fr) {
        case 1: return 15;
        case 2: return 30;
        case 3: return 45;
        default: return 0;
    }
}

static void
dmr_cspdu_pf0_c_bcast_handle_local_time(const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint8_t lt_day = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[0], 5);
    uint8_t lt_mon = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[5], 4);
    uint8_t lt_off = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[9], 4);
    uint8_t lt_off_sign = f->bpbits1[13];
    uint8_t lt_hour = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[0], 5);
    uint8_t lt_mins = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[5], 6);
    uint8_t lt_secs = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[11], 6);
    uint8_t lt_dofw = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[17], 3);
    uint8_t lt_off_fr = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[20], 2);
    uint8_t lt_res = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[22], 2);

    int offset = lt_off_sign ? -(int)lt_off : (int)lt_off;
    int localhour = lt_off_sign ? (int)lt_hour - (int)lt_off : (int)lt_hour + (int)lt_off;
    int localmin = (int)lt_mins + dmr_cspdu_pf0_c_bcast_offset_minutes(lt_off_fr);

    DSD_FPRINTF(stderr, "\n");
    if (lt_mon != 0 && lt_day != 0) {
        DSD_FPRINTF(stderr, " Date: %d.%d;", lt_mon, lt_day);
    }
    const char* day = dmr_cspdu_pf0_c_bcast_weekday_label(lt_dofw);
    if (day && lt_dofw != 0) {
        DSD_FPRINTF(stderr, " %s;", day);
    }
    DSD_FPRINTF(stderr, " UTC Time: %02d:%02d:%02d;", lt_hour, lt_mins, lt_secs);
    if (lt_off != 15) {
        DSD_FPRINTF(stderr, " Local: %02d:%02d:%02d;", localhour, localmin, lt_secs);
        DSD_FPRINTF(stderr, " Offset: %d;", offset);
    }
    if (lt_res) {
        DSD_FPRINTF(stderr, " Res: %d;", lt_res);
    }
}

static void
dmr_cspdu_pf0_c_bcast_handle_chan_freq(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[],
                                       const dmr_cspdu_pf0_c_bcast_fields* f) {
    if (f->a_channel == 0) {
        DSD_FPRINTF(stderr, " LPCN: Null;");
        return;
    }
    if (f->mbc_cdeftype != 0) {
        dmr_cspdu_pf0_c_bcast_print_unknown_cdef(f, 0);
        return;
    }

    dmr_cspdu_pf0_c_bcast_abs_freqs abs_freqs;
    dmr_cspdu_pf0_c_bcast_parse_abs_freqs(cs_pdu_bits, &abs_freqs);
    dmr_cspdu_pf0_c_bcast_print_abs_freqs(&abs_freqs, f->a_channel == 0xFFF);
    dmr_cspdu_pf0_c_bcast_maybe_store_channel(opts, state, f->a_channel, abs_freqs.freqr);
}

static void
dmr_cspdu_pf0_c_bcast_handle_vote_or_adjacent(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[],
                                              int csbk_fid, const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint8_t active_ava = f->bpbits2[0];
    uint8_t active_con = f->bpbits2[1];
    uint8_t c_chan_pri = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[2], 3);
    uint8_t a_chan_pri = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[5], 3);
    uint8_t a_reserved = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[8], 4);

    DSD_FPRINTF(stderr, "\n");
    dmr_decode_syscode(opts, state, (uint8_t*)cs_pdu_bits, csbk_fid, 1);
    if (active_ava != 1) {
        DSD_FPRINTF(stderr, " Active Connection Information Not Available;");
        return;
    }

    DSD_FPRINTF(stderr, active_con == 1 ? " Online;" : " Offline;");
    DSD_FPRINTF(stderr, " CC Pri: %d;", c_chan_pri);
    DSD_FPRINTF(stderr, " AC Pri: %d;", a_chan_pri);
    if (a_reserved) {
        DSD_FPRINTF(stderr, " Res: %X;", a_reserved);
    }
    if (f->a_channel != 0xFFF && f->a_channel != 0) {
        DSD_FPRINTF(stderr, " LPCN: %d;", f->a_channel);
    } else if (f->a_channel == 0) {
        DSD_FPRINTF(stderr, " LPCN: Null;");
    }

    if (f->a_channel != 0xFFF) {
        return;
    }
    if (f->mbc_cdeftype != 0) {
        dmr_cspdu_pf0_c_bcast_print_unknown_cdef(f, f->a_type == 2);
        return;
    }

    dmr_cspdu_pf0_c_bcast_abs_freqs abs_freqs;
    dmr_cspdu_pf0_c_bcast_parse_abs_freqs(cs_pdu_bits, &abs_freqs);
    dmr_cspdu_pf0_c_bcast_print_abs_freqs(&abs_freqs, 1);
    dmr_learn_chan_map(opts, state, abs_freqs.lpchannum, abs_freqs.freqr);
    const long cand[1] = {abs_freqs.freqr};
    dmr_sm_on_neighbor_update(opts, state, cand, 1);
}

static void
dmr_cspdu_pf0_c_bcast_handle_gen_site_params(const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint8_t csi = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[0], 8);
    uint8_t nin = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[16], 8);
    uint8_t hibernate_flag = f->bpbits2[1];
    uint8_t reg_tg_sub = f->bpbits2[16];

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Hibernate Flag: %d; Reg Flag: %d; RES1: %d; RES2: %X; RES3: %X; BPARMS1: %X", hibernate_flag,
                reg_tg_sub, f->bpbits1[0], csi & 0x3F, nin & 0x7F, f->bparms1);
}

static void
dmr_cspdu_pf0_c_bcast_handle_mass_reg(const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint8_t reg_window = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[5], 4);
    uint8_t aloha_mask = (uint8_t)ConvertBitIntoBytes(&f->bpbits1[9], 5);
    uint8_t reg_address = (uint8_t)ConvertBitIntoBytes(&f->bpbits2[16], 8);

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Reg Window: %X; Aloha Mask: %02X; Target: %d; ", reg_window, aloha_mask, reg_address);
    dmr_gateway_identifier(0, reg_address);
}

static void
dmr_cspdu_pf0_c_bcast_print_payload(const dsd_opts* opts, const dmr_cspdu_pf0_c_bcast_fields* f) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n ");
    DSD_FPRINTF(stderr, " SYS: %04X;", f->syscode);
    DSD_FPRINTF(stderr, " Reg: %d;", f->reg_req);
    DSD_FPRINTF(stderr, " Backoff: %X;", f->backoff);
    DSD_FPRINTF(stderr, " BParms1: %04X;", f->bparms1);
    DSD_FPRINTF(stderr, " BParms2: %06X;", f->bparms2);
    if (f->mbc_cdefparms == 0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n ");
    DSD_FPRINTF(stderr, " MBC Op: %02X;", f->mbc_csbko);
    if (f->a_type == 2) {
        DSD_FPRINTF(stderr, " CC: %d;", f->mbc_cc);
    }
    DSD_FPRINTF(stderr, " RES1: %X;", f->mbc_res);
    DSD_FPRINTF(stderr, " RES2: %X;", f->mbc_res2);
    DSD_FPRINTF(stderr, " CDEFTYPE: %X;", f->mbc_cdeftype);
    DSD_FPRINTF(stderr, " CDEFPARMS: %015llX;", f->mbc_cdefparms);
}

static void
dmr_cspdu_pf0_c_bcast_dispatch(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[], int csbk_fid,
                               const dmr_cspdu_pf0_c_bcast_fields* f) {
    switch (f->a_type) {
        case 0: dmr_cspdu_pf0_c_bcast_handle_ann_wd_tscc(opts, state, f); return;
        case 1: dmr_cspdu_pf0_c_bcast_handle_call_timer(f); return;
        case 2:
        case 6: dmr_cspdu_pf0_c_bcast_handle_vote_or_adjacent(opts, state, cs_pdu_bits, csbk_fid, f); return;
        case 3: dmr_cspdu_pf0_c_bcast_handle_local_time(f); return;
        case 4: dmr_cspdu_pf0_c_bcast_handle_mass_reg(f); return;
        case 5: dmr_cspdu_pf0_c_bcast_handle_chan_freq(opts, state, cs_pdu_bits, f); return;
        case 7: dmr_cspdu_pf0_c_bcast_handle_gen_site_params(f); return;
        default: return;
    }
}

static void
dmr_cspdu_pf0_handle_c_bcast(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if (csbk_o != 40) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Announcements (C_BCAST)");

    dmr_cspdu_pf0_c_bcast_fields f;
    dmr_cspdu_pf0_c_bcast_parse(cs_pdu_bits, &f);
    dmr_cspdu_pf0_c_bcast_print_type(f.a_type);
    dmr_cspdu_pf0_c_bcast_dispatch(opts, state, cs_pdu_bits, csbk_fid, &f);
    dmr_cspdu_pf0_c_bcast_print_payload(opts, &f);
}

static void
dmr_cspdu_handle_pf0(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_pf, int csbk_o,
                     int csbk_fid) {
    if (csbk_pf == 0) //okay to run
    {

        //set overarching manufacturer in use when non-standard feature id set is up
        if (csbk_fid != 0) {
            state->dmr_mfid = csbk_fid;
        }

        DSD_FPRINTF(stderr, "%s", KYEL);

        dmr_cspdu_pf0_handle_grants(opts, state, cs_pdu_bits, cs_pdu, csbk_o);

        dmr_cspdu_pf0_handle_move(opts, state, cs_pdu_bits, csbk_o);
        dmr_cspdu_pf0_handle_aloha(opts, state, cs_pdu_bits, csbk_o, csbk_fid);

        dmr_cspdu_pf0_handle_p_clear(opts, state, csbk_o, csbk_fid);

        dmr_cspdu_pf0_handle_p_protect(opts, state, cs_pdu_bits, csbk_o);

        dmr_cspdu_pf0_handle_c_bcast(opts, state, cs_pdu_bits, csbk_o, csbk_fid);

        dmr_cspdu_pf0_handle_c_ahoy(state, cs_pdu_bits, csbk_o, csbk_fid);

        dmr_cspdu_pf0_handle_p_maint(state, cs_pdu_bits, csbk_o);
        dmr_cspdu_pf0_handle_ackvit(csbk_o);
        dmr_cspdu_pf0_handle_acks(state, cs_pdu_bits, csbk_o, csbk_fid);
        dmr_cspdu_pf0_handle_c_rand(csbk_o);
        dmr_cspdu_pf0_handle_tier2_simple(opts, state, cs_pdu_bits, csbk_o);

        dmr_cspdu_pf0_handle_preamble(opts, state, cs_pdu_bits, csbk_o);
        //end tier 2 csbks
    }
}

static void
dmr_cspdu_cap_plus_handle_3a(int csbk_o) {
    if (csbk_o != 0x3A) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Capacity Plus CSBK 0x3A ");
}

static void
dmr_cspdu_cap_plus_handle_3b(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    uint8_t nl[6];
    uint8_t nr[6];
    long cand[6];
    int ccount = 0;

    if (csbk_o != 0x3B) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Capacity Plus Adjacent Sites\n  ");
    DSD_MEMSET(nl, 0, sizeof(nl));
    DSD_MEMSET(nr, 0, sizeof(nr));

    for (int i = 0; i < 6; i++) {
        nl[i] = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[32 + (i * 8)], 4);
        nr[i] = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[36 + (i * 8)], 4);
        if (nl[i]) {
            DSD_FPRINTF(stderr, "Site: %d Rest: %d; ", nl[i], nr[i]);
        }
    }
    for (int i = 0; i < 6; i++) {
        if (nr[i] != 0) {
            long f = state->trunk_chan_map[nr[i]];
            if (f != 0) {
                cand[ccount++] = f;
            }
        }
    }
    if (ccount > 0) {
        dmr_sm_on_neighbor_update(opts, state, cand, ccount);
    }
}

typedef struct {
    uint8_t fl;
    uint8_t ts;
    uint8_t res;
    uint8_t rest_channel;
    uint8_t active_group_count;
    uint8_t bank_one;
    uint8_t bank_two;
    uint8_t ch[24];
    uint8_t pch[24];
    uint16_t t_tg[24];
    int start;
    int end;
} dmr_cap_plus_3e_ctx;

static void
dmr_cspdu_cap_plus_3e_init_ctx(dmr_cap_plus_3e_ctx* ctx, const uint8_t cs_pdu_bits[]) {
    ctx->fl = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 2);
    ctx->ts = cs_pdu_bits[18];
    ctx->res = cs_pdu_bits[19];
    ctx->rest_channel = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[20], 4);
    ctx->active_group_count = 0;
    ctx->bank_one = 0;
    ctx->bank_two = 0;
    ctx->start = 0;
    ctx->end = 16;
    DSD_MEMSET(ctx->ch, 0, sizeof(ctx->ch));
    DSD_MEMSET(ctx->pch, 0, sizeof(ctx->pch));
    DSD_MEMSET(ctx->t_tg, 0, sizeof(ctx->t_tg));
}

static uint8_t
dmr_cspdu_cap_plus_3e_get_block_num(dsd_state* state, uint8_t ts) {
    uint8_t block_num = state->cap_plus_block_num[ts];
    if (block_num > 6) {
        state->cap_plus_block_num[ts] = 6;
        block_num = 6;
    }
    return block_num;
}

static uint8_t
dmr_cspdu_cap_plus_3e_update_multiblock(dsd_state* state, const uint8_t cs_pdu_bits[], const dmr_cap_plus_3e_ctx* ctx,
                                        uint8_t block_num) {
    if (ctx->fl == 2 || ctx->fl == 3) {
        DSD_MEMSET(state->cap_plus_csbk_bits[ctx->ts], 0, sizeof(state->cap_plus_csbk_bits[ctx->ts]));
        for (int i = 0; i < 80; i++) {
            state->cap_plus_csbk_bits[ctx->ts][i] = cs_pdu_bits[i];
        }
        state->cap_plus_block_num[ctx->ts] = 0;
        return 0;
    }

    for (int i = 0; i < 56; i++) {
        state->cap_plus_csbk_bits[ctx->ts][i + 80 + (56 * block_num)] = cs_pdu_bits[i + 24];
    }
    state->cap_plus_block_num[ctx->ts]++;
    return (uint8_t)(block_num + 1);
}

static void
dmr_cspdu_cap_plus_3e_sync_rest(dsd_opts* opts, dsd_state* state, const dmr_cap_plus_3e_ctx* ctx) {
    if (ctx->rest_channel != state->dmr_rest_channel) {
        state->dmr_rest_channel = ctx->rest_channel;
    }
    if (state->trunk_chan_map[ctx->rest_channel] != 0) {
        opts->trunk_is_tuned = 1;
    }
}

static void
dmr_cspdu_cap_plus_3e_print_header(const dmr_cap_plus_3e_ctx* ctx) {
    DSD_FPRINTF(stderr, " Capacity Plus Channel Status - FL: %d TS: %d RS: %d - Rest LSN: %d", ctx->fl, ctx->ts,
                ctx->res, ctx->rest_channel);
    if (ctx->fl == 0) {
        DSD_FPRINTF(stderr, " - Appended Block");
    } else if (ctx->fl == 1) {
        DSD_FPRINTF(stderr, " - Final Block");
    } else if (ctx->fl == 2) {
        DSD_FPRINTF(stderr, " - Initial Block");
    } else if (ctx->fl == 3) {
        DSD_FPRINTF(stderr, " - Single Block");
    }
}

static void
dmr_cspdu_cap_plus_3e_parse_group_banks(dsd_state* state, dmr_cap_plus_3e_ctx* ctx) {
    uint8_t b2_start = 0;

    ctx->bank_one = (uint8_t)ConvertBitIntoBytes(&state->cap_plus_csbk_bits[ctx->ts][24], 8);
    for (int i = 0; i < 8; i++) {
        ctx->ch[i] = state->cap_plus_csbk_bits[ctx->ts][i + 24];
        if (ctx->ch[i] == 1) {
            ctx->active_group_count++;
        }
    }

    ctx->bank_two =
        (uint8_t)ConvertBitIntoBytes(&state->cap_plus_csbk_bits[ctx->ts][32 + (ctx->active_group_count * 8)], 8);
    b2_start = ctx->active_group_count;
    if (ctx->bank_two == 0) {
        return;
    }

    for (int i = 0; i < 8; i++) {
        ctx->ch[i + 8] = state->cap_plus_csbk_bits[ctx->ts][i + 32 + (b2_start * 8)];
        if (ctx->ch[i + 8] == 1) {
            ctx->active_group_count++;
        }
    }
}

static int
dmr_cspdu_cap_plus_3e_parse_private_bank_one(dsd_state* state, dmr_cap_plus_3e_ctx* ctx) {
    int k = 0;
    uint8_t pdflag =
        (uint8_t)ConvertBitIntoBytes(&state->cap_plus_csbk_bits[ctx->ts][40 + (ctx->active_group_count * 8)], 8);
    if (pdflag == 0) {
        return 0;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Bank One F%X Private or Data Call(s) - ", pdflag);
    for (int i = 0; i < 8; i++) {
        ctx->pch[i] = state->cap_plus_csbk_bits[ctx->ts][i + 48 + (ctx->active_group_count * 8)];
        if (ctx->pch[i] != 1) {
            continue;
        }
        DSD_FPRINTF(stderr, " LSN %02d:", i + 1);
        uint16_t private_target = (uint16_t)ConvertBitIntoBytes(
            &state->cap_plus_csbk_bits[ctx->ts][56 + (k * 16) + (ctx->active_group_count * 8)], 16);
        DSD_FPRINTF(stderr, " TGT %d;", private_target);
        k++;
        if (ctx->bank_one == 0) {
            ctx->bank_one = 0xFF;
        }
    }
    return k;
}

static void
dmr_cspdu_cap_plus_3e_parse_private_bank_two(dsd_state* state, dmr_cap_plus_3e_ctx* ctx, int pd_b2) {
    uint8_t pdflag2 = (uint8_t)ConvertBitIntoBytes(
        &state->cap_plus_csbk_bits[ctx->ts][56 + (ctx->active_group_count * 8) + (pd_b2 * 16)], 8);
    int k = 0;

    if (pdflag2 == 0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Bank Two F%02X Private or Data Call(s) - ", pdflag2);
    for (int i = 0; i < 8; i++) {
        ctx->pch[i + 8] = state->cap_plus_csbk_bits[ctx->ts][i + 64 + (ctx->active_group_count * 8) + (pd_b2 * 16)];
        if (ctx->pch[i + 8] != 1) {
            continue;
        }
        DSD_FPRINTF(stderr, " LSN %02d:", i + 1);
        uint16_t private_target = (uint16_t)ConvertBitIntoBytes(
            &state->cap_plus_csbk_bits[ctx->ts][64 + (k * 16) + (ctx->active_group_count * 8) + (pd_b2 * 16)], 16);
        DSD_FPRINTF(stderr, " TGT %d;", private_target);
        k++;
        if (ctx->bank_two == 0) {
            ctx->bank_two = 0xFF;
        }
    }
}

static int
dmr_cspdu_cap_plus_3e_calc_start(const dmr_cap_plus_3e_ctx* ctx) {
    if ((ctx->bank_one & 0xF0) != 0 || ctx->rest_channel < 5) {
        return 0;
    }
    if ((ctx->bank_one & 0x0F) != 0 || ctx->rest_channel < 9) {
        return 4;
    }
    if ((ctx->bank_two & 0xF0) != 0 || ctx->rest_channel < 13) {
        return 8;
    }
    return 12;
}

static int
dmr_cspdu_cap_plus_3e_calc_end(const dmr_cap_plus_3e_ctx* ctx) {
    if ((ctx->bank_two & 0x0F) != 0 || ctx->rest_channel > 12) {
        return 16;
    }
    if ((ctx->bank_two & 0xF0) != 0 || ctx->rest_channel > 9) {
        return 12;
    }
    if ((ctx->bank_one & 0x0F) != 0 || (ctx->rest_channel > 4 && ctx->rest_channel < 9)) {
        return 8;
    }
    if ((ctx->bank_one & 0xF0) != 0 || ctx->rest_channel < 5) {
        return 4;
    }
    return 16;
}

static void
dmr_cspdu_cap_plus_3e_calc_window(dmr_cap_plus_3e_ctx* ctx) {
    const int b1_hi = (ctx->bank_one & 0xF0) != 0;
    const int b1_lo = (ctx->bank_one & 0x0F) != 0;
    const int b2_hi = (ctx->bank_two & 0xF0) != 0;
    const int b2_lo = (ctx->bank_two & 0x0F) != 0;

    if (b1_hi || b1_lo || b2_hi || b2_lo) {
        ctx->start = dmr_cspdu_cap_plus_3e_calc_start(ctx);
        ctx->end = dmr_cspdu_cap_plus_3e_calc_end(ctx);
        return;
    }
    ctx->start = dmr_cspdu_cap_plus_3e_calc_start(ctx);
    ctx->end = dmr_cspdu_cap_plus_3e_calc_end(ctx);
}

static void
dmr_cspdu_cap_plus_3e_emit_row_breaks(int start, int i) {
    if (start < 1 && i == 4) {
        DSD_FPRINTF(stderr, "\n  ");
    }
    if (start < 5 && i == 8) {
        DSD_FPRINTF(stderr, "\n  ");
    }
    if (start < 9 && i == 12) {
        DSD_FPRINTF(stderr, "\n  ");
    }
}

static void
dmr_cspdu_cap_plus_3e_render_activity(const dsd_opts* opts, dsd_state* state, dmr_cap_plus_3e_ctx* ctx) {
    char cap_active[20];
    int k = 0;
    int x = 0;

    DSD_FPRINTF(stderr, "\n  ");
    DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
    DSD_SPRINTF(state->active_channel[0], "Cap+ ");
    state->last_active_time = time(NULL);
    dmr_cspdu_cap_plus_3e_calc_window(ctx);

    for (int i = ctx->start; i < ctx->end; i++) {
        if (i == 8) {
            k++;
        }
        dmr_cspdu_cap_plus_3e_emit_row_breaks(ctx->start, i);
        DSD_FPRINTF(stderr, "LSN %02d: ", i + 1);

        if (ctx->ch[i] == 1) {
            uint16_t tg = (uint16_t)ConvertBitIntoBytes(&state->cap_plus_csbk_bits[ctx->ts][(k * 8) + 32], 8);
            DSD_FPRINTF(stderr, tg ? "%5d;  " : "Group;  ", tg);
            if (opts->trunk_tune_group_calls == 1) {
                ctx->t_tg[i] = tg;
            }
            if (tg != 0) {
                k++;
            }
            DSD_SPRINTF(cap_active, "LSN:%d TG:%d; ", i + 1, tg);
            dsd_append(state->active_channel[i + 1], sizeof state->active_channel[0], cap_active);
            continue;
        }

        if (ctx->pch[i] == 1) {
            uint16_t tg = (uint16_t)ConvertBitIntoBytes(
                &state->cap_plus_csbk_bits[ctx->ts][(ctx->active_group_count * 8) + (x * 16) + 56], 16);
            DSD_FPRINTF(stderr, tg ? "%5d;  " : " P||D;  ", tg);
            if (opts->trunk_tune_private_calls == 1) {
                ctx->t_tg[i] = tg;
            }
            if (tg != 0) {
                x++;
            }
            if (opts->trunk_tune_private_calls == 1) {
                DSD_SPRINTF(cap_active, "LSN:%d PC:%d; ", i + 1, tg);
                dsd_append(state->active_channel[i + 1], sizeof state->active_channel[0], cap_active);
            }
            continue;
        }

        if (i + 1 == ctx->rest_channel) {
            DSD_FPRINTF(stderr, " Rest;  ");
        } else {
            DSD_FPRINTF(stderr, " Idle;  ");
        }
    }
}

static void
dmr_cspdu_cap_plus_3e_set_branding(dsd_state* state) {
    state->dmr_mfid = 0x10;
    DSD_SPRINTF(state->dmr_branding, "%s", "Motorola");
    DSD_SPRINTF(state->dmr_branding_sub, "%s", "Cap+ ");
    DSD_SPRINTF(state->dmr_site_parms, "%s", "");
}

static void
dmr_cspdu_cap_plus_3e_try_tune_grants(dsd_opts* opts, dsd_state* state, const dmr_cap_plus_3e_ctx* ctx) {
    if ((time(NULL) - state->last_vc_sync_time) <= 2) {
        return;
    }

    for (int j = ctx->start; j < ctx->end; j++) {
        const int is_group_call = (ctx->pch[j] == 1) ? 0 : 1;
        dsd_tg_policy_decision policy_decision;
        int policy_allowed = 0;

        dmr_csbk_print_group_label(state, (uint32_t)ctx->t_tg[j]);
        policy_allowed = dmr_policy_tune_allowed(opts, state, ctx->t_tg[j], 0, is_group_call, 0, &policy_decision);
        if (ctx->t_tg[j] == 0 || state->trunk_cc_freq == 0 || opts->trunk_enable != 1) {
            continue;
        }
        if (!policy_allowed) {
            dmr_policy_log_block(opts, is_group_call, ctx->t_tg[j], 0, &policy_decision);
            continue;
        }
        if (state->trunk_chan_map[j + 1] == 0) {
            continue;
        }

        if (state->tg_hold != 0) {
            if ((j & 1) == 0) {
                state->lasttg = ctx->t_tg[j];
            } else {
                state->lasttgR = ctx->t_tg[j];
            }
        }
        if (opts->rtlsdr_center_freq != (uint32_t)state->trunk_chan_map[j + 1]) {
            dmr_reset_blocks(opts, state);
        }
        dsd_trunk_tuning_hook_tune_to_freq(opts, state, state->trunk_chan_map[j + 1], 0);
        break;
    }
}

static void
dmr_cspdu_cap_plus_3e_dump_payload(const dsd_opts* opts, dsd_state* state, const dmr_cap_plus_3e_ctx* ctx,
                                   uint8_t block_num) {
    if (ctx->fl != 1 || opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "%s\n", KYEL);
    DSD_FPRINTF(stderr, " CAP+ Multi Block PDU \n  ");
    for (int i = 0; i < (10 + (block_num * 7)); i++) {
        uint8_t fl_bytes = (uint8_t)ConvertBitIntoBytes(&state->cap_plus_csbk_bits[ctx->ts][((size_t)i * 8)], 8);
        DSD_FPRINTF(stderr, "[%02X]", fl_bytes);
        if (i == 17 || i == 35) {
            DSD_FPRINTF(stderr, "\n  ");
        }
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
dmr_cspdu_cap_plus_3e_try_return_to_rest(dsd_opts* opts, dsd_state* state, const dmr_cap_plus_3e_ctx* ctx) {
    uint16_t empty[24];
    int busy;

    DSD_MEMSET(empty, 0, sizeof(empty));
    busy = memcmp(empty, ctx->t_tg, sizeof(empty));
    if (busy || opts->trunk_enable != 1 || state->trunk_cc_freq == state->trunk_chan_map[ctx->rest_channel]) {
        return;
    }
    if (state->trunk_chan_map[ctx->rest_channel] != 0) {
        state->trunk_cc_freq = state->trunk_chan_map[ctx->rest_channel];
    }

    uint8_t dummy[12];
    uint8_t* dbits = NULL;
    DSD_MEMSET(dummy, 0, sizeof(dummy));
    dummy[0] = 46;
    dummy[1] = 253;
    dmr_cspdu(opts, state, dbits, dummy, 1, 0);
}

static void
dmr_cspdu_cap_plus_handle_3e(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    dmr_cap_plus_3e_ctx ctx;
    uint8_t block_num;

    if (csbk_o != 0x3E) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);

    dmr_cspdu_cap_plus_3e_init_ctx(&ctx, cs_pdu_bits);
    block_num = dmr_cspdu_cap_plus_3e_get_block_num(state, ctx.ts);
    block_num = dmr_cspdu_cap_plus_3e_update_multiblock(state, cs_pdu_bits, &ctx, block_num);
    dmr_cspdu_cap_plus_3e_sync_rest(opts, state, &ctx);
    dmr_cspdu_cap_plus_3e_print_header(&ctx);
    dmr_cspdu_cap_plus_3e_parse_group_banks(state, &ctx);

    if (!(ctx.fl == 1 || ctx.fl == 3)) {
        return;
    }

    int pd_b2 = dmr_cspdu_cap_plus_3e_parse_private_bank_one(state, &ctx);
    dmr_cspdu_cap_plus_3e_parse_private_bank_two(state, &ctx, pd_b2);
    dmr_cspdu_cap_plus_3e_render_activity(opts, state, &ctx);

    dmr_cspdu_cap_plus_3e_set_branding(state);
    DSD_FPRINTF(stderr, "%s", KNRM);

    if (opts->trunk_use_allow_list == 1) {
        state->last_vc_sync_time = 0;
        state->last_vc_sync_time_m = 0.0;
    }
    if (state->tg_hold != 0) {
        state->last_vc_sync_time = 0;
    }
    if ((time(NULL) - state->last_vc_sync_time) > 2) {
        rotate_symbol_out_file(opts, state);
    }

    dmr_cspdu_cap_plus_3e_try_tune_grants(opts, state, &ctx);
    dmr_cspdu_cap_plus_3e_dump_payload(opts, state, &ctx, block_num);
    DSD_MEMSET(state->cap_plus_csbk_bits[ctx.ts], 0, sizeof(state->cap_plus_csbk_bits[ctx.ts]));
    state->cap_plus_block_num[ctx.ts] = 0;
    dmr_cspdu_cap_plus_3e_try_return_to_rest(opts, state, &ctx);
}

static void
dmr_cspdu_handle_cap_plus(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_o,
                          int csbk_fid) {
    UNUSED(cs_pdu);
    if (csbk_fid != 0x10) {
        return;
    }

    dmr_cspdu_cap_plus_handle_3a(csbk_o);
    dmr_cspdu_cap_plus_handle_3b(opts, state, cs_pdu_bits, csbk_o);
    dmr_cspdu_cap_plus_handle_3e(opts, state, cs_pdu_bits, csbk_o);
}

static void
dmr_cspdu_con_plus_set_branding(dsd_state* state) {
    state->dmr_mfid = 0x06;
    DSD_SPRINTF(state->dmr_branding, "%s", "Motorola");
    DSD_SPRINTF(state->dmr_branding_sub, "Con+ ");
}

static void
dmr_cspdu_con_plus_handle_adjacent(dsd_state* state, const uint8_t cs_pdu[], int csbk_o) {
    uint8_t nb[5];

    if (csbk_o != 0x01) {
        return;
    }

    nb[0] = cs_pdu[2] & 0x3F;
    nb[1] = cs_pdu[3] & 0x3F;
    nb[2] = cs_pdu[4] & 0x3F;
    nb[3] = cs_pdu[5] & 0x3F;
    nb[4] = cs_pdu[6] & 0x3F;

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Connect Plus Adjacent Sites:");
    for (int i = 0; i < 5; i++) {
        if (nb[i] != 0) {
            DSD_FPRINTF(stderr, " %d;", nb[i]);
        }
    }
    if (nb[0] == 0) {
        DSD_FPRINTF(stderr, " None Listed;");
    }
    dmr_cspdu_con_plus_set_branding(state);
}

typedef struct {
    uint32_t src_addr;
    uint32_t grp_addr;
    uint8_t lcn;
    uint8_t tslot;
    uint8_t opt;
} dmr_con_plus_voice_grant;

static void
dmr_cspdu_con_plus_print_voice_grant(const dmr_con_plus_voice_grant* g) {
    DSD_FPRINTF(stderr, "%s", KYEL);
    if (g->opt == 2) {
        DSD_FPRINTF(stderr, " Connect Plus Group Voice Channel Grant;");
    } else if (g->opt == 3) {
        DSD_FPRINTF(stderr, " Connect Plus Private Voice Channel Grant;");
    } else {
        DSD_FPRINTF(stderr, " Connect Plus Unknown %02X Channel Grant;", g->opt);
    }
    DSD_FPRINTF(stderr, " Target: %d; Source: %d; LCN: %d; TS: %d;", g->grp_addr, g->src_addr, g->lcn, g->tslot + 1);
}

static int
dmr_cspdu_con_plus_skip_voice_tune(const dsd_opts* opts, uint8_t opt) {
    if (opts->trunk_tune_group_calls == 0 && opt == 2) {
        return 1;
    }
    if (opts->trunk_tune_private_calls == 0 && opt == 3) {
        return 1;
    }
    return opts->trunk_tune_group_calls == 0 && opt != 2 && opt != 3;
}

static void
dmr_cspdu_con_plus_try_tune_voice(dsd_opts* opts, dsd_state* state, const dmr_con_plus_voice_grant* g) {
    dsd_tg_policy_decision policy_decision;
    int policy_allowed;
    long f;

    if (opts->trunk_tune_group_calls != 1) {
        return;
    }
    if (time(NULL) - state->last_vc_sync_time <= (opts->trunk_tune_data_calls == 1 ? 4 : 2)) {
        return;
    }

    policy_allowed =
        dmr_policy_tune_allowed(opts, state, g->grp_addr, g->src_addr, (g->opt == 3) ? 0 : 1, 0, &policy_decision);
    if (state->trunk_cc_freq == 0 || opts->trunk_enable != 1) {
        return;
    }
    if (!policy_allowed) {
        dmr_policy_log_block(opts, (g->opt == 3) ? 0 : 1, g->grp_addr, g->src_addr, &policy_decision);
        return;
    }

    f = state->trunk_chan_map[g->lcn];
    if (f == 0) {
        return;
    }

    state->is_con_plus = 1;
    if (g->opt == 3) {
        dmr_sm_emit_indiv_grant(opts, state, f, g->lcn, g->grp_addr, g->src_addr);
    } else {
        dmr_sm_emit_group_grant(opts, state, f, g->lcn, g->grp_addr, g->src_addr);
    }
}

static void
dmr_cspdu_con_plus_handle_voice(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu[], int csbk_o) {
    dmr_con_plus_voice_grant g;
    char suf[24];

    if (csbk_o != 0x03) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    g.src_addr = ((cs_pdu[2] << 16) + (cs_pdu[3] << 8) + cs_pdu[4]);
    g.grp_addr = ((cs_pdu[5] << 16) + (cs_pdu[6] << 8) + cs_pdu[7]);
    g.lcn = ((cs_pdu[8] & 0xF0) >> 4);
    g.tslot = ((cs_pdu[8] & 0x08) >> 3) & 1;
    g.opt = cs_pdu[9];
    dmr_cspdu_con_plus_print_voice_grant(&g);
    dmr_cspdu_con_plus_set_branding(state);

    dmr_format_chan_suffix(g.tslot, suf, sizeof suf);
    DSD_SPRINTF(state->active_channel[g.tslot], "Active Ch: %04X%s TG: %d; ", g.lcn, suf, g.grp_addr);
    state->last_active_time = time(NULL);

    if (opts->trunk_enable == 0 && state->trunk_chan_map[g.lcn] != 0) {
        state->trunk_vc_freq[0] = state->trunk_chan_map[g.lcn];
        state->trunk_vc_freq[1] = state->trunk_chan_map[g.lcn];
    }
    if (state->tg_hold != 0 && state->tg_hold == g.grp_addr) {
        state->last_vc_sync_time = 0;
        state->last_vc_sync_time_m = 0.0;
    }

    dmr_csbk_print_group_label(state, g.grp_addr);
    if (dmr_cspdu_con_plus_skip_voice_tune(opts, g.opt)) {
        return;
    }
    dmr_cspdu_con_plus_try_tune_voice(opts, state, &g);
}

static void
dmr_cspdu_con_plus_handle_data(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu[], int csbk_o) {
    uint32_t dtarget;
    uint8_t lcn;
    uint8_t tslot;
    dsd_tg_policy_decision policy_decision;
    int policy_allowed;
    char suf[24];

    if (csbk_o != 0x06) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    dtarget = ((cs_pdu[2] << 16) + (cs_pdu[3] << 8) + cs_pdu[4]);
    lcn = ((cs_pdu[5] & 0xF0) >> 4);
    tslot = ((cs_pdu[5] & 0x08) >> 3) & 1;
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Connect Plus Data Channel Grant;");
    DSD_FPRINTF(stderr, " Target: %d; LCN: %d; TS: %d;", dtarget, lcn, tslot + 1);
    dmr_cspdu_con_plus_set_branding(state);

    if (opts->trunk_tune_data_calls == 0) {
        return;
    }

    dmr_format_chan_suffix(tslot, suf, sizeof suf);
    DSD_SPRINTF(state->active_channel[tslot], "Active Ch: %04X%s TG: %d; ", lcn, suf, dtarget);
    state->last_active_time = time(NULL);
    if (opts->trunk_enable == 0 && state->trunk_chan_map[lcn] != 0) {
        state->trunk_vc_freq[0] = state->trunk_chan_map[lcn];
        state->trunk_vc_freq[1] = state->trunk_chan_map[lcn];
    }

    dmr_csbk_print_group_label(state, dtarget);
    if (opts->trunk_tune_data_calls != 1 || (time(NULL) - state->last_vc_sync_time <= 2)) {
        return;
    }

    policy_allowed = dmr_policy_tune_allowed(opts, state, dtarget, 0, 0, 1, &policy_decision);
    if (state->trunk_cc_freq != 0 && opts->trunk_enable == 1 && policy_allowed) {
        if (state->trunk_chan_map[lcn] != 0) {
            dsd_trunk_tuning_hook_tune_to_freq(opts, state, state->trunk_chan_map[lcn], 0);
            state->is_con_plus = 1;
            dmr_reset_blocks(opts, state);
        }
    } else if (state->trunk_cc_freq != 0 && opts->trunk_enable == 1) {
        dmr_policy_log_block(opts, 0, dtarget, 0, &policy_decision);
    }
}

static void
dmr_cspdu_con_plus_handle_termination(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu[], int csbk_o) {
    uint32_t ttarget;

    if (csbk_o != 0x0C) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    ttarget = ((cs_pdu[2] << 16) + (cs_pdu[3] << 8) + cs_pdu[4]);
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Connect Plus Slot Termination;");
    DSD_FPRINTF(stderr, " Target: %d;", ttarget);
    dmr_sm_emit_release(opts, state, -1);
    dmr_cspdu_con_plus_set_branding(state);
}

static void
dmr_cspdu_handle_con_plus(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_o,
                          int csbk_fid) {
    UNUSED(cs_pdu_bits);
    if (csbk_fid != 0x06) {
        return;
    }

    dmr_cspdu_con_plus_handle_adjacent(state, cs_pdu, csbk_o);
    dmr_cspdu_con_plus_handle_voice(opts, state, cs_pdu, csbk_o);
    dmr_cspdu_con_plus_handle_data(opts, state, cs_pdu, csbk_o);
    dmr_cspdu_con_plus_handle_termination(opts, state, cs_pdu, csbk_o);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static const char*
dmr_cspdu_xpt_status_label(uint8_t status) {
    if (status == 3) {
        return " Null; ";
    }
    if (status == 2) {
        return " Priv; ";
    }
    if (status == 1) {
        return " Unk;  ";
    }
    return " Idle; ";
}

static int
dmr_cspdu_xpt_lcn_start(uint8_t seq) {
    if (seq == 1) {
        return 4;
    }
    if (seq == 2) {
        return 7;
    }
    return 1;
}

static void
dmr_cspdu_xpt_update_cc_from_input(dsd_opts* opts, dsd_state* state) {
    if (opts->use_rigctl == 1) {
        long int ccfreq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
            opts->trunk_is_tuned = 1;
        }
    }
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        long int ccfreq = (long int)opts->rtlsdr_center_freq;
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
            opts->trunk_is_tuned = 1;
        }
    }
}

static void
dmr_cspdu_xpt_print_and_collect(const dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t xpt_seq,
                                uint8_t xpt_bank, const uint8_t xpt_ch[6], uint8_t t_tg[18]) {
    char xpt_active[20];
    int xpt_lcn = dmr_cspdu_xpt_lcn_start(xpt_seq);
    const int t_tg_count = 18;

    if (xpt_seq == 0) {
        DSD_SPRINTF(state->active_channel[0], "XPT ");
    } else {
        DSD_SPRINTF(state->active_channel[xpt_seq], "%s", "");
    }
    state->last_active_time = time(NULL);

    for (int i = 0; i < 6; i++) {
        int slot_idx = i + xpt_bank;
        if (slot_idx >= t_tg_count) {
            continue;
        }
        uint16_t tg = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[i * 8 + 32], 8);

        if (i == 0 || i == 2 || i == 4) {
            DSD_FPRINTF(stderr, "\n LCN %d - ", xpt_lcn);
            xpt_lcn++;
        }
        DSD_FPRINTF(stderr, "LSN %02d: ", slot_idx + 1);
        DSD_FPRINTF(stderr, "ST-%X", xpt_ch[i]);
        if (tg != 0) {
            DSD_FPRINTF(stderr, " %03d;  ", tg);
            t_tg[slot_idx] = (uint8_t)tg;
            if (xpt_ch[i] == 3) {
                DSD_SPRINTF(xpt_active, "LSN:%d TG:%d; ", slot_idx + 1, tg);
            } else if (xpt_ch[i] == 2) {
                DSD_SPRINTF(xpt_active, "LSN:%d PC:%d; ", slot_idx + 1, tg);
            } else {
                DSD_SPRINTF(xpt_active, "LSN:%d UK:%d; ", slot_idx + 1, tg);
            }
            dsd_append(state->active_channel[xpt_seq], sizeof state->active_channel[0], xpt_active);
            continue;
        }

        DSD_FPRINTF(stderr, "%s", dmr_cspdu_xpt_status_label(xpt_ch[i]));
        if (xpt_ch[i] == 2 && opts->trunk_tune_private_calls == 1) {
            t_tg[slot_idx] = 1;
        }
    }
}

static void
dmr_cspdu_xpt_try_tune(dsd_opts* opts, dsd_state* state, const uint8_t t_tg[18], uint8_t xpt_bank) {
    if ((time(NULL) - state->last_vc_sync_time) <= 2) {
        return;
    }

    const int t_tg_count = 18;
    for (int j = 0; j < 6; j++) {
        int slot_idx = j + xpt_bank;
        if (slot_idx >= t_tg_count) {
            continue;
        }
        uint32_t tg = t_tg[slot_idx];
        dsd_tg_policy_decision policy_decision;
        int policy_allowed;

        if (tg == 0 || state->trunk_cc_freq == 0 || opts->trunk_enable != 1) {
            continue;
        }

        dmr_csbk_print_group_label(state, tg);
        policy_allowed = dmr_policy_tune_allowed(opts, state, tg, 0, 1, 0, &policy_decision);
        if (!policy_allowed) {
            dmr_policy_log_block(opts, 1, tg, 0, &policy_decision);
            continue;
        }
        if (state->trunk_chan_map[slot_idx + 1] == 0) {
            continue;
        }
        if (!(opts->use_rigctl == 1 || opts->audio_in_type == AUDIO_IN_RTL)) {
            continue;
        }

        DSD_FPRINTF(stderr, "\n LSN/TG to tune to: %d - %d", slot_idx + 1, tg);
        if (state->tg_hold != 0) {
            if ((j & 1) == 0) {
                state->lasttg = (int)tg;
            } else {
                state->lasttgR = (int)tg;
            }
        }
        dmr_sm_emit_group_grant(opts, state, state->trunk_chan_map[slot_idx + 1], 0, tg, 0);
        break;
    }
}

static void
dmr_cspdu_xpt_handle_site_status(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    uint8_t xpt_ch[6];
    uint8_t t_tg[18];
    uint8_t xpt_seq;
    uint8_t xpt_free;
    uint8_t xpt_bank;

    if (csbk_o != 0x0A) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_MEMSET(t_tg, 0, sizeof(t_tg));

    xpt_seq = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[0], 2);
    xpt_free = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[16], 4);
    xpt_bank = (xpt_seq <= 2 && xpt_seq != 0) ? (uint8_t)(xpt_seq * 6) : 0;
    for (int i = 0; i < 6; i++) {
        xpt_ch[i] = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[20 + (i * 2)], 2);
    }

    DSD_FPRINTF(stderr, " Hytera XPT Site Status - Free LCN: %d SN: %d", xpt_free, xpt_seq);
    if (xpt_free != 0) {
        long f = state->trunk_chan_map[xpt_free];
        if (f != 0) {
            const long candx[1] = {f};
            dmr_sm_on_neighbor_update(opts, state, candx, 1);
        }
    }

    dmr_cspdu_xpt_print_and_collect(opts, state, cs_pdu_bits, xpt_seq, xpt_bank, xpt_ch, t_tg);
    DSD_SPRINTF(state->dmr_site_parms, "Free LCN - %d ", xpt_free);
    dmr_cspdu_xpt_update_cc_from_input(opts, state);

    if (opts->trunk_tune_group_calls == 1) {
        if (opts->trunk_use_allow_list == 1) {
            state->last_vc_sync_time = 0;
            state->last_vc_sync_time_m = 0.0;
        }
        if (state->tg_hold != 0) {
            state->last_vc_sync_time = 0;
        }
        if ((time(NULL) - state->last_vc_sync_time) > 2) {
            rotate_symbol_out_file(opts, state);
        }
        dmr_cspdu_xpt_try_tune(opts, state, t_tg, xpt_bank);
    }

    DSD_SPRINTF(state->dmr_branding_sub, "XPT ");
}

static void
dmr_cspdu_xpt_handle_adjacent(uint8_t cs_pdu_bits[], dsd_state* state, int csbk_o) {
    uint8_t xpt_site_id[4];
    uint8_t xpt_site_rp[4];
    uint8_t xpt_sn;

    if (csbk_o != 0x0B) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);

    xpt_sn = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[0], 2);
    for (int i = 0; i < 4; i++) {
        xpt_site_id[i] = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[16 + (i * 16)], 5);
        xpt_site_rp[i] = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[24 + (i * 16)], 4);
    }

    DSD_FPRINTF(stderr, " Hytera XPT CSBK 0x0B - SN: %d", xpt_sn);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " XPT Adjacent ");
    for (int i = 0; i < 4; i++) {
        if (xpt_site_id[i] != 0) {
            DSD_FPRINTF(stderr, "Site:%d Free:%d; ", xpt_site_id[i], xpt_site_rp[i]);
        }
    }
    DSD_SPRINTF(state->dmr_branding_sub, "XPT ");
}

static void
dmr_cspdu_handle_xpt(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_o,
                     int csbk_fid) {
    UNUSED(cs_pdu);
    if (csbk_fid != 0x68) {
        return;
    }

    dmr_cspdu_xpt_handle_site_status(opts, state, cs_pdu_bits, csbk_o);
    dmr_cspdu_xpt_handle_adjacent(cs_pdu_bits, state, csbk_o);
}

static void
dmr_cspdu_handle_moto_unknown(uint8_t cs_pdu[], int csbk_o, int csbk_fid) {
    if (csbk_o == 41 && csbk_fid == 0x10) {
        //initial line break
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, " Moto Data Channel: %02X; ", csbk_o);
        for (int i = 2; i < 10; i++) {
            DSD_FPRINTF(stderr, "%02X ", cs_pdu[i]);
        }

        //SDRTrunk suggest this could be a data channel revert announcement
        //I'm not even sure what a revert data channel is
        //Moto Unknown Data Opcode: 29; 00 00 00 39 04 FC 00 00
    }
}

static int
dmr_cspdu_apply_protect_flag_checks(uint32_t IrrecoverableErrors, int csbk_o, int csbk_fid, int csbk_pf) {
    if (IrrecoverableErrors == 0) {
        //Hytera XPT CSBK Check -- if bits 0 and 1 are used as lcss, gi, ts, then the pf bit may be set on
        if (csbk_fid == 0x68 && (csbk_o == 0x0A || csbk_o == 0x0B)) {
            csbk_pf = 0;
        }
        if (csbk_pf == 1) //check the protect flag, don't run if set
        {
            DSD_FPRINTF(stderr, "%s", KRED);
            DSD_FPRINTF(stderr, "\n Protected Control Signalling Block(s)");
            DSD_FPRINTF(stderr, "%s", KNRM);
        }
    }
    return csbk_pf;
}

static void
dmr_cspdu_init_cc_anchor(const dsd_opts* opts, dsd_state* state) {
    // If trunking is enabled and we don't yet know the CC frequency, set it
    // from the current tuner so return-to-CC and SM logic have an anchor.
    if (opts->trunk_enable == 1 && opts->trunk_is_tuned == 0 && state->trunk_cc_freq == 0) {
        long int ccfreq = 0;
        if (opts->use_rigctl == 1) {
            ccfreq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
        } else if (opts->audio_in_type == AUDIO_IN_RTL) {
#ifdef USE_RTLSDR
            ccfreq = (long int)opts->rtlsdr_center_freq;
#endif
        }
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
        }
    }
}

//function for handling Control Signalling PDUs (CSBK, MBC) messages
void
dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t CRCCorrect,
          uint32_t IrrecoverableErrors) {
    if (opts == NULL || state == NULL || cs_pdu_bits == NULL || cs_pdu == NULL) {
        return;
    }

    int csbk_lb = 0;
    int csbk_pf = 0;
    int csbk_o = 0;
    int csbk_fid = 0;

    csbk_lb = ((cs_pdu[0] & 0x80) >> 7);
    csbk_pf = ((cs_pdu[0] & 0x40) >> 6);
    csbk_o = cs_pdu[0] & 0x3F;
    csbk_fid = cs_pdu[1]; //feature set id
    UNUSED(csbk_lb);

    csbk_pf = dmr_cspdu_apply_protect_flag_checks(IrrecoverableErrors, csbk_o, csbk_fid, csbk_pf);

    if (IrrecoverableErrors == 0 && CRCCorrect == 1) {
        //clear stale Active Channel messages here
        if (((time(NULL) - state->last_active_time) > 3) && ((time(NULL) - state->last_vc_sync_time) > 3)) {
            DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
        }

        //update time to prevent random 'Control Channel Signal Lost' hopping
        //in the middle of voice call on current Control Channel (con+ and t3)
        dsd_mark_cc_sync(state);

        dmr_cspdu_init_cc_anchor(opts, state);

        dmr_cspdu_handle_pf0(opts, state, cs_pdu_bits, cs_pdu, csbk_pf, csbk_o, csbk_fid);
        dmr_cspdu_handle_cap_plus(opts, state, cs_pdu_bits, cs_pdu, csbk_o, csbk_fid);
        dmr_cspdu_handle_con_plus(opts, state, cs_pdu_bits, cs_pdu, csbk_o, csbk_fid);
        dmr_cspdu_handle_xpt(opts, state, cs_pdu_bits, cs_pdu, csbk_o, csbk_fid);
        dmr_cspdu_handle_moto_unknown(cs_pdu, csbk_o, csbk_fid);
    }
    // Relaxed CC heartbeat: when CRC fails (e.g., RAS/vendor variants), allow
    // a last_cc_sync_time refresh to prevent premature CC hunts if configured.
    // This does not process the PDU further — it only keeps the CC timer warm.
    else if (opts->dmr_crc_relaxed_default) {
        state->last_cc_sync_time = time(NULL);
        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

//translate special gateway identifier addresses
typedef struct {
    uint32_t id;
    const char* label;
} dmr_gateway_id_entry;

static const dmr_gateway_id_entry k_dmr_gateway_ids[] = {
    {0xFFFEC0U, "PSTNI; "},    {0xFFFEC1U, "PABXI; "},    {0xFFFEC2U, "LINEI; "},   {0xFFFEC3U, "IPI; "},
    {0xFFFEC4U, "SUPLI; "},    {0xFFFEC5U, "SDMI; "},     {0xFFFEC6U, "REGI; "},    {0xFFFEC7U, "MSI; "},
    {0xFFFEC8U, "RESERVED; "}, {0xFFFEC9U, "DIVERTI; "},  {0xFFFECAU, "TSI; "},     {0xFFFECBU, "DISPATI; "},
    {0xFFFECCU, "STUNI; "},    {0xFFFECDU, "AUTHI; "},    {0xFFFECEU, "GPI; "},     {0xFFFECFU, "KILLI; "},
    {0xFFFED0U, "PSTNDI; "},   {0xFFFED1U, "PABXDI; "},   {0xFFFED2U, "LINEDI; "},  {0xFFFED3U, "DISPATDI; "},
    {0xFFFED4U, "ALLMSI; "},   {0xFFFED5U, "IPDI; "},     {0xFFFED6U, "DGNAI; "},   {0xFFFED7U, "TATTSI; "},
    {0xFFFFFDU, "ALLMSIDL; "}, {0xFFFFFEU, "ALLMSIDZ; "}, {0xFFFFFFU, "ALLMSID; "},
};

static const char*
dmr_gateway_label_for_id(uint32_t id) {
    size_t count = sizeof(k_dmr_gateway_ids) / sizeof(k_dmr_gateway_ids[0]);
    for (size_t i = 0; i < count; i++) {
        if (k_dmr_gateway_ids[i].id == id) {
            return k_dmr_gateway_ids[i].label;
        }
    }
    return NULL;
}

static void
dmr_gateway_identifier(uint32_t source, uint32_t target) {
    const uint32_t ids[2] = {source, target};

    for (size_t i = 0; i < 2; i++) {
        const char* label = dmr_gateway_label_for_id(ids[i]);
        if (label) {
            DSD_FPRINTF(stderr, "%s", label);
        }
    }

    //NOTE: Observed address values of 64250, or 0xFAFA have been observed
    //on some Moto Tier 2 and Cap+ Systems, and 0xFAFAFA has been observed
    //on some Moto Tier 3 (CapMax) Systems, unsure if these are unique to that
    //manufacturer, or not, usually associated with Data Headers and PDU Messages
}

static void
dmr_syscode_decode_model(uint8_t model, uint8_t* cs_pdu_bits, uint16_t* net, uint16_t* site, uint16_t* site_bits,
                         char* model_str, size_t model_str_sz) {
    if (!net || !site || !site_bits || !model_str || model_str_sz == 0) {
        return;
    }

    *net = 0;
    *site = 0;
    *site_bits = 0;
    DSD_SPRINTF(model_str, "%s", " ");

    switch (model) {
        case 0:
            *net = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[42], 9);
            *site = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[51], 3);
            *site_bits = 3;
            DSD_SPRINTF(model_str, "%s", "Tiny");
            break;
        case 1:
            *net = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[42], 7);
            *site = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[49], 5);
            *site_bits = 5;
            DSD_SPRINTF(model_str, "%s", "Small");
            break;
        case 2:
            *net = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[42], 4);
            *site = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[46], 8);
            *site_bits = 8;
            DSD_SPRINTF(model_str, "%s", "Large");
            break;
        default:
            *net = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[42], 2);
            *site = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[44], 10);
            *site_bits = 10;
            DSD_SPRINTF(model_str, "%s", "Huge");
            break;
    }
}

static void
dmr_syscode_set_partition_label(uint8_t par, char* par_str, size_t par_str_sz) {
    if (!par_str || par_str_sz == 0) {
        return;
    }
    DSD_SPRINTF(par_str, "%s", "Res");
    if (par == 1) {
        DSD_SPRINTF(par_str, "%s", "A");
    } else if (par == 2) {
        DSD_SPRINTF(par_str, "%s", "B");
    } else if (par == 3) {
        DSD_SPRINTF(par_str, "%s", "AB");
    }
}

static uint16_t
dmr_syscode_effective_split_n(dsd_opts* opts, dsd_state* state, int csbk_fid, uint16_t site_bits, uint8_t* is_capmax) {
    uint16_t n = 0;

    if (is_capmax) {
        *is_capmax = 0;
    }
    if (csbk_fid == 0x10) {
        if (is_capmax) {
            *is_capmax = 1;
        }
        if (opts->dmr_dmrla_is_set == 0) {
            opts->dmr_dmrla_is_set = 1;
            opts->dmr_dmrla_n = 0;
        }
        DSD_SPRINTF(state->dmr_branding, "%s", "Motorola");
    }
    if (opts->dmr_dmrla_is_set == 1) {
        n = opts->dmr_dmrla_n;
    }
    if (n > site_bits) {
        n = site_bits;
    }
    return n;
}

static void
dmr_syscode_print_type0(const dsd_opts* opts, uint8_t* cs_pdu_bits, const char* model_str, uint16_t net, uint16_t site,
                        uint16_t n, uint16_t sub_mask, const char* par_str, uint16_t syscode, uint8_t is_capmax) {
    uint8_t reserved = cs_pdu_bits[16];
    uint8_t tsccas = cs_pdu_bits[17];
    uint8_t sync = cs_pdu_bits[18];
    uint8_t version = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[19], 3);
    uint8_t offset = cs_pdu_bits[22];
    uint8_t active = cs_pdu_bits[23];
    uint8_t mask = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[24], 5);
    uint8_t sf = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[29], 2);
    uint8_t nrandwait = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[31], 4);
    uint8_t regreq = cs_pdu_bits[35];
    uint8_t backoff = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[36], 4);
    uint32_t target = (uint32_t)ConvertBitIntoBytes(&cs_pdu_bits[56], 24);

    if (n != 0) {
        DSD_FPRINTF(stderr, " C_ALOHA_SYS_PARMS: %s; Net ID: %d; Site ID: %d.%d; Cat: %s;", model_str, net, (site >> n),
                    (site & sub_mask), par_str);
    } else {
        DSD_FPRINTF(stderr, " C_ALOHA_SYS_PARMS: %s; Net ID: %d; Site ID: %d;", model_str, net, site);
    }
    DSD_FPRINTF(stderr, " SYS: %04X;", syscode);
    if (is_capmax) {
        DSD_FPRINTF(stderr, " Capacity Max");
    }
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    if (reserved) {
        DSD_FPRINTF(stderr, " Res: %04X;", reserved);
    }
    if (tsccas) {
        DSD_FPRINTF(stderr, " TSCCAS;");
    }
    if (sync) {
        DSD_FPRINTF(stderr, " Sync;");
    }
    DSD_FPRINTF(stderr, " Ver: %d;", version);
    if (offset) {
        DSD_FPRINTF(stderr, " Offset;");
    }
    if (active) {
        DSD_FPRINTF(stderr, " Active Connection;");
    }
    DSD_FPRINTF(stderr, " SF: %d;", sf);
    DSD_FPRINTF(stderr, " NR: %X;", nrandwait);
    if (regreq) {
        DSD_FPRINTF(stderr, " Reg Required;");
    }
    DSD_FPRINTF(stderr, " Backoff: %X;", backoff);
    if (mask) {
        DSD_FPRINTF(stderr, " Mask: %02X;", mask);
    }
    if (target) {
        DSD_FPRINTF(stderr, " MS: %d; ", target);
    }
    dmr_gateway_identifier(0, target);
}

static void
dmr_syscode_print_type1(const char* model_str, uint16_t net, uint16_t site, uint16_t n, uint16_t sub_mask,
                        uint16_t syscode) {
    if (n != 0) {
        DSD_FPRINTF(stderr, " %s; Net ID: %d; Site ID: %d.%d;", model_str, net, (site >> n), (site & sub_mask));
    } else {
        DSD_FPRINTF(stderr, " %s; Net ID: %d; Site ID: %d;", model_str, net, site);
    }
    DSD_FPRINTF(stderr, " SYS: %04X;", syscode);
}

static void
dmr_decode_syscode(dsd_opts* opts, dsd_state* state, uint8_t* cs_pdu_bits, int csbk_fid, int type) {
    uint8_t bpbits1[14];
    uint16_t syscode;
    uint8_t model;
    uint16_t net = 0;
    uint16_t site = 0;
    uint16_t site_bits = 0;
    uint16_t n = 0;
    uint16_t sub_mask = 0;
    uint8_t par;
    uint8_t is_capmax = 0;
    char model_str[8];
    char par_str[8];

    for (int i = 0; i < 14; i++) {
        bpbits1[i] = cs_pdu_bits[21 + i];
    }
    if (type != 0) {
        for (int i = 0; i < 14; i++) {
            cs_pdu_bits[40 + i] = bpbits1[i];
        }
    }

    syscode = (uint16_t)ConvertBitIntoBytes(&cs_pdu_bits[40], 14);
    if (type == 0) {
        state->dmr_t3_syscode = syscode;
    }

    model = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[40], 2);
    dmr_syscode_decode_model(model, cs_pdu_bits, &net, &site, &site_bits, model_str, sizeof(model_str));

    n = dmr_syscode_effective_split_n(opts, state, csbk_fid, site_bits, &is_capmax);
    sub_mask = (n == 0) ? 0U : (uint16_t)((1U << n) - 1U);
    par = (uint8_t)ConvertBitIntoBytes(&cs_pdu_bits[54], 2);
    dmr_syscode_set_partition_label(par, par_str, sizeof(par_str));

    if (type == 0) {
        dmr_syscode_print_type0(opts, cs_pdu_bits, model_str, net, site, n, sub_mask, par_str, syscode, is_capmax);
        if (n != 0) {
            DSD_SPRINTF(state->dmr_site_parms, "TIII %s:%d-%d.%d;%04X; ", model_str, net, (site >> n),
                        (site & sub_mask), syscode);
        } else {
            DSD_SPRINTF(state->dmr_site_parms, "TIII %s:%d-%d;%04X; ", model_str, net, site, syscode);
        }
    } else if (type == 1) {
        dmr_syscode_print_type1(model_str, net, site, n, sub_mask, syscode);
    }
}
