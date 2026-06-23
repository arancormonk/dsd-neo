// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify policy-backed DMR CSBK grant filtering for group/private allow-list
 * and explicit block-mode behavior.
 */

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_rotate_symbol_out_file_calls = 0;

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

static void
free_test_state(dsd_state* st) {
    if (st) {
        dsd_state_ext_free_all(st);
    }
    free(st);
}

uint64_t
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t v = 0ULL;
    for (uint32_t i = 0; i < BitLength; i++) {
        v = (v << 1) | (uint64_t)(BufferIn[i] & 1U);
    }
    return v;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)src;
    (void)dst;
    (void)data_string;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_rotate_symbol_out_file_calls++;
}

bool
SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(dsd_socket_t sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

long int
GetCurrentFreq(dsd_socket_t sockfd) {
    (void)sockfd;
    return 0;
}

struct RtlSdrContext;

struct RtlSdrContext* g_rtl_ctx = 0; // NOLINT(misc-use-internal-linkage)
static int g_dmr_reset_blocks_calls = 0;
static int g_result_tune_to_freq_calls = 0;
static int g_fail_tune_to_freq_calls = 0;
static int g_return_to_cc_result_calls = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)ted_sps;
    if (!opts || !state || freq <= 0) {
        return;
    }
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->trunk_is_tuned = 1;
    state->last_vc_sync_time = time(NULL);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    if (opts && state) {
        dmr_sm_init(opts, state);
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_dmr_reset_blocks_calls++;
}

uint8_t
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_drain_audio_output(dsd_opts* opts) {
    (void)opts;
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

static void
init_env(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    state->trunk_cc_freq = 851000000;
    dmr_sm_init(opts, state);
}

static void
write_bits_u32(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        size_t shift = (nbits - 1U) - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
build_grant(uint8_t* bits, uint8_t* bytes, uint8_t opcode, uint16_t lpcn, uint32_t target, uint32_t source,
            uint8_t slot) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = (uint8_t)(opcode & 0x3FU);
    write_bits_u32(bits, 16U, (uint32_t)(lpcn & 0x0FFFU), 12U);
    bits[28] = (uint8_t)(slot & 1U);
    write_bits_u32(bits, 32U, target & 0x00FFFFFFU, 24U);
    write_bits_u32(bits, 56U, source & 0x00FFFFFFU, 24U);
}

static void
build_absolute_grant(uint8_t* bits, uint8_t* bytes, uint8_t opcode, uint32_t target, uint32_t source, uint8_t slot,
                     uint16_t mbc_lpcn, uint16_t rx_int, uint16_t rx_step) {
    build_grant(bits, bytes, opcode, 0x0FFFU, target, source, slot);
    write_bits_u32(bits, 112U, 0U, 4U);
    write_bits_u32(bits, 118U, mbc_lpcn & 0x0FFFU, 12U);
    write_bits_u32(bits, 153U, rx_int & 0x03FFU, 10U);
    write_bits_u32(bits, 163U, rx_step & 0x1FFFU, 13U);
}

static void
build_cap_plus_3e_single_group(uint8_t* bits, uint8_t* bytes, uint8_t rest_lsn, uint8_t active_lsn, uint8_t target) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 0x3EU;
    bytes[1] = 0x10U;

    write_bits_u32(bits, 16U, 3U, 2U); // single-block Cap+ channel status
    bits[18] = 0U;                     // TS1 status bank
    write_bits_u32(bits, 20U, rest_lsn & 0x0FU, 4U);
    bits[24U + (active_lsn - 1U)] = 1U; // bank-one active group bitmap
    write_bits_u32(bits, 32U, target, 8U);
}

static void
build_cap_plus_3e_single_private(uint8_t* bits, uint8_t* bytes, uint8_t rest_lsn, uint8_t active_lsn, uint16_t target) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 0x3EU;
    bytes[1] = 0x10U;

    write_bits_u32(bits, 16U, 3U, 2U); // single-block Cap+ channel status
    bits[18] = 0U;                     // TS1 status bank
    write_bits_u32(bits, 20U, rest_lsn & 0x0FU, 4U);
    write_bits_u32(bits, 40U, 0x80U >> ((active_lsn - 1U) & 7U), 8U); // private/data bank flag
    bits[48U + (active_lsn - 1U)] = 1U;
    write_bits_u32(bits, 56U, target, 16U);
}

static void
build_cap_plus_3e_single_idle(uint8_t* bits, uint8_t* bytes, uint8_t rest_lsn) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 0x3EU;
    bytes[1] = 0x10U;

    write_bits_u32(bits, 16U, 3U, 2U); // single-block Cap+ channel status
    bits[18] = 0U;                     // TS1 status bank
    write_bits_u32(bits, 20U, rest_lsn & 0x0FU, 4U);
}

