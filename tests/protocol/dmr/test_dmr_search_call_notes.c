// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR call notes for a signal search and the crypto provenance they carry: a checked LC and a checked PI header each
 * note the slot's call, and only a PI header's ALG ID and key id are received -- never a forced or inferred one -- held
 * across a lost voice LC header for the call the next checked LC opens, kept across a reacquired fade and never
 * inherited by a later call.
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

/* Call notes, per slot, and the record each one found. */
static int notes[2];
static dsd_call_snapshot noted[2];
static int noted_valid[2];

static void
capture_call(const dsd_state* state, uint8_t slot) {
    assert(slot < 2U);
    ++notes[slot];
    noted_valid[slot] = dsd_call_state_get(state, slot, &noted[slot]) > 0;
}

static void
reset_notes(void) {
    DSD_MEMSET(notes, 0, sizeof(notes));
    DSD_MEMSET(noted, 0, sizeof(noted));
    DSD_MEMSET(noted_valid, 0, sizeof(noted_valid));
}

static void
write_bits(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

/* A group voice LC (FLCO 0), standard FID, the given service options. */
static void
run_lc(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t type, uint8_t so, uint32_t target, uint32_t source) {
    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof(bits));
    write_bits(bits, 16U, so, 8U);
    write_bits(bits, 24U, target, 24U);
    write_bits(bits, 48U, source, 24U);
    uint32_t irr = 0;
    state->currentslot = slot;
    dmr_flco(opts, state, bits, 1U, &irr, type);
}

/* A DMRA PI header (MFID 0x10) carrying @p alg and @p kid, its CRC verified. */
static void
run_pi(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t alg, uint8_t kid) {
    uint8_t pi[10] = {alg, 0x10, kid, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00};
    state->currentslot = slot;
    dmr_pi(opts, state, pi, 1U, 0U);
}

static dsd_call_snapshot
call_on(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, slot, &call) > 0);
    return call;
}

static int
received_is(const dsd_call_snapshot* call, uint8_t bits, uint8_t algid, uint16_t kid) {
    return call->crypto_received == bits && ((bits & DSD_CALL_CRYPTO_RX_ALG) == 0U || call->received_algid == algid)
           && ((bits & DSD_CALL_CRYPTO_RX_KID) == 0U || call->received_kid == kid);
}

