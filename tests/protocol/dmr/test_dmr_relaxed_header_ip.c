// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// Smoke test: relaxed header acceptance for SAP=4 (IP-based) with CRC fail

#include <assert.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/fec/rs_12_9.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

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
static unsigned int g_sd_pdu_calls;
static uint16_t g_sd_pdu_last_len;
static uint8_t g_sd_pdu_first_byte;
static uint8_t g_sd_pdu_crc_valid;
static unsigned int g_udp_comp_calls;
static uint16_t g_udp_comp_last_len;
static uint8_t g_udp_comp_first_byte;
static unsigned int g_datacall_calls;
static uint32_t g_datacall_last_src;
static uint32_t g_datacall_last_dst;
static uint8_t g_datacall_last_slot;
static char g_datacall_last_text[256];
static unsigned int g_lip_calls;
static unsigned int g_nmea_calls;
static uint32_t g_nmea_last_src;
static int g_nmea_last_type;
static unsigned int g_utf8_calls;
static uint16_t g_utf8_last_len;
static uint8_t g_utf8_first_byte;

static int
read_file_to_buffer(const char* path, char* out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0U) {
        return -1;
    }
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    size_t n = fread(out, 1U, out_size - 1U, f);
    int bad = ferror(f);
    int close_rc = fclose(f);
    if (bad || close_rc != 0) {
        return -1;
    }
    out[n] = '\0';
    return 0;
}

