// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 47 test suite.
 *
 * Covers functionality introduced in Phases 43-46:
 *
 * TDMA clock (EN 300 392-2 clauses 7.3 and 7.4): the BSCH sets TN, FN and MN as sent (all 1-based on air), normal
 * downlink bursts run the clock by the slots counted since, wrapping FN 18 to 1 and MN 60 to 1 and counting
 * hyperframes, and the initial value of EN 300 392-7 clause 6.3.2.1 never reads 0 across those wraps.
 *
 * Phase 45 — D-TX-GRANTED updates UI fields:
 *  12.  tetra_sds_msg_ref and tetra_sds_last_cc zero-initialise correctly
 *  13.  D-TX-GRANTED with SSI sets lastsrc to granted SSI
 *  14.  D-TX-GRANTED with SSI sets active_channel[0] to "TETRA TG:... SRC:...  (TX-GRANTED)"
 *  15.  D-TX-GRANTED with SSI sets last_active_time != 0
 *
 * Phase 46 — SDS message reference tracking:
 *  16.  D-SDS-DATA stores msg_ref in tetra_sds_msg_ref
 *  17.  D-SDS-DATA stores CC in tetra_sds_last_cc
 *  18.  Two SDS PDUs with different msg_ref values update sds_msg_ref correctly
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_calls.h"
#include "tetra_mle_pdu.h"
#include "tetra_state.h"
#include "tetra_test_log.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  PASS: %s\n", msg);                                                                               \
            g_pass++;                                                                                                  \
        } else {                                                                                                       \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                                           \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void
pack_bits(uint8_t* out, uint32_t val, int offset, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        out[offset++] = (uint8_t)((val >> i) & 1u);
    }
}

static dsd_state*
alloc_state(void) {
    return (dsd_state*)calloc(1, sizeof(dsd_state));
}

static dsd_opts*
alloc_opts(void) {
    return (dsd_opts*)calloc(1, sizeof(dsd_opts));
}

/* A SYNC PDU (EN 300 392-2 table 21.76): `tn_air` is the 2-bit timeslot element (0 for timeslot 1); fn and mn are
 * written as given, legal or not. */
static void
bsch_build(uint8_t* bits, uint8_t colour, uint8_t tn_air, uint8_t fn, uint8_t mn, uint16_t mcc, uint16_t mnc) {
    DSD_MEMSET(bits, 0, 60);
    pack_bits(bits, colour, 4, 6);
    pack_bits(bits, tn_air, 10, 2);
    pack_bits(bits, fn, 12, 5);
    pack_bits(bits, mn, 17, 6);
    pack_bits(bits, mcc, 31, 10);
    pack_bits(bits, mnc, 41, 14);
}

/* -----------------------------------------------------------------------
 * MLE / CMCE helper — wrap a raw CMCE PDU in an CMCE protocol-discriminator envelope.
 * ----------------------------------------------------------------------- */
static void
wrap_mle_cmce(const uint8_t* cmce_body, int cmce_nbits, uint8_t* out, int* out_nbits) {
    DSD_MEMSET(out, 0, (size_t)(3 + cmce_nbits));
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = 3 + cmce_nbits;
}

/* -----------------------------------------------------------------------
 * Helper: build an MM protocol-discriminator wrapper.
 *   mle_type=24, pd=5 (MM), then mm_body
 * ----------------------------------------------------------------------- */
/* =======================================================================
 * TDMA clock
 * ======================================================================= */

static void
free_state(dsd_state* st) {
    if (st) {
        dsd_state_ext_free_all(st);
    }
    free(st);
}

/* Parse a BSCH for network 234/30 colour 0x15 as the decoder does, with the synchronization burst read through block 2
 * when dsd_state::symbolcnt is `symbolcnt`. */
static int
bsch_at(dsd_state* st, uint32_t symbolcnt, uint8_t tn_air, uint8_t fn, uint8_t mn) {
    uint8_t bits[60];
    dsd_opts* opt = alloc_opts();
    bsch_build(bits, 0x15, tn_air, fn, mn, 234, 30);
    st->symbolcnt = symbolcnt;
    const int ok = tetra_bsch_parse(bits, 60, opt, st);
    free(opt);
    return ok;
}

