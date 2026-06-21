// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "fixtures/m17_reference_vectors.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/protocol/m17/m17_parse.h"

struct CODEC2;

static dsd_opts g_opts;
static dsd_state g_state;

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
CNXDNConvolution_start(void) {}

void
CNXDNConvolution_decode(uint8_t s0, uint8_t s1) {
    (void)s0;
    (void)s1;
}

void
CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits) {
    (void)out;
    (void)nBits;
}

void
codec2_decode(struct CODEC2* codec2_state, short speech[], const unsigned char* bits) {
    (void)codec2_state;
    (void)speech;
    (void)bits;
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

int
main(void) {
    int err = 0;
    err |= test_lsf_application_resets_and_stores_state();
    err |= test_lsf_rejects_reserved_type_without_replacing_state();
    err |= test_stream_dispatch_can_filter_and_aes_gates();

    if (err == 0) {
        printf("M17_STATE_DISPATCH: OK\n");
    }
    return err;
}