static int
expect_contains(const char* tag, const char* got, const char* want) {
    if (strstr(got, want) == NULL) {
        DSD_FPRINTF(stderr, "%s: expected output to contain '%s', got '%s'\n", tag, want, got);
        return 1;
    }
    return 0;
}

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
    g_datacall_calls++;
    g_datacall_last_src = src;
    g_datacall_last_dst = dst;
    g_datacall_last_slot = slot;
    DSD_SNPRINTF(g_datacall_last_text, sizeof(g_datacall_last_text), "%s", data_string ? data_string : "");
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)input;
    g_lip_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_iec_61162_1(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int type) {
    (void)opts;
    (void)state;
    (void)input;
    g_nmea_calls++;
    g_nmea_last_src = src;
    g_nmea_last_type = type;
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
rs_12_9_calc_syndrome(const rs_12_9_codeword_t* codeword, rs_12_9_poly_t* syndrome) {
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
aes_ofb_keystream_output(const uint8_t* iv, const uint8_t* key, uint8_t* output, dsd_aes_key_size key_size,
                         int nblocks) {
    (void)iv;
    (void)key;
    (void)output;
    (void)key_size;
    (void)nblocks;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
aes_ecb_decrypt_blocks(const uint8_t* input, const uint8_t* key, uint8_t* output, dsd_aes_key_size key_size,
                       int nblocks) {
    (void)input;
    (void)key;
    (void)output;
    (void)key_size;
    (void)nblocks;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
des_ofb_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int nblocks) {
    (void)mi;
    (void)key_ulli;
    (void)output;
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
    g_sd_pdu_calls++;
    g_sd_pdu_last_len = len;
    g_sd_pdu_first_byte = DMR_PDU ? DMR_PDU[0] : 0U;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_sd_pdu_process(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* dmr_pdu, uint8_t packet_crc_valid) {
    (void)opts;
    (void)state;
    g_sd_pdu_calls++;
    g_sd_pdu_last_len = len;
    g_sd_pdu_first_byte = dmr_pdu ? dmr_pdu[0] : 0U;
    g_sd_pdu_crc_valid = packet_crc_valid;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU) {
    (void)opts;
    (void)state;
    g_udp_comp_calls++;
    g_udp_comp_last_len = len;
    g_udp_comp_first_byte = DMR_PDU ? DMR_PDU[0] : 0U;
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
    g_utf8_calls++;
    g_utf8_last_len = len;
    g_utf8_first_byte = input ? input[0] : 0U;
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
set_byte_bits(uint8_t* bytes, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        size_t bit_index = start + i;
        size_t byte_index = bit_index / 8U;
        uint8_t mask = (uint8_t)(1U << (7U - (bit_index % 8U)));
        if (((value >> (nbits - 1U - i)) & 1U) != 0U) {
            bytes[byte_index] |= mask;
        } else {
            bytes[byte_index] &= (uint8_t)~mask;
        }
    }
}

static void
pack_bits_to_bytes(const uint8_t* bits, uint8_t* bytes, size_t nbits) {
    DSD_MEMSET(bytes, 0, (nbits + 7U) / 8U);
    for (size_t i = 0; i < nbits; i++) {
        if (bits[i] != 0U) {
            bytes[i / 8U] |= (uint8_t)(1U << (7U - (i % 8U)));
        }
    }
}

static void
append_type2_crc16(uint8_t* block, size_t block_len) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof(bits));
    for (size_t i = 0; i < block_len; i++) {
        for (size_t bit = 0; bit < 8U; bit++) {
            bits[(i * 8U) + bit] = (uint8_t)((block[i] >> (7U - bit)) & 1U);
        }
    }
    uint32_t crc = dsd_crc_ccitt16_bits(bits, (block_len * 8U) - 16U);
    block[block_len - 2U] = (uint8_t)((crc >> 8U) & 0xFFU);
    block[block_len - 1U] = (uint8_t)(crc & 0xFFU);
}

static void
build_udt_header(uint8_t dheader[12], uint8_t bits[196], uint8_t udt_format, uint32_t target, uint32_t source) {
    DSD_MEMSET(dheader, 0, 12);
    DSD_MEMSET(bits, 0, 196);
    set_bits(bits, 4, 0U, 4); // DPF=0 UDT
    set_bits(bits, 8, 0U, 4); // SAP=0 UDT
    set_bits(bits, 12, udt_format, 4);
    set_bits(bits, 16, target & 0x00FFFFFFU, 24);
    set_bits(bits, 40, source & 0x00FFFFFFU, 24);
    set_bits(bits, 70, 0U, 2); // encoded UAB 0 -> one appended block
    pack_bits_to_bytes(bits, dheader, 96);
}

static void
build_udt_iso7_block(uint8_t block[12], const char* text) {
    DSD_MEMSET(block, 0, 12);
    for (size_t i = 0; i < 11U && text[i] != '\0'; i++) {
        set_byte_bits(block, i * 7U, (uint8_t)text[i] & 0x7FU, 7U);
    }
    append_type2_crc16(block, 12);
}

static void
build_udt_iso8_block(uint8_t block[12], const char* text) {
    DSD_MEMSET(block, 0, 12);
    for (size_t i = 0; i < 10U && text[i] != '\0'; i++) {
        set_byte_bits(block, i * 8U, (uint8_t)text[i], 8U);
    }
    append_type2_crc16(block, 12);
}

static void
build_udt_bcd_block(uint8_t block[12], const uint8_t* digits, size_t count) {
    DSD_MEMSET(block, 0, 12);
    for (size_t i = 0; i < count && i < 20U; i++) {
        set_byte_bits(block, i * 4U, digits[i] & 0x0FU, 4U);
    }
    append_type2_crc16(block, 12);
}

static void
build_udt_utf16_block(uint8_t block[12], const uint16_t* chars, size_t count) {
    DSD_MEMSET(block, 0, 12);
    for (size_t i = 0; i < count && i < 5U; i++) {
        set_byte_bits(block, i * 16U, chars[i], 16U);
    }
    append_type2_crc16(block, 12);
}

static void
build_udt_mixed_utf16_block(uint8_t block[12], uint32_t address, const uint16_t* chars, size_t count) {
    DSD_MEMSET(block, 0, 12);
    set_byte_bits(block, 8U, address & 0x00FFFFFFU, 24U);
    for (size_t i = 0; i < count && i < 3U; i++) {
        set_byte_bits(block, 32U + (i * 16U), chars[i], 16U);
    }
    append_type2_crc16(block, 12);
}

static void
build_udt_ip4_block(uint8_t block[12], uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    DSD_MEMSET(block, 0, 12);
    set_byte_bits(block, 0U, a, 8U);
    set_byte_bits(block, 8U, b, 8U);
    set_byte_bits(block, 16U, c, 8U);
    set_byte_bits(block, 24U, d, 8U);
    append_type2_crc16(block, 12);
}

static void
build_udt_flag_block(uint8_t block[12], uint8_t encrypted) {
    DSD_MEMSET(block, 0, 12);
    set_byte_bits(block, 0U, encrypted & 1U, 1U);
    append_type2_crc16(block, 12);
}

static void
build_udt_binary_block(uint8_t block[12], const uint8_t* bytes, size_t count) {
    DSD_MEMSET(block, 0, 12);
    size_t copy = count < 10U ? count : 10U;
    if (copy > 0U) {
        DSD_MEMCPY(block, bytes, copy);
    }
    append_type2_crc16(block, 12);
}

static void
build_udt_appended_addressing_block(uint8_t block[12], uint8_t res, uint8_t ok, uint32_t address) {
    DSD_MEMSET(block, 0, 12);
    set_byte_bits(block, 0U, res & 0x7FU, 7U);
    set_byte_bits(block, 7U, ok & 1U, 1U);
    set_byte_bits(block, 8U, address & 0x00FFFFFFU, 24U);
    append_type2_crc16(block, 12);
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
reset_datacall_spy(void) {
    g_decode_ip_calls = 0;
    g_decode_ip_last_len = 0;
    g_decode_ip_first_byte = 0;
    g_sd_pdu_calls = 0;
    g_sd_pdu_last_len = 0;
    g_sd_pdu_first_byte = 0;
    g_sd_pdu_crc_valid = 0;
    g_udp_comp_calls = 0;
    g_udp_comp_last_len = 0;
    g_udp_comp_first_byte = 0;
    g_datacall_calls = 0;
    g_datacall_last_src = 0;
    g_datacall_last_dst = 0;
    g_datacall_last_slot = 0;
    DSD_MEMSET(g_datacall_last_text, 0, sizeof(g_datacall_last_text));
    g_lip_calls = 0;
    g_nmea_calls = 0;
    g_nmea_last_src = 0;
    g_nmea_last_type = 0;
    g_utf8_calls = 0;
    g_utf8_last_len = 0;
    g_utf8_first_byte = 0;
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
    state.data_header_dd_format[0] = 0x16U;
    state.data_header_dd_format[1] = 0x18U;
    state.data_header_bit_padding[0] = 7U;
    state.data_header_bit_padding[1] = 15U;
    state.data_dbsn_have[0] = 1;
    state.data_dbsn_expected[0] = 9;

    dmr_reset_blocks(&opts, &state);

    assert(state.data_block_counter[0] == 1);
    assert(state.data_block_counter[1] == 1);
    assert(state.data_header_blocks[0] == 1);
    assert(state.data_header_blocks[1] == 1);
    assert(state.data_header_format[0] == 7);
    assert(state.data_header_format[1] == 7);
    assert(state.data_header_dd_format[0] == 0U);
    assert(state.data_header_dd_format[1] == 0U);
    assert(state.data_header_bit_padding[0] == 0U);
    assert(state.data_header_bit_padding[1] == 0U);
    assert(state.data_dbsn_have[0] == 0);
    assert(state.data_dbsn_expected[0] == 0);
}

static void
test_udt_iso7_single_block_dispatches_text_event(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t dheader[12];
    uint8_t bits[196];
    uint8_t block[12];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(history, 0, sizeof(history));
    state.event_history_s = history;
    state.currentslot = 0;
    opts.aggressive_framesync = 1;
    build_udt_header(dheader, bits, 0x03U, 0x000111U, 0x000222U);
    build_udt_iso7_block(block, "HELLO");
    reset_datacall_spy();

    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.data_header_format[0] == 0);
    assert(state.data_header_blocks[0] == 1);
    assert(state.data_header_valid[0] == 1);
    assert(state.data_block_counter[0] == 1);
    state.data_block_crc_valid[0][0] = 1;

    dmr_block_assembler(&opts, &state, block, (uint8_t)sizeof(block), 0x0BU, 3U);

    assert(g_datacall_calls == 1U);
    assert(g_datacall_last_src == 0x000222U);
    assert(g_datacall_last_dst == 0x000111U);
    assert(g_datacall_last_slot == 0U);
    assert(strstr(g_datacall_last_text, "ISO7 Text") != NULL);
    assert(strstr(state.event_history_s[0].Event_History_Items[0].text_message, "HELLO") != NULL);
    assert(state.data_header_dd_format[0] == 0U);
    assert(state.data_header_bit_padding[0] == 0U);
    assert(state.lastsrc == 0);
    assert(state.lasttg == 0);
    assert(state.data_header_valid[0] == 0);
    assert(state.data_header_format[0] == 7);
}

static void
run_udt_single_block_on_slot(uint8_t format, const uint8_t block[12], uint8_t slot, dsd_state* state_out) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t dheader[12];
    uint8_t bits[196];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(history, 0, sizeof(history));
    state.event_history_s = history;
    state.currentslot = slot & 1U;
    opts.aggressive_framesync = 1;
    build_udt_header(dheader, bits, format, 0x000111U, 0x000222U);
    reset_datacall_spy();

    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.data_header_valid[state.currentslot] == 1);
    assert(state.data_block_counter[state.currentslot] == 1);
    state.data_block_crc_valid[state.currentslot][0] = 1;

    uint8_t mutable_block[12];
    DSD_MEMCPY(mutable_block, block, sizeof(mutable_block));
    dmr_block_assembler(&opts, &state, mutable_block, (uint8_t)sizeof(mutable_block), 0x0BU, 3U);

    if (state_out != NULL) {
        DSD_MEMCPY(state_out, &state, sizeof(*state_out));
    }
}

