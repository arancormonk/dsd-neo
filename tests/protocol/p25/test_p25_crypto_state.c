// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Shared Phase 1/Phase 2 crypto resolution and slot-isolation regressions. */

#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_int(const char* tag, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* tag, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%016llX want 0x%016llX\n", tag, (unsigned long long)got,
                    (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
bytes_are_zero(const void* ptr, size_t size) {
    const unsigned char* bytes = (const unsigned char*)ptr;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0U) {
            return 0;
        }
    }
    return 1;
}

static void
reset_fixture(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_tune_enc_calls = 1;
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;
}

static int
test_explicit_audio_unmute_respects_lockout(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture(&opts, &state);
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_ENCRYPTED_PENDING;

    int rc = 0;
    rc |= expect_int("encrypted follow defaults to muted", p25_crypto_audio_permitted(&opts, &state, 0), 0);

    opts.unmute_encrypted_p25 = 1;
    rc |= expect_int("P25 explicit unmute permits blocked audio", p25_crypto_audio_permitted(&opts, &state, 0), 1);

    opts.unmute_encrypted_p25 = 0;
    opts.reverse_mute = 1;
    rc |= expect_int("reverse mute permits blocked audio", p25_crypto_audio_permitted(&opts, &state, 0), 1);

    opts.unmute_encrypted_p25 = 1;
    opts.trunk_tune_enc_calls = 0;
    rc |= expect_int("lockout probe overrides explicit unmute", p25_crypto_audio_permitted(&opts, &state, 0), 0);

    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    rc |= expect_int("lockout still permits classified clear audio", p25_crypto_audio_permitted(&opts, &state, 0), 1);
    return rc;
}

static int
test_phase1_resolution_does_not_override_phase2_gate(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture(&opts, &state);

    int rc = 0;
    state.p25_p1_hdu_crypto_fresh = 1;
    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE1, 0, 0x40, 0);
    rc |= expect_int("P1 begin clears HDU freshness", state.p25_p1_hdu_crypto_fresh, 0);
    state.R = 0x0102030405060708ULL;
    rc |= expect_int("P1 DES key decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x81, 0x1001, 1, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("P1 resolution preserves user unmute", opts.unmute_encrypted_p25, 0);
    rc |= expect_int("P1 decryptable audio permitted", p25_crypto_audio_permitted(&opts, &state, 0), 1);

    state.p25_p1_hdu_crypto_fresh = 1;
    p25_crypto_reset_slot(&state, 0);
    rc |= expect_int("P1 reset clears HDU freshness", state.p25_p1_hdu_crypto_fresh, 0);
    state.R = 0ULL;
    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE2, 0, 0x40, 0);
    rc |= expect_int("P2 missing DES key blocked",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, 0x1002, 2, 102),
                     DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("P1 resolution cannot permit P2 blocked audio", p25_crypto_audio_permitted(&opts, &state, 0), 0);

    reset_fixture(&opts, &state);
    opts.unmute_encrypted_p25 = 1;
    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE1, 0, 0x40, 0);
    rc |= expect_int("P1 missing key blocked",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x81, 0x1003, 3, 103),
                     DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("P1 blocked resolution preserves explicit unmute", opts.unmute_encrypted_p25, 1);
    return rc;
}