/* A normal downlink burst `slots` slots after the slot whose start is `slot_start`, followed `jitter` symbols late. */
static void
ndb_at(dsd_state* st, uint32_t slot_start, uint32_t slots, uint32_t jitter) {
    st->symbolcnt = slot_start + slots * TETRA_TDMA_SLOT_SYMBOLS + TETRA_TDMA_NDB_SYNC_SYMBOL + jitter;
    tetra_tdma_advance_ndb(st);
}

static int
clock_is(const dsd_state* st, uint8_t tn, uint8_t fn, uint8_t mn) {
    const tetra_state_t* ts = tetra_st_peek(st);
    return ts->tdma_valid && ts->tn == tn && ts->fn == fn && ts->mn == mn;
}

static void
test_bsch_sets_clock(void) {
    printf("[test_bsch_sets_clock]\n");
    dsd_state* st = alloc_state();

    CHECK(tetra_st(st)->tdma_valid == 0 && tetra_st(st)->tn == 0, "no clock before a BSCH");
    CHECK(bsch_at(st, 1000, 0, 5, 12) == 1 && clock_is(st, 1, 5, 12), "timeslot element 0 is TN 1; FN 5, MN 12");
    CHECK(tetra_st(st)->net_known && tetra_st(st)->mcc == 234 && tetra_st(st)->mnc == 30
              && tetra_st(st)->colour == 0x15,
          "network identity");
    CHECK(tetra_st(st)->tdma_slot_start == 1000u - TETRA_TDMA_SB_PARSE_SYMBOL, "the clock's slot start");
    CHECK(bsch_at(st, 5000, 3, 18, 60) == 1 && clock_is(st, 4, 18, 60), "TN 4, FN 18 and MN 60 are legal");
    CHECK(bsch_at(st, 9000, 2, 15, 30) == 1 && clock_is(st, 3, 15, 30), "a later BSCH sets the clock again");
    CHECK(tetra_bsch_parse(NULL, 60, NULL, st) == 0, "no bits, no BSCH");
    {
        uint8_t bits[60];
        bsch_build(bits, 0x15, 0, 1, 1, 234, 30);
        CHECK(tetra_bsch_parse(bits, 59, NULL, st) == 0 && clock_is(st, 3, 15, 30), "a short PDU changes nothing");
    }
    free_state(st);
}

/* Frame 0 and multiframe 0 are reserved (table 21.76): such a BSCH names the network but sets no clock. */
static void
test_bsch_reserved_numbers(void) {
    printf("[test_bsch_reserved_numbers]\n");
    dsd_state* st = alloc_state();

    CHECK(bsch_at(st, 1000, 0, 0, 1) == 1 && tetra_st(st)->net_known && tetra_st(st)->tdma_valid == 0,
          "FN 0 sets no clock");
    CHECK(bsch_at(st, 2000, 0, 1, 0) == 1 && tetra_st(st)->tdma_valid == 0, "MN 0 sets no clock");
    CHECK(bsch_at(st, 3000, 0, 19, 1) == 1 && tetra_st(st)->tdma_valid == 0, "FN 19 sets no clock");
    CHECK(bsch_at(st, 4000, 0, 1, 61) == 1 && tetra_st(st)->tdma_valid == 0, "MN 61 sets no clock");
    CHECK(bsch_at(st, 5000, 0, 1, 1) == 1 && clock_is(st, 1, 1, 1), "FN 1 MN 1 sets it");
    CHECK(bsch_at(st, 5000 + 255, 0, 0, 1) == 1 && tetra_st(st)->tdma_valid == 0,
          "a reserved number stops a running clock");
    free_state(st);
}