static void
run_udt_single_block(uint8_t format, const uint8_t block[12], dsd_state* state_out) {
    run_udt_single_block_on_slot(format, block, 0U, state_out);
}

static void
test_udt_text_and_dispatch_formats(void) {
    uint8_t block[12];
    uint8_t bcd[] = {1, 2, 10, 11, 15, 12};
    uint16_t utf16_chars[] = {'O', 'K', 0x263A};
    uint16_t mixed_chars[] = {'G', 'O'};
    static dsd_state state_copy;

    build_udt_iso8_block(block, "WORLD");
    run_udt_single_block(0x04U, block, &state_copy);
    assert(g_datacall_calls == 1U);
    assert(strstr(g_datacall_last_text, "ISO8 Text") != NULL);
    assert(strstr(state_copy.event_history_s[0].Event_History_Items[0].text_message, "WORLD") != NULL);

    build_udt_bcd_block(block, bcd, sizeof(bcd));
    run_udt_single_block(0x02U, block, &state_copy);
    assert(g_datacall_calls == 1U);
    assert(strstr(g_datacall_last_text, "Dialer Digits") != NULL);
    assert(strstr(state_copy.event_history_s[0].Event_History_Items[0].text_message, "12*# ") != NULL);

    build_udt_utf16_block(block, utf16_chars, sizeof(utf16_chars) / sizeof(utf16_chars[0]));
    run_udt_single_block(0x07U, block, &state_copy);
    assert(g_datacall_calls == 1U);
    assert(strstr(g_datacall_last_text, "UTF16 Text") != NULL);
    assert(strstr(state_copy.event_history_s[0].Event_History_Items[0].text_message, "OK") != NULL);

    build_udt_mixed_utf16_block(block, 0x00ABCDEFU, mixed_chars, sizeof(mixed_chars) / sizeof(mixed_chars[0]));
    run_udt_single_block(0x0AU, block, &state_copy);
    assert(g_datacall_calls == 1U);
    assert(strstr(g_datacall_last_text, "Mixed Add/Text") != NULL);
    assert(strstr(state_copy.event_history_s[0].Event_History_Items[0].text_message, "Address: 11259375;GO") != NULL);

    build_udt_ip4_block(block, 192U, 168U, 1U, 55U);
    run_udt_single_block(0x06U, block, &state_copy);
    assert(g_datacall_calls == 1U);
    assert(strstr(g_datacall_last_text, "IP4") != NULL);

    build_udt_flag_block(block, 0U);
    run_udt_single_block(0x05U, block, NULL);
    assert(g_nmea_calls == 1U);
    assert(g_nmea_last_src == 0x000222U);
    assert(g_nmea_last_type == 1);
    assert(strstr(g_datacall_last_text, "NMEA") != NULL);

    build_udt_flag_block(block, 0U);
    run_udt_single_block(0x0BU, block, NULL);
    assert(g_lip_calls == 1U);
    assert(strstr(g_datacall_last_text, "LIP") != NULL);
}

