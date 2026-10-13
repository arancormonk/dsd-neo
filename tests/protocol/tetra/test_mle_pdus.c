// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Downlink MLE protocol PDUs (EN 300 392-2 clause 18) through tetra_mle_decode(): D-RESTORE-ACK with its D-CALL
 * RESTORE, D-RESTORE-FAIL, and the network broadcasts D-NWRK-BROADCAST (annex E.2.2), its EXTENSION, DA and REMOVE
 * forms; then what the MLE routes by protocol discriminator besides CMCE (table 18.87): an MM PDU to the MM decoder,
 * and the SN-PDU header (clause 28.4.4, tetra_sndcp_decode()). A PDU that does not fit, or claims an element that shall
 * not be used, decodes incomplete.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_mle_pdu.h"
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

/* Write @p nbits bits of @p val, MSB first, at @p offset. */
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

/* Frees the TETRA state extension with the decoder state. */
static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

static dsd_opts*
alloc_opts(void) {
    return (dsd_opts*)calloc(1, sizeof(dsd_opts));
}

/* A PDU behind protocol discriminator @p pd (EN 300 392-2 table 18.87): the TM-SDU the MLE routes. */
static void
wrap_mle_pd(const uint8_t* body, int body_nbits, uint32_t pd, uint8_t* out, int* out_nbits) {
    DSD_MEMSET(out, 0, (size_t)(3 + body_nbits));
    pack_bits(out, pd, 0, 3);
    DSD_MEMCPY(out + 3, body, (size_t)body_nbits);
    *out_nbits = 3 + body_nbits;
}

static void
wrap_mle_mm(const uint8_t* mm_body, int mm_nbits, uint8_t* out, int* out_nbits) {
    int total = 3 + mm_nbits;
    DSD_MEMSET(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_PD_MM, 0, 3);
    DSD_MEMCPY(out + 3, mm_body, (size_t)mm_nbits);
    *out_nbits = total;
}

