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
#include "test_support.h"

static int g_add_wgid_count;
static int g_add_wuid_count;
static int g_remove_wgid_count;
static int g_update_count;
static int g_kas_count;
static int g_clear_count;
static int g_seed_count;
static int g_grant_count;
static int g_mac_count;
static int g_last_sg;
static int g_last_wgid;
static uint32_t g_last_wuid;
static int g_last_update_patch;
static int g_last_update_active;
static int g_last_key;
static int g_last_alg;
static int g_last_ssn;
static int g_last_grant_channel;
static int g_last_grant_svc;
static int g_last_grant_tg;
static int g_last_grant_src;
static int g_neighbor_update_count;
static int g_neighbor_update_last_count;
static long g_neighbor_update_last_freq;
static int g_confirm_idens_count;
static int g_queued_count;
static int g_deny_count;
static int g_last_response_svc;
static int g_last_response_reason;
static int g_last_response_target;
static int g_process_channel_count;
static int g_last_process_channel;
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
p25_patch_clear_sg(dsd_state* state, int sgid) {
    (void)state;
    g_clear_count++;
    g_last_sg = sgid;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_prepare_grg_update(dsd_state* state, int sgid, int is_patch, int active, int ssn) {
    (void)state;
    (void)ssn;
    g_update_count++;
    g_last_sg = sgid;
    g_last_update_patch = is_patch;
    g_last_update_active = active;
    if (!active) {
        g_clear_count++;
        return 0;
    }
    return 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_patch_set_kas(dsd_state* state, int sgid, int key, int alg, int ssn) {
    (void)state;
    g_kas_count++;
    g_last_sg = sgid;
    g_last_key = key;
    g_last_alg = alg;
    g_last_ssn = ssn;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
process_channel_to_freq(const dsd_opts* opts, dsd_state* state, int channel) {
    (void)opts;
    (void)state;
    g_process_channel_count++;
    g_last_process_channel = channel;
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
    g_grant_count++;
    g_last_grant_channel = channel;
    g_last_grant_svc = svc_bits;
    g_last_grant_tg = tg;
    g_last_grant_src = src;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_on_queued_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target) {
    (void)opts;
    (void)state;
    g_queued_count++;
    g_last_response_svc = svc_type;
    g_last_response_reason = reason_code;
    g_last_response_target = target;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_sm_on_deny_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target) {
    (void)opts;
    (void)state;
    g_deny_count++;
    g_last_response_svc = svc_type;
    g_last_response_reason = reason_code;
    g_last_response_target = target;
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

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
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
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_store_site_lra(dsd_state* state, uint8_t lra) {
    if (!state) {
        return;
    }
    state->p25_site_lra = lra;
    state->p25_site_lra_valid = 1;
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
    g_clear_count = 0;
    g_seed_count = 0;
    g_grant_count = 0;
    g_mac_count = 0;
    g_last_sg = 0;
    g_last_wgid = 0;
    g_last_wuid = 0;
    g_last_update_patch = 0;
    g_last_update_active = 0;
    g_last_key = 0;
    g_last_alg = 0;
    g_last_ssn = 0;
    g_last_grant_channel = 0;
    g_last_grant_svc = 0;
    g_last_grant_tg = 0;
    g_last_grant_src = 0;
    g_neighbor_update_count = 0;
    g_neighbor_update_last_count = 0;
    g_neighbor_update_last_freq = 0;
    g_confirm_idens_count = 0;
    g_queued_count = 0;
    g_deny_count = 0;
    g_last_response_svc = 0;
    g_last_response_reason = 0;
    g_last_response_target = 0;
    g_process_channel_count = 0;
    g_last_process_channel = 0;
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
expect_contains(const char* tag, const char* text, const char* needle) {
    if (!text || !needle || strstr(text, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle ? needle : "(null)", text ? text : "(null)");
        return 1;
    }
    return 0;
}

static int
read_capture_file(const char* path, char* out, size_t out_sz) {
    if (!path || !out || out_sz == 0) {
        return -1;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    const size_t max_read = out_sz - 1;
    size_t n = fread(out, 1, max_read, f);
    if (n > max_read) {
        n = max_read;
    }
    if (n < max_read && ferror(f) != 0) {
        (void)fclose(f);
        return -1;
    }
    out[n] = '\0';
    (void)fclose(f);
    return 0;
}

static void
build_isp_two_party(uint8_t tsbk[TSBK_BYTES_PER_BLOCK], uint8_t opcode, uint8_t byte2, uint8_t byte3) {
    DSD_MEMSET(tsbk, 0, TSBK_BYTES_PER_BLOCK);
    tsbk[0] = opcode & 0x3F;
    tsbk[2] = byte2;
    tsbk[3] = byte3;
    tsbk[4] = 0x0A;
    tsbk[5] = 0xBC;
    tsbk[6] = 0xDE;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
}

static void
build_isp_group(uint8_t tsbk[TSBK_BYTES_PER_BLOCK], uint8_t opcode, uint8_t svc) {
    DSD_MEMSET(tsbk, 0, TSBK_BYTES_PER_BLOCK);
    tsbk[0] = opcode & 0x3F;
    tsbk[2] = svc;
    tsbk[5] = 0x12;
    tsbk[6] = 0x34;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
}

static void
build_isp_wacn_sys(uint8_t tsbk[TSBK_BYTES_PER_BLOCK], uint8_t opcode) {
    DSD_MEMSET(tsbk, 0, TSBK_BYTES_PER_BLOCK);
    tsbk[0] = opcode & 0x3F;
    tsbk[3] = 0xAB;
    tsbk[4] = 0xCD;
    tsbk[5] = 0xE1;
    tsbk[6] = 0x23;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
}

static int
capture_isp_output(const uint8_t tsbk[TSBK_BYTES_PER_BLOCK], char* out, size_t out_sz) {
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_isp_tsbk") != 0) {
        return -1;
    }
    tsbk_handle_isp_messages(tsbk);
    if (dsd_test_capture_stderr_end(&cap) != 0) {
        return -1;
    }
    return read_capture_file(cap.path, out, out_sz);
}

static int
capture_mfid90_isp_output(const uint8_t tsbk[TSBK_BYTES_PER_BLOCK], char* out, size_t out_sz) {
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_mfid90_isp_tsbk") != 0) {
        return -1;
    }
    tsbk_handle_mfid90_isp_messages(tsbk);
    if (dsd_test_capture_stderr_end(&cap) != 0) {
        return -1;
    }
    return read_capture_file(cap.path, out, out_sz);
}

static int
capture_standard_osp_data_output(dsd_opts* opts, dsd_state* state, const uint8_t tsbk[TSBK_BYTES_PER_BLOCK], char* out,
                                 size_t out_sz) {
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_standard_osp_data_tsbk") != 0) {
        return -1;
    }
    int handled = tsbk_handle_standard_osp_data_channel(opts, state, tsbk);
    if (dsd_test_capture_stderr_end(&cap) != 0 || handled == 0) {
        return -1;
    }
    return read_capture_file(cap.path, out, out_sz);
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
test_standard_isp_metadata_logging_and_no_retune(void) {
    uint8_t tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    char out[4096];
    int rc = 0;

    reset_calls();

    build_isp_group(tsbk, 0x00, 0x90);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp group voice label", out, "Group Voice Service Request");
    rc |= expect_contains("isp group voice group", out, "Group [4660][1234]");
    rc |= expect_contains("isp group voice service", out, "SVC [90]");

    build_isp_two_party(tsbk, 0x04, 0x81, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp unit voice label", out, "Unit-to-Unit Voice Service Request");
    rc |= expect_contains("isp unit voice radios", out, "FM [74565] TO [703710]");
    rc |= expect_contains("isp unit voice service", out, "SVC [81]");

    build_isp_two_party(tsbk, 0x05, 0x82, 0x03);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp answer label", out, "Unit-to-Unit Answer Response");
    rc |= expect_contains("isp answer response", out, "RESPONSE [03]");

    build_isp_two_party(tsbk, 0x08, 0x84, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp explicit dial label", out, "Telephone Interconnect Explicit Dial Request");

    build_isp_two_party(tsbk, 0x09, 0x85, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp pstn label", out, "Telephone Interconnect PSTN Request");

    build_isp_two_party(tsbk, 0x0A, 0x86, 0x02);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp telephone answer label", out, "Telephone Interconnect Answer Response");
    rc |= expect_contains("isp telephone answer response", out, "RESPONSE [02]");

    build_isp_two_party(tsbk, 0x10, 0x04, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp individual data label", out, "Individual Data Service Request");

    build_isp_group(tsbk, 0x11, 0x04);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp group data label", out, "Group Data Service Request");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x12;
    tsbk[2] = 0xAA;
    tsbk[3] = 0x12;
    tsbk[4] = 0x34;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp sndcp channel label", out, "SNDCP Data Channel Request");
    rc |= expect_contains("isp sndcp channel fields", out, "DSO [AA] DAC [1234]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x13;
    tsbk[2] = 0xAA;
    tsbk[3] = 0x55;
    tsbk[4] = 0x12;
    tsbk[5] = 0x34;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp sndcp page label", out, "SNDCP Data Page Response");
    rc |= expect_contains("isp sndcp page fields", out, "DSO [AA] RESPONSE [55] DAC [1234]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x14;
    tsbk[2] = 0xAA;
    tsbk[3] = 0x12;
    tsbk[4] = 0x34;
    tsbk[5] = 0x80;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp sndcp reconnect label", out, "SNDCP Reconnect Request");
    rc |= expect_contains("isp sndcp reconnect fields", out, "DSO [AA] DAC [1234] DATA_TO_SEND [1]");

    build_isp_two_party(tsbk, 0x18, 0x21, 0x43);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp status update label", out, "Status Update Request");
    rc |= expect_contains("isp status update fields", out, "UNIT STATUS [21] USER STATUS [43]");

    build_isp_two_party(tsbk, 0x19, 0x21, 0x43);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp status response label", out, "Status Query Response");

    build_isp_two_party(tsbk, 0x1A, 0x00, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp status query label", out, "Status Query Request");

    build_isp_two_party(tsbk, 0x1C, 0xBE, 0xEF);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp message update label", out, "Message Update Request");
    rc |= expect_contains("isp message update data", out, "SHORT DATA [BEEF]");

    build_isp_two_party(tsbk, 0x1F, 0x00, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp call alert label", out, "Call Alert Request");

    build_isp_two_party(tsbk, 0x20, 0x3B, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp ack label", out, "Unit Acknowledge Response");
    rc |= expect_contains("isp ack service", out, "ACK SVC [3B]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x23;
    tsbk[2] = 0x80 | 0x04;
    tsbk[3] = 0x60;
    tsbk[4] = 0x12;
    tsbk[5] = 0x34;
    tsbk[6] = 0x56;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp cancel label", out, "Cancel Service Request");
    rc |= expect_contains("isp cancel fields", out, "VALID [1] SVC [04] REASON [60] INFO [123456]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x24;
    tsbk[2] = 0x12;
    tsbk[3] = 0x34;
    tsbk[4] = 0x56;
    tsbk[5] = 0x78;
    tsbk[6] = 0x9A;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp extended function label", out, "Extended Function Response");
    rc |= expect_contains("isp extended function fields", out, "FUNC [1234] ARG [56789A]");

    build_isp_group(tsbk, 0x27, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp emergency label", out, "Emergency Alarm Request");
    rc |= expect_contains("isp emergency marker", out, "** EMERGENCY **");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x28;
    tsbk[3] = 0x01;
    tsbk[4] = 0x23;
    tsbk[5] = 0x12;
    tsbk[6] = 0x34;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp affiliation label", out, "Group Affiliation Request");
    rc |= expect_contains("isp affiliation sysid", out, "SYSID [123]");
    rc |= expect_contains("isp affiliation group", out, "Group [4660][1234]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x29;
    tsbk[3] = 0x22;
    tsbk[4] = 0x22;
    tsbk[5] = 0x12;
    tsbk[6] = 0x34;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp affiliation query label", out, "Group Affiliation Query Response");
    rc |= expect_contains("isp affiliation query ann", out, "Announcement Group [8738][2222]");

    build_isp_wacn_sys(tsbk, 0x2B);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp dereg label", out, "Unit De-Registration Request");
    rc |= expect_contains("isp dereg wacn", out, "WACN [ABCDE] SYSID [123]");

    build_isp_wacn_sys(tsbk, 0x2C);
    tsbk[2] = 0x80 | 0x55;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp registration label", out, "Unit Registration Request");
    rc |= expect_contains("isp registration fields", out, "EMERGENCY [1] CAPABILITY [55]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x2D;
    tsbk[2] = 0x80 | 0x55;
    tsbk[4] = 0x77;
    tsbk[5] = 0x12;
    tsbk[6] = 0x34;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp location label", out, "Location Registration Request");
    rc |= expect_contains("isp location lra", out, "LRA [77]");

    build_isp_two_party(tsbk, 0x2E, 0x00, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp auth query label", out, "Authentication Query (obsolete)");

    build_isp_two_party(tsbk, 0x2F, 0x00, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp auth response label", out, "Authentication Response (obsolete)");

    build_isp_wacn_sys(tsbk, 0x30);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp protection label", out, "Protection Parameter Request");

    build_isp_wacn_sys(tsbk, 0x32);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp iden request label", out, "Identifier/Frequency Band Update Request");

    build_isp_two_party(tsbk, 0x36, 0x00, 0x00);
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp roaming request label", out, "Roaming Address Request");

    build_isp_wacn_sys(tsbk, 0x37);
    tsbk[2] = 0x80 | 0x05;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp roaming response label", out, "Roaming Address Response");
    rc |= expect_contains("isp roaming response fields", out, "MSN [5] FINAL [1]");

    for (uint8_t op = 0x38; op <= 0x3B; op++) {
        DSD_MEMSET(tsbk, 0, sizeof(tsbk));
        tsbk[0] = op;
        tsbk[2] = 0x10;
        tsbk[3] = 0x20;
        tsbk[4] = 0x30;
        tsbk[5] = 0x40;
        tsbk[6] = 0x50;
        tsbk[7] = 0x01;
        tsbk[8] = 0x23;
        tsbk[9] = 0x45;
        if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
            return 1;
        }
        rc |= expect_contains("isp auth message label", out, "Authentication Message");
    }

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x21;
    tsbk[2] = 0xDE;
    tsbk[3] = 0xAD;
    tsbk[4] = 0xBE;
    tsbk[5] = 0xEF;
    if (capture_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("isp unsupported label", out, "Unsupported ISP opcode");

    rc |= expect_int("isp no grant callbacks", g_grant_count, 0);
    rc |= expect_int("isp no mac callbacks", g_mac_count, 0);
    rc |= expect_int("isp no frequency lookup", g_process_channel_count, 0);
    rc |= expect_int("isp no queued callbacks", g_queued_count, 0);
    rc |= expect_int("isp no deny callbacks", g_deny_count, 0);

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x00;
    tsbk[2] = 0x80;
    tsbk[5] = 0x12;
    tsbk[6] = 0x34;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_mfid90_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("mfid90 isp regroup label", out, "Group Regroup Voice Request");
    rc |= expect_contains("mfid90 isp regroup source", out, "FM [74565]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x01;
    tsbk[2] = 0x12;
    tsbk[3] = 0x34;
    tsbk[4] = 0x56;
    tsbk[5] = 0x78;
    tsbk[6] = 0x9A;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_mfid90_isp_output(tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("mfid90 isp extended function label", out, "Extended Function Response");
    rc |= expect_contains("mfid90 isp extended function fields", out, "FUNC [1234] ARG [56789A]");

    return rc;
}

static int
test_standard_osp_data_channel_metadata_and_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    char out[2048];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();
    g_channel_freq = 851012500;

    tsbk[0] = 0x10;
    tsbk[2] = 0x10;
    tsbk[3] = 0x01;
    tsbk[4] = 0x01;
    tsbk[5] = 0x02;
    tsbk[6] = 0x03;
    tsbk[7] = 0x04;
    tsbk[8] = 0x05;
    tsbk[9] = 0x06;
    if (capture_standard_osp_data_output(&opts, &state, tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("osp individual data label", out, "Individual Data Channel Grant - Obsolete");
    rc |= expect_contains("osp individual data channel", out, "CHAN [1001]");
    rc |= expect_contains("osp individual data target", out, "Target [66051]");
    rc |= expect_contains("osp individual data source", out, "Source [263430]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x11;
    tsbk[2] = 0x90;
    tsbk[3] = 0x10;
    tsbk[4] = 0x02;
    tsbk[5] = 0x12;
    tsbk[6] = 0x34;
    tsbk[7] = 0x01;
    tsbk[8] = 0x23;
    tsbk[9] = 0x45;
    if (capture_standard_osp_data_output(&opts, &state, tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("osp group data label", out, "Group Data Channel Grant - Obsolete");
    rc |= expect_contains("osp group data service", out, "SVC [90]");
    rc |= expect_contains("osp group data group", out, "Group [4660][1234]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x12;
    tsbk[2] = 0x10;
    tsbk[3] = 0x03;
    tsbk[4] = 0x22;
    tsbk[5] = 0x22;
    tsbk[6] = 0x10;
    tsbk[7] = 0x04;
    tsbk[8] = 0x33;
    tsbk[9] = 0x33;
    if (capture_standard_osp_data_output(&opts, &state, tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("osp group announcement label", out, "Group Data Channel Announcement - Obsolete");
    rc |= expect_contains("osp group announcement channel a", out, "CHAN-A [1003]");
    rc |= expect_contains("osp group announcement channel b", out, "CHAN-B [1004]");

    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x13;
    tsbk[2] = 0x40;
    tsbk[3] = 0xAA;
    tsbk[4] = 0x10;
    tsbk[5] = 0x05;
    tsbk[6] = 0x10;
    tsbk[7] = 0x06;
    tsbk[8] = 0x44;
    tsbk[9] = 0x44;
    if (capture_standard_osp_data_output(&opts, &state, tsbk, out, sizeof(out)) != 0) {
        return 1;
    }
    rc |= expect_contains("osp group explicit label", out, "Group Data Channel Announcement Explicit - Obsolete");
    rc |= expect_contains("osp group explicit channels", out, "CHAN-T [1005] CHAN-R [1006]");
    rc |= expect_int("osp data channel frequency lookups", g_process_channel_count, 6);
    rc |= expect_int("osp data channel no grant callbacks", g_grant_count, 0);
    rc |= expect_int("osp data channel no mac callbacks", g_mac_count, 0);
    rc |= expect_int("osp data channel no active text", state.active_channel[0][0], 0);

    tsbk_decode_ctx_t ctx;
    unsigned long long pdu[24] = {0};
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    DSD_MEMSET(tsbk, 0, sizeof(tsbk));
    tsbk[0] = 0x12;
    tsbk[2] = 0x10;
    tsbk[3] = 0x07;
    tsbk[4] = 0x55;
    tsbk[5] = 0x55;
    tsbk[6] = 0x10;
    tsbk[7] = 0x08;
    tsbk[8] = 0x66;
    tsbk[9] = 0x66;
    DSD_MEMCPY(ctx.tsbk_byte, tsbk, sizeof(tsbk));
    pdu[1] = 0x52;
    reset_calls();
    tsbk_dispatch_message(&opts, &state, &ctx, 0, 0, 0, pdu);
    rc |= expect_int("osp data dispatch suppresses mac bridge", g_mac_count, 0);
    rc |= expect_int("osp data dispatch resolves channels", g_process_channel_count, 2);

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
    rc |= expect_int("a4 patch wgid", g_last_wgid, 0x0203);
    rc |= expect_int("a4 patch update count", g_update_count, 1);
    rc |= expect_int("a4 patch is_patch", g_last_update_patch, 1);
    rc |= expect_int("a4 patch active", g_last_update_active, 1);
    rc |= expect_int("a4 patch kas key", g_last_key, 0xBEEF);
    rc |= expect_int("a4 patch kas alg", g_last_alg, 0x01);
    rc |= expect_int("a4 patch kas ssn", g_last_ssn, 7);

    tsbk[2] = (uint8_t)((0x4 << 5) | 0x05);
    reset_calls();
    tsbk_handle_mfid_a4(&state, tsbk);
    rc |= expect_int("a4 inactive clears", g_clear_count, 1);
    rc |= expect_int("a4 inactive does not add wuid", g_add_wuid_count, 0);
    rc |= expect_int("a4 simulselect is_patch", g_last_update_patch, 0);
    rc |= expect_int("a4 simulselect inactive", g_last_update_active, 0);
    rc |= expect_int("a4 inactive no kas", g_kas_count, 0);

    tsbk[2] = (uint8_t)((0x1 << 5) | 0x06);
    reset_calls();
    tsbk_handle_mfid_a4(&state, tsbk);
    rc |= expect_int("a4 active wuid adds", g_add_wuid_count, 1);
    rc |= expect_int("a4 active wuid", (int)g_last_wuid, 0x010203);
    rc |= expect_int("a4 active wuid ssn", g_last_ssn, 6);

    tsbk[0] = 0x31;
    reset_calls();
    tsbk_handle_mfid_a4(&state, tsbk);
    rc |= expect_int("a4 non-op ignored", g_add_wgid_count + g_add_wuid_count + g_update_count + g_kas_count, 0);
    return rc;
}

static int
test_mfid90_extended_function_supergroup_state(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    uint8_t tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    int rc = 0;

    tsbk[2] = 0x02;
    tsbk[3] = 0x00;
    tsbk[4] = 0x00;
    tsbk[5] = 0x12;
    tsbk[6] = 0x34;
    tsbk[7] = 0x01;
    tsbk[8] = 0x02;
    tsbk[9] = 0x03;
    reset_calls();
    tsbk_handle_mfid90_extended_function(&state, tsbk);
    rc |= expect_int("mfid90 ext create update", g_update_count, 1);
    rc |= expect_int("mfid90 ext create sg", g_last_sg, 0x1234);
    rc |= expect_int("mfid90 ext create wuid count", g_add_wuid_count, 1);
    rc |= expect_int("mfid90 ext create wuid", (int)g_last_wuid, 0x010203);

    tsbk[3] = 0x01;
    reset_calls();
    tsbk_handle_mfid90_extended_function(&state, tsbk);
    rc |= expect_int("mfid90 ext cancel clear", g_clear_count, 1);
    rc |= expect_int("mfid90 ext cancel no add", g_add_wuid_count, 0);
    rc |= expect_int("mfid90 ext cancel sg", g_last_sg, 0x1234);

    tsbk[2] = 0x00;
    tsbk[3] = 0x7F;
    reset_calls();
    tsbk_handle_mfid90_extended_function(&state, tsbk);
    rc |= expect_int("mfid90 ext class0 metadata only", g_update_count + g_clear_count + g_add_wuid_count, 0);
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
    tsbk[2] = 0xA5;
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
    rc |= expect_int("grant svc", g_last_grant_svc, 0xA5);
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
test_mfid90_queued_and_deny_callbacks(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    uint8_t queued[TSBK_BYTES_PER_BLOCK] = {0};
    queued[0] = 0x06;
    queued[2] = 0x80 | 0x15;
    queued[3] = 0x42;
    queued[4] = 0x12;
    queued[5] = 0x34;
    queued[6] = 0x56;
    queued[7] = 0xAB;
    queued[8] = 0xCD;
    queued[9] = 0xEF;

    reset_calls();
    tsbk_handle_mfid90(&opts, &state, queued);
    int rc = 0;
    rc |= expect_int("queued callback count", g_queued_count, 1);
    rc |= expect_int("queued deny count", g_deny_count, 0);
    rc |= expect_int("queued svc", g_last_response_svc, 0x15);
    rc |= expect_int("queued reason", g_last_response_reason, 0x42);
    rc |= expect_int("queued target", g_last_response_target, 0xABCDEF);
    rc |= expect_int("queued active label", strstr(state.active_channel[0], "MOT QUEUED") != NULL, 1);
    rc |= expect_int("queued info label", strstr(state.active_channel[0], "Info: 123456") != NULL, 1);

    uint8_t deny[TSBK_BYTES_PER_BLOCK] = {0};
    deny[0] = 0x07;
    deny[2] = 0x02;
    deny[3] = 0x60;
    deny[7] = 0x00;
    deny[8] = 0x01;
    deny[9] = 0x23;

    reset_calls();
    tsbk_handle_mfid90(&opts, &state, deny);
    rc |= expect_int("deny callback count", g_deny_count, 1);
    rc |= expect_int("deny queued count", g_queued_count, 0);
    rc |= expect_int("deny svc", g_last_response_svc, 0x02);
    rc |= expect_int("deny reason", g_last_response_reason, 0x60);
    rc |= expect_int("deny target", g_last_response_target, 0x000123);
    rc |= expect_int("deny active label", strstr(state.active_channel[0], "MOT DENY") != NULL, 1);
    rc |= expect_int("deny reason label", strstr(state.active_channel[0], "Site Access Denial") != NULL, 1);
    rc |= expect_int("deny no info without flag", strstr(state.active_channel[0], "Info:") == NULL, 1);

    uint8_t deny_aii[TSBK_BYTES_PER_BLOCK] = {0};
    deny_aii[0] = 0x07;
    deny_aii[2] = 0x80 | 0x02;
    deny_aii[3] = 0x60;
    deny_aii[4] = 0x12;
    deny_aii[5] = 0x34;
    deny_aii[6] = 0x56;
    deny_aii[7] = 0x00;
    deny_aii[8] = 0x01;
    deny_aii[9] = 0x23;

    reset_calls();
    tsbk_handle_mfid90(&opts, &state, deny_aii);
    rc |= expect_int("deny aii callback count", g_deny_count, 1);
    rc |= expect_int("deny aii target", g_last_response_target, 0x000123);
    rc |= expect_int("deny aii info label", strstr(state.active_channel[0], "Info: 123456") != NULL, 1);
    return rc;
}

static int
test_mfid90_ack_display_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    uint8_t tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    tsbk[0] = 0x08;
    tsbk[2] = 0x04;
    tsbk[4] = 0x01;
    tsbk[5] = 0x02;
    tsbk[6] = 0x03;
    tsbk[7] = 0x04;
    tsbk[8] = 0x05;
    tsbk[9] = 0x06;

    reset_calls();
    tsbk_handle_mfid90(&opts, &state, tsbk);
    int rc = 0;
    rc |= expect_int("ack no queued callback", g_queued_count, 0);
    rc |= expect_int("ack no deny callback", g_deny_count, 0);
    rc |= expect_int("ack no grant", g_grant_count, 0);
    rc |= expect_int("ack active label", strstr(state.active_channel[0], "MOT ACK") != NULL, 1);
    rc |= expect_int("ack service label", strstr(state.active_channel[0], "Service: 04") != NULL, 1);
    rc |= expect_int("ack source label", strstr(state.active_channel[0], "Source: 66051") != NULL, 1);
    rc |= expect_int("ack target label", strstr(state.active_channel[0], "Target: 263430") != NULL, 1);
    return rc;
}

static int
test_mfid90_tdma_data_channel_display_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.p25_trunk = 1;

    uint8_t tsbk[TSBK_BYTES_PER_BLOCK] = {0};
    tsbk[0] = 0x16;
    tsbk[4] = 0x30;
    tsbk[5] = 0x60;
    tsbk[6] = 0x80;
    tsbk[7] = 0x3E;

    reset_calls();
    g_channel_freq = 420612500;
    tsbk_handle_mfid90(&opts, &state, tsbk);
    int rc = 0;
    rc |= expect_int("tdma data resolves channels", g_process_channel_count, 2);
    rc |= expect_int("tdma data last channel", g_last_process_channel, 0x803E);
    rc |= expect_int("tdma data no grant", g_grant_count, 0);
    rc |= expect_int("tdma data no seed", g_seed_count, 0);
    rc |= expect_int("tdma data active label", strstr(state.active_channel[0], "MOT TDMA Data") != NULL, 1);
    rc |= expect_int("tdma data downlink label", strstr(state.active_channel[0], "3060/3060") != NULL, 1);
    rc |= expect_int("tdma data uplink label", strstr(state.active_channel[0], "803E/803E") != NULL, 1);
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
    state.p25_site_lra = 0x77;
    state.p25_site_lra_valid = 1;
    reset_calls();
    g_channel_freq = 851012500;
    tsbk_handle_network_status(&opts, &state, tsbk);
    rc |= expect_long("voice tuned preserves p25 cc", state.p25_cc_freq, 860012500);
    rc |= expect_long("voice tuned preserves trunk cc", state.trunk_cc_freq, 860012500);
    rc |= expect_int("voice tuned preserves tdma marker", state.p25_cc_is_tdma, 1);
    rc |= expect_long("voice tuned preserves wacn", state.p2_wacn, 0x11111);
    rc |= expect_int("voice tuned preserves sysid", state.p2_sysid, 0x222);
    rc |= expect_int("voice tuned preserves lra", state.p25_site_lra, 0x77);
    rc |= expect_int("voice tuned preserves lra valid", state.p25_site_lra_valid, 1);
    rc |= expect_int("voice tuned skips neighbor", g_neighbor_update_count, 0);
    rc |= expect_int("voice tuned skips iden confirmation", g_confirm_idens_count, 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.p25_cc_is_tdma = 1;
    state.p2_wacn = 0x11111;
    state.p2_sysid = 0x222;
    state.p25_iden_fdma[1].populated = 1;
    state.p25_chan_tdma_explicit[1] = 1;
    state.p25_pending_announcement_count = 1;
    state.p25_pending_announcements[0].populated = 1;
    state.p25_pending_announcements[0].channel = 0x1001;
    reset_calls();
    g_channel_freq = 0;
    tsbk_handle_network_status(&opts, &state, tsbk);
    rc |= expect_long("invalid channel leaves p25 cc", state.p25_cc_freq, 0);
    rc |= expect_int("invalid channel still records fdma", state.p25_cc_is_tdma, 0);
    rc |= expect_long("invalid channel records wacn", state.p2_wacn, 0xABCDE);
    rc |= expect_int("invalid channel records sysid", state.p2_sysid, 0x123);
    rc |= expect_int("invalid channel clears stale iden", state.p25_iden_fdma[1].populated, 0);
    rc |= expect_int("invalid channel clears explicit iden", state.p25_chan_tdma_explicit[1], 0);
    rc |= expect_int("invalid channel clears pending", state.p25_pending_announcement_count, 0);
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
    rc |= expect_int("protected isp skips grants", g_grant_count, 0);
    rc |= expect_int("protected isp skips channel lookup", g_process_channel_count, 0);

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
    rc |= test_standard_isp_metadata_logging_and_no_retune();
    rc |= test_standard_osp_data_channel_metadata_and_dispatch();
    rc |= test_mfid90_regroup_add_delete();
    rc |= test_mfid_a4_patch_and_simulselect_paths();
    rc |= test_mfid90_extended_function_supergroup_state();
    rc |= test_mfid90_grant_seeds_trunk_state();
    rc |= test_mfid90_grant_update_trunk_dispatch();
    rc |= test_mfid90_queued_and_deny_callbacks();
    rc |= test_mfid90_ack_display_only();
    rc |= test_mfid90_tdma_data_channel_display_only();
    rc |= test_network_status_state_policy();
    rc |= test_dispatch_gates_mac_and_vendor_handlers();
    return rc;
}

// NOLINTEND(bugprone-suspicious-include)
