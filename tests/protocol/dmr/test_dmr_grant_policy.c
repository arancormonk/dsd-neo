// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify policy-backed DMR CSBK grant filtering for group/private allow-list
 * and explicit block-mode behavior.
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/receive_policy.h>
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

static int
recent_activity_matches(const dsd_state* state, uint8_t index, dsd_call_kind kind, uint64_t target, uint64_t source,
                        uint32_t channel, const char* notice_fragment) {
    dsd_recent_activity_snapshot recent;
    if (dsd_recent_activity_copy_snapshot(state, &recent) <= 0) {
        return 0;
    }
    const dsd_recent_activity_entry* entry = &recent.entries[index];
    return entry->observation.kind == kind && entry->observation.ota_target_id == target
           && entry->observation.ota_source_id == source && entry->observation.channel == channel
           && (!notice_fragment || strstr(entry->notice, notice_fragment) != NULL);
}

static int
recent_activity_is_empty(const dsd_state* state, uint8_t index) {
    dsd_recent_activity_snapshot recent;
    return dsd_recent_activity_copy_snapshot(state, &recent) <= 0
           || recent.entries[index].observation.kind == DSD_CALL_KIND_UNKNOWN;
}

static int
recent_activity_has_slot(const dsd_state* state, uint8_t index, uint8_t slot) {
    dsd_recent_activity_snapshot recent;
    return dsd_recent_activity_copy_snapshot(state, &recent) > 0 && recent.entries[index].observation.slot == slot;
}

static void
seed_voice_call(dsd_state* state, uint8_t slot, dsd_call_kind kind, uint64_t target, uint64_t source) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = slot,
        .kind = kind,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) <= 0) {
        DSD_FPRINTF(stderr, "FAIL: could not seed canonical DMR voice call\n");
        abort();
    }
}

static int
active_call_matches(const dsd_state* state, uint8_t slot, dsd_call_kind kind, uint64_t target, uint64_t source) {
    dsd_call_snapshot call;
    return dsd_call_state_get(state, slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE && call.kind == kind
           && call.ota_target_id == target && call.ota_source_id == source;
}

static int
no_active_call(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    return dsd_call_state_get(state, slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE;
}

static void
free_test_state(dsd_state* st) {
    if (st) {
        dsd_state_ext_free_all(st);
    }
    free(st);
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
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation->ota_source_id;
    (void)observation->ota_target_id;
    (void)notice;
    (void)slot;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_rotate_symbol_out_file_calls++;
}

static int g_dmr_reset_blocks_calls = 0;
static int g_result_tune_to_freq_calls = 0;
static int g_fail_tune_to_freq_calls = 0;
static int g_return_to_cc_result_calls = 0;

static void
reset_to_cc(dsd_opts* opts, dsd_state* state) {
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
    dsd_state_ext_free_all(state);
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
test_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    opts->rtlsdr_center_freq = freq;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    state->last_vc_sync_time = time(NULL);
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

static dsd_trunk_tune_result
cap_plus_result_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    g_result_tune_to_freq_calls++;
    opts->rtlsdr_center_freq = freq;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    state->last_vc_sync_time = time(NULL);
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
fail_result_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    g_fail_tune_to_freq_calls++;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
result_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
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

static int
test_receive_data_grant(dsd_opts* opts, dsd_state* state) {
    init_env(opts, state);
    install_trunk_tuning_hooks();
    opts->verbose = 1; /* The rejected grant's diagnostic must read an initialized policy decision. */
    state->trunk_chan_map[4] = 856250000L;
    state->last_vc_sync_time = 0;
    uint8_t bits[256], bytes[48];
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    int rc = expect_true("slot block parsed",
                         dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_SLOT, 1, "", "1", "", NULL, 0) == 0);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
    rc |= expect_true("slot block prevents Connect Plus data tune", opts->trunk_is_tuned == 0);
    opts->receive.filters[DSD_RECEIVE_SLOT].enabled = 0;
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
    rc |= expect_true("disabled slot filter permits Connect Plus data tune",
                      opts->trunk_is_tuned == 1 && state->trunk_vc_freq[0] == 856250000L);
    return rc;
}

/* Issue #637: a voice channel whose verified colour code the policy rejects is remembered by the DMR context, and a
   grant to it is not followed; once the policy admits the code, the grant is. */
static int
test_receive_rejected_carrier_grant(dsd_opts* opts, dsd_state* state) {
    init_env(opts, state);
    install_trunk_tuning_hooks();
    state->trunk_chan_map[4] = 856250000L;
    state->last_vc_sync_time = 0;
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    ctx->receive_carriers.entries[0] =
        (dsd_receive_carrier){.freq_hz = 856250000L, .value = 7, .kind = DSD_RECEIVE_DCC};
    uint8_t bits[256], bytes[48];
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    int rc = expect_true("dcc block parsed",
                         dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_DCC, 1, "", "7", "", NULL, 0) == 0);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
    rc |= expect_true("a channel whose verified colour code is blocked is not followed", opts->trunk_is_tuned == 0);
    opts->receive.filters[DSD_RECEIVE_DCC].enabled = 0;
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
    rc |= expect_true("the channel is followed once the policy admits its code",
                      opts->trunk_is_tuned == 1 && state->trunk_vc_freq[0] == 856250000L);
    return rc;
}

/* Issue #637: a voice channel a CSBK path tuned itself (Connect Plus data) gets the bounded receive window too: a
   carrier whose verified colour code the policy blocks is released once the window closes, and its code is
   remembered. */
static int
test_receive_csbk_tuned_carrier_window(dsd_opts* opts, dsd_state* state) {
    init_env(opts, state);
    install_trunk_tuning_hooks();
    state->trunk_chan_map[4] = 856250000L;
    state->last_vc_sync_time = 0;
    uint8_t bits[256], bytes[48];
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
    int rc = expect_true("connect plus data grant tuned", opts->trunk_is_tuned == 1);
    state->receive_codes_fresh_freq = 856250000L; /* As the engine's tune does under a filter. */
    rc |= expect_true("dcc block parsed",
                      dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_DCC, 1, "", "7", "", NULL, 0) == 0);
    state->dmr_color_code = 7U;
    opts->frame_dmr = 1; /* The DMR state machine ticks only while DMR trunking can recover. */
    dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    dmr_sm_tick_ctx(ctx, opts, state);
    rc |= expect_true("the window opens on the rejected carrier",
                      opts->trunk_is_tuned == 1 && ctx->receive_check_started_m > 0.0);
    rc |= expect_true("the carrier's code is remembered",
                      dsd_receive_carriers_rejects(&ctx->receive_carriers, opts, 856250000L));
    ctx->receive_check_started_m -= 10.0;
    dmr_sm_tick_ctx(ctx, opts, state);
    rc |= expect_true("the rejected carrier is released when the window closes", opts->trunk_is_tuned == 0);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));
    return rc;
}

/* Issue #637: with every receive filter off a voice channel whose colour code differs from its control channel's keeps
   its grant's call. The tune forgets the published code (dsd_engine_forget_carrier_code_values()), so the voice sync
   that precedes the channel's first EMB seeds the call with no identity, and the channel's code then names it rather
   than ending it as a changed service. */
static int
test_receive_vc_code_differs_from_cc(dsd_opts* opts, dsd_state* state) {
    init_env(opts, state);
    install_trunk_tuning_hooks();
    int rc = expect_true(
        "names parsed",
        dsd_receive_names_parse("--dcc-name '4=Control' --dcc-name '7=Voice'", &opts->receive.names, NULL, 0) == 0);
    dmr_sm_event_t grant = dmr_sm_ev_group_grant(852000000, 0, 100, 200);
    dmr_sm_event(dmr_sm_get_ctx(), opts, state, &grant);
    state->dmr_color_code = 16U; /* As the engine's voice-channel tune leaves it. */
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dmr_sm_emit_voice_sync(opts, state, 0);
    dsd_call_snapshot call;
    rc |= expect_true("the first voice sync opens the grant's call with no identity",
                      dsd_call_state_get(state, 0, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                          && call.ota_target_id == 100U && !call.receive_identity.known);
    state->dmr_color_code = 7U;
    dmr_sm_emit_voice_sync(opts, state, 0);
    dsd_call_state_refresh_receive(opts, state, 0);
    rc |= expect_true("the channel's own code names the call and keeps it",
                      dsd_call_state_get(state, 0, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                          && call.ota_target_id == 100U && call.receive_identity.known
                          && call.receive_identity.value == 7U);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));
    return rc;
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

/* A voice sync on a slot whose own voice passed the colour-code gate, as an accepted voice burst leaves it: only that
   holds a followed call (a voice sync alone never does). */
static void
test_accepted_voice_sync(dsd_opts* opts, dsd_state* state, int slot) {
    state->dmr_confidence_voice_open[slot] = 1;
    state->dmr_confidence_voice_m[slot] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(opts, state, slot);
}

/* ---- Status selection (Capacity Plus, Hytera XPT): hold, priority, preemption, busy view ---- */

#define ST_F1 853000000L
#define ST_F3 853250000L
#define ST_F4 853500000L

static const uint64_t k_status_t0_ns = 1000000ULL * 1000000000ULL;
static int g_status_tunes = 0;

static void
status_clock_at(double seconds) {
    dsd_decode_clock_test_set_ns(k_status_t0_ns + (uint64_t)(seconds * 1e9));
}

static dsd_trunk_tune_result
status_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    g_status_tunes++;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    state->last_vc_sync_time = dsd_decode_time();
    state->last_vc_sync_time_m = dsd_decode_now_mono_s();
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
status_env(dsd_opts* opts, dsd_state* state) {
    dsd_decode_clock_use_test(k_status_t0_ns);
    init_env(opts, state);
    opts->trunk_hangtime = 2.0f;
    state->trunk_chan_map[1] = ST_F1;
    state->trunk_chan_map[2] = ST_F1;
    state->trunk_chan_map[3] = ST_F3;
    state->trunk_chan_map[4] = ST_F4;
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dmr_sm_init(opts, state);
    g_status_tunes = 0;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = status_tune_request,
        .tune_to_cc_request = status_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

static void
status_env_done(dsd_state* state) {
    install_trunk_tuning_hooks();
    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(state);
}

/* The status's two-second voice window closed: what holds the receiver is the trunk state machine's busy view. */
static void
status_window_closed(dsd_state* state) {
    state->last_vc_sync_time = dsd_decode_time() - 10;
}

static void
seed_policy_row(dsd_state* st, uint32_t id, const char* mode, int priority, int preempt) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, mode, "ROW", DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        abort();
    }
    row.priority = priority;
    row.preempt = preempt ? 1U : 0U;
    if (dsd_tg_policy_upsert_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) != 0) {
        abort();
    }
}

/* A single-block Cap+ channel status with group calls on bank-one LSNs 1..4 (@p tgs[i] on LSN i + 1, 0: idle). */
static void
run_cap_plus_groups(dsd_opts* opts, dsd_state* state, const uint8_t tgs[4]) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 0x3EU;
    bytes[1] = 0x10U;
    write_bits_u32(bits, 16U, 3U, 2U);
    write_bits_u32(bits, 19U, 1U, 5U); // rest LSN 1
    size_t at = 32U;
    for (int i = 0; i < 4; i++) {
        if (tgs[i] != 0U) {
            bits[24U + (size_t)i] = 1U;
            write_bits_u32(bits, at, tgs[i], 8U);
            at += 8U;
        }
    }
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

static void
accepted_voice_now(dsd_opts* opts, dsd_state* state, int slot) {
    state->dmr_confidence_voice_open[slot] = 1;
    state->dmr_confidence_voice_m[slot] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(opts, state, slot);
}

