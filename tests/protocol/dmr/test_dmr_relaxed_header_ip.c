// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// Smoke test: relaxed header acceptance for SAP=4 (IP-based) with CRC fail

#include <assert.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/rs_12_9.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/unicode.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Forward under test
extern void dmr_dheader(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t dheader_bits[],
                        uint32_t CRCCorrect, uint32_t IrrecoverableErrors);
extern void dmr_block_assembler(dsd_opts* opts, dsd_state* state, uint8_t block_bytes[], uint8_t block_len,
                                uint8_t databurst, uint8_t type);
extern void dmr_reset_blocks(dsd_opts* opts, dsd_state* state);

static unsigned int g_decode_ip_calls;
static uint16_t g_decode_ip_last_len;
static uint8_t g_decode_ip_first_byte;

// Provide local stubs to avoid pulling full core/audio deps during link
void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)src;
    (void)dst;
    (void)data_string;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_iec_61162_1(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int type) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)type;
}

// Stubs to avoid linking runtime/core
int
dsd_unicode_supported(void) {
    return 0;
}

const char*
dsd_degrees_glyph(void) {
    return "";
}

// FEC RS(12,9) stubs used by dmr_utils.c
void
rs_12_9_calc_syndrome(rs_12_9_codeword_t* codeword, rs_12_9_poly_t* syndrome) {
    (void)codeword;
    (void)syndrome;
}

uint8_t
rs_12_9_check_syndrome(const rs_12_9_poly_t* syndrome) {
    (void)syndrome;
    return 0;
}

rs_12_9_correct_errors_result_t
rs_12_9_correct_errors(rs_12_9_codeword_t* codeword, const rs_12_9_poly_t* syndrome, uint8_t* errors_found) {
    (void)codeword;
    (void)syndrome;
    if (errors_found) {
        *errors_found = 0;
    }
    return RS_12_9_CORRECT_ERRORS_RESULT_NO_ERRORS_FOUND;
}

