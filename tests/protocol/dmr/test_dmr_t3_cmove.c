// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify C_MOVE handling: retunes only while on VC and updates to new VC.
 * Also verify that a CRC-valid CSBK anchors an unknown control channel at the
 * radio's tuned frequency in a build without librtlsdr (SoapySDR or Airspy only).
 * A move of the followed call is matched whatever slot it names, keeps the call's follow time and is refused only
 * when the call's own classification is now denied tuning (not for a Hold mismatch or a media-only restriction); any
 * other move is an ordinary grant under the policy and the busy gate. Its burst-indicator and companion-row effects
 * apply only once accepted.
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RTLSDR
#error "This test must compile dmr_csbk.c without USE_RTLSDR."
#endif

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// --- Stubs for external symbols referenced by dmr_csbk.c ---

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
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

// CRC8 helper used in CSBK path (provide a trivial non-zero value)
uint8_t
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

// --- Helpers ---
static void
init_env(dsd_opts* opts, dsd_state* state) {
    // Free what an earlier step attached (the call-state extension) before the wipe drops its pointer
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    // The followed call is a group call: a C_MOVE of it is judged with its own classification, as it was followed.
    opts->trunk_tune_group_calls = 1;
    state->trunk_cc_freq = 851000000; // mock CC
}

// Compose a minimal C_MOVE CSBK + MBC absolute channel parms into bit and byte arrays
static void
build_cmove_apcn(uint8_t* bits, uint8_t* bytes, uint16_t apcn, uint16_t rx_int_mhz, uint16_t rx_step_125hz,
                 uint8_t slot, uint32_t target) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    // Opcode (low 6 bits of first byte) = 57
    bytes[0] = (uint8_t)(57 & 0x3F);
    // LPCN field (bits 16..27) = 0xFFF (absolute)
    for (int i = 0; i < 12; i++) {
        bits[16 + i] = ((0xFFF >> (11 - i)) & 1);
    }
    // Slot bit (bit 28)
    bits[28] = (slot & 1);
    // Target (bits 32..55): the call the move is for
    for (int i = 0; i < 24; i++) {
        bits[32 + i] = (uint8_t)((target >> (23 - i)) & 1u);
    }
    // MBC CDEFTYPE (bits 112..115) = 0 (absolute channel parms)
    // already zeroed
    // APCN (bits 118..129)
    for (int i = 0; i < 12; i++) {
        bits[118 + i] = ((apcn >> (11 - i)) & 1);
    }
    // RX_INT (bits 153..162) 10 bits
    for (int i = 0; i < 10; i++) {
        bits[153 + i] = ((rx_int_mhz >> (9 - i)) & 1);
    }
    // RX_STEP (bits 163..175) 13 bits
    for (int i = 0; i < 13; i++) {
        bits[163 + i] = ((rx_step_125hz >> (12 - i)) & 1);
    }
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

/* ---- Move admission and commit ---- */

#define MV_F1    852000000L
#define MV_F2    853000000L
#define MV_F3    854000000L
#define MV_LPCN1 0x0010U
#define MV_LPCN2 0x0011U
#define MV_LPCN3 0x0012U

static const uint64_t k_t0_ns = 1000000ULL * 1000000000ULL;

static void
clock_at(double seconds) {
    dsd_decode_clock_test_set_ns(k_t0_ns + (uint64_t)(seconds * 1e9));
}

