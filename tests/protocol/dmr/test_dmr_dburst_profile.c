// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/dmr/r34_viterbi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

FILE*
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_fopen_private(const char* path, const char* mode) {
    (void)path;
    (void)mode;
    return NULL;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
pack_bit_array_into_byte_array(const uint8_t* bits, uint8_t* bytes, int len) {
    if (len <= 0) {
        return;
    }
    const size_t byte_count = (size_t)len;
    DSD_MEMSET(bytes, 0, byte_count * sizeof(uint8_t));
    for (size_t byte = 0U; byte < byte_count; byte++) {
        for (size_t bit = 0U; bit < 8U; bit++) {
            bytes[byte] = (uint8_t)((bytes[byte] << 1U) | (bits[(byte * 8U) + bit] & 1U));
        }
    }
}

enum {
    TEST_DBURST_F_BPTC = 1U << 0U,
    TEST_DBURST_F_TRELLIS = 1U << 1U,
    TEST_DBURST_F_LC = 1U << 3U,
    TEST_DBURST_F_FULL = 1U << 4U,
    TEST_DBURST_F_UDT = 0x80U,
};

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ConvertBitIntoBytes(const uint8_t* bits, uint32_t n) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < n; i++) {
        value = (value << 1U) | (uint64_t)(bits[i] & 1U);
    }
    return value;
}

uint16_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ComputeCrc9Bit(const uint8_t* bits, uint32_t len) {
    (void)bits;
    (void)len;
    return 0;
}

