// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression checks for NXDN element length guards on short payloads.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, uint8_t CrcCorrect, const uint8_t* ElementsContent,
                                  size_t elements_bits);

static int g_alias_prop_calls;
static uint8_t g_alias_prop_crc_ok;

/*
 * Link stubs:
 * Pulling nxdn_element.c directly into this test would duplicate large decoder
 * code paths, so we link against dsd-neo_proto_nxdn and provide focused stubs
 * for external entrypoints that are irrelevant to these bounds checks.
 */
uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
ConvertBitIntoBytes(const uint8_t* bits, uint32_t n) {
    uint64_t v = 0ULL;
    for (uint32_t i = 0U; i < n; i++) {
        v = (v << 1U) | (uint64_t)(bits[i] & 1U);
    }
    return v;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
convert_bits_into_output(const uint8_t* input, int len) {
    if (input == NULL || len <= 0) {
        return 0ULL;
    }
    return ConvertBitIntoBytes(input, (uint32_t)len);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    if (input == NULL || output == NULL || len <= 0) {
        return;
    }
    const int bit_len = len * 8;
    DSD_MEMSET(output, 0, (size_t)bit_len * sizeof(uint8_t));
    for (int i = 0; i < bit_len; i++) {
        output[i] = (uint8_t)((input[i / 8] >> (7 - (i % 8))) & 1U);
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_message_type(const dsd_opts* opts, dsd_state* state, uint8_t MessageType) {
    (void)opts;
    (void)state;
    (void)MessageType;
}

uint32_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_message_crc32(const uint8_t* input, int len) {
    (void)input;
    (void)len;
    return 0U;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_decode_arib(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    (void)crc_ok;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_decode_prop(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    g_alias_prop_calls++;
    g_alias_prop_crc_ok = crc_ok;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_reset(dsd_state* state) {
    (void)state;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel) {
    (void)opts;
    (void)state;
    (void)channel;
    return 0;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel) {
    (void)state;
    (void)channel;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_gps_report(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_sentence_checker(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t slot, int len_bytes) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
    return 0U;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_trunk_diag_log_missing_channel_once(const dsd_opts* opts, dsd_state* state, uint16_t channel,
                                         const char* context) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)context;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

static char g_datacall_event[80];
static uint32_t g_datacall_src;
static uint32_t g_datacall_dst;
static uint8_t g_datacall_slot;
static int g_des_fill_enabled;
static int g_aes_fill_enabled;
static int g_des_calls;
static int g_aes_calls;
static unsigned long long g_des_mi;
static unsigned long long g_des_key;
static int g_des_type;
static int g_des_len;
static uint8_t g_aes_iv[16];
static uint8_t g_aes_key[32];
static int g_aes_type;
static int g_aes_blocks;

static void
reset_datacall_capture(void) {
    DSD_MEMSET(g_datacall_event, 0, sizeof(g_datacall_event));
    g_datacall_src = 0;
    g_datacall_dst = 0;
    g_datacall_slot = 0xFFU;
}

static uint8_t
des_stub_byte(size_t idx) {
    return (uint8_t)(0xA5U ^ (uint8_t)(idx * 17U));
}

static uint8_t
aes_stub_byte(size_t idx) {
    return (uint8_t)(0x5AU ^ (uint8_t)(idx * 13U));
}

static void
reset_crypto_stub_capture(void) {
    g_des_fill_enabled = 0;
    g_aes_fill_enabled = 0;
    g_des_calls = 0;
    g_aes_calls = 0;
    g_des_mi = 0ULL;
    g_des_key = 0ULL;
    g_des_type = 0;
    g_des_len = 0;
    DSD_MEMSET(g_aes_iv, 0, sizeof(g_aes_iv));
    DSD_MEMSET(g_aes_key, 0, sizeof(g_aes_key));
    g_aes_type = 0;
    g_aes_blocks = 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string, uint8_t slot) {
    (void)opts;
    (void)state;
    g_datacall_src = src;
    g_datacall_dst = dst;
    g_datacall_slot = slot;
    DSD_SNPRINTF(g_datacall_event, sizeof(g_datacall_event), "%s", data_string ? data_string : "");
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
LFSR128n(dsd_state* state) {
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
des_multi_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int type,
                           int len) {
    g_des_calls++;
    g_des_mi = mi;
    g_des_key = key_ulli;
    g_des_type = type;
    g_des_len = len;
    if (output == NULL || len <= 0 || g_des_fill_enabled == 0) {
        return;
    }
    const size_t output_len = (size_t)len * 8U;
    for (size_t i = 0U; i < output_len; i++) {
        output[i] = des_stub_byte(i);
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
aes_ofb_keystream_output(const uint8_t* iv, const uint8_t* key, uint8_t* output, int type, int nblocks) {
    g_aes_calls++;
    g_aes_type = type;
    g_aes_blocks = nblocks;
    if (iv != NULL) {
        DSD_MEMCPY(g_aes_iv, iv, sizeof(g_aes_iv));
    }
    if (key != NULL) {
        DSD_MEMCPY(g_aes_key, key, sizeof(g_aes_key));
    }
    if (output == NULL || nblocks <= 0 || g_aes_fill_enabled == 0) {
        return;
    }
    const size_t output_len = (size_t)nblocks * 16U;
    for (size_t i = 0U; i < output_len; i++) {
        output[i] = aes_stub_byte(i);
    }
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_rigctl_query_hook_get_current_freq_hz(const dsd_opts* opts) {
    (void)opts;
    return 0;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ns(void) {
    return 0ULL;
}

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tuning_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    return DSD_TRUNK_TUNE_RESULT_OK;
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
set_extended_message_type(uint8_t* bits, uint8_t type) {
    bits[0] = (uint8_t)((type >> 7U) & 1U);
    bits[1] = (uint8_t)((type >> 6U) & 1U);
    set_message_type(bits, (uint8_t)(type & 0x3FU));
}

static void
write_bits_u64(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0U; i < nbits; i++) {
        size_t shift = (nbits - 1U) - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
write_ascii_bits(uint8_t* bits, size_t start, const char* text) {
    for (size_t i = 0U; text[i] != '\0'; i++) {
        write_bits_u64(bits, start + (i * 8U), (uint8_t)text[i], 8U);
    }
}

static void
write_data_call_header_fields(uint8_t* bits, uint8_t message_type, uint8_t cipher_type, uint8_t key_id, uint16_t source,
                              uint16_t destination, uint8_t block_count, uint8_t padding) {
    set_message_type(bits, message_type);
    write_bits_u64(bits, 16U, 1U, 3U);
    write_bits_u64(bits, 19U, 2U, 5U);
    write_bits_u64(bits, 24U, source, 16U);
    write_bits_u64(bits, 40U, destination, 16U);
    write_bits_u64(bits, 56U, cipher_type, 2U);
    write_bits_u64(bits, 58U, key_id, 6U);
    write_bits_u64(bits, 68U, block_count, 4U);
    write_bits_u64(bits, 72U, padding, 5U);
}

static void
write_standard_alias_marker(uint8_t* bits) {
    write_bits_u64(bits, 8U, 0x68U, 8U);
    write_bits_u64(bits, 16U, 0x8204U, 16U);
    write_bits_u64(bits, 32U, 1U, 4U);
    write_bits_u64(bits, 36U, 1U, 4U);
    write_ascii_bits(bits, 40U, "TEST");
}

static void
write_data_payload_frame(uint8_t* bits, uint8_t message_type, uint8_t pf_num, uint8_t blk_num, const uint8_t* payload,
                         size_t payload_len) {
    set_message_type(bits, message_type);
    write_bits_u64(bits, 8U, pf_num, 4U);
    write_bits_u64(bits, 12U, blk_num, 4U);
    for (size_t i = 0U; i < payload_len; i++) {
        write_bits_u64(bits, 16U + (i * 8U), payload[i], 8U);
    }
}

static void
xor_payload(uint8_t* out, const uint8_t* plain, size_t len, uint8_t (*ks_byte)(size_t)) {
    for (size_t i = 0U; i < len; i++) {
        out[i] = (uint8_t)(plain[i] ^ ks_byte(i));
    }
}

static int
expect_bytes(const char* tag, const uint8_t* got, const uint8_t* want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        DSD_FPRINTF(stderr, "%s: byte arrays differ\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* tag, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%llX want 0x%llX\n", tag, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_string(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
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

static int
read_capture_file(const char* path, char* out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0U) {
        return 1;
    }
    out[0] = '\0';
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return 1;
    }
    size_t nread = fread(out, 1U, out_size - 1U, fp);
    out[nread] = '\0';
    (void)fclose(fp);
    return 0;
}

static int
test_sdcall_header_short_is_ignored(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[26];
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));

    set_message_type(bits, 0x38U);
    state->data_header_valid[0] = 0U;
    state->payload_algid = 77;

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("sdcall-header-short-valid", state->data_header_valid[0], 0);
    rc |= expect_int("sdcall-header-short-algid", state->payload_algid, 77);
    free(state);
    free(opts);
    return rc;
}

static int
test_sdcall_iv_short_type_c_is_ignored(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[26];
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));

    set_message_type(bits, 0x3AU);
    state->payload_mi = 0x1122334455667788ULL;

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = expect_u64("sdcall-iv-short-type-c", (uint64_t)state->payload_mi, 0x1122334455667788ULL);
    free(state);
    free(opts);
    return rc;
}

static int
test_sdcall_iv_type_d_min_length_is_accepted(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[30];
    const uint64_t iv22 = 0x2A55A1ULL;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));

    set_message_type(bits, 0x3AU);
    DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "%s", "Type-D");
    write_bits_u64(bits, 8U, iv22, 22U);

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = expect_u64("sdcall-iv-type-d-min-len", (uint64_t)state->payload_mi, iv22);
    free(state);
    free(opts);
    return rc;
}

static int
test_prop_form_alias_requires_marker(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[96];
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }

    int rc = 0;
    DSD_MEMSET(bits, 0, sizeof(bits));
    set_message_type(bits, 0x3FU);
    g_alias_prop_calls = 0;
    g_alias_prop_crc_ok = 0U;
    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));
    rc |= expect_int("prop-form-no-marker", g_alias_prop_calls, 0);

    DSD_MEMSET(bits, 0, sizeof(bits));
    set_message_type(bits, 0x3FU);
    write_standard_alias_marker(bits);
    g_alias_prop_calls = 0;
    g_alias_prop_crc_ok = 0U;
    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));
    rc |= expect_int("prop-form-marker", g_alias_prop_calls, 1);
    rc |= expect_int("prop-form-marker-crc", g_alias_prop_crc_ok, 1);

    free(state);
    free(opts);
    return rc;
}

static int
test_short_dcall_data_is_rejected(uint8_t message_type, const char* tag_prefix) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[26];
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));

    set_message_type(bits, message_type);
    state->data_header_valid[0] = 1U;
    state->data_header_blocks[0] = 1;
    state->data_header_padding[0] = 0U;
    state->data_header_format[0] = 3U; //8-byte block (still requires 80 bits total)
    DSD_MEMSET(state->dmr_pdu_sf[0], 0xA5, sizeof(state->dmr_pdu_sf[0]));

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    char tag_valid[64];
    char tag_copy[64];
    DSD_SNPRINTF(tag_valid, sizeof(tag_valid), "%s-valid-cleared", tag_prefix);
    DSD_SNPRINTF(tag_copy, sizeof(tag_copy), "%s-no-copy", tag_prefix);
    rc |= expect_int(tag_valid, state->data_header_valid[0], 0);
    rc |= expect_int(tag_copy, state->dmr_pdu_sf[0][64], 0xA5);
    free(state);
    free(opts);
    return rc;
}

