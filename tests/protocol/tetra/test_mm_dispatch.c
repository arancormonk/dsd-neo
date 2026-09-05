// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Unit tests for tetra_mm_dispatch() and tetra_carrier_to_dl_hz().
 * Phase 11: MM decoder + SYSINFO DL carrier frequency computation.
 *
 * Bit layout convention: one byte per bit, value 0 or 1, MSB-first.
 */

#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/opts.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Minimal helpers
 * ----------------------------------------------------------------------- */
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

/* dsd_state is very large — always heap-allocate to avoid stack overflow */
static dsd_state *alloc_state(void)
{
    return (dsd_state *)calloc(1, sizeof(dsd_state));
}

static dsd_opts *alloc_opts(void)
{
    return (dsd_opts *)calloc(1, sizeof(dsd_opts));
}

/* pack an n-bit value v into bits[] starting at offset off (MSB first) */
static void pack_bits(uint8_t *bits, int off, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        bits[off + (n - 1 - i)] = (uint8_t)((v >> i) & 1u);
    }
}

/* -----------------------------------------------------------------------
 * test_carrier_to_dl_hz
 *
 * Verify the formula and the ETSI offset-code mapping:
 * 00=0, 01=+6.25 kHz, 10=-6.25 kHz, 11=+12.5 kHz.
 * ----------------------------------------------------------------------- */
static void test_carrier_to_dl_hz(void)
{
    /* band 0 base = 390 000 000 Hz */
    long hz = tetra_carrier_to_dl_hz(100, 0, 0);
    CHECK(hz == 390000000L + 100L * 25000L,
          "band 0, carrier=100, offset=0 → 392 500 000");

    /* band 4 base = 460 000 000 Hz */
    hz = tetra_carrier_to_dl_hz(50, 4, 0);
    CHECK(hz == 460000000L + 50L * 25000L,
          "band 4, carrier=50, offset=0 → 461 250 000");

    /* band 4 with offset=2 */
    hz = tetra_carrier_to_dl_hz(50, 4, 2);
    CHECK(hz == 460000000L + 50L * 25000L - 6250L,
          "band 4, carrier=50, offset code 2 (-6.25 kHz)");

    hz = tetra_carrier_to_dl_hz(50, 4, 3);
    CHECK(hz == 460000000L + 50L * 25000L + 12500L,
          "band 4, carrier=50, offset code 3 (+12.5 kHz)");

    /* band 8 base = 876 025 000 Hz */
    hz = tetra_carrier_to_dl_hz(0, 8, 0);
    CHECK(hz == 876025000L, "band 8, carrier=0, offset=0 → 876 025 000");

    /* unknown band → 0 */
    hz = tetra_carrier_to_dl_hz(100, 6, 0);
    CHECK(hz == 0L, "band 6 (reserved) → 0");

    hz = tetra_carrier_to_dl_hz(100, 15, 0);
    CHECK(hz == 0L, "band 15 (reserved) → 0");

    fprintf(stderr, "[test_carrier_to_dl_hz] done\n");
}

/* -----------------------------------------------------------------------
 * test_d_lu_accept
 *
 * Build a D-LOCATION-UPDATING-ACCEPT PDU (type=5 = 0b00101) with
 * LA-present=1, LA=1234, then call tetra_mm_dispatch() and check state.
 * ----------------------------------------------------------------------- */
static void test_d_lu_accept(void)
{
    /*
     * PDU type 5 = 0b00101 → bits [0..4] = {0,0,1,0,1}
     * Bit  5 : LA-present = 1
     * Bits 6-19: LA = 1234 (14 bits)
     * Total needed: 20 bits
     */
    const int nbits = 20;
    uint8_t bits[20];
    memset(bits, 0, sizeof(bits));

    pack_bits(bits, 0, 5u, 5);     /* PDU type = 5 */
    pack_bits(bits, 5, 1u, 1);     /* LA-present = 1 */
    pack_bits(bits, 6, 1234u, 14); /* LA = 1234 */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mm_dispatch(bits, nbits, 0, opts, state);

    CHECK(state->tetra_mm_la_valid == 1,
          "D-LU-ACCEPT: tetra_mm_la_valid should be 1");
    CHECK(state->tetra_mm_la == 1234,
          "D-LU-ACCEPT: tetra_mm_la should be 1234");

    fprintf(stderr, "[test_d_lu_accept] la_valid=%u la=%u\n",
            state->tetra_mm_la_valid, state->tetra_mm_la);
    free(state);
    free(opts);
}

/* -----------------------------------------------------------------------
 * test_d_lu_accept_no_la
 *
 * D-LU-ACCEPT with LA-present=0 — state should stay unchanged.
 * ----------------------------------------------------------------------- */
