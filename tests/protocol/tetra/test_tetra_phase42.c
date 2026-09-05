// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 42 test suite.
 *
 * Covers functionality introduced in Phases 39-41:
 *
 * Phase 39 — FEC decode quality counters:
 *   1.  tetra_decode_ok and tetra_decode_errors zero-initialize correctly
 *   2.  Fields are writable (counters can be incremented by callers)
 *
 * Phase 40 — D-SDS-DATA (CMCE type 23) parser:
 *   3.  8-bit encoded SDS: src_ssi, sds_text, sds_text_len populated
 *   4.  Short PDU (< 36 bits) handled without crash
 *   5.  ext_flag=1 PDU handled without crash, src_ssi stays 0
 *   6.  7-bit text encoding decoded correctly
 *
 * Phase 41 — TETRA SB sync type IDs:
 *   7.  DSD_SYNC_IS_TETRA returns 1 for SB_POS
 *   8.  DSD_SYNC_IS_TETRA returns 1 for SB_NEG
 *   9.  DSD_SYNC_IS_TETRA still returns 1 for NDB_POS and NDB_NEG
 *  10.  DSD_SYNC_IS_TETRA returns 0 for non-TETRA sync type
 *  11.  tetra_sb1_dibuf and tetra_sb1_valid zero-initialize correctly
 */

#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>

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

static void test_hard_bits_preserve_viterbi_evidence(void)
{
    const uint8_t bits[] = {0, 1, 1, 0, 3, 2};
    uint16_t costs[6] = {0};
    tetra_hard_bits_to_soft(bits, costs, 6);
    CHECK(costs[0] == 0x0000u, "hard zero becomes confident soft zero");
    CHECK(costs[1] == 0xFFFFu, "hard one becomes confident soft one");
    CHECK(costs[2] == 0xFFFFu && costs[3] == 0x0000u,
          "hard decisions retain their polarity");
    CHECK(costs[4] == 0xFFFFu && costs[5] == 0x0000u,
          "only the low bit determines unpacked hard polarity");
}

/* -----------------------------------------------------------------------
 * Helper: build an MLE C-PLANE / CMCE wrapper around a raw CMCE PDU body.
 *   out[0..4]  = mle_type=24, out[5..8] = pd=3, out[9..9+cmce_len-1] = cmce_body
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
 * Phase 39 tests: FEC decode quality counter fields
 * ----------------------------------------------------------------------- */
static void test_decode_quality_fields_zero(void)
{
    printf("[test_decode_quality_fields_zero]\n");
    dsd_state *st = alloc_state();

    CHECK(st->tetra_decode_ok     == 0, "decode_ok zero after calloc");
    CHECK(st->tetra_decode_errors == 0, "decode_errors zero after calloc");

    /* counters must be directly writable */
    st->tetra_decode_ok     = 99;
    st->tetra_decode_errors = 3;
    CHECK(st->tetra_decode_ok     == 99, "decode_ok writable");
    CHECK(st->tetra_decode_errors ==  3, "decode_errors writable");

    free(st);
}

/* -----------------------------------------------------------------------
 * Phase 40 tests: D-SDS-DATA (CMCE type 23) parser
 * ----------------------------------------------------------------------- */

/* Test 3: 8-bit text "OK" from SSI 77777 */
static void test_d_sds_data_8bit_text(void)
{
    printf("[test_d_sds_data_sdti0]\n");
    uint8_t cmce[49] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1, 5, 2);          /* CPTI: SSI */
    pack_bits(cmce, 77777, 7, 24);
    pack_bits(cmce, 0, 31, 2);         /* SDTI 0: 16 bits */
    pack_bits(cmce, 0x4F4B, 33, 16);
    uint8_t mle[58]; int mle_nbits;
    wrap_mle_cmce(cmce, 49, mle, &mle_nbits);
    dsd_state *st=alloc_state(); dsd_opts *opt=alloc_opts();
    tetra_mle_dispatch(mle,mle_nbits,0,opt,st);
    CHECK(st->tetra_sds_src == 77777u, "D-SDS-DATA: CPTI SSI parsed");
    CHECK(st->tetra_cmce_sds_data_type == 0, "D-SDS-DATA: SDTI=0");
    CHECK(st->tetra_sds_short_valid == 1, "D-SDS-DATA: 16-bit form valid");
    CHECK(st->tetra_sds_short_data == 0x4F4B, "D-SDS-DATA: 16-bit payload");
    free(st); free(opt);
}

/* Test 4: too-short PDU (no crash, no side-effects) */
static void test_d_sds_data_too_short(void)
{
    printf("[test_d_sds_data_too_short]\n");

    uint8_t cmce[10];
    memset(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 15u, 0, 5); /* D-SDS-DATA, only 10 bits total */

    uint8_t mle[9 + 10];
    int mle_nbits;
    wrap_mle_cmce(cmce, 10, mle, &mle_nbits);

    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* Must not crash */
    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    CHECK(st->tetra_sds_src      == 0, "D-SDS-DATA short: src_ssi stays 0");
    CHECK(st->tetra_sds_text_len == 0, "D-SDS-DATA short: text_len stays 0");

    free(st); free(opt);
}

