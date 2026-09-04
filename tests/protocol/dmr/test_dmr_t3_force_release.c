// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Exercise P_CLEAR with TG Hold override forcing immediate SM release.
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Stubs and helpers (same pattern as other DMR tests)
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
rotate_symbol_out_file(dsd_opts* o, dsd_state* s) {
    (void)o;
    (void)s;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* o, dsd_state* s) {
    (void)o;
    (void)s;
}

uint8_t
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

static dsd_trunk_tune_result
test_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->trunk_is_tuned = 1;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

static void
init_env(dsd_opts* o, dsd_state* s) {
    DSD_MEMSET(o, 0, sizeof(*o));
    DSD_MEMSET(s, 0, sizeof(*s));
    o->trunk_enable = 1;
    s->trunk_cc_freq = 851000000;
}

static void
build_pclear(uint8_t* bits, uint8_t* bytes) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = (uint8_t)(46 & 0x3F);
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    static dsd_opts opts;
    static dsd_state state;
    init_env(&opts, &state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_to_freq,
        .return_to_cc_request = test_return_to_cc,
    });
    // 1) Tune to VC via SM grant call
    dmr_sm_emit_group_grant(&opts, &state, /*freq_hz*/ 852000000, /*lpcn*/ 0, /*tg*/ 1234, /*src*/ 42);
    assert(opts.trunk_is_tuned == 1);
    // Model the subsequently decoded voice call: grants are recent activity only.
    const dsd_call_observation call = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1234,
        .policy_target_id = 1234,
        .ota_source_id = 42,
        .frequency_hz = 852000000,
    };
    assert(dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) > 0);
    // Set TG Hold to match active TG; ensure slot 0 context
    state.tg_hold = 1234;
    state.currentslot = 0;
    // 2) P_CLEAR should force release via SM (bypass hangtime/activity)
    uint8_t bits[256], bytes[48];
    build_pclear(bits, bytes);
    dmr_cspdu(&opts, &state, bits, bytes, 1, 0);
    assert(opts.trunk_is_tuned == 0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    printf("DMR_T3_FORCE_RELEASE: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
