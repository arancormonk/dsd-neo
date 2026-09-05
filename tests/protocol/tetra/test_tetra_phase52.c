// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 52 test suite.
 *
 * Covers functionality introduced in Phases 48-51:
 *
 * Phase 48 — MM remaining parsers:
 *   1.  D-PARAMETER-CHANGE with LA sets param_change_valid and la
 *   2.  D-PARAMETER-CHANGE without LA still sets valid=1
 *   3.  D-ITSI-DETACH-ACK sets detach_ack=1
 *   4.  D-LU-DEMAND with cause=3 sets lu_demand_cause and valid
 *   5.  D-LU-DEMAND too-short still sets valid=1 (cause=0)
 *
 * Phase 49 — D-CONNECT full parse:
 *   6.  D-CONNECT extracts the mandatory fields from ETSI table 14.7
 *   7.  D-CONNECT sets tetra_call_active=1
 *   8.  D-CONNECT records transmission grant and ownership
 *   9.  D-CONNECT rejects a too-short PDU
 *
 * Phase 50 — D-SDS-SHORT-DATA:
 *  10.  D-SDS-SHORT-DATA type 0 extracts 16-bit status
 *  11.  D-SDS-SHORT-DATA sets sds_short_src and sds_short_valid
 *  12.  D-SDS-SHORT-DATA too-short (no crash)
 *
 * Phase 51 — MAC-SUPPL → MLE dispatch:
 *  13.  state.h new fields zero-initialise correctly
 *  14.  tetra_mm_param_change_valid/la zero after calloc
 *  15.  tetra_connect_valid zero after calloc
 *  16.  tetra_sds_short_valid zero after calloc
 */

#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/core/state.h>

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

/* MLE wrappers */
static void wrap_mle_cmce(const uint8_t *cmce_body, int cmce_nbits,
                           uint8_t *out, int *out_nbits)
{
    memset(out, 0, (size_t)(9 + cmce_nbits));
    pack_bits(out, 24, 0, 5);   /* mle_type = TETRA_MLE_C_PLANE_DATA */
    pack_bits(out,  3, 5, 4);   /* pd       = TETRA_MLE_PD_CMCE       */
    memcpy(out + 9, cmce_body, (size_t)cmce_nbits);
    *out_nbits = 9 + cmce_nbits;
}

static void wrap_mle_mm(const uint8_t *mm_body, int mm_nbits,
                         uint8_t *out, int *out_nbits)
{
    memset(out, 0, (size_t)(9 + mm_nbits));
    pack_bits(out, 24, 0, 5);   /* mle_type = TETRA_MLE_C_PLANE_DATA */
    pack_bits(out,  5, 5, 4);   /* pd       = TETRA_MLE_PD_MM (5)     */
    memcpy(out + 9, mm_body, (size_t)mm_nbits);
    *out_nbits = 9 + mm_nbits;
}

/* =======================================================================
 * Phase 48: MM remaining parsers
 * ======================================================================= */

/* Test 1: D-PARAMETER-CHANGE with LA present */
static void test_mm_d_parameter_change_with_la(void)
{
    printf("[test_mm_d_parameter_change_with_la]\n");

    /* MM D-PARAMETER-CHANGE:
     *   [0-4]  pdu_type = 11
     *   [5]    la_present = 1
     *   [6-19] LA = 2345 (14 bits)
     * Total: 20 bits */
    uint8_t mm[20];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 11u,   0,  5);
    pack_bits(mm,  1u,   5,  1);
    pack_bits(mm, 2345u, 6, 14);

    uint8_t mle[9 + 20]; int n;
    wrap_mle_mm(mm, 20, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_mm_param_change_valid == 1,     "D-PARAM-CHANGE: valid=1");
    CHECK(st->tetra_mm_param_change_la    == 2345u, "D-PARAM-CHANGE: la=2345");

    free(st); free(opt);
}

/* Test 2: D-PARAMETER-CHANGE without LA */
static void test_mm_d_parameter_change_no_la(void)
{
    printf("[test_mm_d_parameter_change_no_la]\n");

    uint8_t mm[6];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 11u, 0, 5);
    pack_bits(mm,  0u, 5, 1);  /* la_present = 0 */

    uint8_t mle[9 + 6]; int n;
    wrap_mle_mm(mm, 6, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_mm_param_change_valid == 1, "D-PARAM-CHANGE no-LA: valid=1");
    CHECK(st->tetra_mm_param_change_la    == 0, "D-PARAM-CHANGE no-LA: la=0");

    free(st); free(opt);
}

/* Test 3: D-ITSI-DETACH-ACK */
static void test_mm_d_itsi_detach_ack(void)
{
    printf("[test_mm_d_itsi_detach_ack]\n");

    uint8_t mm[5];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 12u, 0, 5);  /* pdu_type = 12 */

    uint8_t mle[9 + 5]; int n;
    wrap_mle_mm(mm, 5, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_mm_detach_ack == 1, "D-ITSI-DETACH-ACK: detach_ack=1");

    free(st); free(opt);
}