static void
test_udt_binary_addressing_reserved_and_slot1_paths(void) {
    uint8_t block[12];
    static dsd_state state_copy;
    const uint8_t binary_payload[] = {'B', 'I', 'N', 0x00};

    build_udt_binary_block(block, binary_payload, sizeof(binary_payload));
    run_udt_single_block(0x00U, block, NULL);
    assert(g_datacall_calls == 1U);
    assert(g_utf8_calls == 1U);
    assert(g_utf8_last_len == 10U);
    assert(g_utf8_first_byte == 'B');
    assert(strstr(g_datacall_last_text, "Binary Data") != NULL);

    build_udt_appended_addressing_block(block, 3U, 1U, 0x00C0FFEEU);
    run_udt_single_block(0x01U, block, NULL);
    assert(g_datacall_calls == 1U);
    assert(g_utf8_calls == 0U);
    assert(g_nmea_calls == 0U);
    assert(g_lip_calls == 0U);
    assert(strstr(g_datacall_last_text, "Appended Addressing") != NULL);

    build_udt_flag_block(block, 1U);
    run_udt_single_block(0x05U, block, NULL);
    assert(g_datacall_calls == 1U);
    assert(g_nmea_calls == 0U);
    assert(strstr(g_datacall_last_text, "NMEA") != NULL);

    build_udt_flag_block(block, 0U);
    run_udt_single_block(0x08U, block, NULL);
    assert(g_datacall_calls == 1U);
    assert(strstr(g_datacall_last_text, "MFID Specific") != NULL);

    build_udt_flag_block(block, 0U);
    run_udt_single_block(0x0CU, block, NULL);
    assert(g_datacall_calls == 1U);
    assert(strstr(g_datacall_last_text, "Reserved") != NULL);

    build_udt_iso8_block(block, "SLOT1");
    run_udt_single_block_on_slot(0x04U, block, 1U, &state_copy);
    assert(g_datacall_calls == 1U);
    assert(g_datacall_last_slot == 1U);
    assert(state_copy.lastsrcR == 0);
    assert(state_copy.lasttgR == 0);
    assert(strstr(state_copy.event_history_s[1].Event_History_Items[0].text_message, "SLOT1") != NULL);
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

static void
run_type1_pdu_for_sap(uint8_t sap, const uint8_t block[12]) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.aggressive_framesync = 1;
    state.currentslot = 0;
    state.data_header_valid[0] = 1;
    state.data_header_blocks[0] = 1;
    state.data_header_format[0] = 2;
    state.data_header_sap[0] = sap;
    state.data_block_counter[0] = 1;
    reset_datacall_spy();

    uint8_t mutable_block[12];
    DSD_MEMCPY(mutable_block, block, sizeof(mutable_block));
    dmr_block_assembler(&opts, &state, mutable_block, (uint8_t)sizeof(mutable_block), 0, 1);
}

