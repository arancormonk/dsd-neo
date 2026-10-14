// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression: a P25 Phase 1 TSDU can carry multiple independent TSBKs.
 * processTSBK() must dispatch each CRC-valid TSBK block until the last-block
 * bit, not majority-vote the blocks as if they were repeated copies.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_callsign.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_mfid90_utils.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// CRC-valid reference block for 00 00 00 10 0A 11 11 00 01 01 AE 8E.
static const uint8_t k_group_grant_1111_dibits[98] = {
    0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 2, 1, 3, 2, 3, 2, 0, 2, 3, 2, 3, 1, 2, 2, 2, 1, 0, 2, 0, 2, 0, 2, 3,
    0, 0, 2, 3, 0, 3, 0, 0, 2, 0, 2, 0, 2, 2, 2, 2, 1, 0, 2, 0, 2, 0, 2, 3, 2, 0, 1, 3, 2, 3, 2, 0, 2,
    0, 2, 0, 2, 1, 0, 3, 3, 0, 2, 0, 2, 0, 2, 0, 2, 2, 2, 3, 0, 3, 0, 0, 2, 3, 0, 3, 0, 1, 2, 1, 2,
};

// CRC-valid reference block for 80 00 00 10 0A 22 22 00 02 02 7A 83.
static const uint8_t k_group_grant_2222_dibits[98] = {
    0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 2, 1, 2, 1, 2, 1, 0, 2, 2, 1, 1, 3, 2, 2, 1, 1, 2, 1, 0, 2, 0, 2, 3,
    0, 0, 2, 0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 3, 2, 1, 0, 2, 0, 2, 0, 2, 3, 2, 0, 1, 2, 1, 2, 1, 0, 2,
    0, 2, 0, 2, 1, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 2, 2, 0, 1, 0, 1, 0, 2, 0, 1, 0, 1, 2, 2, 3, 3,
};

// CRC-valid reference block for BB 00 00 AB CD E1 23 81 23 00 51 97.
static const uint8_t k_network_status_dibits[98] = {
    0, 1, 1, 1, 0, 2, 0, 1, 2, 0, 0, 3, 3, 2, 1, 2, 3, 2, 1, 1, 3, 0, 3, 1, 1, 1, 1, 0, 0, 2, 0, 2, 2,
    2, 1, 1, 1, 2, 0, 1, 2, 1, 0, 1, 0, 2, 0, 0, 1, 3, 1, 2, 0, 2, 0, 2, 2, 2, 3, 3, 2, 1, 2, 1, 0, 2,
    2, 1, 0, 2, 3, 2, 0, 0, 1, 0, 0, 2, 0, 2, 1, 0, 2, 3, 3, 0, 3, 3, 3, 0, 3, 3, 0, 2, 3, 0, 0, 3,
};

static uint8_t g_stream[3 * 101];
static int g_stream_len = 0;
static int g_stream_pos = 0;
static int g_mac_count = 0;
static int g_mac_group[3] = {0};
static int g_mac_source[3] = {0};
static int g_status_count = 0;
static long g_channel_freq = 0;
/* The read a Signal search's yield (or a stalled live read) interrupts: getSymbol() records the interruption there and
   stands erasures in for every symbol after it; -1 for none. The stub goes on handing out the block's real dibits, the
   worst case of erasures the list decoder fits to a CRC. */
static int g_interrupt_at = -1;
/* Signal search evidence the decoded blocks reported, and the protocol named last. */
static int g_evidence = 0;
static char g_evidence_protocol[16];

static void
capture_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source, const char* access) {
    (void)state;
    (void)target;
    (void)source;
    (void)access;
    g_evidence++;
    DSD_SNPRINTF(g_evidence_protocol, sizeof(g_evidence_protocol), "%s", protocol);
}

static void
append_tsbk_stream_block(const uint8_t dibits[98], int* skipdibit) {
    int valid_idx = 0;
    for (int i = 0; i < 101; i++) {
        if ((*skipdibit / 36) == 0) {
            g_stream[g_stream_len++] = dibits[valid_idx++];
        } else {
            g_stream[g_stream_len++] = 0;
            *skipdibit = 0;
        }
        (*skipdibit)++;
    }
}

