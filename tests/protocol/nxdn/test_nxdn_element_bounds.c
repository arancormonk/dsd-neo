// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression checks for NXDN element length guards on short payloads.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/dsp/frame_sync.h>
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
void NXDN_SACCH_Full_decode(dsd_opts* opts, dsd_state* state);

static int g_alias_prop_calls;
static uint8_t g_alias_prop_crc_ok;
static int g_channel_to_frequency_calls;
static int g_channel_to_frequency_quiet_calls;
static uint16_t g_channel_to_frequency_channel;
static uint16_t g_channel_to_frequency_quiet_channel;
static uint16_t g_channel_to_frequency_channels[8];
static uint16_t g_mapped_channel;
static long int g_mapped_channel_freq;

/*
 * Link stubs:
 * Pulling nxdn_element.c directly into this test would duplicate large decoder
 * code paths, so we link against dsd-neo_proto_nxdn and provide focused stubs
 * for external entrypoints that are irrelevant to these bounds checks.
 */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_message_type(const dsd_opts* opts, dsd_state* state, uint8_t MessageType) {
    (void)opts;
    (void)state;
    (void)MessageType;
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
    g_channel_to_frequency_calls++;
    g_channel_to_frequency_channel = channel;
    if (g_channel_to_frequency_calls
        <= (int)(sizeof(g_channel_to_frequency_channels) / sizeof(g_channel_to_frequency_channels[0]))) {
        g_channel_to_frequency_channels[g_channel_to_frequency_calls - 1] = channel;
    }
    return (channel == g_mapped_channel) ? g_mapped_channel_freq : 0;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel) {
    (void)state;
    g_channel_to_frequency_quiet_calls++;
    g_channel_to_frequency_quiet_channel = channel;
    return (channel == g_mapped_channel) ? g_mapped_channel_freq : 0;
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
static int g_des_len;
static uint8_t g_aes_iv[16];
static uint8_t g_aes_key[32];
static dsd_aes_key_size g_aes_key_size;
static int g_aes_blocks;
static int g_tune_cc_calls;
static long int g_tune_cc_freq;
static int g_tune_cc_ted_sps;
static int g_tune_freq_calls;
static long int g_tune_freq_freq;
static int g_tune_freq_ted_sps;
static long int g_current_rigctl_freq;

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
    g_des_len = 0;
    DSD_MEMSET(g_aes_iv, 0, sizeof(g_aes_iv));
    DSD_MEMSET(g_aes_key, 0, sizeof(g_aes_key));
    g_aes_key_size = DSD_AES_KEY_128;
    g_aes_blocks = 0;
}