static void
test_crc_valid_type1_pdu_dispatches_short_data_and_udp_saps(void) {
    uint8_t block[12] = {0x83, 0x10, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0, 0, 0, 0};
    append_type1_crc32(block, sizeof(block));

    run_type1_pdu_for_sap(10U, block);
    assert(g_sd_pdu_calls == 1U);
    assert(g_sd_pdu_last_len == 20U);
    assert(g_sd_pdu_first_byte == 0x83U);
    assert(g_sd_pdu_crc_valid == 1U);
    assert(g_decode_ip_calls == 0U);
    assert(g_udp_comp_calls == 0U);

    run_type1_pdu_for_sap(2U, block);
    assert(g_udp_comp_calls == 1U);
    assert(g_udp_comp_last_len == 20U);
    assert(g_udp_comp_first_byte == 0x83U);
    assert(g_decode_ip_calls == 0U);
    assert(g_sd_pdu_calls == 0U);

    run_type1_pdu_for_sap(3U, block);
    assert(g_udp_comp_calls == 1U);
    assert(g_udp_comp_last_len == 20U);
    assert(g_udp_comp_first_byte == 0x83U);
    assert(g_decode_ip_calls == 0U);
    assert(g_sd_pdu_calls == 0U);
}

static void
test_type1_encrypted_notice_reports_missing_key(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t block[12] = {0x45, 0x00, 0x00, 0x14, 0x12, 0x34, 0x00, 0x00, 0, 0, 0, 0};

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    append_type1_crc32(block, sizeof(block));
    opts.aggressive_framesync = 1;
    state.currentslot = 0;
    state.data_header_valid[0] = 1;
    state.data_header_blocks[0] = 1;
    state.data_header_format[0] = 2;
    state.data_header_sap[0] = 4;
    state.data_block_counter[0] = 1;
    state.dmr_lrrp_source[0] = 0x1234U;
    state.dmr_lrrp_target[0] = 0x5678U;
    state.dmr_so = 0x100;
    state.payload_algid = 4;
    state.payload_keyid = 0x42;
    reset_datacall_spy();

    dmr_block_assembler(&opts, &state, block, (uint8_t)sizeof(block), 0, 1);

    assert(g_decode_ip_calls == 0U);
    assert(g_datacall_calls == 1U);
    assert(g_datacall_last_src == 0x1234U);
    assert(g_datacall_last_dst == 0x5678U);
    assert(strstr(g_datacall_last_text, "ENC PDU") != NULL);
    assert(strstr(state.dmr_lrrp_gps[0], "ENC PDU") != NULL);
}