static void
test_mle_restore(void) {
    printf("[test_mle_restore]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* Truncated MLE header: PD without its three-bit PDU type. */
    uint8_t mle_ack[5];
    DSD_MEMSET(mle_ack, 0, sizeof(mle_ack));
    pack_bits(mle_ack, TETRA_MLE_PD_MLE, 0, 3);
    tetra_mle_pdu pdu;
    CHECK(!tetra_mle_decode(mle_ack, 5, &pdu), "truncated MLE D-RESTORE-ACK rejected");

    /* D-RESTORE-FAIL without the mandatory O-bit is rejected. */
    uint8_t mle_res[8];
    DSD_MEMSET(mle_res, 0, sizeof(mle_res));
    pack_bits(mle_res, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle_res, TETRA_MLE_D_RESTORE_FAIL, 3, 3);
    CHECK(!tetra_mle_decode(mle_res, 8, &pdu), "truncated MLE D-RESTORE-FAIL rejected");

    free_state(st);
    free(opt);
}

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

static void
test_nwrk_broadcast_annex_e16_to_e18(void) {
    printf("[test_nwrk_broadcast_annex_e16_to_e18]\n");
    tetra_mle_pdu mle;
    uint8_t pdu[200];
    int n = 0;

    /* ETSI EN 300 392-2 V3.8.1, Annex E.2.2 table E.16. */
    pack_bits(pdu, TETRA_MLE_PD_MLE, n, 3);
    n += 3;
    pack_bits(pdu, TETRA_MLE_D_NWRK_BROADCAST, n, 3);
    n += 3;
    pack_bits(pdu, 0x4321, n, 16);
    n += 16;
    pack_bits(pdu, 2, n, 2);
    n += 2;
    pack_bits(pdu, 0, n, 1);
    n += 1;

    CHECK(tetra_mle_decode(pdu, n, &mle) && mle.nwrk_broadcast.reselect == 0x4321, "cell re-select parameters");
    CHECK(mle.nwrk_broadcast.load == 2, "cell load");
    CHECK(mle.nwrk_broadcast.neighbor_count_valid == 0, "Annex E.16 has no neighbour count");

    /* Annex E.17 explicitly carries the zero value meaning no neighbour-cell
     * information is available. */
    DSD_MEMSET(pdu, 0, sizeof(pdu));
    n = 0;
    pack_bits(pdu, TETRA_MLE_PD_MLE, n, 3);
    n += 3;
    pack_bits(pdu, TETRA_MLE_D_NWRK_BROADCAST, n, 3);
    n += 3;
    pack_bits(pdu, 0x1111, n, 16);
    n += 16;
    pack_bits(pdu, 1, n, 2);
    n += 2;
    pack_bits(pdu, 1, n, 1);
    n += 1; /* O-bit */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* no network time */
    pack_bits(pdu, 1, n, 1);
    n += 1; /* neighbour count present */
    pack_bits(pdu, 0, n, 3);
    n += 3;
    CHECK(tetra_mle_decode(pdu, n, &mle) && mle.nwrk_broadcast.neighbor_count_valid == 1
              && mle.nwrk_broadcast.neighbor_count == 0,
          "Annex E.17 no-neighbour indication accepted");

    /* Annex E.18: two CA neighbours.  The first has no optional fields; the
     * second carries only its Location Area from the ten standardized P-bits. */
    DSD_MEMSET(pdu, 0, sizeof(pdu));
    n = 0;
    pack_bits(pdu, TETRA_MLE_PD_MLE, n, 3);
    n += 3;
    pack_bits(pdu, TETRA_MLE_D_NWRK_BROADCAST, n, 3);
    n += 3;
    pack_bits(pdu, 0x2222, n, 16);
    n += 16;
    pack_bits(pdu, 3, n, 2);
    n += 2;
    pack_bits(pdu, 1, n, 1);
    n += 1; /* O-bit */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* no network time */
    pack_bits(pdu, 1, n, 1);
    n += 1; /* neighbour count present */
    pack_bits(pdu, 2, n, 3);
    n += 3;
    pack_bits(pdu, 1, n, 5);
    n += 5;
    pack_bits(pdu, 2, n, 2);
    n += 2;
    pack_bits(pdu, 1, n, 1);
    n += 1;
    pack_bits(pdu, 1, n, 2);
    n += 2;
    pack_bits(pdu, 1000, n, 12);
    n += 12;
    pack_bits(pdu, 0, n, 1);
    n += 1; /* first neighbour O-bit */
    pack_bits(pdu, 2, n, 5);
    n += 5;
    pack_bits(pdu, 1, n, 2);
    n += 2;
    pack_bits(pdu, 0, n, 1);
    n += 1;
    pack_bits(pdu, 2, n, 2);
    n += 2;
    pack_bits(pdu, 2000, n, 12);
    n += 12;
    pack_bits(pdu, 1, n, 1);
    n += 1; /* second neighbour O-bit */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* carrier extension */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* MCC */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* MNC */
    pack_bits(pdu, 1, n, 1);
    n += 1; /* LA */
    pack_bits(pdu, 0x2345, n, 14);
    n += 14;
    pack_bits(pdu, 0, n, 1);
    n += 1; /* maximum TX power */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* minimum RX access */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* subscriber class */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* BS service details */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* timeshare/security */
    pack_bits(pdu, 0, n, 1);
    n += 1; /* TDMA frame offset */
    CHECK(tetra_mle_decode(pdu, n, &mle) && mle.nwrk_broadcast.neighbor_count_valid == 1
              && mle.nwrk_broadcast.neighbor_count == 2,
          "Annex E.18 two-neighbour collection accepted");
    CHECK(mle.nwrk_broadcast.neighbor[0].cell_id == 1 && mle.nwrk_broadcast.neighbor[0].main_carrier == 1000
              && mle.nwrk_broadcast.neighbor[1].cell_id == 2 && mle.nwrk_broadcast.neighbor[1].main_carrier == 2000,
          "Annex E.18 mandatory neighbour fields decoded");

    /* Without the final Type 2 P-bit the broadcast is incomplete. */
    CHECK(!tetra_mle_decode(pdu, n - 1, &mle), "truncated Annex E.18 is incomplete");
}

static void
test_nwrk_ext(void) {
    printf("[test_nwrk_ext]\n");
    tetra_mle_pdu pdu;
    int ok = 0;

    uint8_t mle[256];
    DSD_MEMSET(mle, 0, sizeof(mle));
    int off = 0;
    pack_bits(mle, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(mle, TETRA_MLE_D_NWRK_BCAST_EXT, off, 3);
    off += 3;
    pack_bits(mle, 1, off++, 1); /* O-bit */

    /* Serving-cell channel classes: one table 18.41 entry. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 4);
    off += 4;
    pack_bits(mle, 9, off, 4);
    off += 4;
    pack_bits(mle, 0x23456, off, 18);
    off += 18;
    pack_bits(mle, 17, off, 5);
    off += 5;

    /* One CA-neighbour class (table 18.62). */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 5);
    off += 5;
    pack_bits(mle, 21, off, 5);
    off += 5;
    pack_bits(mle, 6, off, 4);
    off += 4;
    pack_bits(mle, 0x15555, off, 18);
    off += 18;
    pack_bits(mle, 12, off, 5);
    off += 5;

    /* Serving irregular channel with the conditional carrier extension. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 5);
    off += 5;
    pack_bits(mle, 27, off, 5);
    off += 5;
    pack_bits(mle, 0x2AAAA, off, 18);
    off += 18;
    pack_bits(mle, 0xABC, off, 12);
    off += 12;
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 0x2D5, off, 10);
    off += 10;

    /* One CA-neighbour irregular channel without a carrier extension. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 6);
    off += 6;
    pack_bits(mle, 7, off, 5);
    off += 5;
    pack_bits(mle, 3, off, 5);
    off += 5;
    pack_bits(mle, 0x12345, off, 18);
    off += 18;
    pack_bits(mle, 0x456, off, 12);
    off += 12;
    pack_bits(mle, 0, off++, 1);
    pack_bits(mle, 0, off++, 1); /* reserved type-2 P-bit */
    pack_bits(mle, 0, off++, 1); /* reserved type-2 P-bit */

    ok = tetra_mle_decode(mle, off, &pdu);
    CHECK(ok, "NWRK-BCAST-EXT accepted");
    CHECK(pdu.ext.serving_classes_present && pdu.ext.serving_class_count == 1 && pdu.ext.serving_class[0].id == 9
              && pdu.ext.serving_class[0].characteristics == 0x23456 && pdu.ext.serving_class[0].bs_power == 17,
          "serving channel class decoded");
    CHECK(pdu.ext.neighbor_classes_present && pdu.ext.neighbor_class[0].cell_id == 21
              && pdu.ext.neighbor_class[0].channel_class.id == 6,
          "CA neighbour channel class decoded");
    CHECK(pdu.ext.serving_irregular_present && pdu.ext.serving_irregular[0].channel_id == 27
              && pdu.ext.serving_irregular[0].carrier == 0xABC && pdu.ext.serving_irregular[0].extension_valid
              && pdu.ext.serving_irregular[0].carrier_extension == 0x2D5,
          "serving irregular channel and extension decoded");
    CHECK(pdu.ext.neighbor_irregular_present && pdu.ext.neighbor_irregular[0].cell_id == 7
              && pdu.ext.neighbor_irregular[0].channel.channel_id == 3
              && !pdu.ext.neighbor_irregular[0].channel.extension_valid,
          "CA neighbour irregular channel decoded");

    /* A repeated entry cut short after its count leaves the PDU incomplete. */
    ok = tetra_mle_decode(mle, 20, &pdu);
    CHECK(!ok, "truncated repeated entry is incomplete");

    /* A missing final P-bit is likewise malformed. */
    ok = tetra_mle_decode(mle, off - 1, &pdu);
    CHECK(!ok, "truncated extension update is incomplete");

    /* Both reserved type-2 fields are explicitly forbidden by table 18.4. */
    mle[off - 1] = 1;
    ok = tetra_mle_decode(mle, off, &pdu);
    CHECK(!ok, "reserved extension field is rejected");
    mle[off - 1] = 0;

    /* O=0 is a complete PDU without any list. */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle, TETRA_MLE_D_NWRK_BCAST_EXT, 3, 3);
    ok = tetra_mle_decode(mle, 7, &pdu);
    CHECK(ok && !pdu.ext.serving_classes_present && !pdu.ext.neighbor_classes_present
              && !pdu.ext.serving_irregular_present && !pdu.ext.neighbor_irregular_present,
          "O=0 decodes an extension with no lists");
}