static int
test_dst_id_info_complete_event(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[96];
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));
    reset_datacall_capture();

    set_message_type(bits, 0x17U);
    bits[8] = 1U;
    bits[9] = 1U;
    write_bits_u64(bits, 10U, 4U, 6U);
    write_ascii_bits(bits, 16U, "RADIO");

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_string("dst-id-event", g_datacall_event, "NXDN Digital Station ID: RADIO");
    rc |= expect_int("dst-id-src", (int)g_datacall_src, 65520);
    rc |= expect_int("dst-id-dst", (int)g_datacall_dst, 0);
    rc |= expect_int("dst-id-slot", (int)g_datacall_slot, 0);
    free(state);
    free(opts);
    return rc;
}

static int
test_sdcall_des_data_decrypts_and_resets(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t header_bits[96];
    uint8_t first_bits[80];
    uint8_t final_bits[80];
    uint8_t encrypted[16];
    static const uint8_t plain[16] = {
        0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU, 0xDEU, 0xF0U, 0x10U, 0x32U, 0x54U, 0x76U, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    const uint8_t key_id = 0x05U;
    const uint64_t des_key = 0x0123456789ABCDEFULL;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    state->event_history_s = (Event_History_I*)calloc(1, sizeof(*state->event_history_s));
    if (state->event_history_s == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: event_history\n");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(header_bits, 0, sizeof(header_bits));
    DSD_MEMSET(first_bits, 0, sizeof(first_bits));
    DSD_MEMSET(final_bits, 0, sizeof(final_bits));
    reset_datacall_capture();
    reset_crypto_stub_capture();
    g_des_fill_enabled = 1;
    state->keyloader = 1;
    state->rkey_array[key_id] = des_key;

    write_data_call_header_fields(header_bits, 0x38U, 2U, key_id, 0x1234U, 0x4567U, 1U, 0U);
    NXDN_Elements_Content_decode(opts, state, 1U, header_bits, sizeof(header_bits));
    state->data_header_format[0] = 3U;

    xor_payload(encrypted, plain, sizeof(encrypted), des_stub_byte);
    write_data_payload_frame(first_bits, 0x39U, 1U, 1U, encrypted, 8U);
    write_data_payload_frame(final_bits, 0x39U, 0U, 0U, encrypted + 8U, 8U);
    NXDN_Elements_Content_decode(opts, state, 1U, first_bits, sizeof(first_bits));
    NXDN_Elements_Content_decode(opts, state, 1U, final_bits, sizeof(final_bits));

    int rc = 0;
    rc |= expect_int("sdcall-des-calls", g_des_calls, 1);
    rc |= expect_u64("sdcall-des-mi", (uint64_t)g_des_mi, 0ULL);
    rc |= expect_u64("sdcall-des-key", (uint64_t)g_des_key, des_key);
    rc |= expect_int("sdcall-des-type", g_des_type, 1);
    rc |= expect_int("sdcall-des-len", g_des_len, 2);
    rc |= expect_string("sdcall-des-event", state->event_history_s[0].Event_History_Items[0].text_message,
                        "Unknown Data Call Format: 1234;");
    rc |= expect_string("sdcall-des-watchdog", g_datacall_event, "DATA CALL SRC: 4660; TGT: 17767;");
    rc |= expect_int("sdcall-des-src", (int)g_datacall_src, 0x1234);
    rc |= expect_int("sdcall-des-dst", (int)g_datacall_dst, 0x4567);
    rc |= expect_int("sdcall-des-slot", (int)g_datacall_slot, 0);
    rc |= expect_int("sdcall-des-valid-reset", state->data_header_valid[0], 0);
    rc |= expect_int("sdcall-des-blocks-reset", state->data_header_blocks[0], 1);
    rc |= expect_int("sdcall-des-format-reset", state->data_header_format[0], 0);
    rc |= expect_int("sdcall-des-alg-reset", state->payload_algid, 0);
    rc |= expect_int("sdcall-des-keyid-reset", state->payload_keyid, 0);

    free(state->event_history_s);
    free(state);
    free(opts);
    return rc;
}

static int
test_dcall_aes_data_decrypts_with_manual_key_and_iv(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t header_bits[160];
    uint8_t first_bits[80];
    uint8_t final_bits[80];
    uint8_t encrypted[16];
    char output[512];
    dsd_test_capture_stderr cap;
    static const uint8_t plain[16] = {
        0xABU, 0xCDU, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U, 0x99U, 0xAAU, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    static const uint8_t expected_key[32] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
        0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    const uint8_t key_id = 0x11U;
    const uint64_t iv = 0x0102030405060708ULL;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    state->event_history_s = (Event_History_I*)calloc(1, sizeof(*state->event_history_s));
    if (state->event_history_s == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: event_history\n");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(header_bits, 0, sizeof(header_bits));
    DSD_MEMSET(first_bits, 0, sizeof(first_bits));
    DSD_MEMSET(final_bits, 0, sizeof(final_bits));
    reset_datacall_capture();
    reset_crypto_stub_capture();
    g_aes_fill_enabled = 1;
    opts->show_keys = 1;
    state->keyloader = 0;
    state->K1 = 0x0102030405060708ULL;
    state->K2 = 0x1112131415161718ULL;
    state->K3 = 0x2122232425262728ULL;
    state->K4 = 0ULL;

    write_data_call_header_fields(header_bits, 0x09U, 3U, key_id, 0x0201U, 0x0302U, 1U, 0U);
    write_bits_u64(header_bits, 88U, iv, 64U);
    if (dsd_test_capture_stderr_begin(&cap, "dsdneo_nxdn_dcall_aes") != 0) {
        DSD_FPRINTF(stderr, "capture stderr begin failed\n");
        free(state->event_history_s);
        free(state);
        free(opts);
        return 1;
    }
    NXDN_Elements_Content_decode(opts, state, 1U, header_bits, sizeof(header_bits));
    state->data_header_format[0] = 3U;

    xor_payload(encrypted, plain, sizeof(encrypted), aes_stub_byte);
    write_data_payload_frame(first_bits, 0x0BU, 1U, 1U, encrypted, 8U);
    write_data_payload_frame(final_bits, 0x0BU, 0U, 0U, encrypted + 8U, 8U);
    NXDN_Elements_Content_decode(opts, state, 1U, first_bits, sizeof(first_bits));
    NXDN_Elements_Content_decode(opts, state, 1U, final_bits, sizeof(final_bits));
    if (dsd_test_capture_stderr_end(&cap) != 0 || read_capture_file(cap.path, output, sizeof output) != 0) {
        DSD_FPRINTF(stderr, "capture stderr read failed\n");
        (void)remove(cap.path);
        free(state->event_history_s);
        free(state);
        free(opts);
        return 1;
    }
    (void)remove(cap.path);

    const uint8_t zero_iv[16] = {0};
    int rc = 0;
    rc |= expect_int("dcall-aes-calls", g_aes_calls, 1);
    rc |= expect_int("dcall-aes-type", g_aes_type, 2);
    rc |= expect_int("dcall-aes-blocks", g_aes_blocks, 1);
    rc |= expect_bytes("dcall-aes-key", g_aes_key, expected_key, sizeof(expected_key));
    rc |= expect_contains("dcall-aes-reveal-full-key", output,
                          "Key: 0102030405060708111213141516171821222324252627280000000000000000;");
    rc |= expect_string("dcall-aes-event", state->event_history_s[0].Event_History_Items[0].text_message,
                        "Unknown Data Call Format: ABCD;");
    rc |= expect_string("dcall-aes-watchdog", g_datacall_event, "DATA CALL SRC: 513; TGT: 770;");
    rc |= expect_int("dcall-aes-src", (int)g_datacall_src, 0x0201);
    rc |= expect_int("dcall-aes-dst", (int)g_datacall_dst, 0x0302);
    rc |= expect_int("dcall-aes-valid-reset", state->data_header_valid[0], 0);
    rc |= expect_int("dcall-aes-alg-reset", state->payload_algid, 0);
    rc |= expect_int("dcall-aes-keyid-reset", state->payload_keyid, 0);
    rc |= expect_u64("dcall-aes-mi-reset", (uint64_t)state->payload_mi, 0ULL);
    rc |= expect_bytes("dcall-aes-iv-reset", state->aes_ivR, zero_iv, sizeof(zero_iv));

    free(state->event_history_s);
    free(state);
    free(opts);
    return rc;
}

static void
write_arib_vcall_fields(uint8_t* bits, uint8_t message_type, uint8_t mfid, uint8_t cc_option, uint8_t call_type,
                        uint8_t voice_call_option, uint16_t source, uint16_t destination, uint8_t cipher_type,
                        uint8_t key_id) {
    set_extended_message_type(bits, message_type);
    write_bits_u64(bits, 8U, mfid, 8U);
    write_bits_u64(bits, 16U, cc_option, 8U);
    write_bits_u64(bits, 24U, call_type, 3U);
    write_bits_u64(bits, 27U, voice_call_option, 5U);
    write_bits_u64(bits, 32U, source, 16U);
    write_bits_u64(bits, 48U, destination, 16U);
    write_bits_u64(bits, 64U, cipher_type, 2U);
    write_bits_u64(bits, 66U, key_id, 6U);
}

static void
write_vcall_fields(uint8_t* bits, uint8_t message_type, uint8_t cc_option, uint8_t call_type, uint8_t voice_call_option,
                   uint16_t source, uint16_t destination, uint8_t cipher_type, uint8_t key_id) {
    set_message_type(bits, message_type);
    write_bits_u64(bits, 8U, cc_option, 8U);
    write_bits_u64(bits, 16U, call_type, 3U);
    write_bits_u64(bits, 19U, voice_call_option, 5U);
    write_bits_u64(bits, 24U, source, 16U);
    write_bits_u64(bits, 40U, destination, 16U);
    write_bits_u64(bits, 56U, cipher_type, 2U);
    write_bits_u64(bits, 58U, key_id, 6U);
}

static void
write_vcall_iv_fields(uint8_t* bits, uint64_t iv) {
    set_message_type(bits, 0x03U);
    write_bits_u64(bits, 8U, iv, 64U);
}

static int
test_arib_vcall_uses_shifted_fields(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[96];
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));

    write_arib_vcall_fields(bits, 0xE1U, 0xABU, 0xA0U, 1U, 2U, 0x1234U, 0x4567U, 1U, 0x2AU);

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("arib-vcall-cc-option", state->NxdnElementsContent.CCOption, 0xA0);
    rc |= expect_int("arib-vcall-call-type", state->NxdnElementsContent.CallType, 1);
    rc |= expect_int("arib-vcall-voice-option", state->NxdnElementsContent.VoiceCallOption, 2);
    rc |= expect_int("arib-vcall-src", state->NxdnElementsContent.SourceUnitID, 0x1234);
    rc |= expect_int("arib-vcall-dst", state->NxdnElementsContent.DestinationID, 0x4567);
    rc |= expect_int("arib-vcall-cipher", state->NxdnElementsContent.CipherType, 1);
    rc |= expect_int("arib-vcall-key", state->NxdnElementsContent.KeyID, 0x2A);
    rc |= expect_int("arib-vcall-last-rid", state->nxdn_last_rid, 0x1234);
    rc |= expect_int("arib-vcall-last-tg", state->nxdn_last_tg, 0x4567);
    rc |= expect_int("arib-vcall-key-state", state->nxdn_key, 0x2A);
    rc |= expect_int("arib-vcall-gi", state->gi[0], 0);
    rc |= expect_int("arib-vcall-encrypted", state->dmr_encL, 1);
    free(state);
    free(opts);
    return rc;
}

static int
test_vcall_des_keyloader_and_iv_signal(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[96];
    uint8_t iv_bits[96];
    const uint8_t key_id = 0x2AU;
    const uint64_t des_key = 0x0123456789ABCDEFULL;
    const uint64_t iv = 0x1122334455667788ULL;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(iv_bits, 0, sizeof(iv_bits));

    state->keyloader = 1;
    state->rkey_array[key_id] = des_key;
    write_vcall_fields(bits, 0x01U, 0x20U, 1U, 2U, 0x1234U, 0x4567U, 2U, key_id);
    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("vcall-des-cipher", state->nxdn_cipher_type, 2);
    rc |= expect_int("vcall-des-key-id", state->nxdn_key, key_id);
    rc |= expect_u64("vcall-des-key", (uint64_t)state->R, des_key);
    rc |= expect_int("vcall-des-current-frame", state->NxdnElementsContent.PartOfCurrentEncryptedFrame, 1);
    rc |= expect_int("vcall-des-next-frame", state->NxdnElementsContent.PartOfNextEncryptedFrame, 2);
    rc |= expect_int("vcall-des-unmutes-loaded-key", state->dmr_encL, 0);

    write_vcall_iv_fields(iv_bits, iv);
    NXDN_Elements_Content_decode(opts, state, 1U, iv_bits, sizeof(iv_bits));
    rc |= expect_u64("vcall-des-iv", (uint64_t)state->payload_miN, iv);
    rc |= expect_int("vcall-des-new-iv", state->nxdn_new_iv, 1);

    free(state);
    free(opts);
    return rc;
}

static int
test_vcall_aes_keyloader_and_iv_signal(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[96];
    uint8_t iv_bits[96];
    const uint8_t key_id = 0x13U;
    const uint64_t a1 = 0x0102030405060708ULL;
    const uint64_t a2 = 0x1112131415161718ULL;
    const uint64_t a3 = 0x2122232425262728ULL;
    const uint64_t a4 = 0x3132333435363738ULL;
    const uint64_t iv = 0xA1A2A3A4A5A6A7A8ULL;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(iv_bits, 0, sizeof(iv_bits));

    state->keyloader = 1;
    state->rkey_array[key_id + 0x000U] = a1;
    state->rkey_array[key_id + 0x101U] = a2;
    state->rkey_array[key_id + 0x201U] = a3;
    state->rkey_array[key_id + 0x301U] = a4;
    write_vcall_fields(bits, 0x01U, 0x20U, 1U, 2U, 0x1234U, 0x4567U, 3U, key_id);
    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("vcall-aes-cipher", state->nxdn_cipher_type, 3);
    rc |= expect_int("vcall-aes-key-id", state->nxdn_key, key_id);
    rc |= expect_u64("vcall-aes-a1", (uint64_t)state->A1[0], a1);
    rc |= expect_u64("vcall-aes-a2", (uint64_t)state->A2[0], a2);
    rc |= expect_u64("vcall-aes-a3", (uint64_t)state->A3[0], a3);
    rc |= expect_u64("vcall-aes-a4", (uint64_t)state->A4[0], a4);
    rc |= expect_int("vcall-aes-loaded", state->aes_key_loaded[0], 1);
    rc |= expect_u64("vcall-aes-display-key", (uint64_t)state->R, a1);
    rc |= expect_int("vcall-aes-unmutes-loaded-key", state->dmr_encL, 0);
    rc |= expect_int("vcall-aes-key-byte-0", state->aes_key[0], 0x01);
    rc |= expect_int("vcall-aes-key-byte-31", state->aes_key[31], 0x38);

    write_vcall_iv_fields(iv_bits, iv);
    NXDN_Elements_Content_decode(opts, state, 1U, iv_bits, sizeof(iv_bits));
    rc |= expect_u64("vcall-aes-iv", (uint64_t)state->payload_miN, iv);
    rc |= expect_int("vcall-aes-new-iv", state->nxdn_new_iv, 1);

    free(state);
    free(opts);
    return rc;
}

static int
test_arib_tx_release_uses_shifted_fields_and_clears_call(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[96];
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));

    write_arib_vcall_fields(bits, 0xE8U, 0xABU, 0x40U, 4U, 0U, 0x2222U, 0x3333U, 0U, 0x05U);
    state->nxdn_last_rid = 0x7777U;
    state->nxdn_last_tg = 0x8888U;
    state->gi[0] = 1;
    DSD_SNPRINTF(state->generic_talker_alias[0], sizeof(state->generic_talker_alias[0]), "%s", "stale");

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("arib-release-cc-option", state->NxdnElementsContent.CCOption, 0x40);
    rc |= expect_int("arib-release-call-type", state->NxdnElementsContent.CallType, 4);
    rc |= expect_int("arib-release-src", state->NxdnElementsContent.SourceUnitID, 0x2222);
    rc |= expect_int("arib-release-dst", state->NxdnElementsContent.DestinationID, 0x3333);
    rc |= expect_int("arib-release-key", state->NxdnElementsContent.KeyID, 0x05);
    rc |= expect_int("arib-release-last-rid", state->nxdn_last_rid, 0);
    rc |= expect_int("arib-release-last-tg", state->nxdn_last_tg, 0);
    rc |= expect_int("arib-release-gi", state->gi[0], -1);
    rc |= expect_int("arib-release-alias", state->generic_talker_alias[0][0], '\0');
    free(state);
    free(opts);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_sdcall_header_short_is_ignored();
    rc |= test_sdcall_iv_short_type_c_is_ignored();
    rc |= test_sdcall_iv_type_d_min_length_is_accepted();
    rc |= test_prop_form_alias_requires_marker();
    rc |= test_short_dcall_data_is_rejected(0x39U, "sdcall-data-short");
    rc |= test_short_dcall_data_is_rejected(0x0BU, "dcall-data-short");
    rc |= test_dst_id_info_complete_event();
    rc |= test_sdcall_des_data_decrypts_and_resets();
    rc |= test_dcall_aes_data_decrypts_with_manual_key_and_iv();
    rc |= test_arib_vcall_uses_shifted_fields();
    rc |= test_vcall_des_keyloader_and_iv_signal();
    rc |= test_vcall_aes_keyloader_and_iv_signal();
    rc |= test_arib_tx_release_uses_shifted_fields_and_clears_call();

    if (rc == 0) {
        printf("NXDN_ELEMENT_BOUNDS: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
