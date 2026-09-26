// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 86 test suite.
 *
 * Covers all 5 improvement items from the integration analysis:
 *
 * Item 1: D-INFO IE expansion (call_timeout, notification)
 * Item 2: D-TX-* stub expansion (call_id, notification parsed)
 * Item 3: channel_info consumption++ (30+ state fields displayed)
 * Item 4: SNDCP (PD=8) minimal parser
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

/* Wrap a CMCE PDU body inside an protocol-discriminator envelope (PD=CMCE). */
static void wrap_mle_cmce(const uint8_t *cmce_body, int cmce_nbits,
                           uint8_t *out, int *out_nbits)
{
    int total = 3 + cmce_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = total;
}

/* Wrap a generic PDU inside an protocol-discriminator envelope with given PD. */
static void wrap_mle_pd(const uint8_t *body, int body_nbits,
                         uint32_t pd, uint8_t *out, int *out_nbits)
{
    int total = 3 + body_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, pd, 0, 3);
    memcpy(out + 3, body, (size_t)body_nbits);
    *out_nbits = total;
}

/* ====================================================================
 * Item 1: D-INFO IE expansion
 * ==================================================================== */
/* ====================================================================
 * Item 2: D-TX-* stub expansion
 * ==================================================================== */
static void test_d_tx_expanded(void)
{
    printf("--- Item 2: D-TX-* stub expansion ---\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* Table 14.17: call id, continue, request permission, notification. */
    uint8_t cmce[32];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_CONTINUE, 0, 5);
    pack_bits(cmce, 0x2345, 5, 14);
    pack_bits(cmce, 1, 19, 1); /* continue */
    pack_bits(cmce, 1, 20, 1); /* request permission */
    pack_bits(cmce, 1, 21, 1); /* O-bit */
    pack_bits(cmce, 1, 22, 1); /* notification present */
    pack_bits(cmce, 7, 23, 6); /* notification value */
    pack_bits(cmce, 0, 29, 1); /* terminating M-bit */

    uint8_t mle[64];
    int mle_nbits;
    wrap_mle_cmce(cmce, 30, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 7, opt, st);

    CHECK(st->tetra_tx_continue == 1,           "D-TX-CONTINUE flag set");
    CHECK(st->tetra_tx_event_call_id == 0x2345, "D-TX-CONTINUE 14-bit call id");
    CHECK(st->tetra_tx_event_notification == 7, "D-TX-CONTINUE notification");
    CHECK(st->tetra_tx_event_continue == 1,     "D-TX-CONTINUE continue value");

    /* D-TX-WAIT: call_id=0, notification=0 */
    memset(st, 0, sizeof(*st));
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, 0, 5);
    pack_bits(cmce, 0, 5, 14);
    pack_bits(cmce, 0, 19, 1);
    pack_bits(cmce, 0, 20, 1); /* O-bit */

    wrap_mle_cmce(cmce, 21, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 7, opt, st);

    CHECK(st->tetra_tx_wait == 1,                "D-TX-WAIT flag set");
    CHECK(st->tetra_tx_event_call_id == 0,       "D-TX-WAIT call_id = 0");
    CHECK(st->tetra_tx_event_notification == 0,   "D-TX-WAIT notification = 0");

    /* Table 14.19 D-TX-INTERRUPT. */
    memset(st, 0, sizeof(*st));
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_INTERRUPT, 0, 5);
    pack_bits(cmce, 0x123, 5, 14);
    pack_bits(cmce, 1, 19, 2); /* transmission not granted */
    pack_bits(cmce, 0, 21, 1); /* request permission */
    pack_bits(cmce, 1, 22, 1); /* encryption */
    pack_bits(cmce, 0, 24, 1); /* O-bit */

    wrap_mle_cmce(cmce, 25, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 7, opt, st);

    CHECK(st->tetra_tx_interrupted == 1,         "D-TX-INTERRUPT flag set");
    CHECK(st->tetra_tx_event_call_id == 0x123,   "D-TX-INTERRUPT call id");
    CHECK(st->tetra_tx_event_encryption == 1,    "D-TX-INTERRUPT encryption");

    free(st);
    free(opt);
}

/* ====================================================================
 * Item 3: channel_info consumption
 * ==================================================================== */
