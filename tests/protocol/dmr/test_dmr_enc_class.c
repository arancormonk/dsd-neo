// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Per-slot DMR encryption-classification hysteresis: strong evidence applies
 * at once, weak evidence applies tentatively and must repeat before it can
 * establish or flip a classification, and only an established encrypted
 * classification may arm the enc lockout. Also covers the PI-header paths
 * that feed the hysteresis as strong evidence, including the Hytera PI
 * checksum gate.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_opts s_opts;
static dsd_state s_state;

static void
reset_fixture(void) {
    DSD_MEMSET(&s_opts, 0, sizeof(s_opts));
    DSD_MEMSET(&s_state, 0, sizeof(s_state));
}

static void
test_first_weak_observation_is_tentative(void) {
    reset_fixture();
    unsigned int so = dmr_enc_class_observe(&s_state, 0U, 0x40U, 0);
    assert((so & 0x40U) != 0U);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 0);

    // A matching repeat corroborates.
    so = dmr_enc_class_observe(&s_state, 0U, 0x40U, 0);
    assert((so & 0x40U) != 0U);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 1);
}

static void
test_strong_observation_establishes_immediately(void) {
    reset_fixture();
    const unsigned int so = dmr_enc_class_observe(&s_state, 0U, 0x40U, 1);
    assert((so & 0x40U) != 0U);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 1);
}

static void
test_lone_contradiction_of_established_is_quarantined(void) {
    reset_fixture();
    // Established clear via a strong observation.
    (void)dmr_enc_class_observe(&s_state, 0U, 0x00U, 1);

    // One corrupt embedded LC claiming encryption: the established clear
    // classification stays applied, so the audio gate does not flap and the
    // lockout cannot arm.
    unsigned int so = dmr_enc_class_observe(&s_state, 0U, 0x40U, 0);
    assert((so & 0x40U) == 0U);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 0);

    // A matching clear observation clears the quarantine.
    so = dmr_enc_class_observe(&s_state, 0U, 0x00U, 0);
    assert((so & 0x40U) == 0U);

    // Two consecutive contradictions flip the classification: the call
    // really changed shape.
    (void)dmr_enc_class_observe(&s_state, 0U, 0x40U, 0);
    so = dmr_enc_class_observe(&s_state, 0U, 0x40U, 0);
    assert((so & 0x40U) != 0U);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 1);
}

static void
test_contradiction_of_tentative_replaces(void) {
    reset_fixture();
    // A lone corrupt LC opening a late entry classifies tentatively...
    (void)dmr_enc_class_observe(&s_state, 0U, 0x40U, 0);
    // ...and the next (clear) observation replaces it instead of being
    // quarantined behind a value that never corroborated.
    unsigned int so = dmr_enc_class_observe(&s_state, 0U, 0x00U, 0);
    assert((so & 0x40U) == 0U);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 0);
    so = dmr_enc_class_observe(&s_state, 0U, 0x00U, 0);
    assert((so & 0x40U) == 0U);
}

static void
test_strong_contradiction_applies_immediately(void) {
    reset_fixture();
    (void)dmr_enc_class_observe(&s_state, 0U, 0x40U, 0);
    (void)dmr_enc_class_observe(&s_state, 0U, 0x40U, 0);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 1);

    // A CRC-verified voice LC header saying clear wins at once.
    const unsigned int so = dmr_enc_class_observe(&s_state, 0U, 0x00U, 1);
    assert((so & 0x40U) == 0U);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 0);
}

static void
test_non_privacy_bits_pass_through(void) {
    reset_fixture();
    (void)dmr_enc_class_observe(&s_state, 0U, 0x00U, 1);
    // Emergency and priority bits are not the classifier's business; only the
    // privacy bit is filtered.
    const unsigned int so = dmr_enc_class_observe(&s_state, 0U, 0xC3U, 0);
    assert((so & 0x80U) != 0U);
    assert((so & 0x03U) == 0x03U);
    assert((so & 0x40U) == 0U);
}

static void
test_force_and_reset(void) {
    reset_fixture();
    dmr_enc_class_force(&s_state, 1U, 1);
    assert(dmr_enc_class_established_enc(&s_state, 1U) == 1);
    dmr_enc_class_reset(&s_state, 1U);
    assert(dmr_enc_class_established_enc(&s_state, 1U) == 0);
    assert(s_state.dmr_enc_class[1] == DMR_ENC_CLASS_NONE);
}

static void
test_slot_isolation(void) {
    reset_fixture();
    (void)dmr_enc_class_observe(&s_state, 0U, 0x40U, 1);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 1);
    assert(dmr_enc_class_established_enc(&s_state, 1U) == 0);
    const unsigned int so = dmr_enc_class_observe(&s_state, 1U, 0x00U, 0);
    assert((so & 0x40U) == 0U);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 1);
}

static uint8_t
hytera_additive_checksum(const uint8_t* bytes, size_t length) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum = (uint8_t)(checksum + bytes[i]);
    }
    return (uint8_t)(0U - checksum);
}

static void
test_hytera_pi_checksum_gates_crypto_writes(void) {
    reset_fixture();
    s_state.currentslot = 0;

    uint8_t pi[11] = {0x02U, 0x68U, 0x34U, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xABU, 0x00U, 0x00U};

    // Corrupt checksum: nothing may reach the live crypto or the classifier.
    pi[9] = (uint8_t)(hytera_additive_checksum(pi, 9U) ^ 0x5AU);
    dmr_pi(&s_opts, &s_state, pi, 0U, 0U);
    assert((s_state.dmr_so & 0x40U) == 0U);
    assert(s_state.payload_algid == 0);
    assert(s_state.payload_keyid == 0);
    assert(s_state.payload_mi == 0ULL);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 0);

    // Verified checksum: strong evidence, applied and established.
    pi[9] = hytera_additive_checksum(pi, 9U);
    dmr_pi(&s_opts, &s_state, pi, 0U, 0U);
    assert((s_state.dmr_so & 0x40U) != 0U);
    assert(s_state.payload_algid == 0x02);
    assert(s_state.payload_keyid == 0x34);
    assert(s_state.payload_mi == 0x0123456789ULL);
    assert(dmr_enc_class_established_enc(&s_state, 0U) == 1);
    dsd_state_ext_free_all(&s_state);
}

static void
test_kirisun_pi_establishes_classification(void) {
    reset_fixture();
    s_state.currentslot = 1;

    uint8_t pi[11] = {0x36U, 0x0AU, 0x40U, 0x01U, 0x23U, 0x45U, 0x67U, 0x11U, 0x22U, 0x33U, 0x00U};
    dmr_pi(&s_opts, &s_state, pi, 1U, 0U);
    assert((s_state.dmr_soR & 0x40U) != 0U);
    assert(dmr_enc_class_established_enc(&s_state, 1U) == 1);
    dsd_state_ext_free_all(&s_state);
}

int
main(void) {
    test_first_weak_observation_is_tentative();
    test_strong_observation_establishes_immediately();
    test_lone_contradiction_of_established_is_quarantined();
    test_contradiction_of_tentative_replaces();
    test_strong_contradiction_applies_immediately();
    test_non_privacy_bits_pass_through();
    test_force_and_reset();
    test_slot_isolation();
    test_hytera_pi_checksum_gates_crypto_writes();
    test_kirisun_pi_establishes_classification();
    return 0;
}
