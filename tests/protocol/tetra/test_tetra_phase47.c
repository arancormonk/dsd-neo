// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 47 test suite.
 *
 * Covers functionality introduced in Phases 43-46:
 *
 * Phase 43 — TDMA timestamps in BSCH:
 *   1.  TDMA fields (tetra_tn/fn/mn/tdma_valid) zero-initialise correctly
 *   2.  tetra_bsch_parse() with TN=1 (raw 0), FN=5, MN=12 populates fields
 *   3.  tetra_tn is 1-based (tn_raw + 1)
 *   4.  tetra_tdma_valid is set to 1 after a successful parse
 *   5.  Second BSCH with different TN/FN/MN overwrites previous values
 *
 * Phase 44 — MM D-LU-REJECT + D-TEMPORARY-ADDRESS parsers:
 *   6.  tetra_mm_lu_reject_* fields zero-initialise correctly
 *   7.  D-LU-REJECT with cause=4 sets tetra_mm_lu_reject_cause=4 and valid=1
 *   8.  D-LU-REJECT with fewer than 8 bits still sets valid=1 (cause=0)
 *   9.  tetra_mm_temp_ssi* fields zero-initialise correctly
 *  10.  D-TEMPORARY-ADDRESS with SSI=123456 sets temp_ssi and valid=1
 *  11.  D-TEMPORARY-ADDRESS with < 29 bits still sets valid=1 (ssi=0)
 *
 * Phase 45 — D-TX-GRANTED updates UI fields:
 *  12.  tetra_sds_msg_ref and tetra_sds_last_cc zero-initialise correctly
 *  13.  D-TX-GRANTED with SSI sets lastsrc to granted SSI
 *  14.  D-TX-GRANTED with SSI sets active_channel[0] to "TETRA TG:... SRC:...  (TX-GRANTED)"
 *  15.  D-TX-GRANTED with SSI sets last_active_time != 0
 *
 * Phase 46 — SDS message reference tracking:
 *  16.  D-SDS-DATA stores msg_ref in tetra_sds_msg_ref
 *  17.  D-SDS-DATA stores CC in tetra_sds_last_cc
 *  18.  Two SDS PDUs with different msg_ref values update sds_msg_ref correctly
 */

#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra.h>
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

/* -----------------------------------------------------------------------
 * BSCH helpers — build a 60-bit PDU with specified fields.
 *
 * BSCH bit layout (ETSI EN 300 392-2 §21.3.3):
 *   [0-3]   scrambling / reserved          (4 bits — leave 0)
 *   [4-9]   colour code                    (6 bits)
 *  [10-11]  TN timeslot (0-based, add 1)   (2 bits)
 *  [12-16]  FN frame number 0-17           (5 bits)
 *  [17-22]  MN multiframe number 0-59      (6 bits)
 *  [23-30]  reserved / HN partial          (8 bits — leave 0)
 *  [31-40]  MCC                            (10 bits)
 *  [41-54]  MNC                            (14 bits)
 *  [55-59]  reserved                       (5 bits — leave 0)
 * ----------------------------------------------------------------------- */
static void bsch_build(uint8_t bits[60],
                       uint8_t colour,
                       uint8_t tn_raw,   /* 0-based, function adds 1 */
                       uint8_t fn,
                       uint8_t mn,
                       uint16_t mcc,
                       uint16_t mnc)
{
    memset(bits, 0, 60);
    pack_bits(bits, colour,  4,  6);
    pack_bits(bits, tn_raw, 10,  2);
    pack_bits(bits, fn,     12,  5);
    pack_bits(bits, mn,     17,  6);
    pack_bits(bits, mcc,    31, 10);
    pack_bits(bits, mnc,    41, 14);
}

/* -----------------------------------------------------------------------
 * MLE / CMCE helper — wrap a raw CMCE PDU in an MLE C-PLANE-DATA frame.
 * ----------------------------------------------------------------------- */
static void wrap_mle_cmce(const uint8_t *cmce_body, int cmce_nbits,
                           uint8_t *out, int *out_nbits)
{
    memset(out, 0, (size_t)(9 + cmce_nbits));
    pack_bits(out, 24, 0, 5);   /* mle_type = TETRA_MLE_C_PLANE_DATA */
    pack_bits(out,  3, 5, 4);   /* pd       = TETRA_MLE_PD_CMCE       */
    memcpy(out + 9, cmce_body, (size_t)cmce_nbits);
    *out_nbits = 9 + cmce_nbits;
}

