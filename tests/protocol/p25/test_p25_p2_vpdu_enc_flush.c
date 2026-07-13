// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Validate P25p2 VPDU SVC encrypted gating starts a silent classification,
 * flushes only the encrypted slot, and preserves the clear slot.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdbool.h>
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

struct RtlSdrContext;

// Stubs to satisfy external references

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

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

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct RtlSdrContext* g_rtl_ctx = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
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
    install_trunk_tuning_hooks();
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_audio});
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    // Trunking + ENC lockout enabled and tuned to VC
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
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
    opts.p25_is_tuned = 1;
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
    opts.p25_is_tuned = 1;
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
    // is still authoritative, before the slot is gated and reset.
    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0x31;
    opts.p25_is_tuned = 1;
    opts.trunk_is_tuned = 1;
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    st.currentslot = 0;
    st.lasttg = 0x2222;
    st.lasttgR = 0;
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

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&st);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