static void
reset_decode_counters(void) {
    g_mac_count = 0;
    g_mac_group[0] = 0;
    g_mac_group[1] = 0;
    g_mac_group[2] = 0;
    g_mac_source[0] = 0;
    g_mac_source[1] = 0;
    g_mac_source[2] = 0;
    g_status_count = 0;
}

static void
build_two_block_stream(void) {
    int skipdibit = 36 - 14;

    DSD_MEMSET(g_stream, 0, sizeof(g_stream));
    g_stream_len = 0;
    g_stream_pos = 0;

    append_tsbk_stream_block(k_group_grant_1111_dibits, &skipdibit);
    append_tsbk_stream_block(k_group_grant_2222_dibits, &skipdibit);
}

static void
build_network_status_stream(void) {
    int skipdibit = 36 - 14;

    DSD_MEMSET(g_stream, 0, sizeof(g_stream));
    g_stream_len = 0;
    g_stream_pos = 0;

    append_tsbk_stream_block(k_network_status_dibits, &skipdibit);
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    if (g_stream_pos == g_interrupt_at && state != NULL) {
        state->input_interrupted = 1;
    }
    uint8_t dibit = 0;
    if (g_stream_pos < g_stream_len) {
        dibit = g_stream[g_stream_pos++];
    }
    if (out_soft) {
        out_soft->reliability = 255;
        out_soft->llr[0] = (dibit & 2U) ? 220 : -220;
        out_soft->llr[1] = (dibit & 1U) ? 220 : -220;
    }
    return dibit;
}

void
p25_status_accum_ensure_started(dsd_state* state) {
    (void)state;
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    (void)dibit_value;
    g_status_count++;
}

void
p25_status_accum_classify(dsd_state* state) {
    (void)state;
}

void
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                 unsigned long long int mac[24]) {
    (void)opts;
    (void)state;
    (void)type;
    (void)pdu_type;
    if (g_mac_count < 3) {
        g_mac_group[g_mac_count] = (int)((mac[5] << 8) | mac[6]);
        g_mac_source[g_mac_count] = (int)((mac[7] << 16) | (mac[8] << 8) | mac[9]);
    }
    g_mac_count++;
}

long int
process_channel_to_freq(const dsd_opts* opts, dsd_state* state, int channel) {
    (void)opts;
    (void)state;
    (void)channel;
    return g_channel_freq;
}

void
p25_format_chan_suffix(const dsd_state* state, uint16_t chan, int slot_hint, char* out, size_t outsz) {
    (void)state;
    (void)chan;
    (void)slot_hint;
    if (out && outsz > 0) {
        out[0] = '\0';
    }
}

void
p25_sm_seed_cc_from_current_tuner_if_unknown(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static p25_sm_ctx_t g_sm_ctx;

p25_sm_ctx_t*
p25_sm_get_ctx(void) {
    return &g_sm_ctx;
}

void
p25_sm_event(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    (void)ctx;
    (void)opts;
    (void)state;
    (void)ev;
}

int
dsd_tg_policy_evaluate_private_call(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                    int encrypted, int data_call, dsd_tg_policy_decision* out) {
    (void)state;
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->target_id = dst;
    out->source_id = src;
    out->encrypted = encrypted;
    out->data_call = data_call;
    out->tune_allowed = 1;
    out->audio_allowed = 1;
    out->record_allowed = 1;
    out->stream_allowed = 1;
    out->match = DSD_TG_POLICY_MATCH_NONE;
    if (opts && opts->trunk_tune_private_calls == 0) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED;
    }
    if (opts && data_call && opts->trunk_tune_data_calls == 0) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_DATA_DISABLED;
    }
    if (opts && encrypted && opts->trunk_tune_enc_calls == 0) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED;
    }
    if (opts && opts->trunk_use_allow_list == 1) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_ALLOWLIST;
    }
    return 0;
}

void
p25_sm_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    (void)ctx;
    (void)opts;
    (void)state;
    (void)reason;
}

void
p25_cc_record_neighbor_frequencies(const dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    (void)opts;
    (void)state;
    (void)freqs;
    (void)count;
}

void
p25_confirm_idens_for_current_site(dsd_state* state) {
    (void)state;
}

