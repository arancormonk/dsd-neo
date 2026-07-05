// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 P1 TSBK handler tests: verify deterministic vendor handler side effects
 * without depending on live TSBK dibit capture or terminal text formatting.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_12.h>
#include <dsd-neo/protocol/p25/p25_callsign.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_mfid90_utils.h"

static int g_add_wgid_count;
static int g_add_wuid_count;
static int g_remove_wgid_count;
static int g_update_count;
static int g_kas_count;
static int g_seed_count;
static int g_grant_count;
static int g_mac_count;
static int g_last_sg;
static int g_last_wgid;
static uint32_t g_last_wuid;
static int g_last_update_patch;
static int g_last_update_active;
static int g_last_key;
static int g_last_ssn;
static int g_last_grant_channel;
static int g_last_grant_tg;
static int g_last_grant_src;
static int g_neighbor_update_count;
static int g_neighbor_update_last_count;
static long g_neighbor_update_last_freq;
static int g_confirm_idens_count;
static long int g_channel_freq;
static int g_soft_llr_count;
static int g_soft_llr_list_count;
static int g_crc_call_count;
static int g_crc_accept_call;
static int g_candidate_count;

enum { TEST_TSBK_BYTES_PER_BLOCK = 12 };

static uint8_t g_candidate_bytes[P25_12_MAX_CANDIDATES][TEST_TSBK_BYTES_PER_BLOCK];
static uint8_t g_soft_llr_bytes[TEST_TSBK_BYTES_PER_BLOCK];

