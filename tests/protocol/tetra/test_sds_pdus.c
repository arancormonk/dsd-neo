// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * D-SDS-DATA (EN 300 392-2 table 14.13) through tetra_sds_decode(): the calling party, the short data type identifier
 * and the user data it selects, and the summary line the dispatcher prints. A PDU cut before the end of its user data
 * is incomplete and prints nothing. The SDS-TL, text and concatenation tests are in test_mle_cmce.c.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>

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

/* A CMCE PDU behind its protocol discriminator (EN 300 392-2 table 18.87): the TM-SDU the MLE routes. */
static void
wrap_mle_cmce(const uint8_t* cmce_body, int cmce_nbits, uint8_t* out, int* out_nbits) {
    DSD_MEMSET(out, 0, (size_t)(3 + cmce_nbits));
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = 3 + cmce_nbits;
}

/* 8-bit text "OK" from SSI 77777 */
static void
test_d_sds_data_short_form(void) {
    printf("[test_d_sds_data_short_form]\n");
    uint8_t cmce[50] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1, 5, 2); /* CPTI: SSI */
    pack_bits(cmce, 77777, 7, 24);
    pack_bits(cmce, 0, 31, 2); /* SDTI 0: 16 bits */
    pack_bits(cmce, 0x4F4B, 33, 16);
    pack_bits(cmce, 0, 49, 1); /* O-bit */
    uint8_t mle[58];
    int mle_nbits;
    wrap_mle_cmce(cmce, 50, mle, &mle_nbits);
    tetra_sds_pdu pdu;
    CHECK(tetra_sds_decode(mle, mle_nbits, &pdu) && pdu.data.calling.ssi == 77777u, "D-SDS-DATA: CPTI SSI parsed");
    CHECK(pdu.data.sdti == 0, "D-SDS-DATA: SDTI=0");
    CHECK(pdu.data.data_bits == 16, "D-SDS-DATA: 16-bit form");
    CHECK(pdu.data.short_data == 0x4F4B, "D-SDS-DATA: 16-bit payload");
}

/* too-short PDU (no crash, no side-effects) */
static void
test_d_sds_data_too_short(void) {
    printf("[test_d_sds_data_too_short]\n");

    uint8_t cmce[10];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 15u, 0, 5); /* D-SDS-DATA, only 10 bits total */

    uint8_t mle[9 + 10];
    int mle_nbits;
    wrap_mle_cmce(cmce, 10, mle, &mle_nbits);

    tetra_sds_pdu pdu;
    CHECK(!tetra_sds_decode(mle, mle_nbits, &pdu), "D-SDS-DATA short: incomplete");
}

/* CPTI=0 has no calling-party address. */
static void
test_d_sds_data_no_calling_party(void) {
    printf("[test_d_sds_data_no_calling_party]\n");
    uint8_t cmce[26] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 0, 5, 2); /* CPTI: no address */
    pack_bits(cmce, 0, 7, 2); /* SDTI 0 */
    pack_bits(cmce, 0x1234, 9, 16);
    pack_bits(cmce, 0, 25, 1); /* O-bit */
    uint8_t mle[34];
    int mle_nbits;
    wrap_mle_cmce(cmce, 26, mle, &mle_nbits);
    tetra_sds_pdu pdu;
    CHECK(tetra_sds_decode(mle, mle_nbits, &pdu) && !pdu.data.calling.ssi_valid && pdu.data.calling.ssi == 0,
          "D-SDS-DATA CPTI=0: no SSI");
    CHECK(pdu.data.short_data == 0x1234, "D-SDS-DATA CPTI=0 payload");
}

/* 7-bit encoding for text "Hi!" */
static void
test_d_sds_data_without_protocol_identifier(void) {
    printf("[test_d_sds_data_without_protocol_identifier]\n");
    uint8_t cmce[66] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1, 5, 2); /* CPTI: SSI */
    pack_bits(cmce, 55555, 7, 24);
    pack_bits(cmce, 3, 31, 2); /* SDTI 3: variable */
    pack_bits(cmce, 21, 33, 11);
    pack_bits(cmce, 'H', 44, 7);
    pack_bits(cmce, 'i', 51, 7);
    pack_bits(cmce, '!', 58, 7);
    pack_bits(cmce, 0, 65, 1); /* O-bit */
    uint8_t mle[74];
    int mle_nbits;
    wrap_mle_cmce(cmce, 66, mle, &mle_nbits);
    tetra_sds_pdu pdu;
    CHECK(tetra_sds_decode(mle, mle_nbits, &pdu) && pdu.data.calling.ssi == 55555u, "D-SDS-DATA SDTI3: SSI parsed");
    CHECK(pdu.data.sdti == 3 && pdu.data.data_bits == 21, "D-SDS-DATA SDTI3 selected");
    CHECK(pdu.data.text.len == 0, "CMCE does not invent SDS-TL text encoding");
}

/* D-SDS-DATA SDTI=0 type 0 with 16-bit status */
static void
test_d_sds_data_sdti0_payload(void) {
    printf("[test_d_sds_data_sdti0_payload]\n");

    /* CMCE D-SDS-DATA SDTI=0:
     *   [0-4]   pdu_type = 15
     *   [5]     ext_flag = 0
     *   [6-29]  calling_ssi = 99999 (24 bits)
     *   [30-31] data_type = 0
     *   [32-47] pre-defined status = 0xABCD (16 bits)
     * Total: 48 bits */
    uint8_t cmce[50];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1u, 5, 2);
    pack_bits(cmce, 99999u, 7, 24);
    pack_bits(cmce, 0u, 31, 2);       /* data_type = 0 */
    pack_bits(cmce, 0xABCDu, 33, 16); /* status */
    pack_bits(cmce, 0u, 49, 1);       /* O-bit */

    uint8_t mle[3 + 50];
    int n;
    wrap_mle_cmce(cmce, 50, mle, &n);

    tetra_sds_pdu pdu;
    CHECK(tetra_sds_decode(mle, n, &pdu) && pdu.data.sdti == 0, "D-SDS-DATA SDTI=0 decodes");
    CHECK(pdu.data.short_data == 0xABCDu, "D-SDS-DATA SDTI=0: data=0xABCD");
}