static void
reset_assignment_capture(void) {
    g_tune_cc_calls = 0;
    g_tune_cc_freq = 0;
    g_tune_cc_ted_sps = -1;
    g_tune_freq_calls = 0;
    g_tune_freq_freq = 0;
    g_tune_freq_ted_sps = -1;
    g_current_rigctl_freq = 0;
    g_channel_to_frequency_calls = 0;
    g_channel_to_frequency_quiet_calls = 0;
    g_channel_to_frequency_channel = 0;
    g_channel_to_frequency_quiet_channel = 0;
    DSD_MEMSET(g_channel_to_frequency_channels, 0, sizeof(g_channel_to_frequency_channels));
    g_mapped_channel = 0;
    g_mapped_channel_freq = 0;
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
des_ofb_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int nblocks) {
    g_des_calls++;
    g_des_mi = mi;
    g_des_key = key_ulli;
    g_des_len = nblocks;
    if (output == NULL || nblocks <= 0 || g_des_fill_enabled == 0) {
        return;
    }
    const size_t output_len = (size_t)nblocks * 8U;
    for (size_t i = 0U; i < output_len; i++) {
        output[i] = des_stub_byte(i);
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
aes_ofb_keystream_output(const uint8_t* iv, const uint8_t* key, uint8_t* output, dsd_aes_key_size key_size,
                         int nblocks) {
    g_aes_calls++;
    g_aes_key_size = key_size;
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
    return g_current_rigctl_freq;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ns(void) {
    return 0ULL;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ms(void) {
    return dsd_time_monotonic_ns() / 1000000U;
}

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                 uint64_t* out_request_id) {
    (void)opts;
    (void)state;
    if (out_request_id != NULL) {
        *out_request_id = 0U;
    }
    g_tune_cc_calls++;
    g_tune_cc_freq = freq;
    g_tune_cc_ted_sps = ted_sps;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_trunk_tuning_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                   uint64_t* out_request_id) {
    (void)state;
    if (out_request_id != NULL) {
        *out_request_id = 0U;
    }
    g_tune_freq_calls++;
    g_tune_freq_freq = freq;
    g_tune_freq_ted_sps = ted_sps;
    if (opts != NULL) {
        opts->trunk_is_tuned = 1;
    }
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
all_sacch_segments_are(uint8_t value, const dsd_state* state) {
    for (size_t frame = 0U; frame < 4U; frame++) {
        if (state->nxdn_sacch_frame_segcrc[frame] != value) {
            return 0;
        }
        for (size_t bit = 0U; bit < 18U; bit++) {
            if (state->nxdn_sacch_frame_segment[frame][bit] != value) {
                return 0;
            }
        }
    }
    return 1;
}

static int
active_channels_are_zero(const dsd_state* state) {
    for (size_t i = 0U; i < 31U; i++) {
        if (state->active_channel[i][0] != '\0') {
            return 0;
        }
    }
    return 1;
}

static void
load_sacch_segments_from_bits(dsd_state* state, const uint8_t* bits) {
    for (size_t frame = 0U; frame < 4U; frame++) {
        DSD_MEMCPY(state->nxdn_sacch_frame_segment[frame], &bits[frame * 18U], 18U);
    }
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
test_decode_guards_and_unknown_dispatch(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[16];
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));

    state->NxdnElementsContent.MessageType = 0x55U;
    state->NxdnElementsContent.VCallCrcIsGood = 1U;
    NXDN_Elements_Content_decode(NULL, state, 0U, bits, sizeof(bits));
    NXDN_Elements_Content_decode(opts, NULL, 0U, bits, sizeof(bits));
    NXDN_Elements_Content_decode(opts, state, 0U, NULL, sizeof(bits));
    NXDN_Elements_Content_decode(opts, state, 0U, bits, 7U);

    int rc = 0;
    rc |= expect_int("decode-guards-type-preserved", state->NxdnElementsContent.MessageType, 0x55);
    rc |= expect_int("decode-guards-crc-preserved", state->NxdnElementsContent.VCallCrcIsGood, 1);

    set_message_type(bits, 0x2AU);
    state->data_header_valid[0] = 1U;
    NXDN_Elements_Content_decode(opts, state, 0U, bits, sizeof(bits));
    rc |= expect_int("unknown-type-recorded", state->NxdnElementsContent.MessageType, 0x2A);
    rc |= expect_int("unknown-crc-recorded", state->NxdnElementsContent.VCallCrcIsGood, 0);
    rc |= expect_int("unknown-keeps-data-state", state->data_header_valid[0], 1);

    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

static int
test_sacch_full_decode_crc_gate_and_reset(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[72];
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
    write_bits_u64(bits, 10U, 3U, 6U);
    write_ascii_bits(bits, 16U, "NODE");
    load_sacch_segments_from_bits(state, bits);
    state->nxdn_sacch_frame_segcrc[0] = 0U;
    state->nxdn_sacch_frame_segcrc[1] = 1U;
    state->nxdn_sacch_frame_segcrc[2] = 0U;
    state->nxdn_sacch_frame_segcrc[3] = 0U;

    NXDN_SACCH_Full_decode(opts, state);

    int rc = 0;
    rc |= expect_string("sacch-bad-crc-no-event", g_datacall_event, "");
    rc |= expect_int("sacch-bad-crc-reset", all_sacch_segments_are(1U, state), 1);

    load_sacch_segments_from_bits(state, bits);
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 0, sizeof(state->nxdn_sacch_frame_segcrc));
    reset_datacall_capture();

    NXDN_SACCH_Full_decode(opts, state);

    rc |= expect_string("sacch-good-crc-event", g_datacall_event, "NXDN Digital Station ID: NODE");
    rc |= expect_int("sacch-good-crc-reset", all_sacch_segments_are(1U, state), 1);
    rc |= expect_int("sacch-recorded-type", state->NxdnElementsContent.MessageType, 0x17);

    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

static int
test_disc_trunk_return_clears_call_state(void) {
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
    set_message_type(bits, 0x11U);
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 851012500L;
    state->nxdn_last_rid = 0x1234;
    state->nxdn_last_tg = 0x4567;
    state->nxdn_cipher_type = 2;
    state->data_header_valid[0] = 1;
    state->data_header_blocks[0] = 7;
    state->payload_algid = 3;
    state->payload_keyid = 44;
    state->payload_mi = 0x1122334455667788ULL;
    state->dmr_lrrp_source[0] = 99;
    state->dmr_lrrp_target[0] = 100;
    DSD_SNPRINTF(state->call_string[0], sizeof(state->call_string[0]), "%s", "active");
    DSD_SNPRINTF(state->nxdn_call_type, sizeof(state->nxdn_call_type), "%s", "Group");
    DSD_SNPRINTF(state->active_channel[3], sizeof(state->active_channel[3]), "%s", "busy");
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 0, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 0, sizeof(state->nxdn_sacch_frame_segcrc));
    g_alias_prop_calls = 0;
    g_tune_cc_calls = 0;
    g_tune_cc_freq = 0;
    g_tune_cc_ted_sps = -1;

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("disc-tune-cc-called", g_tune_cc_calls, 1);
    rc |= expect_int("disc-tune-cc-freq", (int)g_tune_cc_freq, (int)851012500L);
    rc |= expect_int("disc-tune-cc-sps", g_tune_cc_ted_sps, 0);
    rc |= expect_int("disc-trunk-cleared", opts->trunk_is_tuned, 0);
    rc |= expect_int("disc-rid-reset", state->nxdn_last_rid, 0);
    rc |= expect_int("disc-tg-reset", state->nxdn_last_tg, 0);
    rc |= expect_int("disc-cipher-reset", state->nxdn_cipher_type, 0);
    rc |= expect_int("disc-call-string-cleared", state->call_string[0][0], '\0');
    rc |= expect_int("disc-call-type-cleared", state->nxdn_call_type[0], '\0');
    rc |= expect_int("disc-data-valid-reset", state->data_header_valid[0], 0);
    rc |= expect_int("disc-data-blocks-reset", state->data_header_blocks[0], 1);
    rc |= expect_int("disc-payload-alg-reset", state->payload_algid, 0);
    rc |= expect_int("disc-payload-key-reset", state->payload_keyid, 0);
    rc |= expect_u64("disc-payload-mi-reset", (uint64_t)state->payload_mi, 0ULL);
    rc |= expect_int("disc-lrrp-src-reset", state->dmr_lrrp_source[0], 0);
    rc |= expect_int("disc-lrrp-tgt-reset", state->dmr_lrrp_target[0], 0);
    rc |= expect_int("disc-sacch-reset", all_sacch_segments_are(1U, state), 1);
    rc |= expect_int("disc-active-channels-reset", active_channels_are_zero(state), 1);

    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
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
test_srv_info_anchors_control_channel_from_rigctl(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[96];
    const uint32_t location_id = (1U << 12U) | 0x234U;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));

    set_message_type(bits, 0x19U);
    write_bits_u64(bits, 8U, location_id, 24U);
    write_bits_u64(bits, 32U, 0x1200U, 16U);
    write_bits_u64(bits, 48U, 0x345678U, 24U);
    opts->trunk_enable = 1;
    opts->use_rigctl = 1;
    state->p25_cc_freq = 851012500L;
    state->trunk_cc_freq = 851012500L;
    state->nxdn_last_rid = 0x1111U;
    state->nxdn_last_tg = 0x2222U;
    state->nxdn_grant_chan = 77U;
    DSD_SNPRINTF(state->active_channel[3], sizeof(state->active_channel[3]), "%s", "stale");
    g_current_rigctl_freq = 855262500L;

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("srv-info-message-type", state->NxdnElementsContent.MessageType, 0x19);
    rc |= expect_int("srv-info-last-rid-reset", state->nxdn_last_rid, 0);
    rc |= expect_int("srv-info-last-tg-reset", state->nxdn_last_tg, 0);
    rc |= expect_int("srv-info-ran", state->nxdn_last_ran, 0x34);
    rc |= expect_int("srv-info-site-code", state->nxdn_location_site_code, 0x234);
    rc |= expect_int("srv-info-sys-code", state->nxdn_location_sys_code, 1);
    rc |= expect_string("srv-info-category", state->nxdn_location_category, "Global");
    rc |= expect_int("srv-info-cc-freq", (int)state->p25_cc_freq, (int)855262500L);
    rc |= expect_int("srv-info-trunk-cc-freq", (int)state->trunk_cc_freq, (int)855262500L);
    rc |= expect_int("srv-info-active-channels-reset", active_channels_are_zero(state), 1);
    rc |= expect_int("srv-info-grant-chan-reset", state->nxdn_grant_chan, 0);

    g_current_rigctl_freq = 0;
    free(state);
    free(opts);
    return rc;
}

static int
test_cch_dfa_maps_secondary_channels_and_seeds_control_frequency(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[128];
    const uint32_t location_id = (2U << 12U) | 0x021U;
    const uint16_t ofn1 = 0x1221U;
    const uint16_t ifn1 = 0x2332U;
    const uint16_t ofn2 = 0x3443U;
    const uint16_t ifn2 = 0x4554U;
    const long int cc_freq = 852262500L;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));
    reset_assignment_capture();

    state->nxdn_rcn = 1;
    g_mapped_channel = ofn1;
    g_mapped_channel_freq = cc_freq;

    set_message_type(bits, 0x1AU);
    write_bits_u64(bits, 8U, location_id, 24U);
    write_bits_u64(bits, 32U, 0x13U, 6U);
    write_bits_u64(bits, 38U, 1U, 2U);
    write_bits_u64(bits, 40U, ofn1, 16U);
    write_bits_u64(bits, 56U, ifn1, 16U);
    write_bits_u64(bits, 80U, ofn2, 16U);
    write_bits_u64(bits, 96U, ifn2, 16U);

    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("cch-dfa-message-type", state->NxdnElementsContent.MessageType, 0x1A);
    rc |= expect_int("cch-dfa-ran", state->nxdn_last_ran, 0x21);
    rc |= expect_int("cch-dfa-site-code", state->nxdn_location_site_code, 0x21);
    rc |= expect_int("cch-dfa-sys-code", state->nxdn_location_sys_code, 2);
    rc |= expect_string("cch-dfa-category", state->nxdn_location_category, "Global");
    rc |= expect_int("cch-dfa-frequency-calls", g_channel_to_frequency_calls, 4);
    rc |= expect_int("cch-dfa-call-ofn2", g_channel_to_frequency_channels[0], ofn2);
    rc |= expect_int("cch-dfa-call-ifn2", g_channel_to_frequency_channels[1], ifn2);
    rc |= expect_int("cch-dfa-call-ofn1", g_channel_to_frequency_channels[2], ofn1);
    rc |= expect_int("cch-dfa-call-ifn1", g_channel_to_frequency_channels[3], ifn1);
    rc |= expect_int("cch-dfa-last-channel", g_channel_to_frequency_channel, ifn1);
    rc |= expect_int("cch-dfa-lcn-freq", (int)state->trunk_lcn_freq[0], (int)cc_freq);
    rc |= expect_int("cch-dfa-p25-cc", (int)state->p25_cc_freq, (int)cc_freq);
    rc |= expect_int("cch-dfa-trunk-cc", (int)state->trunk_cc_freq, (int)cc_freq);
    rc |= expect_int("cch-dfa-lcn-count", state->lcn_freq_count, 1);

    free(state);
    free(opts);
    return rc;
}

