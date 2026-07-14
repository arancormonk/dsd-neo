// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate the P25 Phase 2 frame-level service-option encrypted mute does not
 * force trunk state-machine release before ESS has identified the ALGID.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void process_2V(dsd_opts* opts, dsd_state* state);

// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_socket_t Connect(char* hostname, int portno);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state);
static int g_return_to_cc_called = 0;

dsd_socket_t
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return DSD_INVALID_SOCKET;
}

static dsd_trunk_tune_result
return_to_cc_result(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    g_return_to_cc_called++;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc_request = return_to_cc_result;
    dsd_trunk_tuning_hooks_set(hooks);
}

void
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) {
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
setup_tuned_tdma(dsd_opts* opts, dsd_state* state, p25_sm_ctx_t** ctx) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->trunk_tune_enc_calls = 0;
    state->currentslot = 0;
    state->p25_vc_cqpsk_pref = -1;
    state->p25_vc_cqpsk_override = -1;
    state->lasttg = 1234;
    state->lasttgR = 5678;

    p25_sm_init_ctx(p25_sm_get_ctx(), opts, state);
    *ctx = p25_sm_get_ctx();
    (*ctx)->state = P25_SM_TUNED;
    (*ctx)->vc_is_tdma = 1;
    (*ctx)->vc_freq_hz = 851000000;
    (*ctx)->t_tune_m = 1.0;
    (*ctx)->t_voice_m = 1.0;
}

static int
test_pre_ess_single_slot_stays_tuned(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 0;
    state.p25_p2_audio_allowed[0] = 1;
    state.dmr_so = 0x40;
    g_return_to_cc_called = 0;

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("pre-ess single slot: no return", g_return_to_cc_called, 0);
    rc |= expect_eq("pre-ess single slot: still tuned", ctx->state == P25_SM_TUNED, 1);
    rc |= expect_eq("pre-ess single slot: pending voice stays inactive", ctx->slots[0].voice_active, 0);
    rc |= expect_eq("pre-ess single slot: gate closed", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("pre-ess single slot: pending crypto state", state.p25_crypto_state[0],
                    DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq("pre-ess single slot: deadline started", ctx->slots[0].crypto_attempt_m > 0.0, 1);
    return rc;
}

static int
test_clear_voice_to_encrypted_restarts_deadline(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_p2_audio_allowed[0] = 1;
    state.dmr_so = 0x40;
    ctx->slots[0].grant_active = 1;
    ctx->slots[0].voice_active = 1;
    ctx->slots[0].last_active_m = 2.0;
    ctx->slots[0].crypto_attempt_m = 1.0;

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("clear-to-encrypted: pending crypto state", state.p25_crypto_state[0],
                    DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq("clear-to-encrypted: gate closed", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("clear-to-encrypted: voice activity cleared", ctx->slots[0].voice_active, 0);
    rc |= expect_eq("clear-to-encrypted: stale deadline replaced", ctx->slots[0].crypto_attempt_m > 1.0, 1);

    const double started_m = ctx->slots[0].crypto_attempt_m;
    process_2V(&opts, &state);
    rc |= expect_eq("clear-to-encrypted: repeated SVC keeps deadline",
                    fabs(ctx->slots[0].crypto_attempt_m - started_m) <= 1.0e-9, 1);
    return rc;
}

static int
test_pre_ess_opposite_clear_slot_stays_tuned(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 1;
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_p2_audio_allowed[1] = 1;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.dmr_soR = 0x40;
    p25_sm_emit_active(&opts, &state, 0);
    g_return_to_cc_called = 0;

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("opposite clear slot: no return", g_return_to_cc_called, 0);
    rc |= expect_eq("opposite clear slot: still tuned", ctx->state == P25_SM_TUNED, 1);
    rc |= expect_eq("opposite clear slot: clear active", ctx->slots[0].voice_active, 1);
    rc |= expect_eq("opposite clear slot: pending ess voice inactive", ctx->slots[1].voice_active, 0);
    rc |= expect_eq("opposite clear slot: clear gate open", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("opposite clear slot: locked gate closed", state.p25_p2_audio_allowed[1], 0);
    rc |= expect_eq("opposite clear slot: pending crypto state", state.p25_crypto_state[1],
                    DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    return rc;
}

static int
test_clear_regroup_override_survives_voice_burst(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 0;
    state.dmr_so = 0x40;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_p2_audio_allowed[0] = 1;
    p25_patch_update(&state, state.lasttg, /*is_patch*/ 1, /*active*/ 1);
    p25_patch_set_kas(&state, state.lasttg, /*key*/ 0, /*alg*/ 0x84, /*ssn*/ 1);

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("clear regroup: crypto remains clear", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    rc |= expect_eq("clear regroup: audio gate remains open", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("clear regroup: voice activity emitted", ctx->slots[0].voice_active, 1);
    return rc;
}

static int
test_private_voice_ignores_regroup_clear_key_collision(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    state.currentslot = 0;
    state.gi[0] = 1;
    state.lasttg = 0x123456;
    state.dmr_so = 0x40;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_p2_audio_allowed[0] = 1;
    p25_patch_add_wgid(&state, 0x2222, 0x3456);
    p25_patch_set_kas(&state, 0x2222, /*key*/ 0, /*alg*/ 0x84, /*ssn*/ 1);

    process_2V(&opts, &state);

    int rc = 0;
    rc |= expect_eq("private patch collision: crypto pending", state.p25_crypto_state[0],
                    DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq("private patch collision: audio gate closed", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("private patch collision: voice activity suppressed", ctx->slots[0].voice_active, 0);
    return rc;
}

static int
test_encrypted_follow_tracks_activity_while_media_is_muted(void) {
    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = NULL;
    setup_tuned_tdma(&opts, &state, &ctx);
    opts.trunk_tune_enc_calls = 1;
    state.currentslot = 0;
    state.dmr_so = 0x40;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    state.p25_p2_audio_allowed[0] = 0;

    p25_sm_emit_ptt(&opts, &state, 0);

    int rc = 0;
    rc |= expect_eq("encrypted follow PTT marks activity", ctx->slots[0].voice_active, 1);
    rc |= expect_eq("encrypted follow PTT keeps media muted", state.p25_p2_audio_allowed[0], 0);

    ctx->slots[0].voice_active = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    process_2V(&opts, &state);

    rc |= expect_eq("encrypted follow voice frame marks activity", ctx->slots[0].voice_active, 1);
    rc |= expect_eq("encrypted follow voice frame keeps media muted", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("encrypted follow voice frame stays tuned", ctx->state == P25_SM_TUNED, 1);
    return rc;
}

int
main(void) {
    install_trunk_tuning_hooks();

    int rc = 0;
    rc |= test_pre_ess_single_slot_stays_tuned();
    rc |= test_clear_voice_to_encrypted_restarts_deadline();
    rc |= test_pre_ess_opposite_clear_slot_stays_tuned();
    rc |= test_clear_regroup_override_survives_voice_burst();
    rc |= test_private_voice_ignores_regroup_clear_key_collision();
    rc |= test_encrypted_follow_tracks_activity_while_media_is_muted();
    return rc;
}