/* -----------------------------------------------------------------------
 * Helper: build an MLE C-PLANE-DATA / MM wrapper.
 *   mle_type=24, pd=5 (MM), then mm_body
 * ----------------------------------------------------------------------- */
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
 * Phase 43: TDMA timestamps in BSCH
 * ======================================================================= */

static void test_tdma_fields_zero(void)
{
    printf("[test_tdma_fields_zero]\n");
    dsd_state *st = alloc_state();

    CHECK(st->tetra_tdma_valid == 0, "tdma_valid zero after calloc");
    CHECK(st->tetra_tn         == 0, "tetra_tn zero after calloc");
    CHECK(st->tetra_fn         == 0, "tetra_fn zero after calloc");
    CHECK(st->tetra_mn         == 0, "tetra_mn zero after calloc");

    free(st);
}

static void test_bsch_tdma_parse(void)
{
    printf("[test_bsch_tdma_parse]\n");

    /* TN raw=0 → stored as 1; FN=5; MN=12 */
    uint8_t bits[60];
    bsch_build(bits, /*colour=*/0x15, /*tn_raw=*/0, /*fn=*/5, /*mn=*/12,
               /*mcc=*/234, /*mnc=*/30);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    int result = tetra_bsch_parse(bits, 60, opt, st);

    CHECK(result           == 1,  "bsch_parse returns 1 on success");
    CHECK(st->tetra_tdma_valid == 1,  "tdma_valid = 1 after bsch_parse");
    CHECK(st->tetra_tn     == 1,  "tetra_tn = tn_raw+1 = 1");
    CHECK(st->tetra_fn     == 5,  "tetra_fn = 5");
    CHECK(st->tetra_mn     == 12, "tetra_mn = 12");

    free(st); free(opt);
}

static void test_bsch_tdma_tn_onebased(void)
{
    printf("[test_bsch_tdma_tn_onebased]\n");

    uint8_t bits[60];
    /* tn_raw = 3 → stored TN = 4 */
    bsch_build(bits, 0x01, 3, 17, 59, 310, 100);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_bsch_parse(bits, 60, opt, st);

    CHECK(st->tetra_tn == 4,  "tetra_tn = tn_raw(3)+1 = 4");
    CHECK(st->tetra_fn == 17, "tetra_fn = 17 (max frame)");
    CHECK(st->tetra_mn == 59, "tetra_mn = 59 (max multiframe)");

    free(st); free(opt);
}

static void test_bsch_tdma_overwrite(void)
{
    printf("[test_bsch_tdma_overwrite]\n");

    uint8_t bits[60];

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* First BSCH: TN=1, FN=0, MN=0 */
    bsch_build(bits, 0x01, 0, 0, 0, 100, 1);
    tetra_bsch_parse(bits, 60, opt, st);
    CHECK(st->tetra_tn == 1, "first BSCH: tn=1");

    /* Second BSCH: TN=3, FN=15, MN=30 */
    bsch_build(bits, 0x01, 2, 15, 30, 100, 1);
    tetra_bsch_parse(bits, 60, opt, st);
    CHECK(st->tetra_tn == 3,  "second BSCH overwrites tn=3");
    CHECK(st->tetra_fn == 15, "second BSCH overwrites fn=15");
    CHECK(st->tetra_mn == 30, "second BSCH overwrites mn=30");

    free(st); free(opt);
}

static void test_tdma_ndb_advance_and_rollover(void)
{
    printf("[test_tdma_ndb_advance_and_rollover]\n");
    dsd_state *st = alloc_state();

    /* No trustworthy BSCH timestamp: leave all counters untouched. */
    st->tetra_tn = 3;
    st->tetra_fn = 9;
    st->tetra_mn = 37;
    tetra_tdma_advance_ndb(st);
    CHECK(st->tetra_tn == 3 && st->tetra_fn == 9 && st->tetra_mn == 37,
          "NDB advance is inert before BSCH timing is valid");

    st->tetra_tdma_valid = 1;
    tetra_tdma_advance_ndb(st);
    CHECK(st->tetra_tn == 4 && st->tetra_fn == 9 && st->tetra_mn == 37,
          "TN3 advances to TN4 without changing FN/MN");

    tetra_tdma_advance_ndb(st);
    CHECK(st->tetra_tn == 1 && st->tetra_fn == 10 && st->tetra_mn == 37,
          "TN4 wraps to TN1 and increments FN");

    st->tetra_tn = 4;
    st->tetra_fn = 17;
    st->tetra_mn = 59;
    tetra_tdma_advance_ndb(st);
    CHECK(st->tetra_tn == 1 && st->tetra_fn == 0 && st->tetra_mn == 0,
          "last frame and multiframe wrap to zero");

    free(st);
}

