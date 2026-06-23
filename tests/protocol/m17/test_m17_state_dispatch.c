// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "fixtures/m17_reference_vectors.h"

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/protocol/m17/m17_parse.h"
#include "dsd-neo/protocol/m17/m17_tables.h"
#include "m17_algorithms.h"

struct CODEC2;

static dsd_opts g_opts;
static dsd_state g_state;
static int g_codec2_decode_calls;
static unsigned char g_codec2_last_bits[8];
static int g_udp_audio_calls;
static size_t g_udp_audio_last_nsam;
static const dsd_opts* g_udp_audio_last_opts;
static dsd_state* g_udp_audio_last_state;
static short g_udp_audio_last_first_sample;
static int g_conv_start_calls;
static int g_conv_decode_calls;
static int g_conv_chainback_calls;
static unsigned int g_conv_chainback_bits;
static uint8_t g_conv_first_symbols[8];

enum { TEST_M17_LSF_BITS = M17_LSF_BYTES * 8U };

// NOLINTNEXTLINE(misc-use-internal-linkage)
uint64_t ConvertBitIntoBytes(const uint8_t* bits, uint32_t n);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void CNXDNConvolution_start(void);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void CNXDNConvolution_decode(uint8_t s0, uint8_t s1);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void codec2_decode(struct CODEC2* codec2_state, short speech[], const unsigned char* bits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void codec2_encode(struct CODEC2* codec2_state, unsigned char* bits, short speech[]);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int codec2_samples_per_frame(struct CODEC2* codec2_state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
uint16_t ComputeCrcCCITT16d(const uint8_t* buf, uint32_t len);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int Connect(char* hostname, int portno);

uint64_t
ConvertBitIntoBytes(const uint8_t* bits, uint32_t n) {
    uint64_t value = 0ULL;
    for (uint32_t i = 0U; i < n; i++) {
        value = (value << 1U) | (uint64_t)(bits[i] & 1U);
    }
    return value;
}

void
CNXDNConvolution_start(void) {
    g_conv_start_calls++;
}

void
CNXDNConvolution_decode(uint8_t s0, uint8_t s1) {
    const int index = g_conv_decode_calls * 2;
    if (index + 1 < (int)sizeof(g_conv_first_symbols)) {
        g_conv_first_symbols[index] = s0;
        g_conv_first_symbols[index + 1] = s1;
    }
    g_conv_decode_calls++;
}

void
CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits) {
    g_conv_chainback_calls++;
    g_conv_chainback_bits = nBits;
    if (out != NULL) {
        const unsigned int byte_count = (nBits + 7U) / 8U;
        for (unsigned int i = 0U; i < byte_count; i++) {
            out[i] = (unsigned char)(0xA0U + i);
        }
    }
}

void
codec2_decode(struct CODEC2* codec2_state, short speech[], const unsigned char* bits) {
    (void)codec2_state;
    g_codec2_decode_calls++;
    if (bits != NULL) {
        DSD_MEMCPY(g_codec2_last_bits, bits, sizeof(g_codec2_last_bits));
    }
    if (speech != NULL) {
        for (size_t i = 0U; i < 160U; i++) {
            speech[i] = (short)(1000 + g_codec2_decode_calls + (int)i);
        }
    }
}

void
codec2_encode(struct CODEC2* codec2_state, unsigned char* bits, short speech[]) {
    (void)codec2_state;
    (void)bits;
    (void)speech;
}

int
codec2_samples_per_frame(struct CODEC2* codec2_state) {
    (void)codec2_state;
    return 0;
}

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

uint16_t
ComputeCrcCCITT16d(const uint8_t* buf, uint32_t len) {
    (void)buf;
    (void)len;
    return 0U;
}

int
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return -1;
}

static void
fake_udp_audio_blast(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    g_udp_audio_calls++;
    g_udp_audio_last_opts = opts;
    g_udp_audio_last_state = state;
    g_udp_audio_last_nsam = nsam;
    g_udp_audio_last_first_sample = (data != NULL) ? ((const short*)data)[0] : 0;
}

static void
reset_audio_fakes(void) {
    dsd_udp_audio_hooks hooks = {0};
    dsd_udp_audio_hooks_set(hooks);
    g_codec2_decode_calls = 0;
    DSD_MEMSET(g_codec2_last_bits, 0, sizeof(g_codec2_last_bits));
    g_udp_audio_calls = 0;
    g_udp_audio_last_nsam = 0U;
    g_udp_audio_last_opts = NULL;
    g_udp_audio_last_state = NULL;
    g_udp_audio_last_first_sample = 0;
}

static void
reset_convolution_fake(void) {
    g_conv_start_calls = 0;
    g_conv_decode_calls = 0;
    g_conv_chainback_calls = 0;
    g_conv_chainback_bits = 0U;
    DSD_MEMSET(g_conv_first_symbols, 0, sizeof(g_conv_first_symbols));
}

static void
install_fake_udp_audio(void) {
    dsd_udp_audio_hooks hooks = {0};
    hooks.blast = fake_udp_audio_blast;
    dsd_udp_audio_hooks_set(hooks);
}

static void
bytes_to_bits(const uint8_t* bytes, uint8_t* bits, size_t byte_count) {
    for (size_t byte = 0U; byte < byte_count; byte++) {
        for (size_t bit = 0U; bit < 8U; bit++) {
            bits[(byte * 8U) + bit] = (uint8_t)((bytes[byte] >> (7U - bit)) & 1U);
        }
    }
}