uint16_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ComputeCrcCCITT(const uint8_t* bits) {
    (void)bits;
    return 0;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ComputeCrc5Bit(const uint8_t* bits) {
    (void)bits;
    return 0;
}

uint32_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ComputeAndCorrectFullLinkControlCrc(uint8_t* bytes, uint32_t* computed, uint32_t mask) {
    (void)bytes;
    (void)mask;
    if (computed != NULL) {
        *computed = 0;
    }
    return 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
BPTCDeInterleaveDMRData(const uint8_t* input, uint8_t* output) {
    (void)input;
    if (output != NULL) {
        DSD_MEMSET(output, 0, 196U);
    }
}

uint32_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
BPTC_196x96_Extract_Data(uint8_t input[196], uint8_t output[96], uint8_t reserved[3]) {
    (void)input;
    if (output != NULL) {
        DSD_MEMSET(output, 0, 96U);
    }
    if (reserved != NULL) {
        DSD_MEMSET(reserved, 0, 3U);
    }
    return 0;
}

uint32_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
BPTC_128x77_Extract_Data(uint8_t input[8][16], uint8_t output[77]) {
    (void)input;
    if (output != NULL) {
        DSD_MEMSET(output, 0, 77U);
    }
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_r34_viterbi_decode(const uint8_t* dibits, uint8_t bytes18[18]) {
    (void)dibits;
    DSD_MEMSET(bytes18, 0, 18U);
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_r34_viterbi_decode_soft(const uint8_t* dibits, const uint8_t* reliab, uint8_t bytes18[18]) {
    (void)dibits;
    (void)reliab;
    DSD_MEMSET(bytes18, 0, 18U);
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_r34_viterbi_decode_list(const uint8_t* dibits, const uint8_t* reliab, dmr_r34_candidate* out, int max_candidates,
                            int* out_count) {
    (void)dibits;
    (void)reliab;
    (void)out;
    (void)max_candidates;
    if (out_count != NULL) {
        *out_count = 0;
    }
    return -1;
}

uint32_t
dmr_34(const uint8_t* input, uint8_t treturn[18]) {
    (void)input;
    DSD_MEMSET(treturn, 0, 18U);
    return 0;
}

void
dmr_pi(dsd_opts* opts, dsd_state* state, uint8_t PI_BYTE[], uint32_t CRCCorrect, uint32_t IrrecoverableErrors) {
    (void)opts;
    (void)state;
    (void)PI_BYTE;
    (void)CRCCorrect;
    (void)IrrecoverableErrors;
}

void
dmr_flco(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[], uint32_t CRCCorrect, uint32_t* IrrecoverableErrors,
         uint8_t type) {
    (void)opts;
    (void)state;
    (void)lc_bits;
    (void)CRCCorrect;
    (void)IrrecoverableErrors;
    (void)type;
}

void
dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t CRCCorrect,
          uint32_t IrrecoverableErrors) {
    (void)opts;
    (void)state;
    (void)cs_pdu_bits;
    (void)cs_pdu;
    (void)CRCCorrect;
    (void)IrrecoverableErrors;
}

void
dmr_dheader(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t dheader_bits[], uint32_t CRCCorrect,
            uint32_t IrrecoverableErrors) {
    (void)opts;
    (void)state;
    (void)dheader;
    (void)dheader_bits;
    (void)CRCCorrect;
    (void)IrrecoverableErrors;
}

void
dmr_block_assembler(dsd_opts* opts, dsd_state* state, uint8_t block_bytes[], uint8_t block_len, uint8_t databurst,
                    uint8_t type) {
    (void)opts;
    (void)state;
    (void)block_bytes;
    (void)block_len;
    (void)databurst;
    (void)type;
}

void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
lip_protocol_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)lc_bits;
}

static int
expect_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_u32(const char* tag, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%08X want 0x%08X\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
capture_profile(uint8_t databurst, uint8_t conf_data, uint8_t header_format, uint8_t* pdu_len, uint8_t* pdu_start,
                uint8_t* crclen, uint32_t* crcmask, uint8_t* flags, char subtype[8], uint8_t* data_p_head) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_neo_dmr_test_dburst_profile_result result;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1;
    state.data_p_head[1] = 1;
    state.data_conf_data[1] = conf_data;
    state.data_header_format[1] = header_format;
    if (!dsd_neo_dmr_test_dburst_profile(&opts, &state, databurst, 1, &result)) {
        return 0;
    }

    *pdu_len = result.pdu_len;
    *pdu_start = result.pdu_start;
    *crclen = result.crclen;
    *crcmask = result.crcmask;
    *flags = result.flags;
    DSD_SNPRINTF(subtype, 8U, "%s", result.subtype);
    *data_p_head = result.data_p_head;
    return 1;
}

static int
test_base_profiles(void) {
    int rc = 0;
    uint8_t pdu_len = 0;
    uint8_t pdu_start = 0xFFU;
    uint8_t crclen = 0;
    uint32_t crcmask = 0;
    uint8_t flags = 0;
    uint8_t data_p_head = 0;
    char subtype[8];
    DSD_MEMSET(subtype, 0, sizeof(subtype));

    rc |= expect_u8(
        "pi-profile-ok",
        (uint8_t)capture_profile(0x00U, 0U, 1U, &pdu_len, &pdu_start, &crclen, &crcmask, &flags, subtype, &data_p_head),
        1U);
    rc |= expect_str("pi-subtype", subtype, " PI  ");
    rc |= expect_u8("pi-pdu-len", pdu_len, 12U);
    rc |= expect_u8("pi-pdu-start", pdu_start, 0U);
    rc |= expect_u8("pi-crclen", crclen, 16U);
    rc |= expect_u32("pi-crcmask", crcmask, 0x6969U);
    rc |= expect_u8("pi-flags", flags, TEST_DBURST_F_BPTC);
    rc |= expect_u8("pi-clears-data-p-head", data_p_head, 0U);

    rc |= expect_u8(
        "vlc-profile-ok",
        (uint8_t)capture_profile(0x01U, 0U, 1U, &pdu_len, &pdu_start, &crclen, &crcmask, &flags, subtype, &data_p_head),
        1U);
    rc |= expect_str("vlc-subtype", subtype, " VLC ");
    rc |= expect_u8("vlc-flags", flags, TEST_DBURST_F_BPTC | TEST_DBURST_F_LC);
    rc |= expect_u8("vlc-crclen", crclen, 24U);
    rc |= expect_u32("vlc-crcmask", crcmask, 0x969696U);

    rc |= expect_u8(
        "r34-profile-ok",
        (uint8_t)capture_profile(0x08U, 0U, 1U, &pdu_len, &pdu_start, &crclen, &crcmask, &flags, subtype, &data_p_head),
        1U);
    rc |= expect_str("r34-subtype", subtype, " R34U ");
    rc |= expect_u8("r34-pdu-len", pdu_len, 18U);
    rc |= expect_u8("r34-flags", flags, TEST_DBURST_F_TRELLIS);
    rc |= expect_u8("r34-keeps-data-p-head", data_p_head, 1U);
    return rc;
}

static int
test_dynamic_profiles(void) {
    int rc = 0;
    uint8_t pdu_len = 0;
    uint8_t pdu_start = 0;
    uint8_t crclen = 0;
    uint32_t crcmask = 0;
    uint8_t flags = 0;
    uint8_t data_p_head = 0;
    char subtype[8];
    DSD_MEMSET(subtype, 0, sizeof(subtype));

    rc |= expect_u8(
        "r12c-profile-ok",
        (uint8_t)capture_profile(0x07U, 1U, 1U, &pdu_len, &pdu_start, &crclen, &crcmask, &flags, subtype, &data_p_head),
        1U);
    rc |= expect_str("r12c-subtype", subtype, " R12C ");
    rc |= expect_u8("r12c-pdu-len", pdu_len, 10U);
    rc |= expect_u8("r12c-pdu-start", pdu_start, 2U);
    rc |= expect_u8("r12c-crclen", crclen, 9U);
    rc |= expect_u8("r12c-flags", flags, TEST_DBURST_F_BPTC);

    rc |= expect_u8(
        "udtc-profile-ok",
        (uint8_t)capture_profile(0x07U, 1U, 0U, &pdu_len, &pdu_start, &crclen, &crcmask, &flags, subtype, &data_p_head),
        1U);
    rc |= expect_str("udtc-subtype", subtype, " UDTC ");
    rc |= expect_u8("udtc-pdu-len", pdu_len, 10U);
    rc |= expect_u8("udtc-flags", flags, TEST_DBURST_F_BPTC | TEST_DBURST_F_UDT);

    rc |= expect_u8(
        "r34c-profile-ok",
        (uint8_t)capture_profile(0x08U, 1U, 1U, &pdu_len, &pdu_start, &crclen, &crcmask, &flags, subtype, &data_p_head),
        1U);
    rc |= expect_str("r34c-subtype", subtype, " R34C ");
    rc |= expect_u8("r34c-pdu-len", pdu_len, 16U);
    rc |= expect_u8("r34c-pdu-start", pdu_start, 2U);

    rc |= expect_u8(
        "full-confirmed-profile-ok",
        (uint8_t)capture_profile(0x0AU, 1U, 1U, &pdu_len, &pdu_start, &crclen, &crcmask, &flags, subtype, &data_p_head),
        1U);
    rc |= expect_str("full-confirmed-subtype", subtype, " R_1C ");
    rc |= expect_u8("full-confirmed-pdu-len", pdu_len, 22U);
    rc |= expect_u8("full-confirmed-pdu-start", pdu_start, 2U);
    rc |= expect_u8("full-confirmed-flags", flags, TEST_DBURST_F_FULL);
    return rc;
}

static int
test_special_profiles(void) {
    int rc = 0;
    uint8_t pdu_len = 0;
    uint8_t pdu_start = 0;
    uint8_t crclen = 0;
    uint32_t crcmask = 0;
    uint8_t flags = 0;
    uint8_t data_p_head = 0;
    char subtype[8];
    DSD_MEMSET(subtype, 0, sizeof(subtype));

    rc |= expect_u8(
        "embedded-profile-ok",
        (uint8_t)capture_profile(0xEBU, 0U, 1U, &pdu_len, &pdu_start, &crclen, &crcmask, &flags, subtype, &data_p_head),
        1U);
    rc |= expect_u8("embedded-pdu-len", pdu_len, 9U);
    rc |= expect_u8("embedded-crclen", crclen, 5U);
    rc |= expect_u8("embedded-clears-data-p-head", data_p_head, 0U);

    rc |= expect_u8(
        "unknown-profile-ok",
        (uint8_t)capture_profile(0x40U, 0U, 1U, &pdu_len, &pdu_start, &crclen, &crcmask, &flags, subtype, &data_p_head),
        1U);
    rc |= expect_str("unknown-subtype", subtype, " _UNK ");
    rc |= expect_u8("unknown-pdu-len", pdu_len, 25U);
    rc |= expect_u8("unknown-flags", flags, TEST_DBURST_F_FULL);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_base_profiles();
    rc |= test_dynamic_profiles();
    rc |= test_special_profiles();
    if (rc == 0) {
        printf("DMR_DBURST_PROFILE: OK\n");
    }
    return rc;
}