/* Test 4: D-LU-DEMAND with cause=3 */
static void test_mm_d_lu_demand_cause(void)
{
    printf("[test_mm_d_lu_demand_cause]\n");

    uint8_t mm[8];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 13u, 0, 5);  /* pdu_type = 13 */
    pack_bits(mm,  3u, 5, 3);  /* cause = 3     */

    uint8_t mle[9 + 8]; int n;
    wrap_mle_mm(mm, 8, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_mm_lu_demand_valid == 1, "D-LU-DEMAND: valid=1");
    CHECK(st->tetra_mm_lu_demand_cause == 3, "D-LU-DEMAND: cause=3");

    free(st); free(opt);
}

/* Test 5: D-LU-DEMAND too short still sets valid */
static void test_mm_d_lu_demand_short(void)
{
    printf("[test_mm_d_lu_demand_short]\n");

    uint8_t mm[5];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 13u, 0, 5);

    uint8_t mle[9 + 5]; int n;
    wrap_mle_mm(mm, 5, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_mm_lu_demand_valid == 1, "D-LU-DEMAND short: valid=1");
    CHECK(st->tetra_mm_lu_demand_cause == 0, "D-LU-DEMAND short: cause=0");

    free(st); free(opt);
}

/* =======================================================================
 * Phase 49: D-CONNECT full parse
 * ======================================================================= */

/* Test 6: D-CONNECT mandatory fields, ETSI EN 300 392-2 table 14.7. */
static void test_d_connect_parse(void)
{
    printf("[test_d_connect_parse]\n");

    uint8_t cmce[29];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u,      0, 5);
    pack_bits(cmce, 0x2345u, 5, 14);
    pack_bits(cmce, 9u,     19, 4);
    pack_bits(cmce, 1u,     23, 1);
    pack_bits(cmce, 1u,     24, 1);
    pack_bits(cmce, 3u,     25, 2);
    pack_bits(cmce, 1u,     27, 1);
    pack_bits(cmce, 1u,     28, 1);

    uint8_t mle[9 + 29]; int n;
    wrap_mle_cmce(cmce, 29, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_connect_valid == 1, "D-CONNECT: valid=1");
    CHECK(st->tetra_call_id == 0x2345, "D-CONNECT: call identifier");
    CHECK(st->tetra_call_timeout == 9, "D-CONNECT: call timeout");
    CHECK(st->tetra_cmce_connect_hook == 1, "D-CONNECT: hook method");
    CHECK(st->tetra_cmce_connect_duplex == 1, "D-CONNECT: simplex/duplex");

    free(st); free(opt);
}

/* Test 7: D-CONNECT sets call_active */
static void test_d_connect_call_active(void)
{
    printf("[test_d_connect_call_active]\n");

    uint8_t cmce[29];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);

    uint8_t mle[9 + 29]; int n;
    wrap_mle_cmce(cmce, 29, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_call_active == 1, "D-CONNECT: call_active=1");

    free(st); free(opt);
}

/* Test 8: D-CONNECT transmission control fields. */
static void test_d_connect_transmission_control(void)
{
    printf("[test_d_connect_transmission_control]\n");

    uint8_t cmce[29];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);
    pack_bits(cmce, 3u, 25, 2);
    pack_bits(cmce, 1u, 27, 1);
    pack_bits(cmce, 1u, 28, 1);

    uint8_t mle[9 + 29]; int n;
    wrap_mle_cmce(cmce, 29, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_cmce_connect_tx_grant == 3, "D-CONNECT: transmission grant");
    CHECK(st->tetra_cmce_connect_tx_permission == 1, "D-CONNECT: request permission");
    CHECK(st->tetra_cmce_connect_ownership == 1, "D-CONNECT: ownership");

    free(st); free(opt);
}

/* Test 9: D-CONNECT too-short (only the PDU type). */
static void test_d_connect_too_short(void)
{
    printf("[test_d_connect_too_short]\n");

    uint8_t cmce[5];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);

    uint8_t mle[9 + 5]; int n;
    wrap_mle_cmce(cmce, 5, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_call_active == 0, "D-CONNECT short: call stays inactive");
    CHECK(st->tetra_connect_valid == 0, "D-CONNECT short: remains invalid");

    free(st); free(opt);
}

/* =======================================================================
 * Phase 50: D-SDS-DATA SDTI=0
 * ======================================================================= */

