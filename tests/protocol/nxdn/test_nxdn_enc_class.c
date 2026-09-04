// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN cipher-classification hysteresis: observations apply tentatively and
 * must repeat before they can flip an established classification or arm the
 * enc lockout, so a single corrupt VCALL/SACCH-2/SCCH element can no longer
 * mute a clear call, unmute an encrypted one, or write the session-permanent
 * blocking talkgroup entry and force the channel released.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, const uint8_t* ElementsContent,
                                  size_t elements_bits);

/*
 * Link stubs for nxdn_element.c entrypoints irrelevant to the classification
 * paths under test (mirrors test_nxdn_element_bounds.c).
 */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_message_type(const dsd_opts* opts, dsd_state* state, uint8_t MessageType) {
    (void)opts;
    (void)state;
    (void)MessageType;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_decode_arib(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits) {
    (void)opts;
    (void)state;
    (void)message_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_decode_prop(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits) {
    (void)opts;
    (void)state;
    (void)message_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_reset(dsd_state* state) {
    (void)state;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel) {
    (void)opts;
    (void)state;
    (void)channel;
    return 0;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel) {
    (void)state;
    (void)channel;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_gps_report(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_sentence_checker(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t slot, int len_bytes) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_trunk_diag_log_missing_channel_once(const dsd_opts* opts, dsd_state* state, uint16_t channel, const char* label) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)label;
}

static void
write_bits(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

/* VCALL (message type 0x01): body at bit offset 8. */
static void
build_vcall(uint8_t bits[96], uint8_t cipher, uint8_t key_id, uint16_t source, uint16_t dest) {
    DSD_MEMSET(bits, 0, 96U);
    write_bits(bits, 2U, 0x01U, 6U);  /* message type */
    write_bits(bits, 8U, 0x00U, 8U);  /* cc option */
    write_bits(bits, 16U, 0x01U, 3U); /* call type: group */
    write_bits(bits, 19U, 0x00U, 5U); /* voice call option */
    write_bits(bits, 24U, source, 16U);
    write_bits(bits, 40U, dest, 16U);
    write_bits(bits, 56U, cipher, 2U);
    write_bits(bits, 58U, key_id, 6U);
}

static void
reset_fixture(dsd_opts* opts, dsd_state* state, Event_History_I* history) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(history, 0, sizeof(*history) * 2U);
    init_event_history(&history[0], 0, 1);
    init_event_history(&history[1], 0, 1);
    state->event_history_s = history;
    opts->trunk_enable = 1;
    opts->trunk_tune_enc_calls = 0;
    /* These cases feed VCALL element content directly. In the decoder that content arrives
     * through a CRC that has already confirmed the transmission, which is what lets a VCALL
     * publish at all (issue #398); the gate itself is covered by NXDN_CONFIRM. */
    state->nxdn_confirmed = 1;
}

/* --- Hysteresis unit coverage ------------------------------------------- */

static void
test_observe_tentative_then_corroborated(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(nxdn_cipher_observe(&state, 2U, 0) == 2U);
    assert(nxdn_cipher_established_enc(&state) == 0);
    assert(nxdn_cipher_observe(&state, 2U, 0) == 2U);
    assert(nxdn_cipher_established_enc(&state) == 1);
}

static void
test_lone_contradiction_is_quarantined_both_directions(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    // Established clear; one non-clear observation is held.
    (void)nxdn_cipher_observe(&state, 0U, 0);
    (void)nxdn_cipher_observe(&state, 0U, 0);
    assert(nxdn_cipher_observe(&state, 1U, 0) == 0U);
    assert(nxdn_cipher_established_enc(&state) == 0);
    // A matching clear observation clears the quarantine.
    assert(nxdn_cipher_observe(&state, 0U, 0) == 0U);

    // Established encrypted; one clear observation must not unmute.
    nxdn_cipher_class_reset(&state);
    (void)nxdn_cipher_observe(&state, 3U, 0);
    (void)nxdn_cipher_observe(&state, 3U, 0);
    assert(nxdn_cipher_established_enc(&state) == 1);
    assert(nxdn_cipher_observe(&state, 0U, 0) == 3U);
    // The corroborated repeat flips it.
    assert(nxdn_cipher_observe(&state, 0U, 0) == 0U);
}

static void
test_contradiction_of_tentative_replaces(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    (void)nxdn_cipher_observe(&state, 2U, 0);
    assert(nxdn_cipher_observe(&state, 0U, 0) == 0U);
    assert(nxdn_cipher_established_enc(&state) == 0);
}

static void
test_force_and_reset(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    nxdn_cipher_force(&state, 1U);
    assert(state.nxdn_cipher_type == 1U);
    assert(nxdn_cipher_established_enc(&state) == 1);
    nxdn_cipher_class_reset(&state);
    assert(nxdn_cipher_established_enc(&state) == 0);
    assert(state.nxdn_cipher_class == 0U);
}

/* --- VCALL end-to-end through NXDN_Elements_Content_decode --------------- */

static void
test_vcall_enc_lockout_requires_corroboration(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[96];
    dsd_tg_policy_lookup lookup;

    reset_fixture(&opts, &state, history);

    // First CRC-good non-clear VCALL: classifies (and mutes) tentatively, but
    // no blocking entry and no forced disconnect.
    build_vcall(bits, 2U, 5U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(state.nxdn_cipher_type == 2U);
    assert(state.dmr_encL == 1);
    assert(!dsd_enc_lockout_lookup(&state, 1234U, 1, NULL));

    // The matching repeat corroborates and the lockout acts -- in the session
    // ledger, never as a talkgroup-policy row.
    build_vcall(bits, 2U, 5U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_enc_lockout_entry_active(&state, 1234U, 1));
    dsd_enc_lockout_entry ledger_entry;
    assert(dsd_enc_lockout_lookup(&state, 1234U, 1, &ledger_entry) == 1);
    assert(ledger_entry.algid == 2);
    assert(dsd_tg_policy_lookup_id(&state, 1234U, &lookup) == 0);
    assert(lookup.match == DSD_TG_POLICY_MATCH_NONE);
    dsd_state_ext_free_all(&state);
}

static void
test_lone_contradicting_vcall_does_not_flap_established_clear(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[96];
    dsd_call_snapshot call;

    reset_fixture(&opts, &state, history);

    // Two clear VCALLs establish the call clear.
    build_vcall(bits, 0U, 0U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(state.nxdn_cipher_type == 0U);

    // A lone corrupt VCALL claiming DES: quarantined -- audio stays open, the
    // published crypto stays clear, no blocking entry, no forced disconnect.
    build_vcall(bits, 2U, 5U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(state.nxdn_cipher_type == 0U);
    assert(state.dmr_encL == 0);
    assert(!dsd_enc_lockout_lookup(&state, 1234U, 1, NULL));
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.crypto == DSD_CALL_CRYPTO_CLEAR);
    assert(call.audio_permitted == 1U);

    // The repeat corroborates a real change: the classification flips and the
    // lockout acts.
    build_vcall(bits, 2U, 5U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(state.nxdn_cipher_type == 2U);
    assert(dsd_enc_lockout_entry_active(&state, 1234U, 1));
    dsd_state_ext_free_all(&state);
}

int
main(void) {
    test_observe_tentative_then_corroborated();
    test_lone_contradiction_is_quarantined_both_directions();
    test_contradiction_of_tentative_replaces();
    test_force_and_reset();
    test_vcall_enc_lockout_requires_corroboration();
    test_lone_contradicting_vcall_does_not_flap_established_clear();
    printf("NXDN enc class: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
