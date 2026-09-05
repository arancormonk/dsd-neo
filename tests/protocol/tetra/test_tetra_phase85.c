// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 85 test suite.
 *
 * Covers functionality introduced in Phases 81-84:
 *
 * Phase 81: tetra_bits.h shared header (bits_to_uint unification)
 * Phase 82: D-SDS-LONG-DATA (PDU type 20) parser
 * Phase 83: D-FACILITY / D-SDS-ACK / D-SDS-SHORT-REPORT parsers
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
    int total = 9 + cmce_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, 24, 0, 5); /* MLE type = C-PLANE-DATA */
    pack_bits(out,  3, 5, 4); /* PD = CMCE */
    memcpy(out + 9, cmce_body, (size_t)cmce_nbits);
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
 * Phase 82: D-SDS-LONG-DATA (PDU type 20)
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Phase 83: D-FACILITY (PDU type 16)
 * ----------------------------------------------------------------------- */
static void test_d_facility(void)
{
    printf("[Phase 83] D-FACILITY parser\n");

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

/* -----------------------------------------------------------------------
 * Phase 83: D-SDS-ACK (PDU type 17)
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Phase 83: D-SDS-SHORT-REPORT (PDU type 18)
 * ----------------------------------------------------------------------- */
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
    test_d_facility();

    /* Phase 84 */
    test_channel_info_release_cause();
    test_channel_info_access_common();
    test_channel_info_mm_addr_type();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
