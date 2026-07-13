// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate P25 Phase 2 early ENC lockout behavior: when one slot is
 * encrypted and ENC lockout is enabled, mute that slot only and remain
 * on the voice channel if the opposite slot is active with clear audio.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

// Test helper from shim
int p25_test_p2_early_enc_handle(dsd_opts* opts, dsd_state* state, int slot);

// Stubs/overrides to avoid IO deps
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

// Stub alias helpers referenced by VPDU path (linked from lib)
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

// Capture return_to_cc calls invoked by p25_sm_release path
static int g_return_to_cc_called = 0;
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tune_result return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id);

dsd_trunk_tune_result
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

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    install_trunk_tuning_hooks();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    // Trunking enabled/tuned to VC, ENC lockout enabled
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;

    // Case 1: other slot active (clear call). ENC on slot 1 should mute slot 1 only, no release.
    st.p25_p2_audio_allowed[0] = 1; // clear/allowed on slot 0
    st.p25_p2_audio_allowed[1] = 1; // prime as active; helper will gate selected slot
    g_return_to_cc_called = 0;
    int rel = p25_test_p2_early_enc_handle(&opts, &st, /*slot*/ 1);
    rc |= expect_eq("no release when other active", rel, 0);
    rc |= expect_eq("return_to_cc not called", g_return_to_cc_called, 0);
    rc |= expect_eq("slot0 remains allowed", st.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("slot1 muted", st.p25_p2_audio_allowed[1], 0);

    // Case 2: both slots idle; ENC on slot 0 should trigger release.
    st.p25_p2_audio_allowed[0] = 1; // active and will be gated
    st.p25_p2_audio_allowed[1] = 0; // other idle
    g_return_to_cc_called = 0;
    rel = p25_test_p2_early_enc_handle(&opts, &st, /*slot*/ 0);
    rc |= expect_eq("release when other idle", rel, 1);
    rc |= expect_eq("return_to_cc called", g_return_to_cc_called, 1);
    rc |= expect_eq("slot0 muted", st.p25_p2_audio_allowed[0], 0);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