// Crypto and PDU helpers (stubs)
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
LFSR128d(dsd_state* state) {
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rc4_block_output(int drop, int keylen, int meslen, const uint8_t* key, uint8_t* output_blocks) {
    (void)drop;
    (void)keylen;
    (void)meslen;
    (void)key;
    (void)output_blocks;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
aes_ofb_keystream_output(const uint8_t* iv, const uint8_t* key, uint8_t* output, int type, int nblocks) {
    (void)iv;
    (void)key;
    (void)output;
    (void)type;
    (void)nblocks;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
aes_ecb_decrypt_blocks(const uint8_t* input, const uint8_t* key, uint8_t* output, int type, int nblocks) {
    (void)input;
    (void)key;
    (void)output;
    (void)type;
    (void)nblocks;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
des_multi_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int type,
                           int nblocks) {
    (void)mi;
    (void)key_ulli;
    (void)output;
    (void)type;
    (void)nblocks;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    g_decode_ip_calls++;
    g_decode_ip_last_len = len;
    g_decode_ip_first_byte = input ? input[0] : 0U;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_sd_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU) {
    (void)opts;
    (void)state;
    (void)len;
    (void)DMR_PDU;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU) {
    (void)opts;
    (void)state;
    (void)len;
    (void)DMR_PDU;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_lrrp(dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, uint8_t* DMR_PDU,
         uint8_t pdu_crc_ok) {
    (void)opts;
    (void)state;
    (void)len;
    (void)source;
    (void)dest;
    (void)DMR_PDU;
    (void)pdu_crc_ok;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t* bits, uint8_t* bytes, uint32_t CRCCorrect,
          uint32_t IrrecoverableErrors) {
    (void)opts;
    (void)state;
    (void)bits;
    (void)bytes;
    (void)CRCCorrect;
    (void)IrrecoverableErrors;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_locn(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU) {
    (void)opts;
    (void)state;
    (void)len;
    (void)DMR_PDU;
}

static void
set_bits(uint8_t* bits, int start, uint32_t value, int nbits) {
    for (int i = 0; i < nbits; i++) {
        int bit = (int)((value >> (nbits - 1 - i)) & 1U);
        bits[start + i] = (uint8_t)bit;
    }
}

static void
pack_type1_crc_bits(const uint8_t* bytes, size_t count, uint8_t* bits) {
    for (size_t i = 0, j = 0; i < count; i += 2, j += 16) {
        if ((i + 1U) < count) {
            bits[j + 0U] = (bytes[i + 1U] >> 7) & 0x01;
            bits[j + 1U] = (bytes[i + 1U] >> 6) & 0x01;
            bits[j + 2U] = (bytes[i + 1U] >> 5) & 0x01;
            bits[j + 3U] = (bytes[i + 1U] >> 4) & 0x01;
            bits[j + 4U] = (bytes[i + 1U] >> 3) & 0x01;
            bits[j + 5U] = (bytes[i + 1U] >> 2) & 0x01;
            bits[j + 6U] = (bytes[i + 1U] >> 1) & 0x01;
            bits[j + 7U] = (bytes[i + 1U] >> 0) & 0x01;
        }

        bits[j + 8U] = (bytes[i] >> 7) & 0x01;
        bits[j + 9U] = (bytes[i] >> 6) & 0x01;
        bits[j + 10U] = (bytes[i] >> 5) & 0x01;
        bits[j + 11U] = (bytes[i] >> 4) & 0x01;
        bits[j + 12U] = (bytes[i] >> 3) & 0x01;
        bits[j + 13U] = (bytes[i] >> 2) & 0x01;
        bits[j + 14U] = (bytes[i] >> 1) & 0x01;
        bits[j + 15U] = (bytes[i] >> 0) & 0x01;
    }
}

static void
append_type1_crc32(uint8_t* bytes, size_t count) {
    assert(count >= 8U);
    uint8_t bits[8 * 24 * 129];
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_type1_crc_bits(bytes, count, bits);
    uint32_t crc = ComputeCrc32Bit(bits, (uint32_t)((count * 8U) - 32U));
    bytes[count - 4U] = (uint8_t)((crc >> 24U) & 0xFFU);
    bytes[count - 3U] = (uint8_t)((crc >> 16U) & 0xFFU);
    bytes[count - 2U] = (uint8_t)((crc >> 8U) & 0xFFU);
    bytes[count - 1U] = (uint8_t)(crc & 0xFFU);
}

static void
test_reset_blocks_restores_integer_defaults(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.data_block_counter[0] = 42;
    state.data_block_counter[1] = 43;
    state.data_header_blocks[0] = 44;
    state.data_header_blocks[1] = 45;
    state.data_header_format[0] = 2;
    state.data_header_format[1] = 3;
    state.data_dbsn_have[0] = 1;
    state.data_dbsn_expected[0] = 9;

    dmr_reset_blocks(&opts, &state);

    assert(state.data_block_counter[0] == 1);
    assert(state.data_block_counter[1] == 1);
    assert(state.data_header_blocks[0] == 1);
    assert(state.data_header_blocks[1] == 1);
    assert(state.data_header_format[0] == 7);
    assert(state.data_header_format[1] == 7);
    assert(state.data_dbsn_have[0] == 0);
    assert(state.data_dbsn_expected[0] == 0);
}

static void
test_crc_valid_type1_pdu_dispatches_in_strict_mode(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.aggressive_framesync = 1;
    opts.dmr_crc_relaxed_default = 0;
    state.currentslot = 0;
    state.data_header_valid[0] = 1;
    state.data_header_blocks[0] = 1;
    state.data_header_format[0] = 2;
    state.data_header_sap[0] = 4;
    state.data_block_counter[0] = 1;

    uint8_t block[12] = {0x45, 0x00, 0x00, 0x14, 0x12, 0x34, 0x00, 0x00, 0, 0, 0, 0};
    append_type1_crc32(block, sizeof(block));

    g_decode_ip_calls = 0;
    g_decode_ip_last_len = 0;
    g_decode_ip_first_byte = 0;

    dmr_block_assembler(&opts, &state, block, (uint8_t)sizeof(block), 0, 1);

    assert(g_decode_ip_calls == 1U);
    assert(g_decode_ip_last_len == 20U);
    assert(g_decode_ip_first_byte == 0x45U);
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    test_reset_blocks_restores_integer_defaults();
    test_crc_valid_type1_pdu_dispatches_in_strict_mode();

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;

    // Craft a minimal DMR Data Header bit array (MSB-first) with:
    // - DPF=2 (Unconfirmed Delivery)
    // - SAP=4 (IP Based)
    // - Non-zero Source/Target
    // We will call dmr_dheader with CRCCorrect=0 and IrrecoverableErrors=0.
    // Expect: strict mode rejects; relaxed mode accepts and stores SAP=4 and DPF=2.

    uint8_t dheader[12];
    uint8_t bits[196];
    DSD_MEMSET(dheader, 0, sizeof(dheader));
    DSD_MEMSET(bits, 0, sizeof(bits));

    // gi(0), a(0), ab(0)
    // mpoc at bit 3 -> 0
    // dpf at [4..7] = 2
    set_bits(bits, 4, 2U, 4);
    // sap at [8..11] = 4 (IP Based)
    set_bits(bits, 8, 4U, 4);
    // poc at [12..15] = 0

    // target [16..39] (24 bits) and source [40..63] (24 bits)
    set_bits(bits, 16, 0x000123U, 24);
    set_bits(bits, 40, 0x000456U, 24);

    // f at [64] = 0, bf [65..71] = 1 (non-zero blocks)
    set_bits(bits, 65, 1U, 7);

    // Strict (aggressive) mode: should NOT accept when CRC fails
    opts.aggressive_framesync = 1;
    uint8_t before_format = state.data_header_format[state.currentslot];
    uint8_t before_sap = state.data_header_sap[state.currentslot];
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/0, /*IrrecoverableErrors=*/0);
    assert(state.data_header_format[state.currentslot] == before_format); // unchanged
    assert(state.data_header_sap[state.currentslot] == before_sap);

    // Relaxed mode: should accept header despite CRC failure
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    opts.aggressive_framesync = 0;
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/0, /*IrrecoverableErrors=*/0);

    assert(state.data_header_format[state.currentslot] == 2); // DPF accepted
    assert(state.data_header_sap[state.currentslot] == 4);    // SAP=4 stored
    assert(state.dmr_lrrp_target[state.currentslot] != 0);
    assert(state.dmr_lrrp_source[state.currentslot] != 0);

    // UDT NMEA with encoded UAB=2 yields decoded UAB=3, which is reserved/unknown.
    // dsd-neo keeps the announced count and marks it for CRC-based end detection.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    opts.aggressive_framesync = 1;
    DSD_MEMSET(bits, 0, sizeof(bits));
    set_bits(bits, 4, 0U, 4);    // DPF=0 UDT
    set_bits(bits, 8, 0U, 4);    // SAP=0 UDT
    set_bits(bits, 12, 0x5U, 4); // UDT format=NMEA LOCN
    set_bits(bits, 16, 0x123456U, 24);
    set_bits(bits, 40, 0x654321U, 24);
    set_bits(bits, 70, 2U, 2); // UAB encoded value 2 -> decoded count 3
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.data_header_format[0] == 0);
    assert(state.data_header_valid[0] == 1);
    assert(state.data_header_blocks[0] == 3);
    assert(state.udt_uab_reserved[0] == 1);

    // Vertex proprietary extended header (MFID 0x77), slot 0.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.data_ks_start[0] = 3;
    DSD_MEMSET(bits, 0, sizeof(bits));
    set_bits(bits, 0, 2U, 4);            // p_sap != 1
    set_bits(bits, 4, 15U, 4);           // dpf=15 (proprietary)
    set_bits(bits, 8, 0x77U, 8);         // p_mfid=Vertex
    set_bits(bits, 16, 0x5AU, 8);        // key id
    set_bits(bits, 48, 0xA1B2C3D4U, 32); // MI(32)
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.payload_algid == 0x07);
    assert(state.payload_keyid == 0x5A);
    assert((uint32_t)state.payload_mi == 0xA1B2C3D4U);
    assert(state.dmr_so == 0x100);
    assert(state.data_ks_start[0] == 0);

    // Vertex proprietary extended header (MFID 0x77), slot 1 mirror fields.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;
    state.data_ks_start[1] = 2;
    DSD_MEMSET(bits, 0, sizeof(bits));
    set_bits(bits, 0, 2U, 4);            // p_sap != 1
    set_bits(bits, 4, 15U, 4);           // dpf=15 (proprietary)
    set_bits(bits, 8, 0x77U, 8);         // p_mfid=Vertex
    set_bits(bits, 16, 0x33U, 8);        // key id
    set_bits(bits, 48, 0x01020304U, 32); // MI(32)
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.payload_algidR == 0x07);
    assert(state.payload_keyidR == 0x33);
    assert((uint32_t)state.payload_miR == 0x01020304U);
    assert(state.dmr_soR == 0x100);
    assert(state.data_ks_start[1] == 0);

    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