static void test_channel_info_expanded(void)
{
    printf("--- Item 3+5: channel_info expanded ---\n");
    dsd_state *st = alloc_state();

    /* Populate many state fields */
    st->tetra_net_known     = 1;
    st->tetra_mcc           = 206;
    st->tetra_mnc           = 1;
    st->tetra_colour        = 42;
    st->tetra_dl_carrier_hz = 390000000L;
    st->tetra_mle_ca_neighbor_count_valid = 1;
    st->tetra_mle_ca_neighbor_count = 2;
    st->tetra_mm_group_identity_valid = 1;
    st->tetra_mm_group_entry_count = 3;
    st->tetra_call_active   = 1;
    st->tetra_gssi          = 100;
    st->tetra_calling_ssi   = 999;
    st->tetra_tdma_valid    = 1;
    st->tetra_tn            = 2;
    st->tetra_fn            = 10;
    st->tetra_mn            = 30;
    st->tetra_cck_valid     = 1;
    st->tetra_cck_id        = 0x1234;
    st->tetra_call_timeout  = 1;
    st->tetra_call_slots    = 1;
    st->tetra_bsch_count    = 5;
    st->tetra_decode_ok     = 100;
    st->tetra_decode_errors = 3;
    st->tetra_frames_total  = 200;
    st->tetra_mm_status_valid = 1;
    st->tetra_mm_status_code = 7;
    st->tetra_tx_continue   = 1;
    st->tetra_tx_interrupted = 1;
    st->tetra_sds_status    = 0x8001;
    st->tetra_sds_short_valid = 1;
    st->tetra_sds_short_data  = 42;
    st->tetra_sds_short_src   = 12345;
    st->tetra_sds_report_valid      = 1;
    st->tetra_sds_report_delivery_ok = 1;
    st->tetra_facility_valid  = 1;
    st->tetra_facility_type   = 3;
    st->tetra_sds_ack_valid   = 1;
    st->tetra_sds_ack_msg_ref = 7;
    st->tetra_sds_short_report_valid  = 1;
    st->tetra_sds_short_report_result = 2;
    st->tetra_sndcp_valid    = 1;
    st->tetra_sndcp_nsapi    = 5;
    st->tetra_sndcp_pdu_type = 1;
    st->tetra_d_info_valid        = 1;
    st->tetra_d_info_call_id      = 1;
    st->tetra_d_info_call_timeout = 1;
    st->tetra_d_info_new_call_id = 2;
    st->tetra_d_info_new_call_id_valid = 1;
    st->tetra_d_info_timeout = 9;
    st->tetra_d_info_timeout_valid = 1;
    st->tetra_d_info_status = 3;
    st->tetra_d_info_status_valid = 1;
    st->tetra_d_info_notification_indicator = 42;
    st->tetra_d_info_notification_indicator_valid = 1;
    st->tetra_call_restore_valid = 1;
    st->tetra_call_restore_id = 17;
    st->tetra_call_restore_grant = 3;
    st->tetra_call_restore_new_id = 18;
    st->tetra_call_restore_new_id_valid = 1;
    st->tetra_call_restore_status = 3;
    st->tetra_call_restore_status_valid = 1;

    char buf[1024];
    tetra_channel_info_fmt(st, buf, sizeof(buf));

    printf("  channel_info: %s\n", buf);

    /* Verify key substrings present */
    CHECK(strstr(buf, "MCC:206") != NULL,     "MCC displayed");
    CHECK(strstr(buf, "nbr:2") != NULL,       "CA neighbour count displayed");
    CHECK(strstr(buf, "groups:3") != NULL,    "MM group count displayed");
    CHECK(strstr(buf, "TG:100") != NULL,      "TG displayed");
    CHECK(strstr(buf, "SRC:999") != NULL,     "SRC displayed");
    CHECK(strstr(buf, "CCK:4660") != NULL,    "CCK displayed");  /* 0x1234 = 4660 */
    CHECK(strstr(buf, "tmo:1") != NULL,       "call_timeout displayed");
    CHECK(strstr(buf, "bsch:5") != NULL,      "BSCH count displayed");
    CHECK(strstr(buf, "ok:100") != NULL,      "decode_ok displayed");
    CHECK(strstr(buf, "err:3") != NULL,       "decode_errors displayed");
    CHECK(strstr(buf, "fr:200") != NULL,      "frames_total displayed");
    CHECK(strstr(buf, "mm:7/MS frequency bands request") != NULL,
          "MM status displayed with ETSI label");
    CHECK(strstr(buf, "tx:CI") != NULL,       "floor-control flags displayed");
    CHECK(strstr(buf, "sts:32769") != NULL,   "SDS status displayed");
    CHECK(strstr(buf, "sds_s:42") != NULL,    "SDS short data displayed");
    CHECK(strstr(buf, "rpt:ok") != NULL,      "SDS report displayed");
    CHECK(strstr(buf, "fac:3") != NULL,       "facility displayed");
    CHECK(strstr(buf, "sds_ack:ref7") != NULL,"SDS ACK displayed");
    CHECK(strstr(buf, "srpt:2") != NULL,      "SDS short report displayed");
    CHECK(strstr(buf, "sndcp:5/1") != NULL,   "SNDCP info displayed");
    CHECK(strstr(buf, "dinfo:id1/t1") != NULL, "D-INFO summary displayed");
    CHECK(strstr(buf, "dinfo:id1/t1->2/to9/st3/n42") != NULL,
          "D-INFO optional state displayed");
    CHECK(strstr(buf, "restore:id17/g3->18/st3") != NULL,
          "call restore state displayed");

    st->tetra_sds_report_cause = 0x21;
    st->tetra_sds_report_msg_ref = 231;
    tetra_channel_info_fmt(st, buf, sizeof(buf));
    CHECK(strstr(buf, "rpt:retry/21 ref:231") != NULL,
          "Stored message is retrying, not a terminal failure");
    st->tetra_sds_report_cause = 0x4a;
    tetra_channel_info_fmt(st, buf, sizeof(buf));
    CHECK(strstr(buf, "rpt:fail/4A") != NULL, "Terminal failure status displayed");

    free(st);
}