/* R13: an allow-listed group on another carrier no longer hops the receiver off a live followed call, and the status
   leaves the voice-sync stamp alone. */
static int
test_cap_plus_allow_list_does_not_hop(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    opts->trunk_use_allow_list = 1;
    seed_policy_row(st, 42, "A", 0, 0);
    seed_policy_row(st, 43, "A", 0, 0);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 0U, 0U, 0U});
    int rc = expect_true("cap+ follows the listed group", st->trunk_vc_freq[0] == ST_F1 && g_status_tunes == 1);
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0);
    status_window_closed(st);
    const time_t stamp = st->last_vc_sync_time;
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 43U, 0U});
    rc |= expect_true("cap+ allow-list: a listed group elsewhere does not hop a live call",
                      st->trunk_vc_freq[0] == ST_F1 && g_status_tunes == 1);
    rc |= expect_true("cap+ allow-list keeps the voice-sync stamp", st->last_vc_sync_time == stamp);
    status_env_done(st);
    return rc;
}

/* Preemption ranking: of two eligible candidates in one status the higher priority is followed, in either order,
   with exactly one tune. */
static int
test_cap_plus_preempt_ranking(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (int order = 0; order < 2; order++) {
        status_env(opts, st);
        seed_policy_row(st, 42, "A", 10, 0);
        seed_policy_row(st, 43, "A", 60, 1);
        seed_policy_row(st, 44, "A", 70, 1);
        run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 0U, 0U, 0U});
        status_clock_at(1.0);
        accepted_voice_now(opts, st, 0);
        status_window_closed(st);
        g_status_tunes = 0;
        run_cap_plus_groups(opts, st,
                            order ? (const uint8_t[4]){0U, 0U, 44U, 43U} : (const uint8_t[4]){0U, 0U, 43U, 44U});
        const long want = order ? ST_F3 : ST_F4;
        rc |= expect_true("cap+ ranking follows the higher priority once",
                          st->trunk_vc_freq[0] == want && g_status_tunes == 1);
        status_env_done(st);
    }

    // Idle: with no priorities channel order stands; with one, priority decides.
    status_env(opts, st);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 43U, 44U});
    rc |= expect_true("cap+ idle keeps channel order", st->trunk_vc_freq[0] == ST_F3);
    status_env_done(st);
    status_env(opts, st);
    seed_policy_row(st, 44, "A", 70, 0);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 43U, 44U});
    rc |= expect_true("cap+ idle ranks by priority", st->trunk_vc_freq[0] == ST_F4);
    status_env_done(st);
    return rc;
}

/* A carrier followed on its seeds alone (slot 0 group 42 p10, slot 1 group 77 p90): a candidate must outrank both. */
static int
test_cap_plus_seed_only_carrier(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 77, "A", 90, 0);
    seed_policy_row(st, 43, "A", 60, 1);
    seed_policy_row(st, 45, "A", 95, 1);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 77U, 0U, 0U});
    int rc = expect_true("cap+ seed-only follow", st->trunk_vc_freq[0] == ST_F1);
    status_clock_at(1.0);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 43U, 0U});
    rc |= expect_true("cap+ p60 does not outrank the p90 seed", st->trunk_vc_freq[0] == ST_F1);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 45U, 0U});
    rc |= expect_true("cap+ p95 outranks both seeds", st->trunk_vc_freq[0] == ST_F3);
    status_env_done(st);
    return rc;
}

/* Reception restricted to TS1: a status follows the p10 TS1 call and seeds a p100 companion on the rejected TS2. Before
   the first voice the acquisition holds only the admitted slot, so a p100 preempting call on another carrier's TS1
   takes the receiver after the dwell. A slot merely awaiting its colour-code check stays a held call: under a pending
   DCC check the p90 seed still keeps a p60 candidate out, and a p95 one outranks both seeds. */
static int
test_cap_plus_seed_receive_slot_admission(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 77, "A", 100, 0);
    seed_policy_row(st, 45, "A", 100, 1);
    int rc = expect_true("slot filter parsed",
                         dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_SLOT, 1, "1", "", "", NULL, 0) == 0);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 77U, 0U, 0U});
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    rc |= expect_true("cap+ follows the TS1 call and seeds the TS2 companion",
                      st->trunk_vc_freq[0] == ST_F1 && sm->slots[1].target == 77U);
    status_clock_at(1.0);
    dmr_sm_busy_view_t view;
    rc |= expect_true("acquisition holds only the admitted slot",
                      dmr_sm_describe_held_calls(sm, opts, st, dsd_decode_now_mono_s(), &view) == 1
                          && view.members == 0x1);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 45U, 0U});
    rc |= expect_true("cap+ p100 preempt outranks the admitted p10 call, not the rejected seed",
                      st->trunk_vc_freq[0] == ST_F3);
    status_env_done(st);

    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 77, "A", 90, 0);
    seed_policy_row(st, 43, "A", 60, 1);
    seed_policy_row(st, 45, "A", 95, 1);
    rc |= expect_true("dcc filter parsed",
                      dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_DCC, 1, "7", "", "", NULL, 0) == 0);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 77U, 0U, 0U});
    rc |= expect_true("cap+ seed-only follow under a pending check", st->trunk_vc_freq[0] == ST_F1);
    status_clock_at(1.0);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 43U, 0U});
    rc |= expect_true("cap+ pending check: p60 does not outrank the p90 seed", st->trunk_vc_freq[0] == ST_F1);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 45U, 0U});
    rc |= expect_true("cap+ pending check: p95 outranks both seeds", st->trunk_vc_freq[0] == ST_F3);
    status_env_done(st);
    return rc;
}

/* Hold takes the receiver from a non-held carrier; a carrier whose companion slot carries the Hold group keeps it. */
static int
test_cap_plus_hold(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 77U, 0U, 0U});
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0);
    st->tg_hold = 43;
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 43U, 0U});
    int rc = expect_true("cap+ hold over a non-held carrier hops", st->trunk_vc_freq[0] == ST_F3);
    status_env_done(st);

    status_env(opts, st);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 77U, 0U, 0U});
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0);
    accepted_voice_now(opts, st, 1); // the companion's call (seeded 77) is live too
    st->tg_hold = 77;
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 77U, 0U});
    rc |= expect_true("cap+ a held companion keeps the carrier", st->trunk_vc_freq[0] == ST_F1);
    status_env_done(st);
    return rc;
}

/* A seed's identity is replaced by a later status naming another group on its LSN; a checked LC's is not. */
static int
test_cap_plus_seed_identity_reconcile(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 0U, 0U, 0U});
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    int rc = expect_true("seeded slot identity", sm->slots[0].target == 42U && sm->slots[0].prov == DMR_SM_ID_SEED);
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0); // voice without an LC
    run_cap_plus_groups(opts, st, (const uint8_t[4]){50U, 0U, 0U, 0U});
    rc |= expect_true("a later status replaces the seed's group",
                      sm->slots[0].target == 50U && sm->slots[0].prov == DMR_SM_ID_SEED);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 50U, 4321U);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){51U, 0U, 0U, 0U});
    rc |= expect_true("a checked LC's identity wins over a later status",
                      sm->slots[0].target == 50U && sm->slots[0].prov == DMR_SM_ID_LC);
    status_env_done(st);
    return rc;
}

/* A single-block Cap+ channel status with a private call on LSN @p private_lsn (bank one) and group @p tg on LSN
   @p group_lsn. */
static void
run_cap_plus_private_and_group(dsd_opts* opts, dsd_state* state, uint8_t private_lsn, uint16_t private_target,
                               uint8_t group_lsn, uint8_t tg) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 0x3EU;
    bytes[1] = 0x10U;
    write_bits_u32(bits, 16U, 3U, 2U);
    write_bits_u32(bits, 19U, 1U, 5U); // rest LSN 1
    bits[24U + (group_lsn - 1U)] = 1U;
    write_bits_u32(bits, 32U, tg, 8U);
    // Bank two's bitmap (40..47) stays empty; then the private bank's flag, bitmap and one 16-bit ID.
    write_bits_u32(bits, 48U, 0x80U >> ((private_lsn - 1U) & 7U), 8U);
    bits[56U + (private_lsn - 1U)] = 1U;
    write_bits_u32(bits, 64U, private_target, 16U);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

/* A seeded group slot that a later status shows carrying a private call holds a call of unknown identity: it keeps
   its activity, and only Hold takes the receiver from it, not an outranking preempting group. */
static int
test_cap_plus_seed_private_activity(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 43, "A", 100, 1);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 0U, 0U, 0U});
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    int rc = expect_true("cap+ seeded group slot", st->trunk_vc_freq[0] == ST_F1 && sm->slots[0].target == 42U);
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0); // voice without an LC
    status_window_closed(st);
    status_clock_at(1.9);
    run_cap_plus_private_and_group(opts, st, 1U, 0x1234U, 3U, 43U);
    rc |= expect_true("cap+ a private call on the seeded slot is an unknown identity",
                      sm->slots[0].kind == (uint8_t)DSD_CALL_KIND_UNKNOWN && sm->slots[0].last_active_m > 0.0);
    rc |= expect_true("cap+ an unknown private call is not preempted", st->trunk_vc_freq[0] == ST_F1);
    st->tg_hold = 43;
    run_cap_plus_private_and_group(opts, st, 1U, 0x1234U, 3U, 43U);
    rc |= expect_true("cap+ an unknown private call yields to Hold", st->trunk_vc_freq[0] == ST_F3);
    st->tg_hold = 0;
    status_env_done(st);
    return rc;
}

/* Cap+ acquisition membership is the status's, not the identities': a selected private LSN (no seed) and a seeded slot a
   later status leaves unknown each hold the carrier before the first voice as a call of unknown identity, so an
   outranking preempting group elsewhere does not take the receiver, and Hold does. */
static int
test_cap_plus_acquisition_unknown_members(dsd_opts* opts, dsd_state* st) {
    /* The selected LSN 2 (slot 1) carries a private call; its companion slot carries seeded group 42. */
    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 0x1234, "A", 50, 0);
    seed_policy_row(st, 43, "A", 100, 1);
    run_cap_plus_private_and_group(opts, st, 2U, 0x1234U, 1U, 42U);
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    int rc = expect_true("cap+ follows the private LSN's carrier",
                         st->trunk_vc_freq[0] == ST_F1 && sm->slots[0].target == 42U
                             && sm->slots[1].kind == (uint8_t)DSD_CALL_KIND_UNKNOWN);
    status_clock_at(1.0);
    status_window_closed(st);
    run_cap_plus_private_and_group(opts, st, 2U, 0x1234U, 3U, 43U);
    rc |= expect_true("cap+ a selected private call in acquisition is not preempted", st->trunk_vc_freq[0] == ST_F1);
    st->tg_hold = 43;
    run_cap_plus_private_and_group(opts, st, 2U, 0x1234U, 3U, 43U);
    rc |= expect_true("cap+ a selected private call in acquisition yields to Hold", st->trunk_vc_freq[0] == ST_F3);
    st->tg_hold = 0;
    status_env_done(st);

    /* The selected LSN 1 carries group 42; seeded companion LSN 2 (group 77) is later shown carrying a private call. */
    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 77, "A", 10, 0);
    seed_policy_row(st, 43, "A", 100, 1);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 77U, 0U, 0U});
    sm = dmr_sm_get_ctx();
    rc |= expect_true("cap+ seeds both slots", st->trunk_vc_freq[0] == ST_F1 && sm->slots[1].target == 77U);
    status_clock_at(1.0);
    status_window_closed(st);
    run_cap_plus_private_and_group(opts, st, 2U, 0x1234U, 3U, 43U);
    rc |= expect_true("cap+ the seeded slot's private call is unknown",
                      sm->slots[1].kind == (uint8_t)DSD_CALL_KIND_UNKNOWN);
    rc |= expect_true("cap+ a seeded slot made unknown is not preempted", st->trunk_vc_freq[0] == ST_F1);
    status_env_done(st);
    return rc;
}