/* D-SDS-DATA SDTI=0 sets sds_short_src */
static void
test_d_sds_data_sdti0_src(void) {
    printf("[test_d_sds_data_sdti0_src]\n");

    uint8_t cmce[50];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1u, 5, 2);
    pack_bits(cmce, 55555u, 7, 24);
    pack_bits(cmce, 0u, 31, 2);
    pack_bits(cmce, 0u, 33, 16);
    pack_bits(cmce, 0u, 49, 1); /* O-bit */

    uint8_t mle[3 + 50];
    int n;
    wrap_mle_cmce(cmce, 50, mle, &n);

    tetra_sds_pdu pdu;
    CHECK(tetra_sds_decode(mle, n, &pdu) && pdu.data.sdti == 0, "D-SDS-DATA SDTI=0 decodes");
    CHECK(pdu.data.calling.ssi_valid && pdu.data.calling.ssi == 55555u, "D-SDS-DATA SDTI=0: src=55555");
}

/* D-SDS-DATA SDTI=0 too short (no crash) */
static void
test_d_sds_data_sdti0_too_short(void) {
    printf("[test_d_sds_data_sdti0_too_short]\n");

    uint8_t cmce[10];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);

    uint8_t mle[9 + 10];
    int n;
    wrap_mle_cmce(cmce, 10, mle, &n);

    tetra_sds_pdu pdu;
    CHECK(!tetra_sds_decode(mle, n, &pdu), "truncated D-SDS-DATA is incomplete");
}

/*
 * Build a minimal D-SDS-DATA CMCE PDU with an explicit msg_ref.
 *
 *   [0-4]   pdu_type = 23
 * The helper builds table 14.13 D-SDS-DATA with CPTI=1 and SDTI=0, including
 * the mandatory Annex E O-bit after the 16-bit user data.
 */
static void
build_d_sds_data(uint8_t* cmce, uint32_t src_ssi, uint8_t sdti) {
    DSD_MEMSET(cmce, 0, 61);
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 1, 5, 2); /* CPTI: SSI */
    pack_bits(cmce, src_ssi, 7, 24);
    pack_bits(cmce, sdti, 31, 2);
    if (sdti == 0) {
        pack_bits(cmce, 0xCAFE, 33, 16);
    }
    if (sdti == 0) {
        pack_bits(cmce, 0, 49, 1); /* O-bit */
    }
}

/* A D-SDS-DATA decodes with its own calling party and data type, and its summary line names the colour code it came
 * with. */
static void
test_sds_decode_and_summary(void) {
    printf("[test_sds_decode_and_summary]\n");
    uint8_t cmce[61];
    build_d_sds_data(cmce, 22222u, 0u);
    uint8_t mle[70];
    int n;
    wrap_mle_cmce(cmce, 50, mle, &n);
    tetra_sds_pdu pdu;
    CHECK(tetra_sds_decode(mle, n, &pdu) && pdu.data.sdti == 0 && pdu.data.calling.ssi == 22222u
              && pdu.data.short_data == 0xCAFE,
          "SDS SDTI = 0");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
    char log[256];
    tetra_test_mle_logged(mle, n, 3, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA CMCE D-SDS-DATA] CC=3 src_SSI=22222 CPTI=1 SDTI=0 data_bits=16") != NULL,
          "SDS summary line with CC = 3");
    free_state(st);
    free(opt);
}

static void
test_sds_summary_colour_code(void) {
    printf("[test_sds_summary_colour_code]\n");
    uint8_t cmce[61];
    build_d_sds_data(cmce, 33333u, 0u);
    uint8_t mle[70];
    int n;
    wrap_mle_cmce(cmce, 50, mle, &n);
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
    char log[256];
    tetra_test_mle_logged(mle, n, 11, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA CMCE D-SDS-DATA] CC=11 src_SSI=33333") != NULL, "SDS summary line with CC = 11");
    free_state(st);
    free(opt);
}

static void
test_sds_each_pdu_its_source(void) {
    printf("[test_sds_each_pdu_its_source]\n");
    uint8_t cmce[61], mle[70];
    int n;
    tetra_sds_pdu pdu;
    build_d_sds_data(cmce, 10001u, 0u);
    wrap_mle_cmce(cmce, 50, mle, &n);
    CHECK(tetra_sds_decode(mle, n, &pdu) && pdu.data.calling.ssi == 10001u, "first SDS source");
    build_d_sds_data(cmce, 10002u, 0u);
    wrap_mle_cmce(cmce, 50, mle, &n);
    CHECK(tetra_sds_decode(mle, n, &pdu) && pdu.data.calling.ssi == 10002u, "second SDS source");
}

static void
test_sds_data_truncation(void) {
    printf("[test_sds_data_truncation]\n");
    /* Truncated D-SDS-DATA payload after the source SSI. */
    uint8_t cmce[40];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
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

int
main(void) {
    test_d_sds_data_short_form();
    test_d_sds_data_too_short();
    test_d_sds_data_no_calling_party();
    test_d_sds_data_without_protocol_identifier();
    test_d_sds_data_sdti0_payload();
    test_d_sds_data_sdti0_src();
    test_d_sds_data_sdti0_too_short();
    test_sds_decode_and_summary();
    test_sds_summary_colour_code();
    test_sds_each_pdu_its_source();
    test_sds_data_truncation();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