/* ====================================================================
 * Item 4: SNDCP (PD=8) parser
 * ==================================================================== */
static void test_sndcp_parser(void)
{
    printf("--- Item 4: SNDCP parser ---\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* Build SNDCP PDU:
     *   bits 0-3: NSAPI = 5 (0101)
     *   bits 4-7: PDU type = 3 (0011)
     *   bits 8+:  filler */
    uint8_t sndcp[16];
    memset(sndcp, 0, sizeof(sndcp));
    pack_bits(sndcp, 5, 0, 4);  /* NSAPI = 5 */
    pack_bits(sndcp, 3, 4, 4);  /* PDU type = 3 */

    /* Prefix the SNDCP PDU with the standard 3-bit SNDCP PD. */
    uint8_t mle[64];
    int mle_nbits;
    wrap_mle_pd(sndcp, 16, TETRA_MLE_PD_SNDCP, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 3, opt, st);

    CHECK(st->tetra_sndcp_valid == 1,     "SNDCP valid flag set");
    CHECK(st->tetra_sndcp_nsapi == 5,     "SNDCP NSAPI = 5");
    CHECK(st->tetra_sndcp_pdu_type == 3,  "SNDCP PDU type = 3");
    CHECK(st->tetra_sndcp_nbits == 16,    "SNDCP nbits = 16");

    /* Test short SNDCP (< 8 bits) */
    memset(st, 0, sizeof(*st));
    uint8_t sndcp_short[4];
    memset(sndcp_short, 0, sizeof(sndcp_short));
    wrap_mle_pd(sndcp_short, 4, TETRA_MLE_PD_SNDCP, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 3, opt, st);

    CHECK(st->tetra_sndcp_valid == 0,     "SNDCP short: rejected");
    CHECK(st->tetra_sndcp_nsapi == 0,     "SNDCP short: NSAPI stays 0");

    free(st);
    free(opt);
}

/* ==================================================================== */
int main(void)
{
    printf("=== TETRA Phase 86 Tests ===\n");

    test_d_tx_expanded();
    test_channel_info_expanded();
    test_sndcp_parser();

    printf("\n=== Phase 86 Results: %d passed, %d failed ===\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}