static void test_d_lu_accept_no_la(void)
{
    const int nbits = 10;
    uint8_t bits[10];
    memset(bits, 0, sizeof(bits));

    pack_bits(bits, 0, 5u, 5);  /* PDU type = 5 */
    pack_bits(bits, 5, 0u, 1);  /* LA-present = 0 */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mm_dispatch(bits, nbits, 0, opts, state);

    CHECK(state->tetra_mm_la_valid == 0,
          "D-LU-ACCEPT no LA: tetra_mm_la_valid should remain 0");

    fprintf(stderr, "[test_d_lu_accept_no_la] la_valid=%u\n",
            state->tetra_mm_la_valid);
    free(state);
    free(opts);
}

/* -----------------------------------------------------------------------
 * test_d_attach_group
 *
 * Build a D-ATTACH-DETACH-GROUP PDU (type=14 = 0b01110) with:
 *   attach (0), class=0, addr_type=0 (GSI), group_ssi=0x456789
 * ----------------------------------------------------------------------- */
static void test_d_attach_group(void)
{
    /*
     * PDU type 14 = 0b01110 → 5 bits
     * Bit  5 : detach = 0 (attach)
     * Bit  6 : class_of_grp = 0
     * Bits 7-8 : addr_type = 0 (GSI)
     * Bits 9-32: group_ssi = 0x456789 (24 bits)
     * Total needed: 33 bits
     */
    const int nbits = 33;
    uint8_t bits[33];
    memset(bits, 0, sizeof(bits));

    pack_bits(bits, 0, 14u,        5);  /* PDU type = 14 */
    pack_bits(bits, 5,  0u,        1);  /* detach = 0 */
    pack_bits(bits, 6,  0u,        1);  /* class_of_grp = 0 */
    pack_bits(bits, 7,  0u,        2);  /* addr_type = 0 (GSI) */
    pack_bits(bits, 9,  0x456789u, 24); /* group_ssi */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mm_dispatch(bits, nbits, 0, opts, state);

    CHECK(state->tetra_mm_group_ssi == 0x456789u,
          "D-ATTACH-GROUP: tetra_mm_group_ssi should be 0x456789");

    fprintf(stderr, "[test_d_attach_group] group_ssi=0x%06X\n",
            state->tetra_mm_group_ssi);
    free(state);
    free(opts);
}

/* -----------------------------------------------------------------------
 * test_d_attach_group_gtsi
 *
 * addr_type = 1 (GTSI, not GSI): group_ssi should stay 0.
 * ----------------------------------------------------------------------- */
static void test_d_attach_group_gtsi(void)
{
    const int nbits = 33;
    uint8_t bits[33];
    memset(bits, 0, sizeof(bits));

    pack_bits(bits, 0, 14u,        5); /* PDU type 14 */
    pack_bits(bits, 5,  0u,        1); /* attach */
    pack_bits(bits, 6,  0u,        1); /* class=0 */
    pack_bits(bits, 7,  1u,        2); /* addr_type = 1 (GTSI) */
    pack_bits(bits, 9,  0xABCDEFu, 24);

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mm_dispatch(bits, nbits, 0, opts, state);

    CHECK(state->tetra_mm_group_ssi == 0u,
          "D-ATTACH-GROUP GTSI: group_ssi should stay 0 for non-GSI addr_type");

    fprintf(stderr, "[test_d_attach_group_gtsi] group_ssi=%u (expected 0)\n",
            state->tetra_mm_group_ssi);
    free(state);
    free(opts);
}

/* -----------------------------------------------------------------------
 * test_dispatch_unknown
 *
 * Unknown PDU type should not crash or modify state.
 * ----------------------------------------------------------------------- */
static void test_dispatch_unknown(void)
{
    uint8_t bits[10];
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0, 31u, 5); /* PDU type 31 = unrecognised */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();
    opts->errorbars = 0;

    tetra_mm_dispatch(bits, 10, 0, opts, state);

    CHECK(state->tetra_mm_la_valid   == 0, "unknown PDU: la_valid unchanged");
    CHECK(state->tetra_mm_group_ssi  == 0, "unknown PDU: group_ssi unchanged");

    fprintf(stderr, "[test_dispatch_unknown] passed\n");
    free(state);
    free(opts);
}

/* -----------------------------------------------------------------------
 * test_dispatch_null_state
 *
 * NULL state should not cause a crash.
 * ----------------------------------------------------------------------- */
static void test_dispatch_null_state(void)
{
    uint8_t bits[20];
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0, 5u,    5);  /* D-LU-ACCEPT */
    pack_bits(bits, 5, 1u,    1);  /* LA present */
    pack_bits(bits, 6, 999u, 14);  /* LA = 999 */

    dsd_opts *opts = alloc_opts();

    /* Should not crash */
    tetra_mm_dispatch(bits, 20, 0, opts, NULL);

    fprintf(stderr, "[test_dispatch_null_state] passed (no crash)\n");
    free(opts);
}

