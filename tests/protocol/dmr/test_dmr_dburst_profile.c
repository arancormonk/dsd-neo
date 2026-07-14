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
#include "dmr_dburst_profile.h"
#include "dsd-neo/core/safe_api.h"

static int g_pi_calls;
static int g_flco_calls;
static uint8_t g_flco_last_type;
static uint32_t g_flco_last_crc_correct;
static int g_cspdu_calls;
static uint32_t g_cspdu_last_crc_correct;
static int g_dheader_calls;
static uint32_t g_dheader_last_crc_correct;
static int g_block_assembler_calls;
static uint8_t g_block_assembler_last_len;
static uint8_t g_block_assembler_last_burst;
static uint8_t g_block_assembler_last_type;
static uint8_t g_block_assembler_last_bytes[25];
static int g_lip_calls;
static int g_reset_blocks_calls;
static uint32_t g_bptc_extract_errors;
static uint8_t g_bptc_reserved[3];
static uint8_t g_bptc_bits[96];
static int g_r34_soft_status;
static int g_r34_hard_status;
static int g_r34_list_status;
static int g_r34_list_count;
static uint8_t g_r34_soft_bytes[18];
static uint8_t g_r34_hard_bytes[18];
static dmr_r34_candidate g_r34_candidates[4];

static void
reset_handler_counters(void) {
    g_pi_calls = 0;
    g_flco_calls = 0;
    g_flco_last_type = 0;
    g_flco_last_crc_correct = 0;
    g_cspdu_calls = 0;
    g_cspdu_last_crc_correct = 0;
    g_dheader_calls = 0;
    g_dheader_last_crc_correct = 0;
    g_block_assembler_calls = 0;
    g_block_assembler_last_len = 0;
    g_block_assembler_last_burst = 0;
    g_block_assembler_last_type = 0;
    DSD_MEMSET(g_block_assembler_last_bytes, 0, sizeof(g_block_assembler_last_bytes));
    g_lip_calls = 0;
    g_reset_blocks_calls = 0;
    g_bptc_extract_errors = 0;
    DSD_MEMSET(g_bptc_reserved, 0, sizeof(g_bptc_reserved));
    DSD_MEMSET(g_bptc_bits, 0, sizeof(g_bptc_bits));
    g_r34_soft_status = 0;
    g_r34_hard_status = 0;
    g_r34_list_status = -1;
    g_r34_list_count = 0;
    DSD_MEMSET(g_r34_soft_bytes, 0, sizeof(g_r34_soft_bytes));
    DSD_MEMSET(g_r34_hard_bytes, 0, sizeof(g_r34_hard_bytes));
    DSD_MEMSET(g_r34_candidates, 0, sizeof(g_r34_candidates));
}

FILE*
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_fopen_private(const char* path, const char* mode) {
    (void)path;
    (void)mode;
    return NULL;
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
        DSD_MEMCPY(output, g_bptc_bits, 96U);
    }
    if (reserved != NULL) {
        DSD_MEMCPY(reserved, g_bptc_reserved, 3U);
    }
    return g_bptc_extract_errors;
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
    DSD_MEMCPY(bytes18, g_r34_hard_bytes, 18U);
    return g_r34_hard_status;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_r34_viterbi_decode_soft(const uint8_t* dibits, const uint8_t* reliab, uint8_t bytes18[18]) {
    (void)dibits;
    (void)reliab;
    DSD_MEMCPY(bytes18, g_r34_soft_bytes, 18U);
    return g_r34_soft_status;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_r34_viterbi_decode_list(const uint8_t* dibits, const uint8_t* reliab, dmr_r34_candidate* out, int max_candidates,
                            int* out_count) {
    (void)dibits;
    (void)reliab;
    if (out != NULL && max_candidates > 0) {
        int count = g_r34_list_count;
        if (count > max_candidates) {
            count = max_candidates;
        }
        DSD_MEMCPY(out, g_r34_candidates, (size_t)count * sizeof(out[0]));
    }
    if (out_count != NULL) {
        *out_count = g_r34_list_count;
    }
    return g_r34_list_status;
}

void
dmr_pi(dsd_opts* opts, dsd_state* state, uint8_t PI_BYTE[], uint32_t CRCCorrect, uint32_t IrrecoverableErrors) {
    (void)opts;
    (void)state;
    (void)PI_BYTE;
    (void)CRCCorrect;
    (void)IrrecoverableErrors;
    g_pi_calls++;
}

void
dmr_flco(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[], uint32_t CRCCorrect, uint32_t* IrrecoverableErrors,
         uint8_t type) {
    (void)opts;
    (void)state;
    (void)lc_bits;
    (void)IrrecoverableErrors;
    g_flco_calls++;
    g_flco_last_type = type;
    g_flco_last_crc_correct = CRCCorrect;
}

void
dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t CRCCorrect,
          uint32_t IrrecoverableErrors) {
    (void)opts;
    (void)state;
    (void)cs_pdu_bits;
    (void)cs_pdu;
    (void)IrrecoverableErrors;
    g_cspdu_calls++;
    g_cspdu_last_crc_correct = CRCCorrect;
}

