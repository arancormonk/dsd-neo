// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The live decryption inputs -- the per-slot ALG ID, key id and MI the voice path keys with, and the P25 crypto
 * classification -- across the call forks a missed terminator causes. What a call record publishes about them is a
 * separate matter (DMR_SEARCH_CALL_NOTES, P25_CRYPTO_STATE); these inputs must stay exactly what the decoder has always
 * left there. The expected values below were recorded by running this same file, unchanged, against the parent
 * release's decoder: it uses only interfaces that predate the Signal search call details.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
write_bits(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

static void
dmr_lc_checked(dsd_opts* opts, dsd_state* state, uint8_t type, uint8_t so, uint32_t target, uint32_t source,
               uint32_t crc_correct) {
    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof(bits));
    write_bits(bits, 16U, so, 8U);
    write_bits(bits, 24U, target, 24U);
    write_bits(bits, 48U, source, 24U);
    uint32_t irr = 0;
    state->currentslot = 0;
    dmr_flco(opts, state, bits, crc_correct, &irr, type);
}

static void
dmr_lc(dsd_opts* opts, dsd_state* state, uint8_t type, uint8_t so, uint32_t target, uint32_t source) {
    dmr_lc_checked(opts, state, type, so, target, source, 1U);
}

static void
dmr_pi_header(dsd_opts* opts, dsd_state* state, uint8_t alg, uint8_t kid, uint8_t mi_low) {
    uint8_t pi[10] = {alg, 0x10, kid, 0x11, 0x22, 0x33, mi_low, 0x00, 0x00, 0x00};
    state->currentslot = 0;
    dmr_pi(opts, state, pi, 1U, 0U);
}

static int failures;

static void
expect_dmr(const char* tag, const dsd_state* state, int algid, int keyid, unsigned long long mi, unsigned int so) {
    printf("%s: algid=%02X keyid=%02X mi=%llX so=%02X\n", tag, state->payload_algid, state->payload_keyid,
           state->payload_mi, state->dmr_so);
    if (state->payload_algid != algid || state->payload_keyid != keyid || state->payload_mi != mi
        || state->dmr_so != so) {
        printf("  MISMATCH: want algid=%02X keyid=%02X mi=%llX so=%02X\n", algid, keyid, mi, so);
        failures++;
    }
}

static void
expect_p25(const char* tag, const dsd_state* state, int algid, int keyid, unsigned long long mi, int crypto_state) {
    printf("%s: algid=%02X keyid=%04X mi=%llX state=%d\n", tag, state->payload_algid, state->payload_keyid,
           state->payload_miP, (int)state->p25_crypto_state[0]);
    if (state->payload_algid != algid || state->payload_keyid != keyid || state->payload_miP != mi
        || (int)state->p25_crypto_state[0] != crypto_state) {
        printf("  MISMATCH: want algid=%02X keyid=%04X mi=%llX state=%d\n", algid, keyid, mi, crypto_state);
        failures++;
    }
}

static dsd_state*
dmr_call_with_pi(dsd_opts* opts) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    state->synctype = state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dsd_decode_clock_test_set_ns(1000000000ULL);
    dmr_lc(opts, state, 1U, 0x40U, 1001U, 2002U);
    dmr_pi_header(opts, state, 0x21U, 0x05U, 0x44U);
    (void)dsd_call_state_update_media(state, 0U, 1, 0.0);
    dsd_decode_clock_test_set_ns(1300000000ULL);
    return state;
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