int
p25_update_system_identity(dsd_state* state, unsigned long long wacn, unsigned long long sysid) {
    if (!state || (wacn == 0 && sysid == 0)) {
        return 0;
    }
    if ((state->p2_wacn != 0 || state->p2_sysid != 0) && (state->p2_wacn != wacn || state->p2_sysid != sysid)) {
        DSD_MEMSET(state->p25_iden_fdma, 0, sizeof(state->p25_iden_fdma));
        DSD_MEMSET(state->p25_iden_tdma, 0, sizeof(state->p25_iden_tdma));
        DSD_MEMSET(state->p25_chan_tdma_explicit, 0, sizeof(state->p25_chan_tdma_explicit));
        DSD_MEMSET(state->p25_pending_announcements, 0, sizeof(state->p25_pending_announcements));
        state->p25_pending_announcement_count = 0;
    }
    state->p2_wacn = wacn;
    state->p2_sysid = sysid;
    return 1;
}

void
p25_store_site_lra(dsd_state* state, uint8_t lra) {
    (void)state;
    (void)lra;
}

void
p25_wacn_sysid_to_callsign(uint32_t wacn, uint16_t sysid, char out[7]) {
    (void)wacn;
    (void)sysid;
    if (out) {
        out[0] = '\0';
    }
}

int
p25_mfid90_base_station_id_decode(const uint8_t tsbk_byte[12], char* cwid, size_t cwid_size, uint16_t* channel) {
    (void)tsbk_byte;
    if (cwid && cwid_size > 0) {
        cwid[0] = '\0';
    }
    if (channel) {
        *channel = 0;
    }
    return 0;
}

void
p25_patch_add_wgid(dsd_state* state, int sg, int wgid) {
    (void)state;
    (void)sg;
    (void)wgid;
}

void
p25_patch_add_wuid(dsd_state* state, int sg, uint32_t wuid) {
    (void)state;
    (void)sg;
    (void)wuid;
}

void
p25_patch_remove_wgid(dsd_state* state, int sg, int wgid) {
    (void)state;
    (void)sg;
    (void)wgid;
}

void
p25_patch_clear_sg(dsd_state* state, int sg) {
    (void)state;
    (void)sg;
}

int
p25_patch_prepare_grg_update(dsd_state* state, int sg, int is_patch, int active, int ssn) {
    (void)state;
    (void)sg;
    (void)is_patch;
    (void)ssn;
    return active ? 1 : 0;
}

void
p25_patch_set_kas(dsd_state* state, int sg, int kas, int algid, int ssn) {
    (void)state;
    (void)sg;
    (void)kas;
    (void)algid;
    (void)ssn;
}

void
p25_patch_update(dsd_state* state, int sg, int is_patch, int active) {
    (void)state;
    (void)sg;
    (void)is_patch;
    (void)active;
}

