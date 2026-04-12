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
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

uint64_t
ConvertBitIntoBytes(uint8_t* bits, uint32_t n) {
    uint64_t v = 0ULL;
    for (uint32_t i = 0; i < n; i++) {
        v = (v << 1) | (uint64_t)(bits[i] & 1U);
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
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
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
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
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

struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
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
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

uint8_t
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

void
dsd_drain_audio_output(dsd_opts* opts) {
    (void)opts;
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

static void
init_env(dsd_opts* opts, dsd_state* state) {
    memset(opts, 0, sizeof(*opts));
    memset(state, 0, sizeof(*state));
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
    memset(bits, 0, 256);
    memset(bytes, 0, 48);
    bytes[0] = (uint8_t)(opcode & 0x3FU);
    write_bits_u32(bits, 16U, (uint32_t)(lpcn & 0x0FFFU), 12U);
    bits[28] = (uint8_t)(slot & 1U);
    write_bits_u32(bits, 32U, target & 0x00FFFFFFU, 24U);
    write_bits_u32(bits, 56U, source & 0x00FFFFFFU, 24U);
}

static int
seed_exact(dsd_state* st, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_legacy_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    return dsd_tg_policy_upsert_legacy_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
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
        fprintf(stderr, "FAIL: alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !st ? " dsd_state" : "");
        free(st);
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

    if (rc == 0) {
        printf("DMR_GRANT_POLICY: OK\n");
    }
    free(st);
    free(opts);
    return rc;
}