/* -----------------------------------------------------------------------
 * test_d_disable / test_d_enable
 *
 * D-DISABLE (PDU type 3) should set tetra_ms_enabled=0.
 * D-ENABLE  (PDU type 4) should set tetra_ms_enabled=1.
 * ----------------------------------------------------------------------- */
static void test_d_disable(void)
{
    uint8_t bits[5];
    memset(bits, 0, sizeof bits);
    pack_bits(bits, 0, 3u, 5);  /* PDU type = 3 (D-DISABLE) */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();
    state->tetra_ms_enabled = 1;  /* pre-condition: enabled */

    tetra_mm_dispatch(bits, 5, 0, opts, state);

    CHECK(state->tetra_ms_enabled == 0,
          "D-DISABLE: tetra_ms_enabled should be 0");

    fprintf(stderr, "[test_d_disable] ms_enabled=%u\n", state->tetra_ms_enabled);
    free(state);
    free(opts);
}

static void test_d_enable(void)
{
    uint8_t bits[5];
    memset(bits, 0, sizeof bits);
    pack_bits(bits, 0, 4u, 5);  /* PDU type = 4 (D-ENABLE) */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();
    state->tetra_ms_enabled = 0;  /* pre-condition: disabled */

    tetra_mm_dispatch(bits, 5, 0, opts, state);

    CHECK(state->tetra_ms_enabled == 1,
          "D-ENABLE: tetra_ms_enabled should be 1");

    fprintf(stderr, "[test_d_enable] ms_enabled=%u\n", state->tetra_ms_enabled);
    free(state);
    free(opts);
}

/* -----------------------------------------------------------------------
 * test_d_subscriber_class_assign
 *
 * D-SUBSCRIBER-CLASS-ASSIGN (PDU type 10 = 0b01010):
 *   Bits 0-4  : pdu_type = 10
 *   Bits 5-20 : SCG = TEST_SCG (16 bits)
 * Expected: state->tetra_subscr_class = TEST_SCG
 * ----------------------------------------------------------------------- */
static void test_d_subscriber_class_assign(void)
{
    const uint32_t TEST_SCG = 0xF0A5u;
    const int nbits = 21;
    uint8_t bits[21];
    memset(bits, 0, sizeof bits);

    pack_bits(bits, 0, 10u,      5);   /* PDU type = 10 */
    pack_bits(bits, 5, TEST_SCG, 16);  /* SCG (16 bits) */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mm_dispatch(bits, nbits, 0, opts, state);

    CHECK(state->tetra_subscr_class == (uint16_t)TEST_SCG,
          "D-SUBSCR-CLASS-ASSIGN: tetra_subscr_class should match SCG");

    fprintf(stderr, "[test_d_subscr_class] tetra_subscr_class=0x%04X\n",
            state->tetra_subscr_class);
    free(state);
    free(opts);
}

/* -----------------------------------------------------------------------
 * test_d_mm_status
 *
 * D-MM-STATUS (PDU type 15 = 0b01111):
 *   Bits 0-4  : pdu_type = 15
 *   Bits 5-12 : status_val = TEST_STATUS (8 bits)
 * Expected: state->tetra_mm_status_code = TEST_STATUS
 * ----------------------------------------------------------------------- */
static void test_d_mm_status(void)
{
    const uint32_t TEST_STATUS = 0xA7u;
    const int nbits = 13;
    uint8_t bits[13];
    memset(bits, 0, sizeof bits);

    pack_bits(bits, 0, 15u,        5);   /* PDU type = 15 */
    pack_bits(bits, 5, TEST_STATUS, 8);  /* status_val (8 bits) */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mm_dispatch(bits, nbits, 0, opts, state);

    CHECK(state->tetra_mm_status_code == (uint8_t)TEST_STATUS,
          "D-MM-STATUS: tetra_mm_status_code should match status_val");

    fprintf(stderr, "[test_d_mm_status] tetra_mm_status_code=%u\n",
            state->tetra_mm_status_code);
    free(state);
    free(opts);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    test_carrier_to_dl_hz();
    test_d_lu_accept();
    test_d_lu_accept_no_la();
    test_d_attach_group();
    test_d_attach_group_gtsi();
    test_dispatch_unknown();
    test_dispatch_null_state();
    test_d_disable();
    test_d_enable();
    test_d_subscriber_class_assign();
    test_d_mm_status();

    if (g_failures == 0) {
        printf("PASS  tetra_mm_dispatch: all checks passed\n");
        return 0;
    } else {
        printf("FAIL  tetra_mm_dispatch: %d check(s) failed\n", g_failures);
        return 1;
    }
}
