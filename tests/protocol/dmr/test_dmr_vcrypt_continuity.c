// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR voice crypto continuity (src/protocol/dmr/dmr_vcrypt.c, issue #729): which superframe a slot's IV belongs to,
 * catching the MI up over superframes the voice readers did not see, and knowing when the chain is lost. The expected
 * MI/IV sequences come from LFSRs written out here, not from the decoder's helpers.
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmr_vcrypt.h"
#include "dsd-neo/core/state_fwd.h"

/* x^32 + x^22 + x^2 + x + 1, one bit at a time: the AES IV is the MI and 96 more bits; the next MI is IV bytes 4-7. */
static uint32_t
expect_aes_iv(uint32_t mi, uint8_t iv[16]) {
    uint64_t lfsr = mi;
    for (int i = 0; i < 4; i++) {
        iv[i] = (uint8_t)(mi >> (24 - 8 * i));
    }
    for (int i = 4; i < 16; i++) {
        uint8_t octet = 0;
        for (int j = 0; j < 8; j++) {
            const uint64_t bit = ((lfsr >> 31) ^ (lfsr >> 21) ^ (lfsr >> 1) ^ lfsr) & 1U;
            lfsr = (lfsr << 1) | bit;
            octet = (uint8_t)((octet << 1) | (uint8_t)bit);
        }
        iv[i] = octet;
    }
    return ((uint32_t)iv[4] << 24) | ((uint32_t)iv[5] << 16) | ((uint32_t)iv[6] << 8) | iv[7];
}

static uint64_t
expect_des_iv(uint32_t mi) {
    uint64_t lfsr = mi;
    for (int i = 0; i < 32; i++) {
        const uint64_t bit = ((lfsr >> 31) ^ (lfsr >> 21) ^ (lfsr >> 1) ^ lfsr) & 1U;
        lfsr = (lfsr << 1) | bit;
    }
    return lfsr;
}

static uint32_t
expect_kirisun_next(uint32_t lfsr) {
    uint32_t next = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t out = 0;
        for (int j = 0; j < 8; j++) {
            const uint32_t msb = (lfsr >> 31) & 1U;
            lfsr <<= 1;
            if (msb) {
                out |= (uint8_t)(1U << j);
                lfsr ^= 0xD459C4F1U;
            }
        }
        next = (next << 8) | out;
    }
    return next;
}

static dsd_state*
new_state(int algid, uint8_t slot, unsigned long long mi) {
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    if (slot == 0U) {
        state->payload_algid = algid;
        state->payload_mi = mi;
    } else {
        state->payload_algidR = algid;
        state->payload_miR = mi;
    }
    state->symbol_clock = 100000U;
    state->symbol_clock_epoch = 7U;
    state->carrier_seq = 3U;
    return state;
}

/* PI prepared superframe 0 (as dmr_pi() does: IV from the PI's MI, payload_mi staged as the next MI), then its voice
   sync. Returns the superframe-0 MI's successor the caller should see staged. */
static void
start_after_pi(dsd_state* state, uint8_t slot, uint32_t pi_mi) {
    uint8_t* iv = slot == 0U ? state->aes_iv : state->aes_ivR;
    unsigned long long* mi = slot == 0U ? &state->payload_mi : &state->payload_miR;
    *mi = expect_aes_iv(pi_mi, iv);
    dmr_vcrypt_note_first(state, slot);
    state->symbol_clock += (uint64_t)2U * DMR_VC_BURST_PAIR_SYMBOLS; /* a header repeat or two before burst A */
    dmr_vcrypt_superframe_start(state, slot);
    assert(state->dmr_vc_ref_state[slot] == DMR_VC_REF_VALID);
    assert(state->dmr_vc_untrusted[slot] == 0U);
}

/* The IV the decoder should hold n superframes after the PI's. */
static void
expected_iv_after(uint32_t pi_mi, int n, uint8_t iv[16], uint32_t* staged) {
    uint32_t mi = pi_mi;
    for (int k = 0; k <= n; k++) {
        mi = expect_aes_iv(mi, iv);
    }
    *staged = mi;
}