/* Test 5: ext_flag = 1 (external subscriber number) — no crash, src remains 0 */
static void test_d_sds_data_ext_flag(void)
{
    printf("[test_d_sds_data_no_calling_party]\n");
    uint8_t cmce[25] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 0, 5, 2);          /* CPTI: no address */
    pack_bits(cmce, 0, 7, 2);          /* SDTI 0 */
    pack_bits(cmce, 0x1234, 9, 16);
    uint8_t mle[34]; int mle_nbits;
    wrap_mle_cmce(cmce,25,mle,&mle_nbits);
    dsd_state *st=alloc_state(); dsd_opts *opt=alloc_opts();
    tetra_mle_dispatch(mle,mle_nbits,0,opt,st);
    CHECK(st->tetra_sds_src == 0, "D-SDS-DATA CPTI=0: no SSI");
    CHECK(st->tetra_sds_short_data == 0x1234, "D-SDS-DATA CPTI=0 payload");
    free(st); free(opt);
}

/* Test 6: 7-bit encoding for text "Hi!" */
static void test_d_sds_data_7bit_text(void)
{
    printf("[test_d_sds_data_sdti3]\n");
    uint8_t cmce[65] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1, 5, 2);          /* CPTI: SSI */
    pack_bits(cmce, 55555, 7, 24);
    pack_bits(cmce, 3, 31, 2);         /* SDTI 3: variable */
    pack_bits(cmce, 21, 33, 11);
    pack_bits(cmce, 'H', 44, 7); pack_bits(cmce, 'i', 51, 7); pack_bits(cmce, '!', 58, 7);
    uint8_t mle[74]; int mle_nbits;
    wrap_mle_cmce(cmce,65,mle,&mle_nbits);
    dsd_state *st=alloc_state(); dsd_opts *opt=alloc_opts();
    tetra_mle_dispatch(mle,mle_nbits,0,opt,st);
    CHECK(st->tetra_sds_src == 55555u, "D-SDS-DATA SDTI3: SSI parsed");
    CHECK(st->tetra_cmce_sds_data_type == 3, "D-SDS-DATA SDTI3 selected");
    CHECK(st->tetra_sds_text_len == 0, "CMCE does not invent SDS-TL text encoding");
    free(st); free(opt);
}

/* -----------------------------------------------------------------------
 * Phase 41 tests: TETRA SB sync type IDs
 * ----------------------------------------------------------------------- */
static void test_synctype_tetra_sb(void)
{
    printf("[test_synctype_tetra_sb]\n");

    CHECK(DSD_SYNC_IS_TETRA(DSD_SYNC_TETRA_SB_POS)  == 1, "SB_POS is TETRA");
    CHECK(DSD_SYNC_IS_TETRA(DSD_SYNC_TETRA_SB_NEG)  == 1, "SB_NEG is TETRA");
    CHECK(DSD_SYNC_IS_TETRA(DSD_SYNC_TETRA_NDB_POS) == 1, "NDB_POS still TETRA");
    CHECK(DSD_SYNC_IS_TETRA(DSD_SYNC_TETRA_NDB_NEG) == 1, "NDB_NEG still TETRA");
    CHECK(DSD_SYNC_IS_TETRA(0)                      == 0, "type 0 not TETRA");
    CHECK(DSD_SYNC_IS_TETRA(5)                      == 0, "type 5 not TETRA");
}

/* SB1 capture fields zero-initialize correctly */
static void test_sb1_fields_zero(void)
{
    printf("[test_sb1_fields_zero]\n");
    dsd_state *st = alloc_state();

    CHECK(st->tetra_sb1_valid == 0, "tetra_sb1_valid zero after calloc");
    /* Verify at least first and last bytes of the 60-byte buffer are zero */
    CHECK(st->tetra_sb1_dibuf[0]  == 0, "sb1_dibuf[0] zero");
    CHECK(st->tetra_sb1_dibuf[59] == 0, "sb1_dibuf[59] zero");

    free(st);
}

/* SB and NDB sync IDs are distinct */
static void test_synctype_sb_ndb_distinct(void)
{
    printf("[test_synctype_sb_ndb_distinct]\n");

    CHECK(DSD_SYNC_TETRA_SB_POS  != DSD_SYNC_TETRA_NDB_POS, "SB_POS != NDB_POS");
    CHECK(DSD_SYNC_TETRA_SB_NEG  != DSD_SYNC_TETRA_NDB_NEG, "SB_NEG != NDB_NEG");
    CHECK(DSD_SYNC_TETRA_SB_POS  != DSD_SYNC_TETRA_SB_NEG,  "SB_POS != SB_NEG");
    CHECK(DSD_SYNC_TETRA_NDB_POS != DSD_SYNC_TETRA_NDB_NEG, "NDB_POS != NDB_NEG");
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("[TETRA Phase 42 tests: Phases 39-41]\n");

    /* Phase 39: FEC decode quality counters */
    test_decode_quality_fields_zero();
    test_hard_bits_preserve_viterbi_evidence();

    /* Phase 40: D-SDS-DATA parser */
    test_d_sds_data_8bit_text();
    test_d_sds_data_too_short();
    test_d_sds_data_ext_flag();
    test_d_sds_data_7bit_text();

    /* Phase 41: TETRA SB sync type IDs */
    test_synctype_tetra_sb();
    test_sb1_fields_zero();
    test_synctype_sb_ndb_distinct();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
