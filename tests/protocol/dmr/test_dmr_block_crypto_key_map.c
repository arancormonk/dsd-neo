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

static void
seed_keyring_and_map(dsd_state* state) {
    state->keyloader = 1;
    state->payload_algid = 0x21; /* RC4 */
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
    state.payload_algidR = 0x21;
    state.payload_keyidR = 0x05; /* nothing imported for 0x05 */
    state.R = 0x1111ULL;
    state.RR = 0x2222ULL;

    dmr_block_crypto_load_ctx(&state, 1U, 1, 12, &ctx);
    rc |= expect_eq("slot2-fallback-rr", (long long)ctx.rkey, 0x2222LL);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_group_data_target_uses_the_map();
    rc |= test_individual_data_target_ignores_the_map();
    rc |= test_slot2_fallback_uses_rr();
    return rc;
}