static int
expect_u64(const char* label, unsigned long long got, unsigned long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%llX want 0x%llX\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u8(const char* label, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", label, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_bytes(const char* label, const uint8_t* got, const uint8_t* want, size_t n) {
    if (memcmp(got, want, n) != 0) {
        DSD_FPRINTF(stderr, "%s: byte mismatch\n", label);
        return 1;
    }
    return 0;
}

static unsigned long long
bits_to_u64(const uint8_t* bits, size_t n) {
    unsigned long long value = 0ULL;
    for (size_t i = 0U; i < n; i++) {
        value = (value << 1U) | (unsigned long long)(bits[i] & 1U);
    }
    return value;
}

static void
write_bits_from_u64(uint8_t* bits, unsigned long long value, size_t n) {
    for (size_t i = 0U; i < n; i++) {
        const size_t shift = n - 1U - i;
        bits[i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
pack_bits_msb(const uint8_t* bits, size_t bit_count, uint8_t* bytes, size_t byte_count) {
    DSD_MEMSET(bytes, 0, byte_count);
    for (size_t bit = 0U; bit < bit_count; bit++) {
        if ((bits[bit] & 1U) != 0U) {
            bytes[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
        }
    }
}

static void
expect_signature_slice(uint8_t* out, size_t offset) {
    bytes_to_bits(M17_REF_SIGNATURE_BYTES + offset, out, 16U);
}

static void
build_lsf_bits(uint8_t lsf_bits[TEST_M17_LSF_BITS], unsigned long long dst, unsigned long long src, uint16_t type_word,
               const uint8_t meta[M17_AES_NONCE_BYTES]) {
    DSD_MEMSET(lsf_bits, 0, TEST_M17_LSF_BITS);
    dsd_neo_m17_test_load_lsf_callsigns(lsf_bits, dst, src);
    write_bits_from_u64(lsf_bits + 96, type_word, 16U);
    if (meta != NULL) {
        for (size_t i = 0U; i < M17_AES_NONCE_BYTES; i++) {
            write_bits_from_u64(lsf_bits + 112U + (i * 8U), meta[i], 8U);
        }
    }
}

static void
build_encoded_lich_chunk(const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint8_t lich_counter,
                         uint8_t encoded[M17_LICH_BITS]) {
    uint8_t content[M17_LICH_CONTENT_BITS];
    DSD_MEMSET(content, 0, sizeof(content));
    DSD_MEMSET(encoded, 0, M17_LICH_BITS);
    assert(m17_lich_build_content(lsf_bits, lich_counter, content) == 0);
    m17_lich_encode_bits(content, encoded);
}

static void
build_ip_stream_frame(uint8_t ip_frame[54], const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint16_t sid, uint16_t fn,
                      uint8_t eot, const uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS]) {
    uint8_t ip_bits[52U * 8U];
    DSD_MEMSET(ip_bits, 0, sizeof(ip_bits));

    write_bits_from_u64(ip_bits, (unsigned long long)'M', 8U);
    write_bits_from_u64(ip_bits + 8, (unsigned long long)'1', 8U);
    write_bits_from_u64(ip_bits + 16, (unsigned long long)'7', 8U);
    write_bits_from_u64(ip_bits + 24, (unsigned long long)' ', 8U);
    write_bits_from_u64(ip_bits + 32, sid, 16U);
    for (size_t i = 0U; i < M17_LSF_LSD_BITS; i++) {
        ip_bits[48U + i] = lsf_bits[i];
    }
    ip_bits[272] = (uint8_t)(eot & 1U);
    write_bits_from_u64(ip_bits + 273, fn & 0x7FFFU, 15U);
    for (size_t i = 0U; i < M17_STREAM_PAYLOAD_BITS; i++) {
        ip_bits[288U + i] = payload_bits[i];
    }

    pack_bits_msb(ip_bits, sizeof(ip_bits), ip_frame, 52U);
    const uint16_t crc = m17_crc16(ip_frame, 52U);
    ip_frame[52] = (uint8_t)((crc >> 8U) & 0xFFU);
    ip_frame[53] = (uint8_t)(crc & 0xFFU);
}

static size_t
build_ip_mpkt_frame(uint8_t* ip_frame, size_t ip_frame_len, const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint16_t sid,
                    const uint8_t* app, size_t app_len) {
    const size_t frame_len = 34U + app_len + 3U;
    if (ip_frame == NULL || lsf_bits == NULL || app == NULL || ip_frame_len < frame_len) {
        return 0U;
    }

    DSD_MEMSET(ip_frame, 0, ip_frame_len);
    ip_frame[0] = 'M';
    ip_frame[1] = 'P';
    ip_frame[2] = 'K';
    ip_frame[3] = 'T';
    ip_frame[4] = (uint8_t)((sid >> 8U) & 0xFFU);
    ip_frame[5] = (uint8_t)(sid & 0xFFU);
    pack_bits_msb(lsf_bits, M17_LSF_LSD_BITS, ip_frame + 6, M17_LSF_LSD_BYTES);
    DSD_MEMCPY(ip_frame + 34, app, app_len);

    const uint16_t crc = m17_crc16(ip_frame, (uint16_t)(frame_len - 2U));
    ip_frame[frame_len - 2U] = (uint8_t)((crc >> 8U) & 0xFFU);
    ip_frame[frame_len - 1U] = (uint8_t)(crc & 0xFFU);
    return frame_len;
}

static struct m17_lsf_result
valid_lsf_result(void) {
    struct m17_lsf_result res;
    DSD_MEMSET(&res, 0, sizeof(res));
    res.dst = 0x0000009FDD51ULL;
    res.src = 0x0000009FDD51ULL;
    res.type_word = 0x0555U;
    res.packet_stream = 1U;
    res.dt = 2U;
    res.et = 2U;
    res.es = 0U;
    res.cn = 9U;
    res.signature = 1U;
    res.meta_is_iv = 1U;
    res.dst_is_valid = 1U;
    res.src_is_valid = 1U;
    res.type_reserved_valid = 1U;
    DSD_MEMCPY(res.dst_csd, "AB1CD", 6U);
    DSD_MEMCPY(res.src_csd, "AB1CD", 6U);
    DSD_MEMCPY(res.meta, M17_REF_AES_NONCE, sizeof(res.meta));
    return res;
}

static int
test_embedded_lich_chunks_store_and_finalize_lsf_state(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t encoded[M17_LICH_BITS];

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = dsd_neo_m17_test_compose_frame_info(1U, 2U, 0U, 0U, 6U, 1U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    (void)dsd_neo_m17_test_attach_lsf_crc(lsf_bits, lsf_packed);

    int err = 0;
    for (uint8_t chunk = 0U; chunk < (M17_LICH_CHUNKS - 1U); chunk++) {
        build_encoded_lich_chunk(lsf_bits, chunk, encoded);
        err |= expect_int("LICH chunk accepted", dsd_neo_m17_test_process_lich(state, opts, encoded), 0);
        for (size_t bit = 0U; bit < M17_LICH_CHUNK_BITS; bit++) {
            err |= expect_u8("LICH chunk stored", state->m17_lsf[((size_t)chunk * M17_LICH_CHUNK_BITS) + bit],
                             lsf_bits[((size_t)chunk * M17_LICH_CHUNK_BITS) + bit]);
        }
        err |= expect_u64("LICH does not decode before final chunk", state->m17_dst, 0ULL);
    }

    build_encoded_lich_chunk(lsf_bits, (uint8_t)(M17_LICH_CHUNKS - 1U), encoded);
    err |= expect_int("final LICH chunk accepted", dsd_neo_m17_test_process_lich(state, opts, encoded), 0);
    err |= expect_u64("final LICH decodes dst", state->m17_dst, dst);
    err |= expect_u64("final LICH decodes src", state->m17_src, src);
    err |= expect_u8("final LICH decodes data type", state->m17_str_dt, 2U);
    err |= expect_u8("final LICH decodes CAN", state->m17_can, 6U);
    err |= expect_u8("final LICH clear encryption", state->m17_enc, 0U);
    err |= expect_u8("final LICH advertises signature", state->m17_signature_advertised, 1U);
    for (size_t bit = 0U; bit < TEST_M17_LSF_BITS; bit++) {
        err |= expect_u8("final LICH clears staged LSF", state->m17_lsf[bit], 0U);
    }
    if (strcmp(state->m17_dst_str, M17_REF_LSF_DST_CSD) != 0 || strcmp(state->m17_src_str, M17_REF_LSF_SRC_CSD) != 0) {
        DSD_FPRINTF(stderr, "final LICH callsigns: dst='%s' src='%s'\n", state->m17_dst_str, state->m17_src_str);
        err |= 1;
    }
    return err;
}

static int
test_embedded_lich_rejects_invalid_counter_and_gates_bad_lsf_crc(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t lsf_packed[M17_LSF_BYTES];
    uint8_t encoded[M17_LICH_BITS];

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = dsd_neo_m17_test_compose_frame_info(1U, 1U, 0U, 0U, 7U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, NULL);
    (void)dsd_neo_m17_test_attach_lsf_crc(lsf_bits, lsf_packed);

    uint8_t invalid_content[M17_LICH_CONTENT_BITS];
    DSD_MEMSET(invalid_content, 0, sizeof(invalid_content));
    invalid_content[40] = 1U;
    invalid_content[41] = 1U;
    invalid_content[42] = 0U;
    m17_lich_encode_bits(invalid_content, encoded);
    DSD_MEMSET(state->m17_lsf, 0x5AU, sizeof(state->m17_lsf));

    int err = 0;
    err |= expect_int("invalid-counter LICH chunk rejected", dsd_neo_m17_test_process_lich(state, opts, encoded), -1);
    for (size_t bit = 0U; bit < TEST_M17_LSF_BITS; bit++) {
        err |= expect_u8("invalid-counter LICH preserves staged LSF", state->m17_lsf[bit], 0x5AU);
    }

    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 1;
    state->m17_dst = 0x111122223333ULL;
    state->m17_src = 0x444455556666ULL;
    lsf_bits[M17_LSF_LSD_BITS + 3U] ^= 1U;
    for (uint8_t chunk = 0U; chunk < M17_LICH_CHUNKS; chunk++) {
        build_encoded_lich_chunk(lsf_bits, chunk, encoded);
        err |= expect_int("bad CRC LICH chunk accepted", dsd_neo_m17_test_process_lich(state, opts, encoded), 0);
    }
    err |= expect_u64("bad LSF CRC preserves dst under aggressive sync", state->m17_dst, 0x111122223333ULL);
    err |= expect_u64("bad LSF CRC preserves src under aggressive sync", state->m17_src, 0x444455556666ULL);
    for (size_t bit = 0U; bit < TEST_M17_LSF_BITS; bit++) {
        err |= expect_u8("bad LSF CRC clears staged LSF", state->m17_lsf[bit], 0U);
    }
    return err;
}

static int
test_lsf_application_resets_and_stores_state(void) {
    dsd_state* state = &g_state;
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_dst = 0x1111ULL;
    state->m17_src = 0x2222ULL;
    state->m17_can = 3U;
    state->m17_enc = 1U;
    state->m17_payload_decrypted = 1U;
    state->m17_signature_received_mask = 0x0FU;
    state->m17_signature_complete = 1U;
    state->m17_signature_bad_sequence = 1U;
    state->m17_signature_verification_status = 99U;
    DSD_MEMSET(state->m17_signature_digest, 0xA5, sizeof(state->m17_signature_digest));
    DSD_MEMSET(state->m17_signature, 0x5A, sizeof(state->m17_signature));

    struct m17_lsf_result res = valid_lsf_result();
    int err = 0;
    err |= expect_int("valid LSF applied", dsd_neo_m17_test_apply_lsf_result(state, &res), 1);
    err |= expect_u64("LSF dst", state->m17_dst, res.dst);
    err |= expect_u64("LSF src", state->m17_src, res.src);
    err |= expect_u8("LSF CAN", state->m17_can, res.cn);
    err |= expect_u8("LSF dt", state->m17_str_dt, res.dt);
    err |= expect_u8("LSF enc", state->m17_enc, res.et);
    err |= expect_u8("LSF enc subtype", state->m17_enc_st, res.es);
    err |= expect_u8("LSF payload decrypted reset", state->m17_payload_decrypted, 0U);
    err |= expect_u8("LSF signature advertised", state->m17_signature_advertised, 1U);
    err |= expect_u8("LSF signature mask reset", state->m17_signature_received_mask, 0U);
    err |= expect_u8("LSF signature complete reset", state->m17_signature_complete, 0U);
    err |= expect_u8("LSF signature sequence reset", state->m17_signature_bad_sequence, 0U);
    err |= expect_u8("LSF signature status reset", state->m17_signature_verification_status, 0U);
    err |= expect_bytes("LSF meta nonce", state->m17_meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    for (size_t i = 0U; i < sizeof(state->m17_signature_digest); i++) {
        err |= expect_u8("LSF digest reset", state->m17_signature_digest[i], 0U);
    }
    for (size_t i = 0U; i < sizeof(state->m17_signature); i++) {
        err |= expect_u8("LSF signature buffer reset", state->m17_signature[i], 0U);
    }
    if (strcmp(state->m17_dst_str, "AB1CD") != 0 || strcmp(state->m17_src_str, "AB1CD") != 0) {
        DSD_FPRINTF(stderr, "LSF callsigns: dst='%s' src='%s'\n", state->m17_dst_str, state->m17_src_str);
        err |= 1;
    }
    return err;
}

static int
test_lsf_rejects_reserved_type_without_replacing_state(void) {
    dsd_state* state = &g_state;
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_dst = 0x1111ULL;
    state->m17_src = 0x2222ULL;
    state->m17_can = 4U;
    state->m17_enc = 1U;

    struct m17_lsf_result res = valid_lsf_result();
    res.rs = 1U;

    int err = 0;
    err |= expect_int("reserved LSF rejected", dsd_neo_m17_test_apply_lsf_result(state, &res), 0);
    err |= expect_u64("reserved LSF dst preserved", state->m17_dst, 0x1111ULL);
    err |= expect_u64("reserved LSF src preserved", state->m17_src, 0x2222ULL);
    err |= expect_u8("reserved LSF CAN preserved", state->m17_can, 4U);
    err |= expect_u8("reserved LSF enc preserved", state->m17_enc, 1U);
    return err;
}

static int
test_lsf_rejects_invalid_addresses_without_replacing_state(void) {
    dsd_state* state = &g_state;
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_dst = 0x1234ULL;
    state->m17_src = 0x5678ULL;
    state->m17_can = 6U;
    state->m17_str_dt = 3U;

    struct m17_lsf_result res = valid_lsf_result();
    res.dst_is_valid = 0U;

    int err = 0;
    err |= expect_int("invalid destination rejected", dsd_neo_m17_test_apply_lsf_result(state, &res), 0);
    err |= expect_u64("invalid destination preserves dst", state->m17_dst, 0x1234ULL);
    err |= expect_u64("invalid destination preserves src", state->m17_src, 0x5678ULL);
    err |= expect_u8("invalid destination preserves CAN", state->m17_can, 6U);
    err |= expect_u8("invalid destination preserves data type", state->m17_str_dt, 3U);

    res = valid_lsf_result();
    res.src_is_valid = 0U;
    err |= expect_int("invalid source rejected", dsd_neo_m17_test_apply_lsf_result(state, &res), 0);
    err |= expect_u64("invalid source preserves dst", state->m17_dst, 0x1234ULL);
    err |= expect_u64("invalid source preserves src", state->m17_src, 0x5678ULL);
    err |= expect_u8("invalid source preserves CAN", state->m17_can, 6U);
    err |= expect_u8("invalid source preserves data type", state->m17_str_dt, 3U);
    return err;
}

static int
test_lsf_null_meta_decodes_text_and_honors_can_filter(void) {
    dsd_state* state = &g_state;
    uint8_t expected_text[M17_TEXT_BLOCK_BYTES];
    DSD_MEMSET(expected_text, 0, sizeof(expected_text));
    DSD_MEMCPY(expected_text, "LSF-META-TEXT", M17_TEXT_BLOCK_BYTES);

    struct m17_lsf_result res = valid_lsf_result();
    res.et = 0U;
    res.es = 0U;
    res.has_meta = 1U;
    res.meta_is_iv = 0U;
    res.cn = 9U;
    DSD_MEMSET(res.meta, 0, sizeof(res.meta));
    res.meta[0] = 0x11U;
    DSD_MEMCPY(res.meta + 1, expected_text, M17_TEXT_BLOCK_BYTES);

    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;

    int err = 0;
    err |= expect_int("null META LSF accepted", dsd_neo_m17_test_apply_lsf_result(state, &res), 1);
    err |= expect_u8("null META text expected bitmap", state->m17_text_meta_expected_bitmap, 0x01U);
    err |= expect_u8("null META text received bitmap", state->m17_text_meta_received_bitmap, 0x01U);
    err |= expect_u8("null META text control", state->m17_text_meta_control_or, 0x11U);
    err |= expect_bytes("null META text bytes", state->m17_text_meta, expected_text, sizeof(expected_text));
    err |= expect_bytes("null META payload stored", state->m17_meta, res.meta, sizeof(res.meta));

    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = 4;
    state->m17_text_meta_expected_bitmap = 0x0FU;
    state->m17_text_meta_received_bitmap = 0x02U;
    state->m17_text_meta_control_or = 0xF2U;
    DSD_MEMSET(state->m17_text_meta, 0x5AU, sizeof(state->m17_text_meta));

    err |= expect_int("CAN-filtered null META LSF accepted", dsd_neo_m17_test_apply_lsf_result(state, &res), 1);
    err |= expect_u8("CAN-filtered null META preserves expected", state->m17_text_meta_expected_bitmap, 0x0FU);
    err |= expect_u8("CAN-filtered null META preserves received", state->m17_text_meta_received_bitmap, 0x02U);
    err |= expect_u8("CAN-filtered null META preserves control", state->m17_text_meta_control_or, 0xF2U);
    for (size_t i = 0U; i < sizeof(state->m17_text_meta); i++) {
        err |= expect_u8("CAN-filtered null META preserves text", state->m17_text_meta[i], 0x5AU);
    }

    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    state->m17_text_meta_expected_bitmap = 0x0FU;
    state->m17_text_meta_received_bitmap = 0x02U;
    state->m17_text_meta_control_or = 0xF2U;
    DSD_MEMSET(state->m17_text_meta, 0x5AU, sizeof(state->m17_text_meta));
    res.es = 3U;

    err |= expect_int("reserved null META LSF accepted", dsd_neo_m17_test_apply_lsf_result(state, &res), 1);
    err |= expect_u8("reserved null META preserves expected", state->m17_text_meta_expected_bitmap, 0x0FU);
    err |= expect_u8("reserved null META preserves received", state->m17_text_meta_received_bitmap, 0x02U);
    err |= expect_u8("reserved null META preserves control", state->m17_text_meta_control_or, 0xF2U);
    for (size_t i = 0U; i < sizeof(state->m17_text_meta); i++) {
        err |= expect_u8("reserved null META preserves text", state->m17_text_meta[i], 0x5AU);
    }
    return err;
}

static int
test_stream_dispatch_can_filter_and_aes_gates(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    uint8_t plaintext_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    bytes_to_bits(M17_REF_AES_CIPHERTEXT, payload_bits, sizeof(M17_REF_AES_CIPHERTEXT));
    bytes_to_bits(M17_REF_AES_PLAINTEXT, plaintext_bits, sizeof(M17_REF_AES_PLAINTEXT));
    state->m17_str_dt = 1U;
    state->m17_can = 9U;
    state->m17_can_en = 8;

    int err = 0;
    err |= expect_int(
        "CAN filter blocks stream",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_AES_TRANSMITTED_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_CAN_FILTERED);

    state->m17_can_en = -1;
    state->m17_enc = 2U;
    state->m17_enc_st = 0U;
    DSD_MEMCPY(state->m17_meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    err |= expect_int(
        "AES missing key locks stream",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_AES_TRANSMITTED_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_ENCRYPTED_LOCKED);

    DSD_MEMCPY(state->aes_key, M17_REF_AES128_KEY, sizeof(M17_REF_AES128_KEY));
    state->aes_key_loaded[0] = 1;
    state->aes_key_segments[0] = 2U;
    state->m17_payload_decrypted = 7U;
    err |= expect_int(
        "AES stream dispatches",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_AES_TRANSMITTED_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_ENCRYPTED_DISPATCHED);
    err |= expect_bytes("AES stream plaintext bits", processed_bits, plaintext_bits, sizeof(plaintext_bits));
    err |= expect_u8("AES transient decrypt flag restored", state->m17_payload_decrypted, 7U);
    return err;
}

static int
test_stream_dispatch_legacy_aes_key_segments(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    uint8_t plaintext_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    bytes_to_bits(M17_REF_AES_CIPHERTEXT, payload_bits, sizeof(M17_REF_AES_CIPHERTEXT));
    bytes_to_bits(M17_REF_AES_PLAINTEXT, plaintext_bits, sizeof(M17_REF_AES_PLAINTEXT));
    DSD_MEMCPY(state->m17_meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    state->m17_str_dt = 1U;
    state->m17_can_en = -1;
    state->m17_enc = 2U;
    state->m17_enc_st = 0U;
    state->K1 = 0x0001020304050607ULL;
    state->K2 = 0x08090A0B0C0D0E0FULL;

    int err = 0;
    err |= expect_int(
        "legacy AES key dispatches",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_AES_TRANSMITTED_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_ENCRYPTED_DISPATCHED);
    err |= expect_bytes("legacy AES key plaintext bits", processed_bits, plaintext_bits, sizeof(plaintext_bits));
    return err;
}

static int
test_stream_dispatch_rejects_invalid_arguments_and_unknown_encryption(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(payload_bits, 1, sizeof(payload_bits));
    DSD_MEMSET(processed_bits, 0xA5, sizeof(processed_bits));

    int err = 0;
    err |= expect_int(
        "stream null opts rejected",
        dsd_neo_m17_test_dispatch_stream_payload(NULL, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_INVALID);
    for (size_t i = 0U; i < sizeof(processed_bits); i++) {
        err |= expect_u8("stream null opts preserves output", processed_bits[i], 0xA5U);
    }

    state->m17_can_en = -1;
    state->m17_enc = 3U;
    state->m17_str_dt = 1U;
    err |= expect_int(
        "unknown encryption locks stream",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_ENCRYPTED_LOCKED);
    for (size_t i = 0U; i < sizeof(processed_bits); i++) {
        err |= expect_u8("unknown encryption clears output", processed_bits[i], 0U);
    }
    err |= expect_u8("unknown encryption leaves decrypt flag clear", state->m17_payload_decrypted, 0U);
    return err;
}

static int
test_stream_dispatch_scrambler_decrypts_with_seed_and_subtype(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    uint8_t expected_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));
    DSD_MEMSET(expected_bits, 0, sizeof(expected_bits));

    for (size_t i = 0U; i < sizeof(payload_bits); i++) {
        payload_bits[i] = (uint8_t)(((i * 3U) + 1U) & 1U);
    }
    const uint8_t subtype = 1U;
    const uint32_t seed = 0xACE1U;
    const uint16_t frame_number = 0x0123U;
    assert(m17_scrambler_apply_bits(subtype, seed, frame_number, payload_bits, expected_bits, M17_STREAM_PAYLOAD_BITS)
           == 0);

    state->m17_can_en = -1;
    state->m17_enc = 1U;
    state->m17_enc_st = subtype;
    state->m17_str_dt = 1U;
    state->R = seed;
    state->m17_payload_decrypted = 9U;

    int err = 0;
    err |= expect_int("scrambler stream dispatches",
                      dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, frame_number, processed_bits),
                      DSD_NEO_M17_TEST_STREAM_ENCRYPTED_DISPATCHED);
    err |= expect_bytes("scrambler stream output", processed_bits, expected_bits, sizeof(expected_bits));
    err |= expect_u8("scrambler transient decrypt flag restored", state->m17_payload_decrypted, 9U);
    return err;
}

static int
test_stream_dispatch_legacy_aes_array_key_segments(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    uint8_t plaintext_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    bytes_to_bits(M17_REF_AES_CIPHERTEXT, payload_bits, sizeof(M17_REF_AES_CIPHERTEXT));
    bytes_to_bits(M17_REF_AES_PLAINTEXT, plaintext_bits, sizeof(M17_REF_AES_PLAINTEXT));
    DSD_MEMCPY(state->m17_meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    state->m17_str_dt = 1U;
    state->m17_can_en = -1;
    state->m17_enc = 2U;
    state->m17_enc_st = 0U;
    state->A1[0] = 0x0001020304050607ULL;
    state->A2[0] = 0x08090A0B0C0D0E0FULL;

    int err = 0;
    err |= expect_int(
        "legacy AES array key dispatches",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_AES_TRANSMITTED_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_ENCRYPTED_DISPATCHED);
    err |= expect_bytes("legacy AES array key plaintext bits", processed_bits, plaintext_bits, sizeof(plaintext_bits));
    return err;
}

static int
test_stream_signature_frames_are_consumed_and_verify_without_key(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    static const uint16_t signature_fns[4] = {M17_STREAM_SIGNATURE_FN0, M17_STREAM_SIGNATURE_FN1,
                                              M17_STREAM_SIGNATURE_FN2, M17_STREAM_SIGNATURE_FN3};
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_signature_advertised = 1U;
    state->m17_str_dt = 2U;

    int err = 0;
    for (size_t i = 0U; i < 4U; i++) {
        expect_signature_slice(payload_bits, i * 16U);
        err |= expect_int(
            "signature payload consumed",
            dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, signature_fns[i], processed_bits),
            DSD_NEO_M17_TEST_STREAM_SIGNATURE_CONSUMED);
        err |=
            expect_u8("signature received mask", state->m17_signature_received_mask, (uint8_t)((1U << (i + 1U)) - 1U));
        err |= expect_bytes("signature bytes stored", state->m17_signature + (i * 16U),
                            M17_REF_SIGNATURE_BYTES + (i * 16U), 16U);
    }
    err |= expect_u8("signature complete", state->m17_signature_complete, 1U);
    err |= expect_u8("signature sequence ok", state->m17_signature_bad_sequence, 0U);
    err |= expect_u8("signature no public key status", state->m17_signature_verification_status, 4U);
    return err;
}

static int
test_stream_signature_out_of_order_marks_sequence_error(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_signature_advertised = 1U;
    state->m17_str_dt = 3U;

    expect_signature_slice(payload_bits, 16U);

    int err = 0;
    err |= expect_int(
        "out-of-order signature consumed",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_STREAM_SIGNATURE_FN1, processed_bits),
        DSD_NEO_M17_TEST_STREAM_SIGNATURE_CONSUMED);
    err |= expect_u8("out-of-order signature mask", state->m17_signature_received_mask, 0x02U);
    err |= expect_u8("out-of-order signature sequence", state->m17_signature_bad_sequence, 1U);
    err |= expect_u8("out-of-order signature incomplete", state->m17_signature_complete, 0U);
    return err;
}

static int
test_clear_signed_payload_updates_digest_and_dispatches(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    uint8_t expected_digest[16];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_signature_advertised = 1U;
    state->m17_str_dt = 1U;
    state->m17_can_en = -1;

    bytes_to_bits(M17_REF_STREAM_PAYLOAD_BYTES, payload_bits, sizeof(M17_REF_STREAM_PAYLOAD_BYTES));
    DSD_MEMCPY(expected_digest, M17_REF_STREAM_PAYLOAD_BYTES, sizeof(expected_digest));
    const uint8_t first = expected_digest[0];
    DSD_MEMMOVE(expected_digest, expected_digest + 1, sizeof(expected_digest) - 1U);
    expected_digest[sizeof(expected_digest) - 1U] = first;

    int err = 0;
    err |= expect_int(
        "clear signed payload dispatches",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_CLEAR_DISPATCHED);
    err |= expect_bytes("clear signed payload preserved", processed_bits, payload_bits, sizeof(payload_bits));
    err |= expect_bytes("clear signed payload digest", state->m17_signature_digest, expected_digest,
                        sizeof(expected_digest));
    err |= expect_u8("clear signed payload decrypt flag restored", state->m17_payload_decrypted, 0U);
    return err;
}

#ifdef USE_CODEC2
static int
test_stream_voice_3200_dispatch_routes_pair_audio_to_udp(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    static const uint8_t payload_bytes[16] = {0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
                                              0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_audio_fakes();
    install_fake_udp_audio();

    opts->slot1_on = 1;
    opts->audio_out_type = 8;
    state->m17_str_dt = 2U;
    state->m17_can_en = -1;
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    int err = 0;
    err |= expect_int(
        "3200 voice stream dispatches",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_CLEAR_DISPATCHED);
    err |= expect_bytes("3200 voice processed bits", processed_bits, payload_bits, sizeof(payload_bits));
    err |= expect_int("3200 voice Codec2 decodes", g_codec2_decode_calls, 2);
    err |= expect_bytes("3200 voice second codec frame", g_codec2_last_bits, payload_bytes + 8U, 8U);
    err |= expect_int("3200 voice UDP calls", g_udp_audio_calls, 2);
    err |= expect_int("3200 voice UDP bytes", (int)g_udp_audio_last_nsam, (int)(160U * sizeof(short)));
    err |= expect_int("3200 voice UDP last sample", (int)g_udp_audio_last_first_sample, 1002);
    err |= expect_int("3200 voice UDP opts", g_udp_audio_last_opts == opts, 1);
    err |= expect_int("3200 voice UDP state", g_udp_audio_last_state == state, 1);
    reset_audio_fakes();
    return err;
}

static int
test_stream_voice_1600_dispatch_routes_single_audio_to_udp(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    static const uint8_t payload_bytes[16] = {0x30U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U,
                                              0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_audio_fakes();
    install_fake_udp_audio();

    opts->slot1_on = 1;
    opts->audio_out_type = 8;
    state->m17_str_dt = 3U;
    state->m17_can_en = -1;
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    int err = 0;
    err |= expect_int(
        "1600 voice stream dispatches",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_CLEAR_DISPATCHED);
    err |= expect_bytes("1600 voice processed bits", processed_bits, payload_bits, sizeof(payload_bits));
    err |= expect_int("1600 voice Codec2 decodes", g_codec2_decode_calls, 1);
    err |= expect_bytes("1600 voice codec frame", g_codec2_last_bits, payload_bytes, 8U);
    err |= expect_int("1600 voice UDP calls", g_udp_audio_calls, 1);
    err |= expect_int("1600 voice UDP bytes", (int)g_udp_audio_last_nsam, (int)(320U * sizeof(short)));
    err |= expect_int("1600 voice UDP first sample", (int)g_udp_audio_last_first_sample, 1001);
    reset_audio_fakes();
    return err;
}

static int
test_stream_voice_audio_gate_suppresses_udp_when_slot_disabled(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    static const uint8_t payload_bytes[16] = {0x40U, 0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U,
                                              0x50U, 0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U};
    uint8_t payload_bits[128];
    uint8_t processed_bits[128];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_audio_fakes();
    install_fake_udp_audio();

    opts->slot1_on = 0;
    opts->audio_out_type = 8;
    state->m17_str_dt = 2U;
    state->m17_can_en = -1;
    bytes_to_bits(payload_bytes, payload_bits, sizeof(payload_bytes));

    int err = 0;
    err |= expect_int(
        "slot-disabled voice stream dispatches",
        dsd_neo_m17_test_dispatch_stream_payload(opts, state, payload_bits, M17_REF_STREAM_FN, processed_bits),
        DSD_NEO_M17_TEST_STREAM_CLEAR_DISPATCHED);
    err |= expect_int("slot-disabled voice still decodes", g_codec2_decode_calls, 2);
    err |= expect_int("slot-disabled voice suppresses UDP", g_udp_audio_calls, 0);
    err |= expect_int("slot-disabled voice leaves UDP byte count clear", (int)g_udp_audio_last_nsam, 0);
    reset_audio_fakes();
    return err;
}
#endif

static int
test_bert_payload_locks_from_default_state_and_continues(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    uint16_t tx_lfsr = 1U;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    m17_prbs9_fill_bits(&tx_lfsr, bert_bits, M17_BERT_PAYLOAD_BITS);
    dsd_neo_m17_test_process_bert_payload(opts, state, bert_bits);

    int err = 0;
    err |= expect_u8("BERT locks from default LFSR", state->m17_bert_locked, 1U);
    err |= expect_int("BERT first payload counted bits", (int)state->m17_bert_bits,
                      M17_BERT_PAYLOAD_BITS - M17_PRBS9_LOCK_BITS);
    err |= expect_int("BERT first payload errors", (int)state->m17_bert_errors, 0);
    err |= expect_int("BERT first payload resyncs", (int)state->m17_bert_resyncs, 0);

    m17_prbs9_fill_bits(&tx_lfsr, bert_bits, M17_BERT_PAYLOAD_BITS);
    dsd_neo_m17_test_process_bert_payload(opts, state, bert_bits);

    err |= expect_u8("BERT stays locked", state->m17_bert_locked, 1U);
    err |= expect_int("BERT second payload counted bits", (int)state->m17_bert_bits,
                      (M17_BERT_PAYLOAD_BITS - M17_PRBS9_LOCK_BITS) + M17_BERT_PAYLOAD_BITS);
    err |= expect_int("BERT second payload errors", (int)state->m17_bert_errors, 0);
    err |= expect_int("BERT second payload resyncs", (int)state->m17_bert_resyncs, 0);
    return err;
}

static int
test_bert_payload_resyncs_after_error_threshold(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    uint16_t tx_lfsr = 1U;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_bert_lfsr = 1U;
    state->m17_bert_locked = 1U;

    m17_prbs9_fill_bits(&tx_lfsr, bert_bits, M17_BERT_PAYLOAD_BITS);
    for (int i = 0; i < 19; i++) {
        bert_bits[i] ^= 1U;
    }

    dsd_neo_m17_test_process_bert_payload(opts, state, bert_bits);

    int err = 0;
    err |= expect_u8("BERT relocks after threshold", state->m17_bert_locked, 1U);
    err |= expect_int("BERT resync count", (int)state->m17_bert_resyncs, 1);
    err |= expect_int("BERT threshold counted bits", (int)state->m17_bert_bits,
                      M17_BERT_PAYLOAD_BITS - M17_PRBS9_LOCK_BITS);
    err |= expect_int("BERT threshold errors", (int)state->m17_bert_errors, 19);
    err |= expect_int("BERT window after relock", (int)state->m17_bert_window_bits,
                      M17_BERT_PAYLOAD_BITS - M17_PRBS9_RESYNC_WINDOW_BITS - M17_PRBS9_LOCK_BITS);
    err |= expect_int("BERT window errors reset after resync", (int)state->m17_bert_window_errors, 0);
    return err;
}

static int
test_bert_hard_payload_decode_primitives(void) {
    uint8_t input_bits[M17_PAYLOAD_BITS];
    uint8_t depunc[M17_BERT_TYPE2_BITS];
    uint8_t expected_depunc[M17_BERT_TYPE2_BITS];
    uint8_t unpacked[24];
    uint8_t expected_unpacked[16];
    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    uint8_t chainback_bytes[25];
    uint8_t expected_bert_bits[200];
    int bit_in = 0;

    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        input_bits[i] = (uint8_t)(((i * 5) + 1) & 1);
    }
    DSD_MEMSET(depunc, 0xA5, sizeof(depunc));
    DSD_MEMSET(expected_depunc, 0, sizeof(expected_depunc));

    dsd_neo_m17_test_depuncture_p2_hard(input_bits, depunc, M17_BERT_TYPE2_BITS);
    for (int i = 0; i < M17_BERT_TYPE2_BITS; i++) {
        if (m17_puncture_pattern_2[i % M17_PUNCTURE_P2_LEN] == 1U && bit_in < M17_PAYLOAD_BITS) {
            expected_depunc[i] = input_bits[bit_in++];
        }
    }

    int err = 0;
    err |= expect_int("BERT depuncture consumes payload", bit_in, M17_PAYLOAD_BITS);
    err |= expect_bytes("BERT depunctured P2 bits", depunc, expected_depunc, sizeof(depunc));

    DSD_MEMSET(depunc, 0x5A, sizeof(depunc));
    dsd_neo_m17_test_depuncture_p2_hard(input_bits, depunc, -1);
    err |= expect_u8("BERT depuncture negative guard preserves output", depunc[0], 0x5AU);

    static const uint8_t packed[2] = {0xA5U, 0x3CU};
    static const uint8_t unpacked_expected[16] = {1U, 0U, 1U, 0U, 0U, 1U, 0U, 1U, 0U, 0U, 1U, 1U, 1U, 1U, 0U, 0U};
    DSD_MEMSET(unpacked, 0xCC, sizeof(unpacked));
    DSD_MEMCPY(expected_unpacked, unpacked_expected, sizeof(expected_unpacked));
    dsd_neo_m17_test_unpack_bytes_to_bits(packed, 2, unpacked);
    err |= expect_bytes("BERT byte unpack MSB first", unpacked, expected_unpacked, sizeof(expected_unpacked));
    err |= expect_u8("BERT byte unpack does not overrun", unpacked[16], 0xCCU);
    dsd_neo_m17_test_unpack_bytes_to_bits(NULL, 2, unpacked);
    err |= expect_u8("BERT byte unpack null guard preserves output", unpacked[0], 1U);

    reset_convolution_fake();
    DSD_MEMSET(bert_bits, 0x5A, sizeof(bert_bits));
    dsd_neo_m17_test_decode_bert_payload_bits(input_bits, bert_bits);
    err |= expect_int("BERT hard decode starts convolution", g_conv_start_calls, 1);
    err |= expect_int("BERT hard decode symbol pairs", g_conv_decode_calls, M17_BERT_TYPE1_FLUSH_BITS);
    err |= expect_int("BERT hard decode chainback calls", g_conv_chainback_calls, 1);
    err |= expect_int("BERT hard decode chainback bits", (int)g_conv_chainback_bits, M17_BERT_PAYLOAD_BITS);
    for (int i = 0; i < (int)sizeof(g_conv_first_symbols); i++) {
        err |= expect_u8("BERT hard decode feeds depunctured soft symbols", g_conv_first_symbols[i],
                         (uint8_t)(expected_depunc[i] << 1U));
    }

    for (size_t i = 0U; i < sizeof(chainback_bytes); i++) {
        chainback_bytes[i] = (uint8_t)(0xA0U + i);
    }
    bytes_to_bits(chainback_bytes, expected_bert_bits, sizeof(chainback_bytes));
    err |=
        expect_bytes("BERT hard decode unpacks chainback bytes", bert_bits, expected_bert_bits, M17_BERT_PAYLOAD_BITS);
    return err;
}

static int
test_frame_info_packet_and_ip_helpers(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    int err = 0;
    err |= expect_int("frame info masks fields", dsd_neo_m17_test_compose_frame_info(3U, 7U, 6U, 5U, 0x1FU, 3U, 0x1FU),
                      0xFFB7);
    err |= expect_int("frame info selected fields", dsd_neo_m17_test_compose_frame_info(1U, 2U, 1U, 3U, 9U, 1U, 0xAU),
                      0xACED);

    uint8_t ip_frame[12];
    DSD_MEMSET(ip_frame, 0, sizeof(ip_frame));
    ip_frame[4] = 0x01U;
    ip_frame[5] = 0x23U;
    ip_frame[6] = 0x45U;
    ip_frame[7] = 0x67U;
    ip_frame[8] = 0x89U;
    ip_frame[9] = 0xABU;
    err |= expect_u64("IP source", dsd_neo_m17_test_read_ip_source(ip_frame), 0x0123456789ABULL);
    err |= expect_u64("IP null source", dsd_neo_m17_test_read_ip_source(NULL), 0ULL);

    err |= expect_int("pkt negative clamp", dsd_neo_m17_test_pkt_ptr_clamped(-4), 0);
    err |= expect_int("pkt second frame ptr", dsd_neo_m17_test_pkt_ptr_clamped(2), 50);
    err |= expect_int("pkt high clamp", dsd_neo_m17_test_pkt_ptr_clamped(99), 825);

    err |=
        expect_int("clear packet not encrypted", dsd_neo_m17_test_decode_pkt_should_report_encrypted(state, 0x05U), 0);
    state->m17_enc = 2U;
    err |= expect_int("GNSS meta allowed while encrypted",
                      dsd_neo_m17_test_decode_pkt_should_report_encrypted(state, 0x81U), 0);
    err |=
        expect_int("SMS blocked while encrypted", dsd_neo_m17_test_decode_pkt_should_report_encrypted(state, 0x05U), 1);
    state->m17_payload_decrypted = 1U;
    err |= expect_int("decrypted packet allowed", dsd_neo_m17_test_decode_pkt_should_report_encrypted(state, 0x05U), 0);

    DSD_MEMSET(state, 0, sizeof(*state));
    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    uint8_t disc[10] = {'D', 'I', 'S', 'C', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    dsd_neo_m17_test_dispatch_ip_frame(opts, state, disc, sizeof(disc));
    err |= expect_int("DISC drops carrier", state->carrier, 0);
    err |= expect_int("DISC clears synctype", state->synctype, DSD_SYNC_NONE);

    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    uint8_t eotx[10] = {'E', 'O', 'T', 'X', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    dsd_neo_m17_test_dispatch_ip_frame(opts, state, eotx, sizeof(eotx));
    err |= expect_int("EOTX drops carrier", state->carrier, 0);
    err |= expect_int("EOTX clears synctype", state->synctype, DSD_SYNC_NONE);

    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    uint8_t conn[11] = {'C', 'O', 'N', 'N', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 'A'};
    dsd_neo_m17_test_dispatch_ip_frame(opts, state, conn, sizeof(conn));
    err |= expect_int("CONN keeps carrier", state->carrier, 1);
    err |= expect_int("CONN keeps synctype", state->synctype, DSD_SYNC_M17_STR_POS);

    uint8_t ping[10] = {'P', 'I', 'N', 'G', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    dsd_neo_m17_test_dispatch_ip_frame(opts, state, ping, sizeof(ping));
    err |= expect_int("PING keeps carrier", state->carrier, 1);
    err |= expect_int("PING keeps synctype", state->synctype, DSD_SYNC_M17_STR_POS);

    uint8_t pong[10] = {'P', 'O', 'N', 'G', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    dsd_neo_m17_test_dispatch_ip_frame(opts, state, pong, sizeof(pong));
    err |= expect_int("PONG keeps carrier", state->carrier, 1);
    err |= expect_int("PONG keeps synctype", state->synctype, DSD_SYNC_M17_STR_POS);

    uint8_t short_mpkt[12] = {'M', 'P', 'K', 'T', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0x55, 0xAA};
    state->m17_dst = 0x111122223333ULL;
    state->m17_src = 0x444455556666ULL;
    dsd_neo_m17_test_dispatch_ip_frame(opts, state, short_mpkt, sizeof(short_mpkt));
    err |= expect_u64("short MPKT preserves dst", state->m17_dst, 0x111122223333ULL);
    err |= expect_u64("short MPKT preserves src", state->m17_src, 0x444455556666ULL);

    uint8_t unknown[10] = {'N', 'O', 'P', 'E', 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
    state->carrier = 1;
    state->synctype = DSD_SYNC_M17_STR_POS;
    dsd_neo_m17_test_dispatch_ip_frame(opts, state, unknown, sizeof(unknown));
    err |= expect_int("unknown IP magic preserves carrier", state->carrier, 1);
    err |= expect_int("unknown IP magic preserves synctype", state->synctype, DSD_SYNC_M17_STR_POS);
    return err;
}

static int
test_ip_stream_frames_apply_crc_gated_lsf_state(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t ip_frame[54];
    uint8_t counter[M17_AES_COUNTER_BYTES];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));
    DSD_MEMSET(counter, 0, sizeof(counter));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t type_word = dsd_neo_m17_test_compose_frame_info(1U, 1U, 2U, 0U, 5U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, type_word, M17_REF_AES_NONCE);
    bytes_to_bits(M17_REF_STREAM_PAYLOAD_BYTES, payload_bits, sizeof(M17_REF_STREAM_PAYLOAD_BYTES));
    build_ip_stream_frame(ip_frame, lsf_bits, 0xBEEF, M17_REF_STREAM_FN, 0U, payload_bits);

    state->m17_can_en = -1;
    dsd_neo_m17_test_dispatch_ip_frame(opts, state, ip_frame, sizeof(ip_frame));

    int err = 0;
    err |= expect_int("valid IP stream sets carrier", state->carrier, 1);
    err |= expect_int("valid IP stream sets synctype", state->synctype, DSD_SYNC_M17_STR_POS);
    err |= expect_u64("valid IP stream dst", state->m17_dst, dst);
    err |= expect_u64("valid IP stream src", state->m17_src, src);
    err |= expect_u8("valid IP stream data type", state->m17_str_dt, 1U);
    err |= expect_u8("valid IP stream CAN", state->m17_can, 5U);
    err |= expect_u8("valid IP stream AES mode", state->m17_enc, 2U);
    err |= expect_u8("valid IP stream AES subtype", state->m17_enc_st, 0U);
    err |= expect_u8("valid IP stream remains locked without key", state->m17_payload_decrypted, 0U);
    err |= expect_bytes("valid IP stream nonce", state->m17_meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    for (size_t i = 0U; i < M17_LSF_LSD_BITS; i++) {
        err |= expect_u8("valid IP stream copied LSF bits", state->m17_lsf[i], lsf_bits[i]);
    }

    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    state->m17_str_dt = 1U;
    state->m17_dst = 0x111122223333ULL;
    state->m17_src = 0x444455556666ULL;
    state->m17_enc = 0U;
    DSD_MEMSET(state->m17_meta, 0x5A, sizeof(state->m17_meta));
    m17_aes_build_counter(state->m17_meta, (uint16_t)(M17_STREAM_FRAME_END_MASK | M17_REF_STREAM_FN), counter);
    build_ip_stream_frame(ip_frame, lsf_bits, 0xBEEF, M17_REF_STREAM_FN, 1U, payload_bits);
    ip_frame[52] ^= 0x01U;

    dsd_neo_m17_test_dispatch_ip_frame(opts, state, ip_frame, sizeof(ip_frame));
    err |= expect_int("bad IP stream CRC sets carrier", state->carrier, 1);
    err |= expect_int("bad IP stream CRC sets synctype", state->synctype, DSD_SYNC_M17_STR_POS);
    err |= expect_u64("bad IP stream CRC preserves dst", state->m17_dst, 0x111122223333ULL);
    err |= expect_u64("bad IP stream CRC preserves src", state->m17_src, 0x444455556666ULL);
    err |= expect_u8("bad IP stream CRC preserves enc", state->m17_enc, 0U);
    err |= expect_u8("bad IP stream CRC stores counter high", state->m17_meta[14], counter[14]);
    err |= expect_u8("bad IP stream CRC stores counter low", state->m17_meta[15], counter[15]);
    return err;
}

static int
test_ip_mpkt_frames_apply_crc_gated_packet_state(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    uint8_t mpkt_frame[80];
    uint8_t app[2U + M17_META_BYTES];
    uint8_t expected_text[M17_TEXT_BLOCK_BYTES];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(app, 0, sizeof(app));
    DSD_MEMSET(expected_text, 0, sizeof(expected_text));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    const uint16_t packet_type_word = dsd_neo_m17_test_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U);
    build_lsf_bits(lsf_bits, dst, src, packet_type_word, NULL);

    app[0] = 0xC2U;
    app[1] = 0x80U;
    app[2] = 0x11U;
    DSD_MEMCPY(app + 3, "IP-MPKT-OK", 10U);
    DSD_MEMCPY(expected_text, "IP-MPKT-OK", 10U);
    const size_t mpkt_len = build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), lsf_bits, 0xCAFEU, app, sizeof(app));

    state->m17_can_en = -1;
    dsd_neo_m17_test_dispatch_ip_frame(opts, state, mpkt_frame, (int)mpkt_len);

    int err = 0;
    err |= expect_u64("valid MPKT dst", state->m17_dst, dst);
    err |= expect_u64("valid MPKT src", state->m17_src, src);
    err |= expect_u8("valid MPKT packet mode data type", state->m17_str_dt, 20U);
    err |= expect_u8("valid MPKT CAN", state->m17_can, 5U);
    err |= expect_u8("valid MPKT clear encryption", state->m17_enc, 0U);
    err |= expect_u8("valid MPKT text expected bitmap", state->m17_text_meta_expected_bitmap, 0x01U);
    err |= expect_u8("valid MPKT text received bitmap", state->m17_text_meta_received_bitmap, 0x01U);
    err |= expect_u8("valid MPKT text control", state->m17_text_meta_control_or, 0x11U);
    err |= expect_bytes("valid MPKT text bytes", state->m17_text_meta, expected_text, sizeof(expected_text));

    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    state->m17_dst = 0xAAAABBBBCCCCULL;
    state->m17_src = 0x111122223333ULL;
    state->m17_text_meta_expected_bitmap = 0x0FU;
    state->m17_text_meta_received_bitmap = 0x02U;
    state->m17_text_meta_control_or = 0xF2U;
    DSD_MEMSET(state->m17_text_meta, 0x5AU, sizeof(state->m17_text_meta));
    build_ip_mpkt_frame(mpkt_frame, sizeof(mpkt_frame), lsf_bits, 0xCAFEU, app, sizeof(app));
    mpkt_frame[mpkt_len - 1U] ^= 0x01U;

    dsd_neo_m17_test_dispatch_ip_frame(opts, state, mpkt_frame, (int)mpkt_len);
    err |= expect_u64("bad MPKT CRC preserves dst", state->m17_dst, 0xAAAABBBBCCCCULL);
    err |= expect_u64("bad MPKT CRC preserves src", state->m17_src, 0x111122223333ULL);
    err |= expect_u8("bad MPKT CRC preserves text expected", state->m17_text_meta_expected_bitmap, 0x0FU);
    err |= expect_u8("bad MPKT CRC preserves text received", state->m17_text_meta_received_bitmap, 0x02U);
    err |= expect_u8("bad MPKT CRC preserves text control", state->m17_text_meta_control_or, 0xF2U);
    for (size_t i = 0U; i < sizeof(state->m17_text_meta); i++) {
        err |= expect_u8("bad MPKT CRC preserves text bytes", state->m17_text_meta[i], 0x5AU);
    }
    return err;
}

static void
build_packet_text_meta_app(uint8_t app[2U + M17_META_BYTES], const char text[M17_TEXT_BLOCK_BYTES]) {
    DSD_MEMSET(app, 0, 2U + M17_META_BYTES);
    app[0] = 0xC2U;
    app[1] = 0x80U;
    app[2] = 0x11U;
    DSD_MEMCPY(app + 3, text, M17_TEXT_BLOCK_BYTES);
}

static void
stage_packet_with_crc(dsd_state* state, const uint8_t* app, uint16_t app_len, uint16_t crc) {
    DSD_MEMSET(state->m17_pkt, 0, sizeof(state->m17_pkt));
    DSD_MEMCPY(state->m17_pkt, app, app_len);
    state->m17_pkt[app_len] = (uint8_t)(crc >> 8U);
    state->m17_pkt[app_len + 1U] = (uint8_t)(crc & 0xFFU);
}

static int
test_packet_eot_finalization_crc_gates_decode_and_clears_state(void) {
    dsd_opts* opts = &g_opts;
    dsd_state* state = &g_state;
    uint8_t app[2U + M17_META_BYTES];
    uint8_t expected_text[M17_TEXT_BLOCK_BYTES];
    static const char text[M17_TEXT_BLOCK_BYTES] = {'R', 'F', '-', 'P', 'K', 'T', '-', 'O', 'K', 0, 0, 0, 0};
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(expected_text, 0, sizeof(expected_text));
    build_packet_text_meta_app(app, text);
    DSD_MEMCPY(expected_text, text, sizeof(expected_text));

    const uint16_t app_len = (uint16_t)sizeof(app);
    const size_t total_len = (size_t)app_len + (size_t)M17_PACKET_CRC_BYTES;
    const uint16_t crc = m17_crc16(app, app_len);
    state->m17_can_en = -1;
    state->m17_pbc_ct = 4;
    stage_packet_with_crc(state, app, app_len, crc);

    dsd_neo_m17_test_finalize_packet_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));

    int err = 0;
    err |= expect_u8("valid packet EOT text expected bitmap", state->m17_text_meta_expected_bitmap, 0x01U);
    err |= expect_u8("valid packet EOT text received bitmap", state->m17_text_meta_received_bitmap, 0x01U);
    err |= expect_u8("valid packet EOT text control", state->m17_text_meta_control_or, 0x11U);
    err |= expect_bytes("valid packet EOT text bytes", state->m17_text_meta, expected_text, sizeof(expected_text));
    err |= expect_int("valid packet EOT clears PBC", state->m17_pbc_ct, 0);
    for (size_t i = 0U; i < total_len; i++) {
        err |= expect_u8("valid packet EOT clears packet buffer", state->m17_pkt[i], 0U);
    }

    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    state->m17_pbc_ct = 2;
    state->m17_text_meta_expected_bitmap = 0x0FU;
    state->m17_text_meta_received_bitmap = 0x02U;
    state->m17_text_meta_control_or = 0xF2U;
    DSD_MEMSET(state->m17_text_meta, 0x5AU, sizeof(state->m17_text_meta));
    opts->aggressive_framesync = 1;
    stage_packet_with_crc(state, app, app_len, (uint16_t)(crc ^ 0x0001U));

    dsd_neo_m17_test_finalize_packet_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));

    err |= expect_u8("bad packet CRC preserves text expected", state->m17_text_meta_expected_bitmap, 0x0FU);
    err |= expect_u8("bad packet CRC preserves text received", state->m17_text_meta_received_bitmap, 0x02U);
    err |= expect_u8("bad packet CRC preserves text control", state->m17_text_meta_control_or, 0xF2U);
    for (size_t i = 0U; i < sizeof(state->m17_text_meta); i++) {
        err |= expect_u8("bad packet CRC preserves text bytes", state->m17_text_meta[i], 0x5AU);
    }
    err |= expect_int("bad packet CRC clears PBC", state->m17_pbc_ct, 0);
    for (size_t i = 0U; i < total_len; i++) {
        err |= expect_u8("bad packet CRC clears packet buffer", state->m17_pkt[i], 0U);
    }

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->m17_can_en = -1;
    stage_packet_with_crc(state, app, app_len, (uint16_t)(crc ^ 0x0001U));

    dsd_neo_m17_test_finalize_packet_eot(opts, state, app_len, (int)(app_len + M17_PACKET_CRC_BYTES));

    err |=
        expect_u8("non-aggressive bad CRC still decodes expected bitmap", state->m17_text_meta_expected_bitmap, 0x01U);
    err |=
        expect_u8("non-aggressive bad CRC still decodes received bitmap", state->m17_text_meta_received_bitmap, 0x01U);
    err |= expect_u8("non-aggressive bad CRC still decodes control", state->m17_text_meta_control_or, 0x11U);
    err |=
        expect_bytes("non-aggressive bad CRC text bytes", state->m17_text_meta, expected_text, sizeof(expected_text));
    return err;
}

static int
test_encoder_control_lsf_and_dibit_helpers(void) {
    uint8_t conn[11];
    uint8_t disc[10];
    uint8_t eotx[10];
    uint8_t lsf_bits[240];
    uint8_t lsf_packed[30];
    uint8_t dibits[M17_FRAME_SYMBOLS];
    uint8_t payload_bits[M17_PAYLOAD_BITS];
    DSD_MEMSET(conn, 0xA5, sizeof(conn));
    DSD_MEMSET(disc, 0xA5, sizeof(disc));
    DSD_MEMSET(eotx, 0xA5, sizeof(eotx));
    DSD_MEMSET(lsf_bits, 0, sizeof(lsf_bits));
    DSD_MEMSET(lsf_packed, 0, sizeof(lsf_packed));
    DSD_MEMSET(dibits, 0, sizeof(dibits));
    DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));

    dsd_neo_m17_test_setup_conn_disc_eotx(0x0123456789ABULL, 'Z', conn, disc, eotx);

    int err = 0;
    static const uint8_t want_conn[11] = {'C', 'O', 'N', 'N', 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xABU, 'Z'};
    static const uint8_t want_disc[10] = {'D', 'I', 'S', 'C', 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xABU};
    static const uint8_t want_eotx[10] = {'E', 'O', 'T', 'X', 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xABU};
    err |= expect_bytes("CONN frame", conn, want_conn, sizeof(want_conn));
    err |= expect_bytes("DISC frame", disc, want_disc, sizeof(want_disc));
    err |= expect_bytes("EOTX frame", eotx, want_eotx, sizeof(want_eotx));

    const unsigned long long dst = m17_encode_b40_callsign(0ULL, M17_REF_LSF_DST_CSD);
    const unsigned long long src = m17_encode_b40_callsign(0ULL, M17_REF_LSF_SRC_CSD);
    dsd_neo_m17_test_load_lsf_callsigns(lsf_bits, dst, src);
    const uint16_t type_word = dsd_neo_m17_test_compose_frame_info(1U, 2U, 0U, 0U, 9U, 1U, 0U);
    for (int i = 0; i < 16; i++) {
        lsf_bits[96 + i] = (uint8_t)((type_word >> (15 - i)) & 1U);
    }
    const uint16_t crc = dsd_neo_m17_test_attach_lsf_crc(lsf_bits, lsf_packed);
    err |= expect_u64("LSF dst bits", bits_to_u64(lsf_bits, 48U), dst);
    err |= expect_u64("LSF src bits", bits_to_u64(lsf_bits + 48, 48U), src);
    err |= expect_int("LSF type word", (int)bits_to_u64(lsf_bits + 96, 16U), type_word);
    err |= expect_int("LSF packed CRC", crc, m17_crc16(lsf_packed, M17_LSF_LSD_BYTES));
    err |= expect_int("LSF CRC bits", (int)bits_to_u64(lsf_bits + M17_LSF_LSD_BITS, M17_LSF_CRC_BITS), crc);

    dsd_neo_m17_test_apply_frame_prefix_dibits(2, dibits);
    uint8_t want_sync[M17_SYNC_SYMBOLS];
    DSD_MEMSET(want_sync, 0, sizeof(want_sync));
    m17_fill_sync_dibits_from_word(M17_SYNC_STREAM_WORD, want_sync);
    err |= expect_bytes("stream sync prefix", dibits, want_sync, sizeof(want_sync));
    for (int i = M17_SYNC_SYMBOLS; i < M17_FRAME_SYMBOLS; i++) {
        err |= expect_u8("stream prefix leaves payload clear", dibits[i], 0U);
    }

    DSD_MEMSET(dibits, 0, sizeof(dibits));
    dsd_neo_m17_test_apply_frame_prefix_dibits(55, dibits);
    uint8_t want_eot_dibits[M17_FRAME_SYMBOLS];
    DSD_MEMSET(want_eot_dibits, 0, sizeof(want_eot_dibits));
    m17_fill_repeating_16bit_dibits(M17_EOT_MARKER_WORD, want_eot_dibits);
    err |= expect_bytes("EOT repeating prefix", dibits, want_eot_dibits, sizeof(want_eot_dibits));

    for (int i = 0; i < M17_PAYLOAD_BITS; i++) {
        payload_bits[i] = (uint8_t)(i & 1);
    }
    DSD_MEMSET(dibits, 0xEE, sizeof(dibits));
    dsd_neo_m17_test_load_payload_dibits(payload_bits, dibits);
    err |= expect_u8("payload helper preserves prefix byte", dibits[0], 0xEEU);
    for (int i = 0; i < M17_PAYLOAD_SYMBOLS; i++) {
        err |= expect_u8("payload dibit mapping", dibits[i + M17_SYNC_SYMBOLS], 1U);
    }

    err |= expect_int("clip high", dsd_neo_m17_test_clip_float_to_short(40000.0f), 32767);
    err |= expect_int("clip low", dsd_neo_m17_test_clip_float_to_short(-40000.0f), -32768);
    err |= expect_int("clip rounded positive", dsd_neo_m17_test_clip_float_to_short(123.6f), 124);
    err |= expect_int("clip rounded negative", dsd_neo_m17_test_clip_float_to_short(-123.4f), -123);

    uint8_t symbol_dibits[M17_FRAME_SYMBOLS];
    int symbols[M17_FRAME_SYMBOLS];
    int upsampled[M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR];
    short baseband[M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR];
    DSD_MEMSET(symbol_dibits, 0, sizeof(symbol_dibits));
    DSD_MEMSET(symbols, 0, sizeof(symbols));
    DSD_MEMSET(upsampled, 0, sizeof(upsampled));
    DSD_MEMSET(baseband, 0, sizeof(baseband));
    symbol_dibits[0] = 0U;
    symbol_dibits[1] = 1U;
    symbol_dibits[2] = 2U;
    symbol_dibits[3] = 3U;
    symbol_dibits[4] = 7U;
    dsd_neo_m17_test_dibits_to_symbols(symbol_dibits, symbols);
    err |= expect_int("dibit 0 symbol", symbols[0], 1);
    err |= expect_int("dibit 1 symbol", symbols[1], 3);
    err |= expect_int("dibit 2 symbol", symbols[2], -1);
    err |= expect_int("dibit 3 symbol", symbols[3], -3);
    err |= expect_int("dibit masked symbol", symbols[4], -3);

    dsd_neo_m17_test_upsample_symbols_10x(symbols, upsampled);
    for (int i = 0; i < M17_RECOMMENDED_UPSAMPLE_FACTOR; i++) {
        err |= expect_int("first symbol upsampled", upsampled[i], 1);
        err |= expect_int("second symbol upsampled", upsampled[M17_RECOMMENDED_UPSAMPLE_FACTOR + i], 3);
    }

    dsd_neo_m17_test_baseband_no_filter(upsampled, baseband);
    err |= expect_int("baseband symbol +1", baseband[0], 7168);
    err |= expect_int("baseband symbol +3", baseband[M17_RECOMMENDED_UPSAMPLE_FACTOR], 21504);
    symbols[0] = -1;
    symbols[1] = -3;
    DSD_MEMSET(upsampled, 0, sizeof(upsampled));
    DSD_MEMSET(baseband, 0, sizeof(baseband));
    dsd_neo_m17_test_upsample_symbols_10x(symbols, upsampled);
    dsd_neo_m17_test_baseband_no_filter(upsampled, baseband);
    err |= expect_int("baseband symbol -1", baseband[0], -7168);
    err |= expect_int("baseband symbol -3", baseband[M17_RECOMMENDED_UPSAMPLE_FACTOR], -21504);

    DSD_MEMSET(symbol_dibits, 0x22, sizeof(symbol_dibits));
    for (size_t i = 0U; i < (M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR); i++) {
        baseband[i] = 55;
    }
    dsd_neo_m17_test_maybe_apply_dead_air(1, symbol_dibits, baseband);
    err |= expect_u8("non-dead-air preserves dibit", symbol_dibits[0], 0x22U);
    err |= expect_int("non-dead-air preserves baseband", baseband[0], 55);
    dsd_neo_m17_test_maybe_apply_dead_air(99, symbol_dibits, baseband);
    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        err |= expect_u8("dead-air dibit marker", symbol_dibits[i], 0xFFU);
    }
    for (size_t i = 0U; i < (M17_FRAME_SYMBOLS * M17_RECOMMENDED_UPSAMPLE_FACTOR); i++) {
        err |= expect_int("dead-air baseband silence", baseband[i], 0);
    }

    uint8_t bert_bits[M17_BERT_PAYLOAD_BITS];
    uint8_t reversed[208];
    for (int i = 0; i < M17_BERT_PAYLOAD_BITS; i++) {
        bert_bits[i] = (uint8_t)((i % 3) == 0);
    }
    DSD_MEMSET(reversed, 0xA5, sizeof(reversed));
    dsd_neo_m17_test_reverse_brt_bits(bert_bits, reversed);
    err |= expect_u8("BERT reverse leading zero 0", reversed[0], 0U);
    err |= expect_u8("BERT reverse leading zero 1", reversed[1], 0U);
    err |= expect_u8("BERT reverse leading zero 2", reversed[2], 0U);
    for (int i = 0; i < M17_BERT_PAYLOAD_BITS; i++) {
        err |= expect_u8("BERT reverse payload", reversed[i + 3], bert_bits[(M17_BERT_PAYLOAD_BITS - 1) - i]);
    }
    err |= expect_u8("BERT reverse trailing zero", reversed[200], 0U);

    err |= expect_int("strlen null", (int)dsd_neo_m17_test_strlen_limit(NULL, 9U), 0);
    err |= expect_int("strlen full", (int)dsd_neo_m17_test_strlen_limit("abc", 9U), 3);
    err |= expect_int("strlen capped", (int)dsd_neo_m17_test_strlen_limit("abcdef", 3U), 3);
    err |= expect_int("strlen zero cap", (int)dsd_neo_m17_test_strlen_limit("abcdef", 0U), 0);

    return err;
}