/* Test 10: D-SDS-DATA SDTI=0 type 0 with 16-bit status */
static void test_d_sds_short_data_type0(void)
{
    printf("[test_d_sds_short_data_type0]\n");

    /* CMCE D-SDS-DATA SDTI=0:
     *   [0-4]   pdu_type = 15
     *   [5]     ext_flag = 0
     *   [6-29]  calling_ssi = 99999 (24 bits)
     *   [30-31] data_type = 0
     *   [32-47] pre-defined status = 0xABCD (16 bits)
     * Total: 48 bits */
    uint8_t cmce[49];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA,     0,  5);
    pack_bits(cmce,  1u,     5,  2);
    pack_bits(cmce, 99999u,  7, 24);
    pack_bits(cmce,  0u,    31,  2);  /* data_type = 0 */
    pack_bits(cmce, 0xABCDu, 33, 16); /* status */

    uint8_t mle[9 + 49]; int n;
    wrap_mle_cmce(cmce, 49, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_sds_short_valid == 1,       "D-SDS-SHORT: valid=1");
    CHECK(st->tetra_sds_short_data  == 0xABCDu, "D-SDS-SHORT: data=0xABCD");

    free(st); free(opt);
}

/* Test 11: D-SDS-DATA SDTI=0 sets sds_short_src */
static void test_d_sds_short_data_src(void)
{
    printf("[test_d_sds_short_data_src]\n");

    uint8_t cmce[49];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA,    0,  5);
    pack_bits(cmce,  1u,    5,  2);
    pack_bits(cmce, 55555u, 7, 24);
    pack_bits(cmce,  0u,   31,  2);
    pack_bits(cmce,  0u,   33, 16);

    uint8_t mle[9 + 49]; int n;
    wrap_mle_cmce(cmce, 49, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_sds_short_src   == 55555u, "D-SDS-SHORT: src=55555");
    CHECK(st->tetra_sds_short_valid == 1,      "D-SDS-SHORT: valid=1");
    /* Also sets the shared sds_src field */
    CHECK(st->tetra_sds_src         == 55555u, "D-SDS-SHORT: sds_src=55555");

    free(st); free(opt);
}

/* Test 12: D-SDS-DATA SDTI=0 too short (no crash) */
static void test_d_sds_short_data_too_short(void)
{
    printf("[test_d_sds_short_data_too_short]\n");

    uint8_t cmce[10];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);

    uint8_t mle[9 + 10]; int n;
    wrap_mle_cmce(cmce, 10, mle, &n);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* Must not crash */
    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(st->tetra_sds_short_valid == 0, "D-SDS-SHORT short: valid stays 0");

    free(st); free(opt);
}

/* =======================================================================
 * Phase 51: zero-initialisation of new fields
 * ======================================================================= */

static void test_new_fields_zero(void)
{
    printf("[test_new_fields_zero]\n");
    dsd_state *st = alloc_state();

    /* Phase 48 fields */
    CHECK(st->tetra_mm_param_change_valid == 0, "param_change_valid zero");
    CHECK(st->tetra_mm_param_change_la    == 0, "param_change_la zero");
    CHECK(st->tetra_mm_detach_ack         == 0, "detach_ack zero");
    CHECK(st->tetra_mm_lu_demand_valid    == 0, "lu_demand_valid zero");
    CHECK(st->tetra_mm_lu_demand_cause    == 0, "lu_demand_cause zero");

    /* Phase 49 fields */
    CHECK(st->tetra_connect_valid         == 0, "connect_valid zero");
    CHECK(st->tetra_connect_enc_mode      == 0, "connect_enc_mode zero");
    CHECK(st->tetra_connect_call_type     == 0, "connect_call_type zero");

    /* Phase 50 fields */
    CHECK(st->tetra_sds_short_valid       == 0, "sds_short_valid zero");
    CHECK(st->tetra_sds_short_data        == 0, "sds_short_data zero");
    CHECK(st->tetra_sds_short_src         == 0, "sds_short_src zero");

    free(st);
}

/* =======================================================================
 * main
 * ======================================================================= */
int main(void)
{
    printf("=== TETRA Phase 52 Test Suite ===\n\n");

    /* Phase 48 */
    printf("--- Phase 48: MM remaining parsers ---\n");
    test_mm_d_parameter_change_with_la();
    test_mm_d_parameter_change_no_la();
    test_mm_d_itsi_detach_ack();
    test_mm_d_lu_demand_cause();
    test_mm_d_lu_demand_short();

    /* Phase 49 */
    printf("\n--- Phase 49: D-CONNECT full parse ---\n");
    test_d_connect_parse();
    test_d_connect_call_active();
    test_d_connect_transmission_control();
    test_d_connect_too_short();

    /* Phase 50 */
    printf("\n--- Phase 50: D-SDS-SHORT-DATA ---\n");
    test_d_sds_short_data_type0();
    test_d_sds_short_data_src();
    test_d_sds_short_data_too_short();

    /* Phase 51 */
    printf("\n--- Phase 51: new fields zero-init ---\n");
    test_new_fields_zero();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