static int
test_begin_and_sticky_unknown(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture(&opts, &state);

    state.p25_p2_audio_allowed[0] = 1;
    state.p25_p2_audio_ring_count[0] = 2;
    state.p25_p2_audio_ring_count[1] = 3;
    state.fourv_counter[0] = 2;
    state.ess_b[0][0] = 1;
    state.s_l4[0][0] = 111;
    state.s_r4[0][0] = 222;

    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE2, 0, 0x40, 0);

    int rc = 0;
    rc |= expect_int("encrypted grant pending", state.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_int("encrypted grant gate", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_int("encrypted grant ring purged", state.p25_p2_audio_ring_count[0], 0);
    rc |= expect_int("encrypted grant companion ring preserved", state.p25_p2_audio_ring_count[1], 3);
    rc |= expect_int("encrypted grant int16 tail purged", state.s_l4[0][0], 0);
    rc |= expect_int("encrypted grant companion tail preserved", state.s_r4[0][0], 222);
    rc |= expect_int("encrypted grant ESS index preserved", state.fourv_counter[0], 2);
    rc |= expect_int("encrypted grant ESS bits preserved", state.ess_b[0][0], 1);

    state.p25_p2_audio_allowed[0] = 1;
    rc |= expect_int("ALGID zero stays pending",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0, 0, 0, 100),
                     DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_int("ALGID zero cannot reopen gate", state.p25_p2_audio_allowed[0], 0);

    p25_crypto_reset_slot(&state, 0);
    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE2, 0, 0x00, 0);
    rc |= expect_int("clear grant classified clear", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    rc |= expect_int("clear grant waits for media policy", state.p25_p2_audio_allowed[0], 0);

    p25_crypto_reset_slot(&state, 0);
    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE2, 0, -1, 0);
    rc |= expect_int("unknown service grant pending", state.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    p25_crypto_reset_slot(&state, 0);
    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE2, 0, 0x40, 1);
    rc |= expect_int("clear regroup override", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    return rc;
}

static int
test_phase1_clear_conflict_requires_corroboration(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture(&opts, &state);
    opts.unmute_encrypted_p25 = 1;

    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE1, 0, 0x04, 0);
    state.dmr_so = 0x04;
    state.p25_service_options_valid[0] = 1;

    int rc = 0;
    rc |= expect_int(
        "first clear-conflict tuple stays pending",
        p25_crypto_resolve(NULL, &state, DSD_P25_CRYPTO_PHASE1, 0, 0xA0, 0x0064, UINT64_C(0x0102030405060708), 3069),
        DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_int("first clear-conflict tuple arms candidate", state.p25_p1_crypto_conflict.active, 1);
    rc |= expect_int("clear-conflict candidate ALGID", state.p25_p1_crypto_conflict.algid, 0xA0);
    rc |= expect_int("clear-conflict candidate KID", state.p25_p1_crypto_conflict.keyid, 0x0064);
    rc |= expect_int("pending conflict is not confirmed", p25_crypto_metadata_is_confirmed_encrypted(&state, 0), 0);
    rc |=
        expect_int("pending conflict overrides explicit audio unmute", p25_crypto_audio_permitted(&opts, &state, 0), 0);

    rc |= expect_int("ALGID zero does not confirm conflict",
                     p25_crypto_resolve(NULL, &state, DSD_P25_CRYPTO_PHASE1, 0, 0, 0, 0, 3069),
                     DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_int("ALGID zero preserves conflict candidate", state.p25_p1_crypto_conflict.active, 1);

    rc |= expect_int(
        "different tuple replaces conflict candidate",
        p25_crypto_resolve(NULL, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x84, 0x1234, UINT64_C(0x1112131415161718), 3069),
        DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_int("replacement candidate ALGID", state.p25_p1_crypto_conflict.algid, 0x84);
    rc |= expect_int("replacement candidate KID", state.p25_p1_crypto_conflict.keyid, 0x1234);

    rc |= expect_int(
        "matching second tuple confirms encryption",
        p25_crypto_resolve(NULL, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x84, 0x1234, UINT64_C(0x2122232425262728), 3069),
        DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("confirmed tuple clears candidate", state.p25_p1_crypto_conflict.active, 0);
    rc |= expect_int("confirmed tuple is encrypted", p25_crypto_metadata_is_confirmed_encrypted(&state, 0), 1);
    rc |= expect_int("confirmed blocked tuple restores configured unmute", p25_crypto_audio_permitted(&opts, &state, 0),
                     1);

    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE1, 0, 0x04, 0);
    state.dmr_so = 0x04;
    state.p25_service_options_valid[0] = 1;
    (void)p25_crypto_resolve(NULL, &state, DSD_P25_CRYPTO_PHASE1, 0, 0xA0, 0x0064, UINT64_C(0x3132333435363738), 3069);
    rc |= expect_int(
        "clear ALGID resolves quarantined tuple",
        p25_crypto_resolve(NULL, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x80, 0, UINT64_C(0x4142434445464748), 3069),
        DSD_P25_CRYPTO_CLEAR);
    rc |= expect_int("clear ALGID clears candidate", state.p25_p1_crypto_conflict.active, 0);

    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE1, 0, 0x04, 0);
    state.dmr_so = 0x04;
    state.p25_service_options_valid[0] = 1;
    state.p25_p1_identity_pending = 1;
    rc |= expect_int(
        "identity-pending tuple remains authoritative until LCW",
        p25_crypto_resolve(NULL, &state, DSD_P25_CRYPTO_PHASE1, 0, 0xA0, 0x0064, UINT64_C(0x5152535455565758), 3069),
        DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("identity-pending tuple does not use stale clear service", state.p25_p1_crypto_conflict.active, 0);
    state.p25_p1_identity_pending = 0;
    rc |= expect_int("accepted clear LCW defers retained tuple", p25_crypto_p1_defer_clear_conflict(&state, 0x04), 1);
    rc |= expect_int("retained tuple becomes pending", state.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    p25_crypto_reset_slot(&state, 0);
    rc |= expect_int("slot reset clears conflict", state.p25_p1_crypto_conflict.active, 0);
    state.dmr_so = 0x04;
    state.p25_service_options_valid[0] = 1;
    rc |= expect_int(
        "Phase 2 bypasses Phase 1 conflict policy",
        p25_crypto_resolve(NULL, &state, DSD_P25_CRYPTO_PHASE2, 0, 0xA0, 0x0064, UINT64_C(0x6162636465666768), 3069),
        DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("Phase 2 leaves P1 candidate clear", state.p25_p1_crypto_conflict.active, 0);
    return rc;
}

static int
test_algorithm_and_manual_key_resolution(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture(&opts, &state);

    int rc = 0;
    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE2, 0, -1, 0);
    rc |=
        expect_int("definitive clear ALGID",
                   p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, 101), DSD_P25_CRYPTO_CLEAR);
    rc |= expect_int("clear classification", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    rc |= expect_int("clear preserves user mute setting", opts.unmute_encrypted_p25, 0);
    state.p25_p2_audio_allowed[0] = 1;
    rc |= expect_int("ALGID zero preserves definitive clear",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0, 0, 0, 101), DSD_P25_CRYPTO_CLEAR);
    rc |= expect_int("ALGID zero preserves clear gate", state.p25_p2_audio_allowed[0], 1);

    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE2, 0, 0x40, 0);
    rc |= expect_int("missing DES key blocked",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, 0x1001, 1, 101),
                     DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("missing key gate", state.p25_p2_audio_allowed[0], 0);

    state.R = 0x0102030405060708ULL;
    rc |= expect_int("manual DES key decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, 0x1001, 2, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    state.p25_p2_audio_allowed[0] = 1;
    rc |= expect_int("ALGID zero preserves decryptable state",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0, 0, 0, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("ALGID zero preserves decryptable gate", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_int("ALGID zero preserves decryptable key id", state.payload_keyid, 0x1001);
    state.R = 0x0000001122334455ULL;
    rc |= expect_int("manual RC4 key decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0xAA, 0x1002, 3, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("P1 manual RC4 key decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE1, 0, 0xAA, 0x1002, 3, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);

    state.aes_key_loaded[0] = 1;
    state.aes_key_segments[0] = 1U;
    rc |= expect_int("incomplete AES-128 blocked",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x89, 0x1003, 4, 101),
                     DSD_P25_CRYPTO_BLOCKED);
    state.aes_key_segments[0] = 2U;
    rc |= expect_int("manual AES-128 decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x89, 0x1003, 5, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    state.aes_key_segments[0] = 3U;
    rc |= expect_int("incomplete AES-256 blocked",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x84, 0x1004, 6, 101),
                     DSD_P25_CRYPTO_BLOCKED);
    state.aes_key_segments[0] = 4U;
    rc |= expect_int("manual AES-256 decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x84, 0x1004, 7, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("P1 manual AES-256 decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x84, 0x1004, 7, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);

    state.R = 0x0123456789ABCDEFULL;
    rc |= expect_int("P2 DES-XL unsupported",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x9F, 0x1005, 8, 101),
                     DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("P1 DES-XL decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x9F, 0x1005, 9, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("P1 decryptable preserves user mute setting", opts.unmute_encrypted_p25, 0);

    state.aes_key_loaded[0] = 1;
    state.aes_key_segments[0] = 3U;
    rc |= expect_int("P1 TDEA decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x83, 0x1006, 10, 101),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("P2 TDEA unsupported",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x83, 0x1006, 11, 101),
                     DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("unknown algorithm blocked",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x82, 0x1007, 12, 101),
                     DSD_P25_CRYPTO_BLOCKED);
    return rc;
}

static int
test_imported_key_activation_is_slot_aware(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture(&opts, &state);
    state.keyloader = 1;

    const int des_kid = 0x1234;
    state.rkey_array[des_kid] = 0x0102030405060708ULL;
    state.rkey_array_loaded[des_kid] = 1U;

    int rc = 0;
    rc |= expect_int("imported slot0 DES decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, des_kid, 0x1111, 200),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("P1 imported slot0 DES decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x81, des_kid, 0x1111, 200),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_u64("imported slot0 scalar activated", state.R, 0x0102030405060708ULL);
    rc |= expect_u64("imported slot0 does not alter slot1", state.RR, 0ULL);

    const int aes_kid = 0x2345;
    state.rkey_array[aes_kid] = 0x1111222233334444ULL;
    state.rkey_array_loaded[aes_kid] = 1U;
    state.rkey_array[aes_kid + 0x101] = 0x5555666677778888ULL;
    state.rkey_array_loaded[aes_kid + 0x101] = 1U;
    rc |= expect_int("imported slot1 AES-128 decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 1, 0x89, aes_kid, 0x2222, 201),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_u64("imported slot1 AES word 1", state.A1[1], 0x1111222233334444ULL);
    rc |= expect_u64("imported slot1 AES word 2", state.A2[1], 0x5555666677778888ULL);
    rc |= expect_int("imported slot1 AES segment count", state.aes_key_segments[1], 2);
    rc |= expect_u64("imported slot1 preserves slot0 scalar", state.R, 0x0102030405060708ULL);

    const int sparse_aes_kid = 0x4567;
    state.rkey_array[sparse_aes_kid] = 0x0101010101010101ULL;
    state.rkey_array_loaded[sparse_aes_kid] = 1U;
    state.rkey_array[sparse_aes_kid + 0x201] = 0x0303030303030303ULL;
    state.rkey_array_loaded[sparse_aes_kid + 0x201] = 1U;
    rc |= expect_int("sparse imported AES-128 blocked",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 1, 0x89, sparse_aes_kid, 0x2223, 201),
                     DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("sparse imported AES segment count", state.aes_key_segments[1], 2);

    state.currentslot = 1;
    state.payload_keyidR = aes_kid;
    state.RR = 0ULL;
    keyring_activate_slot(&opts, &state, state.currentslot);
    rc |= expect_u64("keyring activates explicit slot", state.RR, 0x1111222233334444ULL);

    rc |= expect_int("missing imported key blocked",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 1, 0x81, 0x3456, 0x3333, 201),
                     DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_u64("missing imported key clears active scalar", state.RR, 0ULL);

    state.A1[1] = 0x9999AAAABBBBCCCCULL;
    p25_crypto_reset_slot(&state, 0);
    rc |= expect_u64("release clears imported slot0 scalar", state.R, 0ULL);
    rc |= expect_u64("release preserves imported keyring material", state.rkey_array[des_kid], 0x0102030405060708ULL);
    rc |= expect_u64("release does not clear other active slot word", state.A1[1], 0x9999AAAABBBBCCCCULL);
    return rc;
}

static int
test_slot_local_transition_purge_and_mi_refresh(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture(&opts, &state);

    state.audio_out_float_buf = (float*)calloc(200U, sizeof(float));
    state.audio_out_float_bufR = (float*)calloc(200U, sizeof(float));
    state.audio_out_buf = (short*)calloc(200U, sizeof(short));
    state.audio_out_bufR = (short*)calloc(200U, sizeof(short));
    if (!state.audio_out_float_buf || !state.audio_out_float_bufR || !state.audio_out_buf || !state.audio_out_bufR) {
        free(state.audio_out_float_buf);
        free(state.audio_out_float_bufR);
        free(state.audio_out_buf);
        free(state.audio_out_bufR);
        DSD_FPRINTF(stderr, "audio buffer allocation failed\n");
        return 1;
    }

    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    state.payload_algid = 0x80;
    state.payload_algidR = 0x80;
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_p2_audio_allowed[1] = 1;
    state.p25_p2_audio_ring_count[0] = 2;
    state.p25_p2_audio_ring_count[1] = 3;
    state.audio_out_temp_buf[0] = 1.0f;
    state.audio_out_temp_bufR[0] = 2.0f;
    state.f_l4[0][0] = 3.0f;
    state.f_r4[0][0] = 4.0f;
    state.s_l4[0][0] = 5;
    state.s_r4[0][0] = 6;
    state.s_l4u[0][0] = 7;
    state.s_r4u[0][0] = 8;
    state.voice_counter[0] = 9;
    state.voice_counter[1] = 10;
    state.fourv_counter[0] = 2;
    state.ess_b[0][0] = 1;
    state.audio_out_float_buf[0] = 9.0f;
    state.audio_out_float_bufR[0] = 10.0f;
    state.audio_out_buf[0] = 11;
    state.audio_out_bufR[0] = 12;

    int rc = 0;
    rc |= expect_int("clear-to-blocked transition",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, 0x4000, 0x4444, 300),
                     DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("transition purges selected float ring", state.p25_p2_audio_ring_count[0], 0);
    rc |= expect_int("transition preserves companion float ring", state.p25_p2_audio_ring_count[1], 3);
    rc |= expect_int("transition purges selected float frame", state.f_l4[0][0] == 0.0f, 1);
    rc |= expect_int("transition preserves companion float frame", state.f_r4[0][0] == 4.0f, 1);
    rc |= expect_int("transition purges selected int16 tail", state.s_l4[0][0], 0);
    rc |= expect_int("transition preserves companion int16 tail", state.s_r4[0][0], 6);
    rc |= expect_int("transition purges selected upsample tail", state.s_l4u[0][0], 0);
    rc |= expect_int("transition preserves companion upsample tail", state.s_r4u[0][0], 8);
    rc |= expect_int("transition purges selected voice counter", state.voice_counter[0], 0);
    rc |= expect_int("transition preserves companion voice counter", state.voice_counter[1], 10);
    rc |= expect_int("transition preserves ESS index", state.fourv_counter[0], 2);
    rc |= expect_int("transition preserves ESS bits", state.ess_b[0][0], 1);
    rc |= expect_int("transition purges selected dynamic float lead",
                     bytes_are_zero(state.audio_out_float_buf, 100U * sizeof(float)), 1);
    rc |= expect_int("transition preserves companion dynamic float lead", state.audio_out_float_bufR[0] == 10.0f, 1);
    rc |= expect_int("transition purges selected dynamic int16 lead",
                     bytes_are_zero(state.audio_out_buf, 100U * sizeof(short)), 1);
    rc |= expect_int("transition preserves companion dynamic int16 lead", state.audio_out_bufR[0], 12);

    state.R = 0x0102030405060708ULL;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    state.payload_algid = 0x81;
    state.payload_keyid = 0x4000;
    state.payload_miP = 0x5000ULL;
    state.p25_p2_audio_ring_count[0] = 2;
    state.voice_counter[0] = 7;
    state.s_l4[0][0] = 13;
    state.DMRvcL = 14;
    state.bit_counterL = 15;
    state.dropL = 333;
    state.ks_octetL[0] = 0xA5;
    rc |= expect_int("MI refresh remains decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, 0x4000, 0x5001, 300),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("MI refresh preserves queued ring", state.p25_p2_audio_ring_count[0], 2);
    rc |= expect_int("MI refresh preserves voice cadence", state.voice_counter[0], 7);
    rc |= expect_int("MI refresh preserves int16 audio", state.s_l4[0][0], 13);
    rc |= expect_int("P2 MI refresh preserves crypto voice counter", state.DMRvcL, 14);
    rc |= expect_int("P2 MI refresh preserves crypto bit counter", state.bit_counterL, 15);
    rc |= expect_int("P2 MI refresh preserves RC4 position", state.dropL, 333);
    rc |= expect_int("P2 MI refresh preserves keystream", state.ks_octetL[0], 0xA5);

    state.DMRvcL = 16;
    state.bit_counterL = 17;
    state.dropL = 444;
    state.ks_octetL[0] = 0x5A;
    rc |= expect_int("P1 MI refresh remains decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE1, 0, 0x81, 0x4000, 0x5002, 300),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("P1 MI refresh resets crypto voice counter", state.DMRvcL, 0);
    rc |= expect_int("P1 MI refresh resets crypto bit counter", state.bit_counterL, 0);
    rc |= expect_int("P1 MI refresh resets RC4 position", state.dropL, 267);
    rc |= expect_int("P1 MI refresh clears keystream", state.ks_octetL[0], 0);

    state.p25_p2_audio_ring_count[0] = 1;
    state.s_l4[0][0] = 14;
    rc |= expect_int("key identity change remains decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, 0x4001, 0x5003, 300),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("key identity change purges ring", state.p25_p2_audio_ring_count[0], 0);
    rc |= expect_int("key identity change purges int16 tail", state.s_l4[0][0], 0);

    free(state.audio_out_float_buf);
    free(state.audio_out_float_bufR);
    free(state.audio_out_buf);
    free(state.audio_out_bufR);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_explicit_audio_unmute_respects_lockout();
    rc |= test_phase1_resolution_does_not_override_phase2_gate();
    rc |= test_begin_and_sticky_unknown();
    rc |= test_phase1_clear_conflict_requires_corroboration();
    rc |= test_algorithm_and_manual_key_resolution();
    rc |= test_imported_key_activation_is_slot_aware();
    rc |= test_slot_local_transition_purge_and_mi_refresh();
    return rc;
}