static int
test_encoder_packet_payload_layout(void) {
    uint8_t packed[M17_PACKET_MAX_TOTAL_BYTES];
    uint8_t full_bits[M17_PACKET_MAX_FRAMES * M17_PACKET_CHUNK_BITS];
    uint16_t app_len = 0;
    int block = 0;
    uint8_t lst = 0;
    uint16_t crc = 0;
    int err = 0;

    DSD_MEMSET(packed, 0xA5, sizeof(packed));
    DSD_MEMSET(full_bits, 0xA5, sizeof(full_bits));
    err |= expect_int("packet layout null guard",
                      dsd_neo_m17_test_prepare_pkt_payload("x", NULL, full_bits, &app_len, &block, &lst, &crc), -1);

    err |= expect_int("packet layout short text",
                      dsd_neo_m17_test_prepare_pkt_payload("hi", packed, full_bits, &app_len, &block, &lst, &crc), 0);
    err |= expect_int("packet short app len", app_len, 4);
    err |= expect_int("packet short block count", block, 1);
    err |= expect_u8("packet short last frame bytes", lst, 6U);
    err |= expect_u8("packet protocol SMS", packed[0], 0x05U);
    err |= expect_u8("packet text h", packed[1], 'h');
    err |= expect_u8("packet text i", packed[2], 'i');
    err |= expect_u8("packet text terminator", packed[3], 0U);
    err |= expect_int("packet short crc", crc, m17_crc16(packed, app_len));
    err |= expect_u8("packet crc high", packed[app_len], (uint8_t)(crc >> 8U));
    err |= expect_u8("packet crc low", packed[app_len + 1U], (uint8_t)(crc & 0xFFU));
    err |= expect_u8("packet first bit", full_bits[0], 0U);
    err |= expect_u8("packet protocol low bit", full_bits[7], 1U);
    err |= expect_u8("packet h high bit", full_bits[8], 0U);
    err |= expect_u8("packet h bit 1", full_bits[9], 1U);

    char long_text[M17_PACKET_MAX_APPLICATION_BYTES + 64U];
    for (size_t i = 0U; i < sizeof(long_text) - 1U; i++) {
        long_text[i] = (char)('A' + (i % 26U));
    }
    long_text[sizeof(long_text) - 1U] = '\0';
    DSD_MEMSET(packed, 0, sizeof(packed));
    DSD_MEMSET(full_bits, 0, sizeof(full_bits));
    err |=
        expect_int("packet layout max text",
                   dsd_neo_m17_test_prepare_pkt_payload(long_text, packed, full_bits, &app_len, &block, &lst, &crc), 0);
    err |= expect_int("packet max app len", app_len, M17_PACKET_MAX_APPLICATION_BYTES);
    err |= expect_int("packet max block count", block, M17_PACKET_MAX_FRAMES);
    err |= expect_u8("packet max last frame bytes", lst, M17_PACKET_CHUNK_BYTES);
    err |= expect_u8("packet max protocol", packed[0], 0x05U);
    err |= expect_u8("packet max last text", packed[M17_PACKET_MAX_APPLICATION_BYTES - 2U],
                     (uint8_t)long_text[M17_PACKET_MAX_APPLICATION_BYTES - 3U]);
    err |= expect_u8("packet max terminator", packed[M17_PACKET_MAX_APPLICATION_BYTES - 1U], 0U);
    err |= expect_int("packet max crc", crc, m17_crc16(packed, app_len));

    return err;
}

