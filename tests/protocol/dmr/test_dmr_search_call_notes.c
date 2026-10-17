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
#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdint.h>
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

/* A terminator LC whose CRC did not verify: it may be a mis-typed voice burst, so the call ends recoverably. */
static void
run_unverified_terminator(dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t target, uint32_t source) {
    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof(bits));
    write_bits(bits, 16U, 0x40U, 8U);
    write_bits(bits, 24U, target, 24U);
    write_bits(bits, 48U, source, 24U);
    uint32_t irr = 0;
    state->currentslot = slot;
    dmr_flco(opts, state, bits, 0U, &irr, 2U);
}

/* The identity-less voice media mark that heals a recoverably ended call. */
static void
run_media_mark(dsd_state* state, uint8_t slot) {
    dsd_call_observation media = {0};
    media.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    media.slot = slot;
    media.kind = DSD_CALL_KIND_VOICE;
    (void)dsd_call_state_observe(state, &media, DSD_CALL_BOUNDARY_BEGIN);
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

/* The single-burst codeword dmr_sbrc() reads as DMRA ALG 5 (0x25), KEY 0x44. */
static void
load_single_burst(dsd_state* state, uint8_t slot) {
    static const uint8_t k_225[16] = {0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1};
    uint8_t data_matrix[32];
    for (int i = 0; i < 16; i++) {
        data_matrix[i] = k_225[i];
        data_matrix[16 + i] = k_225[i];
    }
    for (int i = 0; i < 32; i++) {
        state->dmr_embedded_signalling[slot][5][i + 8] =
            data_matrix[DeInterleaveReverseChannelBptcPlacement[DeInterleaveReverseChannelBptc[i]]];
    }
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
   reacquired call keeps the crypto it was restored with, received bits included, though the live values (what the
   voice path decrypts with, as before) are gone until the transmission's next PI. */
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
    assert(after.epoch != before.epoch && after.phase == DSD_CALL_PHASE_ACTIVE);
    assert(after.algid == 0x21U && after.kid == 0x05U && after.crypto_restored != 0U);
    assert(state->payload_algid == 0 && state->payload_keyid == 0 && state->payload_mi == 0ULL);
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
    // The call's voice, then, as on the air, the next transmission's voice marking media on the still-active call
    // before its LC forks it.
    assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
    assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
    run_lc(opts, state, 0U, 1U, 0x40U, 3333U, 4444U);
    call = call_on(state, 0U);
    // Nor does it publish the previous call's live ALG ID and key id as its own: they are that call's. The voice path
    // still decrypts with them, exactly as it always has.
    assert(call.ota_target_id == 3333U && call.algid == 0U && call.kid == 0U && call.crypto_received == 0U);
    assert(state->payload_algid == 0x21 && state->payload_keyid == 0x05 && state->payload_mi == 0x11223344ULL);
    // Its own checked PI then gives it its own crypto, and notes it.
    const int notes_before = notes[0];
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    call = call_on(state, 0U);
    assert(call.ota_target_id == 3333U && received_is(&call, RX_BOTH, 0x21U, 0x05U) && call.algid == 0x21U);
    assert(notes[0] == notes_before + 1 && noted[0].algid == 0x21U);
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

/* A replacing call's inherited crypto stays the replaced call's across the heals of unverified terminators: the heal
   puts the live values back for decryption, and with them the mark that they are not this call's, so the call's next
   embedded LC does not publish them as its own -- after one heal, and after a second with no LC or PI in between. */
static void
test_heal_keeps_the_inherited_mark(dsd_opts* opts) {
    for (int heals = 1; heals <= 2; heals++) {
        dsd_state* state = fresh_state();
        dsd_decode_clock_use_test(20000000000ULL);
        run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
        run_pi(opts, state, 0U, 0x21U, 0x05U);
        assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
        dsd_decode_clock_test_set_ns(20300000000ULL);
        run_lc(opts, state, 0U, 1U, 0x40U, 3333U, 4444U);
        dsd_call_snapshot call = call_on(state, 0U);
        assert(call.ota_target_id == 3333U && call.algid == 0U && call.kid == 0U);
        uint64_t now_ns = 20300000000ULL;
        for (int i = 0; i < heals; i++) {
            assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
            now_ns += 100000000ULL;
            dsd_decode_clock_test_set_ns(now_ns);
            run_unverified_terminator(opts, state, 0U, 3333U, 4444U);
            assert(state->payload_algid == 0 && state->payload_keyid == 0);
            now_ns += 50000000ULL;
            dsd_decode_clock_test_set_ns(now_ns);
            run_media_mark(state, 0U);
            assert(state->payload_algid == 0x21 && state->payload_keyid == 0x05);
        }
        run_lc(opts, state, 0U, 3U, 0x40U, 3333U, 4444U);
        call = call_on(state, 0U);
        assert(call.ota_target_id == 3333U && call.algid == 0U && call.kid == 0U && call.crypto_received == 0U);
        assert(noted_valid[0] && noted[0].algid == 0U && noted[0].kid == 0U);
        assert(state->payload_algid == 0x21 && state->payload_keyid == 0x05 && state->payload_mi == 0x11223344ULL);
        // B's own checked PI still makes the values its own.
        run_pi(opts, state, 0U, 0x21U, 0x05U);
        call = call_on(state, 0U);
        assert(call.algid == 0x21U && call.kid == 0x05U && received_is(&call, RX_BOTH, 0x21U, 0x05U));
        dsd_decode_clock_use_system();
        free_state(state);
    }
}

static int
event_row_names_alg(const Event_History_I* history) {
    return strstr(history->Event_History_Items[0].event_string, "ALG:") != NULL;
}

/* A transmission that replaces a call whose terminator was missed takes nothing of that call's crypto into its own:
   not into its call record, its history row or the Monitor's call view, whether it is clear or encrypted. The live
   decryption inputs are left exactly as they always were: the previous call's, so a --dmr-force-algid fallback does
   not apply over them (it fills only an empty ALG ID), as before. */
static void
test_missed_terminator_publishes_no_stale_crypto(dsd_opts* opts) {
    static Event_History_I history[2];
    dsd_state* state = fresh_state();
    DSD_MEMSET(history, 0, sizeof(history));
    init_event_history(&history[0], 0, 1);
    init_event_history(&history[1], 0, 1);
    state->event_history_s = history;
    dsd_decode_clock_use_test(9000000000ULL);
    run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
    assert(event_row_names_alg(&history[0]));
    dsd_app_slot_call view;
    dsd_app_slot_call_view(state, 0U, dsd_decode_now_mono_s(), &view);
    assert(view.enc != 0U && view.algid == 0x21U && view.kid == 0x05U);

    // A clear call on the slot, its header the first thing heard of it.
    dsd_decode_clock_test_set_ns(9500000000ULL);
    run_lc(opts, state, 0U, 1U, 0x00U, 3333U, 4444U);
    dsd_call_snapshot call = call_on(state, 0U);
    assert(call.ota_target_id == 3333U && call.crypto == DSD_CALL_CRYPTO_CLEAR && call.algid == 0U && call.kid == 0U);
    assert(call.enc_received == 0U); // its checked LC says clear
    assert(!event_row_names_alg(&history[0]));
    dsd_app_slot_call_view(state, 0U, dsd_decode_now_mono_s(), &view);
    assert(view.enc == 0U && view.algid == 0U && view.kid == 0U);

    // An encrypted call after it, with no PI of its own: encrypted, and no ALG ID or key id.
    run_pi(opts, state, 1U, 0x21U, 0x09U); // the other slot's own PI, held for its own call
    assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
    dsd_decode_clock_test_set_ns(10000000000ULL);
    run_lc(opts, state, 0U, 1U, 0x40U, 5555U, 6666U);
    call = call_on(state, 0U);
    assert(call.ota_target_id == 5555U && call.algid == 0U && call.kid == 0U);
    // The carried values are not its own, but its checked LC's privacy bit is: a detection shows it "Encrypted".
    assert(call.enc_received == 1U);
    dsd_app_slot_call_view(state, 0U, dsd_decode_now_mono_s(), &view);
    assert(view.enc != 0U && view.algid == 0U);
    assert(!event_row_names_alg(&history[0]));
    assert(state->payload_algid == 0x21 && state->payload_keyid == 0x05);
    state->M = 0x24;
    state->currentslot = 0;
    assert(dsd_dmr_apply_forced_algid(state) == 0);
    run_lc(opts, state, 0U, 3U, 0x40U, 5555U, 6666U);
    call = call_on(state, 0U);
    assert(call.algid == 0U && call.kid == 0U && call.crypto_received == 0U);
    state->M = 0;
    // The other slot's held PI was untouched by this slot's calls.
    assert(state->dmr_pi_precall_rx[1] == RX_BOTH);
    dsd_decode_clock_use_system();
    state->event_history_s = NULL;
    free_state(state);
}

/* The decoder's own choice for a replacing call is that call's own. A --dmr-force-algid value, or the ALG ID the
   late-entry path infers from a loaded key, each with the 0xFF placeholder key id, left live by a call whose terminator
   was missed is exactly what the voice path chooses for the replacing call too: its record, history row and Monitor
   view show it, as before, and never as received. */
static void
test_replacing_call_shows_the_decoders_choice(dsd_opts* opts) {
    static Event_History_I history[2];
    for (int inferred = 0; inferred < 2; ++inferred) {
        dsd_state* state = fresh_state();
        DSD_MEMSET(history, 0, sizeof(history));
        init_event_history(&history[0], 0, 1);
        init_event_history(&history[1], 0, 1);
        state->event_history_s = history;
        dsd_decode_clock_use_test(40000000000ULL);
        run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
        uint8_t want = 0U;
        if (inferred) {
            state->R = 0x1122334455ULL; // a loaded 40-bit key: the late-entry path infers 0x21
            state->payload_algid = 0x21;
            state->payload_keyid = 0xFF;
            want = 0x21U;
        } else {
            state->M = 0x24;
            state->currentslot = 0;
            assert(dsd_dmr_apply_forced_algid(state) == 1);
            want = 0x24U;
        }
        run_lc(opts, state, 0U, 3U, 0x40U, 1001U, 2002U);
        assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
        dsd_decode_clock_test_set_ns(40500000000ULL);
        run_lc(opts, state, 0U, 1U, 0x40U, 3333U, 4444U);
        dsd_call_snapshot call = call_on(state, 0U);
        assert(call.ota_target_id == 3333U && call.algid == want && call.kid == 0xFFU && call.crypto_received == 0U);
        assert(noted_valid[0] && noted[0].ota_target_id == 3333U && noted[0].crypto_received == 0U);
        assert(event_row_names_alg(&history[0]));
        dsd_app_slot_call view;
        dsd_app_slot_call_view(state, 0U, dsd_decode_now_mono_s(), &view);
        assert(view.enc != 0U && view.algid == want && view.kid == 0xFFU);
        // Decryption keeps the same live values, as it always has.
        assert(state->payload_algid == (int)want && state->payload_keyid == 0xFF);
        state->M = 0;
        dsd_decode_clock_use_system();
        state->event_history_s = NULL;
        free_state(state);
    }
    // The same values are the previous call's when the decoder would not choose them for this call, so its record and
    // history row show no ALG ID: (0) an inferred one with no key loaded; (1) an inferred one for a clear call (SO 0)
    // with a key loaded; (2) an inferred one while --dmr-force-algid is set alongside a loaded key (the inference never
    // runs then); (3) a forced one for a clear call.
    for (int c = 0; c < 4; ++c) {
        dsd_state* state = fresh_state();
        DSD_MEMSET(history, 0, sizeof(history));
        init_event_history(&history[0], 0, 1);
        init_event_history(&history[1], 0, 1);
        state->event_history_s = history;
        dsd_decode_clock_use_test(41000000000ULL);
        run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
        state->payload_algid = c == 3 ? 0x24 : 0x21;
        state->payload_keyid = 0xFF;
        if (c == 1 || c == 2) {
            state->R = 0x1122334455ULL;
        }
        if (c >= 2) {
            state->M = 0x24;
        }
        run_lc(opts, state, 0U, 3U, 0x40U, 1001U, 2002U);
        assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
        dsd_decode_clock_test_set_ns(41500000000ULL);
        const uint8_t so = (c == 1 || c == 3) ? 0x00U : 0x40U;
        run_lc(opts, state, 0U, 1U, so, 3333U, 4444U);
        dsd_call_snapshot call = call_on(state, 0U);
        assert(call.ota_target_id == 3333U && call.algid == 0U && call.kid == 0U && call.crypto_received == 0U);
        assert(!event_row_names_alg(&history[0]));
        state->M = 0;
        dsd_decode_clock_use_system();
        state->event_history_s = NULL;
        free_state(state);
    }
}

/* A late-entry SB of the replacing call, decoded after its LC, belongs to it even when it repeats the values the
   replaced call left live: the next LC publishes them as the call's own, as before, and never as received. */
static void
test_sb_after_the_fork_is_the_calls_own(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    opts->dmr_le = 1;
    dsd_decode_clock_use_test(42000000000ULL);
    run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
    run_pi(opts, state, 0U, 0x25U, 0x44U);
    assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
    dsd_decode_clock_test_set_ns(42500000000ULL);
    run_lc(opts, state, 0U, 1U, 0x40U, 3333U, 4444U);
    dsd_call_snapshot call = call_on(state, 0U);
    assert(call.ota_target_id == 3333U && call.algid == 0U && call.kid == 0U);
    load_single_burst(state, 0U);
    state->currentslot = 0;
    dmr_sbrc(opts, state, 0);
    assert(state->payload_algid == 0x25 && state->payload_keyid == 0x44);
    run_lc(opts, state, 0U, 3U, 0x40U, 3333U, 4444U);
    call = call_on(state, 0U);
    assert(call.ota_target_id == 3333U && call.algid == 0x25U && call.kid == 0x44U && call.crypto_received == 0U);
    opts->dmr_le = 0;
    dsd_decode_clock_use_system();
    free_state(state);
}

/* A transmission whose identity the slot never learned (an identity-less voice epoch) and that carried voice is
   replaced by a call whose LC names one. The canonical layer keeps the epoch and names it after the new call; the
   ALG ID, key id and received bits the replaced transmission published into it are not the new call's. */
static void
test_specialized_epoch_drops_the_replaced_crypto(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    dsd_decode_clock_use_test(43000000000ULL);
    const dsd_call_observation provisional = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    assert(dsd_call_state_observe(state, &provisional, DSD_CALL_BOUNDARY_BEGIN) > 0);
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    const dsd_call_snapshot before = call_on(state, 0U);
    assert(before.ota_target_id == 0U && received_is(&before, RX_BOTH, 0x21U, 0x05U) && before.algid == 0x21U);
    assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
    dsd_decode_clock_test_set_ns(43500000000ULL);
    run_lc(opts, state, 0U, 1U, 0x40U, 3333U, 4444U);
    const dsd_call_snapshot after = call_on(state, 0U);
    assert(after.epoch == before.epoch && after.ota_target_id == 3333U);
    assert(after.algid == 0U && after.kid == 0U && after.mi == 0ULL && after.crypto_received == 0U);
    assert(after.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    assert(noted_valid[0] && noted[0].ota_target_id == 3333U && noted[0].crypto_received == 0U);
    // Decryption keeps the live values, as it always has.
    assert(state->payload_algid == 0x21 && state->payload_keyid == 0x05 && state->payload_mi == 0x11223344ULL);
    // Its own PI then gives it its own crypto.
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    const dsd_call_snapshot own = call_on(state, 0U);
    assert(own.ota_target_id == 3333U && own.algid == 0x21U && received_is(&own, RX_BOTH, 0x21U, 0x05U));
    dsd_decode_clock_use_system();
    free_state(state);
}

/* Documented limit: the next transmission's PI, its voice LC header lost and the previous call's terminator missed,
   decodes while that call is still active, so it is attributed to that call. The call its LC then opens shows no ALG ID
   or key id (they are the previous call's record's) until a later PI of its own; the live decryption inputs are the
   PI's, as they always were. */
static void
test_pi_while_active_belongs_to_that_call(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    dsd_decode_clock_use_test(20000000000ULL);
    run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    assert(dsd_call_state_update_media(state, 0U, 1, 0.0) > 0);
    dsd_decode_clock_test_set_ns(20500000000ULL);
    run_pi(opts, state, 0U, 0x25U, 0x07U);
    dsd_call_snapshot call = call_on(state, 0U);
    assert(call.ota_target_id == 1001U && received_is(&call, RX_BOTH, 0x25U, 0x07U) && call.algid == 0x25U);
    assert(state->dmr_pi_precall_rx[0] == 0U);
    dsd_decode_clock_test_set_ns(20800000000ULL);
    run_lc(opts, state, 0U, 3U, 0x40U, 3333U, 4444U);
    call = call_on(state, 0U);
    assert(call.ota_target_id == 3333U && call.algid == 0U && call.kid == 0U && call.crypto_received == 0U);
    assert(state->payload_algid == 0x25 && state->payload_keyid == 0x07);
    dsd_decode_clock_use_system();
    free_state(state);
}

/* A held PI waits for its own transmission's LC only as long as a voice LC header may repeat: an LC long after it
   belongs to no transmission the PI led. */
static void
test_held_pi_expires(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    dsd_decode_clock_use_test(30000000000ULL);
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    assert(state->dmr_pi_precall_rx[0] == RX_BOTH);
    dsd_decode_clock_test_set_ns(30000000000ULL + (uint64_t)((DMR_PI_PRECALL_HOLD_S + 0.5) * 1e9));
    run_lc(opts, state, 0U, 1U, 0x40U, 1001U, 2002U);
    dsd_call_snapshot call = call_on(state, 0U);
    assert(call.crypto_received == 0U && state->dmr_pi_precall_rx[0] == 0U);
    // Inside the window it transfers (the both-slots case covers the immediate LC).
    run_lc(opts, state, 0U, 2U, 0x40U, 1001U, 2002U);
    dsd_decode_clock_test_set_ns(40000000000ULL);
    run_pi(opts, state, 0U, 0x21U, 0x05U);
    dsd_decode_clock_test_set_ns(40000000000ULL + (uint64_t)((DMR_PI_PRECALL_HOLD_S - 0.5) * 1e9));
    run_lc(opts, state, 0U, 1U, 0x40U, 1111U, 2222U);
    call = call_on(state, 0U);
    assert(call.ota_target_id == 1111U && received_is(&call, RX_BOTH, 0x21U, 0x05U));
    dsd_decode_clock_use_system();
    free_state(state);
}

/* What the search's evidence hook and the scan hold each heard from the last report. */
static int evidence_reports;
static uint32_t evidence_target, evidence_source;
static int scan_reports;
static uint32_t scan_target;

static void
capture_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source, const char* access) {
    (void)state;
    (void)access;
    assert(strcmp(protocol, "DMR") == 0);
    ++evidence_reports;
    evidence_target = target;
    evidence_source = source;
}

static void
capture_scan_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source, int is_private,
                      int encrypted, int data_call) {
    (void)opts;
    (void)state;
    (void)source;
    (void)is_private;
    (void)encrypted;
    (void)data_call;
    ++scan_reports;
    scan_target = target;
}

/* An LC the RAS heuristic accepted under -F confirms DMR to a search and still holds a scanned RAS repeater, but its
   target and source name no detection: only an LC whose own CRC verified does. */
static void
test_ras_lc_names_no_detection(dsd_opts* opts) {
    dsd_state* state = fresh_state();
    dsd_signal_search_evidence_set(capture_evidence);
    dsd_trunk_scan_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof(hooks));
    hooks.dmr_conventional_activity = capture_scan_activity;
    dsd_trunk_scan_hooks_set(&hooks);
    for (uint8_t slot = 0U; slot < 2U; ++slot) {
        evidence_reports = scan_reports = 0;
        state->event_crc_invalid[slot] = DSD_CRC_RAS_UNVERIFIED;
        run_lc(opts, state, slot, 1U, 0x00U, 1001U, 2002U);
        state->event_crc_invalid[slot] = 0U;
        assert(evidence_reports == 1 && evidence_target == 0U && evidence_source == 0U);
        assert(scan_reports == 1 && scan_target == 1001U);
        run_lc(opts, state, slot, 3U, 0x00U, 1001U, 2002U);
        assert(evidence_reports == 2 && evidence_target == 1001U && evidence_source == 2002U);
    }
    dsd_trunk_scan_hooks_set(NULL);
    dsd_signal_search_evidence_set(NULL);
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
    test_ras_lc_names_no_detection(opts);
    test_missed_terminator_publishes_no_stale_crypto(opts);
    test_pi_while_active_belongs_to_that_call(opts);
    test_held_pi_expires(opts);
    test_replacing_call_shows_the_decoders_choice(opts);
    test_sb_after_the_fork_is_the_calls_own(opts);
    test_specialized_epoch_drops_the_replaced_crypto(opts);
    test_heal_keeps_the_inherited_mark(opts);
    dsd_signal_search_call_set(NULL);
    free(opts);
    return 0;
}
