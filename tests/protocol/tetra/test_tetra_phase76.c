// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 76 test suite.
 *
 * Covers functionality introduced in Phases 71-75:
 *
 * Phase 71 — cmce_d_sds_data rejects truncated payload atomically
 *   1. Send truncated SDS-DATA and ensure the prior valid state is retained
 *
 * Phase 74 — MAC ACCESS-DEFINE exact state caching
 *   5. Parses MAC ACCESS-DEFINE and saves num_ra, frame_len_f, ts_ptr, min_pdu_pri
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_mac_pdu.h"
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

static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

static dsd_opts*
alloc_opts(void) {
    return (dsd_opts*)calloc(1, sizeof(dsd_opts));
}

/* Wrap a CMCE body in 3-bit CMCE protocol discriminator */
static void
wrap_mle_cmce(const uint8_t* cmce_body, int cmce_nbits, uint8_t* out, int* out_nbits) {
    int total = 3 + cmce_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = total;
}

/* =======================================================================
 * Phase 71: CMCE D-SDS-DATA truncation does not publish partial state
 * ======================================================================= */
static void
test_sds_data_truncation(void) {
    printf("[test_sds_data_truncation]\n");
    /* Truncated D-SDS-DATA payload after the source SSI. */
    uint8_t cmce[40];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 15, 0, 5);   /* D-SDS-DATA */
    pack_bits(cmce, 1, 5, 2);    /* CPTI: SSI */
    pack_bits(cmce, 111, 7, 24); /* calling_ssi */
    pack_bits(cmce, 0, 31, 2);   /* SDTI=0, but payload is absent */

    uint8_t pdu[40 + 9];
    int n;
    wrap_mle_cmce(cmce, 33, pdu, &n);
    tetra_sds_pdu sds;
    CHECK(!tetra_sds_decode(pdu, n, &sds), "D-SDS-DATA without its user data is incomplete");

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
    char log[256];
    tetra_test_mle_logged(pdu, n, 0, opt, st, log, sizeof log);
    CHECK(strstr(log, "D-SDS-DATA") == NULL, "Partial source SSI is not published");

    free_state(st);
    free(opt);
}

/* =======================================================================
 * Phase 74: MAC ACCESS-DEFINE Fields
 * ======================================================================= */
static void
test_mac_access_define(void) {
    printf("[test_mac_access_define]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* MAC-BROADCAST (2) -> ACCESS-DEFINE (1), EN 300 392-2 table 21.79 */
    uint8_t mac[50];
    memset(mac, 0, sizeof(mac));
    pack_bits(mac, 2, 0, 2);  /* PDU = BROADCAST */
    pack_bits(mac, 1, 2, 2);  /* BROADCAST type = ACCESS-DEFINE */
    pack_bits(mac, 1, 4, 1);  /* assigned control channel */
    pack_bits(mac, 2, 5, 2);  /* access code C */
    pack_bits(mac, 5, 7, 4);  /* imm = 5 */
    pack_bits(mac, 2, 11, 4); /* wait = 2 */
    pack_bits(mac, 9, 15, 4); /* num_ra = 9 */
    pack_bits(mac, 1, 19, 1); /* frame_len_f = 1 */
    pack_bits(mac, 4, 20, 4); /* ts_ptr = 4 */
    pack_bits(mac, 3, 24, 3); /* min_prio = 3 */
    pack_bits(mac, 0, 27, 2); /* no optional field */
    pack_bits(mac, 4, 29, 3); /* filler bits 100 */

    tetra_mac_broadcast bc;
    CHECK(tetra_mac_broadcast_decode(mac, 32, &bc) && bc.type == TETRA_MAC_BC_ACCESS_DEF
              && bc.access_define.nbits == 32,
          "ACCESS-DEFINE decodes");
    CHECK(bc.access_define.assigned == 1, "Extracted assigned control channel flag");
    CHECK(bc.access_define.access_code == 2, "Extracted access code");
    CHECK(bc.access_define.imm == 5, "Extracted immediate");
    CHECK(bc.access_define.wait_time == 2, "Extracted wait time");
    CHECK(bc.access_define.num_ra == 9, "Extracted num_ra");
    CHECK(bc.access_define.frame_length_factor == 1, "Extracted frame_len_f");
    CHECK(bc.access_define.timeslot_pointer == 4, "Extracted ts_ptr");
    CHECK(bc.access_define.min_priority == 3, "Extracted min_pdu_pri");

    char log[256];
    tetra_test_mac_logged(mac, 32, 0, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA ACCESS-DEFINE] CC=0  assigned  access_code=C  imm=5  wait=2  num_ra=9  frame_len_f=1  "
                      "ts_ptr=4  min_prio=3\n")
              != NULL,
          "ACCESS-DEFINE summary line");

    free_state(st);
    free(opt);
}

int
main(void) {
    printf("=== TETRA Phase 76 Test Suite ===\n\n");

    test_sds_data_truncation();
    test_mac_access_define();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