/* =======================================================================
 * Phase 44: MM D-LU-REJECT + D-TEMPORARY-ADDRESS
 * ======================================================================= */

static void test_mm_lu_reject_fields_zero(void)
{
    printf("[test_mm_lu_reject_fields_zero]\n");
    dsd_state *st = alloc_state();

    CHECK(st->tetra_mm_lu_reject_valid == 0, "lu_reject_valid zero after calloc");
    CHECK(st->tetra_mm_lu_reject_cause == 0, "lu_reject_cause zero after calloc");

    free(st);
}

static void test_mm_d_lu_reject_cause(void)
{
    printf("[test_mm_d_lu_reject_cause]\n");

    /*
     * MM D-LU-REJECT PDU:
     *   [0-4]  pdu_type = 7  (TETRA_MM_D_LOCATION_UPDATING_REJECT)
     *   [5-7]  cause = 4     (3 bits)
     * Total: 8 bits
     */
    uint8_t mm[8];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 7u, 0, 5);   /* pdu_type = 7 */
    pack_bits(mm, 4u, 5, 3);   /* cause = 4    */

    uint8_t mle[9 + 8];
    int mle_nbits;
    wrap_mle_mm(mm, 8, mle, &mle_nbits);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, mle_nbits, 7, opt, st);

    CHECK(st->tetra_mm_lu_reject_valid == 1, "D-LU-REJECT: valid=1");
    CHECK(st->tetra_mm_lu_reject_cause == 4, "D-LU-REJECT: cause=4");

    free(st); free(opt);
}

static void test_mm_d_lu_reject_short(void)
{
    printf("[test_mm_d_lu_reject_short]\n");

    /* Only 5 bits — pdu_type only, no cause field */
    uint8_t mm[5];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 7u, 0, 5);

    uint8_t mle[9 + 5];
    int mle_nbits;
    wrap_mle_mm(mm, 5, mle, &mle_nbits);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* Must not crash */
    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    CHECK(st->tetra_mm_lu_reject_valid == 1, "D-LU-REJECT short: still sets valid=1");
    CHECK(st->tetra_mm_lu_reject_cause == 0, "D-LU-REJECT short: cause defaults to 0");

    free(st); free(opt);
}

static void test_mm_temp_ssi_fields_zero(void)
{
    printf("[test_mm_temp_ssi_fields_zero]\n");
    dsd_state *st = alloc_state();

    CHECK(st->tetra_mm_temp_ssi_valid == 0, "temp_ssi_valid zero after calloc");
    CHECK(st->tetra_mm_temp_ssi       == 0, "temp_ssi zero after calloc");

    free(st);
}

static void test_mm_d_temporary_address_ssi(void)
{
    printf("[test_mm_d_temporary_address_ssi]\n");

    /*
     * MM D-TEMPORARY-ADDRESS PDU:
     *   [0-4]   pdu_type = 9
     *   [5-28]  temporary SSI = 123456 (24 bits)
     * Total: 29 bits
     */
    uint8_t mm[29];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 9u,      0,  5);  /* pdu_type = 9 */
    pack_bits(mm, 123456u, 5, 24);  /* temp_ssi     */

    uint8_t mle[9 + 29];
    int mle_nbits;
    wrap_mle_mm(mm, 29, mle, &mle_nbits);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    CHECK(st->tetra_mm_temp_ssi_valid == 1,      "D-TEMP-ADDR: valid=1");
    CHECK(st->tetra_mm_temp_ssi       == 123456u, "D-TEMP-ADDR: ssi=123456");

    free(st); free(opt);
}

static void test_mm_d_temporary_address_short(void)
{
    printf("[test_mm_d_temporary_address_short]\n");

    /* Only pdu_type, no SSI field */
    uint8_t mm[5];
    memset(mm, 0, sizeof(mm));
    pack_bits(mm, 9u, 0, 5);

    uint8_t mle[9 + 5];
    int mle_nbits;
    wrap_mle_mm(mm, 5, mle, &mle_nbits);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    CHECK(st->tetra_mm_temp_ssi_valid == 1, "D-TEMP-ADDR short: still sets valid=1");
    CHECK(st->tetra_mm_temp_ssi       == 0, "D-TEMP-ADDR short: ssi=0");

    free(st); free(opt);
}