static void
assert_aes_at(const dsd_state* state, uint8_t slot, uint32_t pi_mi, int n) {
    uint8_t iv[16];
    uint32_t staged = 0;
    expected_iv_after(pi_mi, n, iv, &staged);
    assert(memcmp(slot == 0U ? state->aes_iv : state->aes_ivR, iv, 16) == 0);
    assert((uint32_t)(slot == 0U ? state->payload_mi : state->payload_miR) == staged);
}

/* A processed VC6: the late-entry step advances the IV, then the superframe ends; the next voice sync is a burst pair
   later. */
static void
run_vc6_then_next_vc1(dsd_state* state, uint8_t slot) {
    uint8_t* iv = slot == 0U ? state->aes_iv : state->aes_ivR;
    unsigned long long* mi = slot == 0U ? &state->payload_mi : &state->payload_miR;
    *mi = expect_aes_iv((uint32_t)*mi, iv);
    state->symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS - DMR_VC_BURST_PAIR_SYMBOLS;
    dmr_vcrypt_superframe_end(state, slot);
    state->symbol_clock += DMR_VC_BURST_PAIR_SYMBOLS;
    dmr_vcrypt_superframe_start(state, slot);
}

static void
test_continuous_superframes_take_no_catch_up(void) {
    dsd_state* state = new_state(0x25, 0U, 0U);
    start_after_pi(state, 0U, 0x12345678U);
    assert_aes_at(state, 0U, 0x12345678U, 0);
    for (int n = 1; n <= 5; n++) {
        run_vc6_then_next_vc1(state, 0U);
        assert_aes_at(state, 0U, 0x12345678U, n);
    }
    assert(state->dmr_vc_untrusted[0] == 0U);
    free(state);
}

static void
test_lost_superframes_are_caught_up(void) {
    const int losses[] = {1, 2, 30};
    for (size_t c = 0; c < sizeof(losses) / sizeof(losses[0]); c++) {
        for (uint8_t slot = 0; slot < 2; slot++) {
            dsd_state* state = new_state(0x25, slot, 0U);
            start_after_pi(state, slot, 0x0BADCAFEU);
            // superframe 0's VC6 and the next losses[c]-1 superframes go unseen: the next voice sync is losses[c]
            // superframes after superframe 0's.
            state->symbol_clock += (uint64_t)losses[c] * DMR_VC_SUPERFRAME_SYMBOLS;
            dmr_vcrypt_superframe_start(state, slot);
            assert_aes_at(state, slot, 0x0BADCAFEU, losses[c]);
            assert(state->dmr_vc_untrusted[slot] == 0U);
            assert((slot == 0U ? state->DMRvcL : state->DMRvcR) == 0);
            free(state);
        }
    }
}

static void
test_voice_sync_slips_round_to_the_nearest_superframe(void) {
    // The reference after a VC6 lies ahead of the next voice sync's nominal clock; the grid slips a voice sync by a
    // few symbols either way. -1, 0 and +1 are the same superframe; +863 is too, +865 the next one.
    const int64_t offsets[] = {-1, 0, 1, (DMR_VC_SUPERFRAME_SYMBOLS / 2) - 1, (DMR_VC_SUPERFRAME_SYMBOLS / 2) + 1};
    const int expect_n[] = {1, 1, 1, 1, 2};
    for (size_t c = 0; c < sizeof(offsets) / sizeof(offsets[0]); c++) {
        dsd_state* state = new_state(0x25, 0U, 0U);
        start_after_pi(state, 0U, 0x55AA55AAU);
        uint8_t* iv = state->aes_iv;
        state->payload_mi = expect_aes_iv((uint32_t)state->payload_mi, iv); // VC6's step
        state->symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS - DMR_VC_BURST_PAIR_SYMBOLS;
        dmr_vcrypt_superframe_end(state, 0U);
        state->symbol_clock = (uint64_t)((int64_t)state->symbol_clock + DMR_VC_BURST_PAIR_SYMBOLS + offsets[c]);
        dmr_vcrypt_superframe_start(state, 0U);
        assert_aes_at(state, 0U, 0x55AA55AAU, expect_n[c]);
        assert(state->dmr_vc_ref_state[0] == DMR_VC_REF_VALID);
        free(state);
    }
}