static void
test_ndb_follows_slots(void) {
    printf("[test_ndb_follows_slots]\n");
    dsd_state* st = alloc_state();
    const uint32_t s0 = 2000u - TETRA_TDMA_SB_PARSE_SYMBOL;

    tetra_st(st)->tn = 3;
    tetra_st(st)->fn = 9;
    tetra_st(st)->mn = 37;
    ndb_at(st, 0, 1, 0);
    CHECK(tetra_st(st)->tdma_valid == 0 && tetra_st(st)->tn == 3 && tetra_st(st)->fn == 9,
          "a normal burst does not start a clock");

    bsch_at(st, 2000, 2, 9, 37);
    ndb_at(st, s0, 1, 0);
    CHECK(clock_is(st, 4, 9, 37), "the next slot is TN 4");
    ndb_at(st, s0, 2, 0);
    CHECK(clock_is(st, 1, 10, 37), "TN 4 is followed by TN 1 of the next frame");
    ndb_at(st, s0, 5, 0);
    CHECK(clock_is(st, 4, 10, 37), "three slots without a burst still count");
    ndb_at(st, s0, 6, 40);
    CHECK(clock_is(st, 1, 11, 37), "a burst found 40 symbols late is in its own slot");
    ndb_at(st, s0, 7, 0);
    ndb_at(st, s0, 7, 0);
    CHECK(clock_is(st, 2, 11, 37), "the same slot twice moves nothing");
    free_state(st);
}

/* EN 300 392-2 clause 7.3.2: FN 18 is followed by FN 1, MN 60 by MN 1 (and the next hyperframe). The 0-based wrap
 * this replaces made FN 0 and MN 0, whose initial value is 0, and skipped the hyperframe. */
static void
test_frame_and_multiframe_wrap(void) {
    printf("[test_frame_and_multiframe_wrap]\n");
    dsd_state* st = alloc_state();
    const uint32_t s0 = 3000u - TETRA_TDMA_SB_PARSE_SYMBOL;
    const uint32_t s1 = 9000u - TETRA_TDMA_SB_PARSE_SYMBOL;

    bsch_at(st, 3000, 3, 17, 59);
    ndb_at(st, s0, 4, 0);
    CHECK(clock_is(st, 4, 18, 59), "TN 4 of FN 17 is followed by FN 18");
    ndb_at(st, s0, 5, 0);
    CHECK(clock_is(st, 1, 1, 60), "FN 18 is followed by FN 1 of the next multiframe");
    CHECK(tetra_air_build_iv(tetra_st(st)->tn, tetra_st(st)->fn, tetra_st(st)->mn, 0, 0) != 0u,
          "the IV after the frame wrap is legal");

    bsch_at(st, 9000, 3, 18, 60);
    tetra_st(st)->hyperframe_valid = 1;
    tetra_st(st)->hyperframe = 7;
    ndb_at(st, s1, 1, 0);
    CHECK(clock_is(st, 1, 1, 1) && tetra_st(st)->hyperframe == 8, "MN 60 is followed by MN 1 of the next hyperframe");
    CHECK(tetra_air_build_iv(tetra_st(st)->tn, tetra_st(st)->fn, tetra_st(st)->mn, tetra_st(st)->hyperframe, 0)
              == ((8u << 13) | (1u << 7) | (1u << 2)),
          "the IV after the hyperframe wrap");

    tetra_st(st)->hyperframe = 0xFFFF;
    ndb_at(st, s1, 1 + 4u * 18u * 60u, 0);
    CHECK(tetra_st(st)->tdma_valid == 0, "a whole hyperframe without a burst stops the clock");
    free_state(st);
}

static void
test_hyperframe_count_long_run(void) {
    printf("[test_hyperframe_count_long_run]\n");
    dsd_state* st = alloc_state();
    uint32_t start = 500u - TETRA_TDMA_SB_PARSE_SYMBOL;

    bsch_at(st, 500, 0, 1, 1);
    tetra_st(st)->hyperframe_valid = 1;
    tetra_st(st)->hyperframe = 0xFFFF;
    /* One burst a frame for two hyperframes: the clock wraps twice and the 16-bit number with it. */
    for (uint32_t slot = 4; slot <= 2u * 4u * 18u * 60u; slot += 4) {
        ndb_at(st, start, slot, 0);
    }
    CHECK(clock_is(st, 1, 1, 1) && tetra_st(st)->hyperframe == 1, "two hyperframes later the number has moved by 2");
    free_state(st);
}