/* XPT entry kinds: a private status is an unknown identity, followed only with private tuning on and never admitted or
   ranked by a row; unknown statuses are not followed; a busy carrier yields only to an outranking preempt. */
static int
test_xpt_entry_kinds(dsd_opts* opts, dsd_state* st) {
    uint8_t bits[256];
    uint8_t bytes[48];
    int rc = 0;

    // Private with private tuning off (group on): not followed.
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    opts->trunk_tune_private_calls = 0;
    st->trunk_chan_map[1] = 461262500L;
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){2U, 0U, 0U, 0U, 0U, 0U},
                          (const uint8_t[6]){88U, 0U, 0U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt private with private tuning off is not followed", g_status_tunes == 0);
    status_env_done(st);

    // Private with private tuning on and group tuning off: followed as an unknown private identity.
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    opts->trunk_tune_group_calls = 0;
    st->trunk_chan_map[1] = 461262500L;
    status_window_closed(st);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt private follows with group tuning off", st->trunk_vc_freq[0] == 461262500L
                                                                       && dmr_sm_get_ctx()->vc_is_group == 0
                                                                       && dmr_sm_get_ctx()->vc_dst == 0);
    status_env_done(st);

    // Under the allow-list, a priority-100 row equal to the 8-bit value and an ID-0 row admit nothing.
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    opts->trunk_use_allow_list = 1;
    seed_policy_row(st, 88, "A", 100, 1);
    seed_policy_row(st, 0, "A", 100, 1);
    st->trunk_chan_map[1] = 461262500L;
    status_window_closed(st);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt private hash collision admits nothing under the allow-list", g_status_tunes == 0);
    status_env_done(st);

    // Unknown (kind 1) with a value: not followed.
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    st->trunk_chan_map[1] = 461262500L;
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){1U, 0U, 0U, 0U, 0U, 0U},
                          (const uint8_t[6]){66U, 0U, 0U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt unknown status is not followed", g_status_tunes == 0);
    status_env_done(st);
    return rc;
}

/* XPT ranks one bank's group entries by priority, in either order, with one grant. */
static int
test_xpt_ranking(dsd_opts* opts, dsd_state* st) {
    uint8_t bits[256];
    uint8_t bytes[48];
    int rc = 0;
    for (int order = 0; order < 2; order++) {
        status_env(opts, st);
        opts->audio_in_type = AUDIO_IN_RTL;
        opts->rtlsdr_center_freq = 461250000U;
        seed_policy_row(st, 44, "A", 70, 0);
        st->trunk_chan_map[1] = 461262500L;
        st->trunk_chan_map[3] = 461287500L;
        status_window_closed(st);
        build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 3U, 0U, 0U, 0U},
                              order ? (const uint8_t[6]){44U, 0U, 43U, 0U, 0U, 0U}
                                    : (const uint8_t[6]){43U, 0U, 44U, 0U, 0U, 0U});
        dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
        rc |= expect_true("xpt ranking follows the higher priority once",
                          st->trunk_vc_freq[0] == (order ? 461262500L : 461287500L) && g_status_tunes == 1
                              && dmr_sm_get_ctx()->vc_tg == 44);
        status_env_done(st);
    }
    return rc;
}

/* ---- Connect Plus: policy before the voice window ---- */

#define CP_FA 855125000L
#define CP_FB 855250000L
#define CP_FC 855375000L

static void
con_plus_voice(dsd_opts* opts, dsd_state* st, uint32_t src, uint32_t tg, uint8_t lcn, uint8_t slot, uint8_t opt) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_con_plus_voice(bits, bytes, src, tg, lcn, slot, opt);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
}

static void
con_plus_env(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    opts->trunk_tune_data_calls = 0; // the 2 s window
    st->trunk_chan_map[5] = CP_FA;
    st->trunk_chan_map[6] = CP_FB;
    st->trunk_chan_map[7] = CP_FC;
}

static int
test_con_plus_policy_before_window(dsd_opts* opts, dsd_state* st) {
    int rc = 0;

    // Window open with a live call: an ordinary grant is refused as before; an outranking preempt is followed.
    con_plus_env(opts, st);
    seed_policy_row(st, 100, "A", 10, 0);
    seed_policy_row(st, 300, "A", 60, 1);
    con_plus_voice(opts, st, 1, 100, 5, 0, 2);
    rc |= expect_true("con+ follows A", st->trunk_vc_freq[0] == CP_FA && st->is_con_plus == 1);
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0);
    st->last_vc_sync_time = dsd_decode_time();
    con_plus_voice(opts, st, 2, 200, 6, 0, 2);
    rc |= expect_true("con+ window refuses a grant without preempt", st->trunk_vc_freq[0] == CP_FA);
    con_plus_voice(opts, st, 3, 300, 6, 0, 2);
    rc |= expect_true("con+ preempt past the dwell is followed inside the window",
                      st->trunk_vc_freq[0] == CP_FB && dmr_sm_get_ctx()->vc_tg == 300);
    status_env_done(st);

    // Hold: followed over a non-held call, refused while the held call is the Hold group.
    con_plus_env(opts, st);
    con_plus_voice(opts, st, 1, 100, 5, 0, 2);
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0);
    st->last_vc_sync_time = dsd_decode_time();
    st->tg_hold = 400;
    con_plus_voice(opts, st, 4, 400, 6, 0, 2);
    rc |= expect_true("con+ hold over a non-held call is followed", st->trunk_vc_freq[0] == CP_FB);
    status_clock_at(2.0);
    accepted_voice_now(opts, st, 0);
    st->last_vc_sync_time = dsd_decode_time();
    con_plus_voice(opts, st, 4, 400, 7, 0, 2);
    rc |= expect_true("con+ hold is refused while the Hold group is held", st->trunk_vc_freq[0] == CP_FB);
    st->tg_hold = 0;
    status_env_done(st);

    // A data channel the CSBK path tuned itself (the machine follows no call there): a preempt is refused inside the
    // window, a Hold is followed.
    con_plus_env(opts, st);
    opts->trunk_tune_data_calls = 1;
    seed_policy_row(st, 300, "A", 60, 1);
    st->trunk_chan_map[4] = 856250000L;
    status_window_closed(st);
    uint8_t bits[256];
    uint8_t bytes[48];
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ data channel tuned", st->trunk_vc_freq[0] == 856250000L);
    status_clock_at(1.0);
    st->last_vc_sync_time = dsd_decode_time();
    con_plus_voice(opts, st, 3, 300, 6, 0, 2);
    rc |= expect_true("con+ preempt is refused on a CSBK-tuned data channel", st->trunk_vc_freq[0] == 856250000L);
    st->tg_hold = 400;
    con_plus_voice(opts, st, 4, 400, 6, 0, 2);
    rc |= expect_true("con+ hold is followed from a CSBK-tuned data channel", st->trunk_vc_freq[0] == CP_FB);
    st->tg_hold = 0;
    status_env_done(st);

    // Group tuning off still refuses a private grant.
    con_plus_env(opts, st);
    opts->trunk_tune_group_calls = 0;
    status_window_closed(st);
    con_plus_voice(opts, st, 0x001122U, 0x003344U, 6, 0, 3);
    rc |= expect_true("con+ private with group tuning off is refused", g_status_tunes == 0);
    status_env_done(st);
    return rc;
}

static int
test_xpt_busy_carrier(dsd_opts* opts, dsd_state* st) {
    uint8_t bits[256];
    uint8_t bytes[48];
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    seed_policy_row(st, 77, "A", 10, 0);
    seed_policy_row(st, 99, "A", 60, 1);
    st->trunk_chan_map[1] = 461262500L;
    st->trunk_chan_map[3] = 461287500L;
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 0U, 0U, 0U, 0U},
                          (const uint8_t[6]){77U, 0U, 0U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    int rc = expect_true("xpt follows group 77", st->trunk_vc_freq[0] == 461262500L);
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0);
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){0U, 0U, 3U, 0U, 0U, 0U},
                          (const uint8_t[6]){0U, 0U, 88U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt busy carrier refuses a candidate without preempt", st->trunk_vc_freq[0] == 461262500L);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){0U, 0U, 3U, 0U, 0U, 0U},
                          (const uint8_t[6]){0U, 0U, 99U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt busy carrier yields to an outranking preempt", st->trunk_vc_freq[0] == 461287500L);
    status_env_done(st);

    // Parked (no call followed) where a wanted call is heard: a candidate elsewhere is busy.
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    st->trunk_cc_freq = 461250000L;
    dmr_sm_init(opts, st);
    st->trunk_chan_map[3] = 461287500L;
    accepted_voice_now(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 55U, 5U);
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){0U, 0U, 3U, 0U, 0U, 0U},
                          (const uint8_t[6]){0U, 0U, 88U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt resident parking call is busy", g_status_tunes == 0);
    status_env_done(st);
    return rc;
}

/* A followed XPT carrier's slots are what the latest status names on them, before the busy view and the ranking read
   them: after following group 77, a private status on its LSN leaves the call of unknown identity (only Hold takes the
   receiver from it, not an outranking preempting group ranked against 77's priority), and a group status names the
   slot's new call, whose dwell starts then. */
static int
test_xpt_status_reconciles_followed_slot(dsd_opts* opts, dsd_state* st) {
    uint8_t bits[256];
    uint8_t bytes[48];
    int rc = 0;
    for (int variant = 0; variant < 2; variant++) {
        status_env(opts, st);
        opts->audio_in_type = AUDIO_IN_RTL;
        opts->rtlsdr_center_freq = 461250000U;
        seed_policy_row(st, 77, "A", 10, 0);
        seed_policy_row(st, 88, "A", 50, 0);
        seed_policy_row(st, 99, "A", 60, 1);
        st->trunk_chan_map[1] = 461262500L;
        st->trunk_chan_map[3] = 461287500L;
        status_window_closed(st);
        build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 0U, 0U, 0U, 0U},
                              (const uint8_t[6]){77U, 0U, 0U, 0U, 0U, 0U});
        dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
        dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
        rc |= expect_true("xpt follows group 77", st->trunk_vc_freq[0] == 461262500L && sm->slots[0].target == 77U);
        status_clock_at(1.0);
        accepted_voice_now(opts, st, 0);
        status_window_closed(st);
        if (variant == 0) {
            build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){2U, 0U, 3U, 0U, 0U, 0U},
                                  (const uint8_t[6]){5U, 0U, 99U, 0U, 0U, 0U});
            dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
            rc |= expect_true("xpt a private status leaves the followed slot unknown",
                              sm->slots[0].kind == (uint8_t)DSD_CALL_KIND_UNKNOWN && sm->vc_identity_known == 0);
            rc |= expect_true("xpt an unknown private call is not preempted", st->trunk_vc_freq[0] == 461262500L);
            st->tg_hold = 99;
            dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
            rc |= expect_true("xpt an unknown private call yields to Hold", st->trunk_vc_freq[0] == 461287500L);
            st->tg_hold = 0;
        } else {
            build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 3U, 0U, 0U, 0U},
                                  (const uint8_t[6]){88U, 0U, 99U, 0U, 0U, 0U});
            dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
            rc |= expect_true("xpt a group status names the followed slot's new call",
                              sm->slots[0].target == 88U && sm->vc_tg == 88);
            /* Ranked above the p50 call, but that call is new on the slot: its dwell has just begun. */
            rc |= expect_true("xpt a p60 preempt waits for the dwell of the call now on the slot",
                              st->trunk_vc_freq[0] == 461262500L);
        }
        status_env_done(st);
    }
    return rc;
}