static int
test_packet_protocol_identifier_utf8_boundaries(void) {
    uint8_t out[4];
    int err = 0;

    DSD_MEMSET(out, 0xA5, sizeof(out));
    err |= expect_int("packet protocol null guard", (int)dsd_neo_m17_test_encode_packet_protocol_id(0x05U, NULL), 0);
    err |= expect_int("packet protocol high guard",
                      (int)dsd_neo_m17_test_encode_packet_protocol_id(M17_PACKET_PROTOCOL_MAX + 1U, out), 0);
    for (size_t i = 0U; i < sizeof(out); i++) {
        err |= expect_u8("packet protocol invalid preserves output", out[i], 0xA5U);
    }

    static const uint8_t want_sms[4] = {0x05U, 0xA5U, 0xA5U, 0xA5U};
    err |= expect_int("packet protocol one byte", (int)dsd_neo_m17_test_encode_packet_protocol_id(0x05U, out), 1);
    err |= expect_bytes("packet protocol one-byte layout", out, want_sms, sizeof(want_sms));

    DSD_MEMSET(out, 0xA5, sizeof(out));
    static const uint8_t want_text_meta[4] = {0xC2U, 0x80U, 0xA5U, 0xA5U};
    err |= expect_int("packet protocol two bytes", (int)dsd_neo_m17_test_encode_packet_protocol_id(0x80U, out), 2);
    err |= expect_bytes("packet protocol two-byte layout", out, want_text_meta, sizeof(want_text_meta));

    DSD_MEMSET(out, 0xA5, sizeof(out));
    static const uint8_t want_three[4] = {0xE0U, 0xA0U, 0x80U, 0xA5U};
    err |= expect_int("packet protocol three bytes", (int)dsd_neo_m17_test_encode_packet_protocol_id(0x0800U, out), 3);
    err |= expect_bytes("packet protocol three-byte layout", out, want_three, sizeof(want_three));

    DSD_MEMSET(out, 0xA5, sizeof(out));
    static const uint8_t want_max[4] = {0xF7U, 0xBFU, 0xBFU, 0xBFU};
    err |= expect_int("packet protocol four bytes",
                      (int)dsd_neo_m17_test_encode_packet_protocol_id(M17_PACKET_PROTOCOL_MAX, out), 4);
    err |= expect_bytes("packet protocol four-byte layout", out, want_max, sizeof(want_max));

    return err;
}

