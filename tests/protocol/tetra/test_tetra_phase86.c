// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 86 test suite.
 *
 * Covers all 5 improvement items from the integration analysis:
 *
 * Item 1: D-INFO IE expansion (call_timeout, notification)
 * Item 2: D-TX-* stub expansion (call_id, notification parsed)
 * Item 4: SNDCP minimal parser
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Wrap a CMCE PDU body inside an protocol-discriminator envelope (PD=CMCE). */
static void
wrap_mle_cmce(const uint8_t* cmce_body, int cmce_nbits, uint8_t* out, int* out_nbits) {
    int total = 3 + cmce_nbits;
    DSD_MEMSET(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = total;
}

/* Wrap a generic PDU inside an protocol-discriminator envelope with given PD. */
static void
wrap_mle_pd(const uint8_t* body, int body_nbits, uint32_t pd, uint8_t* out, int* out_nbits) {
    int total = 3 + body_nbits;
    DSD_MEMSET(out, 0, (size_t)total);
    pack_bits(out, pd, 0, 3);
    DSD_MEMCPY(out + 3, body, (size_t)body_nbits);
    *out_nbits = total;
}

/* ====================================================================
 * Item 1: D-INFO IE expansion
 * ==================================================================== */
/* ====================================================================
 * Item 2: D-TX-* stub expansion
 * ==================================================================== */
static void
test_d_tx_expanded(void) {
    printf("--- Item 2: D-TX-* stub expansion ---\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* Table 14.17: call id, continue, request permission, notification. */
    uint8_t cmce[32];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_CONTINUE, 0, 5);
    pack_bits(cmce, 0x2345, 5, 14);
    pack_bits(cmce, 1, 19, 1); /* continue */
    pack_bits(cmce, 1, 20, 1); /* request permission */
    pack_bits(cmce, 1, 21, 1); /* O-bit */
    pack_bits(cmce, 1, 22, 1); /* notification present */
    pack_bits(cmce, 7, 23, 6); /* notification value */
    pack_bits(cmce, 0, 29, 1); /* terminating M-bit */

    uint8_t mle[64];
    char log[256];
    int mle_nbits;
    wrap_mle_cmce(cmce, 30, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 7, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-TX-CONTINUE] CC=7 call=9029 request=1 notif=7 continue=1") != NULL,
          "D-TX-CONTINUE call id, notification and continue value");

    /* D-TX-WAIT: call_id=0, notification=0 */
    dsd_state_ext_free_all(st);
    DSD_MEMSET(st, 0, sizeof(*st));
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, 0, 5);
    pack_bits(cmce, 0, 5, 14);
    pack_bits(cmce, 0, 19, 1);
    pack_bits(cmce, 0, 20, 1); /* O-bit */

    wrap_mle_cmce(cmce, 21, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 7, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-TX-WAIT] CC=7 call=0 request=0 notif=0") != NULL,
          "D-TX-WAIT call id 0 without notification");

    /* Table 14.19 D-TX-INTERRUPT. */
    dsd_state_ext_free_all(st);
    DSD_MEMSET(st, 0, sizeof(*st));
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_INTERRUPT, 0, 5);
    pack_bits(cmce, 0x123, 5, 14);
    pack_bits(cmce, 1, 19, 2); /* transmission not granted */
    pack_bits(cmce, 0, 21, 1); /* request permission */
    pack_bits(cmce, 1, 22, 1); /* encryption */
    pack_bits(cmce, 0, 24, 1); /* O-bit */

    wrap_mle_cmce(cmce, 25, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 7, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-TX-INTERRUPT] CC=7 call=291 request=0 notif=0 continue=0 grant=1 enc=1") != NULL,
          "D-TX-INTERRUPT call id and encryption");

    free_state(st);
    free(opt);
}

/* ====================================================================
 * Item 4: SNDCP parser (EN 300 392-2 clause 28.4.4): the SN PDU type comes
 * first (table 28.121); the NSAPI follows only in the PDU types that carry one.
 * ==================================================================== */
static void
test_sndcp_parser(void) {
    printf("--- Item 4: SNDCP parser ---\n");
    /* SN-UNITDATA (table 28.43): SN PDU type 4, NSAPI 5, then PCOMP/DCOMP. */
    uint8_t sndcp[16];
    DSD_MEMSET(sndcp, 0, sizeof(sndcp));
    pack_bits(sndcp, 4, 0, 4); /* SN PDU type = SN-UNITDATA */
    pack_bits(sndcp, 5, 4, 4); /* NSAPI = 5 */

    /* Prefix the SNDCP PDU with the standard 3-bit SNDCP PD. */
    uint8_t mle[64];
    int mle_nbits;
    wrap_mle_pd(sndcp, 16, TETRA_MLE_PD_SNDCP, mle, &mle_nbits);
    tetra_sndcp_pdu pdu;
    CHECK(tetra_sndcp_decode(mle, mle_nbits, &pdu) == 1, "SNDCP header decodes");
    CHECK(pdu.nsapi == 5, "SNDCP NSAPI = 5");
    CHECK(pdu.pdu_type == 4, "SNDCP PDU type = SN-UNITDATA");
    CHECK(pdu.nbits == 16, "SNDCP nbits = 16");

    /* SN-END OF DATA (table 28.34) carries no NSAPI. */
    DSD_MEMSET(sndcp, 0, sizeof(sndcp));
    pack_bits(sndcp, 8, 0, 4);
    pack_bits(sndcp, 1, 4, 1); /* immediate service change */
    wrap_mle_pd(sndcp, 5, TETRA_MLE_PD_SNDCP, mle, &mle_nbits);
    CHECK(tetra_sndcp_decode(mle, mle_nbits, &pdu) && pdu.pdu_type == 8 && pdu.nsapi == 0,
          "SN-END OF DATA has no NSAPI");

    /* Test short SNDCP: an SN-ACTIVATE PDP CONTEXT ACCEPT cut before its NSAPI */
    uint8_t sndcp_short[4];
    DSD_MEMSET(sndcp_short, 0, sizeof(sndcp_short));
    wrap_mle_pd(sndcp_short, 4, TETRA_MLE_PD_SNDCP, mle, &mle_nbits);
    CHECK(!tetra_sndcp_decode(mle, mle_nbits, &pdu), "SNDCP short: rejected");
}

/* ==================================================================== */
int
main(void) {
    printf("=== TETRA Phase 86 Tests ===\n");

    test_d_tx_expanded();
    test_sndcp_parser();

    printf("\n=== Phase 86 Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
