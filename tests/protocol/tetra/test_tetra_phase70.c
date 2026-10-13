// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 70 test suite.
 *
 * Covers functionality introduced in Phases 62-68:
 *
 * Phase 62 — tetra_sds_text_len promoted to uint16_t
 *   1. Verify sizeof(state->tetra_sds_text_len) == 2
 *
 * Phase 67 — CMCE D-TX-CEASED permission bits
 *   6. request permission read; the transmission ends
 *
 * Phase 68 — MLE D-RESTORE-ACK SDU / D-RESTORE-FAIL cause
 *   7. restore_ack_la extracted when nbits >= 19
 *   8. restore_response_result extracted
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>

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
    DSD_MEMSET(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = total;
}

/* =======================================================================
 * Phase 64: CMCE D-ALERT call_id
 * ======================================================================= */
/* =======================================================================
 * Phase 65: CMCE D-CALL-PROCEEDING call_id
 * ======================================================================= */
/* =======================================================================
 * Phase 66: CMCE D-CONNECT-ACK call_id
 * ======================================================================= */
/* =======================================================================
 * CMCE D-TX-CEASED standard mandatory layout
 * ======================================================================= */
static void
test_d_tx_ceased(void) {
    printf("[test_d_tx_ceased]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* PDU type, 14-bit call identifier, request permission. */
    uint8_t cmce[21];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 9, 0, 5); /* D-TX-CEASED */
    pack_bits(cmce, 0x1234, 5, 14);
    pack_bits(cmce, 1, 19, 1); /* requests remain permitted */
    pack_bits(cmce, 0, 20, 1); /* O-bit */

    /* Pre-condition: call 0x1234 has a transmission. */
    tetra_call_event setup = {.call_id = 0x1234, .kind = TETRA_CALL_EV_SETUP, .basic_service = 0x04};
    tetra_calls_on_event(opt, st, &setup, NULL);
    tetra_calls_find(st, 0x1234)->phase = TETRA_CALL_TX_GRANTED;

    uint8_t pdu[21 + 3];
    char log[256];
    int n;
    wrap_mle_cmce(cmce, 21, pdu, &n);
    tetra_test_mle_logged(pdu, n, 0, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-TX-CEASED] CC=0 call=4660 request=1") != NULL,
          "D-TX-CEASED request permission extracted");
    CHECK(tetra_calls_peek(st, 0x1234)->phase == TETRA_CALL_CEASED, "D-TX-CEASED ends the transmission");

    free_state(st);
    free(opt);
}

/* =======================================================================
 * Phase 68: MLE D-RESTORE-ACK SDU / D-RESTORE-FAIL cause
 * ======================================================================= */
static void
test_mle_restore_fields(void) {
    printf("[test_mle_restore_fields]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* D-RESTORE-ACK carrying CMCE D-CALL-RESTORE (table 18.8). */
    uint8_t mle_ack[31];
    DSD_MEMSET(mle_ack, 0, sizeof(mle_ack));
    pack_bits(mle_ack, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle_ack, TETRA_MLE_D_RESTORE_ACK, 3, 3);
    pack_bits(mle_ack, 0, 6, 1); /* mandatory MLE O-bit */
    pack_bits(mle_ack, TETRA_CMCE_D_CALL_RESTORE, 7, 5);
    pack_bits(mle_ack, 777, 12, 14);
    pack_bits(mle_ack, 2, 26, 2);
    pack_bits(mle_ack, 1, 28, 1);
    pack_bits(mle_ack, 1, 29, 1);
    pack_bits(mle_ack, 0, 30, 1); /* CMCE O-bit */
    tetra_mle_pdu pdu;
    CHECK(tetra_mle_decode(mle_ack, 31, &pdu) && pdu.restore_ack.call.call_id == 777 && pdu.restore_ack.call.grant == 2
              && pdu.restore_ack.call.permission == 1 && pdu.restore_ack.call.reset == 1,
          "D-RESTORE-ACK decodes its D-CALL RESTORE");
    char log[256];
    tetra_test_mle_logged(mle_ack, 31, 0, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA CMCE D-CALL-RESTORE] CC=0 call=777 grant=2 request=1 reset=1") != NULL,
          "D-RESTORE-ACK CMCE SDU parsed");

    uint8_t mle_res[9];
    DSD_MEMSET(mle_res, 0, sizeof(mle_res));
    pack_bits(mle_res, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle_res, TETRA_MLE_D_RESTORE_FAIL, 3, 3);
    pack_bits(mle_res, 1, 6, 2);
    pack_bits(mle_res, 0, 8, 1);
    CHECK(tetra_mle_decode(mle_res, 9, &pdu) && pdu.fail_cause == 1, "D-RESTORE-FAIL cause extracted");

    free_state(st);
    free(opt);
}

int
main(void) {
    printf("=== TETRA Phase 70 Test Suite ===\n\n");

    test_d_tx_ceased();
    test_mle_restore_fields();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