static void
test_nwrk_remove(void) {
    printf("[test_nwrk_remove]\n");
    tetra_mle_pdu pdu;
    int ok = 0;
    uint8_t mle[256] = {0};
    int off = 0, da_record_start;

    pack_bits(mle, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(mle, TETRA_MLE_EXTENDED_PDU, off, 3);
    off += 3;
    pack_bits(mle, TETRA_MLE_D_NWRK_BCAST_REMOVE, off, 4);
    off += 4;
    pack_bits(mle, 1, off++, 1); /* O-bit */

    /* Two CA records: remove one whole cell, then selected classes/channels. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 2, off, 5);
    off += 5;
    pack_bits(mle, 7, off, 5);
    off += 5;
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 9, off, 5);
    off += 5;
    pack_bits(mle, 0, off++, 1);
    pack_bits(mle, 2, off, 4);
    off += 4;
    pack_bits(mle, 3, off, 4);
    off += 4;
    pack_bits(mle, 4, off, 4);
    off += 4;
    pack_bits(mle, 2, off, 5);
    off += 5;
    pack_bits(mle, 5, off, 5);
    off += 5;
    pack_bits(mle, 6, off, 5);
    off += 5;

    /* One DA record with selected class/channel removal. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 8);
    off += 8;
    da_record_start = off;
    pack_bits(mle, 0xAB, off, 8);
    off += 8;
    pack_bits(mle, 0, off++, 1);
    pack_bits(mle, 1, off, 4);
    off += 4;
    pack_bits(mle, 8, off, 4);
    off += 4;
    pack_bits(mle, 1, off, 5);
    off += 5;
    pack_bits(mle, 17, off, 5);
    off += 5;

    /* Serving-cell class/channel removal. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 2, off, 4);
    off += 4;
    pack_bits(mle, 1, off, 4);
    off += 4;
    pack_bits(mle, 2, off, 4);
    off += 4;
    pack_bits(mle, 1, off, 5);
    off += 5;
    pack_bits(mle, 30, off, 5);
    off += 5;
    for (int i = 0; i < 4; i++) {
        pack_bits(mle, 0, off++, 1); /* reserved P-bits */
    }

    ok = tetra_mle_decode(mle, off, &pdu);
    CHECK(ok && pdu.remove.ca_present && pdu.remove.ca_count == 2, "network-broadcast CA removal list accepted");
    CHECK(pdu.remove.ca[0].cell_id == 7 && pdu.remove.ca[0].remove_cell && pdu.remove.ca[1].cell_id == 9
              && pdu.remove.ca[1].channels.class_count == 2 && pdu.remove.ca[1].channels.class_id[1] == 4
              && pdu.remove.ca[1].channels.channel_id[1] == 6,
          "whole-cell and selective CA removals decoded");
    CHECK(pdu.remove.da_present && pdu.remove.da_count == 1 && pdu.remove.da[0].cell_id == 0xAB
              && pdu.remove.da[0].channels.class_id[0] == 8 && pdu.remove.da[0].channels.channel_id[0] == 17,
          "selective DA removal decoded");
    CHECK(pdu.remove.serving_present && pdu.remove.serving.class_count == 2 && pdu.remove.serving.class_id[1] == 2
              && pdu.remove.serving.channel_id[0] == 30,
          "serving-cell removal decoded");
    ok = tetra_mle_decode(mle, da_record_start + 4, &pdu);
    CHECK(!ok, "truncated nested DA removal is incomplete");
    ok = tetra_mle_decode(mle, off - 1, &pdu);
    CHECK(!ok, "truncated removal update is incomplete");
    mle[off - 1] = 1;
    ok = tetra_mle_decode(mle, off, &pdu);
    CHECK(!ok, "reserved removal field is rejected");

    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle, TETRA_MLE_EXTENDED_PDU, 3, 3);
    pack_bits(mle, TETRA_MLE_D_NWRK_BCAST_REMOVE, 6, 4);
    ok = tetra_mle_decode(mle, 11, &pdu);
    CHECK(ok && !pdu.remove.ca_present && !pdu.remove.da_present && !pdu.remove.serving_present,
          "O=0 decodes a removal with no records");
}

