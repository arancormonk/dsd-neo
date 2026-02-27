// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void dmr_refresh_algids_on_error(dsd_opts* opts, dsd_state* state);

static unsigned long long int
hytera_expected_next_mi(unsigned long long int mi_value) {
    uint8_t mi[5];
    uint8_t taps[5] = {0x12, 0x24, 0x48, 0x22, 0x14};
    mi[0] = (uint8_t)((mi_value & 0xFF00000000ULL) >> 32U);
    mi[1] = (uint8_t)((mi_value & 0x00FF000000ULL) >> 24U);
    mi[2] = (uint8_t)((mi_value & 0x0000FF0000ULL) >> 16U);
    mi[3] = (uint8_t)((mi_value & 0x000000FF00ULL) >> 8U);
    mi[4] = (uint8_t)(mi_value & 0x00000000FFULL);

    for (int i = 0; i < 5; i++) {
        uint8_t bit = (uint8_t)((mi[i] >> 7U) & 1U);
        mi[i] = (uint8_t)(mi[i] << 1U);
        if (bit != 0U) {
            mi[i] ^= taps[i];
        }
        mi[i] |= bit;
    }

    unsigned long long int out = 0;
    for (int i = 0; i < 5; i++) {
        out <<= 8U;
        out |= mi[i];
    }
    return out;
}

static void
test_slot1_hytera_refresh_updates_slot1_mi(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);

    state->currentslot = 0;
    state->payload_algid = 0;
    state->payload_algidR = 0x02;
    state->payload_keyidR = 0x7F;
    state->payload_mi = 0x0123456789ULL;
    state->payload_miR = 0x0A1B2C3D4EULL;

    const unsigned long long int expected_r = hytera_expected_next_mi(state->payload_miR);
    dmr_refresh_algids_on_error(opts, state);

    assert(state->payload_mi == 0x0123456789ULL);
    assert(state->payload_miR == expected_r);
    assert(state->currentslot == 1);
    assert(state->dropL == 0);
    assert(state->dropR == 256);

    free(state);
    free(opts);
}

static void
test_slot1_refresh_gate_uses_slot1_algid(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts != NULL);
    assert(state != NULL);

    state->currentslot = 1;
    state->payload_algid = 0x02;
    state->payload_keyid = 0x55;
    state->payload_algidR = 0;
    state->payload_mi = 0x0011223344ULL;
    state->payload_miR = 0x0099887766ULL;

    const unsigned long long int expected_l = hytera_expected_next_mi(state->payload_mi);
    dmr_refresh_algids_on_error(opts, state);

    assert(state->payload_mi == expected_l);
    assert(state->payload_miR == 0x0099887766ULL);
    assert(state->currentslot == 0);
    assert(state->dropL == 256);
    assert(state->dropR == 0);

    free(state);
    free(opts);
}

int
main(void) {
    test_slot1_hytera_refresh_updates_slot1_mi();
    test_slot1_refresh_gate_uses_slot1_algid();
    printf("DMR BS error alg refresh: OK\n");
    return 0;
}
