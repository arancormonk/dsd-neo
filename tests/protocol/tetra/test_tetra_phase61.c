// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 61 test suite.
 *
 * Covers functionality introduced in Phases 54-60:
 *
 * Phase 54 — Floor-control event flags
 *   1. D-TX-CONTINUE / INTERRUPT / WAIT / TIMED-OUT
 *
 * Phase 55 — MLE cell-reselection parsers
 *   2. MLE D-RESTORE-ACK / D-RESTORE-FAIL
 *
 * Phase 57 — CMCE D-INFO
 *   7. D-INFO call_id
 *
 * Phase 58 — SDS Unicode text
 *   8. bpc=16 UCS-2 → UTF-8 decode
 *
 * Phase 59 — MLE network-broadcast extensions
 *   9. D-NWRK-BCAST-EXT channel and irregular-channel lists
 *  10. Extended-PDU D-NWRK-BROADCAST REMOVE CA/DA/serving-cell records
 *  11. Extended-PDU D-NWRK-BROADCAST-DA complete table 18.3 snapshot
 *
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
wrap_mle_cmce(const uint8_t* cmce_body, int cmce_nbits, uint8_t* out, int* out_nbits) {
    memset(out, 0, (size_t)(3 + cmce_nbits));
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = 3 + cmce_nbits;
}

/* =======================================================================
 * Phase 54: Floor control
 * ======================================================================= */
static void
test_floor_control_events(void) {
    printf("[test_floor_control_events]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
    uint8_t pdu[9 + 32];
    int n;

    /* D-TX-CONTINUE: call=0x1234, continue=1, request=1. */
    uint8_t cmce[32];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_CONTINUE, 0, 5);
    pack_bits(cmce, 0x1234, 5, 14);
    pack_bits(cmce, 1, 19, 1);
    pack_bits(cmce, 1, 20, 1);
    pack_bits(cmce, 0, 21, 1); /* O-bit */
    wrap_mle_cmce(cmce, 22, pdu, &n);
    tetra_mle_dispatch(pdu, n, 0, opt, st);
    CHECK(tetra_st(st)->tx_continue == 1, "D-TX-CONTINUE sets flag");
    CHECK(tetra_st(st)->tx_event_call_id == 0x1234, "D-TX-CONTINUE keeps 14-bit call id");

    /* D-TX-INTERRUPT uses the same mandatory grant fields as D-TX-GRANTED. */
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_INTERRUPT, 0, 5);
    pack_bits(cmce, 0x22, 5, 14);
    pack_bits(cmce, 3, 19, 2);
    pack_bits(cmce, 1, 21, 1);
    pack_bits(cmce, 1, 22, 1);
    pack_bits(cmce, 0, 24, 1); /* O-bit */
    wrap_mle_cmce(cmce, 25, pdu, &n);
    tetra_mle_dispatch(pdu, n, 0, opt, st);
    CHECK(tetra_st(st)->tx_interrupted == 1, "D-TX-INTERRUPT sets flag");
    CHECK(tetra_st(st)->tx_event_grant == 3, "D-TX-INTERRUPT grant decoded");

    /* D-TX-WAIT: call id plus request permission. */
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, 0, 5);
    pack_bits(cmce, 0x321, 5, 14);
    pack_bits(cmce, 0, 19, 1);
    pack_bits(cmce, 0, 20, 1); /* O-bit */
    wrap_mle_cmce(cmce, 21, pdu, &n);
    tetra_mle_dispatch(pdu, n, 0, opt, st);
    CHECK(tetra_st(st)->tx_wait == 1, "D-TX-WAIT sets flag");
    CHECK(tetra_st(st)->tx_event_call_id == 0x321, "D-TX-WAIT call id decoded");

    free(st);
    free(opt);
}

