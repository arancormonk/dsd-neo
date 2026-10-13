// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 52 test suite.
 *
 * Covers functionality introduced in Phases 48-51:
 *
 * Phase 49 — D-CONNECT full parse:
 *   6.  D-CONNECT extracts the mandatory fields from ETSI table 14.7
 *   7.  D-CONNECT connects the call it names, and only a call a record holds
 *   8.  D-CONNECT records transmission grant and ownership
 *   9.  D-CONNECT rejects a too-short PDU
 *
 * Phase 50 — D-SDS-DATA SDTI=0:
 *  10.  SDTI=0 extracts 16-bit user data
 *  11.  SDTI=0 sets sds_short_src and sds_short_valid
 *  12.  Too-short D-SDS-DATA has no side effects
 *
 * Phase 51 — MAC-SUPPL → MLE dispatch:
 *  13.  state.h new fields zero-initialise correctly
 *  16.  tetra_sds_short_valid zero after calloc
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "tetra_calls.h"
#include "tetra_state.h"

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

static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

/* Call @p call_id, set up by a D-SETUP (table 14.15) to a group. */
static void
setup_call(dsd_state* st, dsd_opts* opt, uint16_t call_id) {
    uint8_t mle[44];
    DSD_MEMSET(mle, 0, sizeof mle);
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(mle, TETRA_CMCE_D_SETUP, 3, 5);
    pack_bits(mle, call_id, 8, 14);
    pack_bits(mle, 0x04u, 28, 8); /* basic service: clear speech, point to multipoint */
    tetra_mle_dispatch(mle, 44, 0, opt, st);
}

/* MLE wrappers */
static void
wrap_mle_cmce(const uint8_t* cmce_body, int cmce_nbits, uint8_t* out, int* out_nbits) {
    DSD_MEMSET(out, 0, (size_t)(3 + cmce_nbits));
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = 3 + cmce_nbits;
}

/* =======================================================================
 * Phase 49: D-CONNECT full parse
 * ======================================================================= */

/* Test 6: D-CONNECT mandatory fields, ETSI EN 300 392-2 table 14.7. */
static void
test_d_connect_parse(void) {
    printf("[test_d_connect_parse]\n");

    uint8_t cmce[30];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);
    pack_bits(cmce, 0x2345u, 5, 14);
    pack_bits(cmce, 9u, 19, 4);
    pack_bits(cmce, 1u, 23, 1);
    pack_bits(cmce, 1u, 24, 1);
    pack_bits(cmce, 3u, 25, 2);
    pack_bits(cmce, 1u, 27, 1);
    pack_bits(cmce, 1u, 28, 1);
    pack_bits(cmce, 0u, 29, 1); /* O-bit */

    uint8_t mle[3 + 30];
    int n;
    wrap_mle_cmce(cmce, 30, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    setup_call(st, opt, 0x2345);
    tetra_mle_dispatch(mle, n, 0, opt, st);

    const tetra_call_t* const c = tetra_calls_peek(st, 0x2345);
    CHECK(c && c->phase == TETRA_CALL_CONNECTED, "D-CONNECT: the call it names is connected");
    CHECK(c && c->timeout == 9, "D-CONNECT: call timeout");
    CHECK(tetra_st(st)->cmce_connect_hook == 1, "D-CONNECT: hook method");
    CHECK(tetra_st(st)->cmce_connect_duplex == 1, "D-CONNECT: simplex/duplex");

    free_state(st);
    free(opt);
}

/* Test 7: D-CONNECT connects the call it names, and makes no record for a call no D-SETUP was heard for. */
static void
test_d_connect_call_active(void) {
    printf("[test_d_connect_call_active]\n");

    uint8_t cmce[30];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);

    uint8_t mle[3 + 30];
    int n;
    wrap_mle_cmce(cmce, 30, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);
    CHECK(tetra_calls_peek(st, 0) == NULL, "D-CONNECT of an unknown call: no record");
    setup_call(st, opt, 0);
    tetra_mle_dispatch(mle, n, 0, opt, st);
    CHECK(tetra_calls_peek(st, 0) && tetra_calls_peek(st, 0)->phase == TETRA_CALL_CONNECTED,
          "D-CONNECT: the known call is connected");

    free_state(st);
    free(opt);
}

/* Test 8: D-CONNECT transmission control fields. */
static void
test_d_connect_transmission_control(void) {
    printf("[test_d_connect_transmission_control]\n");

    uint8_t cmce[30];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);
    pack_bits(cmce, 3u, 25, 2);
    pack_bits(cmce, 1u, 27, 1);
    pack_bits(cmce, 1u, 28, 1);
    pack_bits(cmce, 0u, 29, 1); /* O-bit */

    uint8_t mle[3 + 30];
    int n;
    wrap_mle_cmce(cmce, 30, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(tetra_st(st)->cmce_connect_tx_grant == 3, "D-CONNECT: transmission grant");
    CHECK(tetra_st(st)->cmce_connect_tx_permission == 1, "D-CONNECT: request permission");
    CHECK(tetra_st(st)->cmce_connect_ownership == 1, "D-CONNECT: ownership");

    free_state(st);
    free(opt);
}