static void
test_nwrk_broadcast_da(void) {
    printf("[test_nwrk_broadcast_da]\n");
    tetra_mle_pdu pdu;
    int ok = 0;
    uint8_t mle[512] = {0};
    int off = 0, da_entry_start, inner_reserved_pos, outer_reserved_pos;

    pack_bits(mle, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(mle, TETRA_MLE_EXTENDED_PDU, off, 3);
    off += 3;
    pack_bits(mle, TETRA_MLE_D_NWRK_BROADCAST_DA, off, 4);
    off += 4;
    pack_bits(mle, 0, off, 3);
    off += 3;                    /* mandatory reserved field */
    pack_bits(mle, 1, off++, 1); /* O-bit */

    pack_bits(mle, 0, off++, 1); /* reserved 14-bit Type-2 field */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 0x42, off, 8);
    off += 8;
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 0xA55A, off, 16);
    off += 16;
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 5, off, 3);
    off += 3;
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 0x1234, off, 16);
    off += 16;
    pack_bits(mle, 0x89ABCDEF, off, 32);
    off += 32;

    /* Local CA cell, table 18.64, with no optional fields. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 17, off, 5);
    off += 5;
    pack_bits(mle, 2, off, 2);
    off += 2;
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 3, off, 2);
    off += 2;
    pack_bits(mle, 0xABC, off, 12);
    off += 12;
    pack_bits(mle, 0, off++, 1);

    /* One DA neighbour, table 18.65. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 3);
    off += 3;
    da_entry_start = off;
    pack_bits(mle, 0x91, off, 8);
    off += 8;
    pack_bits(mle, 0xC3, off, 8);
    off += 8;
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 0, off++, 1);
    pack_bits(mle, 2, off, 2);
    off += 2;
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 6, off, 3);
    off += 3;
    pack_bits(mle, 0x789, off, 12);
    off += 12;
    pack_bits(mle, 5, off, 3);
    off += 3;
    pack_bits(mle, 4, off, 3);
    off += 3;
    pack_bits(mle, 1, off++, 1); /* neighbour O-bit */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 0x155, off, 10);
    off += 10;
    pack_bits(mle, 0, off++, 1); /* MCC */
    pack_bits(mle, 0, off++, 1); /* MNC */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 0x2345, off, 14);
    off += 14;
    for (int i = 4; i < 11; i++) {
        pack_bits(mle, 0, off++, 1);
    }
    inner_reserved_pos = off;
    for (int i = 0; i < 3; i++) {
        pack_bits(mle, 0, off++, 1);
    }

    /* Serving and DA-neighbour channel classes. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 4);
    off += 4;
    pack_bits(mle, 7, off, 4);
    off += 4;
    pack_bits(mle, 0x23456, off, 18);
    off += 18;
    pack_bits(mle, 19, off, 5);
    off += 5;

    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 5);
    off += 5;
    pack_bits(mle, 0x91, off, 8);
    off += 8;
    pack_bits(mle, 12, off, 4);
    off += 4;
    pack_bits(mle, 0x15555, off, 18);
    off += 18;
    pack_bits(mle, 9, off, 5);
    off += 5;

    /* Serving irregular has an extension; neighbour irregular does not. */
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 5);
    off += 5;
    pack_bits(mle, 23, off, 5);
    off += 5;
    pack_bits(mle, 0x2AAAA, off, 18);
    off += 18;
    pack_bits(mle, 0x456, off, 12);
    off += 12;
    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 0x2D5, off, 10);
    off += 10;

    pack_bits(mle, 1, off++, 1);
    pack_bits(mle, 1, off, 6);
    off += 6;
    pack_bits(mle, 0x91, off, 8);
    off += 8;
    pack_bits(mle, 4, off, 5);
    off += 5;
    pack_bits(mle, 0x12345, off, 18);
    off += 18;
    pack_bits(mle, 0x321, off, 12);
    off += 12;
    pack_bits(mle, 0, off++, 1);

    outer_reserved_pos = off;
    pack_bits(mle, 0, off++, 1);
    pack_bits(mle, 0, off++, 1);

    ok = tetra_mle_decode(mle, off, &pdu);
    CHECK(ok && pdu.da.cell_id_valid && pdu.da.cell_id == 0x42 && pdu.da.reselect == 0xA55A && pdu.da.load == 5
              && pdu.da.network_time == UINT64_C(0x123489ABCDEF),
          "DA broadcast identity, reselection, load, and network time decoded");
    CHECK(pdu.da.local_ca_valid && pdu.da.local_ca.cell_id == 17 && pdu.da.local_ca.reselect_types == 2
              && pdu.da.local_ca.main_carrier == 0xABC,
          "DA broadcast local CA cell decoded");
    CHECK(pdu.da.neighbor_list_present && pdu.da.neighbor_count == 1 && pdu.da.neighbor[0].cell_id == 0x91
              && pdu.da.neighbor[0].load == 6 && pdu.da.neighbor[0].main_carrier == 0x789
              && pdu.da.neighbor[0].optional_mask == ((1u << 0) | (1u << 3))
              && pdu.da.neighbor[0].carrier_extension == 0x155 && pdu.da.neighbor[0].la == 0x2345,
          "DA neighbour mandatory and optional fields decoded");
    CHECK(pdu.da.lists.serving_classes_present && pdu.da.lists.serving_class[0].id == 7
              && pdu.da.lists.neighbor_classes_present && pdu.da.lists.neighbor_class[0].cell_id == 0x91
              && pdu.da.lists.neighbor_class[0].channel_class.id == 12,
          "DA serving and neighbour channel classes decoded");
    CHECK(pdu.da.lists.serving_irregular_present && pdu.da.lists.serving_irregular[0].channel_id == 23
              && pdu.da.lists.serving_irregular[0].extension_valid
              && pdu.da.lists.serving_irregular[0].carrier_extension == 0x2D5 && pdu.da.lists.neighbor_irregular_present
              && pdu.da.lists.neighbor_irregular[0].cell_id == 0x91
              && !pdu.da.lists.neighbor_irregular[0].channel.extension_valid,
          "DA serving and neighbour irregular channels decoded");
    ok = tetra_mle_decode(mle, da_entry_start + 24, &pdu);
    CHECK(!ok, "truncated DA neighbour is incomplete");
    ok = tetra_mle_decode(mle, off - 1, &pdu);
    CHECK(!ok, "missing final DA Type-2 P-bit is incomplete");
    mle[inner_reserved_pos] = 1;
    ok = tetra_mle_decode(mle, off, &pdu);
    CHECK(!ok, "reserved DA-neighbour optional field is rejected");
    mle[inner_reserved_pos] = 0;
    mle[outer_reserved_pos] = 1;
    ok = tetra_mle_decode(mle, off, &pdu);
    CHECK(!ok, "reserved outer DA field is rejected");
    mle[outer_reserved_pos] = 0;
    mle[10] = 1;
    ok = tetra_mle_decode(mle, off, &pdu);
    CHECK(!ok, "nonzero mandatory DA reserved bits are rejected");

    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle, TETRA_MLE_EXTENDED_PDU, 3, 3);
    pack_bits(mle, TETRA_MLE_D_NWRK_BROADCAST_DA, 6, 4);
    ok = tetra_mle_decode(mle, 14, &pdu);
    CHECK(ok && !pdu.da.cell_id_valid && !pdu.da.neighbor_list_present && !pdu.da.lists.serving_classes_present
              && !pdu.da.lists.neighbor_irregular_present,
          "O=0 decodes a DA broadcast with no optional elements");
}