static void
test_repeated_reentry_without_vc6_is_idempotent(void) {
    dsd_state* state = new_state(0x25, 1U, 0U);
    start_after_pi(state, 1U, 0x13572468U);
    state->symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS;
    dmr_vcrypt_superframe_start(state, 1U); // caught up over 1
    state->symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS;
    dmr_vcrypt_superframe_start(state, 1U); // one more, not two
    assert_aes_at(state, 1U, 0x13572468U, 2);
    // and the same voice sync seen again (a bootstrap after a loop that read it) takes nothing
    dmr_vcrypt_superframe_start(state, 1U);
    assert_aes_at(state, 1U, 0x13572468U, 2);
    free(state);
}

static void
test_other_slot_is_untouched(void) {
    dsd_state* state = new_state(0x25, 0U, 0U);
    state->payload_algidR = 0x25;
    state->payload_miR = 0xFEEDF00DULL;
    start_after_pi(state, 0U, 0x01020304U);
    state->symbol_clock += (uint64_t)3U * DMR_VC_SUPERFRAME_SYMBOLS;
    dmr_vcrypt_superframe_start(state, 0U);
    assert(state->payload_miR == 0xFEEDF00DULL);
    assert(state->dmr_vc_ref_state[1] == DMR_VC_REF_NONE);
    free(state);
}

static void
test_broken_timeline_loses_the_chain(void) {
    for (int kind = 0; kind < 3; kind++) {
        dsd_state* state = new_state(0x25, 0U, 0U);
        start_after_pi(state, 0U, 0x89ABCDEFU);
        state->dmr_crypto_pi_verified[0] = 1U;
        const unsigned long long staged = state->payload_mi;
        state->symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS;
        if (kind == 0) {
            state->symbol_clock_epoch++; // a stream flush or input switch
        } else if (kind == 1) {
            state->carrier_seq++; // a carrier boundary
        } else {
            state->symbol_clock += (uint64_t)(DMR_VC_CATCHUP_MAX + 1) * DMR_VC_SUPERFRAME_SYMBOLS;
        }
        dmr_vcrypt_superframe_start(state, 0U);
        assert(state->payload_mi == staged); // no guess
        assert(state->dmr_vc_ref_state[0] == DMR_VC_REF_NONE);
        assert(state->dmr_vc_untrusted[0] == 1U);
        // nor are the PI's identifiers known to be the call's: an SB identifier may name them again
        assert(state->dmr_crypto_pi_verified[0] == 0U);
        // a late entry that names the next superframe's MI re-keys it from the superframe after: this one's frames
        // still to decode stay muted, the next voice sync's are trusted
        dmr_vcrypt_note_next_superframe_mi(state, 0U);
        assert(state->dmr_vc_ref_state[0] == DMR_VC_REF_VALID);
        assert(state->dmr_vc_untrusted[0] == 1U);
        dmr_vcrypt_superframe_end(state, 0U);
        state->symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS;
        const unsigned long long rekeyed = state->payload_mi;
        dmr_vcrypt_superframe_start(state, 0U);
        assert(state->payload_mi == rekeyed); // the installed MI is this superframe's: no step
        assert(state->dmr_vc_untrusted[0] == 0U);
        free(state);
    }
}

static void
test_pi_far_before_first_voice_sync_is_not_superframe_0(void) {
    // The first superframe(s) after the PI were lost: the voice sync that arrives cannot be placed against the PI.
    dsd_state* state = new_state(0x25, 0U, 0U);
    state->payload_mi = expect_aes_iv(0x24681357U, state->aes_iv);
    dmr_vcrypt_note_first(state, 0U);
    state->symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS + ((uint64_t)4U * DMR_VC_BURST_PAIR_SYMBOLS);
    state->dmr_crypto_pi_verified[0] = 1U;
    dmr_vcrypt_superframe_start(state, 0U);
    assert(state->dmr_vc_untrusted[0] == 1U);
    assert(state->dmr_vc_ref_state[0] == DMR_VC_REF_NONE);
    assert(state->dmr_crypto_pi_verified[0] == 0U); /* the PI led to no voice of its own */
    free(state);
}