/* A status naming another group on the followed slot is subordinate to a checked LC's identity there, in place as in
   its reconciliation: an unlisted status target never replaces the p100 call the LC named, so a p60 preempt elsewhere
   stays busy on the next status. */
static int
test_xpt_status_in_place_keeps_lc_identity(dsd_opts* opts, dsd_state* st) {
    uint8_t bits[256];
    uint8_t bytes[48];
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    seed_policy_row(st, 77, "A", 10, 0);
    seed_policy_row(st, 100, "A", 100, 0);
    seed_policy_row(st, 99, "A", 60, 1);
    st->trunk_chan_map[1] = 461262500L;
    st->trunk_chan_map[3] = 461287500L;
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 0U, 0U, 0U, 0U},
                          (const uint8_t[6]){77U, 0U, 0U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    int rc = expect_true("xpt follows group 77", st->trunk_vc_freq[0] == 461262500L && sm->state == DMR_SM_TUNED);
    status_clock_at(0.5);
    accepted_voice_now(opts, st, 0);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 100U, 1U);
    rc |= expect_true("the checked LC names the followed slot's call",
                      sm->slots[0].target == 100U && sm->slots[0].prov == (uint8_t)DMR_SM_ID_LC);
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0);
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 0U, 0U, 0U, 0U},
                          (const uint8_t[6]){55U, 0U, 0U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("an unlisted status target leaves the LC's identity in place",
                      st->trunk_vc_freq[0] == 461262500L && sm->slots[0].target == 100U
                          && sm->slots[0].prov == (uint8_t)DMR_SM_ID_LC);
    status_clock_at(2.0);
    accepted_voice_now(opts, st, 0);
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){0U, 0U, 3U, 0U, 0U, 0U},
                          (const uint8_t[6]){0U, 0U, 99U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("a p60 preempt does not displace the LC's p100 call", st->trunk_vc_freq[0] == 461262500L);
    status_env_done(st);
    return rc;
}

/* An anonymous voice call (no LC yet) on slot 0 of the receiver's carrier, as the vocoder opens one: identity-less
   voice reacquires a call a sync loss ended within the reacquire gap, else begins a call of its own. */
static uint64_t
anonymous_voice_on_slot0(dsd_opts* opts, dsd_state* st) {
    const dsd_call_observation voice = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(st, &voice, DSD_CALL_BOUNDARY_BEGIN);
    (void)dsd_call_state_update_media(st, 0U, 1, 0.0);
    accepted_voice_now(opts, st, 0);
    dsd_call_snapshot call;
    return dsd_call_state_get(st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
               ? dsd_call_snapshot_transmission(&call)
               : 0U;
}

/* A status (XPT site status, @p capplus 0; Cap+ channel status, 1) names group 77 on the followed slot before any
   checked LC, and its voice binds the slot's record to that call. A sync loss ends the call, and a status naming 77
   again arrives before the call is reacquired: the record keeps its binding to the faded call, which the reacquired
   voice heals (same transmission). Once that call ends and the hangtime runs out, the next anonymous call there inherits
   neither 77's identity nor its priority: a p60 preempt on another carrier never takes the receiver from it. */
static int
test_status_repeat_during_fade_keeps_binding(dsd_opts* opts, dsd_state* st, int capplus) {
    const char* const label = capplus ? "cap+" : "xpt";
    uint8_t bits[256];
    uint8_t bytes[48];
    char what[128];
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    seed_policy_row(st, 77, "A", 10, 0);
    seed_policy_row(st, 99, "A", 60, 1);
    const long followed = capplus ? ST_F1 : 461262500L;
    const long other = capplus ? ST_F3 : 461287500L;
    if (!capplus) {
        st->trunk_chan_map[1] = followed;
        st->trunk_chan_map[3] = other;
    }
    status_window_closed(st);
    if (capplus) {
        run_cap_plus_groups(opts, st, (const uint8_t[4]){77U, 0U, 0U, 0U});
    } else {
        build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 0U, 0U, 0U, 0U},
                              (const uint8_t[6]){77U, 0U, 0U, 0U, 0U, 0U});
        dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    }
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    DSD_SNPRINTF(what, sizeof what, "%s follows group 77", label);
    int rc = expect_true(what, st->trunk_vc_freq[0] == followed && sm->slots[0].target == 77U);

    status_clock_at(0.3);
    const uint64_t first = anonymous_voice_on_slot0(opts, st);
    DSD_SNPRINTF(what, sizeof what, "%s the voice binds the record", label);
    rc |= expect_true(what, first != 0U && sm->slots[0].call_epoch == first);

    status_clock_at(1.0);
    (void)dsd_call_state_end_ex(st, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_SYNC_LOSS);
    status_clock_at(1.2);
    status_window_closed(st);
    if (capplus) {
        run_cap_plus_groups(opts, st, (const uint8_t[4]){77U, 0U, 0U, 0U});
    } else {
        dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    }
    DSD_SNPRINTF(what, sizeof what, "%s a status repeat during the fade keeps the binding", label);
    rc |= expect_true(what, sm->slots[0].target == 77U && sm->slots[0].call_epoch == first);

    status_clock_at(1.4);
    const uint64_t healed = anonymous_voice_on_slot0(opts, st);
    DSD_SNPRINTF(what, sizeof what, "%s the reacquired call is the faded transmission, still bound", label);
    rc |= expect_true(what, healed == first && sm->slots[0].call_epoch == first);

    status_clock_at(2.0);
    (void)dsd_call_state_end_ex(st, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_TERMINATOR);
    status_clock_at(4.5); /* past the 2 s hangtime */
    const uint64_t next = anonymous_voice_on_slot0(opts, st);
    DSD_SNPRINTF(what, sizeof what, "%s the next transmission is a call of its own", label);
    rc |= expect_true(what, next != 0U && next != first);

    status_clock_at(4.6);
    status_window_closed(st);
    if (capplus) {
        run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 99U, 0U});
    } else {
        build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){0U, 0U, 3U, 0U, 0U, 0U},
                              (const uint8_t[6]){0U, 0U, 99U, 0U, 0U, 0U});
        dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    }
    DSD_SNPRINTF(what, sizeof what, "%s the next anonymous call is not preempted as the faded one", label);
    rc |= expect_true(what, st->trunk_vc_freq[0] == followed && sm->slots[0].target != 77U);
    status_env_done(st);
    return rc;
}

/* As the fade case, but the status-named call ends by an unverified terminator, whose heal window (0.25 s) is shorter
   than a sync loss's: a status naming 77 again 0.3 s later finds a call that can no longer be reacquired, so the record
   awaits the next call instead of keeping a binding no heal will use. Voice 0.4 s after the end is that next call, which
   the status named: 77, its dwell kept from the follow, so an eligible p60 preempt on another carrier takes it. */
static int
test_xpt_status_after_unverified_terminator_awaits_next(dsd_opts* opts, dsd_state* st) {
    uint8_t bits[256];
    uint8_t bytes[48];
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    seed_policy_row(st, 77, "A", 10, 0);
    seed_policy_row(st, 99, "A", 60, 1);
    st->trunk_chan_map[1] = 461262500L;
    st->trunk_chan_map[3] = 461287500L;
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 0U, 0U, 0U, 0U},
                          (const uint8_t[6]){77U, 0U, 0U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    int rc = expect_true("xpt follows group 77", st->trunk_vc_freq[0] == 461262500L && sm->slots[0].target == 77U);
    status_clock_at(0.3);
    const uint64_t first = anonymous_voice_on_slot0(opts, st);
    rc |= expect_true("xpt the voice binds the record", first != 0U && sm->slots[0].call_epoch == first);

    status_clock_at(1.0);
    (void)dsd_call_state_end_ex(st, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_UNVERIFIED_TERMINATOR);
    status_clock_at(1.3);
    status_window_closed(st);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt a status past the terminator's heal window awaits the next call",
                      sm->slots[0].target == 77U && sm->slots[0].call_epoch == 0U);

    status_clock_at(1.4);
    const uint64_t next = anonymous_voice_on_slot0(opts, st);
    rc |= expect_true("xpt voice after the heal window is a call of its own, the status-named one",
                      next != 0U && next != first && sm->slots[0].target == 77U && sm->slots[0].call_epoch == next);
    status_clock_at(1.5);
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){0U, 0U, 3U, 0U, 0U, 0U},
                          (const uint8_t[6]){0U, 0U, 99U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt an eligible p60 preempt takes the p10 call", st->trunk_vc_freq[0] == 461287500L);
    status_env_done(st);
    return rc;
}

/* An XPT site status that passed its CRC follows group 77; before the first voice, a RAS-accepted status (-F) names
   group 88 on the followed slot. The followed call's identity is the later status's, and so is its provenance: the
   call published at the first voice reads RAS-unverified, never verified. */
static int
test_xpt_status_reassignment_carries_crc_provenance(dsd_opts* opts, dsd_state* st) {
    uint8_t bits[256];
    uint8_t bytes[48];
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    st->trunk_chan_map[1] = 461262500L;
    st->currentslot = 0;
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 0U, 0U, 0U, 0U},
                          (const uint8_t[6]){77U, 0U, 0U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    int rc = expect_true("xpt follows group 77 from a verified status",
                         st->trunk_vc_freq[0] == 461262500L && sm->vc_tg == 77 && sm->vc_crc_flags == 0U);
    status_clock_at(0.2);
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){3U, 0U, 0U, 0U, 0U, 0U},
                          (const uint8_t[6]){88U, 0U, 0U, 0U, 0U, 0U});
    st->event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    st->event_crc_invalid[0] = 0U;
    rc |= expect_true("xpt the RAS status renames the followed call with its provenance",
                      sm->vc_tg == 88 && sm->vc_crc_flags == (uint8_t)DSD_CRC_RAS_UNVERIFIED);
    status_clock_at(0.3);
    accepted_voice_now(opts, st, 0);
    dsd_call_snapshot call;
    rc |= expect_true("xpt the renamed call publishes as unverified",
                      dsd_call_state_get(st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                          && call.policy_target_id == 88U && call.crc_invalid == (uint8_t)DSD_CRC_RAS_UNVERIFIED);
    status_env_done(st);
    return rc;
}

/* Parked on an XPT carrier while an admitted resident call plays: inside the status's voice window a status naming that
   same carrier is not followed (no retune, no block reset, the live slot's activity kept). */
static int
test_xpt_resident_same_carrier_in_voice_window(dsd_opts* opts, dsd_state* st) {
    uint8_t bits[256];
    uint8_t bytes[48];
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    st->trunk_cc_freq = 461250000L;
    dmr_sm_init(opts, st);
    st->trunk_chan_map[2] = 461250000L; // LSN 2 (TS2) is the parked carrier
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 55U, 5U);
    st->last_vc_sync_time = dsd_decode_time(); // the status's voice window is open
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    const double active_m = sm->slots[1].last_active_m;
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){0U, 3U, 0U, 0U, 0U, 0U},
                          (const uint8_t[6]){0U, 55U, 0U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    int rc = expect_true("xpt resident same-carrier status inside the voice window is not followed",
                         g_status_tunes == 0 && sm->state == DMR_SM_ON_CC);
    rc |=
        expect_true("the live slot's call and activity are kept",
                    sm->slots[1].target == 55U && active_m > 0.0 && fabs(sm->slots[1].last_active_m - active_m) < 1e-9);
    status_env_done(st);
    return rc;
}

/* A Connect Plus preemption to another carrier ends the displaced calls of the carrier left behind (EXPLICIT): TG100
   on slot 0 of the old carrier never stays active beside TG300 on slot 1 of the new one. */