static void
test_clock_stops(void) {
    printf("[test_clock_stops]\n");
    dsd_state* st = alloc_state();
    const uint32_t s0 = 4000u - TETRA_TDMA_SB_PARSE_SYMBOL;

    bsch_at(st, 4000, 0, 1, 1);
    tetra_st(st)->hyperframe_valid = 1;
    ndb_at(st, s0, 72, 0);
    CHECK(clock_is(st, 1, 1, 2), "one multiframe without a burst is still followed");
    ndb_at(st, s0, 72 + 73, 0);
    CHECK(tetra_st(st)->tdma_valid == 0 && tetra_st(st)->hyperframe_valid == 0,
          "more than a multiframe without a burst stops the clock and the hyperframe count");

    bsch_at(st, 4000, 0, 1, 1);
    tetra_st(st)->hyperframe_valid = 1;
    ndb_at(st, s0 - 2u * TETRA_TDMA_SLOT_SYMBOLS, 0, 0);
    CHECK(tetra_st(st)->tdma_valid == 0 && tetra_st(st)->hyperframe_valid == 0, "a burst before the clock stops it");

    bsch_at(st, 0xFFFFFF00u, 0, 1, 1);
    ndb_at(st, 0xFFFFFF00u - TETRA_TDMA_SB_PARSE_SYMBOL, 2, 0);
    CHECK(st->symbolcnt < 0x1000u && clock_is(st, 3, 1, 1), "the clock is followed across the symbol count wrap");
    free_state(st);
}

/* A BSCH after the clock ran on its own: the hyperframe number follows the clock to the BSCH slot, and moves by one
 * when the BSCH puts that slot on the other side of a hyperframe boundary. */
static void
test_bsch_keeps_hyperframe(void) {
    printf("[test_bsch_keeps_hyperframe]\n");
    dsd_state* st = alloc_state();

    bsch_at(st, 1000, 3, 18, 60);
    tetra_st(st)->hyperframe_valid = 1;
    tetra_st(st)->hyperframe = 7;
    bsch_at(st, 1000 + 255, 0, 1, 1);
    CHECK(clock_is(st, 1, 1, 1) && tetra_st(st)->hyperframe == 8 && tetra_st(st)->hyperframe_valid,
          "a BSCH in the next slot carries the hyperframe across MN 60");

    bsch_at(st, 5000, 2, 18, 60);
    tetra_st(st)->hyperframe = 7;
    bsch_at(st, 5000 + 255, 0, 1, 1);
    CHECK(clock_is(st, 1, 1, 1) && tetra_st(st)->hyperframe == 8, "a clock one slot behind the BSCH is corrected");

    bsch_at(st, 9000, 0, 1, 1);
    tetra_st(st)->hyperframe = 8;
    bsch_at(st, 9000 + 255, 3, 18, 60);
    CHECK(clock_is(st, 4, 18, 60) && tetra_st(st)->hyperframe == 7, "a clock one slot ahead of the BSCH is corrected");

    {
        uint8_t bits[60];
        bsch_build(bits, 0x16, 0, 1, 1, 234, 30);
        st->symbolcnt = 9000 + 510;
        tetra_bsch_parse(bits, 60, NULL, st);
        CHECK(tetra_st(st)->hyperframe_valid == 0 && clock_is(st, 1, 1, 1) && tetra_st(st)->colour == 0x16,
              "another cell's BSCH drops the hyperframe number");
    }
    free_state(st);
}

/* =======================================================================
 * Phase 45: D-TX-GRANTED updates UI fields
 * ======================================================================= */

/*
 * Build a D-TX-GRANTED CMCE PDU with granted SSI present.
 *
 * CMCE D-TX-GRANTED layout (PDU type=11): mandatory call and floor-control
 * fields, O-bit, notification P-bit, transmitting-party P-bit/TPTI/SSI, and
 * the terminating M-bit. Total: 54 bits.
 */