static int
test_adj_site_skips_disabled_entries_for_channel_and_dfa_versions(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t ch_bits[128];
    uint8_t dfa_bits[112];
    const uint32_t site1 = (3U << 12U) | 0x011U;
    const uint32_t site2 = (4U << 12U) | 0x022U;
    const uint32_t site3 = (5U << 12U) | 0x033U;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(ch_bits, 0, sizeof(ch_bits));
    DSD_MEMSET(dfa_bits, 0, sizeof(dfa_bits));
    reset_assignment_capture();

    set_message_type(ch_bits, 0x1BU);
    write_bits_u64(ch_bits, 8U, site1, 24U);
    write_bits_u64(ch_bits, 32U, 0x10U, 6U);
    write_bits_u64(ch_bits, 38U, 0x111U, 10U);
    write_bits_u64(ch_bits, 48U, site2, 24U);
    write_bits_u64(ch_bits, 72U, 0x12U, 6U);
    write_bits_u64(ch_bits, 78U, 0x222U, 10U);
    write_bits_u64(ch_bits, 88U, site3, 24U);
    write_bits_u64(ch_bits, 112U, 0x03U, 6U);
    write_bits_u64(ch_bits, 118U, 0x333U, 10U);

    NXDN_Elements_Content_decode(opts, state, 1U, ch_bits, sizeof(ch_bits));

    int rc = 0;
    rc |= expect_int("adj-site-ch-calls", g_channel_to_frequency_calls, 2);
    rc |= expect_int("adj-site-ch-first", g_channel_to_frequency_channels[0], 0x222);
    rc |= expect_int("adj-site-ch-second", g_channel_to_frequency_channels[1], 0x333);
    rc |= expect_int("adj-site-ch-last", g_channel_to_frequency_channel, 0x333);
    rc |= expect_int("adj-site-current-site-preserved", state->nxdn_location_site_code, 0);
    rc |= expect_int("adj-site-current-sys-preserved", state->nxdn_location_sys_code, 0);

    reset_assignment_capture();
    state->nxdn_rcn = 1;
    set_message_type(dfa_bits, 0x1BU);
    write_bits_u64(dfa_bits, 8U, site1, 24U);
    write_bits_u64(dfa_bits, 32U, 0x20U, 6U);
    write_bits_u64(dfa_bits, 38U, 2U, 2U);
    write_bits_u64(dfa_bits, 40U, 0x1771U, 16U);
    write_bits_u64(dfa_bits, 56U, site2, 24U);
    write_bits_u64(dfa_bits, 80U, 0x04U, 6U);
    write_bits_u64(dfa_bits, 86U, 1U, 2U);
    write_bits_u64(dfa_bits, 88U, 0x2882U, 16U);

    NXDN_Elements_Content_decode(opts, state, 1U, dfa_bits, sizeof(dfa_bits));

    rc |= expect_int("adj-site-dfa-calls", g_channel_to_frequency_calls, 1);
    rc |= expect_int("adj-site-dfa-channel", g_channel_to_frequency_channel, 0x2882);
    rc |= expect_int("adj-site-dfa-first", g_channel_to_frequency_channels[0], 0x2882);
    rc |= expect_int("adj-site-dfa-current-site-preserved", state->nxdn_location_site_code, 0);
    rc |= expect_int("adj-site-dfa-current-sys-preserved", state->nxdn_location_sys_code, 0);

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
        0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU, 0xDEU, 0xF0U, 0x10U, 0x32U, 0x54U, 0x76U, 0x22U, 0x4EU, 0x9CU, 0xA1U,
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
        0xABU, 0xCDU, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U, 0x99U, 0xAAU, 0x3BU, 0x9BU, 0x17U, 0xDDU,
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
    rc |= expect_int("dcall-aes-key-size", g_aes_key_size, DSD_AES_KEY_256);
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
write_assignment_fields(uint8_t* bits, uint8_t message_type, uint8_t cc_option, uint8_t call_type,
                        uint8_t voice_call_option, uint16_t source, uint16_t destination, uint16_t channel,
                        uint16_t ofn) {
    set_message_type(bits, message_type);
    write_bits_u64(bits, 8U, cc_option, 8U);
    write_bits_u64(bits, 16U, call_type, 3U);
    write_bits_u64(bits, 19U, voice_call_option, 5U);
    write_bits_u64(bits, 24U, source, 16U);
    write_bits_u64(bits, 40U, destination, 16U);
    write_bits_u64(bits, 62U, channel, 10U);
    if (ofn != 0U) {
        write_bits_u64(bits, 64U, ofn, 16U);
    }
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
test_bad_crc_encrypted_vcall_records_metadata(void) {
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

    write_vcall_fields(bits, 0x01U, 0x20U, 1U, 2U, 0x1357U, 0x2468U, 3U, 0x09U);
    NXDN_Elements_Content_decode(opts, state, 0U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("bad-crc-vcall-type", state->NxdnElementsContent.MessageType, 0x01);
    rc |= expect_int("bad-crc-vcall-crc", state->NxdnElementsContent.VCallCrcIsGood, 0);
    rc |= expect_int("bad-crc-vcall-src", state->NxdnElementsContent.SourceUnitID, 0x1357);
    rc |= expect_int("bad-crc-vcall-dst", state->NxdnElementsContent.DestinationID, 0x2468);
    rc |= expect_int("bad-crc-vcall-cipher", state->NxdnElementsContent.CipherType, 3);
    rc |= expect_int("bad-crc-vcall-key", state->NxdnElementsContent.KeyID, 0x09);
    rc |= expect_int("bad-crc-vcall-last-rid", state->nxdn_last_rid, 0x1357);
    rc |= expect_int("bad-crc-vcall-last-tg", state->nxdn_last_tg, 0x2468);
    rc |= expect_int("bad-crc-vcall-cipher-state", state->nxdn_cipher_type, 3);
    rc |= expect_int("bad-crc-vcall-lockout", state->dmr_encL, 1);
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
test_vcall_scrambler_keyloader_uses_active_nxdn48_profile(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[96];
    const uint8_t key_id = 0x2AU;
    const uint64_t scrambler_key = 0x5A5AU;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));

    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state->keyloader = 1;
    state->rkey_array[key_id] = scrambler_key;
    write_vcall_fields(bits, 0x01U, 0x20U, 1U, 2U, 0x1234U, 0x4567U, 1U, key_id);
    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("vcall-scrambler-active-variant", dsd_frame_sync_active_nxdn_variant(opts, state),
                     DSD_NXDN_VARIANT_48);
    rc |= expect_int("vcall-scrambler-cipher", state->nxdn_cipher_type, 1);
    rc |= expect_u64("vcall-scrambler-key", (uint64_t)state->R, scrambler_key);
    rc |= expect_int("vcall-scrambler-unmutes-loaded-key", state->dmr_encL, 0);

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

static int
test_assignment_group_grant_anchors_tunes_and_loads_scrambler(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t bits[96];
    const uint16_t channel = 0x12AU;
    const long int grant_freq = 855512500L;
    const long int control_freq = 851012500L;
    const uint16_t source = 0x0123U;
    const uint16_t target = 0x0456U;
    const unsigned long long scrambler_key = 12345ULL;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(bits, 0, sizeof(bits));
    reset_assignment_capture();

    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->use_rigctl = 1;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->M = 1;
    state->rkey_array[target] = scrambler_key;
    g_current_rigctl_freq = control_freq;
    g_mapped_channel = channel;
    g_mapped_channel_freq = grant_freq;

    write_assignment_fields(bits, 0x04U, 0x80U, 1U, 0U, source, target, channel, 0U);
    NXDN_Elements_Content_decode(opts, state, 1U, bits, sizeof(bits));

    int rc = 0;
    rc |= expect_int("assignment-group-quiet-channel", g_channel_to_frequency_quiet_channel, channel);
    rc |= expect_int("assignment-group-channel", g_channel_to_frequency_channel, channel);
    rc |= expect_int("assignment-group-tune-calls", g_tune_freq_calls, 1);
    rc |= expect_int("assignment-group-tune-freq", (int)g_tune_freq_freq, (int)grant_freq);
    rc |= expect_int("assignment-group-tune-sps", g_tune_freq_ted_sps, 0);
    rc |= expect_int("assignment-group-cc-freq", (int)state->p25_cc_freq, (int)control_freq);
    rc |= expect_int("assignment-group-trunk-cc-freq", (int)state->trunk_cc_freq, (int)control_freq);
    rc |= expect_int("assignment-group-grant-channel", state->nxdn_grant_chan, channel);
    rc |= expect_int("assignment-group-grant-freq", (int)state->nxdn_grant_freq, (int)grant_freq);
    rc |= expect_contains("assignment-group-active", state->active_channel[0], "Active Ch: 298");
    rc |= expect_contains("assignment-group-active-tg", state->active_channel[0], "TG: 1110 SRC: 291");
    rc |= expect_string("assignment-group-call-type", state->nxdn_call_type, "Group Call");
    rc |= expect_string("assignment-group-call-string", state->call_string[0], "Group Call Emergency");
    rc |= expect_u64("assignment-group-key", (uint64_t)state->R, (uint64_t)scrambler_key);
    rc |= expect_u64("assignment-group-mi", (uint64_t)state->payload_miN, (uint64_t)scrambler_key);
    rc |= expect_int("assignment-group-m-cipher", state->nxdn_cipher_type, 1);
    rc |= expect_int("assignment-group-sync-reset", state->lastsynctype, DSD_SYNC_NONE);
    rc |= expect_int("assignment-group-sacch-reset", all_sacch_segments_are(1U, state), 1);

    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

static int
test_assignment_data_gate_and_duplicate_release(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    uint8_t data_bits[96];
    uint8_t dup_bits[96];
    const uint16_t data_channel = 0x0088U;
    const uint16_t voice_channel = 0x0055U;
    const long int data_freq = 856112500L;
    const long int voice_freq = 857112500L;
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    DSD_MEMSET(data_bits, 0, sizeof(data_bits));
    DSD_MEMSET(dup_bits, 0, sizeof(dup_bits));
    reset_assignment_capture();

    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_data_calls = 0;
    state->p25_cc_freq = 851012500L;
    state->trunk_cc_freq = 851012500L;
    g_mapped_channel = data_channel;
    g_mapped_channel_freq = data_freq;

    write_assignment_fields(data_bits, 0x0DU, 0x00U, 1U, 3U, 0x0201U, 0x0302U, data_channel, 0U);
    NXDN_Elements_Content_decode(opts, state, 1U, data_bits, sizeof(data_bits));

    int rc = 0;
    rc |= expect_int("assignment-data-grant-channel", state->nxdn_grant_chan, data_channel);
    rc |= expect_int("assignment-data-grant-freq", (int)state->nxdn_grant_freq, (int)data_freq);
    rc |= expect_int("assignment-data-no-tune", g_tune_freq_calls, 0);
    rc |= expect_contains("assignment-data-active", state->active_channel[0], "TG: 770 SRC: 513");

    reset_assignment_capture();
    opts->trunk_tune_data_calls = 1;
    opts->trunk_is_tuned = 1;
    opts->trunk_hangtime = 0;
    state->last_vc_sync_time = 0;
    state->lastsynctype = DSD_SYNC_NXDN_NEG;
    g_mapped_channel = voice_channel;
    g_mapped_channel_freq = voice_freq;

    write_assignment_fields(dup_bits, 0x05U, 0x00U, 1U, 2U, 0x0401U, 0x0502U, voice_channel, 0U);
    NXDN_Elements_Content_decode(opts, state, 1U, dup_bits, sizeof(dup_bits));

    rc |= expect_int("assignment-dup-channel", state->nxdn_grant_chan, voice_channel);
    rc |= expect_int("assignment-dup-tune-calls", g_tune_freq_calls, 1);
    rc |= expect_int("assignment-dup-tune-freq", (int)g_tune_freq_freq, (int)voice_freq);
    rc |= expect_int("assignment-dup-sync-reset", state->lastsynctype, DSD_SYNC_NONE);
    rc |= expect_string("assignment-dup-call-string", state->call_string[0], "Group Call");
    rc |= expect_contains("assignment-dup-active", state->active_channel[0], "TG: 1282 SRC: 1025");

    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_decode_guards_and_unknown_dispatch();
    rc |= test_sacch_full_decode_crc_gate_and_reset();
    rc |= test_disc_trunk_return_clears_call_state();
    rc |= test_sdcall_header_short_is_ignored();
    rc |= test_sdcall_iv_short_type_c_is_ignored();
    rc |= test_sdcall_iv_type_d_min_length_is_accepted();
    rc |= test_prop_form_alias_requires_marker();
    rc |= test_short_dcall_data_is_rejected(0x39U, "sdcall-data-short");
    rc |= test_short_dcall_data_is_rejected(0x0BU, "dcall-data-short");
    rc |= test_dst_id_info_complete_event();
    rc |= test_srv_info_anchors_control_channel_from_rigctl();
    rc |= test_cch_dfa_maps_secondary_channels_and_seeds_control_frequency();
    rc |= test_adj_site_skips_disabled_entries_for_channel_and_dfa_versions();
    rc |= test_sdcall_des_data_decrypts_and_resets();
    rc |= test_dcall_aes_data_decrypts_with_manual_key_and_iv();
    rc |= test_arib_vcall_uses_shifted_fields();
    rc |= test_bad_crc_encrypted_vcall_records_metadata();
    rc |= test_vcall_des_keyloader_and_iv_signal();
    rc |= test_vcall_scrambler_keyloader_uses_active_nxdn48_profile();
    rc |= test_vcall_aes_keyloader_and_iv_signal();
    rc |= test_arib_tx_release_uses_shifted_fields_and_clears_call();
    rc |= test_assignment_group_grant_anchors_tunes_and_loads_scrambler();
    rc |= test_assignment_data_gate_and_duplicate_release();

    if (rc == 0) {
        printf("NXDN_ELEMENT_BOUNDS: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
