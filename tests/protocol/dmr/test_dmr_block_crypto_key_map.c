// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <stdio.h>

#include "../../../src/protocol/dmr/dmr_block_crypto.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_eq(const char* tag, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", tag, got, want);
        return 1;
    }
    return 0;
}

// payload_algid is read here in the DATA path's ALG numbering, not the voice one: this file drives
// dmr_block_crypto_load_ctx(), and 0x21 -- voice RC4 -- is not a data ALG at all. Alg 1 is data
// RC4, which is what consumes the scalar seeded below.
static void
seed_keyring_and_map(dsd_state* state) {
    state->keyloader = 1;
    state->payload_algid = 1; /* data RC4 */
    state->payload_keyid = 0x03;
    state->rkey_array[0x03] = 0xAAAAAULL;
    state->rkey_array_loaded[0x03] = 1U;
    state->rkey_array[0x7B] = 0xBBBBBULL;
    state->rkey_array_loaded[0x7B] = 1U;
    state->dmr_tg_key_map_tg[0] = 123U;
    state->dmr_tg_key_map_kid[0] = 0x7B;
    state->dmr_tg_key_map_count = 1;
}

// A mapped group target keys the data burst the same way it keys the call's voice. Before this,
// voice decrypted via the map while the same call's PDUs decoded to garbage.
static int
test_group_data_target_uses_the_map(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    seed_keyring_and_map(&state);
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 1U;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("data-mapped-kid", ctx.kid, 0x7B);
    rc |= expect_eq("data-mapped-rkey", (long long)ctx.rkey, 0xBBBBBLL);
    rc |= expect_eq("data-mapped-flag", ctx.mapped, 1);
    // The OTA id stays the truth for the printed header.
    rc |= expect_eq("data-signaled-kid", ctx.signaled_kid, 0x03);
    return rc;
}

// An individual data target is a radio id, which shares the talkgroup's 24-bit space.
static int
test_individual_data_target_ignores_the_map(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    seed_keyring_and_map(&state);
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 0U;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("data-individual-kid", ctx.kid, 0x03);
    rc |= expect_eq("data-individual-rkey", (long long)ctx.rkey, 0xAAAAALL);
    rc |= expect_eq("data-individual-flag", ctx.mapped, 0);
    return rc;
}

// Slot 2 must fall back to RR, not slot 1's R.
static int
test_slot2_fallback_uses_rr(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    state.keyloader = 1;
    state.currentslot = 1;
    state.payload_algidR = 1;    /* data RC4 */
    state.payload_keyidR = 0x05; /* nothing imported for 0x05 */
    state.R = 0x1111ULL;
    state.RR = 0x2222ULL;

    dmr_block_crypto_load_ctx(&state, 1U, 1, 12, &ctx);
    rc |= expect_eq("slot2-fallback-rr", (long long)ctx.rkey, 0x2222LL);
    return rc;
}

// payload_keyid is shared across protocols and P25 writes a full 16-bit KID into it, so a
// signaled id that cannot round-trip through the resolver's uint8_t must keep its full width.
// Narrowing it made the burst decrypt with rkey_array[id & 0xFF] -- an unrelated key.
static int
test_wide_signaled_kid_is_not_narrowed(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    seed_keyring_and_map(&state);
    state.currentslot = 0;
    state.payload_keyid = 0x103; /* low byte 0x03 has material; 0x103 does not */
    state.rkey_array[0x103] = 0xCCCCCULL;
    state.rkey_array_loaded[0x103] = 1U;
    state.dmr_lrrp_target[0] = 999ULL; /* unmapped target */
    state.dmr_data_target_is_group[0] = 1U;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("wide-kid-kept", ctx.kid, 0x103);
    rc |= expect_eq("wide-kid-rkey", (long long)ctx.rkey, 0xCCCCCLL);
    rc |= expect_eq("wide-kid-unmapped", ctx.mapped, 0);
    return rc;
}

// The map must not steer a wide signaled id either: the resolver cannot represent it, so the
// burst keeps the OTA id rather than resolving a truncated one.
static int
test_wide_signaled_kid_bypasses_the_map(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    seed_keyring_and_map(&state);
    state.currentslot = 0;
    state.payload_keyid = 0x103;
    state.rkey_array[0x103] = 0xCCCCCULL;
    state.rkey_array_loaded[0x103] = 1U;
    state.dmr_lrrp_target[0] = 123ULL; /* a mapped group target */
    state.dmr_data_target_is_group[0] = 1U;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("wide-kid-map-bypassed", ctx.kid, 0x103);
    rc |= expect_eq("wide-kid-map-rkey", (long long)ctx.rkey, 0xCCCCCLL);
    rc |= expect_eq("wide-kid-map-flag", ctx.mapped, 0);
    return rc;
}

// The data path's ALG numbering is its own (0 BP, 1 RC4, 2 DES, 4 AES-128, 5 AES-256, 7 VTX), so
// it translates locally rather than normalizing into the voice space -- where data-2 (DES) and
// voice-0x02 (Hytera Enhanced) already collide. A row may only apply when the mapped key id holds
// what THAT alg consumes.
static int
test_data_alg_need_gates_the_map(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    // AES-128 burst, row pointing at a scalar-only key id: the row must not apply.
    DSD_MEMSET(&state, 0, sizeof(state));
    seed_keyring_and_map(&state);
    state.payload_algid = 4; /* data AES-128 */
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 1U;
    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("aes-row-scalar-only-unmapped", ctx.mapped, 0);
    rc |= expect_eq("aes-row-scalar-only-kid", ctx.kid, 0x03);

    // RC4 burst, row pointing at an AES-only key id: the row must not apply either.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.keyloader = 1;
    state.payload_algid = 1; /* data RC4 */
    state.payload_keyid = 0x03;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    state.rkey_array[0x1CC] = 0x2222ULL; /* segment 1 of key id 0xCB */
    state.rkey_array_loaded[0x1CC] = 1U;
    state.dmr_tg_key_map_tg[0] = 123U;
    state.dmr_tg_key_map_kid[0] = 0xCB;
    state.dmr_tg_key_map_count = 1;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 1U;
    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("rc4-row-aes-only-unmapped", ctx.mapped, 0);
    rc |= expect_eq("rc4-row-aes-only-kid", ctx.kid, 0x03);
    rc |= expect_eq("rc4-row-aes-only-rkey", (long long)ctx.rkey, 0xAAAAALL);

    // Moto BP (alg 0) reads state->K and the BPK table, never the keyring: no row applies.
    DSD_MEMSET(&state, 0, sizeof(state));
    seed_keyring_and_map(&state);
    state.payload_algid = 0;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 1U;
    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("bp-never-mapped", ctx.mapped, 0);

    // VTX STD (alg 7) has no decrypt branch at all.
    state.payload_algid = 7;
    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("vtx-never-mapped", ctx.mapped, 0);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_group_data_target_uses_the_map();
    rc |= test_individual_data_target_ignores_the_map();
    rc |= test_slot2_fallback_uses_rr();
    rc |= test_wide_signaled_kid_is_not_narrowed();
    rc |= test_wide_signaled_kid_bypasses_the_map();
    rc |= test_data_alg_need_gates_the_map();
    return rc;
}
