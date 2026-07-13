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
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

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
test_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_return_to_cc_calls++;
    if (g_return_to_cc_result != DSD_TRUNK_TUNE_RESULT_OK) {
        return g_return_to_cc_result;
    }

    if (opts) {
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

static void
build_t3_grant(uint8_t* bits, uint8_t* bytes, uint8_t opcode, uint16_t lpcn, uint8_t slot, uint32_t target,
               uint32_t source) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = (uint8_t)(opcode & 0x3FU);
    write_bits_u32(bits, 16U, lpcn & 0x0FFFU, 12U);
    bits[28] = (uint8_t)(slot & 1U);
    write_bits_u32(bits, 32U, target & 0x00FFFFFFU, 24U);
    write_bits_u32(bits, 56U, source & 0x00FFFFFFU, 24U);
}

static void
build_c_aloha_small(uint8_t* bits, uint8_t* bytes, uint16_t net, uint16_t site) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 25U; /* C_ALOHA_SYS_PARMS */
    write_bits_u32(bits, 40U, 1U, 2U);
    write_bits_u32(bits, 42U, net & 0x007FU, 7U);
    write_bits_u32(bits, 49U, site & 0x001FU, 5U);
    write_bits_u32(bits, 54U, 2U, 2U);
}

static int
expect_active_channel_contains(const char* tag, const char* actual, const char* expected) {
    if (strstr(actual, expected) == NULL) {
        DSD_FPRINTF(stderr, "FAIL: %s: expected '%s' in '%s'\n", tag, expected, actual);
        return 1;
    }
    return 0;
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
        .return_to_cc_request = test_return_to_cc,
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

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    build_t3_grant(bits, bytes, 49U, 0x0123U, 0U, 1001U, 2002U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |=
        expect_active_channel_contains("group grant active-channel kind", state.active_channel[0], "Active Group Ch:");

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    build_t3_grant(bits, bytes, 52U, 0x0124U, 1U, 1002U, 2003U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_active_channel_contains("data grant active-channel kind", state.active_channel[1], "Active Data Ch:");

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    build_t3_grant(bits, bytes, 48U, 0x0125U, 0U, 1003U, 2004U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_active_channel_contains("private grant active-channel kind", state.active_channel[0],
                                         "Active Private Ch:");

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    build_c_aloha_small(bits, bytes, 2U, 27U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("C_ALOHA applies default DMRLA split",
                      strcmp(state.dmr_site_parms, "TIII Small:3-1.28;105B; ") == 0);

    dsd_state_ext_free_all(&state);
    init_env(&opts, &state);
    opts.dmr_dmrla_is_set = 1;
    opts.dmr_dmrla_n = 0;
    build_c_aloha_small(bits, bytes, 2U, 27U);
    dmr_cspdu(&opts, &state, bits, bytes, 1U, 0U);
    rc |= expect_true("C_ALOHA honors raw DMRLA override", strcmp(state.dmr_site_parms, "TIII Small:2-27;105B; ") == 0);

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