static dsd_trunk_tune_result
cap_plus_result_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)ted_sps;
    g_result_tune_to_freq_calls++;
    opts->rtlsdr_center_freq = freq;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    state->last_vc_sync_time = time(NULL);
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
fail_result_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    g_fail_tune_to_freq_calls++;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
result_return_to_cc(dsd_opts* opts, dsd_state* state) {
    g_return_to_cc_result_calls++;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static long int
fake_get_current_freq_hz(const dsd_opts* opts) {
    (void)opts;
    return 859987500L;
}

static int
seed_exact(dsd_state* st, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    return dsd_tg_policy_upsert_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

static void
build_con_plus_voice(uint8_t* bits, uint8_t* bytes, uint32_t source, uint32_t target, uint8_t lcn, uint8_t slot,
                     uint8_t opt) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 0x03U;
    bytes[1] = 0x06U;
    bytes[2] = (uint8_t)((source >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)((source >> 8U) & 0xFFU);
    bytes[4] = (uint8_t)(source & 0xFFU);
    bytes[5] = (uint8_t)((target >> 16U) & 0xFFU);
    bytes[6] = (uint8_t)((target >> 8U) & 0xFFU);
    bytes[7] = (uint8_t)(target & 0xFFU);
    bytes[8] = (uint8_t)(((lcn & 0x0FU) << 4U) | ((slot & 1U) << 3U));
    bytes[9] = opt;
}

static void
build_con_plus_data(uint8_t* bits, uint8_t* bytes, uint32_t target, uint8_t lcn, uint8_t slot) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 0x06U;
    bytes[1] = 0x06U;
    bytes[2] = (uint8_t)((target >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)((target >> 8U) & 0xFFU);
    bytes[4] = (uint8_t)(target & 0xFFU);
    bytes[5] = (uint8_t)(((lcn & 0x0FU) << 4U) | ((slot & 1U) << 3U));
}

static void
build_con_plus_termination(uint8_t* bits, uint8_t* bytes, uint32_t target) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 0x0CU;
    bytes[1] = 0x06U;
    bytes[2] = (uint8_t)((target >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)((target >> 8U) & 0xFFU);
    bytes[4] = (uint8_t)(target & 0xFFU);
}

static void
build_pf0(uint8_t* bits, uint8_t* bytes, uint8_t opcode, uint8_t fid) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = (uint8_t)(opcode & 0x3FU);
    bytes[1] = fid;
}

static void
build_preamble(uint8_t* bits, uint8_t* bytes, uint8_t content, uint8_t gi, uint8_t blocks, uint32_t target,
               uint32_t source) {
    build_pf0(bits, bytes, 61U, 0U);
    bits[16] = (uint8_t)(content & 1U);
    bits[17] = (uint8_t)(gi & 1U);
    write_bits_u32(bits, 24U, blocks, 8U);
    write_bits_u32(bits, 32U, target & 0x00FFFFFFU, 24U);
    write_bits_u32(bits, 56U, source & 0x00FFFFFFU, 24U);
}

static void
build_p_protect(uint8_t* bits, uint8_t* bytes, uint8_t kind, uint8_t gi, uint32_t target, uint32_t source) {
    build_pf0(bits, bytes, 47U, 0U);
    write_bits_u32(bits, 28U, kind & 0x07U, 3U);
    bits[31] = (uint8_t)(gi & 1U);
    write_bits_u32(bits, 32U, target & 0x00FFFFFFU, 24U);
    write_bits_u32(bits, 56U, source & 0x00FFFFFFU, 24U);
}

static void
build_c_ahoy(uint8_t* bits, uint8_t* bytes, uint8_t gi, uint8_t svc_kind, uint32_t target, uint32_t source) {
    build_pf0(bits, bytes, 28U, 0U);
    bits[25] = (uint8_t)(gi & 1U);
    write_bits_u32(bits, 28U, svc_kind & 0x0FU, 4U);
    write_bits_u32(bits, 32U, target & 0x00FFFFFFU, 24U);
    write_bits_u32(bits, 56U, source & 0x00FFFFFFU, 24U);
}

static void
build_aloha(uint8_t* bits, uint8_t* bytes, uint8_t fid) {
    build_pf0(bits, bytes, 25U, fid);
}

static void
build_c_move(uint8_t* bits, uint8_t* bytes, uint16_t lpcn, uint8_t slot, uint32_t target, uint32_t source) {
    build_pf0(bits, bytes, 57U, 0U);
    write_bits_u32(bits, 16U, lpcn & 0x0FFFU, 12U);
    bits[28] = (uint8_t)(slot & 1U);
    write_bits_u32(bits, 32U, target & 0x00FFFFFFU, 24U);
    write_bits_u32(bits, 56U, source & 0x00FFFFFFU, 24U);
}

static void
build_p_clear(uint8_t* bits, uint8_t* bytes, uint8_t fid) {
    build_pf0(bits, bytes, 46U, fid);
}

static void
build_c_bcast_ann_wd_tscc(uint8_t* bits, uint8_t* bytes, uint16_t ch1, uint16_t ch2, uint8_t ch1_flag,
                          uint8_t ch2_flag) {
    build_pf0(bits, bytes, 40U, 0U);
    write_bits_u32(bits, 16U, 0U, 5U);
    bits[33] = (uint8_t)(ch1_flag & 1U);
    bits[34] = (uint8_t)(ch2_flag & 1U);
    write_bits_u32(bits, 56U, ch1 & 0x0FFFU, 12U);
    write_bits_u32(bits, 68U, ch2 & 0x0FFFU, 12U);
}

static void
build_c_bcast_chan_freq(uint8_t* bits, uint8_t* bytes, uint16_t channel, uint16_t rx_int, uint16_t rx_step) {
    build_pf0(bits, bytes, 40U, 0U);
    write_bits_u32(bits, 16U, 5U, 5U);
    write_bits_u32(bits, 68U, channel & 0x0FFFU, 12U);
    write_bits_u32(bits, 112U, 0U, 4U);
    write_bits_u32(bits, 118U, channel & 0x0FFFU, 12U);
    write_bits_u32(bits, 130U, rx_int & 0x03FFU, 10U);
    write_bits_u32(bits, 140U, rx_step & 0x1FFFU, 13U);
    write_bits_u32(bits, 153U, rx_int & 0x03FFU, 10U);
    write_bits_u32(bits, 163U, rx_step & 0x1FFFU, 13U);
}

static void
build_c_bcast_adjacent_apcn(uint8_t* bits, uint8_t* bytes, uint16_t channel, uint16_t rx_int, uint16_t rx_step) {
    build_pf0(bits, bytes, 40U, 0U);
    write_bits_u32(bits, 16U, 6U, 5U);
    bits[56] = 1U;
    write_bits_u32(bits, 68U, 0x0FFFU, 12U);
    write_bits_u32(bits, 112U, 0U, 4U);
    write_bits_u32(bits, 118U, channel & 0x0FFFU, 12U);
    write_bits_u32(bits, 130U, rx_int & 0x03FFU, 10U);
    write_bits_u32(bits, 140U, rx_step & 0x1FFFU, 13U);
    write_bits_u32(bits, 153U, rx_int & 0x03FFU, 10U);
    write_bits_u32(bits, 163U, rx_step & 0x1FFFU, 13U);
}

static void
build_xpt_site_status(uint8_t* bits, uint8_t* bytes, uint8_t seq, uint8_t free_lcn, const uint8_t status[6],
                      const uint8_t tg[6]) {
    build_pf0(bits, bytes, 0x0AU, 0x68U);
    write_bits_u32(bits, 0U, seq & 0x03U, 2U);
    write_bits_u32(bits, 16U, free_lcn & 0x0FU, 4U);
    for (size_t i = 0; i < 6U; i++) {
        write_bits_u32(bits, 20U + (i * 2U), status[i] & 0x03U, 2U);
        write_bits_u32(bits, 32U + (i * 8U), tg[i], 8U);
    }
}

static void
build_xpt_adjacent(uint8_t* bits, uint8_t* bytes, uint8_t seq, const uint8_t site_id[4], const uint8_t free_lcn[4]) {
    build_pf0(bits, bytes, 0x0BU, 0x68U);
    write_bits_u32(bits, 0U, seq & 0x03U, 2U);
    for (size_t i = 0; i < 4U; i++) {
        write_bits_u32(bits, 16U + (i * 16U), site_id[i] & 0x1FU, 5U);
        write_bits_u32(bits, 24U + (i * 16U), free_lcn[i] & 0x0FU, 4U);
    }
}

int
main(void) {
    int rc = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    uint8_t bits[256];
    uint8_t bytes[48];
    const uint16_t lpcn = 0x0010;
    const long freq = 852012500L;

    if (!opts || !st) {
        DSD_FPRINTF(stderr, "FAIL: alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !st ? " dsd_state" : "");
        free_test_state(st);
        free(opts);
        return 1;
    }

    init_env(opts, st);
    st->trunk_chan_map[lpcn] = freq;
    opts->trunk_use_allow_list = 1;

    build_grant(bits, bytes, 49U, lpcn, 1100U, 2100U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("group unknown blocked in allow-list", opts->trunk_is_tuned == 0);

    rc |= expect_true("seed group allow", seed_exact(st, 1100U, "A", "ALLOW-GRP") == 0);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("group known allowed", opts->trunk_is_tuned == 1 && st->trunk_vc_freq[0] == freq);

    return_to_cc(opts, st);
    rc |= expect_true("seed group block", seed_exact(st, 1100U, "B", "BLOCK-GRP") == 0);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("group explicit block mode", opts->trunk_is_tuned == 0);

    build_grant(bits, bytes, 48U, lpcn, 9001U, 9002U, 0U);
    return_to_cc(opts, st);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("private unknown blocked in allow-list", opts->trunk_is_tuned == 0);

    rc |= expect_true("seed private allow source", seed_exact(st, 9002U, "A", "ALLOW-SRC") == 0);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("private known source allowed", opts->trunk_is_tuned == 1 && st->trunk_vc_freq[0] == freq);

    opts->trunk_use_allow_list = 0;
    return_to_cc(opts, st);
    build_grant(bits, bytes, 50U, lpcn, 1200U, 2200U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("broadcast voice grant is normalized to group", dmr_sm_get_ctx()->vc_tg == 1200);

    return_to_cc(opts, st);
    build_grant(bits, bytes, 52U, lpcn, 1300U, 2300U, 1U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("data grant enabled for tuning is normalized to group", dmr_sm_get_ctx()->vc_tg == 1300);

    return_to_cc(opts, st);
    build_absolute_grant(bits, bytes, 49U, 3300U, 4400U, 0U, 88U, 452U, 100U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("absolute grant learns mbc lpcn", st->trunk_chan_map[88] == 452012500L);
    rc |= expect_true("absolute grant marks unconfirmed trust while off cc", st->dmr_lcn_trust[88] == 2U);
    rc |= expect_true("absolute grant active channel uses mbc lpcn",
                      strstr(st->active_channel[0], "Active Group Ch: 0058 (TDMA S1) TG: 3300;") != NULL);
    rc |= expect_true("absolute grant dispatches learned frequency",
                      opts->trunk_is_tuned == 1 && st->trunk_vc_freq[0] == 452012500L);

    return_to_cc(opts, st);
    build_absolute_grant(bits, bytes, 49U, 3310U, 4410U, 0U, 89U, 453U, 200U);
    write_bits_u32(bits, 112U, 3U, 4U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("unknown absolute cdef does not learn lpcn", st->trunk_chan_map[89] == 0);
    rc |= expect_true("unknown absolute cdef does not tune", opts->trunk_is_tuned == 0);

    build_grant(bits, bytes, 49U, 0U, 3320U, 4420U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("invalid zero grant channel does not tune", opts->trunk_is_tuned == 0);

    st->trunk_chan_map[lpcn] = freq;
    st->tg_hold = 3330U;
    st->last_vc_sync_time = 123;
    st->last_vc_sync_time_m = 456.0;
    opts->trunk_enable = 0;
    opts->trunk_is_tuned = 0;
    build_grant(bits, bytes, 49U, lpcn, 3330U, 4430U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("tg hold grant clears wall-clock vc sync", st->last_vc_sync_time == 0);
    rc |= expect_true("tg hold grant clears monotonic vc sync", st->last_vc_sync_time_m == 0.0);
    rc |= expect_true("trunk disabled hold grant records vc frequency", st->trunk_vc_freq[0] == freq);
    rc |= expect_true("trunk disabled hold grant does not tune", opts->trunk_is_tuned == 0);
    st->tg_hold = 0;

    opts->trunk_enable = 0;
    opts->trunk_is_tuned = 0;
    build_grant(bits, bytes, 49U, lpcn, 3340U, 4440U, 1U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("trunk disabled grant still records vc frequency", st->trunk_vc_freq[0] == freq);
    rc |= expect_true("trunk disabled grant does not tune", opts->trunk_is_tuned == 0);
    opts->trunk_enable = 1;

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_result = cap_plus_result_tune_to_freq,
    });
    static dsd_opts cap_opts;
    static dsd_state cap_st;
    init_env(&cap_opts, &cap_st);
    const long cap_old_freq = 851000000L;
    const long cap_grant_freq = 853000000L;
    cap_opts.rtlsdr_center_freq = cap_old_freq;
    cap_st.trunk_cc_freq = cap_old_freq;
    cap_st.trunk_chan_map[1] = cap_grant_freq;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    g_dmr_reset_blocks_calls = 0;
    g_result_tune_to_freq_calls = 0;
    build_cap_plus_3e_single_group(bits, bytes, 1U, 1U, 42U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |= expect_true("cap+ 3e tune hook called", g_result_tune_to_freq_calls == 1);
    rc |= expect_true("cap+ 3e tune updates rtl center", cap_opts.rtlsdr_center_freq == cap_grant_freq);
    rc |= expect_true("cap+ 3e reset uses pre-tune center", g_dmr_reset_blocks_calls == 1);

    init_env(&cap_opts, &cap_st);
    cap_opts.rtlsdr_center_freq = cap_old_freq;
    cap_st.trunk_cc_freq = cap_old_freq;
    cap_st.trunk_chan_map[3] = 853250000L;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    g_dmr_reset_blocks_calls = 0;
    g_result_tune_to_freq_calls = 0;
    build_cap_plus_3e_single_private(bits, bytes, 1U, 3U, 4242U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |= expect_true("cap+ 3e private tune hook called", g_result_tune_to_freq_calls == 1);
    rc |= expect_true("cap+ 3e private tune updates VC", cap_st.trunk_vc_freq[0] == 853250000L);
    rc |= expect_true("cap+ 3e private active channel", strstr(cap_st.active_channel[3], "LSN:3 PC:4242;") != NULL);
    rc |= expect_true("cap+ 3e private clears block counter", cap_st.cap_plus_block_num[0] == 0);

    init_env(&cap_opts, &cap_st);
    cap_opts.trunk_tune_private_calls = 0;
    cap_st.trunk_chan_map[3] = 853250000L;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    g_result_tune_to_freq_calls = 0;
    build_cap_plus_3e_single_private(bits, bytes, 1U, 3U, 4242U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |= expect_true("cap+ 3e private disabled suppresses tune",
                      g_result_tune_to_freq_calls == 0 && cap_opts.trunk_is_tuned == 0);
    rc |= expect_true("cap+ 3e private disabled omits active PC", cap_st.active_channel[3][0] == '\0');

    init_env(&cap_opts, &cap_st);
    cap_st.trunk_cc_freq = 851000000L;
    cap_st.trunk_chan_map[4] = 852000000L;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    g_result_tune_to_freq_calls = 0;
    build_cap_plus_3e_single_idle(bits, bytes, 4U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |= expect_true("cap+ 3e idle rest channel becomes CC", cap_st.trunk_cc_freq == 852000000L);
    rc |= expect_true("cap+ 3e idle rest channel does not tune VC", g_result_tune_to_freq_calls == 0);
    rc |= expect_true("cap+ 3e idle rest channel still brands", strcmp(cap_st.dmr_branding_sub, "Cap+ ") == 0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});

    static dsd_opts con_opts;
    static dsd_state con_st;
    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_data_calls = 1;
    con_st.trunk_chan_map[5] = 855125000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    build_con_plus_voice(bits, bytes, 0x112233U, 0x445566U, 5U, 1U, 2U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ voice group tune", con_opts.trunk_is_tuned == 1 && con_st.trunk_vc_freq[0] == 855125000L);
    rc |= expect_true("con+ voice branding", strcmp(con_st.dmr_branding, "Motorola") == 0);
    rc |= expect_true("con+ voice sub-branding", strcmp(con_st.dmr_branding_sub, "Con+ ") == 0);
    rc |= expect_true("con+ voice active slot",
                      strstr(con_st.active_channel[1], "Active Ch: 0005 (TDMA S2) TG: 4478310;") != NULL);
    rc |= expect_true("con+ voice state-machine tg", dmr_sm_get_ctx()->vc_tg == 0x445566);

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_private_calls = 1;
    con_st.trunk_chan_map[6] = 855250000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    build_con_plus_voice(bits, bytes, 0x001122U, 0x003344U, 6U, 0U, 3U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ private voice tunes individual grant",
                      con_opts.trunk_is_tuned == 1 && con_st.trunk_vc_freq[0] == 855250000L);
    rc |= expect_true("con+ private voice state-machine target/source",
                      dmr_sm_get_ctx()->vc_tg == 0 && dmr_sm_get_ctx()->vc_src == 0x001122);
    rc |= expect_true("con+ private voice marks flavor", con_st.is_con_plus == 1);
    rc |= expect_true("con+ private voice active slot",
                      strstr(con_st.active_channel[0], "Active Ch: 0006 (TDMA S1) TG: 13124;") != NULL);

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_group_calls = 0;
    con_st.trunk_chan_map[5] = 855125000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    build_con_plus_voice(bits, bytes, 0x112233U, 0x445566U, 5U, 1U, 2U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ group disabled keeps grant visible",
                      strstr(con_st.active_channel[1], "Active Ch: 0005 (TDMA S2) TG: 4478310;") != NULL);
    rc |= expect_true("con+ group disabled suppresses tune",
                      con_opts.trunk_is_tuned == 0 && dmr_sm_get_ctx()->vc_tg == 0);
    rc |= expect_true("con+ group disabled still brands", strcmp(con_st.dmr_branding_sub, "Con+ ") == 0);

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_data_calls = 1;
    con_st.trunk_chan_map[4] = 856250000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    g_dmr_reset_blocks_calls = 0;
    g_result_tune_to_freq_calls = 0;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_result = cap_plus_result_tune_to_freq,
    });
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ data tune hook called", g_result_tune_to_freq_calls == 1);
    rc |= expect_true("con+ data tune updates VC", con_st.trunk_vc_freq[0] == 856250000L);
    rc |= expect_true("con+ data reset blocks", g_dmr_reset_blocks_calls == 1);
    rc |= expect_true("con+ data active slot",
                      strstr(con_st.active_channel[0], "Active Ch: 0004 (TDMA S1) TG: 51966;") != NULL);
    rc |= expect_true("con+ data marks flavor", con_st.is_con_plus == 1);

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_data_calls = 0;
    DSD_SNPRINTF(con_st.active_channel[0], sizeof(con_st.active_channel[0]), "pre-data-channel");
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ data disabled suppresses tune", con_opts.trunk_is_tuned == 0 && con_st.is_con_plus == 0);
    rc |= expect_true("con+ data disabled still brands", strcmp(con_st.dmr_branding_sub, "Con+ ") == 0);

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_data_calls = 1;
    con_opts.trunk_use_allow_list = 1;
    con_opts.verbose = 1;
    con_st.trunk_chan_map[4] = 856250000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ data policy block keeps grant visible",
                      strstr(con_st.active_channel[0], "Active Ch: 0004 (TDMA S1) TG: 51966;") != NULL);
    rc |=
        expect_true("con+ data policy block suppresses tune", con_opts.trunk_is_tuned == 0 && con_st.is_con_plus == 0);

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_data_calls = 1;
    con_st.trunk_chan_map[4] = 856250000L;
    con_st.last_active_time = time(NULL);
    con_st.last_vc_sync_time = time(NULL) - 10;
    DSD_SNPRINTF(con_st.active_channel[0], sizeof(con_st.active_channel[0]), "previous active");
    g_fail_tune_to_freq_calls = 0;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_result = fail_result_tune_to_freq,
    });
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ data failed tune hook called", g_fail_tune_to_freq_calls == 1);
    rc |= expect_true("con+ data rollback active", strcmp(con_st.active_channel[0], "previous active") == 0);
    rc |= expect_true("con+ data failed tune does not mark flavor", con_st.is_con_plus == 0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});

    init_env(&con_opts, &con_st);
    con_st.trunk_chan_map[5] = 855125000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    build_con_plus_voice(bits, bytes, 0x112233U, 0x445566U, 5U, 1U, 2U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ termination setup tunes vc",
                      con_opts.trunk_is_tuned == 1 && dmr_sm_get_ctx()->vc_tg == 0x445566);
    dmr_sm_emit_voice_sync(&con_opts, &con_st, 0);
    dmr_sm_emit_voice_sync(&con_opts, &con_st, 1);
    rc |= expect_true("con+ termination setup marks voice active",
                      dmr_sm_get_ctx()->slots[0].voice_active == 1 && dmr_sm_get_ctx()->slots[1].voice_active == 1);
    build_con_plus_termination(bits, bytes, 0x445566U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ termination clears voice activity",
                      dmr_sm_get_ctx()->slots[0].voice_active == 0 && dmr_sm_get_ctx()->slots[1].voice_active == 0);
    rc |= expect_true("con+ termination leaves tuned grant pending", con_opts.trunk_is_tuned == 1
                                                                         && con_st.p25_sm_release_count == 0
                                                                         && dmr_sm_get_ctx()->vc_tg == 0x445566);
    rc |= expect_true("con+ termination keeps branding", strcmp(con_st.dmr_branding_sub, "Con+ ") == 0);

    static dsd_opts pf0_opts;
    static dsd_state pf0_st;
    init_env(&pf0_opts, &pf0_st);
    pf0_st.currentslot = 0;
    build_preamble(bits, bytes, 1U, 1U, 4U, 0x00ABCDU, 0x001234U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("preamble marks slot 1 data burst", pf0_st.dmrburstL == 6);
    rc |= expect_true("preamble leaves other burst alone", pf0_st.dmrburstR == 0);

    init_env(&pf0_opts, &pf0_st);
    pf0_st.currentslot = 1;
    build_preamble(bits, bytes, 0U, 0U, 2U, 0x00BCDEU, 0x002345U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("preamble marks slot 2 data burst", pf0_st.dmrburstR == 6);
    rc |= expect_true("preamble slot 2 leaves slot 1 burst alone", pf0_st.dmrburstL == 0);

    init_env(&pf0_opts, &pf0_st);
    pf0_st.currentslot = 0;
    build_p_protect(bits, bytes, 0U, 1U, 0x003333U, 0x004444U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("group protect marks slot 1 voice burst", pf0_st.dmrburstL == 1);
    rc |= expect_true("group protect maps GI to group", pf0_st.gi[0] == 0);

    init_env(&pf0_opts, &pf0_st);
    pf0_st.currentslot = 1;
    build_p_protect(bits, bytes, 1U, 0U, 0x005555U, 0x006666U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("private protect marks slot 2 voice burst", pf0_st.dmrburstR == 1);
    rc |= expect_true("private protect maps GI to private", pf0_st.gi[1] == 1);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.trunk_is_tuned = 1;
    pf0_st.last_vc_sync_time = 0;
    pf0_st.last_vc_sync_time_m = 0.0;
    build_p_protect(bits, bytes, 2U, 1U, 0x007777U, 0x008888U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("hangtime protect refreshes wall-clock VC sync", pf0_st.last_vc_sync_time > 0);
    rc |= expect_true("hangtime protect refreshes monotonic VC sync", pf0_st.last_vc_sync_time_m > 0.0);

    init_env(&pf0_opts, &pf0_st);
    pf0_st.currentslot = 1;
    pf0_st.gi[1] = 9;
    build_c_ahoy(bits, bytes, 1U, 4U, 0x009999U, 0x000777U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_ahoy group maps slot GI to group", pf0_st.gi[1] == 0);

    init_env(&pf0_opts, &pf0_st);
    pf0_st.currentslot = 0;
    pf0_st.gi[0] = 9;
    build_c_ahoy(bits, bytes, 0U, 2U, 0x008888U, 0x000666U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_ahoy private maps slot GI to private", pf0_st.gi[0] == 1);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.use_rigctl = 1;
    pf0_opts.trunk_is_tuned = 0;
    pf0_st.trunk_cc_freq = 0;
    g_rotate_symbol_out_file_calls = 0;
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){
        .get_current_freq_hz = fake_get_current_freq_hz,
    });
    build_aloha(bits, bytes, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    rc |= expect_true("aloha rigctl learns control-channel frequency", pf0_st.trunk_cc_freq == 859987500L);
    rc |= expect_true("aloha rigctl rotates output file on cc", g_rotate_symbol_out_file_calls == 1);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.audio_in_type = AUDIO_IN_RTL;
    pf0_opts.rtlsdr_center_freq = 460012500L;
    pf0_opts.trunk_is_tuned = 0;
    pf0_st.trunk_cc_freq = 0;
    g_rotate_symbol_out_file_calls = 0;
    build_aloha(bits, bytes, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("aloha rtl learns control-channel frequency", pf0_st.trunk_cc_freq == 460012500L);
    rc |= expect_true("aloha rtl rotates output file on cc", g_rotate_symbol_out_file_calls == 1);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.trunk_enable = 0;
    pf0_st.gi[0] = 0;
    build_c_move(bits, bytes, 12U, 0U, 0x001234U, 0x005678U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_move slot 1 target", pf0_st.lasttg == 0x001234U);
    rc |= expect_true("c_move slot 1 source", pf0_st.lastsrc == 0x005678U);
    rc |= expect_true("c_move slot 1 call string", strcmp(pf0_st.call_string[0], "   Group  Move      ") == 0);
    rc |= expect_true("c_move slot 1 debounces opposite slot", pf0_st.dmrburstL == 16 && pf0_st.dmrburstR == 9);
    rc |= expect_true("c_move slot 1 active channel",
                      strstr(pf0_st.active_channel[0], "Active Ch: 000C (TDMA S1) TG: 4660;") != NULL);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.trunk_enable = 0;
    pf0_st.gi[1] = 1;
    build_c_move(bits, bytes, 13U, 1U, 0x002345U, 0x006789U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_move slot 2 target", pf0_st.lasttgR == 0x002345U);
    rc |= expect_true("c_move slot 2 source", pf0_st.lastsrcR == 0x006789U);
    rc |= expect_true("c_move slot 2 call string", strcmp(pf0_st.call_string[1], " Private  Move      ") == 0);
    rc |= expect_true("c_move slot 2 debounces opposite slot", pf0_st.dmrburstR == 16 && pf0_st.dmrburstL == 9);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.trunk_enable = 0;
    pf0_st.currentslot = 0;
    pf0_st.dmrburstL = 6;
    pf0_st.dmrburstR = 6;
    DSD_SNPRINTF(pf0_st.call_string[0], sizeof(pf0_st.call_string[0]), "slot one active");
    build_p_clear(bits, bytes, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("p_clear trunk disabled leaves slot 1 burst", pf0_st.dmrburstL == 6);
    rc |= expect_true("p_clear trunk disabled leaves opposite burst", pf0_st.dmrburstR == 6);
    rc |= expect_true("p_clear trunk disabled leaves slot 1 call string",
                      strcmp(pf0_st.call_string[0], "slot one active") == 0);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.trunk_is_tuned = 1;
    pf0_st.currentslot = 1;
    pf0_st.dmrburstL = 6;
    pf0_st.dmrburstR = 16;
    DSD_SNPRINTF(pf0_st.call_string[1], sizeof(pf0_st.call_string[1]), "slot two data");
    DSD_SNPRINTF(pf0_st.active_channel[1], sizeof(pf0_st.active_channel[1]), "slot two data channel");
    build_p_clear(bits, bytes, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("p_clear data clears slot 2 burst state", pf0_st.dmrburstL == 9 && pf0_st.dmrburstR == 9);
    rc |= expect_true("p_clear data clears slot 2 call string", pf0_st.call_string[1][0] == '\0');
    rc |= expect_true("p_clear data clears slot 2 active channel", pf0_st.active_channel[1][0] == '\0');
    rc |= expect_true("p_clear data does not force release", pf0_st.trunk_sm_force_release == 0);
    rc |= expect_true("p_clear data does not return to cc", pf0_st.p25_sm_release_count == 0);

    init_env(&pf0_opts, &pf0_st);
    pf0_st.trunk_chan_map[lpcn] = freq;
    pf0_st.last_vc_sync_time = time(NULL) - 10;
    build_grant(bits, bytes, 49U, lpcn, 5500U, 6500U, 1U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("p_clear hold setup tunes vc", pf0_opts.trunk_is_tuned == 1 && dmr_sm_get_ctx()->vc_tg == 5500);
    pf0_st.currentslot = 1;
    pf0_st.lasttgR = 5500;
    pf0_st.tg_hold = 5500U;
    pf0_st.dmrburstL = 16;
    pf0_st.dmrburstR = 16;
    DSD_SNPRINTF(pf0_st.call_string[1], sizeof(pf0_st.call_string[1]), "slot two voice");
    DSD_SNPRINTF(pf0_st.active_channel[1], sizeof(pf0_st.active_channel[1]), "slot two voice channel");
    g_return_to_cc_result_calls = 0;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .return_to_cc_result = result_return_to_cc,
    });
    build_p_clear(bits, bytes, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    rc |= expect_true("p_clear hold forced return hook called", g_return_to_cc_result_calls == 1);
    rc |= expect_true("p_clear hold clears force latch", pf0_st.trunk_sm_force_release == 0);
    rc |= expect_true("p_clear hold returns to cc", pf0_opts.trunk_is_tuned == 0 && pf0_st.p25_sm_release_count == 1);
    rc |= expect_true("p_clear hold clears slot 2 labels",
                      pf0_st.call_string[1][0] == '\0' && pf0_st.active_channel[1][0] == '\0');
    pf0_st.tg_hold = 0;

    init_env(&pf0_opts, &pf0_st);
    build_c_bcast_chan_freq(bits, bytes, 77U, 451U, 100U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_bcast channel frequency learned", pf0_st.trunk_chan_map[77] == 451012500L);
    rc |= expect_true("c_bcast channel frequency tracked", pf0_st.trunk_lcn_freq[0] == 451012500L);
    dsd_state_ext_free_all(&pf0_st);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.dmr_t3_heuristic_fill = 1;
    pf0_st.trunk_chan_map[10] = 451000000L;
    pf0_st.trunk_chan_map[12] = 451025000L;
    build_c_bcast_adjacent_apcn(bits, bytes, 14U, 451U, 400U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_bcast apcn learned anchor", pf0_st.trunk_chan_map[14] == 451050000L);
    rc |= expect_true("heuristic fill synthesizes missing lcn 11", pf0_st.trunk_chan_map[11] == 451012500L);
    rc |= expect_true("heuristic fill synthesizes missing lcn 13", pf0_st.trunk_chan_map[13] == 451037500L);
    dsd_state_ext_free_all(&pf0_st);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.trunk_enable = 1;
    pf0_opts.trunk_is_tuned = 0;
    pf0_st.trunk_cc_freq = 851000000L;
    pf0_st.trunk_chan_map[11] = 851000000L;
    pf0_st.trunk_chan_map[22] = 852250000L;
    g_return_to_cc_result_calls = 0;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .return_to_cc_result = result_return_to_cc,
    });
    build_c_bcast_ann_wd_tscc(bits, bytes, 11U, 22U, 1U, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    rc |= expect_true("c_bcast tscc switch hook called", g_return_to_cc_result_calls == 1);
    rc |= expect_true("c_bcast tscc switch commits new cc", pf0_st.trunk_cc_freq == 852250000L);
    dsd_state_ext_free_all(&pf0_st);

    static dsd_opts xpt_opts;
    static dsd_state xpt_st;
    init_env(&xpt_opts, &xpt_st);
    xpt_opts.audio_in_type = AUDIO_IN_RTL;
    xpt_opts.rtlsdr_center_freq = 461250000L;
    xpt_st.trunk_chan_map[1] = 461262500L;
    xpt_st.trunk_chan_map[2] = 461275000L;
    xpt_st.last_vc_sync_time = time(NULL) - 10;
    uint8_t xpt_status[6] = {3U, 2U, 1U, 0U, 3U, 0U};
    uint8_t xpt_tg[6] = {77U, 88U, 0U, 0U, 99U, 0U};
    build_xpt_site_status(bits, bytes, 0U, 2U, xpt_status, xpt_tg);
    dmr_cspdu(&xpt_opts, &xpt_st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt site status updates site parms", strcmp(xpt_st.dmr_site_parms, "Free LCN - 2 ") == 0);
    rc |= expect_true("xpt site status records active TG",
                      strstr(xpt_st.active_channel[0], "LSN:1 TG:77;") != NULL
                          && strstr(xpt_st.active_channel[0], "LSN:5 TG:99;") != NULL);
    rc |=
        expect_true("xpt site status records active private", strstr(xpt_st.active_channel[0], "LSN:2 PC:88;") != NULL);
    rc |= expect_true("xpt site status sets rtl control channel",
                      xpt_opts.trunk_is_tuned == 1 && xpt_st.trunk_cc_freq == 461250000L);
    rc |= expect_true("xpt site status sets branding", strcmp(xpt_st.dmr_branding_sub, "XPT ") == 0);
    rc |= expect_true("xpt site status emits grant", dmr_sm_get_ctx()->vc_tg == 77);
    dsd_state_ext_free_all(&xpt_st);

    init_env(&xpt_opts, &xpt_st);
    xpt_opts.audio_in_type = AUDIO_IN_RTL;
    xpt_opts.rtlsdr_center_freq = 462000000L;
    xpt_opts.trunk_tune_private_calls = 1;
    xpt_st.trunk_chan_map[7] = 462012500L;
    xpt_st.last_vc_sync_time = time(NULL) - 10;
    g_rotate_symbol_out_file_calls = 0;
    uint8_t xpt_private_status[6] = {2U, 0U, 0U, 0U, 0U, 0U};
    uint8_t xpt_private_tg[6] = {0U, 0U, 0U, 0U, 0U, 0U};
    build_xpt_site_status(bits, bytes, 1U, 0U, xpt_private_status, xpt_private_tg);
    dmr_cspdu(&xpt_opts, &xpt_st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt seq1 status-only private emits placeholder grant", dmr_sm_get_ctx()->vc_tg == 1);
    rc |= expect_true("xpt seq1 private uses banked LSN map", xpt_st.trunk_vc_freq[0] == 462012500L);
    rc |= expect_true("xpt seq1 learns rtl control channel", xpt_st.trunk_cc_freq == 462000000L);
    rc |= expect_true("xpt seq1 rotates symbols before tune", g_rotate_symbol_out_file_calls == 1);
    rc |= expect_true("xpt seq1 writes banked active channel", xpt_st.active_channel[1][0] == '\0');
    dsd_state_ext_free_all(&xpt_st);

    init_env(&xpt_opts, &xpt_st);
    xpt_opts.audio_in_type = AUDIO_IN_RTL;
    xpt_opts.rtlsdr_center_freq = 463000000L;
    xpt_st.trunk_chan_map[13] = 463012500L;
    xpt_st.last_vc_sync_time = time(NULL);
    uint8_t xpt_debounce_status[6] = {3U, 0U, 0U, 0U, 0U, 0U};
    uint8_t xpt_debounce_tg[6] = {55U, 0U, 0U, 0U, 0U, 0U};
    build_xpt_site_status(bits, bytes, 2U, 0U, xpt_debounce_status, xpt_debounce_tg);
    dmr_cspdu(&xpt_opts, &xpt_st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt seq2 records banked active TG", strstr(xpt_st.active_channel[2], "LSN:13 TG:55;") != NULL);
    rc |= expect_true("xpt seq2 debounce suppresses grant", dmr_sm_get_ctx()->vc_tg == 0);
    rc |= expect_true("xpt seq2 still learns rtl control channel", xpt_st.trunk_cc_freq == 463000000L);
    dsd_state_ext_free_all(&xpt_st);

    init_env(&xpt_opts, &xpt_st);
    xpt_opts.audio_in_type = AUDIO_IN_RTL;
    xpt_opts.rtlsdr_center_freq = 464000000L;
    xpt_opts.trunk_use_allow_list = 1;
    xpt_opts.verbose = 1;
    xpt_st.trunk_chan_map[1] = 464012500L;
    xpt_st.last_vc_sync_time = time(NULL) - 10;
    uint8_t xpt_block_status[6] = {3U, 0U, 0U, 0U, 0U, 0U};
    uint8_t xpt_block_tg[6] = {44U, 0U, 0U, 0U, 0U, 0U};
    build_xpt_site_status(bits, bytes, 0U, 0U, xpt_block_status, xpt_block_tg);
    dmr_cspdu(&xpt_opts, &xpt_st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt allow-list block keeps active TG visible",
                      strstr(xpt_st.active_channel[0], "LSN:1 TG:44;") != NULL);
    rc |= expect_true("xpt allow-list block suppresses grant", dmr_sm_get_ctx()->vc_tg == 0);
    rc |= expect_true("xpt allow-list block does not tune VC", xpt_st.trunk_vc_freq[0] == 0);
    dsd_state_ext_free_all(&xpt_st);

    init_env(&xpt_opts, &xpt_st);
    uint8_t xpt_sites[4] = {3U, 4U, 0U, 7U};
    uint8_t xpt_free[4] = {2U, 5U, 0U, 9U};
    build_xpt_adjacent(bits, bytes, 2U, xpt_sites, xpt_free);
    dmr_cspdu(&xpt_opts, &xpt_st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt adjacent sets branding", strcmp(xpt_st.dmr_branding_sub, "XPT ") == 0);

    if (rc == 0) {
        printf("DMR_GRANT_POLICY: OK\n");
    }
    free_test_state(st);
    free(opts);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