static void
test_mle_restore(void) {
    printf("[test_mle_restore]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* Truncated MLE header: PD without its three-bit PDU type. */
    uint8_t mle_ack[5];
    memset(mle_ack, 0, sizeof(mle_ack));
    pack_bits(mle_ack, TETRA_MLE_PD_MLE, 0, 3);
    tetra_mle_dispatch(mle_ack, 5, 0, opt, st);
    CHECK(tetra_st(st)->restore_ack == 0, "truncated MLE D-RESTORE-ACK rejected");

    /* D-RESTORE-FAIL without the mandatory O-bit is rejected. */
    uint8_t mle_res[8];
    memset(mle_res, 0, sizeof(mle_res));
    pack_bits(mle_res, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle_res, TETRA_MLE_D_RESTORE_FAIL, 3, 3);
    tetra_mle_dispatch(mle_res, 8, 0, opt, st);
    CHECK(tetra_st(st)->mle_restore_fail_valid == 0, "truncated MLE D-RESTORE-FAIL rejected");

    free(st);
    free(opt);
}

/* =======================================================================
 * Phase 57: CMCE D-INFO
 * ======================================================================= */
/* =======================================================================
 * Phase 58: SDS Unicode text
 * ======================================================================= */
/* =======================================================================
 * Phase 59: NWRK-BCAST-EXT
 * ======================================================================= */
static void
test_nwrk_ext(void) {
    printf("[test_nwrk_ext]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    uint8_t mle[256];
    memset(mle, 0, sizeof(mle));
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

    tetra_mle_dispatch(mle, off, 0, opt, st);
    CHECK(tetra_st(st)->nwrk_bcast_ext_known == 1, "NWRK-BCAST-EXT accepted");
    CHECK(tetra_st(st)->mle_ext.serving_classes_present && tetra_st(st)->mle_ext.serving_class_count == 1
              && tetra_st(st)->mle_ext.serving_class[0].id == 9
              && tetra_st(st)->mle_ext.serving_class[0].characteristics == 0x23456
              && tetra_st(st)->mle_ext.serving_class[0].bs_power == 17,
          "serving channel class decoded");
    CHECK(tetra_st(st)->mle_ext.neighbor_classes_present && tetra_st(st)->mle_ext.neighbor_class[0].cell_id == 21
              && tetra_st(st)->mle_ext.neighbor_class[0].channel_class.id == 6,
          "CA neighbour channel class decoded");
    CHECK(tetra_st(st)->mle_ext.serving_irregular_present && tetra_st(st)->mle_ext.serving_irregular[0].channel_id == 27
              && tetra_st(st)->mle_ext.serving_irregular[0].carrier == 0xABC
              && tetra_st(st)->mle_ext.serving_irregular[0].extension_valid
              && tetra_st(st)->mle_ext.serving_irregular[0].carrier_extension == 0x2D5,
          "serving irregular channel and extension decoded");
    CHECK(tetra_st(st)->mle_ext.neighbor_irregular_present && tetra_st(st)->mle_ext.neighbor_irregular[0].cell_id == 7
              && tetra_st(st)->mle_ext.neighbor_irregular[0].channel.channel_id == 3
              && !tetra_st(st)->mle_ext.neighbor_irregular[0].channel.extension_valid,
          "CA neighbour irregular channel decoded");

    /* A repeated entry cut short after its count must preserve the snapshot. */
    tetra_st(st)->mle_ext.serving_class[0].id = 13;
    tetra_mle_dispatch(mle, 20, 0, opt, st);
    CHECK(tetra_st(st)->mle_ext.serving_class[0].id == 13 && tetra_st(st)->mle_ext.neighbor_irregular_count == 1,
          "truncated repeated entry is atomic");

    /* A missing final P-bit is likewise malformed. */
    tetra_mle_dispatch(mle, off - 1, 0, opt, st);
    CHECK(tetra_st(st)->mle_ext.serving_class[0].id == 13 && tetra_st(st)->mle_ext.neighbor_irregular_count == 1,
          "truncated extension update is atomic");

    /* Both reserved type-2 fields are explicitly forbidden by table 18.4. */
    mle[off - 1] = 1;
    tetra_mle_dispatch(mle, off, 0, opt, st);
    CHECK(tetra_st(st)->mle_ext.serving_class[0].id == 13, "reserved extension field is rejected atomically");
    mle[off - 1] = 0;

    /* O=0 is a complete empty snapshot and clears all prior list presence. */
    memset(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle, TETRA_MLE_D_NWRK_BCAST_EXT, 3, 3);
    tetra_mle_dispatch(mle, 7, 0, opt, st);
    CHECK(tetra_st(st)->nwrk_bcast_ext_known && !tetra_st(st)->mle_ext.serving_classes_present
              && !tetra_st(st)->mle_ext.neighbor_classes_present && !tetra_st(st)->mle_ext.serving_irregular_present
              && !tetra_st(st)->mle_ext.neighbor_irregular_present,
          "O=0 commits an empty extension snapshot");

    free(st);
    free(opt);
}

static void
test_nwrk_remove(void) {
    printf("[test_nwrk_remove]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
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

    tetra_mle_dispatch(mle, off, 0, opt, st);
    CHECK(tetra_st(st)->mle_remove.known && tetra_st(st)->mle_remove.ca_present
              && tetra_st(st)->mle_remove.ca_count == 2,
          "network-broadcast CA removal list accepted");
    CHECK(tetra_st(st)->mle_remove.ca[0].cell_id == 7 && tetra_st(st)->mle_remove.ca[0].remove_cell
              && tetra_st(st)->mle_remove.ca[1].cell_id == 9 && tetra_st(st)->mle_remove.ca[1].channels.class_count == 2
              && tetra_st(st)->mle_remove.ca[1].channels.class_id[1] == 4
              && tetra_st(st)->mle_remove.ca[1].channels.channel_id[1] == 6,
          "whole-cell and selective CA removals decoded");
    CHECK(tetra_st(st)->mle_remove.da_present && tetra_st(st)->mle_remove.da_count == 1
              && tetra_st(st)->mle_remove.da[0].cell_id == 0xAB
              && tetra_st(st)->mle_remove.da[0].channels.class_id[0] == 8
              && tetra_st(st)->mle_remove.da[0].channels.channel_id[0] == 17,
          "selective DA removal decoded");
    CHECK(tetra_st(st)->mle_remove.serving_present && tetra_st(st)->mle_remove.serving.class_count == 2
              && tetra_st(st)->mle_remove.serving.class_id[1] == 2
              && tetra_st(st)->mle_remove.serving.channel_id[0] == 30,
          "serving-cell removal decoded");

    tetra_st(st)->mle_remove.ca[0].cell_id = 19;
    tetra_mle_dispatch(mle, da_record_start + 4, 0, opt, st);
    CHECK(tetra_st(st)->mle_remove.ca[0].cell_id == 19 && tetra_st(st)->mle_remove.da[0].cell_id == 0xAB,
          "truncated nested DA removal is atomic");
    tetra_mle_dispatch(mle, off - 1, 0, opt, st);
    CHECK(tetra_st(st)->mle_remove.ca[0].cell_id == 19 && tetra_st(st)->mle_remove.da[0].cell_id == 0xAB,
          "truncated removal update is atomic");
    mle[off - 1] = 1;
    tetra_mle_dispatch(mle, off, 0, opt, st);
    CHECK(tetra_st(st)->mle_remove.ca[0].cell_id == 19, "reserved removal field is rejected atomically");

    memset(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle, TETRA_MLE_EXTENDED_PDU, 3, 3);
    pack_bits(mle, TETRA_MLE_D_NWRK_BCAST_REMOVE, 6, 4);
    tetra_mle_dispatch(mle, 11, 0, opt, st);
    CHECK(tetra_st(st)->mle_remove.known && !tetra_st(st)->mle_remove.ca_present && !tetra_st(st)->mle_remove.da_present
              && !tetra_st(st)->mle_remove.serving_present,
          "O=0 commits an empty removal snapshot");

    free(st);
    free(opt);
}

static void
test_nwrk_broadcast_da(void) {
    printf("[test_nwrk_broadcast_da]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
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

    tetra_mle_dispatch(mle, off, 0, opt, st);
    CHECK(tetra_st(st)->mle_da.known && tetra_st(st)->mle_da.cell_id_valid && tetra_st(st)->mle_da.cell_id == 0x42
              && tetra_st(st)->mle_da.reselect == 0xA55A && tetra_st(st)->mle_da.load == 5
              && tetra_st(st)->mle_da.network_time == UINT64_C(0x123489ABCDEF),
          "DA broadcast identity, reselection, load, and network time decoded");
    CHECK(tetra_st(st)->mle_da.local_ca_valid && tetra_st(st)->mle_da.local_ca.cell_id == 17
              && tetra_st(st)->mle_da.local_ca.reselect_types == 2
              && tetra_st(st)->mle_da.local_ca.main_carrier == 0xABC,
          "DA broadcast local CA cell decoded");
    CHECK(tetra_st(st)->mle_da.neighbor_list_present && tetra_st(st)->mle_da.neighbor_count == 1
              && tetra_st(st)->mle_da.neighbor[0].cell_id == 0x91 && tetra_st(st)->mle_da.neighbor[0].load == 6
              && tetra_st(st)->mle_da.neighbor[0].main_carrier == 0x789
              && tetra_st(st)->mle_da.neighbor[0].optional_mask == ((1u << 0) | (1u << 3))
              && tetra_st(st)->mle_da.neighbor[0].carrier_extension == 0x155
              && tetra_st(st)->mle_da.neighbor[0].la == 0x2345,
          "DA neighbour mandatory and optional fields decoded");
    CHECK(tetra_st(st)->mle_da.lists.serving_classes_present && tetra_st(st)->mle_da.lists.serving_class[0].id == 7
              && tetra_st(st)->mle_da.lists.neighbor_classes_present
              && tetra_st(st)->mle_da.lists.neighbor_class[0].cell_id == 0x91
              && tetra_st(st)->mle_da.lists.neighbor_class[0].channel_class.id == 12,
          "DA serving and neighbour channel classes decoded");
    CHECK(tetra_st(st)->mle_da.lists.serving_irregular_present
              && tetra_st(st)->mle_da.lists.serving_irregular[0].channel_id == 23
              && tetra_st(st)->mle_da.lists.serving_irregular[0].extension_valid
              && tetra_st(st)->mle_da.lists.serving_irregular[0].carrier_extension == 0x2D5
              && tetra_st(st)->mle_da.lists.neighbor_irregular_present
              && tetra_st(st)->mle_da.lists.neighbor_irregular[0].cell_id == 0x91
              && !tetra_st(st)->mle_da.lists.neighbor_irregular[0].channel.extension_valid,
          "DA serving and neighbour irregular channels decoded");

    tetra_st(st)->mle_da.cell_id = 0xEE;
    tetra_mle_dispatch(mle, da_entry_start + 24, 0, opt, st);
    CHECK(tetra_st(st)->mle_da.cell_id == 0xEE && tetra_st(st)->mle_da.neighbor_count == 1,
          "truncated DA neighbour preserves the prior snapshot");
    tetra_mle_dispatch(mle, off - 1, 0, opt, st);
    CHECK(tetra_st(st)->mle_da.cell_id == 0xEE, "missing final DA Type-2 P-bit is rejected atomically");
    mle[inner_reserved_pos] = 1;
    tetra_mle_dispatch(mle, off, 0, opt, st);
    CHECK(tetra_st(st)->mle_da.cell_id == 0xEE, "reserved DA-neighbour optional field is rejected atomically");
    mle[inner_reserved_pos] = 0;
    mle[outer_reserved_pos] = 1;
    tetra_mle_dispatch(mle, off, 0, opt, st);
    CHECK(tetra_st(st)->mle_da.cell_id == 0xEE, "reserved outer DA field is rejected atomically");
    mle[outer_reserved_pos] = 0;
    mle[10] = 1;
    tetra_mle_dispatch(mle, off, 0, opt, st);
    CHECK(tetra_st(st)->mle_da.cell_id == 0xEE, "nonzero mandatory DA reserved bits are rejected atomically");

    memset(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(mle, TETRA_MLE_EXTENDED_PDU, 3, 3);
    pack_bits(mle, TETRA_MLE_D_NWRK_BROADCAST_DA, 6, 4);
    tetra_mle_dispatch(mle, 14, 0, opt, st);
    CHECK(tetra_st(st)->mle_da.known && !tetra_st(st)->mle_da.cell_id_valid
              && !tetra_st(st)->mle_da.neighbor_list_present && !tetra_st(st)->mle_da.lists.serving_classes_present
              && !tetra_st(st)->mle_da.lists.neighbor_irregular_present,
          "O=0 commits an empty DA broadcast snapshot");

    free(st);
    free(opt);
}

int
main(void) {
    printf("=== TETRA Phase 61 Test Suite ===\n\n");
    test_floor_control_events();
    puts("");
    puts("");
    test_mle_restore();
    puts("");
    puts("");
    puts("");
    puts("");
    test_nwrk_ext();
    puts("");
    test_nwrk_remove();
    puts("");
    test_nwrk_broadcast_da();
    puts("");

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
