// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression checks for NXDN element length guards on short payloads.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, uint8_t CrcCorrect, uint8_t* ElementsContent,
                                  size_t elements_bits);

/*
 * Link stubs:
 * Pulling nxdn_element.c directly into this test would duplicate large decoder
 * code paths, so we link against dsd-neo_proto_nxdn and provide focused stubs
 * for external entrypoints that are irrelevant to these bounds checks.
 */
uint64_t
ConvertBitIntoBytes(uint8_t* bits, uint32_t n) {
    uint64_t v = 0ULL;
    for (uint32_t i = 0U; i < n; i++) {
        v = (v << 1U) | (uint64_t)(bits[i] & 1U);
    }
    return v;
}

uint64_t
convert_bits_into_output(uint8_t* input, int len) {
    if (input == NULL || len <= 0) {
        return 0ULL;
    }
    return ConvertBitIntoBytes(input, (uint32_t)len);
}

void
unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len) {
    if (input == NULL || output == NULL || len <= 0) {
        return;
    }
    memset(output, 0, (size_t)len * sizeof(uint8_t));
    for (int i = 0; i < len; i++) {
        output[i] = (uint8_t)((input[i / 8] >> (7 - (i % 8))) & 1U);
    }
}

void
nxdn_message_type(dsd_opts* opts, dsd_state* state, uint8_t MessageType) {
    (void)opts;
    (void)state;
    (void)MessageType;
}

uint32_t
nxdn_message_crc32(uint8_t* input, int len) {
    (void)input;
    (void)len;
    return 0U;
}

void
nxdn_alias_decode_arib(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    (void)crc_ok;
}

void
nxdn_alias_decode_prop(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    (void)crc_ok;
}

void
nxdn_alias_reset(dsd_state* state) {
    (void)state;
}

long int
nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel) {
    (void)opts;
    (void)state;
    (void)channel;
    return 0;
}

long int
nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel) {
    (void)state;
    (void)channel;
    return 0;
}

void
nxdn_gps_report(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
}

uint8_t
nmea_sentence_checker(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t slot, int len_bytes) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
    return 0U;
}

void
nxdn_trunk_diag_log_missing_channel_once(const dsd_opts* opts, dsd_state* state, uint16_t channel,
                                         const char* context) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)context;
}

void
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
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
LFSR128n(dsd_state* state) {
    (void)state;
}

void
des_multi_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int type,
                           int len) {
    (void)mi;
    (void)key_ulli;
    (void)output;
    (void)type;
    (void)len;
}

void
aes_ofb_keystream_output(uint8_t* iv, uint8_t* key, uint8_t* output, int type, int nblocks) {
    (void)iv;
    (void)key;
    (void)output;
    (void)type;
    (void)nblocks;
}

long int
dsd_rigctl_query_hook_get_current_freq_hz(const dsd_opts* opts) {
    (void)opts;
    return 0;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return 0ULL;
}

void
dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
}

void
dsd_trunk_tuning_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
}

static void
set_message_type(uint8_t* bits, uint8_t type) {
    bits[2] = (uint8_t)((type >> 5U) & 1U);
    bits[3] = (uint8_t)((type >> 4U) & 1U);
    bits[4] = (uint8_t)((type >> 3U) & 1U);
    bits[5] = (uint8_t)((type >> 2U) & 1U);
    bits[6] = (uint8_t)((type >> 1U) & 1U);
    bits[7] = (uint8_t)(type & 1U);
}

static void
write_bits_u64(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0U; i < nbits; i++) {
        size_t shift = (nbits - 1U) - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* tag, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got 0x%llX want 0x%llX\n", tag, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
test_sdcall_header_short_is_ignored(void) {
    dsd_opts opts;
    dsd_state state;
    uint8_t bits[26];
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));
    memset(bits, 0, sizeof(bits));

    set_message_type(bits, 0x38U);
    state.data_header_valid[0] = 0U;
    state.payload_algid = 77;

    NXDN_Elements_Content_decode(&opts, &state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("sdcall-header-short-valid", state.data_header_valid[0], 0);
    rc |= expect_int("sdcall-header-short-algid", state.payload_algid, 77);
    return rc;
}

static int
test_sdcall_iv_short_type_c_is_ignored(void) {
    dsd_opts opts;
    dsd_state state;
    uint8_t bits[26];
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));
    memset(bits, 0, sizeof(bits));

    set_message_type(bits, 0x3AU);
    state.payload_mi = 0x1122334455667788ULL;

    NXDN_Elements_Content_decode(&opts, &state, 1U, bits, sizeof(bits));

    return expect_u64("sdcall-iv-short-type-c", (uint64_t)state.payload_mi, 0x1122334455667788ULL);
}

static int
test_sdcall_iv_type_d_min_length_is_accepted(void) {
    dsd_opts opts;
    dsd_state state;
    uint8_t bits[30];
    const uint64_t iv22 = 0x2A55A1ULL;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));
    memset(bits, 0, sizeof(bits));

    set_message_type(bits, 0x3AU);
    snprintf(state.nxdn_location_category, sizeof(state.nxdn_location_category), "%s", "Type-D");
    write_bits_u64(bits, 8U, iv22, 22U);

    NXDN_Elements_Content_decode(&opts, &state, 1U, bits, sizeof(bits));

    return expect_u64("sdcall-iv-type-d-min-len", (uint64_t)state.payload_mi, iv22);
}

static int
test_short_dcall_data_is_rejected(uint8_t message_type, const char* tag_prefix) {
    dsd_opts opts;
    dsd_state state;
    uint8_t bits[26];
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));
    memset(bits, 0, sizeof(bits));

    set_message_type(bits, message_type);
    state.data_header_valid[0] = 1U;
    state.data_header_blocks[0] = 1;
    state.data_header_padding[0] = 0U;
    state.data_header_format[0] = 3U; //8-byte block (still requires 80 bits total)
    memset(state.dmr_pdu_sf[0], 0xA5, sizeof(state.dmr_pdu_sf[0]));

    NXDN_Elements_Content_decode(&opts, &state, 1U, bits, sizeof(bits));

    int rc = 0;
    char tag_valid[64];
    char tag_copy[64];
    snprintf(tag_valid, sizeof(tag_valid), "%s-valid-cleared", tag_prefix);
    snprintf(tag_copy, sizeof(tag_copy), "%s-no-copy", tag_prefix);
    rc |= expect_int(tag_valid, state.data_header_valid[0], 0);
    rc |= expect_int(tag_copy, state.dmr_pdu_sf[0][64], 0xA5);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_sdcall_header_short_is_ignored();
    rc |= test_sdcall_iv_short_type_c_is_ignored();
    rc |= test_sdcall_iv_type_d_min_length_is_accepted();
    rc |= test_short_dcall_data_is_rejected(0x39U, "sdcall-data-short");
    rc |= test_short_dcall_data_is_rejected(0x0BU, "dcall-data-short");

    if (rc == 0) {
        printf("NXDN_ELEMENT_BOUNDS: OK\n");
    }
    return rc;
}