static void
dmr_scenarios(dsd_opts* opts) {
    // Missed terminator, then the next call's voice LC header, without and then with its own PI.
    dsd_state* state = dmr_call_with_pi(opts);
    expect_dmr("dmr call A with PI", state, 0x21, 0x05, 0x11223344ULL, 0x40U);
    dmr_lc(opts, state, 1U, 0x40U, 3333U, 4444U);
    expect_dmr("dmr fork to B, no PI of its own", state, 0x21, 0x05, 0x11223344ULL, 0x40U);
    dmr_pi_header(opts, state, 0x21U, 0x09U, 0x55U);
    expect_dmr("dmr B's own PI", state, 0x21, 0x09, 0x11223355ULL, 0x40U);
    dmr_lc(opts, state, 3U, 0x40U, 3333U, 4444U);
    expect_dmr("dmr B's embedded LC", state, 0x21, 0x09, 0x11223355ULL, 0x40U);
    free_state(state);

    // A clear call after a missed terminator.
    state = dmr_call_with_pi(opts);
    dmr_lc(opts, state, 1U, 0x00U, 5555U, 6666U);
    expect_dmr("dmr fork to a clear call", state, 0x21, 0x05, 0x11223344ULL, 0x00U);
    free_state(state);

    // The next transmission's PI before its lost header, then its embedded LC.
    state = dmr_call_with_pi(opts);
    dmr_pi_header(opts, state, 0x21U, 0x07U, 0x66U);
    expect_dmr("dmr next PI while A is active", state, 0x21, 0x07, 0x11223366ULL, 0x40U);
    dmr_lc(opts, state, 3U, 0x40U, 3333U, 4444U);
    expect_dmr("dmr fork by B's embedded LC", state, 0x21, 0x07, 0x11223366ULL, 0x40U);
    free_state(state);

    // After the fork, the new call's terminator decodes unverified (it may be a mis-typed voice burst), and the call's
    // next voice frame heals it: the heal puts back the crypto the slot carried.
    state = dmr_call_with_pi(opts);
    dmr_lc(opts, state, 1U, 0x40U, 3333U, 4444U);
    (void)dsd_call_state_update_media(state, 0U, 1, 0.0);
    dsd_decode_clock_test_set_ns(1400000000ULL);
    dmr_lc_checked(opts, state, 2U, 0x40U, 3333U, 4444U, 0U);
    expect_dmr("dmr unverified terminator resets", state, 0, 0, 0ULL, 0x00U);
    dsd_decode_clock_test_set_ns(1450000000ULL);
    dsd_call_observation media = {0};
    media.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    media.slot = 0U;
    media.kind = DSD_CALL_KIND_VOICE;
    (void)dsd_call_state_observe(state, &media, DSD_CALL_BOUNDARY_BEGIN);
    expect_dmr("dmr heal restores", state, 0x21, 0x05, 0x11223344ULL, 0x40U);
    free_state(state);

    // Two unverified terminators after the fork, each healed by the next voice frame, with no LC or PI in between:
    // every heal puts back the crypto the slot carried, the second as well as the first.
    state = dmr_call_with_pi(opts);
    dmr_lc(opts, state, 1U, 0x40U, 3333U, 4444U);
    (void)dsd_call_state_update_media(state, 0U, 1, 0.0);
    dsd_decode_clock_test_set_ns(1400000000ULL);
    dmr_lc_checked(opts, state, 2U, 0x40U, 3333U, 4444U, 0U);
    dsd_decode_clock_test_set_ns(1450000000ULL);
    (void)dsd_call_state_observe(state, &media, DSD_CALL_BOUNDARY_BEGIN);
    expect_dmr("dmr first heal restores", state, 0x21, 0x05, 0x11223344ULL, 0x40U);
    (void)dsd_call_state_update_media(state, 0U, 1, 0.0);
    dsd_decode_clock_test_set_ns(1550000000ULL);
    dmr_lc_checked(opts, state, 2U, 0x40U, 3333U, 4444U, 0U);
    expect_dmr("dmr second unverified terminator resets", state, 0, 0, 0ULL, 0x00U);
    dsd_decode_clock_test_set_ns(1600000000ULL);
    (void)dsd_call_state_observe(state, &media, DSD_CALL_BOUNDARY_BEGIN);
    expect_dmr("dmr second heal restores", state, 0x21, 0x05, 0x11223344ULL, 0x40U);
    free_state(state);

    // An encrypted call's terminator and the next call's header both missed: the next call's first embedded LC says
    // clear. The established classification holds the encryption, as it always has.
    state = dmr_call_with_pi(opts);
    dmr_lc(opts, state, 3U, 0x00U, 7777U, 8888U);
    expect_dmr("dmr clear embedded LC of the next call", state, 0x21, 0x05, 0x11223344ULL, 0x40U);
    free_state(state);

    // A PI with no call active, then the call's header.
    state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    state->synctype = state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dmr_pi_header(opts, state, 0x21U, 0x05U, 0x44U);
    dmr_lc(opts, state, 1U, 0x40U, 1001U, 2002U);
    expect_dmr("dmr PI then header", state, 0x21, 0x05, 0x11223344ULL, 0x40U);
    free_state(state);
}

