// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Validate P25p2 VPDU SVC encrypted gating starts a silent classification,
 * flushes only the encrypted slot, and preserves the clear slot.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Stubs to satisfy external references

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

static int g_return_to_cc_called = 0;
static int g_audio_capture_calls = 0;
static short g_first_audio_block[320];

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    g_return_to_cc_called++;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc_request = return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
capture_audio(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    (void)state;
    g_audio_capture_calls++;
    if (g_audio_capture_calls == 1 && data && bytes >= sizeof(g_first_audio_block)) {
        DSD_MEMCPY(g_first_audio_block, data, sizeof(g_first_audio_block));
    }
}

static void
reset_audio_capture(void) {
    g_audio_capture_calls = 0;
    DSD_MEMSET(g_first_audio_block, 0, sizeof(g_first_audio_block));
}

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
seed_policy_group(dsd_state* st, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    return dsd_tg_policy_append_exact(st, &row);
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    static Event_History_I event_history[2];
    install_trunk_tuning_hooks();
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_audio});
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    st.event_history_s = event_history;
    init_event_history(&event_history[0], 0U, 255U);
    init_event_history(&event_history[1], 0U, 255U);

    // Trunking + ENC lockout enabled and tuned to VC
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;

    // Scenario 1: other slot active. ENC should gate only current slot and not release.
    st.currentslot = 0;             // so VPDU slot=0 for FACCH
    st.p25_p2_audio_allowed[0] = 1; // will be gated
    st.p25_p2_audio_allowed[1] = 1; // other slot active
    st.p25_p2_audio_ring_count[0] = 2;
    st.p25_p2_audio_ring_count[1] = 1;
    g_return_to_cc_called = 0;

    // Pre-mark TG as already DE to skip event emission branches in VPDU
    rc |= expect_eq("seed DE row", seed_policy_group(&st, 0x1234u, "DE", "ENC LO"), 0);

    unsigned long long MAC[24] = {0};
    // Group Voice Channel Message (opcode 0x01)
    MAC[1] = 0x01;
    MAC[2] = 0x40; // SVC with ENC bit set
    MAC[3] = 0x12; // TG high
    MAC[4] = 0x34; // TG low
    MAC[5] = 0x00; // SRC high
    MAC[6] = 0x00; // SRC mid
    MAC[7] = 0x01; // SRC low
    process_MAC_VPDU(&opts, &st, /*type FACCH*/ 0, MAC);

    rc |= expect_eq("slot0 muted", st.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("slot0 ring flushed", st.p25_p2_audio_ring_count[0], 0);
    rc |= expect_eq("slot1 ring kept", st.p25_p2_audio_ring_count[1], 1);
    rc |= expect_eq("no release", g_return_to_cc_called, 0);

    // Scenario 2: a repeated indication cannot reopen the pending slot. Release
    // is deferred until definitive crypto resolution or classification timeout.
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 0; // other idle
    st.p25_p2_audio_ring_count[0] = 0;
    st.p25_p2_audio_ring_count[1] = 0;
    g_return_to_cc_called = 0;

    process_MAC_VPDU(&opts, &st, 0, MAC);

    rc |= expect_eq("slot0 muted again", st.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("slot0 ring remains empty", st.p25_p2_audio_ring_count[0], 0);
    rc |= expect_eq("classification does not release early", g_return_to_cc_called, 0);
    rc |= expect_eq("slot0 remains pending", st.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    // Scenario 3: unit-to-unit encrypted fallback should honor recent opposite-slot MAC activity,
    // matching the group-call fallback and avoiding a premature CC return while the other slot is active.
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0x02; // Unit-to-unit voice channel message
    MAC[2] = 0x40; // SVC with ENC bit set
    MAC[3] = 0x00; // TGT high
    MAC[4] = 0x12; // TGT mid
    MAC[5] = 0x34; // TGT low
    MAC[6] = 0x00; // SRC high
    MAC[7] = 0x00; // SRC mid
    MAC[8] = 0x02; // SRC low
    opts.trunk_is_tuned = 1;
    st.currentslot = 0;
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_UNKNOWN;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 0;
    st.p25_p2_audio_ring_count[0] = 1;
    st.p25_p2_audio_ring_count[1] = 0;
    st.p25_p2_last_mac_active[1] = time(NULL);
    st.p25_p2_last_mac_active_m[1] = dsd_time_now_monotonic_s();
    g_return_to_cc_called = 0;

    process_MAC_VPDU(&opts, &st, 0, MAC);

    rc |= expect_eq("unit slot0 muted", st.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("unit slot0 ring flushed", st.p25_p2_audio_ring_count[0], 0);
    rc |= expect_eq("unit recent other slot avoids release", g_return_to_cc_called, 0);

    // Scenario 4: an explicit clear KAS key on an active regroup overrides the
    // encrypted service bit for the member talkgroup.
    p25_patch_update(&st, 0x3456, /*is_patch*/ 1, /*active*/ 1);
    p25_patch_add_wgid(&st, 0x3456, 0x1234);
    p25_patch_set_kas(&st, 0x3456, /*key*/ 0, /*alg*/ 0x84, /*ssn*/ 1);
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0x01;
    MAC[2] = 0x40;
    MAC[3] = 0x12;
    MAC[4] = 0x34;
    MAC[7] = 0x03;
    opts.trunk_is_tuned = 1;
    st.currentslot = 0;
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_UNKNOWN;
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_ring_count[0] = 0;

    process_MAC_VPDU(&opts, &st, 0, MAC);

    rc |= expect_eq("late clear regroup member classified", st.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);

    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_ring_count[0] = 2;
    process_MAC_VPDU(&opts, &st, 0, MAC);

    rc |= expect_eq("clear regroup member remains clear", st.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    rc |= expect_eq("clear regroup member gate remains open", st.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("clear regroup member ring preserved", st.p25_p2_audio_ring_count[0], 2);

    // Scenario 5: MAC Release drains a short int16 tail while crypto readiness
    // is still authoritative, invalidates the released slot's PTT marker, and
    // permits an identical follow-up PTT while the companion retains the carrier.
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0x31;
    opts.trunk_is_tuned = 1;
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    st.currentslot = 0;
    p25_crypto_reset_slot(&st, 0);
    p25_crypto_reset_slot(&st, 1);
    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    p25_sm_init_ctx(sm, &opts, &st);
    sm->state = P25_SM_TUNED;
    sm->vc_is_tdma = 1;
    sm->vc_freq_hz = 851500000;
    const double first_ptt_m = dsd_time_now_monotonic_s();
    for (int slot = 0; slot < 2; slot++) {
        sm->slots[slot].grant_active = 1;
        sm->slots[slot].freq_hz = sm->vc_freq_hz;
        sm->slots[slot].channel = 0x1234 | slot;
        sm->slots[slot].target_id = slot == 0 ? 0x2222 : 0x3333;
        sm->slots[slot].ota_tg = sm->slots[slot].target_id;
        sm->slots[slot].src = slot + 1;
        sm->slots[slot].is_group = 1;
        sm->slots[slot].svc_bits = 0;
        sm->slots[slot].last_grant_m = first_ptt_m;
    }
    const uint8_t release_ptt_signature[P25_SM_PTT_SIGNATURE_BYTES] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x99, 0x80, 0x12, 0x34, 0x00, 0x00, 0x01, 0x22, 0x22,
    };
    p25_sm_event_t release_ptt = p25_sm_ev_ptt_call(0, 0x2222, 0, 1, 1, 0);
    DSD_MEMCPY(release_ptt.ptt_signature, release_ptt_signature, sizeof(release_ptt.ptt_signature));
    release_ptt.ptt_signature_valid = 1;
    release_ptt.observed_m = first_ptt_m;
    p25_sm_event(sm, &opts, &st, &release_ptt);
    p25_sm_event_t companion_active = p25_sm_ev_active_call(1, 0x3333, 0, 2, 1, 0);
    companion_active.observed_m = first_ptt_m;
    p25_sm_event(sm, &opts, &st, &companion_active);
    dsd_call_snapshot released_call = {0};
    rc |= expect_eq("MAC Release seed active call",
                    dsd_call_state_get(&st, 0U, &released_call) > 0 && released_call.phase == DSD_CALL_PHASE_ACTIVE, 1);
    const uint64_t released_epoch = released_call.epoch;
    st.dmrburstL = 21;
    st.dmrburstR = 21;
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    st.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 1;
    st.p25_p2_audio_ring_count[0] = 0;
    st.p25_p2_audio_ring_count[1] = 1;
    st.voice_counter[0] = 1;
    st.s_l4[0][0] = 321;
    g_return_to_cc_called = 0;
    reset_audio_capture();

    process_MAC_VPDU(&opts, &st, 0, MAC);

    rc |= expect_eq("MAC Release tail emitted", g_audio_capture_calls > 0, 1);
    rc |= expect_eq("MAC Release tail left sample", g_first_audio_block[0], 321);
    rc |= expect_eq("MAC Release tail right sample muted", g_first_audio_block[1], 0);
    rc |= expect_eq("MAC Release tail drained", st.s_l4[0][0], 0);
    rc |= expect_eq("MAC Release crypto reset after flush", st.p25_crypto_state[0], DSD_P25_CRYPTO_UNKNOWN);
    rc |= expect_eq("MAC Release retains active companion", g_return_to_cc_called, 0);
    rc |= expect_eq("MAC Release invalidates released PTT marker", sm->slots[0].ptt_signature_valid, 0);
    rc |= expect_eq("MAC Release clears released slot activity", sm->slots[0].voice_active, 0);
    rc |= expect_eq("MAC Release preserves released slot assignment", sm->slots[0].grant_active, 1);
    dsd_call_context_snapshot call_context;
    rc |= expect_eq("MAC Release copies call context", dsd_call_context_copy_snapshot(&st, &call_context), 1);
    rc |= expect_eq("MAC Release ends released slot", call_context.calls.slots[0].phase, DSD_CALL_PHASE_ENDED);
    rc |= expect_eq("MAC Release finalizes released event", call_context.events[0].ended_committed, 1);
    rc |= expect_eq("MAC Release keeps companion active", call_context.calls.slots[1].phase, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_eq("MAC Release leaves companion event open", call_context.events[1].ended_committed, 0);

    release_ptt.observed_m = first_ptt_m + 0.5;
    p25_sm_event(sm, &opts, &st, &release_ptt);
    rc |= expect_eq("MAC Release follow-up copies call", dsd_call_state_get(&st, 0U, &released_call) > 0, 1);
    rc |= expect_eq("MAC Release follow-up reopens call", released_call.phase, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_eq("MAC Release follow-up starts new epoch", released_call.epoch == released_epoch + 1U, 1);

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&st);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
