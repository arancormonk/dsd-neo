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
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/position.h"
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

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency_display(const dsd_state* state, uint16_t channel) {
    (void)state;
    (void)channel;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_gps_report(const dsd_opts* opts, dsd_state* state, const uint8_t* input, size_t input_bits, uint32_t src,
                const dsd_position_origin* origin) {
    (void)opts;
    (void)state;
    (void)input;
    (void)input_bits;
    (void)src;
    (void)origin;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot, int len_bytes,
                      int packet_verified, const dsd_position_origin* origin) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
    (void)packet_verified;
    (void)origin;
    return 0U;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_payload_framed(const uint8_t* input, int len_bytes) {
    /* The real framing: line ends, then '$' or '!'. */
    for (int i = 0; input != NULL && i < len_bytes; i++) {
        unsigned octet = 0U;
        for (int b = 0; b < 8; b++) {
            octet = (octet << 1) | (input[(i * 8) + b] & 1U);
        }
        if (octet != (unsigned)'\r' && octet != (unsigned)'\n') {
            return octet == (unsigned)'$' || octet == (unsigned)'!';
        }
    }
    return 0;
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

/* VCALL (message type 0x01) of call type @p call_type: body at bit offset 8. */
static void
build_typed_vcall(uint8_t bits[96], uint8_t call_type, uint8_t cipher, uint8_t key_id, uint16_t source, uint16_t dest) {
    DSD_MEMSET(bits, 0, 96U);
    write_bits(bits, 2U, 0x01U, 6U); /* message type */
    write_bits(bits, 8U, 0x00U, 8U); /* cc option */
    write_bits(bits, 16U, call_type, 3U);
    write_bits(bits, 19U, 0x00U, 5U); /* voice call option */
    write_bits(bits, 24U, source, 16U);
    write_bits(bits, 40U, dest, 16U);
    write_bits(bits, 56U, cipher, 2U);
    write_bits(bits, 58U, key_id, 6U);
}

/* A group (001) VCALL. */
static void
build_vcall(uint8_t bits[96], uint8_t cipher, uint8_t key_id, uint16_t source, uint16_t dest) {
    build_typed_vcall(bits, 0x01U, cipher, key_id, source, dest);
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
    // ledger, never as a talkgroup-policy row. The disconnect it synthesizes was
    // never received, so the received identity keeps what the carrier sent.
    dsd_rxid_note_nxdn_location(&state, 0U, 0x123U, 7U);
    const dsd_received_identity before = state.rx_id;
    build_vcall(bits, 2U, 5U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_enc_lockout_entry_active(&state, 1234U, 1));
    assert(state.rx_id.valid == before.valid && state.rx_id.fresh == before.fresh && state.rx_id.epoch == before.epoch);
    assert(state.rx_id.nxdn_sys == 0x123U && state.rx_id.nxdn_site == 7U);
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

/* The call receives a VCALL's cipher and key id only once the hysteresis corroborated the encryption: a lone
   non-clear VCALL is published (as before) but not received. */
static void
test_vcall_cipher_received_once_corroborated(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[96];
    dsd_call_snapshot call;

    reset_fixture(&opts, &state, history);
    opts.trunk_enable = 0;
    build_vcall(bits, 2U, 5U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.algid == 2U && call.kid == 5U && call.crypto_received == 0U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.crypto_received == (DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID));
    assert(call.received_algid == 2U && call.received_kid == 5U);
    dsd_state_ext_free_all(&state);
}

/* A forced scrambler (-ns, M == 1) establishes cipher 1 on the decoder's own authority; that is not a received cipher.
   A checked VCALL advertising clear is held off by the hysteresis, so decryption keeps cipher 1, but nothing about it
   was received. A VCALL advertising the forced cipher is received only once the air itself corroborates it. */
static void
test_forced_cipher_is_not_received(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[96];
    dsd_call_snapshot call;

    reset_fixture(&opts, &state, history);
    opts.trunk_enable = 0;
    state.M = 1;
    nxdn_cipher_force(&state, 0x1U);
    build_vcall(bits, 0U, 0U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(state.nxdn_cipher_type == 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.algid == 1U && call.crypto_received == 0U);
    // Decryption keeps the forced scrambler, but the call's own checked VCALL said clear: nothing says encrypted, so a
    // detection shows no "Encrypted" for it (ENGINE_SIGNAL_SEARCH).
    assert(call.crypto != DSD_CALL_CRYPTO_CLEAR && call.enc_received == 0U);

    reset_fixture(&opts, &state, history);
    opts.trunk_enable = 0;
    state.M = 1;
    nxdn_cipher_force(&state, 0x1U);
    build_vcall(bits, 1U, 3U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(state.nxdn_cipher_type == 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.algid == 1U && call.kid == 3U && call.crypto_received == 0U);
    assert(call.enc_received == 1U); // this VCALL itself advertises the scrambler
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.crypto_received == (DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID));
    assert(call.received_algid == 1U && call.received_kid == 3U);
    dsd_state_ext_free_all(&state);
}

/* Received crypto belongs to the VCALL that carried it. A new call that changes cipher without a decoded release (AES
   to DES) is held off by the hysteresis, so decryption keeps AES a while, but its own VCALL carried DES: its record
   receives nothing until the air corroborates DES, then DES with its own key id. A lone contradicting VCALL inside a
   call withdraws what was received; the next VCALL carrying the cipher receives it again, and a continuation with the
   same cipher stays received. */
static void
test_received_crypto_is_the_current_vcalls(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[96];
    dsd_call_snapshot call;
    const uint8_t both = (uint8_t)(DSD_CALL_CRYPTO_RX_ALG | DSD_CALL_CRYPTO_RX_KID);

    reset_fixture(&opts, &state, history);
    opts.trunk_enable = 0;
    build_vcall(bits, 3U, 5U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.crypto_received == both && call.received_algid == 3U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.crypto_received == both && call.received_algid == 3U
           && call.received_kid == 5U);

    // The next call, DES, with no release decoded between them.
    const uint64_t aes_epoch = call.epoch;
    build_vcall(bits, 2U, 7U, 200U, 5678U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(state.nxdn_cipher_type == 3U); // decryption as before: the hysteresis holds AES
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.epoch != aes_epoch && call.ota_target_id == 5678U);
    assert(call.crypto_received == 0U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(state.nxdn_cipher_type == 2U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.crypto_received == both && call.received_algid == 2U
           && call.received_kid == 7U);

    // A lone contradicting VCALL in the DES call withdraws the received cipher; the next DES VCALL restores it.
    build_vcall(bits, 3U, 7U, 200U, 5678U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(state.nxdn_cipher_type == 2U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && (call.crypto_received & DSD_CALL_CRYPTO_RX_ALG) == 0U);
    build_vcall(bits, 2U, 7U, 200U, 5678U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.crypto_received == both && call.received_algid == 2U
           && call.received_kid == 7U);
    dsd_state_ext_free_all(&state);
}

/* An SCCH INFO1 (outbound) naming cipher @p cipher with key id @p key_id. */
static void
build_scch_info1(uint8_t message[32], uint8_t cipher, uint8_t key_id) {
    DSD_MEMSET(message, 0, 32U);
    write_bits(message, 0U, 0x3U, 2U); /* SF: INFO1 */
    write_bits(message, 16U, cipher, 2U);
    write_bits(message, 18U, key_id, 6U);
}

/* The NXDN call's crypto republishes (a VCALL_IV, an SCCH INFO1) while the slot still holds another protocol's open call,
   its end never decoded (-fa or a -Y step reaching an NXDN carrier before any VCALL there decoded): they describe no
   NXDN call, so that call keeps its own crypto. On the NXDN call itself they still apply. */
static void
test_republish_leaves_another_protocols_call_alone(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[96];
    uint8_t message[32];
    dsd_call_snapshot call;

    reset_fixture(&opts, &state, history);
    opts.trunk_enable = 0;
    const dsd_call_observation dmr = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 77U,
        .policy_target_id = 77U,
        .ota_source_id = 88U,
    };
    assert(dsd_call_state_observe(&state, &dmr, DSD_CALL_BOUNDARY_BEGIN) > 0);
    state.nxdn_cipher_type = 2U; /* a cipher the NXDN path holds from before */
    state.nxdn_key = 5U;
    DSD_MEMSET(bits, 0, sizeof(bits));
    write_bits(bits, 2U, 0x03U, 6U); /* VCALL_IV */
    write_bits(bits, 8U, 0x1122334455667788ULL, 64U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && DSD_SYNC_IS_DMR(call.protocol));
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.crypto == DSD_CALL_CRYPTO_UNKNOWN);
    assert(call.algid == 0U && call.kid == 0U && call.mi == 0U && call.audio_permitted == 0U);
    build_scch_info1(message, 2U, 6U);
    NXDN_decode_scch(&opts, &state, message, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && DSD_SYNC_IS_DMR(call.protocol));
    assert(call.crypto == DSD_CALL_CRYPTO_UNKNOWN && call.algid == 0U && call.kid == 0U && call.mi == 0U);

    // The NXDN call's own VCALL opens it; its INFO1 then republishes onto it.
    build_vcall(bits, 2U, 5U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && DSD_SYNC_IS_NXDN(call.protocol));
    assert(call.algid == 2U && call.kid == 5U);
    build_scch_info1(message, 2U, 6U);
    NXDN_decode_scch(&opts, &state, message, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && DSD_SYNC_IS_NXDN(call.protocol) && call.kid == 6U);
    dsd_state_ext_free_all(&state);
}

/* The lockout entry is the call type's endpoint: an individual call (100) locks its called radio out, never a talkgroup
   of that number. An interconnect (110) or speed dial (111) call's endpoint is a PSTN ID or a radio behind the
   gateway, so it takes no session-long lockout entry in either namespace; nor does a call of generic voice (011,
   101). */
static void
test_vcall_lockout_endpoint_by_call_type(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[96];

    dsd_call_snapshot call;
    reset_fixture(&opts, &state, history);
    build_typed_vcall(bits, 0x04U, 2U, 5U, 100U, 1234U);
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    assert(dsd_enc_lockout_entry_active(&state, 1234U, 0));
    assert(!dsd_enc_lockout_lookup(&state, 1234U, 1, NULL));
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED); // the forced DISC
    dsd_state_ext_free_all(&state);

    // Dropped all the same (the forced DISC ends the call), skipped as a call, never locked out for the session.
    static const uint8_t no_lockout[] = {0x03U, 0x05U, 0x06U, 0x07U};
    for (size_t i = 0; i < sizeof(no_lockout); i++) {
        reset_fixture(&opts, &state, history);
        build_typed_vcall(bits, no_lockout[i], 2U, 5U, 100U, 1234U);
        NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
        assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
        assert(state.nxdn_enc_skip_valid == 0U);
        NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
        assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
        assert(!dsd_enc_lockout_lookup(&state, 1234U, 0, NULL));
        assert(!dsd_enc_lockout_lookup(&state, 1234U, 1, NULL));
        /* The call alone, never an operator call skip of its number (issue #903). */
        assert(state.nxdn_enc_skip_valid == 1U && state.nxdn_enc_skip_target == 1234U
               && state.nxdn_enc_skip_source == 100U && state.nxdn_enc_skip_call_type == no_lockout[i]);
        assert(!dsd_tg_policy_call_skip_active(&state, 1234U, dsd_decode_now_mono_s()));
        dsd_state_ext_free_all(&state);
    }
}

/* An encrypted interconnect call (110) under --enc-lockout leaves no session-long lockout on its endpoint, but is still
   dropped and skipped as a call. */
static void
test_vcall_interconnect_no_endpoint_lockout(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[96];

    reset_fixture(&opts, &state, history);
    build_typed_vcall(bits, 0x06U, 3U, 9U, 100U, 4321U);
    for (int i = 0; i < 3; i++) {
        NXDN_Elements_Content_decode(&opts, &state, bits, sizeof(bits));
    }
    assert(state.dmr_encL == 1);
    assert(!dsd_enc_lockout_lookup(&state, 4321U, 0, NULL));
    assert(!dsd_enc_lockout_lookup(&state, 4321U, 1, NULL));
    // Still suppressed: dropped by the forced DISC and skipped as a call for a short while.
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    assert(state.nxdn_enc_skip_valid == 1U && state.nxdn_enc_skip_target == 4321U
           && state.nxdn_enc_skip_call_type == 6U);
    dsd_state_ext_free_all(&state);
}

int
main(void) {
    test_observe_tentative_then_corroborated();
    test_lone_contradiction_is_quarantined_both_directions();
    test_contradiction_of_tentative_replaces();
    test_force_and_reset();
    test_vcall_enc_lockout_requires_corroboration();
    test_vcall_lockout_endpoint_by_call_type();
    test_vcall_interconnect_no_endpoint_lockout();
    test_lone_contradicting_vcall_does_not_flap_established_clear();
    test_vcall_cipher_received_once_corroborated();
    test_forced_cipher_is_not_received();
    test_received_crypto_is_the_current_vcalls();
    test_republish_leaves_another_protocols_call_alone();
    printf("NXDN enc class: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