static void
build_d_tx_granted(uint8_t* cmce, uint32_t granted_ssi) {
    DSD_MEMSET(cmce, 0, 54);
    pack_bits(cmce, 11u, 0, 5);           /* D-TX-GRANTED */
    pack_bits(cmce, 1u, 5, 14);           /* call identifier */
    pack_bits(cmce, 3u, 19, 2);           /* granted to another */
    pack_bits(cmce, 1u, 21, 1);           /* request permission */
    pack_bits(cmce, 0u, 22, 1);           /* encryption */
    pack_bits(cmce, 0u, 23, 1);           /* reserved */
    pack_bits(cmce, 1u, 24, 1);           /* O-bit */
    pack_bits(cmce, 0u, 25, 1);           /* notification absent */
    pack_bits(cmce, 1u, 26, 1);           /* TPTI present */
    pack_bits(cmce, 1u, 27, 2);           /* TPTI = SSI */
    pack_bits(cmce, granted_ssi, 29, 24); /* SSI */
    pack_bits(cmce, 0u, 53, 1);           /* terminating M-bit */
}

/* Call 1 to group @p gssi: a D-SETUP (table 14.15) in the MAC context of a MAC-RESOURCE addressed to the group. */
static void
setup_call(dsd_state* st, dsd_opts* opt, uint32_t gssi) {
    uint8_t cmce[41];
    DSD_MEMSET(cmce, 0, sizeof cmce);
    pack_bits(cmce, TETRA_CMCE_D_SETUP, 0, 5);
    pack_bits(cmce, 1u, 5, 14);    /* call identifier */
    pack_bits(cmce, 0x04u, 25, 8); /* basic service: clear speech, point to multipoint */
    uint8_t mle[3 + 41];
    int mle_nbits;
    wrap_mle_cmce(cmce, 41, mle, &mle_nbits);
    tetra_mac_sdu sdu;
    DSD_MEMSET(&sdu, 0, sizeof sdu);
    sdu.addr.type = TETRA_MAC_ADDR_SSI;
    sdu.addr.ssi = gssi;
    sdu.has_addr = 1;
    tetra_mle_dispatch_sdu(mle, mle_nbits, 0, &sdu, opt, st);
}

static void
test_tx_granted_identity_set(void) {
    printf("[test_tx_granted_identity_set]\n");

    uint8_t cmce[54];
    build_d_tx_granted(cmce, 88888u);

    uint8_t mle[9 + 54];
    int mle_nbits;
    wrap_mle_cmce(cmce, 54, mle, &mle_nbits);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    setup_call(st, opt, 44444u);
    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    const tetra_call_t* const c = tetra_calls_peek(st, 1);
    CHECK(c && c->talker_ssi == 88888u, "TX-GRANTED: talker = 88888");
    CHECK(c && c->phase == TETRA_CALL_TX_GRANTED, "TX-GRANTED: transmission granted");
    CHECK(c && c->called_ssi == 44444u, "TX-GRANTED: the called group is retained");

    free_state(st);
    free(opt);
}

static void
test_tx_granted_without_channel_assignment(void) {
    printf("[test_tx_granted_without_channel_assignment]\n");

    uint8_t cmce[54];
    build_d_tx_granted(cmce, 12345u);

    uint8_t mle[3 + 54];
    int mle_nbits;
    wrap_mle_cmce(cmce, 54, mle, &mle_nbits);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
    setup_call(st, opt, 44444u);
    tetra_calls_find(st, 1)->alloc.freq_hz = 390000000L;

    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    const tetra_call_t* const c = tetra_calls_peek(st, 1);
    CHECK(c && c->talker_ssi == 12345u, "TX-GRANTED: granted SSI is retained");
    CHECK(c && c->alloc.freq_hz == 390000000L, "TX-GRANTED: absent channel IE does not overwrite the call's channel");

    free_state(st);
    free(opt);
}

static void
test_tx_granted_floor_state(void) {
    printf("[test_tx_granted_floor_state]\n");

    uint8_t cmce[54];
    build_d_tx_granted(cmce, 111u);

    uint8_t mle[3 + 54];
    int mle_nbits;
    wrap_mle_cmce(cmce, 54, mle, &mle_nbits);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    setup_call(st, opt, 44444u);
    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    CHECK(tetra_calls_peek(st, 1)->phase == TETRA_CALL_TX_GRANTED, "TX-GRANTED: floor grant marked valid");

    free_state(st);
    free(opt);
}