/* =======================================================================
 * Phase 45: D-TX-GRANTED updates UI fields
 * ======================================================================= */

static void test_tx_granted_ui_fields_zero(void)
{
    printf("[test_tx_granted_ui_fields_zero]\n");
    dsd_state *st = alloc_state();

    CHECK(st->tetra_sds_msg_ref  == 0,  "sds_msg_ref zero after calloc");
    CHECK(st->tetra_sds_last_cc  == 0,  "sds_last_cc zero after calloc");

    free(st);
}

/*
 * Build a D-TX-GRANTED CMCE PDU with granted SSI present.
 *
 * CMCE D-TX-GRANTED layout (PDU type=10):
 *   [0-4]  pdu_type = 10
 *   [5]    tx_perm  (1 bit)
 *   [6-7]  enc_mode (2 bits)
 *   [8]    reserv   (1 bit)
 *   [9]    gp_present = 1
 *   [10-12] addr_type = 1 (SSI)
 *   [13-36] granted_ssi (24 bits)
 *   [37]   ac_present = 0
 * Total: 38 bits
 */
static void build_d_tx_granted(uint8_t *cmce, uint32_t granted_ssi)
{
    memset(cmce, 0, 52);
    pack_bits(cmce, 11u,          0,  5);   /* D-TX-GRANTED */
    pack_bits(cmce,  1u,          5, 14);   /* call identifier */
    pack_bits(cmce,  3u,         19,  2);   /* granted to another */
    pack_bits(cmce,  1u,         21,  1);   /* request permission */
    pack_bits(cmce,  0u,         22,  1);   /* encryption */
    pack_bits(cmce,  0u,         23,  1);   /* reserved */
    pack_bits(cmce,  0u,         24,  1);   /* notification absent */
    pack_bits(cmce,  1u,         25,  1);   /* TPTI present */
    pack_bits(cmce,  1u,         26,  2);   /* TPTI = SSI */
    pack_bits(cmce, granted_ssi, 28, 24);   /* SSI */
}

static void test_tx_granted_identity_set(void)
{
    printf("[test_tx_granted_identity_set]\n");

    uint8_t cmce[52];
    build_d_tx_granted(cmce, 88888u);

    uint8_t mle[9 + 52];
    int mle_nbits;
    wrap_mle_cmce(cmce, 52, mle, &mle_nbits);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    st->tetra_gssi = 44444;

    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    CHECK(st->tetra_tx_granted_ssi    == 88888u, "TX-GRANTED: tx_granted_ssi = 88888");
    CHECK(st->tetra_tx_granted_valid  == 1,      "TX-GRANTED: valid = 1");
    CHECK(st->tetra_gssi              == 44444u, "TX-GRANTED: current GSSI retained");

    free(st); free(opt);
}

static void test_tx_granted_without_channel_assignment(void)
{
    printf("[test_tx_granted_without_channel_assignment]\n");

    uint8_t cmce[52];
    build_d_tx_granted(cmce, 12345u);

    uint8_t mle[9 + 52];
    int mle_nbits;
    wrap_mle_cmce(cmce, 52, mle, &mle_nbits);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();
    st->tetra_vc_freq_hz = 390000000L;

    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    CHECK(st->tetra_tx_granted_ssi == 12345u,
          "TX-GRANTED: granted SSI is retained");
    CHECK(st->tetra_vc_freq_hz == 390000000L,
          "TX-GRANTED: absent channel IE does not overwrite VC frequency");

    free(st); free(opt);
}

static void test_tx_granted_floor_state(void)
{
    printf("[test_tx_granted_floor_state]\n");

    uint8_t cmce[52];
    build_d_tx_granted(cmce, 111u);

    uint8_t mle[9 + 52];
    int mle_nbits;
    wrap_mle_cmce(cmce, 52, mle, &mle_nbits);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    CHECK(st->tetra_tx_granted_valid == 1,
          "TX-GRANTED: floor grant marked valid");
    CHECK(st->tetra_cmce_tx_granted_perm == 1,
          "TX-GRANTED: transmit permission retained");

    free(st); free(opt);
}

/* =======================================================================
 * Phase 46: SDS message reference tracking
 * ======================================================================= */