static void
put_bits(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

static void
csbk_grant_or_move(dsd_opts* opts, dsd_state* state, uint8_t opcode, uint16_t lpcn, uint8_t slot, uint32_t target,
                   uint32_t source) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = opcode;
    put_bits(bits, 16U, lpcn, 12U);
    bits[28] = (uint8_t)(slot & 1U);
    put_bits(bits, 32U, target, 24U);
    put_bits(bits, 56U, source, 24U);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

static void
move_env(dsd_opts* opts, dsd_state* state) {
    init_env(opts, state);
    opts->trunk_tune_private_calls = 1;
    opts->trunk_hangtime = 2.0f;
    state->trunk_chan_map[MV_LPCN1] = MV_F1;
    state->trunk_chan_map[MV_LPCN2] = MV_F2;
    state->trunk_chan_map[MV_LPCN3] = MV_F3;
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    clock_at(0.0);
    dmr_sm_init(opts, state);
}

static void
seed_row(dsd_state* state, uint32_t id, const char* mode, int priority, int preempt) {
    dsd_tg_policy_entry row;
    assert(dsd_tg_policy_make_exact_entry(id, mode, "ROW", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0);
    row.priority = priority;
    row.preempt = preempt ? 1U : 0U;
    assert(dsd_tg_policy_upsert_exact(state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
}

static void
accepted_voice(dsd_opts* opts, dsd_state* state, int slot) {
    state->dmr_confidence_voice_open[slot] = 1;
    state->dmr_confidence_voice_m[slot] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(opts, state, slot);
}

static int
recent_row_target(const dsd_state* state, uint8_t index, uint64_t* target) {
    dsd_recent_activity_snapshot recent;
    if (dsd_recent_activity_copy_snapshot(state, &recent) <= 0
        || recent.entries[index].observation.kind == DSD_CALL_KIND_UNKNOWN) {
        return 0;
    }
    *target = recent.entries[index].observation.ota_target_id;
    return 1;
}

/* A slot-changing move of the followed call keeps its follow time: a preemption candidate's dwell still runs from
   the grant. */
static void
test_move_keeps_follow_time(dsd_opts* opts, dsd_state* state) {
    move_env(opts, state);
    seed_row(state, 101, "A", 10, 0);
    seed_row(state, 300, "A", 60, 1);
    csbk_grant_or_move(opts, state, 49U, MV_LPCN1, 0, 101, 1234);
    assert(state->trunk_vc_freq[0] == MV_F1);
    clock_at(0.6);
    accepted_voice(opts, state, 0);
    csbk_grant_or_move(opts, state, 57U, MV_LPCN2, 1, 101, 1234);
    assert(state->trunk_vc_freq[0] == MV_F2);
    assert(dmr_sm_get_ctx()->vc_slot == 1 && dmr_sm_get_ctx()->vc_tg == 101);
    clock_at(0.8);
    csbk_grant_or_move(opts, state, 49U, MV_LPCN3, 0, 300, 3);
    assert(state->trunk_vc_freq[0] == MV_F3 && dmr_sm_get_ctx()->vc_tg == 300);
}

/* A refused move (a blocked talkgroup; the companion's call while the followed one is held) leaves the burst indicators
   and the other slot's activity row alone. */
static void
test_refused_moves_commit_nothing(dsd_opts* opts, dsd_state* state) {
    move_env(opts, state);
    seed_row(state, 777, "B", 0, 0);
    csbk_grant_or_move(opts, state, 49U, MV_LPCN1, 0, 101, 1234);
    clock_at(0.5);
    accepted_voice(opts, state, 0);
    const dsd_call_observation companion = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS, .slot = 1U, .kind = DSD_CALL_KIND_GROUP_VOICE, .ota_target_id = 202U};
    (void)dsd_recent_activity_publish(state, 1U, &companion, "companion", 0U);
    state->dmrburstL = 16;
    state->dmrburstR = 16;

    csbk_grant_or_move(opts, state, 57U, MV_LPCN2, 0, 777, 9);
    uint64_t target = 0;
    assert(state->trunk_vc_freq[0] == MV_F1);
    assert(state->dmrburstL == 16 && state->dmrburstR == 16);
    assert(recent_row_target(state, 1U, &target) && target == 202U);

    csbk_grant_or_move(opts, state, 57U, MV_LPCN2, 1, 202, 9);
    assert(state->trunk_vc_freq[0] == MV_F1);
    assert(state->dmrburstL == 16 && state->dmrburstR == 16);

    // Hold on the companion's group: its move takes the receiver.
    state->tg_hold = 202;
    csbk_grant_or_move(opts, state, 57U, MV_LPCN2, 1, 202, 9);
    assert(state->trunk_vc_freq[0] == MV_F2 && dmr_sm_get_ctx()->vc_tg == 202);
    assert(state->dmrburstR == 16 && state->dmrburstL == 9);
    state->tg_hold = 0;
}

/* A same-carrier move of the followed call to the other slot is taken in place, with its debounce. */
static void
test_move_in_place(dsd_opts* opts, dsd_state* state) {
    move_env(opts, state);
    csbk_grant_or_move(opts, state, 49U, MV_LPCN1, 0, 101, 1234);
    clock_at(0.5);
    accepted_voice(opts, state, 0);
    state->dmrburstL = 16;
    state->dmrburstR = 16;
    const unsigned int tunes = state->p25_sm_tune_count;
    csbk_grant_or_move(opts, state, 57U, MV_LPCN1, 1, 101, 1234);
    assert(state->trunk_vc_freq[0] == MV_F1 && state->p25_sm_tune_count == tunes);
    assert(dmr_sm_get_ctx()->vc_slot == 1 && dmr_sm_get_ctx()->slots[1].target == 101U);
    assert(state->dmrburstR == 16 && state->dmrburstL == 9);
}

/* A same-carrier move takes the call's activity to its new slot: the vacated slot is no phantom held call, so a
   priority candidate elsewhere still preempts. */
static void
test_move_in_place_vacates(dsd_opts* opts, dsd_state* state) {
    move_env(opts, state);
    seed_row(state, 101, "A", 10, 0);
    seed_row(state, 300, "A", 60, 1);
    csbk_grant_or_move(opts, state, 49U, MV_LPCN1, 0, 101, 1234);
    clock_at(0.5);
    accepted_voice(opts, state, 0);
    csbk_grant_or_move(opts, state, 57U, MV_LPCN1, 1, 101, 1234);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(ctx->vc_slot == 1 && ctx->slots[0].voice_active == 0 && ctx->slots[0].last_active_m <= 0.0);
    assert(ctx->slots[1].voice_active == 1);
    clock_at(1.0);
    csbk_grant_or_move(opts, state, 49U, MV_LPCN3, 0, 300, 3);
    assert(state->trunk_vc_freq[0] == MV_F3 && dmr_sm_get_ctx()->vc_tg == 300);
}

/* A move after a checked LC renamed the followed call republishes the LC's identity on the new carrier, not the
   grant's. */
static void
test_move_carries_lc_identity(dsd_opts* opts, dsd_state* state) {
    move_env(opts, state);
    csbk_grant_or_move(opts, state, 49U, MV_LPCN1, 0, 101, 1234);
    clock_at(0.5);
    accepted_voice(opts, state, 0);
    dmr_sm_note_lc_identity(opts, state, 0, DSD_CALL_KIND_GROUP_VOICE, 202U, 1234U);
    csbk_grant_or_move(opts, state, 57U, MV_LPCN2, 0, 202, 1234);
    assert(state->trunk_vc_freq[0] == MV_F2);
    assert(dmr_sm_get_ctx()->vc_tg == 202 && dmr_sm_get_ctx()->slots[0].target == 202U);
}

/* A matched move is judged with the followed call's own classification: a Hold mismatch or a media-only restriction
   never stops following it, a private call keeps moving with group tuning off, and a lockout since it started does. */
static void
test_matched_move_policy(dsd_opts* opts, dsd_state* state) {
    move_env(opts, state);
    dsd_tg_policy_entry quiet;
    assert(dsd_tg_policy_make_exact_entry(101, "A", "QUIET", DSD_TG_POLICY_SOURCE_IMPORTED, &quiet) == 0);
    quiet.record = 0U; // media only: the call is still tuned
    assert(dsd_tg_policy_upsert_exact(state, &quiet, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    csbk_grant_or_move(opts, state, 49U, MV_LPCN1, 0, 101, 1234);
    assert(state->trunk_vc_freq[0] == MV_F1);
    clock_at(0.5);
    accepted_voice(opts, state, 0);
    state->tg_hold = 999; // a Hold set since: the followed call no longer matches it
    csbk_grant_or_move(opts, state, 57U, MV_LPCN2, 0, 101, 1234);
    assert(state->trunk_vc_freq[0] == MV_F2);
    state->tg_hold = 0;

    move_env(opts, state);
    opts->trunk_tune_group_calls = 0;
    csbk_grant_or_move(opts, state, 48U, MV_LPCN1, 0, 5001, 5002);
    assert(state->trunk_vc_freq[0] == MV_F1 && dmr_sm_get_ctx()->vc_is_group == 0);
    clock_at(0.5);
    accepted_voice(opts, state, 0);
    csbk_grant_or_move(opts, state, 57U, MV_LPCN2, 0, 5001, 5002);
    assert(state->trunk_vc_freq[0] == MV_F2);

    move_env(opts, state);
    csbk_grant_or_move(opts, state, 49U, MV_LPCN1, 0, 101, 1234);
    clock_at(0.5);
    accepted_voice(opts, state, 0);
    seed_row(state, 101, "B", 0, 0);
    state->dmrburstL = 16;
    state->dmrburstR = 16;
    csbk_grant_or_move(opts, state, 57U, MV_LPCN2, 0, 101, 1234);
    assert(state->trunk_vc_freq[0] == MV_F1);
    assert(state->dmrburstL == 16 && state->dmrburstR == 16);
}

static void
test_move_admission(dsd_opts* opts, dsd_state* state) {
    dsd_decode_clock_use_test(k_t0_ns);
    test_move_keeps_follow_time(opts, state);
    test_refused_moves_commit_nothing(opts, state);
    test_move_in_place(opts, state);
    test_move_in_place_vacates(opts, state);
    test_move_carries_lc_identity(opts, state);
    test_matched_move_policy(opts, state);
    dsd_decode_clock_use_system();
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

    // Step 1: tune to initial VC via SM grant
    long f1 = 852000000;
    dmr_sm_emit_group_grant(&opts, &state, f1, /*lpcn*/ 0x0010, /*tg*/ 101, /*src*/ 1234);
    assert(opts.trunk_is_tuned == 1);
    assert(state.trunk_vc_freq[0] == f1);

    // Step 2: issue a C_MOVE of the followed call (TG 101) to a new absolute channel (eg. 853.500000 MHz). A move
    // naming another call would be an ordinary grant, which the followed call's acquisition keeps out.
    uint8_t bits[256];
    uint8_t bytes[48];
    uint16_t apcn = 0x0123;           // also learned into map
    uint16_t rx_int = 853;            // MHz
    uint16_t rx_step = 4000;          // 4000 * 125 Hz = 500 kHz
    long f2 = 853000000 + 4000 * 125; // 853.500000 MHz
    build_cmove_apcn(bits, bytes, apcn, rx_int, rx_step, /*slot*/ 0, /*target*/ 101);
    dmr_cspdu(&opts, &state, bits, bytes, /*crc ok*/ 1, /*errs*/ 0);
    assert(opts.trunk_is_tuned == 1);
    assert(state.trunk_vc_freq[0] == f2);

    // Step 3: while on CC (not on VC), a C_MOVE should NOT cause a retune
    assert(dsd_trunk_tuning_hook_return_to_cc(&opts, &state, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(opts.trunk_is_tuned == 0);
    // Build another move to 854.000000 MHz
    rx_int = 854;
    rx_step = 0;
    build_cmove_apcn(bits, bytes, apcn, rx_int, rx_step, /*slot*/ 0, /*target*/ 101);
    dmr_cspdu(&opts, &state, bits, bytes, /*crc ok*/ 1, /*errs*/ 0);
    // Expect no tune while on CC
    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_vc_freq[0] == 0);

    // Step 4: with no CC known yet, a CRC-valid CSBK received on a radio input anchors the CC at the tuned frequency
    init_env(&opts, &state);
    state.trunk_cc_freq = 0;
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 851012500U;
    build_cmove_apcn(bits, bytes, apcn, rx_int, rx_step, /*slot*/ 0, /*target*/ 101);
    dmr_cspdu(&opts, &state, bits, bytes, /*crc ok*/ 1, /*errs*/ 0);
    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_cc_freq == 851012500);

    test_move_admission(&opts, &state);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_state_ext_free_all(&state);
    printf("DMR_T3_CMOVE: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
