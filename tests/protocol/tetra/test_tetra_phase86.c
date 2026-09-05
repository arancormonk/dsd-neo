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
 * Item 5: D-SDS-LONG-DATA text display in channel_info
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

/* Wrap a CMCE PDU body inside an MLE C-PLANE-DATA envelope (PD=CMCE). */
static void wrap_mle_cmce(const uint8_t *cmce_body, int cmce_nbits,
                           uint8_t *out, int *out_nbits)
{
    int total = 9 + cmce_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_C_PLANE_DATA, 0, 5);  /* MLE type = 24 */
    pack_bits(out, TETRA_MLE_PD_CMCE, 5, 4);        /* PD = 3       */
    memcpy(out + 9, cmce_body, (size_t)cmce_nbits);
    *out_nbits = total;
}

/* Wrap a generic PDU inside an MLE C-PLANE-DATA envelope with given PD. */
static void wrap_mle_pd(const uint8_t *body, int body_nbits,
                         uint32_t pd, uint8_t *out, int *out_nbits)
{
    int total = 9 + body_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_C_PLANE_DATA, 0, 5);
    pack_bits(out, pd, 5, 4);
    memcpy(out + 9, body, (size_t)body_nbits);
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
    pack_bits(cmce, 1, 21, 1); /* notification present */
    pack_bits(cmce, 7, 22, 6); /* notification value */

    uint8_t mle[64];
    int mle_nbits;
    wrap_mle_cmce(cmce, 28, mle, &mle_nbits);
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

    wrap_mle_cmce(cmce, 20, mle, &mle_nbits);
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

    wrap_mle_cmce(cmce, 24, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 7, opt, st);

    CHECK(st->tetra_tx_interrupted == 1,         "D-TX-INTERRUPT flag set");
    CHECK(st->tetra_tx_event_call_id == 0x123,   "D-TX-INTERRUPT call id");
    CHECK(st->tetra_tx_event_encryption == 1,    "D-TX-INTERRUPT encryption");

    free(st);
    free(opt);
}

/* ====================================================================
 * Item 3+5: channel_info consumption++ & D-SDS-LONG display
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
    st->tetra_mm_temp_ssi_valid = 1;
    st->tetra_mm_temp_ssi       = 54321;
    st->tetra_sndcp_valid    = 1;
    st->tetra_sndcp_nsapi    = 5;
    st->tetra_sndcp_pdu_type = 1;
    st->tetra_d_info_valid        = 1;
    st->tetra_d_info_call_id      = 1;
    st->tetra_d_info_call_timeout = 1;

    /* D-SDS-LONG-DATA text (item 5) */
    st->tetra_sds_long_valid    = 1;
    st->tetra_sds_long_text_len = 5;
    memcpy(st->tetra_sds_long_text, "hello", 6);

    char buf[1024];
    tetra_channel_info_fmt(st, buf, sizeof(buf));

    printf("  channel_info: %s\n", buf);

    /* Verify key substrings present */
    CHECK(strstr(buf, "MCC:206") != NULL,     "MCC displayed");
    CHECK(strstr(buf, "TG:100") != NULL,      "TG displayed");
    CHECK(strstr(buf, "SRC:999") != NULL,     "SRC displayed");
    CHECK(strstr(buf, "CCK:4660") != NULL,    "CCK displayed");  /* 0x1234 = 4660 */
    CHECK(strstr(buf, "tmo:1") != NULL,       "call_timeout displayed");
    CHECK(strstr(buf, "bsch:5") != NULL,      "BSCH count displayed");
    CHECK(strstr(buf, "ok:100") != NULL,      "decode_ok displayed");
    CHECK(strstr(buf, "err:3") != NULL,       "decode_errors displayed");
    CHECK(strstr(buf, "fr:200") != NULL,      "frames_total displayed");
    CHECK(strstr(buf, "tx:CI") != NULL,       "floor-control flags displayed");
    CHECK(strstr(buf, "sts:32769") != NULL,   "SDS status displayed");
    CHECK(strstr(buf, "sds_s:42") != NULL,    "SDS short data displayed");
    CHECK(strstr(buf, "rpt:ok") != NULL,      "SDS report displayed");
    CHECK(strstr(buf, "fac:3") != NULL,       "facility displayed");
    CHECK(strstr(buf, "sds_ack:ref7") != NULL,"SDS ACK displayed");
    CHECK(strstr(buf, "srpt:2") != NULL,      "SDS short report displayed");
    CHECK(strstr(buf, "tmpSSI:54321") != NULL, "MM temp SSI displayed");
    CHECK(strstr(buf, "sndcp:5/1") != NULL,   "SNDCP info displayed");
    CHECK(strstr(buf, "dinfo:id1/t1") != NULL, "D-INFO summary displayed");
    CHECK(strstr(buf, "LSDS:\"hello\"") != NULL, "D-SDS-LONG text displayed");

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

    /* Wrap in MLE C-PLANE-DATA with PD=SNDCP(8) */
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

/* ====================================================================
 * Item 5 (extra): D-SDS-LONG-DATA text not shown when empty
 * ==================================================================== */
static void test_long_sds_not_shown_when_empty(void)
{
    printf("--- Item 5 extra: LSDS not shown when empty ---\n");
    dsd_state *st = alloc_state();

    st->tetra_net_known = 1;
    st->tetra_mcc = 1;
    st->tetra_mnc = 1;
    st->tetra_colour = 1;
    st->tetra_sds_long_valid    = 1;
    st->tetra_sds_long_text_len = 0;  /* empty text */

    char buf[512];
    tetra_channel_info_fmt(st, buf, sizeof(buf));
    CHECK(strstr(buf, "LSDS") == NULL, "LSDS not shown when text empty");

    free(st);
}

/* ==================================================================== */
int main(void)
{
    printf("=== TETRA Phase 86 Tests ===\n");

    test_d_tx_expanded();
    test_channel_info_expanded();
    test_sndcp_parser();
    test_long_sds_not_shown_when_empty();

    printf("\n=== Phase 86 Results: %d passed, %d failed ===\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}
