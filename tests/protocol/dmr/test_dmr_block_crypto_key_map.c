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

// Precedence: an explicit --dmr-tg-key-csv row beats the global manual AES key (state->K1..K4,
// set by -2/-H and the keys menu). Safe now in a way it was not before the alg-need gate: a row
// can only apply for data alg 4 when cells kid+0x000 and kid+0x101 are non-zero, and those are
// exactly the cells dmr_block_load_aes_key() reads -- so its "all four zero, substitute K1..K4"
// fallback is unreachable whenever ctx->mapped is set, rather than being skipped because a lone
// RC4 scalar happened to sit in parts[0].
static int
test_mapped_aes_row_beats_manual_key(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.keyloader = 1;
    state.payload_algid = 4; /* data AES-128 */
    state.payload_keyid = 0x03;
    state.K1 = 0xEEEEEEEEEEEEEEEEULL;
    state.K2 = 0xDDDDDDDDDDDDDDDDULL;
    state.K3 = 0xCCCCCCCCCCCCCCCCULL;
    state.K4 = 0xBBBBBBBBBBBBBBBBULL;
    // Key id 0x40: a real two-segment AES-128 import.
    state.rkey_array[0x40] = 0x1111111111111111ULL;
    state.rkey_array_loaded[0x40] = 1U;
    state.rkey_array[0x141] = 0x2222222222222222ULL;
    state.rkey_array_loaded[0x141] = 1U;
    state.dmr_tg_key_map_tg[0] = 123U;
    state.dmr_tg_key_map_kid[0] = 0x40;
    state.dmr_tg_key_map_count = 1;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 1U;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("manual-key-row-mapped", ctx.mapped, 1);
    rc |= expect_eq("manual-key-row-kid", ctx.kid, 0x40);
    // First byte of A1 comes from the row, not from K1.
    rc |= expect_eq("manual-key-row-wins", ctx.aes_key[0], 0x11);
    return rc;
}

// And with no row applying, the manual key fallback still works: the key id has nothing imported,
// so all four parts are zero and K1..K4 substitute exactly as before.
static int
test_manual_key_fallback_survives_without_a_row(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.keyloader = 1;
    state.payload_algid = 4;
    state.payload_keyid = 0x05; /* nothing imported at 0x05 */
    state.K1 = 0xEEEEEEEEEEEEEEEEULL;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 999ULL; /* unmapped */
    state.dmr_data_target_is_group[0] = 1U;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("no-row-unmapped", ctx.mapped, 0);
    rc |= expect_eq("no-row-manual-key-used", ctx.aes_key[0], 0xEE);
    return rc;
}

// The scalar fallback is slot-correct: R keys slot 1, RR keys slot 2. Reading R for both decrypted
// slot 2 with slot 1's key whenever the resolved id had nothing imported. Both directions are
// pinned here because only the slot-2 case had a test.
static int
test_slot1_fallback_uses_r(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.keyloader = 1;
    state.payload_algid = 1;    /* data RC4 */
    state.payload_keyid = 0x05; /* nothing imported */
    state.R = 0x1234ULL;
    state.RR = 0x5678ULL;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 999ULL;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("slot1-fallback-uses-r", (long long)ctx.rkey, 0x1234LL);
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
    rc |= test_mapped_aes_row_beats_manual_key();
    rc |= test_manual_key_fallback_survives_without_a_row();
    rc |= test_slot1_fallback_uses_r();
    if (rc == 0) {
        DSD_FPRINTF(stdout, "DMR_BLOCK_CRYPTO_KEY_MAP: OK\n");
    }
    return rc;
}