static void
test_mm_group_identity_through_mle(void) {
    printf("[test_mm_group_identity_through_mle]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    uint8_t mm[100];
    int n = 0;

    pack_bits(mm, TETRA_MM_D_ATTACH_DETACH_GROUP, n, 4);
    n += 4;
    pack_bits(mm, 1, n, 1);
    n += 1; // group identity report
    pack_bits(mm, 1, n, 1);
    n += 1; // acknowledgement request
    pack_bits(mm, 1, n, 1);
    n += 1; // attach/detach mode
    pack_bits(mm, 0, n, 1);
    n += 1; // no optional elements

    uint8_t pdu[200];
    int out_n;
    wrap_mle_mm(mm, n, pdu, &out_n);
    char log[256];
    tetra_test_mle_logged(pdu, out_n, 0, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA MM D-GROUP-IDENTITY] CC=0 report=1 ack_request=1 detach_all=1 groups=0") != NULL,
          "group identity report, acknowledgement request and attach/detach mode reach the MM decoder");

    free_state(st);
    free(opt);
}

static void
test_sndcp_header(void) {
    printf("[test_sndcp_header]\n");
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

int
main(void) {
    test_mle_restore();
    test_mle_restore_fields();
    test_nwrk_broadcast_annex_e16_to_e18();
    test_nwrk_ext();
    test_nwrk_remove();
    test_nwrk_broadcast_da();
    test_mm_group_identity_through_mle();
    test_sndcp_header();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