/* =======================================================================
 * Phase 46: SDS message reference tracking
 * ======================================================================= */

/*
 * Build a minimal D-SDS-DATA CMCE PDU with an explicit msg_ref.
 *
 *   [0-4]   pdu_type = 23
 * The helper builds table 14.13 D-SDS-DATA with CPTI=1 and SDTI=0, including
 * the mandatory Annex E O-bit after the 16-bit user data.
 */
static void
build_d_sds_data(uint8_t* cmce, uint32_t src_ssi, uint8_t sdti) {
    DSD_MEMSET(cmce, 0, 61);
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1, 5, 2); /* CPTI: SSI */
    pack_bits(cmce, src_ssi, 7, 24);
    pack_bits(cmce, sdti, 31, 2);
    if (sdti == 0) {
        pack_bits(cmce, 0xCAFE, 33, 16);
    }
    if (sdti == 0) {
        pack_bits(cmce, 0, 49, 1); /* O-bit */
    }
}

/* A D-SDS-DATA decodes with its own calling party and data type, and its summary line names the colour code it came
 * with. */
static void
test_sds_msg_ref_stored(void) {
    printf("[test_sds_sdti_and_cc_stored]\n");
    uint8_t cmce[61];
    build_d_sds_data(cmce, 22222u, 0u);
    uint8_t mle[70];
    int n;
    wrap_mle_cmce(cmce, 50, mle, &n);
    tetra_sds_pdu pdu;
    CHECK(tetra_sds_decode(mle, n, &pdu) && pdu.data.sdti == 0 && pdu.data.calling.ssi == 22222u
              && pdu.data.short_data == 0xCAFE,
          "SDS SDTI = 0");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
    char log[256];
    tetra_test_mle_logged(mle, n, 3, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA CMCE D-SDS-DATA] CC=3 src_SSI=22222 CPTI=1 SDTI=0 data_bits=16") != NULL,
          "SDS summary line with CC = 3");
    free_state(st);
    free(opt);
}

static void
test_sds_last_cc_stored(void) {
    printf("[test_sds_last_cc_stored]\n");
    uint8_t cmce[61];
    build_d_sds_data(cmce, 33333u, 0u);
    uint8_t mle[70];
    int n;
    wrap_mle_cmce(cmce, 50, mle, &n);
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
    char log[256];
    tetra_test_mle_logged(mle, n, 11, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA CMCE D-SDS-DATA] CC=11 src_SSI=33333") != NULL, "SDS summary line with CC = 11");
    free_state(st);
    free(opt);
}

static void
test_sds_msg_ref_overwrite(void) {
    printf("[test_sds_source_overwrite]\n");
    uint8_t cmce[61], mle[70];
    int n;
    tetra_sds_pdu pdu;
    build_d_sds_data(cmce, 10001u, 0u);
    wrap_mle_cmce(cmce, 50, mle, &n);
    CHECK(tetra_sds_decode(mle, n, &pdu) && pdu.data.calling.ssi == 10001u, "first SDS source");
    build_d_sds_data(cmce, 10002u, 0u);
    wrap_mle_cmce(cmce, 50, mle, &n);
    CHECK(tetra_sds_decode(mle, n, &pdu) && pdu.data.calling.ssi == 10002u, "second SDS source");
}

/* =======================================================================
 * main
 * ======================================================================= */
int
main(void) {
    printf("=== TETRA Phase 47 Test Suite ===\n\n");

    printf("--- TDMA clock ---\n");
    test_bsch_sets_clock();
    test_bsch_reserved_numbers();
    test_ndb_follows_slots();
    test_frame_and_multiframe_wrap();
    test_hyperframe_count_long_run();
    test_clock_stops();
    test_bsch_keeps_hyperframe();

    /* Phase 45 */
    printf("\n--- Phase 45: D-TX-GRANTED UI fields ---\n");
    test_tx_granted_identity_set();
    test_tx_granted_without_channel_assignment();
    test_tx_granted_floor_state();

    /* Phase 46 */
    printf("\n--- Phase 46: SDS message reference tracking ---\n");
    test_sds_msg_ref_stored();
    test_sds_last_cc_stored();
    test_sds_msg_ref_overwrite();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