static int
test_encoder_packet_state_overrides_prepare_lsf_and_payload(void) {
    static dsd_state state;
    uint8_t lsf_bits[240];
    uint8_t packed[M17_PACKET_MAX_TOTAL_BYTES];
    uint16_t app_len = 0U;
    uint16_t lsf_crc = 0U;
    uint8_t can = 0U;
    unsigned long long dst = 0ULL;
    unsigned long long src = 0ULL;
    int err = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.m17_can_en = -1;
    DSD_MEMSET(lsf_bits, 0xA5, sizeof(lsf_bits));
    DSD_MEMSET(packed, 0xA5, sizeof(packed));
    err |= expect_int(
        "packet state helper null guard",
        dsd_neo_m17_test_prepare_pkt_from_state(NULL, lsf_bits, packed, &app_len, &lsf_crc, &can, &dst, &src), -1);
    err |= expect_int(
        "packet default state helper",
        dsd_neo_m17_test_prepare_pkt_from_state(&state, lsf_bits, packed, &app_len, &lsf_crc, &can, &dst, &src), 0);
    err |= expect_u8("packet default CAN", can, 7U);
    err |= expect_u64("packet default dst broadcast", dst, 0xFFFFFFFFFFFFULL);
    err |= expect_u64("packet default src", src, m17_encode_b40_callsign(0ULL, "DSD-neo  "));
    err |= expect_int("packet default LSF type", (int)bits_to_u64(lsf_bits + 96, 16U),
                      dsd_neo_m17_test_compose_frame_info(0U, 0U, 0U, 0U, 7U, 0U, 0U));
    err |= expect_int("packet default LSF CRC bits", (int)bits_to_u64(lsf_bits + M17_LSF_LSD_BITS, M17_LSF_CRC_BITS),
                      lsf_crc);
    err |= expect_int("packet default app length", app_len, 447);
    err |= expect_u8("packet default protocol", packed[0], 0x05U);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.m17_can_en = 3;
    DSD_SNPRINTF(state.str50c, sizeof(state.str50c), "%s", "N0CALL");
    DSD_SNPRINTF(state.str50b, sizeof(state.str50b), "%s", "ALL");
    DSD_SNPRINTF(state.m17sms, sizeof(state.m17sms), "%s", "override");
    DSD_MEMSET(lsf_bits, 0, sizeof(lsf_bits));
    DSD_MEMSET(packed, 0, sizeof(packed));

    err |= expect_int(
        "packet override state helper",
        dsd_neo_m17_test_prepare_pkt_from_state(&state, lsf_bits, packed, &app_len, &lsf_crc, &can, &dst, &src), 0);
    err |= expect_u8("packet override CAN", can, 3U);
    err |= expect_u64("packet ALL dst broadcast", dst, 0xFFFFFFFFFFFFULL);
    err |= expect_u64("packet override src", src, m17_encode_b40_callsign(0ULL, "N0CALL"));
    err |= expect_int("packet override LSF type", (int)bits_to_u64(lsf_bits + 96, 16U),
                      dsd_neo_m17_test_compose_frame_info(0U, 0U, 0U, 0U, 3U, 0U, 0U));
    err |= expect_int("packet override LSF CRC bits", (int)bits_to_u64(lsf_bits + M17_LSF_LSD_BITS, M17_LSF_CRC_BITS),
                      lsf_crc);
    err |= expect_int("packet override app len", app_len, 10);
    err |= expect_u8("packet override protocol", packed[0], 0x05U);
    err |= expect_bytes("packet override text", packed + 1, (const uint8_t*)"override", 8U);
    err |= expect_u8("packet override terminator", packed[9], 0U);

    return err;
}