/* A slot with no chain is untrusted whatever its ALG ID reads at the voice sync: --dmr-force-algid installs one only
   when a frame decodes, after the sync was read with none, and those frames stay muted until the slot is re-keyed. The
   vocoder ignores the flag for a clear call (no MI-keyed cipher). */
static void
test_unknown_chain_is_untrusted_whatever_the_algid(void) {
    dsd_state* state = new_state(0, 0U, 0U);
    dmr_vcrypt_superframe_start(state, 0U);
    assert(state->dmr_vc_untrusted[0] == 1U);
    state->symbol_clock_epoch++;
    dmr_vcrypt_superframe_start(state, 0U);
    assert(state->dmr_vc_untrusted[0] == 1U);
    free(state);
}

static void
test_des_and_kirisun_steps(void) {
    dsd_state* state = new_state(0x22, 0U, 0U);
    state->payload_miP = expect_des_iv(0x11223344U);
    state->payload_mi = state->payload_miP & 0xFFFFFFFFULL;
    dmr_vcrypt_note_first(state, 0U);
    dmr_vcrypt_superframe_start(state, 0U);
    state->symbol_clock += (uint64_t)2U * DMR_VC_SUPERFRAME_SYMBOLS;
    dmr_vcrypt_superframe_start(state, 0U);
    uint64_t iv = expect_des_iv(0x11223344U);
    iv = expect_des_iv((uint32_t)iv);
    iv = expect_des_iv((uint32_t)iv);
    assert(state->payload_miP == iv);
    assert(state->payload_mi == (iv & 0xFFFFFFFFULL));
    free(state);

    dsd_state* kirisun = new_state(0x36, 1U, 0xC0FFEE11ULL);
    dmr_vcrypt_note_first(kirisun, 1U);
    dmr_vcrypt_superframe_start(kirisun, 1U);
    kirisun->symbol_clock += DMR_VC_SUPERFRAME_SYMBOLS;
    dmr_vcrypt_superframe_start(kirisun, 1U);
    assert((uint32_t)kirisun->payload_miR == expect_kirisun_next(0xC0FFEE11U));
    free(kirisun);
}

static void
test_forget_and_frame_positions(void) {
    dsd_state* state = new_state(0x25, 0U, 0U);
    start_after_pi(state, 0U, 0x0F0F0F0FU);
    state->dmr_crypto_pi_verified[0] = 1U;
    state->dmr_le_candidate_valid[0] = 1U;
    dmr_vcrypt_forget(state, 0U);
    assert(state->dmr_vc_ref_state[0] == DMR_VC_REF_NONE);
    assert(state->dmr_crypto_pi_verified[0] == 0U && state->dmr_le_candidate_valid[0] == 0U);

    assert(state->dmr_voice_ks_pos[1] == 0); // a zeroed state is unpositioned
    dmr_vcrypt_set_frame(state, 1U, 6U, 2U);
    assert(state->dmr_voice_ks_pos[1] == 18); // position 17, stored one up
    dmr_vcrypt_set_frame(state, 1U, 1U, 0U);
    assert(state->dmr_voice_ks_pos[1] == 1);
    dmr_vcrypt_set_frame(state, 1U, 7U, 0U);
    assert(state->dmr_voice_ks_pos[1] == 0);
    dmr_vcrypt_clear_frame(state, 1U);
    assert(state->dmr_voice_ks_pos[1] == 0);
    free(state);
}

int
main(void) {
    test_continuous_superframes_take_no_catch_up();
    test_lost_superframes_are_caught_up();
    test_voice_sync_slips_round_to_the_nearest_superframe();
    test_repeated_reentry_without_vc6_is_idempotent();
    test_other_slot_is_untouched();
    test_broken_timeline_loses_the_chain();
    test_pi_far_before_first_voice_sync_is_not_superframe_0();
    test_unknown_chain_is_untrusted_whatever_the_algid();
    test_des_and_kirisun_steps();
    test_forget_and_frame_positions();
    printf("DMR voice crypto continuity: OK\n");
    return 0;
}
