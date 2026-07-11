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
    rc |= expect_int("encrypted grant marker", state.p25_p2_enc_lockout_muted[0], 1);
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
test_algorithm_and_manual_key_resolution(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture(&opts, &state);

    int rc = 0;
    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE2, 0, -1, 0);
    rc |=
        expect_int("definitive clear ALGID",
                   p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x80, 0, 0, 101), DSD_P25_CRYPTO_CLEAR);
    rc |= expect_int("clear marker removed", state.p25_p2_enc_lockout_muted[0], 0);
    rc |= expect_int("clear resets P1 compatibility unmute", opts.unmute_encrypted_p25, 0);
    state.p25_p2_audio_allowed[0] = 1;
    rc |= expect_int("ALGID zero preserves definitive clear",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0, 0, 0, 101), DSD_P25_CRYPTO_CLEAR);
    rc |= expect_int("ALGID zero preserves clear gate", state.p25_p2_audio_allowed[0], 1);

    p25_crypto_begin_voice_call(&state, DSD_P25_CRYPTO_PHASE2, 0, 0x40, 0);
    rc |= expect_int("missing DES key blocked",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, 0x1001, 1, 101),
                     DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("missing key marker", state.p25_p2_enc_lockout_muted[0], 1);

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
    rc |= expect_int("P1 decryptable compatibility unmute", opts.unmute_encrypted_p25, 1);

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
    keyring(&opts, &state);
    rc |= expect_u64("compatibility keyring uses current slot", state.RR, 0x1111222233334444ULL);

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
    rc |= expect_int("MI refresh remains decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, 0x4000, 0x5001, 300),
                     DSD_P25_CRYPTO_DECRYPTABLE);
    rc |= expect_int("MI refresh preserves queued ring", state.p25_p2_audio_ring_count[0], 2);
    rc |= expect_int("MI refresh preserves voice cadence", state.voice_counter[0], 7);
    rc |= expect_int("MI refresh preserves int16 audio", state.s_l4[0][0], 13);
    rc |= expect_int("MI refresh resets crypto voice counter", state.DMRvcL, 0);
    rc |= expect_int("MI refresh resets crypto bit counter", state.bit_counterL, 0);

    state.p25_p2_audio_ring_count[0] = 1;
    state.s_l4[0][0] = 14;
    rc |= expect_int("key identity change remains decryptable",
                     p25_crypto_resolve(&opts, &state, DSD_P25_CRYPTO_PHASE2, 0, 0x81, 0x4001, 0x5002, 300),
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
    rc |= test_begin_and_sticky_unknown();
    rc |= test_algorithm_and_manual_key_resolution();
    rc |= test_imported_key_activation_is_slot_aware();
    rc |= test_slot_local_transition_purge_and_mi_refresh();
    return rc;
}