static void
test_type2_rejects_out_of_bounds_aggregate_length(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t block[12] = {0x80, 0x01, 0x02, 0x03};

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.data_header_valid[0] = 1;
    state.data_block_counter[0] = 9;
    state.data_block_crc_valid[0][0] = 1;

    dmr_block_assembler(&opts, &state, block, (uint8_t)sizeof(block), 0, 2);

    assert(state.data_block_counter[0] == 4);
    assert(state.data_block_crc_valid[0][0] == 0);
    assert(state.data_header_valid[0] == 1);
}

static int
test_data_header_prints_fsn_and_final_flag(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t dheader[12];
    uint8_t bits[196];
    char output[2048];
    dsd_test_capture_stderr cap;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(dheader, 0, sizeof(dheader));
    state.currentslot = 0;
    opts.aggressive_framesync = 1;

    if (dsd_test_capture_stderr_begin(&cap, "dmr_header_fsn_final") != 0) {
        return 1;
    }

    DSD_MEMSET(bits, 0, sizeof(bits));
    set_bits(bits, 4, 2U, 4); // DPF=2, unconfirmed delivery
    set_bits(bits, 8, 4U, 4); // SAP=4, IP based
    set_bits(bits, 16, 0x000123U, 24);
    set_bits(bits, 40, 0x000456U, 24);
    set_bits(bits, 64, 1U, 1); // final-fragment flag
    set_bits(bits, 65, 3U, 7); // blocks to follow
    set_bits(bits, 76, 9U, 4); // fragment sequence number
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);

    DSD_MEMSET(bits, 0, sizeof(bits));
    set_bits(bits, 4, 3U, 4); // DPF=3, confirmed delivery
    set_bits(bits, 8, 4U, 4); // SAP=4, IP based
    set_bits(bits, 16, 0x000123U, 24);
    set_bits(bits, 40, 0x000456U, 24);
    set_bits(bits, 64, 1U, 1);  // final-fragment flag
    set_bits(bits, 65, 4U, 7);  // blocks to follow
    set_bits(bits, 72, 1U, 1);  // send-sequence flag
    set_bits(bits, 73, 5U, 3);  // send-sequence number
    set_bits(bits, 76, 10U, 4); // fragment sequence number
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);

    if (dsd_test_capture_stderr_end(&cap) != 0 || read_file_to_buffer(cap.path, output, sizeof(output)) != 0) {
        (void)remove(cap.path);
        return 1;
    }
    (void)remove(cap.path);

    rc |= expect_contains("unconfirmed-fsn-final", output, "FINAL 1 (FINAL) - BLOCKS 03 - PAD 00 - FSN 9");
    rc |= expect_contains("confirmed-fsn-final", output, "FINAL 1 (FINAL) - BLOCKS 04 - PAD 00 - S 1 - NS 5 - FSN 10");
    return rc;
}

static void
test_irrecoverable_header_resets_data_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t dheader[12];
    uint8_t bits[196];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(dheader, 0, sizeof(dheader));
    DSD_MEMSET(bits, 0, sizeof(bits));
    state.currentslot = 1;
    state.data_header_valid[1] = 1;
    state.data_p_head[1] = 1;
    state.data_conf_data[1] = 1;
    state.data_block_counter[1] = 9;
    state.data_header_blocks[1] = 9;
    state.data_header_format[1] = 2;
    state.data_header_dd_format[1] = 0x16U;
    state.data_header_bit_padding[1] = 23U;
    DSD_SNPRINTF(state.dmr_lrrp_gps[1], sizeof(state.dmr_lrrp_gps[1]), "%s", "stale gps");

    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/0, /*IrrecoverableErrors=*/1);

    assert(state.data_header_valid[1] == 0);
    assert(state.data_p_head[1] == 0);
    assert(state.data_conf_data[1] == 0);
    assert(state.data_block_counter[1] == 1);
    assert(state.data_header_blocks[1] == 1);
    assert(state.data_header_format[1] == 7);
    assert(state.data_header_dd_format[1] == 0U);
    assert(state.data_header_bit_padding[1] == 0U);
    assert(strcmp(state.dmr_lrrp_gps[1], "") == 0);
}