void
dmr_dheader(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t dheader_bits[], uint32_t CRCCorrect,
            uint32_t IrrecoverableErrors) {
    (void)opts;
    (void)state;
    (void)dheader;
    (void)dheader_bits;
    (void)IrrecoverableErrors;
    g_dheader_calls++;
    g_dheader_last_crc_correct = CRCCorrect;
}

void
dmr_block_assembler(dsd_opts* opts, dsd_state* state, uint8_t block_bytes[], uint8_t block_len, uint8_t databurst,
                    uint8_t type) {
    (void)opts;
    (void)state;
    g_block_assembler_calls++;
    g_block_assembler_last_len = block_len;
    g_block_assembler_last_burst = databurst;
    g_block_assembler_last_type = type;
    if (block_bytes != NULL) {
        const size_t copy_len =
            block_len < sizeof(g_block_assembler_last_bytes) ? block_len : sizeof(g_block_assembler_last_bytes);
        DSD_MEMCPY(g_block_assembler_last_bytes, block_bytes, copy_len);
    }
}

void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_reset_blocks_calls++;
}

void
lip_protocol_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)lc_bits;
    g_lip_calls++;
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

static void
write_bits_u32(uint8_t* bits, size_t start, size_t count, uint32_t value) {
    for (size_t i = 0; i < count; i++) {
        const size_t shift = count - 1U - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
write_byte_bits(uint8_t* bits, size_t start, uint8_t value) {
    write_bits_u32(bits, start, 8U, value);
}

static void
write_confirmed_crc9_payload(uint8_t* bits, uint8_t dbsn, uint16_t crc_mask) {
    write_bits_u32(bits, 0U, 7U, dbsn);
    write_bits_u32(bits, 7U, 9U, crc_mask);
}

static void
write_confirmed_crc9_bytes(uint8_t* bytes, uint8_t dbsn, uint16_t crc_mask) {
    uint8_t bits[144];
    DSD_MEMSET(bits, 0, sizeof(bits));
    write_confirmed_crc9_payload(bits, dbsn, crc_mask);
    pack_bit_array_into_byte_array(bits, bytes, 18);
}

static void
prepare_handler_state(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst, uint8_t conf_data,
                      uint8_t header_format, int audio_in_type) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(info, 0, 196U);
    reset_handler_counters();
    opts->audio_in_type = audio_in_type;
    state->currentslot = 1;
    state->dmr_color_code = 16;
    state->data_p_head[1] = 1;
    state->data_conf_data[1] = conf_data;
    state->data_header_format[1] = header_format;
    if (databurst == 0x0BU) {
        info[4] = 0;
    }
}

typedef struct {
    uint8_t databurst;
    uint8_t confirmed;
    uint8_t header_format;
    const char* subtype;
    uint32_t crcmask;
    uint8_t flags;
    uint8_t crclen;
    uint8_t pdu_len;
    uint8_t pdu_start;
} expected_profile;

static int
test_profile_contracts(void) {
    static const expected_profile expected[] = {
        {0x00U, 0U, 1U, " PI  ", 0x6969U, DMR_DBURST_F_BPTC, 16U, 12U, 0U},
        {0x01U, 0U, 1U, " VLC ", 0x969696U, DMR_DBURST_F_BPTC | DMR_DBURST_F_LC, 24U, 12U, 0U},
        {0x02U, 0U, 1U, " TLC ", 0x999999U, DMR_DBURST_F_BPTC | DMR_DBURST_F_LC, 24U, 12U, 0U},
        {0x03U, 0U, 1U, " CSBK ", 0xA5A5U, DMR_DBURST_F_BPTC, 16U, 12U, 0U},
        {0x04U, 0U, 1U, " MBCH ", 0xAAAAU, DMR_DBURST_F_BPTC, 16U, 12U, 0U},
        {0x05U, 0U, 1U, " MBCC ", 0U, DMR_DBURST_F_BPTC, 0U, 12U, 0U},
        {0x06U, 0U, 1U, " DATA ", 0xCCCCU, DMR_DBURST_F_BPTC, 16U, 12U, 0U},
        {0x07U, 0U, 1U, " R12U ", 0x0F0U, DMR_DBURST_F_BPTC, 9U, 12U, 0U},
        {0x07U, 1U, 1U, " R12C ", 0x0F0U, DMR_DBURST_F_BPTC, 9U, 10U, 2U},
        {0x07U, 0U, 0U, " UDTU ", 0x0F0U, DMR_DBURST_F_BPTC | DMR_DBURST_F_UDT, 9U, 12U, 0U},
        {0x07U, 1U, 0U, " UDTC ", 0x0F0U, DMR_DBURST_F_BPTC | DMR_DBURST_F_UDT, 9U, 10U, 2U},
        {0x08U, 0U, 1U, " R34U ", 0x1FFU, DMR_DBURST_F_TRELLIS, 9U, 18U, 0U},
        {0x08U, 1U, 1U, " R34C ", 0x1FFU, DMR_DBURST_F_TRELLIS, 9U, 16U, 2U},
        {0x09U, 0U, 1U, " IDLE ", 0U, 0U, 0U, 0U, 0U},
        {0x0AU, 0U, 1U, " R_1U ", 0x10FU, DMR_DBURST_F_FULL, 9U, 24U, 0U},
        {0x0AU, 1U, 1U, " R_1C ", 0x10FU, DMR_DBURST_F_FULL, 9U, 22U, 2U},
        {0x0BU, 0U, 1U, " USBD ", 0x3333U, DMR_DBURST_F_BPTC, 16U, 12U, 0U},
        {0xEBU, 0U, 1U, NULL, 0U, DMR_DBURST_F_EMB, 5U, 9U, 0U},
        {0x40U, 0U, 1U, " _UNK ", 0U, DMR_DBURST_F_FULL, 0U, 25U, 0U},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const expected_profile* want = &expected[i];
        dmr_dburst_profile got;
        dmr_dburst_profile_resolve(want->databurst, want->confirmed, want->header_format, &got);
        char tag[64];
        DSD_SNPRINTF(tag, sizeof(tag), "profile[%zu] flags", i);
        rc |= expect_u8(tag, got.flags, want->flags);
        DSD_SNPRINTF(tag, sizeof(tag), "profile[%zu] crc length", i);
        rc |= expect_u8(tag, got.crclen, want->crclen);
        DSD_SNPRINTF(tag, sizeof(tag), "profile[%zu] crc mask", i);
        rc |= expect_u32(tag, got.crcmask, want->crcmask);
        DSD_SNPRINTF(tag, sizeof(tag), "profile[%zu] PDU length", i);
        rc |= expect_u8(tag, got.pdu_len, want->pdu_len);
        DSD_SNPRINTF(tag, sizeof(tag), "profile[%zu] PDU start", i);
        rc |= expect_u8(tag, got.pdu_start, want->pdu_start);
        if ((got.subtype == NULL) != (want->subtype == NULL)
            || (got.subtype != NULL && strcmp(got.subtype, want->subtype) != 0)) {
            DSD_FPRINTF(stderr, "profile[%zu] subtype: got '%s' want '%s'\n", i, got.subtype ? got.subtype : "(null)",
                        want->subtype ? want->subtype : "(null)");
            rc = 1;
        }
    }
    return rc;
}

static int
test_handler_dispatch_paths(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t info[196];

    prepare_handler_state(&opts, &state, info, 0x03U, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x03U, NULL);
    rc |= expect_u8("csbk-call", (uint8_t)g_cspdu_calls, 1U);
    rc |= expect_u8("csbk-clears-head", state.data_p_head[1], 0U);
    rc |= expect_u8("csbk-crc-fails-with-stub-mask", (uint8_t)g_cspdu_last_crc_correct, 0U);
    rc |= expect_str("csbk-subtype", state.fsubtype, " CSBK ");

    prepare_handler_state(&opts, &state, info, 0x04U, 0U, 1U, 0);
    state.data_block_counter[1] = 7;
    dmr_data_burst_handler(&opts, &state, info, 0x04U, NULL);
    rc |= expect_u8("mbch-block-call", (uint8_t)g_block_assembler_calls, 1U);
    rc |= expect_u8("mbch-block-type", g_block_assembler_last_type, 2U);
    rc |= expect_u8("mbch-header-valid", state.data_header_valid[1], 1U);
    rc |= expect_u8("mbch-counter-reset", state.data_block_counter[1], 0U);

    prepare_handler_state(&opts, &state, info, 0x06U, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x06U, NULL);
    rc |= expect_u8("data-header-call", (uint8_t)g_dheader_calls, 1U);
    rc |= expect_u8("data-header-keeps-head", state.data_p_head[1], 1U);
    rc |= expect_u8("data-header-crc-fails-with-stub-mask", (uint8_t)g_dheader_last_crc_correct, 0U);

    prepare_handler_state(&opts, &state, info, 0x07U, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x07U, NULL);
    rc |= expect_u8("r12u-block-call", (uint8_t)g_block_assembler_calls, 1U);
    rc |= expect_u8("r12u-block-type", g_block_assembler_last_type, 1U);
    rc |= expect_u8("r12u-block-len", g_block_assembler_last_len, 12U);

    prepare_handler_state(&opts, &state, info, 0x07U, 1U, 0U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x07U, NULL);
    rc |= expect_u8("udtc-block-call", (uint8_t)g_block_assembler_calls, 1U);
    rc |= expect_u8("udtc-block-type", g_block_assembler_last_type, 3U);
    rc |= expect_u8("udtc-block-len", g_block_assembler_last_len, 10U);

    prepare_handler_state(&opts, &state, info, 0x08U, 0U, 1U, AUDIO_IN_SYMBOL_BIN);
    g_r34_hard_bytes[0] = 0x42U;
    g_r34_soft_bytes[0] = 0x99U;
    dmr_data_burst_handler(&opts, &state, info, 0x08U, NULL);
    rc |= expect_u8("r34u-block-call", (uint8_t)g_block_assembler_calls, 1U);
    rc |= expect_u8("r34u-block-burst", g_block_assembler_last_burst, 0x08U);
    rc |= expect_u8("r34u-block-len", g_block_assembler_last_len, 18U);
    rc |= expect_u8("r34u-symbol-replay-uses-canonical-hard-decoder", g_block_assembler_last_bytes[0], 0x42U);

    prepare_handler_state(&opts, &state, info, 0x0AU, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x0AU, NULL);
    rc |= expect_u8("full-rate-block-call", (uint8_t)g_block_assembler_calls, 1U);
    rc |= expect_u8("full-rate-block-burst", g_block_assembler_last_burst, 0x0AU);
    rc |= expect_u8("full-rate-block-len", g_block_assembler_last_len, 24U);

    prepare_handler_state(&opts, &state, info, 0x0BU, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x0BU, NULL);
    rc |= expect_u8("usbd-lip-call", (uint8_t)g_lip_calls, 1U);
    rc |= expect_str("usbd-subtype", state.fsubtype, " USBD ");

    prepare_handler_state(&opts, &state, info, 0xEBU, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0xEBU, NULL);
    rc |= expect_u8("embedded-flco-call", (uint8_t)g_flco_calls, 1U);
    rc |= expect_u8("embedded-flco-type", g_flco_last_type, 3U);
    rc |= expect_u8("embedded-crc-correct", (uint8_t)g_flco_last_crc_correct, 1U);
    rc |= expect_u8("embedded-clears-head", state.data_p_head[1], 0U);

    return rc;
}

static int
test_handler_additional_dispatch_paths(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t info[196];

    prepare_handler_state(&opts, &state, info, 0x00U, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x00U, NULL);
    rc |= expect_u8("pi-call", (uint8_t)g_pi_calls, 1U);
    rc |= expect_u8("pi-clears-head", state.data_p_head[1], 0U);

    prepare_handler_state(&opts, &state, info, 0x01U, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x01U, NULL);
    rc |= expect_u8("vlc-flco-call", (uint8_t)g_flco_calls, 1U);
    rc |= expect_u8("vlc-flco-type", g_flco_last_type, 1U);
    rc |= expect_u8("vlc-crc-correct", (uint8_t)g_flco_last_crc_correct, 1U);

    prepare_handler_state(&opts, &state, info, 0x02U, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x02U, NULL);
    rc |= expect_u8("tlc-flco-call", (uint8_t)g_flco_calls, 1U);
    rc |= expect_u8("tlc-flco-type", g_flco_last_type, 2U);

    prepare_handler_state(&opts, &state, info, 0x05U, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x05U, NULL);
    rc |= expect_u8("mbcc-block-call", (uint8_t)g_block_assembler_calls, 1U);
    rc |= expect_u8("mbcc-block-type", g_block_assembler_last_type, 2U);
    rc |= expect_u8("mbcc-block-len", g_block_assembler_last_len, 12U);

    prepare_handler_state(&opts, &state, info, 0x40U, 0U, 1U, 0);
    dmr_data_burst_handler(&opts, &state, info, 0x40U, NULL);
    rc |= expect_str("unknown-handler-subtype", state.fsubtype, " _UNK ");
    rc |= expect_u8("unknown-handler-no-assembler", (uint8_t)g_block_assembler_calls, 0U);
    return rc;
}

static int
test_bptc_ras_and_usbd_services(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t info[196];

    prepare_handler_state(&opts, &state, info, 0x06U, 0U, 1U, 0);
    g_bptc_reserved[2] = 1U;
    dmr_data_burst_handler(&opts, &state, info, 0x06U, NULL);
    rc |= expect_u8("ras-data-header-call", (uint8_t)g_dheader_calls, 1U);
    rc |= expect_u8("ras-crc-presented-correct", (uint8_t)g_dheader_last_crc_correct, 1U);
    rc |= expect_u8("ras-header-valid", state.data_block_crc_valid[1][0], 1U);

    prepare_handler_state(&opts, &state, info, 0x06U, 0U, 1U, 0);
    g_bptc_reserved[2] = 1U;
    write_byte_bits(g_bptc_bits, 8U, 0x68U);
    dmr_data_burst_handler(&opts, &state, info, 0x06U, NULL);
    rc |= expect_u8("ras-disabled-by-proprietary-header-call", (uint8_t)g_dheader_calls, 1U);
    rc |= expect_u8("ras-disabled-crc-stays-failed", (uint8_t)g_dheader_last_crc_correct, 0U);

    prepare_handler_state(&opts, &state, info, 0x0BU, 0U, 1U, 0);
    write_byte_bits(g_bptc_bits, 0U, 0x90U);
    dmr_data_burst_handler(&opts, &state, info, 0x0BU, NULL);
    rc |= expect_u8("usbd-reserved-service-no-lip", (uint8_t)g_lip_calls, 0U);
    rc |= expect_str("usbd-reserved-service-subtype", state.fsubtype, " USBD ");
    return rc;
}

static int
test_confirmed_sequence_paths(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t info[196];

    prepare_handler_state(&opts, &state, info, 0x07U, 1U, 1U, 0);
    state.data_block_counter[1] = 4U;
    write_confirmed_crc9_payload(g_bptc_bits, 12U, 0U);
    dmr_data_burst_handler(&opts, &state, info, 0x07U, NULL);
    rc |= expect_u8("r12c-crc-records-failure", state.data_block_crc_valid[1][4], 0U);
    rc |= expect_u8("r12c-non-aggressive-seeds-dbsn", state.data_dbsn_have[1], 1U);
    rc |= expect_u8("r12c-expected-dbsn", state.data_dbsn_expected[1], 13U);
    rc |= expect_u8("r12c-dispatched-after-seed", (uint8_t)g_block_assembler_calls, 1U);

    prepare_handler_state(&opts, &state, info, 0x0AU, 1U, 1U, 0);
    opts.aggressive_framesync = 1;
    state.data_dbsn_have[1] = 1U;
    state.data_dbsn_expected[1] = 7U;
    state.data_block_counter[1] = 5U;
    write_confirmed_crc9_payload(info, 5U, 0x10FU);
    dmr_data_burst_handler(&opts, &state, info, 0x0AU, NULL);
    rc |= expect_u8("full-confirmed-crc-records-pass", state.data_block_crc_valid[1][5], 1U);
    rc |= expect_u8("full-confirmed-seq-reset", (uint8_t)g_reset_blocks_calls, 1U);
    rc |= expect_u8("full-confirmed-seq-suppresses-dispatch", (uint8_t)g_block_assembler_calls, 0U);
    return rc;
}

static int
test_trellis_candidate_and_fallback_paths(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    uint8_t info[196];
    uint8_t reliab[98];

    prepare_handler_state(&opts, &state, info, 0x08U, 1U, 1U, 0);
    state.data_dbsn_have[1] = 1U;
    state.data_dbsn_expected[1] = 9U;
    state.data_block_counter[1] = 6U;
    DSD_MEMSET(reliab, 7, sizeof(reliab));
    g_r34_list_status = 0;
    g_r34_list_count = 2;
    write_confirmed_crc9_bytes(g_r34_candidates[0].bytes18, 3U, 0U);
    write_confirmed_crc9_bytes(g_r34_candidates[1].bytes18, 9U, 0x1FFU);
    dmr_data_burst_handler(&opts, &state, info, 0x08U, reliab);
    rc |= expect_u8("r34c-list-crc-records-pass", state.data_block_crc_valid[1][6], 1U);
    rc |= expect_u8("r34c-list-updates-dbsn", state.data_dbsn_expected[1], 10U);
    rc |= expect_u8("r34c-list-dispatches", (uint8_t)g_block_assembler_calls, 1U);
    rc |= expect_u8("r34c-list-block-len", g_block_assembler_last_len, 16U);

    prepare_handler_state(&opts, &state, info, 0x08U, 0U, 1U, 0);
    DSD_MEMSET(reliab, 13, sizeof(reliab));
    g_r34_soft_bytes[2] = 0xA5U;
    dmr_data_burst_handler(&opts, &state, info, 0x08U, reliab);
    rc |= expect_u8("r34u-soft-fallback-dispatches", (uint8_t)g_block_assembler_calls, 1U);
    rc |= expect_u8("r34u-soft-fallback-len", g_block_assembler_last_len, 18U);
    rc |= expect_u8("r34u-soft-fallback-payload", g_block_assembler_last_bytes[2], 0xA5U);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_profile_contracts();
    rc |= test_handler_dispatch_paths();
    rc |= test_handler_additional_dispatch_paths();
    rc |= test_bptc_ras_and_usbd_services();
    rc |= test_confirmed_sequence_paths();
    rc |= test_trellis_candidate_and_fallback_paths();
    if (rc == 0) {
        printf("DMR_DBURST_PROFILE: OK\n");
    }
    return rc;
}
