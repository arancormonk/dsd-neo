// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 85 test suite.
 *
 * Covers functionality introduced in Phases 81-84:
 *
 * Phase 81: tetra_bits.h shared header (bits_to_uint unification)
 * Phase 83: D-FACILITY parser
 * SDS transport messages are covered by test_mle_cmce.c; they are carried
 * inside D-SDS-DATA or D-STATUS, not separate CMCE PDU types.
 * Phase 84: channel_info_fmt extensions (release cause, access, mm_addr)
 */

#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_channel_info.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/opts.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            printf("  PASS: %s\n", msg); \
            g_pass++; \
        } else { \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
            g_fail++; \
        } \
    } while (0)

static void pack_bits(uint8_t *out, uint32_t val, int offset, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--)
        out[offset++] = (uint8_t)((val >> i) & 1u);
}

static dsd_state *alloc_state(void) { return (dsd_state *)calloc(1, sizeof(dsd_state)); }
static dsd_opts  *alloc_opts(void)  { return (dsd_opts  *)calloc(1, sizeof(dsd_opts));  }

static void wrap_mle_cmce(const uint8_t *cmce_body, int cmce_nbits,
                           uint8_t *out, int *out_nbits)
{
    int total = 3 + cmce_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = total;
}

/* -----------------------------------------------------------------------
 * Phase 81: tetra_bits_to_uint shared utility
 * ----------------------------------------------------------------------- */
static void test_tetra_bits_to_uint(void)
{
    printf("[Phase 81] tetra_bits_to_uint shared header\n");

    /* Pack 0xAB = 10101011 into bit array */
    uint8_t bits[32];
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0xAB, 0, 8);

    uint32_t v = tetra_bits_to_uint(bits, 0, 8);
    CHECK(v == 0xAB, "tetra_bits_to_uint full byte 0xAB");

    uint32_t hi = tetra_bits_to_uint(bits, 0, 4);
    CHECK(hi == 0x0A, "tetra_bits_to_uint high nibble 0x0A");

    uint32_t lo = tetra_bits_to_uint(bits, 4, 4);
    CHECK(lo == 0x0B, "tetra_bits_to_uint low nibble 0x0B");

    /* 24-bit value */
    pack_bits(bits, 0x123456, 0, 24);
    v = tetra_bits_to_uint(bits, 0, 24);
    CHECK(v == 0x123456u, "tetra_bits_to_uint 24-bit value");
}

/* -----------------------------------------------------------------------
 * Phase 83: D-FACILITY (PDU type 16)
 * ----------------------------------------------------------------------- */
static void test_d_facility_annex_e3(void)
{
    printf("[Phase 83] Annex E.3 D-FACILITY parser\n");

    dsd_state *st = alloc_state();
    dsd_opts  *op = alloc_opts();

    uint8_t cmce[40];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_FACILITY, 0, 5);
    pack_bits(cmce,  1, 5, 4);  /* one SS-PDU */
    pack_bits(cmce, 12, 9, 11); /* twelve content bits */
    pack_bits(cmce,  7, 20, 6); /* SS-Type */
    pack_bits(cmce,  3, 26, 5); /* SS-PDU type */
    pack_bits(cmce,  0, 31, 1); /* SS-PDU optional elements absent */
    pack_bits(cmce,  0, 32, 1); /* D-FACILITY optional elements absent */

    uint8_t mle[48];
    int mle_nbits = 0;
    wrap_mle_cmce(cmce, 33, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 1, op, st);

    CHECK(st->tetra_facility_valid == 1, "D-FACILITY valid flag set");
    CHECK(st->tetra_facility_type == 7, "D-FACILITY first SS-Type parsed");

    st->tetra_facility_valid = 0;
    wrap_mle_cmce(cmce, 25, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 1, op, st);
    CHECK(st->tetra_facility_valid == 0, "truncated D-FACILITY rejected");

    free(st);
    free(op);
}

/* ETSI EN 300 392-2 V3.8.1, Annex E.1.2 table E.2. The contents are
 * independent SS-PDUs: each length covers its own complete encoding and only
 * then does the outer D-FACILITY O-bit follow. */