void
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
dsd_rtl_stream_metrics_hook_p25p1_ber_update(int ok_delta, int err_delta) {
    (void)ok_delta;
    (void)err_delta;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    dsd_decode_clock_use_test(1000000000ULL);
    dsd_signal_search_evidence_set(capture_evidence);
    build_two_block_stream();
    reset_decode_counters();

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    // Issue #657: a control frame leaves the user's slot preference as set (it used to reset it to Auto).
    opts.slot_preference = 1;

    processTSBK(&opts, &state);

    int rc = 0;
    rc |= expect_eq_int("tsbk keeps the slot preference", opts.slot_preference, 1);
    rc |= expect_eq_int("mac dispatch count", g_mac_count, 2);
    rc |= expect_eq_int("first group", g_mac_group[0], 0x1111);
    rc |= expect_eq_int("second group", g_mac_group[1], 0x2222);
    rc |= expect_eq_int("first source", g_mac_source[0], 0x000101);
    rc |= expect_eq_int("second source", g_mac_source[1], 0x000202);
    rc |= expect_eq_int("stream consumed through last block", g_stream_pos, 202);
    rc |= expect_eq_int("status symbols collected", g_status_count, 6);
    rc |= expect_eq_int("fec ok count", (int)state.p25_p1_fec_ok, 2);
    rc |= expect_eq_int("fec err count", (int)state.p25_p1_fec_err, 0);
    /* A block whose CRC passes is the P25 evidence a Signal search confirms a control channel by: its NID alone is
       not, since a list-decoded BCH takes several percent of random words. */
    rc |= expect_eq_int("each crc-valid block is search evidence", g_evidence, 2);
    rc |= expect_eq_int("search evidence names p25",
                        g_evidence_protocol[0] == 'P' && g_evidence_protocol[1] == '2' && g_evidence_protocol[2] == '5'
                            && !g_evidence_protocol[3],
                        1);

    /* A block whose CRC fails is no evidence. */
    build_two_block_stream();
    for (int i = 101 + 20; i < 101 + 80; i++) {
        g_stream[i] = (uint8_t)(3 - g_stream[i]);
    }
    reset_decode_counters();
    g_evidence = 0;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    processTSBK(&opts, &state);
    rc |= expect_eq_int("corrupt block fails its crc", state.p25_p1_fec_err > 0, 1);
    rc |= expect_eq_int("only crc-valid blocks are search evidence", g_evidence, (int)state.p25_p1_fec_ok);

    build_network_status_stream();
    reset_decode_counters();
    g_channel_freq = 0;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.p25_cc_is_tdma = 2;

    processTSBK(&opts, &state);

    rc |= expect_eq_int("net-sts missing-iden stores wacn", (int)state.p2_wacn, 0xABCDE);
    rc |= expect_eq_int("net-sts missing-iden stores sysid", (int)state.p2_sysid, 0x123);
    rc |= expect_eq_int("net-sts missing-iden marks p1 cc fdma", state.p25_cc_is_tdma, 0);
    rc |= expect_eq_int("net-sts missing-iden leaves p25 cc empty", (int)state.p25_cc_freq, 0);
    rc |= expect_eq_int("net-sts missing-iden leaves trunk cc empty", (int)state.trunk_cc_freq, 0);

    build_network_status_stream();
    reset_decode_counters();
    g_channel_freq = 863812500;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_is_tuned = 1;
    state.p25_cc_freq = 851000000;
    state.trunk_cc_freq = 851000000;
    state.p25_cc_is_tdma = 1;

    processTSBK(&opts, &state);

    rc |= expect_eq_int("net-sts rejected voice preserves p25 cc", (int)state.p25_cc_freq, 851000000);
    rc |= expect_eq_int("net-sts rejected voice preserves trunk cc", (int)state.trunk_cc_freq, 851000000);
    rc |= expect_eq_int("net-sts rejected voice preserves tdma cc hint", state.p25_cc_is_tdma, 1);
    rc |= expect_eq_int("net-sts rejected voice skips wacn", (int)state.p2_wacn, 0);
    rc |= expect_eq_int("net-sts rejected voice skips sysid", (int)state.p2_sysid, 0);

    /* A Signal search that leaves the TSDU (its visit ended, or a command came), or a live read that gives up on a
       stalled input, cuts it: from the cut every symbol is an erasure getSymbol() stood in. Neither the block it cut
       nor any after it counts or dispatches, whatever its CRC says; the blocks read whole before it stand. */
    static const int cuts[3] = {150, 100, 0};
    static const int grants_before[3] = {1, 0, 0};
    for (int c = 0; c < 3; c++) {
        build_two_block_stream();
        reset_decode_counters();
        g_channel_freq = 0;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        g_interrupt_at = cuts[c];
        g_evidence = 0;

        processTSBK(&opts, &state);

        rc |= expect_eq_int("cut tsdu is interrupted", state.input_interrupted, 1);
        rc |= expect_eq_int("cut tsdu dispatches only the blocks before the cut", g_mac_count, grants_before[c]);
        rc |=
            expect_eq_int("cut tsdu counts only the blocks before the cut", (int)state.p25_p1_fec_ok, grants_before[c]);
        rc |= expect_eq_int("cut tsdu counts no fec error", (int)state.p25_p1_fec_err, 0);
        rc |= expect_eq_int("cut tsdu reports only the blocks before the cut", g_evidence, grants_before[c]);
    }
    g_interrupt_at = -1;
    dsd_decode_clock_use_system();
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