static int
check_response_header_nack_reason(uint8_t r_type, const char* expected) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t dheader[12];
    uint8_t bits[196];
    char output[2048];
    dsd_test_capture_stderr cap;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(dheader, 0, sizeof(dheader));
    DSD_MEMSET(bits, 0, sizeof(bits));
    state.currentslot = 0;
    opts.aggressive_framesync = 1;
    set_bits(bits, 4, 1U, 4); // DPF=1, response packet
    set_bits(bits, 8, 4U, 4); // SAP=4, IP based
    set_bits(bits, 16, 0x000123U, 24);
    set_bits(bits, 40, 0x000456U, 24);
    set_bits(bits, 72, 1U, 2); // NACK class
    set_bits(bits, 74, r_type, 3);

    if (dsd_test_capture_stderr_begin(&cap, "dmr_header_response_nack") != 0) {
        return 1;
    }
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    if (dsd_test_capture_stderr_end(&cap) != 0 || read_file_to_buffer(cap.path, output, sizeof(output)) != 0) {
        (void)remove(cap.path);
        return 1;
    }
    (void)remove(cap.path);

    assert(state.data_header_format[0] == 1);
    assert(state.data_header_valid[0] == 1);
    assert(strcmp(state.dmr_lrrp_gps[0], "") == 0);
    return expect_contains("response-nack", output, expected);
}

static int
test_response_header_reports_nack_reason(void) {
    int rc = 0;
    rc |= check_response_header_nack_reason(1U, "NACK - Packet CRC ERR");
    rc |= check_response_header_nack_reason(2U, "NACK - Memory Full");
    return rc;
}

static void
test_short_data_defined_sets_blocks_and_confirmed_flag(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t dheader[12];
    uint8_t bits[196];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(dheader, 0, sizeof(dheader));
    DSD_MEMSET(bits, 0, sizeof(bits));
    state.currentslot = 0;
    opts.aggressive_framesync = 1;
    set_bits(bits, 1, 1U, 1);  // response requested
    set_bits(bits, 2, 1U, 2);  // S_AB high bits
    set_bits(bits, 4, 13U, 4); // DPF=13, short data defined
    set_bits(bits, 8, 10U, 4); // SAP=short data
    set_bits(bits, 12, 1U, 4); // S_AB low bits: total blocks 17
    set_bits(bits, 16, 0x010203U, 24);
    set_bits(bits, 40, 0x040506U, 24);
    set_bits(bits, 64, 18U, 6); // DD format UTF-8
    set_bits(bits, 72, 7U, 8);  // pad bits

    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);

    assert(state.data_header_format[0] == 13);
    assert(state.data_header_sap[0] == 10);
    assert(state.data_header_blocks[0] == 17);
    assert(state.data_header_dd_format[0] == 0x12U);
    assert(state.data_header_bit_padding[0] == 7U);
    assert(state.data_block_poc[0] == 0U);
    assert(state.data_conf_data[0] == 1);
    assert(strstr(state.dmr_lrrp_gps[0], "Short DT") != NULL);
    assert(strstr(state.dmr_lrrp_gps[0], "- RSP REQ") != NULL);
}