static void test_d_facility_annex_e2_two_ss_pdus(void)
{
    printf("[Phase 83] Annex E.2 D-FACILITY with two SS-PDUs\n");

    dsd_state *st = alloc_state();
    dsd_opts  *op = alloc_opts();
    uint8_t cmce[80];
    uint8_t mle[88];
    int mle_nbits = 0;
    memset(cmce, 0, sizeof(cmce));

    pack_bits(cmce, TETRA_CMCE_D_FACILITY, 0, 5);
    pack_bits(cmce, 2, 5, 4);    /* Annex E.2: two independent SS-PDUs */
    pack_bits(cmce, 12, 9, 11);
    pack_bits(cmce, 7, 20, 6);   /* first SS-Type */
    pack_bits(cmce, 3, 26, 5);   /* first SS-PDU type */
    pack_bits(cmce, 0, 31, 1);   /* first SS-PDU O-bit */
    pack_bits(cmce, 12, 32, 11);
    pack_bits(cmce, 12, 43, 6);  /* second SS-Type */
    pack_bits(cmce, 5, 49, 5);   /* second SS-PDU type */
    pack_bits(cmce, 0, 54, 1);   /* second SS-PDU O-bit */
    pack_bits(cmce, 0, 55, 1);   /* outer D-FACILITY O-bit */

    wrap_mle_cmce(cmce, 56, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 2, op, st);
    CHECK(st->tetra_facility_valid == 1, "Annex E.2 two-SS-PDU collection accepted");
    CHECK(st->tetra_facility_type == 7, "Annex E.2 first independent SS-Type retained");

    /* Stop inside the second independently length-delimited SS-PDU. State
     * publication must remain atomic rather than exposing the first member. */
    st->tetra_facility_type = 9;
    wrap_mle_cmce(cmce, 54, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 2, op, st);
    CHECK(st->tetra_facility_valid == 1, "truncated second SS-PDU preserves valid state");
    CHECK(st->tetra_facility_type == 9, "truncated second SS-PDU preserves previous SS-Type");

    /* Both SS-PDUs are complete, but Annex E requires the outer O-bit. */
    wrap_mle_cmce(cmce, 55, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 2, op, st);
    CHECK(st->tetra_facility_type == 9, "missing outer D-FACILITY O-bit preserves state");

    free(st);
    free(op);
}

/* ETSI EN 300 392-2 V3.8.1, Annex E.1.2 table E.8.  This is the
 * D-CONNECT example carrying a 42-bit SS-AL INVOKE1 ACK Facility as a
 * length-delimited type-3 element.  The SS-AL values come from
 * EN 300 392-9 (SS type 21) and EN 300 392-12-21 (INVOKE1 ACK type 11). */
static void test_d_connect_annex_e8_ss_al_facility(void)
{
    printf("[Phase 83] Annex E.8 D-CONNECT with SS-AL Facility\n");

    dsd_state *st = alloc_state();
    dsd_opts  *op = alloc_opts();
    uint8_t cmce[128];
    uint8_t mle[136];
    int mle_nbits = 0;
    int off = 0;
    memset(cmce, 0, sizeof(cmce));

    pack_bits(cmce, TETRA_CMCE_D_CONNECT, off, 5); off += 5;
    pack_bits(cmce, 0x1234, off, 14); off += 14; /* call identifier: any */
    pack_bits(cmce, 9, off, 4); off += 4;        /* call time-out: any */
    pack_bits(cmce, 0, off, 1); off++;           /* hook method */
    pack_bits(cmce, 0, off, 1); off++;           /* simplex */
    pack_bits(cmce, 1, off, 2); off += 2;        /* transmission present */
    pack_bits(cmce, 0, off, 1); off++;           /* request permission */
    pack_bits(cmce, 0, off, 1); off++;           /* call ownership */
    pack_bits(cmce, 1, off, 1); off++;           /* O-bit */
    pack_bits(cmce, 0, off, 1); off++;           /* no call priority */
    pack_bits(cmce, 0, off, 1); off++;           /* no basic service */
    pack_bits(cmce, 0, off, 1); off++;           /* no temporary address */
    pack_bits(cmce, 0, off, 1); off++;           /* no notification */
    pack_bits(cmce, 1, off, 1); off++;           /* M-bit: Facility follows */
    pack_bits(cmce, 3, off, 4); off += 4;        /* type-3 Facility identifier */
    pack_bits(cmce, 42, off, 11); off += 11;
    pack_bits(cmce, 21, off, 6); off += 6;       /* SS-AL */
    pack_bits(cmce, 11, off, 5); off += 5;       /* INVOKE1 ACK */
    pack_bits(cmce, 1, off, 2); off += 2;        /* affected party is SSI */
    pack_bits(cmce, 0x345678, off, 24); off += 24;
    pack_bits(cmce, 0, off, 4); off += 4;        /* invocation accepted */
    pack_bits(cmce, 0, off, 1); off++;           /* SS-PDU O-bit */
    pack_bits(cmce, 0, off, 1); off++;           /* D-CONNECT terminal M-bit */
    CHECK(off == 93, "Annex E.8 CMCE body is exactly 93 bits");

    wrap_mle_cmce(cmce, off, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 3, op, st);
    CHECK(st->tetra_connect_valid == 1, "Annex E.8 D-CONNECT accepted");
    CHECK(st->tetra_call_id == 0x1234 && st->tetra_call_timeout == 9,
          "Annex E.8 mandatory call fields published");
    CHECK(st->tetra_cmce_connect_tx_grant == 1,
          "Annex E.8 transmission grant published");
    CHECK(st->tetra_cmce_call_generation == 1,
          "Annex E.8 complete message advances generation");

    /* The 42-bit Facility is length-delimited.  Stopping inside it must not
     * replace the last complete call state. */
    pack_bits(cmce, 0x2222, 5, 14);
    wrap_mle_cmce(cmce, 91, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 3, op, st);
    CHECK(st->tetra_call_id == 0x1234 && st->tetra_cmce_call_generation == 1,
          "truncated Annex E.8 Facility preserves call state");

    /* The value is complete at bit 92, but Annex E still requires the
     * terminating outer M-bit at bit 93. */
    wrap_mle_cmce(cmce, 92, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 3, op, st);
    CHECK(st->tetra_call_id == 0x1234 && st->tetra_cmce_call_generation == 1,
          "missing Annex E.8 terminal M-bit preserves call state");

    free(st);
    free(op);
}