int
main(void) {
    int err = 0;
    err |= test_embedded_lich_chunks_store_and_finalize_lsf_state();
    err |= test_embedded_lich_rejects_invalid_counter_and_gates_bad_lsf_crc();
    err |= test_lsf_application_resets_and_stores_state();
    err |= test_lsf_rejects_reserved_type_without_replacing_state();
    err |= test_lsf_rejects_invalid_addresses_without_replacing_state();
    err |= test_lsf_null_meta_decodes_text_and_honors_can_filter();
    err |= test_stream_dispatch_can_filter_and_aes_gates();
    err |= test_stream_dispatch_legacy_aes_key_segments();
    err |= test_stream_dispatch_rejects_invalid_arguments_and_unknown_encryption();
    err |= test_stream_dispatch_scrambler_decrypts_with_seed_and_subtype();
    err |= test_stream_dispatch_legacy_aes_array_key_segments();
    err |= test_stream_signature_frames_are_consumed_and_verify_without_key();
    err |= test_stream_signature_out_of_order_marks_sequence_error();
    err |= test_clear_signed_payload_updates_digest_and_dispatches();
#ifdef USE_CODEC2
    err |= test_stream_voice_3200_dispatch_routes_pair_audio_to_udp();
    err |= test_stream_voice_1600_dispatch_routes_single_audio_to_udp();
    err |= test_stream_voice_audio_gate_suppresses_udp_when_slot_disabled();
#endif
    err |= test_bert_payload_locks_from_default_state_and_continues();
    err |= test_bert_payload_resyncs_after_error_threshold();
    err |= test_bert_hard_payload_decode_primitives();
    err |= test_frame_info_packet_and_ip_helpers();
    err |= test_ip_stream_frames_apply_crc_gated_lsf_state();
    err |= test_ip_mpkt_frames_apply_crc_gated_packet_state();
    err |= test_packet_eot_finalization_crc_gates_decode_and_clears_state();
    err |= test_encoder_control_lsf_and_dibit_helpers();
    err |= test_encoder_packet_payload_layout();
    err |= test_packet_protocol_identifier_utf8_boundaries();
    err |= test_encoder_packet_state_overrides_prepare_lsf_and_payload();

    if (err == 0) {
        printf("M17_STATE_DISPATCH: OK\n");
    }
    return err;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