/*
 * Build a minimal D-SDS-DATA CMCE PDU with an explicit msg_ref.
 *
 *   [0-4]   pdu_type = 23
 *   [5]     ext_flag = 0
 *   [6-29]  calling_ssi (24 bits)
 *   [30-33] msg_ref (4 bits)
 *   [34]    store_fwd = 0
 *   [35]    vp_flag = 0
 *   [36]    dt_flag = 0
 *   [37-44] bpc = 8
 *   [45-52] num_chars = 1
 *   [53-60] 'A' = 65
 * Total: 61 bits
 */
static void build_d_sds_data(uint8_t *cmce, uint32_t src_ssi, uint8_t sdti)
{
    memset(cmce, 0, 61);
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1, 5, 2);          /* CPTI: SSI */
    pack_bits(cmce, src_ssi, 7, 24);
    pack_bits(cmce, sdti, 31, 2);
    if (sdti == 0)
        pack_bits(cmce, 0xCAFE, 33, 16);
}

static void test_sds_msg_ref_stored(void)
{
    printf("[test_sds_sdti_and_cc_stored]\n");
    uint8_t cmce[61]; build_d_sds_data(cmce, 22222u, 0u);
    uint8_t mle[70]; int n; wrap_mle_cmce(cmce,49,mle,&n);
    dsd_state *st=alloc_state(); dsd_opts *opt=alloc_opts();
    tetra_mle_dispatch(mle,n,3,opt,st);
    CHECK(st->tetra_cmce_sds_data_type == 0, "SDS SDTI = 0");
    CHECK(st->tetra_sds_last_cc == 3, "SDS last_cc = 3");
    free(st); free(opt);
}

static void test_sds_last_cc_stored(void)
{
    printf("[test_sds_last_cc_stored]\n");
    uint8_t cmce[61]; build_d_sds_data(cmce,33333u,0u);
    uint8_t mle[70]; int n; wrap_mle_cmce(cmce,49,mle,&n);
    dsd_state *st=alloc_state(); dsd_opts *opt=alloc_opts();
    tetra_mle_dispatch(mle,n,11,opt,st);
    CHECK(st->tetra_sds_last_cc == 11, "SDS last_cc = 11");
    free(st); free(opt);
}

static void test_sds_msg_ref_overwrite(void)
{
    printf("[test_sds_source_overwrite]\n");
    dsd_state *st=alloc_state(); dsd_opts *opt=alloc_opts();
    uint8_t cmce[61],mle[70]; int n;
    build_d_sds_data(cmce,10001u,0u); wrap_mle_cmce(cmce,49,mle,&n);
    tetra_mle_dispatch(mle,n,0,opt,st);
    CHECK(st->tetra_sds_src == 10001u, "first SDS source");
    build_d_sds_data(cmce,10002u,0u); wrap_mle_cmce(cmce,49,mle,&n);
    tetra_mle_dispatch(mle,n,0,opt,st);
    CHECK(st->tetra_sds_src == 10002u, "second SDS source overwrites first");
    free(st); free(opt);
}

/* =======================================================================
 * main
 * ======================================================================= */
int main(void)
{
    printf("=== TETRA Phase 47 Test Suite ===\n\n");

    /* Phase 43 */
    printf("--- Phase 43: TDMA timestamps in BSCH ---\n");
    test_tdma_fields_zero();
    test_bsch_tdma_parse();
    test_bsch_tdma_tn_onebased();
    test_bsch_tdma_overwrite();
    test_tdma_ndb_advance_and_rollover();

    /* Phase 44 */
    printf("\n--- Phase 44: MM D-LU-REJECT + D-TEMPORARY-ADDRESS ---\n");
    test_mm_lu_reject_fields_zero();
    test_mm_d_lu_reject_cause();
    test_mm_d_lu_reject_short();
    test_mm_temp_ssi_fields_zero();
    test_mm_d_temporary_address_ssi();
    test_mm_d_temporary_address_short();

    /* Phase 45 */
    printf("\n--- Phase 45: D-TX-GRANTED UI fields ---\n");
    test_tx_granted_ui_fields_zero();
    test_tx_granted_identity_set();
    test_tx_granted_without_channel_assignment();
    test_tx_granted_floor_state();

    /* Phase 46 */
    printf("\n--- Phase 46: SDS message reference tracking ---\n");
    test_sds_msg_ref_stored();
    test_sds_last_cc_stored();
    test_sds_msg_ref_overwrite();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