/* Test 9: D-CONNECT too-short (only the PDU type). */
static void
test_d_connect_too_short(void) {
    printf("[test_d_connect_too_short]\n");

    uint8_t cmce[5];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);

    uint8_t mle[9 + 5];
    int n;
    wrap_mle_cmce(cmce, 5, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(tetra_calls_peek(st, 0) == NULL, "D-CONNECT short: no call");
    CHECK(tetra_st(st)->cmce_connect_tx_grant == 0 && tetra_st(st)->cmce_connect_hook == 0,
          "D-CONNECT short: nothing published");

    free_state(st);
    free(opt);
}

/* =======================================================================
 * Phase 50: D-SDS-DATA SDTI=0
 * ======================================================================= */

/* Test 10: D-SDS-DATA SDTI=0 type 0 with 16-bit status */
static void
test_d_sds_data_sdti0_payload(void) {
    printf("[test_d_sds_data_sdti0_payload]\n");

    /* CMCE D-SDS-DATA SDTI=0:
     *   [0-4]   pdu_type = 15
     *   [5]     ext_flag = 0
     *   [6-29]  calling_ssi = 99999 (24 bits)
     *   [30-31] data_type = 0
     *   [32-47] pre-defined status = 0xABCD (16 bits)
     * Total: 48 bits */
    uint8_t cmce[50];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1u, 5, 2);
    pack_bits(cmce, 99999u, 7, 24);
    pack_bits(cmce, 0u, 31, 2);       /* data_type = 0 */
    pack_bits(cmce, 0xABCDu, 33, 16); /* status */
    pack_bits(cmce, 0u, 49, 1);       /* O-bit */

    uint8_t mle[3 + 50];
    int n;
    wrap_mle_cmce(cmce, 50, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(tetra_st(st)->sds_short_valid == 1, "D-SDS-DATA SDTI=0: valid=1");
    CHECK(tetra_st(st)->sds_short_data == 0xABCDu, "D-SDS-DATA SDTI=0: data=0xABCD");

    free_state(st);
    free(opt);
}

/* Test 11: D-SDS-DATA SDTI=0 sets sds_short_src */
static void
test_d_sds_data_sdti0_src(void) {
    printf("[test_d_sds_data_sdti0_src]\n");

    uint8_t cmce[50];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1u, 5, 2);
    pack_bits(cmce, 55555u, 7, 24);
    pack_bits(cmce, 0u, 31, 2);
    pack_bits(cmce, 0u, 33, 16);
    pack_bits(cmce, 0u, 49, 1); /* O-bit */

    uint8_t mle[3 + 50];
    int n;
    wrap_mle_cmce(cmce, 50, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(tetra_st(st)->sds_short_src == 55555u, "D-SDS-DATA SDTI=0: src=55555");
    CHECK(tetra_st(st)->sds_short_valid == 1, "D-SDS-DATA SDTI=0: valid=1");
    /* Also sets the shared sds_src field */
    CHECK(tetra_st(st)->sds_src == 55555u, "D-SDS-DATA SDTI=0: shared src=55555");

    free_state(st);
    free(opt);
}

/* Test 12: D-SDS-DATA SDTI=0 too short (no crash) */
static void
test_d_sds_data_sdti0_too_short(void) {
    printf("[test_d_sds_data_sdti0_too_short]\n");

    uint8_t cmce[10];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);

    uint8_t mle[9 + 10];
    int n;
    wrap_mle_cmce(cmce, 10, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* Must not crash */
    tetra_mle_dispatch(mle, n, 0, opt, st);

    CHECK(tetra_st(st)->sds_short_valid == 0, "truncated D-SDS-DATA stays invalid");

    free_state(st);
    free(opt);
}

/* =======================================================================
 * Phase 51: zero-initialisation of new fields
 * ======================================================================= */

static void
test_new_fields_zero(void) {
    printf("[test_new_fields_zero]\n");
    dsd_state* st = alloc_state();

    /* Phase 50 fields */
    CHECK(tetra_st(st)->sds_short_valid == 0, "sds_short_valid zero");
    CHECK(tetra_st(st)->sds_short_data == 0, "sds_short_data zero");
    CHECK(tetra_st(st)->sds_short_src == 0, "sds_short_src zero");

    free_state(st);
}

/* =======================================================================
 * main
 * ======================================================================= */
int
main(void) {
    printf("=== TETRA Phase 52 Test Suite ===\n\n");

    /* Phase 49 */
    printf("\n--- Phase 49: D-CONNECT full parse ---\n");
    test_d_connect_parse();
    test_d_connect_call_active();
    test_d_connect_transmission_control();
    test_d_connect_too_short();

    /* Phase 50 */
    printf("\n--- Phase 50: D-SDS-DATA SDTI=0 ---\n");
    test_d_sds_data_sdti0_payload();
    test_d_sds_data_sdti0_src();
    test_d_sds_data_sdti0_too_short();

    /* Phase 51 */
    printf("\n--- Phase 51: new fields zero-init ---\n");
    test_new_fields_zero();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