static void
test_short_data_raw_padding_and_packet_poc_isolation(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t dheader[12];
    uint8_t bits[196];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(dheader, 0, sizeof(dheader));
    DSD_MEMSET(bits, 0, sizeof(bits));
    state.currentslot = 0;
    opts.aggressive_framesync = 1;

    set_bits(bits, 4, 14U, 4); // DPF=14, raw short data
    set_bits(bits, 8, 10U, 4);
    set_bits(bits, 12, 3U, 4); // appended blocks, not POC
    set_bits(bits, 72, 11U, 8);
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.data_header_blocks[0] == 3U);
    assert(state.data_header_bit_padding[0] == 11U);
    assert(state.data_header_dd_format[0] == 0U);
    assert(state.data_block_poc[0] == 0U);

    DSD_MEMSET(bits, 0, sizeof(bits));
    set_bits(bits, 4, 2U, 4);  // DPF=2 packet data
    set_bits(bits, 12, 9U, 4); // POC
    set_bits(bits, 65, 1U, 7);
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.data_block_poc[0] == 9U);
    assert(state.data_header_dd_format[0] == 0U);
    assert(state.data_header_bit_padding[0] == 0U);

    DSD_MEMSET(bits, 0, sizeof(bits));
    set_bits(bits, 4, 3U, 4);  // DPF=3 packet data
    set_bits(bits, 12, 6U, 4); // POC
    set_bits(bits, 65, 1U, 7);
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.data_block_poc[0] == 6U);
}

static void
test_motorola_encryption_header_updates_payload_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t dheader[12];
    uint8_t bits[196];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(dheader, 0, sizeof(dheader));
    DSD_MEMSET(bits, 0, sizeof(bits));
    state.currentslot = 0;
    state.data_ks_start[0] = 3;
    set_bits(bits, 0, 2U, 4);            // p_sap != 1
    set_bits(bits, 4, 15U, 4);           // DPF=15, proprietary
    set_bits(bits, 8, 0x10U, 8);         // Motorola MFID
    set_bits(bits, 17, 4U, 3);           // AES128
    set_bits(bits, 20, 1U, 4);           // encrypted
    set_bits(bits, 24, 0x42U, 8);        // key id
    set_bits(bits, 48, 0x11223344U, 32); // MI(32)
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.dmr_so == 0x100);
    assert(state.payload_algid == 4);
    assert(state.payload_keyid == 0x42);
    assert((uint32_t)state.payload_mi == 0x11223344U);
    assert(state.data_ks_start[0] == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(bits, 0, sizeof(bits));
    state.currentslot = 1;
    state.data_ks_start[1] = 3;
    set_bits(bits, 0, 2U, 4);
    set_bits(bits, 4, 15U, 4);
    set_bits(bits, 8, 0x10U, 8);
    set_bits(bits, 17, 5U, 3); // AES256
    set_bits(bits, 20, 1U, 4);
    set_bits(bits, 24, 0x24U, 8);
    set_bits(bits, 48, 0x55667788U, 32);
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/1, /*IrrecoverableErrors=*/0);
    assert(state.dmr_soR == 0x100);
    assert(state.payload_algidR == 5);
    assert(state.payload_keyidR == 0x24);
    assert((uint32_t)state.payload_miR == 0x55667788U);
    assert(state.data_ks_start[1] == 0);
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    test_reset_blocks_restores_integer_defaults();
    test_udt_iso7_single_block_dispatches_text_event();
    test_udt_text_and_dispatch_formats();
    test_udt_binary_addressing_reserved_and_slot1_paths();
    test_crc_valid_type1_pdu_dispatches_in_strict_mode();
    test_crc_valid_type1_pdu_dispatches_short_data_and_udp_saps();
    test_type1_encrypted_notice_reports_missing_key();
    test_type2_rejects_out_of_bounds_aggregate_length();
    int rc = test_data_header_prints_fsn_and_final_flag();
    test_irrecoverable_header_resets_data_state();
    rc |= test_response_header_reports_nack_reason();
    test_short_data_defined_sets_blocks_and_confirmed_flag();
    test_short_data_raw_padding_and_packet_poc_isolation();
    test_motorola_encryption_header_updates_payload_state();

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
    state.data_header_dd_format[0] = 0x16U;
    state.data_header_bit_padding[0] = 16U;
    state.data_block_poc[0] = 7U;
    uint8_t before_format = state.data_header_format[state.currentslot];
    uint8_t before_sap = state.data_header_sap[state.currentslot];
    dmr_dheader(&opts, &state, dheader, bits, /*CRCCorrect=*/0, /*IrrecoverableErrors=*/0);
    assert(state.data_header_format[state.currentslot] == before_format); // unchanged
    assert(state.data_header_sap[state.currentslot] == before_sap);
    assert(state.data_header_dd_format[0] == 0U);
    assert(state.data_header_bit_padding[0] == 0U);
    assert(state.data_block_poc[0] == 0U);

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

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
