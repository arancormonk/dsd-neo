// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA downlink TM-SDU reconstruction (EN 300 392-2 clause 23.4.3.1.1): a MAC-RESOURCE with length indication
 * 111111 starts the TM-SDU, MAC-FRAG PDUs continue it and the MAC-END completes it, each fragment without its fill
 * bits. Fragment runs are kept per downlink timeslot. Blocks are built from the ETSI tables in tetra_test_pdu.h.
 */

#include <assert.h>
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
#include "tetra_test_pdu.h"

static int g_fail;

static void
check(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static dsd_state*
new_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    assert(state != NULL);
    assert(tetra_st(state) != NULL);
    return state;
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

/* BL-UDATA (table 21.10) carrying a CMCE D-SETUP (table 14.15) of a group call: 48 bits. A delivered TM-SDU shows as
 * the call record it creates. */
static void
put_d_setup(ttb_block* sdu, uint32_t call_id) {
    ttb_put(sdu, 2, 4); /* LLC PDU type 0010: BL-UDATA */
    ttb_put(sdu, TETRA_MLE_PD_CMCE, 3);
    ttb_put(sdu, TETRA_CMCE_D_SETUP, 5);
    ttb_put(sdu, call_id, 14);
    ttb_put(sdu, 4, 4);    /* call time-out */
    ttb_put(sdu, 0, 2);    /* hook method selection, simplex/duplex selection */
    ttb_put(sdu, 0x04, 8); /* basic service: clear speech, point to multipoint */
    ttb_put(sdu, 3, 2);    /* transmission granted to another user */
    ttb_put(sdu, 0, 1);    /* transmission request permission */
    ttb_put(sdu, 0, 4);    /* call priority */
    ttb_put(sdu, 0, 1);    /* O-bit */
}

static int
delivered(const dsd_state* state, uint16_t call_id) {
    return tetra_calls_peek(state, call_id) != NULL;
}

/* SCH/HD block whose only PDU is a first fragment (MAC-RESOURCE with length indication 111111) to @p ssi. */
static void
first_fragment(ttb_block* b, uint32_t ssi, const uint8_t* part, int nbits) {
    *b = (ttb_block){0};
    const int start = ttb_resource(b, 0, TETRA_MAC_ADDR_SSI, ssi);
    ttb_set(b, start + 7, 63, 6);
    ttb_resource_flags(b, 0);
    ttb_put_bits(b, part, nbits);
    ttb_finish(b, TTB_SCH_HD_BITS, start + 2);
}

static void
continuation(ttb_block* b, const uint8_t* part, int nbits) {
    *b = (ttb_block){0};
    const int fill = ttb_frag(b);
    ttb_put_bits(b, part, nbits);
    ttb_finish(b, TTB_SCH_HD_BITS, fill);
}

/* MAC-END with a basic slot granting element (table 21.96) ahead of the last fragment, then a Null PDU. */
static void
last_fragment(ttb_block* b, const uint8_t* part, int nbits) {
    *b = (ttb_block){0};
    const int start = ttb_end(b);
    ttb_set(b, start + 11, 1, 1); /* slot granting flag */
    ttb_put(b, 0x15, 8);          /* basic slot granting element */
    ttb_put(b, 0, 1);             /* channel allocation flag */
    ttb_put_bits(b, part, nbits);
    ttb_close_end(b, start);
    ttb_finish(b, TTB_SCH_HD_BITS, -1);
}

static void
parse(dsd_state* state, dsd_opts* opts, const ttb_block* b, int cc) {
    tetra_mac_parse_schd(b->bits, b->n, cc, opts, state);
}

static void
test_full_fragment_sequence(void) {
    ttb_block sdu = {0};
    put_d_setup(&sdu, 5);
    dsd_state* state = new_state();
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    const tetra_state_t* ts = tetra_st(state);
    const tetra_mac_fragment* f = &ts->mac_frag[0];
    ttb_block b;

    first_fragment(&b, 0x001234u, sdu.bits, 10);
    parse(state, opts, &b, 2);
    check(f->active && f->nbits == 10 && f->addr.ssi == 0x001234u && !delivered(state, 5),
          "first fragment buffered without its fill bits");
    continuation(&b, sdu.bits + 10, 8);
    parse(state, opts, &b, 2);
    check(f->active && f->nbits == 18 && !delivered(state, 5), "MAC-FRAG appended without its fill bits");
    last_fragment(&b, sdu.bits + 18, sdu.n - 18);
    parse(state, opts, &b, 2);
    check(!f->active && f->nbits == 0, "MAC-END closes the TM-SDU");
    check(delivered(state, 5), "TM-SDU taken from after the MAC-END header elements");
    check(tetra_calls_peek(state, 5)->called_ssi == 0x001234u, "rebuilt TM-SDU keeps the first fragment's address");
    free_state(state);
    free(opts);
}

static void
test_fragment_guards(void) {
    ttb_block sdu = {0};
    put_d_setup(&sdu, 7);
    dsd_state* state = new_state();
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    tetra_state_t* ts = tetra_st(state);
    tetra_mac_fragment* f = &ts->mac_frag[0];
    ttb_block b;

    continuation(&b, sdu.bits, 9);
    parse(state, opts, &b, 1);
    check(!f->active && f->nbits == 0, "MAC-FRAG without a first fragment is discarded");
    last_fragment(&b, sdu.bits, sdu.n);
    parse(state, opts, &b, 1);
    check(!delivered(state, 7), "MAC-END without a first fragment delivers nothing");

    first_fragment(&b, 0x001234u, sdu.bits, 9);
    parse(state, opts, &b, 1);
    continuation(&b, sdu.bits + 9, 5);
    parse(state, opts, &b, 2);
    check(!f->active && f->nbits == 0, "a fragment from another colour code ends the TM-SDU");

    /* Clause 23.4.3.1.1 i): a new first fragment on the channel drops the partial TM-SDU. */
    first_fragment(&b, 0x001234u, sdu.bits, 9);
    parse(state, opts, &b, 1);
    first_fragment(&b, 0x005678u, sdu.bits, 4);
    parse(state, opts, &b, 1);
    check(f->active && f->nbits == 4 && f->addr.ssi == 0x005678u, "a new first fragment replaces the partial one");

    f->nbits = TETRA_MAC_TM_SDU_MAX_BITS - 2;
    continuation(&b, sdu.bits, 8);
    parse(state, opts, &b, 1);
    check(!f->active && f->nbits == 0, "a TM-SDU longer than N.202 is discarded");
    free_state(state);
    free(opts);
}

/* Fragments of runs on different timeslots are never joined. */
static void
test_fragments_kept_per_timeslot(void) {
    ttb_block one = {0};
    ttb_block two = {0};
    put_d_setup(&one, 11);
    put_d_setup(&two, 22);
    dsd_state* state = new_state();
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    tetra_state_t* ts = tetra_st(state);
    ts->tdma_valid = 1;
    ttb_block b;

    ts->tn = 1;
    first_fragment(&b, 0x000011u, one.bits, 12);
    parse(state, opts, &b, 0);
    ts->tn = 2;
    first_fragment(&b, 0x000022u, two.bits, 12);
    parse(state, opts, &b, 0);
    ts->tn = 1;
    last_fragment(&b, one.bits + 12, one.n - 12);
    parse(state, opts, &b, 0);
    check(delivered(state, 11) && !delivered(state, 22) && tetra_calls_peek(state, 11)->called_ssi == 0x000011u,
          "timeslot 1 TM-SDU rebuilt from its own fragments");
    check(ts->mac_frag[2].active && ts->mac_frag[2].nbits == 12, "timeslot 2 TM-SDU still under way");
    ts->tn = 2;
    last_fragment(&b, two.bits + 12, two.n - 12);
    parse(state, opts, &b, 0);
    check(delivered(state, 22) && tetra_calls_peek(state, 22)->called_ssi == 0x000022u && !ts->mac_frag[2].active,
          "timeslot 2 TM-SDU rebuilt from its own fragments");
    free_state(state);
    free(opts);
}

/* N.203 (annex B.2): a partial TM-SDU is dropped after 4 slots of its timeslot without a fragment. */
static void
test_fragment_timeout(void) {
    ttb_block sdu = {0};
    put_d_setup(&sdu, 9);
    dsd_state* state = new_state();
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    const tetra_state_t* ts = tetra_st(state);
    ttb_block b;
    ttb_block other = {0};
    const int start = ttb_resource(&other, 0, TETRA_MAC_ADDR_SSI, 0x000099u);
    ttb_resource_flags(&other, 0);
    ttb_close_resource(&other, start);
    ttb_finish(&other, TTB_SCH_HD_BITS, -1);

    first_fragment(&b, 0x001234u, sdu.bits, 9);
    parse(state, opts, &b, 0);
    for (int half_slot = 0; half_slot < 7; half_slot++) {
        parse(state, opts, &other, 0);
    }
    check(ts->mac_frag[0].active, "7 half slots without a fragment keep the TM-SDU");
    parse(state, opts, &other, 0);
    check(!ts->mac_frag[0].active, "8 half slots without a fragment drop it");
    last_fragment(&b, sdu.bits + 9, sdu.n - 9);
    parse(state, opts, &b, 0);
    check(!delivered(state, 9), "a late MAC-END delivers nothing");
    free_state(state);
    free(opts);
}

int
main(void) {
    test_full_fragment_sequence();
    test_fragment_guards();
    test_fragments_kept_per_timeslot();
    test_fragment_timeout();
    if (g_fail != 0) {
        DSD_FPRINTF(stderr, "%d TETRA fragment reconstruction check(s) FAILED\n", g_fail);
        return 1;
    }
    DSD_FPRINTF(stderr, "TETRA fragment reconstruction tests passed\n");
    return 0;
}