uint64_t
dsd_time_monotonic_ns(void) {
    return 41000000000ULL;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_update(dsd_state* state, int sgid, int is_patch, int active) {
    (void)state;
    g_update_count++;
    g_last_sg = sgid;
    g_last_update_patch = is_patch;
    g_last_update_active = active;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_add_wgid(dsd_state* state, int sgid, int wgid) {
    (void)state;
    g_add_wgid_count++;
    g_last_sg = sgid;
    g_last_wgid = wgid;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_add_wuid(dsd_state* state, int sgid, uint32_t wuid) {
    (void)state;
    g_add_wuid_count++;
    g_last_sg = sgid;
    g_last_wuid = wuid;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_remove_wgid(dsd_state* state, int sgid, int wgid) {
    (void)state;
    g_remove_wgid_count++;
    g_last_sg = sgid;
    g_last_wgid = wgid;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_remove_wuid(dsd_state* state, int sgid, uint32_t wuid) {
    (void)state;
    (void)sgid;
    (void)wuid;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_set_kas(dsd_state* state, int sgid, int key, int alg, int ssn) {
    (void)state;
    (void)alg;
    g_kas_count++;
    g_last_sg = sgid;
    g_last_key = key;
    g_last_ssn = ssn;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_channel_to_freq(const dsd_opts* opts, dsd_state* state, int channel) {
    (void)opts;
    (void)state;
    (void)channel;
    return g_channel_freq;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_format_chan_suffix(const dsd_state* state, uint16_t chan, int slot_hint, char* out, size_t outsz) {
    (void)state;
    (void)slot_hint;
    DSD_SNPRINTF(out, outsz, "/%04X", chan);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_seed_cc_from_current_tuner_if_unknown(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_seed_count++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    (void)opts;
    (void)state;
    (void)svc_bits;
    g_grant_count++;
    g_last_grant_channel = channel;
    g_last_grant_tg = tg;
    g_last_grant_src = src;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    (void)opts;
    (void)state;
    g_neighbor_update_count++;
    g_neighbor_update_last_count = count;
    g_neighbor_update_last_freq = (freqs != NULL && count > 0) ? freqs[0] : 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_confirm_idens_for_current_site(dsd_state* state) {
    (void)state;
    g_confirm_idens_count++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_store_site_lra(dsd_state* state, uint8_t lra) {
    (void)state;
    (void)lra;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int mac[24]) {
    (void)opts;
    (void)state;
    (void)type;
    (void)mac;
    g_mac_count++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_wacn_sysid_to_callsign(uint32_t wacn, uint16_t sysid, char callsign[7]) {
    (void)wacn;
    (void)sysid;
    DSD_SNPRINTF(callsign, 7, "TST123");
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_mfid90_base_station_id_decode(const uint8_t tsbk_byte[12], char* cwid, size_t cwid_size, uint16_t* channel) {
    (void)tsbk_byte;
    DSD_SNPRINTF(cwid, cwid_size, "%s", "CWID");
    *channel = 0x1234;
    return 4;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    if (out_soft != NULL) {
        out_soft->llr[0] = 100;
        out_soft->llr[1] = -100;
    }
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_status_accum_ensure_started(dsd_state* state) {
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    (void)dibit_value;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_status_accum_classify(dsd_state* state, const dsd_opts* opts) {
    (void)state;
    (void)opts;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_12_soft_llr(const uint8_t* input, const int16_t* bit_llr196, uint8_t treturn[12]) {
    (void)input;
    (void)bit_llr196;
    g_soft_llr_count++;
    DSD_MEMCPY(treturn, g_soft_llr_bytes, TEST_TSBK_BYTES_PER_BLOCK);
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_12_soft_llr_list(const uint8_t* input, const int16_t* bit_llr196, p25_12_candidate_t* candidates,
                     int max_candidates) {
    (void)input;
    (void)bit_llr196;
    g_soft_llr_list_count++;
    int count = g_candidate_count;
    if (count > max_candidates) {
        count = max_candidates;
    }
    for (int i = 0; i < count; i++) {
        DSD_MEMCPY(candidates[i].bytes, g_candidate_bytes[i], TEST_TSBK_BYTES_PER_BLOCK);
    }
    return count;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
crc16_lb_bridge(const int* payload, int len) {
    (void)payload;
    (void)len;
    g_crc_call_count++;
    if (g_crc_accept_call > 0) {
        return (g_crc_call_count == g_crc_accept_call) ? 0 : 1;
    }
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

#include "../../../src/protocol/p25/phase1/p25p1_tsbk.c"

static void
reset_calls(void) {
    g_add_wgid_count = 0;
    g_add_wuid_count = 0;
    g_remove_wgid_count = 0;
    g_update_count = 0;
    g_kas_count = 0;
    g_seed_count = 0;
    g_grant_count = 0;
    g_mac_count = 0;
    g_last_sg = 0;
    g_last_wgid = 0;
    g_last_wuid = 0;
    g_last_update_patch = 0;
    g_last_update_active = 0;
    g_last_key = 0;
    g_last_ssn = 0;
    g_last_grant_channel = 0;
    g_last_grant_tg = 0;
    g_last_grant_src = 0;
    g_neighbor_update_count = 0;
    g_neighbor_update_last_count = 0;
    g_neighbor_update_last_freq = 0;
    g_confirm_idens_count = 0;
    g_channel_freq = 0;
    g_soft_llr_count = 0;
    g_soft_llr_list_count = 0;
    g_crc_call_count = 0;
    g_crc_accept_call = 0;
    g_candidate_count = 0;
    DSD_MEMSET(g_candidate_bytes, 0, sizeof(g_candidate_bytes));
    DSD_MEMSET(g_soft_llr_bytes, 0, sizeof(g_soft_llr_bytes));
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_bytes(const char* tag, const uint8_t* got, const uint8_t* want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        DSD_FPRINTF(stderr, "%s: byte mismatch\n", tag);
        return 1;
    }
    return 0;
}

static int
test_crc_candidate_selection_and_fallback(void) {
    uint8_t dibits[TSBK_DIBITS_PER_REP] = {0};
    int16_t llr[TSBK_SOFT_BITS_PER_REP] = {0};
    uint8_t out[TSBK_BYTES_PER_BLOCK] = {0};
    uint8_t want[TSBK_BYTES_PER_BLOCK] = {0};

    reset_calls();
    g_candidate_count = 3;
    g_crc_accept_call = 2;
    for (int c = 0; c < g_candidate_count; c++) {
        for (int i = 0; i < TSBK_BYTES_PER_BLOCK; i++) {
            g_candidate_bytes[c][i] = (uint8_t)((c + 1) * 0x10 + i);
        }
    }
    DSD_MEMCPY(want, g_candidate_bytes[1], sizeof(want));
    tsbk_decode_repetition_bytes(dibits, llr, out);

    int rc = 0;
    rc |= expect_int("candidate list decoder used", g_soft_llr_list_count, 1);
    rc |= expect_int("candidate crc attempts", g_crc_call_count, 2);
    rc |= expect_int("fallback decoder skipped", g_soft_llr_count, 0);
    rc |= expect_bytes("selected crc-clean candidate", out, want, sizeof(out));

    reset_calls();
    g_candidate_count = 2;
    g_crc_accept_call = 9;
    for (int c = 0; c < g_candidate_count; c++) {
        for (int i = 0; i < TSBK_BYTES_PER_BLOCK; i++) {
            g_candidate_bytes[c][i] = (uint8_t)(0x80 + (c * 0x10) + i);
        }
    }
    DSD_MEMCPY(want, g_candidate_bytes[0], sizeof(want));
    tsbk_decode_repetition_bytes(dibits, llr, out);
    rc |= expect_int("all candidates tried before default", g_crc_call_count, 2);
    rc |= expect_bytes("default candidate retained", out, want, sizeof(out));

    reset_calls();
    for (int i = 0; i < TSBK_BYTES_PER_BLOCK; i++) {
        g_soft_llr_bytes[i] = (uint8_t)(0xA0 + i);
        want[i] = g_soft_llr_bytes[i];
    }
    tsbk_decode_repetition_bytes(dibits, llr, out);
    rc |= expect_int("fallback list called", g_soft_llr_list_count, 1);
    rc |= expect_int("fallback decoder called", g_soft_llr_count, 1);
    rc |= expect_bytes("fallback bytes copied", out, want, sizeof(out));
    return rc;
}

static int
test_mfid90_regroup_add_delete(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    uint8_t add_tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    add_tsbk[2] = 0x12;
    add_tsbk[3] = 0x34;
    add_tsbk[4] = 0x01;
    add_tsbk[5] = 0x02;
    add_tsbk[8] = 0x03;
    add_tsbk[9] = 0x04;

    reset_calls();
    tsbk_handle_mfid90_regroup_add_del(&state, add_tsbk, 1);
    int rc = 0;
    rc |= expect_int("add wgid count", g_add_wgid_count, 2);
    rc |= expect_int("add last sg", g_last_sg, 0x1234);
    rc |= expect_int("add last wgid", g_last_wgid, 0x0304);
    rc |= expect_int("add update count", g_update_count, 1);
    rc |= expect_int("add update patch", g_last_update_patch, 1);
    rc |= expect_int("add update active", g_last_update_active, 1);

    reset_calls();
    tsbk_handle_mfid90_regroup_add_del(&state, add_tsbk, 0);
    rc |= expect_int("delete wgid count", g_remove_wgid_count, 2);
    rc |= expect_int("delete update count", g_update_count, 0);
    rc |= expect_int("delete last wgid", g_last_wgid, 0x0304);
    return rc;
}

static int
test_mfid_a4_patch_and_simulselect_paths(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    uint8_t tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    tsbk[0] = 0x30;
    tsbk[2] = (uint8_t)((0x3 << 5) | 0x07);
    tsbk[3] = 0x12;
    tsbk[4] = 0x34;
    tsbk[5] = 0xBE;
    tsbk[6] = 0xEF;
    tsbk[7] = 0x01;
    tsbk[8] = 0x02;
    tsbk[9] = 0x03;

    reset_calls();
    tsbk_handle_mfid_a4(&state, tsbk);
    int rc = 0;
    rc |= expect_int("a4 patch adds wgid", g_add_wgid_count, 1);
    rc |= expect_int("a4 patch sg", g_last_sg, 0x1234);
    rc |= expect_int("a4 patch wgid", g_last_wgid, 0x010203);
    rc |= expect_int("a4 patch update count", g_update_count, 1);
    rc |= expect_int("a4 patch is_patch", g_last_update_patch, 1);
    rc |= expect_int("a4 patch active", g_last_update_active, 1);
    rc |= expect_int("a4 patch kas key", g_last_key, 0xBEEF);
    rc |= expect_int("a4 patch kas ssn", g_last_ssn, 7);

    tsbk[2] = (uint8_t)((0x4 << 5) | 0x05);
    reset_calls();
    tsbk_handle_mfid_a4(&state, tsbk);
    rc |= expect_int("a4 simulselect adds wuid", g_add_wuid_count, 1);
    rc |= expect_int("a4 simulselect wuid", (int)g_last_wuid, 0x010203);
    rc |= expect_int("a4 simulselect is_patch", g_last_update_patch, 0);
    rc |= expect_int("a4 simulselect inactive", g_last_update_active, 0);
    rc |= expect_int("a4 simulselect ssn", g_last_ssn, 5);

    tsbk[0] = 0x31;
    reset_calls();
    tsbk_handle_mfid_a4(&state, tsbk);
    rc |= expect_int("a4 non-op ignored", g_add_wgid_count + g_add_wuid_count + g_update_count + g_kas_count, 0);
    return rc;
}

static int
test_mfid90_grant_seeds_trunk_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.p25_trunk = 1;

    uint8_t tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    tsbk[3] = 0x12;
    tsbk[4] = 0x34;
    tsbk[5] = 0x45;
    tsbk[6] = 0x67;
    tsbk[7] = 0x01;
    tsbk[8] = 0x02;
    tsbk[9] = 0x03;

    reset_calls();
    g_channel_freq = 851012500;
    tsbk_handle_mfid90_grant(&opts, &state, tsbk);
    int rc = 0;
    rc |= expect_int("grant seed count", g_seed_count, 1);
    rc |= expect_int("grant count", g_grant_count, 1);
    rc |= expect_int("grant channel", g_last_grant_channel, 0x1234);
    rc |= expect_int("grant tg", g_last_grant_tg, 0x4567);
    rc |= expect_int("grant src", g_last_grant_src, 0x010203);
    rc |= expect_int("active channel set", strstr(state.active_channel[0], "1234/1234") != NULL, 1);

    reset_calls();
    g_channel_freq = 0;
    tsbk_handle_mfid90_grant(&opts, &state, tsbk);
    rc |= expect_int("zero freq skips grant", g_grant_count, 0);
    return rc;
}

static int
test_mfid90_grant_update_trunk_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.p25_trunk = 1;

    uint8_t tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    tsbk[2] = 0x11;
    tsbk[3] = 0x22;
    tsbk[4] = 0x33;
    tsbk[5] = 0x44;
    tsbk[6] = 0x55;
    tsbk[7] = 0x66;
    tsbk[8] = 0x77;
    tsbk[9] = 0x88;

    reset_calls();
    g_channel_freq = 851012500;
    tsbk_handle_mfid90_grant_update(&opts, &state, tsbk);
    int rc = 0;
    rc |= expect_int("grant update seeds twice", g_seed_count, 2);
    rc |= expect_int("grant update count", g_grant_count, 2);
    rc |= expect_int("grant update last channel", g_last_grant_channel, 0x5566);
    rc |= expect_int("grant update last tg", g_last_grant_tg, 0x7788);
    rc |= expect_int("grant update source is zero", g_last_grant_src, 0);
    rc |= expect_int("grant update active channel", strstr(state.active_channel[0], "1122/1122") != NULL, 1);

    tsbk[2] = 0x00;
    tsbk[3] = 0x00;
    reset_calls();
    g_channel_freq = 851012500;
    tsbk_handle_mfid90_grant_update(&opts, &state, tsbk);
    rc |= expect_int("zero first channel skips first grant", g_grant_count, 1);
    rc |= expect_int("zero first channel still grants second", g_last_grant_channel, 0x5566);

    reset_calls();
    g_channel_freq = 0;
    tsbk_handle_mfid90_grant_update(&opts, &state, tsbk);
    rc |= expect_int("zero translated freq skips update grants", g_grant_count, 0);
    return rc;
}

static int
test_network_status_state_policy(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    uint8_t tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    tsbk[3] = 0xAB;
    tsbk[4] = 0xCD;
    tsbk[5] = 0xE1;
    tsbk[6] = 0x23;
    tsbk[7] = 0x45;
    tsbk[8] = 0x67;

    reset_calls();
    state.p25_cc_is_tdma = 1;
    g_channel_freq = 851012500;
    tsbk_handle_network_status(&opts, &state, tsbk);
    int rc = 0;
    rc |= expect_long("accepted p25 cc", state.p25_cc_freq, 851012500);
    rc |= expect_long("accepted trunk cc", state.trunk_cc_freq, 851012500);
    rc |= expect_int("accepted cc is fdma", state.p25_cc_is_tdma, 0);
    rc |= expect_long("accepted wacn", state.p2_wacn, 0xABCDE);
    rc |= expect_int("accepted sysid", state.p2_sysid, 0x123);
    rc |= expect_int("neighbor update count", g_neighbor_update_count, 1);
    rc |= expect_int("neighbor update length", g_neighbor_update_last_count, 1);
    rc |= expect_long("neighbor update freq", g_neighbor_update_last_freq, 851012500);
    rc |= expect_long("lcn zero learned", state.trunk_lcn_freq[0], 851012500);
    rc |= expect_int("iden confirmation", g_confirm_idens_count, 1);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.p25_is_tuned = 1;
    state.p25_cc_freq = 860012500;
    state.trunk_cc_freq = 860012500;
    state.p25_cc_is_tdma = 1;
    state.p2_wacn = 0x11111;
    state.p2_sysid = 0x222;
    reset_calls();
    g_channel_freq = 851012500;
    tsbk_handle_network_status(&opts, &state, tsbk);
    rc |= expect_long("voice tuned preserves p25 cc", state.p25_cc_freq, 860012500);
    rc |= expect_long("voice tuned preserves trunk cc", state.trunk_cc_freq, 860012500);
    rc |= expect_int("voice tuned preserves tdma marker", state.p25_cc_is_tdma, 1);
    rc |= expect_long("voice tuned preserves wacn", state.p2_wacn, 0x11111);
    rc |= expect_int("voice tuned preserves sysid", state.p2_sysid, 0x222);
    rc |= expect_int("voice tuned skips neighbor", g_neighbor_update_count, 0);
    rc |= expect_int("voice tuned skips iden confirmation", g_confirm_idens_count, 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.p25_cc_is_tdma = 1;
    reset_calls();
    g_channel_freq = 0;
    tsbk_handle_network_status(&opts, &state, tsbk);
    rc |= expect_long("invalid channel leaves p25 cc", state.p25_cc_freq, 0);
    rc |= expect_int("invalid channel still records fdma", state.p25_cc_is_tdma, 0);
    rc |= expect_long("invalid channel records wacn", state.p2_wacn, 0xABCDE);
    rc |= expect_int("invalid channel records sysid", state.p2_sysid, 0x123);
    rc |= expect_int("invalid channel skips neighbor", g_neighbor_update_count, 0);
    rc |= expect_int("invalid channel skips iden confirmation", g_confirm_idens_count, 0);
    return rc;
}

static int
test_dispatch_gates_mac_and_vendor_handlers(void) {
    static dsd_opts opts;
    static dsd_state state;
    tsbk_decode_ctx_t ctx;
    unsigned long long pdu[24] = {0};
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));

    int rc = 0;
    reset_calls();
    tsbk_dispatch_message(&opts, &state, &ctx, 1, 0, 0, pdu);
    rc |= expect_int("err skips dispatch", g_mac_count + g_add_wgid_count, 0);

    reset_calls();
    pdu[1] = 0x40;
    tsbk_dispatch_message(&opts, &state, &ctx, 0, 0, 0, pdu);
    rc |= expect_int("standard dispatch mac", g_mac_count, 1);

    reset_calls();
    pdu[1] = 0x7B;
    tsbk_dispatch_message(&opts, &state, &ctx, 0, 0, 0, pdu);
    rc |= expect_int("0x7b pdu suppressed", g_mac_count, 0);

    reset_calls();
    ctx.tsbk_byte[0] = 0x40;
    ctx.tsbk_byte[5] = 0x12;
    ctx.tsbk_byte[6] = 0x34;
    ctx.tsbk_byte[7] = 0x56;
    ctx.tsbk_byte[8] = 0x78;
    ctx.tsbk_byte[9] = 0x90;
    tsbk_dispatch_message(&opts, &state, &ctx, 0, 0, 1, pdu);
    rc |= expect_int("protected isp skips mac", g_mac_count, 0);

    reset_calls();
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    ctx.tsbk_byte[0] = 0x00;
    ctx.tsbk_byte[2] = 0x00;
    ctx.tsbk_byte[3] = 0x55;
    ctx.tsbk_byte[4] = 0x00;
    ctx.tsbk_byte[5] = 0x66;
    tsbk_dispatch_message(&opts, &state, &ctx, 0, 0x90, 0, pdu);
    rc |= expect_int("mfid90 dispatch", g_add_wgid_count, 1);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_crc_candidate_selection_and_fallback();
    rc |= test_mfid90_regroup_add_delete();
    rc |= test_mfid_a4_patch_and_simulselect_paths();
    rc |= test_mfid90_grant_seeds_trunk_state();
    rc |= test_mfid90_grant_update_trunk_dispatch();
    rc |= test_network_status_state_policy();
    rc |= test_dispatch_gates_mac_and_vendor_handlers();
    return rc;
}

// NOLINTEND(bugprone-suspicious-include)