static int
test_con_plus_takeover_ends_displaced_calls(dsd_opts* opts, dsd_state* st) {
    con_plus_env(opts, st);
    seed_policy_row(st, 100, "A", 10, 0);
    seed_policy_row(st, 300, "A", 60, 1);
    con_plus_voice(opts, st, 1, 100, 5, 0, 2);
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0); // publishes TG100 on slot 0
    int rc =
        expect_true("con+ TG100 is active on slot 0", active_call_matches(st, 0U, DSD_CALL_KIND_GROUP_VOICE, 100U, 1U));
    st->last_vc_sync_time = dsd_decode_time();
    con_plus_voice(opts, st, 3, 300, 6, 1, 2);
    rc |= expect_true("con+ TG300 preempts onto slot 1 of another carrier", st->trunk_vc_freq[0] == CP_FB);
    dsd_call_snapshot call;
    rc |= expect_true("con+ the displaced TG100 call ended EXPLICIT",
                      dsd_call_state_get(st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED
                          && call.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);
    status_clock_at(1.2);
    accepted_voice_now(opts, st, 1);
    rc |= expect_true("con+ only TG300 is active",
                      active_call_matches(st, 1U, DSD_CALL_KIND_GROUP_VOICE, 300U, 3U) && no_active_call(st, 0U));
    status_env_done(st);
    return rc;
}

/* A Capacity Plus status preemption ends the displaced call of the carrier left behind, as a grant's does. */
static int
test_cap_plus_takeover_ends_displaced_calls(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 43, "A", 60, 1);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 0U, 0U, 0U});
    status_clock_at(1.0);
    seed_voice_call(st, 0U, DSD_CALL_KIND_GROUP_VOICE, 42U, 7U);
    accepted_voice_now(opts, st, 0);
    status_window_closed(st);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 0U, 43U, 0U});
    int rc = expect_true("cap+ p60 preempts the p10 call", st->trunk_vc_freq[0] == ST_F3);
    rc |= expect_true("cap+ the displaced call is not active on the carrier left", no_active_call(st, 0U));
    status_env_done(st);
    return rc;
}

/* Capacity Plus: a checked LC's identity is its call's. After that call's checked terminator, anonymous voice of a new
   (private) call on the same LSN is not the LC's group: the slot is of unknown identity, the private status may say so
   (the LC no longer outranks it), and a priority-100 preempting group never preempts it as the ended p10 group. Until
   the new call, the ended call's identity holds through its hangtime. */
static int
test_cap_plus_lc_identity_ends_with_its_call(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 43, "A", 100, 1);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 0U, 0U, 0U});
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    status_clock_at(0.5);
    seed_voice_call(st, 0U, DSD_CALL_KIND_GROUP_VOICE, 42U, 7U);
    accepted_voice_now(opts, st, 0);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 42U, 7U);
    status_clock_at(0.9);
    (void)dsd_call_state_end_ex(st, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_TERMINATOR);
    status_clock_at(1.0);
    status_window_closed(st);
    dmr_sm_busy_view_t view;
    (void)dmr_sm_describe_held_calls(sm, opts, st, dsd_decode_now_mono_s(), &view);
    int rc = expect_true("cap+ the ended call's LC identity holds through its hangtime",
                         view.held && !view.unknown_identity && sm->slots[0].target == 42U
                             && sm->slots[0].prov == (uint8_t)DMR_SM_ID_LC);
    status_clock_at(1.2);
    const dsd_call_observation anonymous = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(st, &anonymous, DSD_CALL_BOUNDARY_BEGIN);
    accepted_voice_now(opts, st, 0);
    status_window_closed(st);
    run_cap_plus_private_and_group(opts, st, 1U, 0x1234U, 3U, 43U);
    rc |= expect_true("cap+ the new call is of unknown identity", sm->slots[0].kind == (uint8_t)DSD_CALL_KIND_UNKNOWN);
    rc |= expect_true("cap+ a p100 preempt does not take the new call as the ended p10 group",
                      st->trunk_vc_freq[0] == ST_F1);
    st->tg_hold = 43;
    run_cap_plus_private_and_group(opts, st, 1U, 0x1234U, 3U, 43U);
    rc |= expect_true("cap+ the new call yields to Hold", st->trunk_vc_freq[0] == ST_F3);
    st->tg_hold = 0;
    status_env_done(st);
    return rc;
}

/* Parked on a Capacity Plus carrier (the rest channel) where an admitted resident call plays, a status naming that same
   carrier outside the voice window is followed in place: the receiver does not leave the carrier, so the resident
   call goes on as the same call (its canonical epoch active, its checked LC's identity and follow time kept). */
static int
test_cap_plus_in_place_follow_keeps_resident_call(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = (uint32_t)ST_F1;
    st->trunk_cc_freq = ST_F1;
    dmr_sm_init(opts, st);
    seed_policy_row(st, 77, "A", 10, 0);
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    status_clock_at(1.0);
    seed_voice_call(st, 1U, DSD_CALL_KIND_GROUP_VOICE, 77U, 7U);
    accepted_voice_now(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 77U, 7U);
    dsd_call_snapshot call;
    const uint64_t epoch = dsd_call_state_get(st, 1U, &call) > 0 ? call.epoch : 0U;
    const double follow_m = sm->slots[1].follow_m;
    int rc = expect_true("cap+ the resident call is named on the parked carrier",
                         sm->state == DMR_SM_ON_CC && sm->slots[1].prov == (uint8_t)DMR_SM_ID_LC && epoch != 0U);
    status_clock_at(1.5);
    accepted_voice_now(opts, st, 1);
    status_window_closed(st);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){0U, 77U, 0U, 0U});
    rc |= expect_true("cap+ the parked carrier's call is followed in place",
                      sm->state == DMR_SM_TUNED && sm->vc_freq_hz == ST_F1 && g_status_tunes == 1);
    rc |= expect_true("cap+ the resident call goes on as the same call", dsd_call_state_get(st, 1U, &call) > 0
                                                                             && call.epoch == epoch
                                                                             && call.phase == DSD_CALL_PHASE_ACTIVE);
    rc |= expect_true("cap+ the resident call keeps its LC identity and follow time",
                      sm->slots[1].target == 77U && sm->slots[1].prov == (uint8_t)DMR_SM_ID_LC
                          && fabs(sm->slots[1].follow_m - follow_m) < 1e-9);
    status_env_done(st);
    return rc;
}

/* Capacity Plus: slot 0's call ends and its identity expires past the hangtime while the companion slot keeps the
   carrier busy. A new call on slot 0 is unknown until a status names its LSN: inside the status's voice window (no
   in-place reseed) a status naming a group there names it afresh (the LSN comes from the carrier, not the expired
   identity), so a priority-100 preempting group named by the repeated statuses takes the receiver once that call was
   held for the dwell. */
static int
test_cap_plus_status_names_expired_slot(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 50, "A", 10, 0);
    seed_policy_row(st, 77, "A", 10, 0);
    seed_policy_row(st, 43, "A", 100, 1);
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 77U, 0U, 0U});
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    int rc = expect_true("cap+ both slots seeded",
                         st->trunk_vc_freq[0] == ST_F1 && sm->slots[0].target == 42U && sm->slots[1].target == 77U);
    status_clock_at(0.5);
    seed_voice_call(st, 0U, DSD_CALL_KIND_GROUP_VOICE, 42U, 7U);
    seed_voice_call(st, 1U, DSD_CALL_KIND_GROUP_VOICE, 77U, 8U);
    accepted_voice_now(opts, st, 0);
    accepted_voice_now(opts, st, 1);
    status_clock_at(1.0);
    (void)dsd_call_state_end_ex(st, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_TERMINATOR);
    for (int step = 0; step < 6; step++) {
        status_clock_at(1.0 + 0.5 * step);
        accepted_voice_now(opts, st, 1); // the companion keeps the carrier busy
    }
    status_clock_at(3.6);
    const dsd_call_observation anonymous = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(st, &anonymous, DSD_CALL_BOUNDARY_BEGIN);
    accepted_voice_now(opts, st, 0);
    accepted_voice_now(opts, st, 1);
    rc |= expect_true("cap+ the expired slot's new call is unknown",
                      sm->slots[0].prov == (uint8_t)DMR_SM_ID_NONE && sm->state == DMR_SM_TUNED);
    st->last_vc_sync_time = dsd_decode_time(); // live voice: the status's voice window is open (no in-place reseed)
    run_cap_plus_groups(opts, st, (const uint8_t[4]){50U, 77U, 43U, 0U});
    rc |= expect_true("cap+ a status names the expired slot's new call",
                      sm->slots[0].target == 50U && sm->slots[0].prov == (uint8_t)DMR_SM_ID_SEED
                          && sm->slots[0].channel == 1);
    rc |= expect_true("cap+ the preempting group waits for the named call's dwell", st->trunk_vc_freq[0] == ST_F1);
    status_clock_at(4.5);
    accepted_voice_now(opts, st, 0);
    accepted_voice_now(opts, st, 1);
    st->last_vc_sync_time = dsd_decode_time(); // live voice: the status's voice window is open (no in-place reseed)
    run_cap_plus_groups(opts, st, (const uint8_t[4]){50U, 77U, 43U, 0U});
    rc |=
        expect_true("cap+ a repeated status's p100 preempting group takes the receiver", st->trunk_vc_freq[0] == ST_F3);
    status_env_done(st);
    return rc;
}

/* Capacity Plus: the followed carrier's slot 1 (LSN 2) carries a private call, which no seed names: its record, unknown,
   is the followed carrier's, so once a later status names a group on its LSN (voice keeping the window open) the slot
   takes that group and a priority-100 preempting group elsewhere is judged against it and takes the receiver after the
   dwell. */
static int
test_cap_plus_status_names_anonymous_private_slot(dsd_opts* opts, dsd_state* st) {
    status_env(opts, st);
    seed_policy_row(st, 42, "A", 10, 0);
    seed_policy_row(st, 50, "A", 10, 0);
    seed_policy_row(st, 0x1234, "A", 50, 0);
    seed_policy_row(st, 43, "A", 100, 1);
    run_cap_plus_private_and_group(opts, st, 2U, 0x1234U, 1U, 42U);
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    int rc = expect_true("cap+ follows the private LSN's carrier, slot 1 unnamed",
                         st->trunk_vc_freq[0] == ST_F1 && sm->slots[1].prov == (uint8_t)DMR_SM_ID_NONE);
    status_clock_at(0.5);
    accepted_voice_now(opts, st, 0);
    accepted_voice_now(opts, st, 1);
    st->last_vc_sync_time = dsd_decode_time(); // live voice: the status's voice window is open (no in-place reseed)
    run_cap_plus_private_and_group(opts, st, 2U, 0x1234U, 1U, 42U);
    rc |=
        expect_true("cap+ the private slot's record is the followed carrier's",
                    sm->slots[1].kind == (uint8_t)DSD_CALL_KIND_UNKNOWN && sm->slots[1].prov == (uint8_t)DMR_SM_ID_SEED
                        && sm->slots[1].channel == 2 && sm->slots[1].carrier_hz == ST_F1);
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0);
    accepted_voice_now(opts, st, 1);
    st->last_vc_sync_time = dsd_decode_time();
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 50U, 0U, 0U});
    rc |= expect_true("cap+ a group status names the anonymous slot",
                      sm->slots[1].target == 50U && sm->slots[1].kind == (uint8_t)DSD_CALL_KIND_GROUP_VOICE
                          && sm->slots[1].channel == 2);
    status_clock_at(2.0);
    accepted_voice_now(opts, st, 0);
    accepted_voice_now(opts, st, 1);
    st->last_vc_sync_time = dsd_decode_time();
    run_cap_plus_groups(opts, st, (const uint8_t[4]){42U, 50U, 43U, 0U});
    rc |= expect_true("cap+ a p100 preempting group takes the receiver from the named slot",
                      st->trunk_vc_freq[0] == ST_F3);
    status_env_done(st);
    return rc;
}