/* -----------------------------------------------------------------------
 * Phase 84: channel_info_fmt extensions
 * ----------------------------------------------------------------------- */
static void test_channel_info_release_cause(void)
{
    printf("[Phase 84] channel_info release cause\n");

    dsd_state *st = alloc_state();
    st->tetra_net_known = 1;
    st->tetra_mcc = 1;
    st->tetra_mnc = 1;
    st->tetra_colour = 1;
    st->tetra_cmce_release_cause_type = 1; /* flag that release cause is present */
    st->tetra_cmce_release_cause = 2;      /* pre-emption */

    char buf[512];
    tetra_channel_info_fmt(st, buf, sizeof(buf));
    CHECK(strstr(buf, "rel:pre-emption") != NULL, "channel_info shows release cause");

    free(st);
}

static void test_channel_info_access_common(void)
{
    printf("[Phase 84] channel_info access common flag\n");

    dsd_state *st = alloc_state();
    st->tetra_net_known = 1;
    st->tetra_mcc = 1;
    st->tetra_mnc = 1;
    st->tetra_colour = 1;
    st->tetra_access_common_flag = 1;

    char buf[512];
    tetra_channel_info_fmt(st, buf, sizeof(buf));
    CHECK(strstr(buf, "acc:common") != NULL, "channel_info shows access common");

    free(st);
}

static void test_channel_info_mm_addr_type(void)
{
    printf("[Phase 84] channel_info MM addr type\n");

    dsd_state *st = alloc_state();
    st->tetra_net_known = 1;
    st->tetra_mcc = 1;
    st->tetra_mnc = 1;
    st->tetra_colour = 1;
    st->tetra_mm_addr_type = 3; /* SMI */

    char buf[512];
    tetra_channel_info_fmt(st, buf, sizeof(buf));
    CHECK(strstr(buf, "mm_addr:SMI") != NULL, "channel_info shows mm_addr:SMI");

    free(st);
}

/* -----------------------------------------------------------------------
 * CMCE constant sanity checks
 * ----------------------------------------------------------------------- */
static void test_cmce_constants(void)
{
    printf("[Phase 83] ETSI table 14.66 constants\n");
    CHECK(TETRA_CMCE_D_ALERT == 0, "D-ALERT type");
    CHECK(TETRA_CMCE_D_INFO == 5, "D-INFO type");
    CHECK(TETRA_CMCE_D_SETUP == 7, "D-SETUP type");
    CHECK(TETRA_CMCE_D_TX_GRANTED == 11, "D-TX-GRANTED type");
    CHECK(TETRA_CMCE_D_CALL_RESTORE == 14, "D-CALL-RESTORE type");
    CHECK(TETRA_CMCE_D_SDS_DATA == 15, "D-SDS-DATA type");
    CHECK(TETRA_CMCE_D_FACILITY == 16, "D-FACILITY type");
    CHECK(TETRA_CMCE_FUNCTION_NOT_SUPPORTED == 31, "FUNCTION-NOT-SUPPORTED type");
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("=== TETRA Phase 85 Tests ===\n\n");

    /* Phase 81 */
    test_tetra_bits_to_uint();

    /* Phase 82 */

    /* Phase 83 */
    test_cmce_constants();
    test_d_facility_annex_e3();
    test_d_facility_annex_e2_two_ss_pdus();
    test_d_connect_annex_e8_ss_al_facility();

    /* Phase 84 */
    test_channel_info_release_cause();
    test_channel_info_access_common();
    test_channel_info_mm_addr_type();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
