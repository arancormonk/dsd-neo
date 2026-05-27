// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Ann_WD_TSCC switches while already on the CC must keep the return-to-CC
 * cleanup semantics, and deferred switches must not advance the CC anchor.
 */

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_return_to_cc_calls = 0;
static int g_dmr_reset_blocks_calls = 0;
static dsd_trunk_tune_result g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
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

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct RtlSdrContext* g_rtl_ctx = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_dmr_reset_blocks_calls++;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
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

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* opts, dsd_state* state) {
    g_return_to_cc_calls++;
    if (g_return_to_cc_result != DSD_TRUNK_TUNE_RESULT_OK) {
        return g_return_to_cc_result;
    }

    if (opts) {
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
        state->lasttg = 0;
        state->lasttgR = 0;
        state->lastsrc = 0;
        state->lastsrcR = 0;
    }
    dmr_reset_blocks(opts, state);
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
init_env(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 0;
    state->trunk_cc_freq = 851000000L;
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
build_ann_wd_tscc(uint8_t* bits, uint8_t* bytes, uint16_t ch1, uint16_t ch2, uint8_t ch1_flag, uint8_t ch2_flag) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 40U; /* C_BCAST */
    write_bits_u32(bits, 25U, 1U, 4U);
    write_bits_u32(bits, 29U, 2U, 4U);
    bits[33] = (uint8_t)(ch1_flag & 1U);
    bits[34] = (uint8_t)(ch2_flag & 1U);
    write_bits_u32(bits, 56U, ch1 & 0x0FFFU, 12U);
    write_bits_u32(bits, 68U, ch2 & 0x0FFFU, 12U);
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[256];
    uint8_t bytes[48];
    const uint16_t old_ch = 10U;
    const uint16_t next_ch = 11U;
    const long old_cc = 851000000L;
    const long next_cc = 852000000L;

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .return_to_cc_result = test_return_to_cc,
    });

    init_env(&opts, &state);
    state.trunk_chan_map[old_ch] = old_cc;
    state.trunk_chan_map[next_ch] = next_cc;
    state.trunk_vc_freq[0] = 853000000L;
    state.trunk_vc_freq[1] = 853000000L;
    state.lasttg = 1001;
    state.lastsrc = 2002;
    DSD_SNPRINTF(state.active_channel[0], sizeof(state.active_channel[0]), "%s", "stale");
    g_return_to_cc_calls = 0;
    g_dmr_reset_blocks_calls = 0;
    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

    build_ann_wd_tscc(bits, bytes, old_ch, next_ch, 1U, 0U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("tscc switch uses return-to-cc hook", g_return_to_cc_calls == 1);
    rc |= expect_true("tscc switch resets DMR blocks", g_dmr_reset_blocks_calls == 1);
    rc |= expect_true("tscc switch advances CC", state.trunk_cc_freq == next_cc);
    rc |= expect_true("tscc switch clears stale VC", state.trunk_vc_freq[0] == 0 && state.trunk_vc_freq[1] == 0);
    rc |= expect_true("tscc switch clears active channel", state.active_channel[0][0] == '\0');
    rc |= expect_true("tscc switch clears last call", state.lasttg == 0 && state.lastsrc == 0);

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    state.trunk_chan_map[old_ch] = old_cc;
    state.trunk_chan_map[next_ch] = next_cc;
    g_return_to_cc_calls = 0;
    g_dmr_reset_blocks_calls = 0;
    g_return_to_cc_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;

    build_ann_wd_tscc(bits, bytes, old_ch, next_ch, 1U, 0U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("deferred tscc switch calls return-to-cc", g_return_to_cc_calls == 1);
    rc |= expect_true("deferred tscc switch does not reset", g_dmr_reset_blocks_calls == 0);
    rc |= expect_true("deferred tscc switch restores old CC", state.trunk_cc_freq == old_cc);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_state_ext_free_all(&state);
    if (rc == 0) {
        printf("DMR_T3_ANN_WD_TSCC: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