static dsd_state*
p25_conventional_state(dsd_opts* opts) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->trunk_tune_enc_calls = 1;
    state->synctype = state->lastsynctype = DSD_SYNC_P25P1_POS;
    p25_sm_init_ctx(p25_sm_get_ctx(), opts, state);
    return state;
}

static void
p25_scenarios(dsd_opts* opts) {
    // Missed TDU, then the next call's LDU1, and its own LDU2 after it.
    dsd_state* state = p25_conventional_state(opts);
    dsd_decode_clock_test_set_ns(2000000000ULL);
    (void)p25_sm_emit_active_call(opts, state, 0, 1001, 0, 101, 1, 0x40);
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE1, 0, 0x84, 0x1234, 0x0102030405060708ULL, 1001);
    (void)dsd_call_state_update_media(state, 0U, 1, 0.0);
    expect_p25("p25 call A", state, 0x84, 0x1234, 0x0102030405060708ULL, DSD_P25_CRYPTO_BLOCKED);
    dsd_decode_clock_test_set_ns(2300000000ULL);
    (void)p25_sm_emit_active_call(opts, state, 0, 2002, 0, 202, 1, 0x00);
    expect_p25("p25 fork to B (clear service)", state, 0x84, 0x1234, 0x0102030405060708ULL, DSD_P25_CRYPTO_BLOCKED);
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE1, 0, 0x80, 0x0000, 0ULL, 2002);
    expect_p25("p25 B's own clear LDU2", state, 0x80, 0x0000, 0ULL, DSD_P25_CRYPTO_CLEAR);
    free_state(state);

    // Late entry: the next transmission's LDU2 decodes before its LDU1.
    state = p25_conventional_state(opts);
    dsd_decode_clock_test_set_ns(3000000000ULL);
    (void)p25_sm_emit_active_call(opts, state, 0, 1001, 0, 101, 1, 0x40);
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE1, 0, 0x84, 0x1234, 0x0102030405060708ULL, 1001);
    (void)dsd_call_state_update_media(state, 0U, 1, 0.0);
    dsd_decode_clock_test_set_ns(3200000000ULL);
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE1, 0, 0xAA, 0x0042, 0x1111111111111111ULL, 1001);
    expect_p25("p25 B's LDU2 while A is active", state, 0xAA, 0x0042, 0x1111111111111111ULL, DSD_P25_CRYPTO_BLOCKED);
    dsd_decode_clock_test_set_ns(3300000000ULL);
    (void)p25_sm_emit_active_call(opts, state, 0, 2002, 0, 202, 1, 0x40);
    expect_p25("p25 fork by B's LDU1", state, 0xAA, 0x0042, 0x1111111111111111ULL, DSD_P25_CRYPTO_BLOCKED);
    free_state(state);

    // One call: HDU, LDU1, LDU2.
    state = p25_conventional_state(opts);
    dsd_decode_clock_test_set_ns(4000000000ULL);
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE1, 0, 0xAA, 0x0042, 0x1111111111111111ULL, 0);
    (void)p25_sm_emit_active_call(opts, state, 0, 3003, 0, 303, 1, 0x40);
    expect_p25("p25 HDU then LDU1", state, 0xAA, 0x0042, 0x1111111111111111ULL, DSD_P25_CRYPTO_BLOCKED);
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE1, 0, 0xAA, 0x0042, 0x2222222222222222ULL, 3003);
    expect_p25("p25 LDU2", state, 0xAA, 0x0042, 0x2222222222222222ULL, DSD_P25_CRYPTO_BLOCKED);
    free_state(state);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    dsd_decode_clock_use_test(1000000000ULL);
    dmr_scenarios(opts);
    p25_scenarios(opts);
    dsd_decode_clock_use_system();
    free(opts);
    return failures == 0 ? 0 : 1;
}
