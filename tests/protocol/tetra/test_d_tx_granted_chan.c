// SPDX-License-Identifier: GPL-3.0-or-later
/* ETSI EN 300 392-2 Table 14.18 D-TX-GRANTED regression tests. */
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "tetra_calls.h"
#include "tetra_state.h"

static int failures;
#define CHECK(c, m)                                                                                                    \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            DSD_FPRINTF(stderr, "FAIL: %s\n", m);                                                                      \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void
pack(uint8_t* b, uint32_t v, int off, int n) {
    for (int i = n - 1; i >= 0; --i) {
        b[off++] = (uint8_t)((v >> i) & 1u);
    }
}

static int
build(uint8_t* b, uint16_t call_id, uint8_t grant, uint8_t request_allowed, uint8_t encrypted, int party_present,
      uint32_t ssi) {
    DSD_MEMSET(b, 0, 80);
    pack(b, TETRA_MLE_PD_CMCE, 0, 3);
    pack(b, 11, 3, 5); /* D-TX-GRANTED */
    pack(b, call_id, 8, 14);
    pack(b, grant, 22, 2);
    pack(b, request_allowed, 24, 1);
    pack(b, encrypted, 25, 1);
    pack(b, 0, 26, 1);                       /* reserved */
    pack(b, party_present ? 1u : 0u, 27, 1); /* O-bit */
    if (!party_present) {
        return 28;
    }
    pack(b, 0, 28, 1); /* notification absent */
    pack(b, 1, 29, 1); /* TPTI present */
    pack(b, 1, 30, 2); /* TPTI: SSI */
    pack(b, ssi, 32, 24);
    pack(b, 0, 56, 1); /* terminating M-bit */
    return 57;
}

static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

/* Call @p call_id, set up by a D-SETUP (table 14.15) to a group, decoded without its MAC PDU. */
static void
setup_call(dsd_opts* opt, dsd_state* st, uint16_t call_id) {
    uint8_t b[80];
    DSD_MEMSET(b, 0, sizeof b);
    pack(b, TETRA_MLE_PD_CMCE, 0, 3);
    pack(b, TETRA_CMCE_D_SETUP, 3, 5);
    pack(b, call_id, 8, 14);
    pack(b, 0x04u, 28, 8); /* basic service: clear speech, point to multipoint */
    tetra_mle_dispatch(b, 44, 0, opt, st);
}

/* A grant to another user names the talker. Its encryption control is the end-to-end encryption of the transmission
 * (table 14.18): it never becomes the air interface encryption mode. */
static void
test_standard_layout_and_identity(void) {
    uint8_t bits[80];
    const uint32_t ssi = 0xABCDEFu;
    int nbits = build(bits, 0x2345u, 3, 1, 1, 1, ssi);
    dsd_state* st = calloc(1, sizeof(*st));
    dsd_opts* opt = calloc(1, sizeof(*opt));
    setup_call(opt, st, 0x2345u);
    tetra_mle_dispatch(bits, nbits, 0, opt, st);
    const tetra_call_t* const c = tetra_calls_peek(st, 0x2345u);
    CHECK(c && c->phase == TETRA_CALL_TX_GRANTED, "grant-to-another starts a transmission");
    CHECK(c && c->talker_ssi == ssi, "transmitting SSI decoded");
    CHECK(c && c->e2ee == 1, "encryption control marks the transmission end-to-end encrypted");
    CHECK(tetra_st(st)->enc_mode == TETRA_ENC_MODE_NONE, "the air interface encryption mode is not touched");
    free_state(st);
    free(opt);
}

static void
test_non_voice_grants_do_not_enable_audio(void) {
    uint8_t bits[80];
    dsd_state* st = calloc(1, sizeof(*st));
    dsd_opts* opt = calloc(1, sizeof(*opt));
    setup_call(opt, st, 1);
    int nbits = build(bits, 1, 1, 0, 0, 0, 0);
    tetra_mle_dispatch(bits, nbits, 0, opt, st);
    CHECK(tetra_calls_peek(st, 1)->phase == TETRA_CALL_SETUP, "not-granted starts no transmission");
    nbits = build(bits, 1, 2, 0, 0, 0, 0);
    tetra_mle_dispatch(bits, nbits, 0, opt, st);
    CHECK(tetra_calls_peek(st, 1)->phase == TETRA_CALL_SETUP, "queued starts no transmission");
    free_state(st);
    free(opt);
}

static void
test_cmce_never_overwrites_mac_allocation(void) {
    uint8_t bits[80];
    int nbits = build(bits, 7, 0, 1, 0, 0, 0);
    dsd_state* st = calloc(1, sizeof(*st));
    dsd_opts* opt = calloc(1, sizeof(*opt));
    setup_call(opt, st, 7);
    tetra_call_t* const c = tetra_calls_find(st, 7);
    c->alloc.carrier = 400;
    c->alloc.timeslots = 4;
    c->alloc.freq_hz = 470000000L;
    tetra_mle_dispatch(bits, nbits, 0, opt, st);
    CHECK(c->alloc.carrier == 400, "MAC carrier retained");
    CHECK(c->alloc.timeslots == 4, "MAC timeslot bitmap retained");
    CHECK(c->alloc.freq_hz == 470000000L, "MAC frequency retained");
    free_state(st);
    free(opt);
}

int
main(void) {
    test_standard_layout_and_identity();
    test_non_voice_grants_do_not_enable_audio();
    test_cmce_never_overwrites_mac_allocation();
    if (failures == 0) {
        puts("PASS test_d_tx_granted_chan");
    }
    return failures != 0;
}