static dsd_state*
fresh_state(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    reset_notes();
    return state;
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

enum { RX_BOTH = DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID };

/* Identity, then a checked PI with no later LC: the PI's crypto-only update notes the call, received. */
static void
test_identity_then_pi(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
    assert(notes[0] == 1 && noted_valid[0] && noted[0].ota_target_id == 1001U && noted[0].crypto_received == 0U);
    assert(noted[0].crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    assert(notes[0] == 2 && received_is(&noted[0], RX_BOTH, 0x21U, 0x05U));
    assert(noted[0].ota_target_id == 1001U && notes[1] == 0);
    // The call's own embedded LCs add nothing and take nothing away.
    run_lc(opts, state, 0U, 3U, 0x40U, 1001U, 2002U);
    assert(notes[0] == 3 && received_is(&noted[0], RX_BOTH, 0x21U, 0x05U));
    free_state(state);
}

/* A PI after a lost voice LC header, on both slots at once: each slot's first checked LC opens a call that received
   that slot's PI, and nothing else. */
static void
test_precall_pi_on_both_slots(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    run_pi(opts, state, 1U, 0x21U, 0x09U);
    assert(state->dmr_pi_precall_rx[0] == RX_BOTH && state->dmr_pi_precall_rx[1] == RX_BOTH);
    assert(notes[0] == 0 && notes[1] == 0); // no call to name yet
    run_lc(opts, state, 1U, 3U, 0x40U, 3003U, 4004U);
    dsd_call_snapshot second = call_on(state, 1U);
    assert(second.ota_target_id == 3003U && received_is(&second, RX_BOTH, 0x21U, 0x09U));
    assert(notes[1] == 1 && received_is(&noted[1], RX_BOTH, 0x21U, 0x09U));
    run_lc(opts, state, 0U, 3U, 0x40U, 1001U, 2002U);
    dsd_call_snapshot first = call_on(state, 0U);
    assert(first.ota_target_id == 1001U && received_is(&first, RX_BOTH, 0x21U, 0x05U));
    assert(state->dmr_pi_precall_rx[0] == 0U && state->dmr_pi_precall_rx[1] == 0U);
    // Further LCs keep them, with no later PI.
    run_lc(opts, state, 0U, 3U, 0x40U, 1001U, 2002U);
    run_lc(opts, state, 1U, 3U, 0x40U, 3003U, 4004U);
    first = call_on(state, 0U);
    second = call_on(state, 1U);
    assert(received_is(&first, RX_BOTH, 0x21U, 0x05U) && received_is(&second, RX_BOTH, 0x21U, 0x09U));
    free_state(state);
}

/* A forced or inferred ALG ID is published as the call's ALG ID, as before, but never as received. */
static void
test_forced_and_inferred_algid_not_received(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
    state->M = 0x24; // --dmr-force-algid
    state->currentslot = 0;
    assert(dsd_dmr_apply_forced_algid(state) == 1);
    run_lc(opts, state, 0U, 3U, 0x40U, 1001U, 2002U);
    dsd_call_snapshot call = call_on(state, 0U);
    assert(call.algid == 0x24U && call.kid == 0xFFU && call.crypto_received == 0U);
    assert(noted_valid[0] && noted[0].crypto_received == 0U);
    state->M = 0;
    // The late-entry inference from a loaded key writes the live ALG ID and the 0xFF sentinel the same way.
    run_lc(opts, state, 1U, 1U, 0x40U, 3003U, 4004U);
    state->payload_algidR = 0x21;
    state->payload_keyidR = 0xFF;
    run_lc(opts, state, 1U, 3U, 0x40U, 3003U, 4004U);
    call = call_on(state, 1U);
    assert(call.algid == 0x21U && call.crypto_received == 0U);
    free_state(state);
}

/* What the PI carried is received only from a burst whose own check verified and that names an algorithm. */
static void
test_unverified_pi_not_received(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
    state->event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED; // a RAS-accepted burst under -F
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    state->event_crc_invalid[0] = 0U;
    dsd_call_snapshot call = call_on(state, 0U);
    assert(call.algid == 0x21U && call.crypto_received == 0U);
    // Held for no later call either.
    run_pi(opts, state, 1U, 0x21U, 0x05U);
    assert(state->dmr_pi_precall_rx[1] == RX_BOTH);
    state->event_crc_invalid[1] = DSD_CRC_FAILED;
    run_pi(opts, state, 1U, 0x21U, 0x05U);
    state->event_crc_invalid[1] = 0U;
    assert(state->dmr_pi_precall_rx[1] == 0U);
    // A DMRA PI naming an algorithm it does not define publishes ALG 0: nothing received.
    run_pi(opts, state, 0U, 0x30U, 0x05U);
    call = call_on(state, 0U);
    assert(call.algid == 0U && call.crypto_received == 0U);
    // A Kirisun key id is a hash of the ALG ID and the target, not something the air carried.
    uint8_t kirisun[10] = {0x36, 0x0A, 0x40, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x01};
    state->currentslot = 0;
    dmr_pi(opts, state, kirisun, 1U, 0U);
    call = call_on(state, 0U);
    assert(received_is(&call, DSD_CALL_CRYPTO_RX_ALG, 0x36U, 0U));
    free_state(state);
}

/* A checked PI, a sync loss (the no-carrier reset clears the live crypto), and the same call's LC reacquiring it: the
   reacquired call keeps what was received, though the live values republished with it are gone. */
static void
test_reacquired_call_keeps_received(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    dsd_decode_clock_use_test(5000000000ULL);
    run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    const dsd_call_snapshot before = call_on(state, 0U);
    dsd_decode_clock_test_set_ns(5200000000ULL);
    assert(dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) == 1);
    state->payload_algid = 0;
    state->payload_keyid = 0;
    state->payload_mi = 0;
    dmr_pi_precall_reset(state, 0U);
    dsd_decode_clock_test_set_ns(5400000000ULL);
    run_lc(opts, state, 0U, 3U, 0x40U, 1001U, 2002U);
    const dsd_call_snapshot after = call_on(state, 0U);
    assert(after.epoch != before.epoch && after.phase == DSD_CALL_PHASE_ACTIVE && after.algid == 0U);
    assert(received_is(&after, RX_BOTH, 0x21U, 0x05U) && received_is(&noted[0], RX_BOTH, 0x21U, 0x05U));
    dsd_decode_clock_use_system();
    free_state(state);
}

/* A separate later call does not inherit: not after the terminator, not when the live crypto outlived a missed one,
   and not from a PI held for a call whose transmission ended before its LC. */
static void
test_later_call_does_not_inherit(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    run_lc(opts, state, 0U, 2U, 0x40U, 1001U, 2002U); // its terminator
    run_lc(opts, state, 0U, 1U, 0x40U, 1111U, 2222U);
    dsd_call_snapshot call = call_on(state, 0U);
    assert(call.ota_target_id == 1111U && call.crypto_received == 0U);
    // The terminator was missed: the next call's LC finds the live ALG ID and key id still in place.
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    call = call_on(state, 0U);
    assert(received_is(&call, RX_BOTH, 0x21U, 0x05U));
    run_lc(opts, state, 0U, 1U, 0x40U, 3333U, 4444U);
    call = call_on(state, 0U);
    assert(call.ota_target_id == 3333U && call.algid == 0x21U && call.crypto_received == 0U);
    // A PI held across a lost header is forgotten at its transmission's terminator.
    run_lc(opts, state, 1U, 2U, 0x40U, 5555U, 6666U);
    run_pi(opts, state, 1U, 0x21U, 0x09U);
    assert(state->dmr_pi_precall_rx[1] == RX_BOTH);
    run_lc(opts, state, 1U, 2U, 0x40U, 5555U, 6666U);
    assert(state->dmr_pi_precall_rx[1] == 0U);
    run_lc(opts, state, 1U, 1U, 0x40U, 7777U, 8888U);
    call = call_on(state, 1U);
    assert(call.ota_target_id == 7777U && call.crypto_received == 0U);
    free_state(state);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    dsd_signal_search_call_set(capture_call);
    test_identity_then_pi(opts);
    test_precall_pi_on_both_slots(opts);
    test_forced_and_inferred_algid_not_received(opts);
    test_unverified_pi_not_received(opts);
    test_reacquired_call_keeps_received(opts);
    test_later_call_does_not_inherit(opts);
    dsd_signal_search_call_set(NULL);
    free(opts);
    return 0;
}
