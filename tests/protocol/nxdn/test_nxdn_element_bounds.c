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

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, uint8_t CrcCorrect, const uint8_t* ElementsContent,
                                  size_t elements_bits);

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
    DSD_MEMSET(output, 0, (size_t)len * sizeof(uint8_t));
    for (int i = 0; i < len; i++) {
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
nxdn_alias_decode_arib(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    (void)crc_ok;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_decode_prop(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    (void)crc_ok;
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
    (void)mi;
    (void)key_ulli;
    (void)output;
    (void)type;
    (void)len;
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
    DSD_MEMSET(g_datacall_event, 0, sizeof(g_datacall_event));
    g_datacall_src = 0;
    g_datacall_dst = 0;
    g_datacall_slot = 0xFFU;

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
    rc |= test_short_dcall_data_is_rejected(0x39U, "sdcall-data-short");
    rc |= test_short_dcall_data_is_rejected(0x0BU, "dcall-data-short");
    rc |= test_dst_id_info_complete_event();
    rc |= test_arib_vcall_uses_shifted_fields();
    rc |= test_arib_tx_release_uses_shifted_fields_and_clears_call();

    if (rc == 0) {
        printf("NXDN_ELEMENT_BOUNDS: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
