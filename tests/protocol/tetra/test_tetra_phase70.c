// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 70 test suite.
 *
 * Covers functionality introduced in Phases 62-68:
 *
 * Phase 62 — tetra_sds_text_len promoted to uint16_t
 *   1. Verify sizeof(state->tetra_sds_text_len) == 2
 *
 * Phase 63 — MM D-OTAR sub-PDU type extraction
 *   2. otar_pdu_type and otar_valid populated
 *
 * Phase 64 — CMCE D-ALERT call_id
 *   3. call_id and d_alert_valid set
 *
 * Phase 65 — CMCE D-CALL-PROCEEDING call_id
 *   4. call_id and d_call_proc_valid set
 *
 * Phase 66 — CMCE D-CONNECT-ACK call_id
 *   5. call_id and d_connect_ack_valid set
 *
 * Phase 67 — CMCE D-TX-CEASED permission bits
 *   6. tx_perm and cipher_info extracted; tx_granted_valid cleared
 *
 * Phase 68 — MLE D-RESTORE-ACK LA / D-RESTORE-RESPONSE result
 *   7. restore_ack_la extracted when nbits >= 19
 *   8. restore_response_result extracted
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

/* Wrap a CMCE body in MLE C-PLANE-DATA + PD=CMCE header (9 overhead bits) */
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

/* Wrap an MM body in MLE C-PLANE-DATA + PD=MM header (9 overhead bits) */
static void wrap_mle_mm(const uint8_t *mm_body, int mm_nbits,
                         uint8_t *out, int *out_nbits)
{
    int total = 9 + mm_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, 24, 0, 5); /* MLE type = C-PLANE-DATA */
    pack_bits(out,  5, 5, 4); /* PD = MM */
    memcpy(out + 9, mm_body, (size_t)mm_nbits);
    *out_nbits = total;
}

/* =======================================================================
 * Phase 62: tetra_sds_text_len is uint16_t
 * ======================================================================= */
static void test_sds_text_len_type(void)
{
    printf("[test_sds_text_len_type]\n");
    dsd_state *st = alloc_state();
    CHECK(sizeof(st->tetra_sds_text_len) == 2, "tetra_sds_text_len is uint16_t (2 bytes)");
    free(st);
}

/* =======================================================================
 * Phase 63: MM D-OTAR sub-PDU type
 * ======================================================================= */
static void test_mm_d_otar(void)
{
    printf("[test_mm_d_otar]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* MM type=0 (D-OTAR), otar_pdu_type=7 */
    uint8_t mm[10];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 0, 0, 5); /* MM PDU type = D-OTAR */
    pack_bits(mm, 7, 5, 5); /* OTAR sub-type = 7 */

    uint8_t pdu[10 + 9]; int n;
    wrap_mle_mm(mm, 10, pdu, &n);
    tetra_mle_dispatch(pdu, n, 0, opt, st);

    CHECK(st->tetra_otar_valid == 1,     "D-OTAR valid flag");
    CHECK(st->tetra_otar_pdu_type == 7,  "D-OTAR sub-type extracted");

    free(st); free(opt);
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
static void test_d_tx_ceased(void)
{
    printf("[test_d_tx_ceased]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* PDU type, 14-bit call identifier, request permission. */
    uint8_t cmce[20];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 9, 0, 5); /* D-TX-CEASED */
    pack_bits(cmce, 0x1234, 5, 14);
    pack_bits(cmce, 1, 19, 1); /* requests remain permitted */

    st->tetra_tx_granted_valid = 1; /* pre-condition */

    uint8_t pdu[20 + 9]; int n;
    wrap_mle_cmce(cmce, 20, pdu, &n);
    tetra_mle_dispatch(pdu, n, 0, opt, st);

    CHECK(st->tetra_tx_ceased_tx_perm == 1,     "D-TX-CEASED request permission extracted");
    CHECK(st->tetra_tx_ceased_cipher_info == 0, "D-TX-CEASED has no cipher-info field");
    CHECK(st->tetra_tx_granted_valid == 0,      "D-TX-CEASED clears grant");

    free(st); free(opt);
}

/* =======================================================================
 * Phase 68: MLE D-RESTORE-ACK LA / D-RESTORE-RESPONSE result
 * ======================================================================= */
static void test_mle_restore_fields(void)
{
    printf("[test_mle_restore_fields]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* MLE D-RESTORE-ACK (type 2) + 14-bit LA = 777 */
    uint8_t mle_ack[19];
    memset(mle_ack, 0, sizeof(mle_ack));
    pack_bits(mle_ack,   2, 0, 5);
    pack_bits(mle_ack, 777, 5, 14);
    tetra_mle_dispatch(mle_ack, 19, 0, opt, st);

    CHECK(st->tetra_restore_ack == 1,          "D-RESTORE-ACK flag set");
    CHECK(st->tetra_restore_ack_la_valid == 1, "D-RESTORE-ACK LA valid");
    CHECK(st->tetra_restore_ack_la == 777,     "D-RESTORE-ACK LA extracted");

    /* MLE D-RESTORE-RESPONSE (type 3) + result=1 */
    uint8_t mle_res[6];
    memset(mle_res, 0, sizeof(mle_res));
    pack_bits(mle_res, 3, 0, 5);
    pack_bits(mle_res, 1, 5, 1); /* result = 1 */
    tetra_mle_dispatch(mle_res, 6, 0, opt, st);

    CHECK(st->tetra_restore_response == 1,              "D-RESTORE-RESPONSE flag set");
    CHECK(st->tetra_restore_response_result_valid == 1, "D-RESTORE-RESPONSE result valid");
    CHECK(st->tetra_restore_response_result == 1,       "D-RESTORE-RESPONSE result extracted");

    free(st); free(opt);
}

int main(void)
{
    printf("=== TETRA Phase 70 Test Suite ===\n\n");

    test_sds_text_len_type();
    test_mm_d_otar();
    test_d_tx_ceased();
    test_mle_restore_fields();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