/* XPT submits the LSN its ranking judged: parked where a resident private call to 42 (channel 0) plays, the outranking
   group 42 on LSN 3 (slot 0 of another carrier) is the same route the ranking found eligible, so the machine's own
   preemption check never mistakes it for the held private call and refuses it. */
static int
test_xpt_submit_channel_is_the_ranked_lsn(dsd_opts* opts, dsd_state* st) {
    uint8_t bits[256];
    uint8_t bytes[48];
    status_env(opts, st);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 461250000U;
    st->trunk_cc_freq = 461250000L;
    dmr_sm_init(opts, st);
    st->trunk_chan_map[3] = 461287500L;
    /* A group range ranks group 42 p60; the private call to 42 matches no exact row (p0). */
    dsd_tg_policy_entry range;
    if (dsd_tg_policy_make_exact_entry(40U, "A", "range", DSD_TG_POLICY_SOURCE_IMPORTED, &range) != 0) {
        abort();
    }
    range.id_end = 49U;
    range.kind = DSD_TG_POLICY_ROW_RANGE;
    range.priority = 60;
    range.preempt = 1U;
    if (dsd_tg_policy_add_range_entry(st, &range) != 0) {
        abort();
    }
    status_clock_at(1.0);
    accepted_voice_now(opts, st, 0);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_PRIVATE_VOICE, 42U, 7U);
    status_clock_at(2.0);
    accepted_voice_now(opts, st, 0);
    status_window_closed(st);
    build_xpt_site_status(bits, bytes, 0U, 0U, (const uint8_t[6]){0U, 0U, 3U, 0U, 0U, 0U},
                          (const uint8_t[6]){0U, 0U, 42U, 0U, 0U, 0U});
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    int rc = expect_true("xpt the ranked p60 group 42 preempts the resident private call 42",
                         st->trunk_vc_freq[0] == 461287500L && dmr_sm_get_ctx()->vc_lpcn == 3);
    status_env_done(st);
    return rc;
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

    install_trunk_tuning_hooks();
    init_env(opts, st);
    st->trunk_chan_map[lpcn] = freq;
    opts->trunk_use_allow_list = 1;

    build_grant(bits, bytes, 49U, lpcn, 1100U, 2100U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("group unknown blocked in allow-list", opts->trunk_is_tuned == 0);

    rc |= expect_true("seed group allow", seed_exact(st, 1100U, "A", "ALLOW-GRP") == 0);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("group known allowed", opts->trunk_is_tuned == 1 && st->trunk_vc_freq[0] == freq);

    reset_to_cc(opts, st);
    rc |= expect_true("seed group block", seed_exact(st, 1100U, "B", "BLOCK-GRP") == 0);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("group explicit block mode", opts->trunk_is_tuned == 0);

    build_grant(bits, bytes, 48U, lpcn, 9001U, 9002U, 0U);
    reset_to_cc(opts, st);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("private unknown blocked in allow-list", opts->trunk_is_tuned == 0);

    rc |= expect_true("seed private allow source", seed_exact(st, 9002U, "A", "ALLOW-SRC") == 0);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("private known source allowed", opts->trunk_is_tuned == 1 && st->trunk_vc_freq[0] == freq);

    opts->trunk_use_allow_list = 0;
    reset_to_cc(opts, st);
    build_grant(bits, bytes, 50U, lpcn, 1200U, 2200U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("broadcast voice grant is normalized to group", dmr_sm_get_ctx()->vc_tg == 1200);

    reset_to_cc(opts, st);
    build_grant(bits, bytes, 52U, lpcn, 1300U, 2300U, 1U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("data grant enabled for tuning is normalized to group", dmr_sm_get_ctx()->vc_tg == 1300);

    reset_to_cc(opts, st);
    build_absolute_grant(bits, bytes, 49U, 3300U, 4400U, 0U, 88U, 452U, 100U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("absolute grant learns mbc lpcn", st->trunk_chan_map[88] == 452012500L);
    rc |= expect_true("absolute grant marks unconfirmed trust while off cc", st->dmr_lcn_trust[88] == 2U);
    rc |= expect_true("absolute grant publishes mbc lpcn activity",
                      recent_activity_matches(st, 0U, DSD_CALL_KIND_GROUP_VOICE, 3300U, 4400U, 88U,
                                              "Active Group Ch: 0058 (TDMA S1) TG: 3300;"));
    rc |= expect_true("absolute grant dispatches learned frequency",
                      opts->trunk_is_tuned == 1 && st->trunk_vc_freq[0] == 452012500L);

    reset_to_cc(opts, st);
    build_absolute_grant(bits, bytes, 49U, 3310U, 4410U, 0U, 89U, 453U, 200U);
    write_bits_u32(bits, 112U, 3U, 4U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("unknown absolute cdef does not learn lpcn", st->trunk_chan_map[89] == 0);
    rc |= expect_true("unknown absolute cdef does not tune", opts->trunk_is_tuned == 0);

    build_grant(bits, bytes, 49U, 0U, 3320U, 4420U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("invalid zero grant channel does not tune", opts->trunk_is_tuned == 0);

    reset_to_cc(opts, st);
    const uint16_t unmapped_lpcn = 0x0123;
    (void)dsd_recent_activity_clear(st, 0U);
    build_grant(bits, bytes, 49U, unmapped_lpcn, 3325U, 4425U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    rc |= expect_true("unmapped logical grant publishes activity",
                      recent_activity_matches(st, 0U, DSD_CALL_KIND_GROUP_VOICE, 3325U, 4425U, unmapped_lpcn,
                                              "Active Group Ch: 0123 (TDMA S1) TG: 3325;"));
    rc |= expect_true("unmapped logical grant does not tune", opts->trunk_is_tuned == 0 && st->trunk_vc_freq[0] == 0);

    st->trunk_chan_map[lpcn] = freq;
    st->tg_hold = 3330U;
    st->last_vc_sync_time = 123;
    st->last_vc_sync_time_m = 456.0;
    opts->trunk_enable = 0;
    opts->trunk_is_tuned = 0;
    build_grant(bits, bytes, 49U, lpcn, 3330U, 4430U, 0U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
    /* A Hold grant is admitted by its own decision: it no longer wipes the voice-sync stamps the no-carrier return
       reads (that zeroing made a live voice channel look dead). */
    rc |= expect_true("tg hold grant keeps wall-clock vc sync", st->last_vc_sync_time == 123);
    rc |= expect_true("tg hold grant keeps monotonic vc sync", st->last_vc_sync_time_m > 455.9);
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
        .tune_to_freq_request = cap_plus_result_tune_to_freq,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
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
    cap_opts.trunk_tune_group_calls = 0;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    g_result_tune_to_freq_calls = 0;
    build_cap_plus_3e_single_group(bits, bytes, 1U, 1U, 42U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |= expect_true("cap+ disabled group preserves decoded target",
                      recent_activity_matches(&cap_st, 1U, DSD_CALL_KIND_GROUP_VOICE, 42U, 0U, 1U, "LSN:1 TG:42;"));
    rc |= expect_true("cap+ LSN 1 publishes slot zero", recent_activity_has_slot(&cap_st, 1U, 0U));
    rc |= expect_true("cap+ disabled group does not tune", g_result_tune_to_freq_calls == 0);

    init_env(&cap_opts, &cap_st);
    cap_opts.trunk_tune_group_calls = 0;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    build_cap_plus_3e_single_group(bits, bytes, 1U, 2U, 43U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |= expect_true("cap+ LSN 2 preserves decoded target",
                      recent_activity_matches(&cap_st, 2U, DSD_CALL_KIND_GROUP_VOICE, 43U, 0U, 2U, "LSN:2 TG:43;"));
    rc |= expect_true("cap+ LSN 2 publishes slot one", recent_activity_has_slot(&cap_st, 2U, 1U));

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
    rc |= expect_true("cap+ 3e private activity", recent_activity_matches(&cap_st, 3U, DSD_CALL_KIND_PRIVATE_VOICE,
                                                                          4242U, 0U, 3U, "LSN:3 PC:4242;"));
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
    rc |= expect_true("cap+ 3e private disabled omits activity", recent_activity_is_empty(&cap_st, 3U));

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

    // Each active LSN's talkgroup is where the banks put it, whichever LSNs the status window shows: with only LSN 13
    // active and the rest on 13 the window starts at LSN 13, and bank two's bitmap byte (32..39) precedes its talkgroup.
    init_env(&cap_opts, &cap_st);
    cap_opts.trunk_tune_group_calls = 0;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 0x3EU;
    bytes[1] = 0x10U;
    write_bits_u32(bits, 16U, 3U, 2U);
    write_bits_u32(bits, 19U, 13U, 5U);
    write_bits_u32(bits, 32U, 0x08U, 8U); // bank two: LSN 13
    write_bits_u32(bits, 40U, 109U, 8U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |=
        expect_true("cap+ 3e window start 13 reads LSN 13's talkgroup",
                    recent_activity_matches(&cap_st, 13U, DSD_CALL_KIND_GROUP_VOICE, 109U, 0U, 13U, "LSN:13 TG:109;"));

    // A zero talkgroup byte still belongs to its LSN: the next LSN's talkgroup is the byte after it.
    init_env(&cap_opts, &cap_st);
    cap_opts.trunk_tune_group_calls = 0;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 0x3EU;
    bytes[1] = 0x10U;
    write_bits_u32(bits, 16U, 3U, 2U);
    write_bits_u32(bits, 19U, 1U, 5U);
    write_bits_u32(bits, 24U, 0x60U, 8U); // LSNs 2 and 3
    write_bits_u32(bits, 32U, 0U, 8U);
    write_bits_u32(bits, 40U, 214U, 8U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |= expect_true("cap+ 3e zero talkgroup keeps the next LSN aligned",
                      recent_activity_matches(&cap_st, 3U, DSD_CALL_KIND_GROUP_VOICE, 214U, 0U, 3U, "LSN:3 TG:214;"));

    // Bank-two private IDs follow bank two's flag and bitmap bytes, as bank one's do. Trunking off: decoded, not
    // followed.
    init_env(&cap_opts, &cap_st);
    cap_opts.trunk_enable = 0;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = 0x3EU;
    bytes[1] = 0x10U;
    write_bits_u32(bits, 16U, 2U, 2U); // initial block: status bits 0..79
    write_bits_u32(bits, 19U, 1U, 5U);
    write_bits_u32(bits, 40U, 0x80U, 8U); // bank one private flag
    write_bits_u32(bits, 48U, 0x40U, 8U); // LSN 2
    write_bits_u32(bits, 56U, 4242U, 16U);
    write_bits_u32(bits, 72U, 0x80U, 8U); // bank two private flag
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    DSD_MEMSET(bits, 0, 256);
    write_bits_u32(bits, 16U, 1U, 2U); // final block: its bits 24..79 are status bits 80..135
    write_bits_u32(bits, 19U, 1U, 5U);
    write_bits_u32(bits, 24U, 0x20U, 8U); // bank two bitmap: LSN 11
    write_bits_u32(bits, 32U, 5151U, 16U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |= expect_true("cap+ 3e bank-one private ID", recent_activity_matches(&cap_st, 2U, DSD_CALL_KIND_PRIVATE_VOICE,
                                                                             4242U, 0U, 2U, "LSN:2 PC:4242;"));
    rc |= expect_true("cap+ 3e bank-two private ID", recent_activity_matches(&cap_st, 11U, DSD_CALL_KIND_PRIVATE_VOICE,
                                                                             5151U, 0U, 11U, "LSN:11 PC:5151;"));

    // The status's rest LSN is five bits: LSN 16 is reachable, and the rest channel is followed there.
    init_env(&cap_opts, &cap_st);
    cap_st.trunk_cc_freq = 851000000L;
    cap_st.trunk_chan_map[16] = 852500000L;
    cap_st.last_vc_sync_time = time(NULL) - 10;
    build_cap_plus_3e_single_idle(bits, bytes, 0U);
    write_bits_u32(bits, 19U, 16U, 5U);
    dmr_cspdu(&cap_opts, &cap_st, bits, bytes, 1U, 0U);
    rc |= expect_true("cap+ 3e rest LSN 16", cap_st.dmr_rest_channel == 16 && cap_st.trunk_cc_freq == 852500000L);
    install_trunk_tuning_hooks();

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
    rc |= expect_true("con+ voice activity slot",
                      recent_activity_matches(&con_st, 1U, DSD_CALL_KIND_GROUP_VOICE, 0x445566U, 0x112233U, 5U,
                                              "Active Ch: 0005 (TDMA S2) TG: 4478310;"));
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
    rc |= expect_true("con+ private voice activity slot",
                      recent_activity_matches(&con_st, 0U, DSD_CALL_KIND_PRIVATE_VOICE, 0x003344U, 0x001122U, 6U,
                                              "Active Ch: 0006 (TDMA S1) TG: 13124;"));

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_group_calls = 0;
    con_st.trunk_chan_map[5] = 855125000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    build_con_plus_voice(bits, bytes, 0x112233U, 0x445566U, 5U, 1U, 2U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ group disabled keeps grant activity visible",
                      recent_activity_matches(&con_st, 1U, DSD_CALL_KIND_GROUP_VOICE, 0x445566U, 0x112233U, 5U,
                                              "Active Ch: 0005 (TDMA S2) TG: 4478310;"));
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
        .tune_to_freq_request = cap_plus_result_tune_to_freq,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ data tune hook called", g_result_tune_to_freq_calls == 1);
    rc |= expect_true("con+ data tune updates VC", con_st.trunk_vc_freq[0] == 856250000L);
    rc |= expect_true("con+ data reset blocks", g_dmr_reset_blocks_calls == 1);
    rc |= expect_true("con+ data activity slot", recent_activity_matches(&con_st, 0U, DSD_CALL_KIND_DATA, 0x00CAFEU, 0U,
                                                                         4U, "Active Ch: 0004 (TDMA S1) TG: 51966;"));
    rc |= expect_true("con+ data marks flavor", con_st.is_con_plus == 1);

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_data_calls = 0;
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ data disabled suppresses tune", con_opts.trunk_is_tuned == 0 && con_st.is_con_plus == 0);
    rc |= expect_true("con+ data disabled still brands", strcmp(con_st.dmr_branding_sub, "Con+ ") == 0);
    rc |= expect_true("con+ data disabled keeps grant activity",
                      recent_activity_matches(&con_st, 0U, DSD_CALL_KIND_DATA, 0x00CAFEU, 0U, 4U,
                                              "Active Ch: 0004 (TDMA S1) TG: 51966;"));

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_data_calls = 1;
    con_opts.trunk_use_allow_list = 1;
    con_opts.verbose = 1;
    con_st.trunk_chan_map[4] = 856250000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ data policy block keeps grant activity visible",
                      recent_activity_matches(&con_st, 0U, DSD_CALL_KIND_DATA, 0x00CAFEU, 0U, 4U,
                                              "Active Ch: 0004 (TDMA S1) TG: 51966;"));
    rc |=
        expect_true("con+ data policy block suppresses tune", con_opts.trunk_is_tuned == 0 && con_st.is_con_plus == 0);

    init_env(&con_opts, &con_st);
    con_opts.trunk_tune_data_calls = 1;
    con_st.trunk_chan_map[4] = 856250000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    const dsd_call_observation previous_activity = {
        .protocol = DSD_SYNC_DMR_BS_DATA_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_DATA,
        .ota_target_id = 777U,
        .channel = 9U,
    };
    (void)dsd_recent_activity_publish(&con_st, 0U, &previous_activity, "previous active", 0U);
    g_fail_tune_to_freq_calls = 0;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = fail_result_tune_to_freq,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
    build_con_plus_data(bits, bytes, 0x00CAFEU, 4U, 0U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ data failed tune hook called", g_fail_tune_to_freq_calls == 1);
    rc |= expect_true("con+ data rollback restores activity",
                      recent_activity_matches(&con_st, 0U, DSD_CALL_KIND_DATA, 777U, 0U, 9U, "previous active"));
    rc |= expect_true("con+ data failed tune does not mark flavor", con_st.is_con_plus == 0);
    install_trunk_tuning_hooks();

    init_env(&con_opts, &con_st);
    con_st.trunk_chan_map[5] = 855125000L;
    con_st.last_vc_sync_time = time(NULL) - 10;
    build_con_plus_voice(bits, bytes, 0x112233U, 0x445566U, 5U, 1U, 2U);
    dmr_cspdu(&con_opts, &con_st, bits, bytes, 1U, 0U);
    rc |= expect_true("con+ termination setup tunes vc",
                      con_opts.trunk_is_tuned == 1 && dmr_sm_get_ctx()->vc_tg == 0x445566);
    test_accepted_voice_sync(&con_opts, &con_st, 0);
    test_accepted_voice_sync(&con_opts, &con_st, 1);
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
    rc |= expect_true("group protect does not create a voice call", no_active_call(&pf0_st, 0U));

    init_env(&pf0_opts, &pf0_st);
    pf0_st.currentslot = 1;
    build_p_protect(bits, bytes, 1U, 0U, 0x005555U, 0x006666U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("private protect marks slot 2 voice burst", pf0_st.dmrburstR == 1);
    rc |= expect_true("private protect does not create a voice call", no_active_call(&pf0_st, 1U));

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
    build_c_ahoy(bits, bytes, 1U, 4U, 0x009999U, 0x000777U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_ahoy group does not create a voice call", no_active_call(&pf0_st, 1U));

    init_env(&pf0_opts, &pf0_st);
    pf0_st.currentslot = 0;
    build_c_ahoy(bits, bytes, 0U, 2U, 0x008888U, 0x000666U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_ahoy private does not create a voice call", no_active_call(&pf0_st, 0U));

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
    build_c_move(bits, bytes, 12U, 0U, 0x001234U, 0x005678U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_move slot 1 activity",
                      recent_activity_matches(&pf0_st, 0U, DSD_CALL_KIND_GROUP_VOICE, 0x001234U, 0x005678U, 12U,
                                              "Active Ch: 000C (TDMA S1) TG: 4660;"));
    rc |= expect_true("c_move slot 1 does not create a call", no_active_call(&pf0_st, 0U));
    rc |= expect_true("c_move slot 1 debounces opposite slot", pf0_st.dmrburstL == 16 && pf0_st.dmrburstR == 9);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.trunk_enable = 0;
    build_c_move(bits, bytes, 13U, 1U, 0x002345U, 0x006789U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_move slot 2 activity",
                      recent_activity_matches(&pf0_st, 1U, DSD_CALL_KIND_GROUP_VOICE, 0x002345U, 0x006789U, 13U,
                                              "Active Ch: 000D (TDMA S2) TG: 9029;"));
    rc |= expect_true("c_move slot 2 does not create a call", no_active_call(&pf0_st, 1U));
    rc |= expect_true("c_move slot 2 debounces opposite slot", pf0_st.dmrburstR == 16 && pf0_st.dmrburstL == 9);

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.trunk_enable = 0;
    pf0_st.currentslot = 0;
    pf0_st.dmrburstL = 6;
    pf0_st.dmrburstR = 6;
    seed_voice_call(&pf0_st, 0U, DSD_CALL_KIND_GROUP_VOICE, 4400U, 3300U);
    build_p_clear(bits, bytes, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("p_clear trunk disabled leaves slot 1 burst", pf0_st.dmrburstL == 6);
    rc |= expect_true("p_clear trunk disabled leaves opposite burst", pf0_st.dmrburstR == 6);
    rc |= expect_true("p_clear trunk disabled leaves canonical slot 1 call",
                      active_call_matches(&pf0_st, 0U, DSD_CALL_KIND_GROUP_VOICE, 4400U, 3300U));

    init_env(&pf0_opts, &pf0_st);
    pf0_opts.trunk_is_tuned = 1;
    pf0_st.currentslot = 1;
    pf0_st.dmrburstL = 6;
    pf0_st.dmrburstR = 16;
    const dsd_call_observation slot_two_data = dsd_call_observation_data(DSD_SYNC_DMR_BS_DATA_POS, 1U, 0U, 9900U);
    (void)dsd_recent_activity_publish(&pf0_st, 1U, &slot_two_data, "slot two data channel", 1U);
    build_p_clear(bits, bytes, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("p_clear data clears slot 2 burst state", pf0_st.dmrburstR == 9);
    rc |= expect_true("p_clear data leaves companion burst hint alone", pf0_st.dmrburstL == 6);
    rc |= expect_true("p_clear data clears slot 2 activity", recent_activity_is_empty(&pf0_st, 1U));
    rc |= expect_true("p_clear data does not create a call", no_active_call(&pf0_st, 1U));
    rc |= expect_true("p_clear data does not force release", pf0_st.trunk_sm_force_release == 0);
    rc |= expect_true("p_clear data does not return to cc", pf0_st.p25_sm_release_count == 0);

    // A P_CLEAR for one slot must not force-release the channel while the
    // companion slot's canonical call epoch is ACTIVE, whatever the
    // companion's burst hint reads: the hint records the last burst decoded,
    // and a slot mid-call sits on TLC/data/stale values between voice bursts.
    static const unsigned int companion_hints[] = {2U, 6U, 9U, 15U, 17U};
    for (size_t h = 0; h < sizeof(companion_hints) / sizeof(companion_hints[0]); h++) {
        init_env(&pf0_opts, &pf0_st);
        pf0_opts.trunk_is_tuned = 1;
        pf0_st.currentslot = 0;
        pf0_st.dmrburstL = 9;
        pf0_st.dmrburstR = companion_hints[h];
        seed_voice_call(&pf0_st, 1U, DSD_CALL_KIND_GROUP_VOICE, 7700U, 8800U);
        build_p_clear(bits, bytes, 0U);
        dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
        rc |= expect_true("p_clear active companion does not force release", pf0_st.trunk_sm_force_release == 0);
        rc |= expect_true("p_clear active companion does not return to cc", pf0_st.p25_sm_release_count == 0);
        rc |= expect_true("p_clear active companion keeps its burst hint", pf0_st.dmrburstR == companion_hints[h]);
        rc |= expect_true("p_clear active companion call stays active",
                          active_call_matches(&pf0_st, 1U, DSD_CALL_KIND_GROUP_VOICE, 7700U, 8800U));
    }

    init_env(&pf0_opts, &pf0_st);
    pf0_st.trunk_chan_map[lpcn] = freq;
    pf0_st.last_vc_sync_time = time(NULL) - 10;
    build_grant(bits, bytes, 49U, lpcn, 5500U, 6500U, 1U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("p_clear hold setup tunes vc", pf0_opts.trunk_is_tuned == 1 && dmr_sm_get_ctx()->vc_tg == 5500);
    pf0_st.currentslot = 1;
    seed_voice_call(&pf0_st, 1U, DSD_CALL_KIND_GROUP_VOICE, 5500U, 6500U);
    pf0_st.tg_hold = 5500U;
    pf0_st.dmrburstL = 16;
    pf0_st.dmrburstR = 16;
    const dsd_call_observation slot_two_voice_activity = {
        .protocol = DSD_SYNC_DMR_BS_DATA_POS,
        .slot = 1U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 5500U,
        .ota_source_id = 6500U,
    };
    (void)dsd_recent_activity_publish(&pf0_st, 1U, &slot_two_voice_activity, "slot two voice channel", 1U);
    g_return_to_cc_result_calls = 0;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = result_return_to_cc,
    });
    build_p_clear(bits, bytes, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    install_trunk_tuning_hooks();
    rc |= expect_true("p_clear hold forced return hook called", g_return_to_cc_result_calls == 1);
    rc |= expect_true("p_clear hold clears force latch", pf0_st.trunk_sm_force_release == 0);
    rc |= expect_true("p_clear hold returns to cc", pf0_opts.trunk_is_tuned == 0 && pf0_st.p25_sm_release_count == 1);
    rc |= expect_true("p_clear hold clears slot 2 activity", recent_activity_is_empty(&pf0_st, 1U));
    pf0_st.tg_hold = 0;

    init_env(&pf0_opts, &pf0_st);
    build_c_bcast_chan_freq(bits, bytes, 77U, 451U, 100U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_bcast channel frequency learned", pf0_st.trunk_chan_map[77] == 451012500L);
    rc |= expect_true("c_bcast channel frequency tracked", pf0_st.trunk_lcn_freq[0] == 451012500L);
    rc |= expect_true("c_bcast first learned frequency sets count", pf0_st.lcn_freq_count == 1);

    /* Distinct announcements append rather than writing modulo a fixed window, and a repeat of
     * an already-tracked frequency is not appended twice. */
    build_c_bcast_chan_freq(bits, bytes, 78U, 451U, 300U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_bcast second frequency appended", pf0_st.trunk_lcn_freq[1] == 451037500L);
    rc |= expect_true("c_bcast second frequency raises count", pf0_st.lcn_freq_count == 2);
    build_c_bcast_chan_freq(bits, bytes, 79U, 451U, 300U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("c_bcast duplicate frequency not appended", pf0_st.lcn_freq_count == 2);
    dsd_state_ext_free_all(&pf0_st);

    /* An imported channel map is positional operator intent: a site broadcast must neither write
     * into it nor lower lcn_freq_count to the handful of slots the broadcast knows about. Slot 1
     * holds the deliberate 0 an unparseable CSV row leaves behind to keep LCN numbering. */
    init_env(&pf0_opts, &pf0_st);
    DSD_SNPRINTF(pf0_opts.chan_in_file, sizeof pf0_opts.chan_in_file, "%s", "imported.csv");
    for (int i = 0; i < 40; i++) {
        rc |= expect_true("imported list reserve", dsd_state_trunk_lcn_reserve(&pf0_st, (size_t)i + 1U) == 0);
        *dsd_state_trunk_lcn_slot(&pf0_st, i) = (i == 1) ? 0L : (460000000L + (long)i * 12500L);
    }
    pf0_st.lcn_freq_count = 40;
    build_c_bcast_chan_freq(bits, bytes, 77U, 451U, 100U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    rc |= expect_true("imported map still learns the channel", pf0_st.trunk_chan_map[77] == 451012500L);
    rc |= expect_true("imported map keeps its length", pf0_st.lcn_freq_count == 40);
    int imported_intact = 1;
    for (int i = 0; i < 40; i++) {
        const long want = (i == 1) ? 0L : (460000000L + (long)i * 12500L);
        if (*dsd_state_trunk_lcn_slot(&pf0_st, i) != want) {
            DSD_FPRINTF(stderr, "imported map slot %d clobbered: got %ld want %ld\n", i,
                        *dsd_state_trunk_lcn_slot(&pf0_st, i), want);
            imported_intact = 0;
        }
    }
    rc |= expect_true("imported map entries untouched", imported_intact);
    pf0_opts.chan_in_file[0] = '\0';
    dsd_state_trunk_lcn_free(&pf0_st);
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
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = result_return_to_cc,
    });
    build_c_bcast_ann_wd_tscc(bits, bytes, 11U, 22U, 1U, 0U);
    dmr_cspdu(&pf0_opts, &pf0_st, bits, bytes, 1U, 0U);
    install_trunk_tuning_hooks();
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
    rc |= expect_true("xpt site status records active TG activity",
                      recent_activity_matches(&xpt_st, 0U, DSD_CALL_KIND_DATA, 0U, 0U, 0U, "LSN:1 TG:77;"));
    rc |= expect_true("xpt site status records second active TG activity",
                      recent_activity_matches(&xpt_st, 0U, DSD_CALL_KIND_DATA, 0U, 0U, 0U, "LSN:5 TG:99;"));
    rc |= expect_true("xpt site status records active private activity",
                      recent_activity_matches(&xpt_st, 0U, DSD_CALL_KIND_DATA, 0U, 0U, 0U, "LSN:2 PC:88;"));
    rc |= expect_true("xpt site status sets rtl control channel",
                      xpt_opts.trunk_is_tuned == 1 && xpt_st.trunk_cc_freq == 461250000L);
    rc |= expect_true("xpt site status sets branding", strcmp(xpt_st.dmr_branding_sub, "XPT ") == 0);
    rc |= expect_true("xpt site status emits grant", dmr_sm_get_ctx()->vc_tg == 77);
    dsd_state_ext_free_all(&xpt_st);

    /* Issue #637: a candidate the receive policy rejects is passed over, so an allowed TS2 call after a rejected TS1
       one is followed. */
    init_env(&xpt_opts, &xpt_st);
    xpt_opts.audio_in_type = AUDIO_IN_RTL;
    xpt_opts.rtlsdr_center_freq = 461250000L;
    xpt_st.trunk_chan_map[1] = 461262500L;
    xpt_st.trunk_chan_map[2] = 461262500L;
    xpt_st.last_vc_sync_time = time(NULL) - 10;
    rc |= expect_true("slot allow parsed",
                      dsd_receive_edit_parse(&xpt_opts.receive, DSD_RECEIVE_SLOT, 1, "2", "", "", NULL, 0) == 0);
    uint8_t xpt_two_status[6] = {3U, 3U, 0U, 0U, 0U, 0U};
    uint8_t xpt_two_tg[6] = {77U, 66U, 0U, 0U, 0U, 0U};
    build_xpt_site_status(bits, bytes, 0U, 0U, xpt_two_status, xpt_two_tg);
    dmr_cspdu(&xpt_opts, &xpt_st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt follows the allowed TS2 call past the rejected TS1 one", dmr_sm_get_ctx()->vc_tg == 66);
    DSD_MEMSET(&xpt_opts.receive, 0, sizeof xpt_opts.receive);
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
    /* An XPT private status names no radio: the grant carries an unknown private identity, not the placeholder group 1
       it used to be followed as. */
    rc |= expect_true("xpt seq1 status-only private follows an unknown private identity",
                      dmr_sm_get_ctx()->vc_is_group == 0 && dmr_sm_get_ctx()->vc_dst == 0
                          && dmr_sm_get_ctx()->vc_tg == 0 && dmr_sm_get_ctx()->vc_identity_known == 0);
    rc |= expect_true("xpt seq1 private uses banked LSN map", xpt_st.trunk_vc_freq[0] == 462012500L);
    rc |= expect_true("xpt seq1 learns rtl control channel", xpt_st.trunk_cc_freq == 462000000L);
    rc |= expect_true("xpt seq1 rotates symbols before tune", g_rotate_symbol_out_file_calls == 1);
    rc |= expect_true("xpt seq1 publishes empty bank activity",
                      recent_activity_matches(&xpt_st, 1U, DSD_CALL_KIND_DATA, 0U, 0U, 0U, NULL));
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
    rc |= expect_true("xpt seq2 records banked active TG activity",
                      recent_activity_matches(&xpt_st, 2U, DSD_CALL_KIND_DATA, 0U, 0U, 0U, "LSN:13 TG:55;"));
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
    rc |= expect_true("xpt allow-list block keeps active TG activity visible",
                      recent_activity_matches(&xpt_st, 0U, DSD_CALL_KIND_DATA, 0U, 0U, 0U, "LSN:1 TG:44;"));
    rc |= expect_true("xpt allow-list block suppresses grant", dmr_sm_get_ctx()->vc_tg == 0);
    rc |= expect_true("xpt allow-list block does not tune VC", xpt_st.trunk_vc_freq[0] == 0);
    dsd_state_ext_free_all(&xpt_st);

    init_env(&xpt_opts, &xpt_st);
    uint8_t xpt_sites[4] = {3U, 4U, 0U, 7U};
    uint8_t xpt_free[4] = {2U, 5U, 0U, 9U};
    build_xpt_adjacent(bits, bytes, 2U, xpt_sites, xpt_free);
    dmr_cspdu(&xpt_opts, &xpt_st, bits, bytes, 1U, 0U);
    rc |= expect_true("xpt adjacent sets branding", strcmp(xpt_st.dmr_branding_sub, "XPT ") == 0);

    rc |= test_receive_data_grant(opts, st);
    rc |= test_receive_rejected_carrier_grant(opts, st);
    rc |= test_receive_csbk_tuned_carrier_window(opts, st);
    rc |= test_receive_vc_code_differs_from_cc(opts, st);
    rc |= test_cap_plus_allow_list_does_not_hop(opts, st);
    rc |= test_cap_plus_preempt_ranking(opts, st);
    rc |= test_cap_plus_seed_only_carrier(opts, st);
    rc |= test_cap_plus_seed_receive_slot_admission(opts, st);
    rc |= test_cap_plus_hold(opts, st);
    rc |= test_cap_plus_seed_identity_reconcile(opts, st);
    rc |= test_cap_plus_seed_private_activity(opts, st);
    rc |= test_cap_plus_acquisition_unknown_members(opts, st);
    rc |= test_xpt_entry_kinds(opts, st);
    rc |= test_xpt_ranking(opts, st);
    rc |= test_con_plus_policy_before_window(opts, st);
    rc |= test_xpt_busy_carrier(opts, st);
    rc |= test_xpt_status_reconciles_followed_slot(opts, st);
    rc |= test_xpt_status_in_place_keeps_lc_identity(opts, st);
    rc |= test_status_repeat_during_fade_keeps_binding(opts, st, 0);
    rc |= test_status_repeat_during_fade_keeps_binding(opts, st, 1);
    rc |= test_xpt_status_after_unverified_terminator_awaits_next(opts, st);
    rc |= test_xpt_status_reassignment_carries_crc_provenance(opts, st);
    rc |= test_xpt_resident_same_carrier_in_voice_window(opts, st);
    rc |= test_con_plus_takeover_ends_displaced_calls(opts, st);
    rc |= test_cap_plus_takeover_ends_displaced_calls(opts, st);
    rc |= test_cap_plus_lc_identity_ends_with_its_call(opts, st);
    rc |= test_xpt_submit_channel_is_the_ranked_lsn(opts, st);
    rc |= test_cap_plus_in_place_follow_keeps_resident_call(opts, st);
    rc |= test_cap_plus_status_names_expired_slot(opts, st);
    rc |= test_cap_plus_status_names_anonymous_private_slot(opts, st);
    if (rc == 0) {
        printf("DMR_GRANT_POLICY: OK\n");
    }
    dsd_state_ext_free_all(&cap_st);
    dsd_state_ext_free_all(&con_st);
    dsd_state_ext_free_all(&pf0_st);
    dsd_state_ext_free_all(&xpt_st);
    free_test_state(st);
    free(opts);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
