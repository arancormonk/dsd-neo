// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "fixtures/m17_reference_vectors.h"
#include "m17_algorithms.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/ecdsa.h>
#include <dsd-neo/protocol/m17/m17_parse.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int
expect_u32(const char* label, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%X want 0x%X\n", label, got, want);
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

static int
is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static int
expect_commit_id(const char* label, const char* value) {
    if (strlen(value) != 40U) {
        DSD_FPRINTF(stderr, "%s: invalid commit id length\n", label);
        return 1;
    }
    for (size_t i = 0U; i < 40U; i++) {
        if (!is_hex_digit(value[i])) {
            DSD_FPRINTF(stderr, "%s: invalid commit id character\n", label);
            return 1;
        }
    }
    return 0;
}

static int
expect_sync_symbols(const char* label, uint16_t sync_word, const int8_t want[M17_SYNC_SYMBOLS]) {
    int err = 0;
    uint8_t dibits[M17_SYNC_SYMBOLS];

    m17_fill_sync_dibits_from_word(sync_word, dibits);
    for (int i = 0; i < M17_SYNC_SYMBOLS; i++) {
        err |= expect_int(label, m17_symbol_from_dibit(dibits[i]), want[i]);
    }
    return err;
}

static void
bytes_to_bits(const uint8_t* bytes, uint8_t* bits, size_t byte_count) {
    for (size_t byte = 0U; byte < byte_count; byte++) {
        for (size_t bit = 0U; bit < 8U; bit++) {
            bits[(byte * 8U) + bit] = (uint8_t)((bytes[byte] >> (7U - bit)) & 1U);
        }
    }
}

static void
bits_to_bytes_padded(const uint8_t* bits, size_t bit_count, uint8_t* bytes, size_t byte_count) {
    DSD_MEMSET(bytes, 0, byte_count);
    for (size_t i = 0U; i < bit_count; i++) {
        bytes[i / 8U] = (uint8_t)((bytes[i / 8U] << 1U) | (bits[i] & 1U));
        if ((i % 8U) == 7U) {
            continue;
        }
    }

    const size_t tail = bit_count % 8U;
    if (tail != 0U && bit_count / 8U < byte_count) {
        bytes[bit_count / 8U] = (uint8_t)(bytes[bit_count / 8U] << (8U - tail));
    }
}

static uint64_t
fnv1a64_dibits(const uint8_t dibits[M17_FRAME_SYMBOLS]) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (int i = 0; i < M17_FRAME_SYMBOLS; i++) {
        hash ^= (uint64_t)(dibits[i] & 0x03U);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int
expect_frame_hash(const char* label, const uint8_t dibits[M17_FRAME_SYMBOLS], uint64_t want) {
    const uint64_t got = fnv1a64_dibits(dibits);
    if (got != want) {
        DSD_FPRINTF(stderr, "%s FNV64: got 0x%016llX want 0x%016llX\n", label, (unsigned long long)got,
                    (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
build_lsf_bits(uint8_t lsf_type1[M17_LSF_TYPE1_FLUSH_BITS]) {
    DSD_MEMSET(lsf_type1, 0, M17_LSF_TYPE1_FLUSH_BITS);
    bytes_to_bits(M17_REF_LSF_BYTES, lsf_type1, sizeof(M17_REF_LSF_BYTES));
    return 0;
}

static void
build_payload_bits_from_bytes(const uint8_t bytes[16], uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS]) {
    bytes_to_bits(bytes, payload_bits, 16U);
}

static void
build_packet_chunk_bits(uint8_t chunk_bits[M17_PACKET_CHUNK_BITS]) {
    bytes_to_bits(M17_REF_PACKET_CHUNK_BYTES, chunk_bits, sizeof(M17_REF_PACKET_CHUNK_BYTES));
}

static int
build_stream_frame_hash(uint16_t frame_number, const uint8_t payload_bytes[16], uint8_t lich_cnt, uint64_t want,
                        const char* label) {
    int err = 0;
    uint8_t lsf_type1[M17_LSF_TYPE1_FLUSH_BITS];
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t stream_type1[M17_STREAM_TYPE1_FLUSH_BITS];
    uint8_t stream_punctured[M17_STREAM_PUNCTURED_BITS];
    uint8_t lich_content[M17_LICH_CONTENT_BITS];
    uint8_t lich_encoded[M17_LICH_BITS];
    uint8_t combined[M17_PAYLOAD_BITS];
    uint8_t randomized[M17_PAYLOAD_BITS];
    uint8_t dibits[M17_FRAME_SYMBOLS];

    build_lsf_bits(lsf_type1);
    build_payload_bits_from_bytes(payload_bytes, payload_bits);
    m17_stream_build_type1_bits(frame_number, payload_bits, stream_type1);
    m17_stream_encode_type1_bits(stream_type1, stream_punctured);
    err |= expect_int("LICH content", m17_lich_build_content(lsf_type1, lich_cnt, lich_content), 0);
    m17_lich_encode_bits(lich_content, lich_encoded);
    m17_stream_combine_frame_bits(lich_encoded, stream_punctured, combined);
    m17_payload_encode_bits(combined, randomized);
    m17_frame_build_dibits(M17_SYNC_STREAM_WORD, randomized, dibits);
    err |= expect_frame_hash(label, dibits, want);
    return err;
}

static int
test_reference_vector_metadata(void) {
    int err = 0;

    err |= expect_commit_id("reference gr-m17 commit", M17_REF_GR_M17_COMMIT);
    err |= expect_commit_id("reference m17-cxx-demod commit", M17_REF_M17_IMPLEMENTATIONS_COMMIT);
    err |= expect_commit_id("reference libm17 commit", M17_REF_LIBM17_COMMIT);
    err |= expect_int("reference GNU Radio version present", M17_REF_GNURADIO_VERSION[0] != '\0', 1);
    err |= expect_sync_symbols("reference stream sync", M17_SYNC_STREAM_WORD, M17_REF_STREAM_SYNC_SYMBOLS);
    err |= expect_sync_symbols("reference packet sync", M17_SYNC_PACKET_WORD, M17_REF_PACKET_SYNC_SYMBOLS);
    return err;
}

static int
test_lsf_reference(void) {
    int err = 0;
    uint8_t lsf_type1[M17_LSF_TYPE1_FLUSH_BITS];
    uint8_t randomized[M17_PAYLOAD_BITS];
    uint8_t dibits[M17_FRAME_SYMBOLS];
    struct m17_lsf_result lsf;
    uint16_t consumed = 0U;

    build_lsf_bits(lsf_type1);
    err |= expect_u32("LSF CRC", m17_crc16(M17_REF_LSF_BYTES, M17_LSF_LSD_BYTES), M17_REF_LSF_CRC);
    err |= expect_u32("LSF CRC residue", m17_crc16(M17_REF_LSF_BYTES, M17_LSF_BYTES), 0U);
    err |= expect_int("LSF parse", m17_parse_lsf(lsf_type1, M17_LSF_TYPE1_BITS, &lsf), 0);
    err |= expect_u32("LSF type", lsf.type_word, M17_REF_LSF_TYPE_WORD);
    err |= expect_int("LSF dst", strcmp(lsf.dst_csd, M17_REF_LSF_DST_CSD), 0);
    err |= expect_int("LSF src", strcmp(lsf.src_csd, M17_REF_LSF_SRC_CSD), 0);
    err |= expect_u32("LSF packet_stream", lsf.packet_stream, 1U);
    err |= expect_u32("LSF dt", lsf.dt, 2U);
    err |= expect_u32("LSF et", lsf.et, 2U);
    err |= expect_u32("LSF es", lsf.es, 0U);
    err |= expect_u32("LSF CAN", lsf.cn, 9U);
    err |= expect_u32("LSF signature", lsf.signature, 1U);
    err |= expect_u32("LSF reserved", lsf.rs, 0U);
    err |= expect_u32("LSF type reserved valid", lsf.type_reserved_valid, 1U);
    err |= expect_u32("LSF meta is IV", lsf.meta_is_iv, 1U);
    err |= expect_bytes("LSF meta", lsf.meta, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));

    err |=
        expect_int("LSF RF encode bits", m17_lsf_encode_type1_bits(lsf_type1, randomized, &consumed), M17_PAYLOAD_BITS);
    err |= expect_int("LSF RF consumed", consumed, M17_LSF_TYPE2_BITS);
    m17_frame_build_dibits(M17_SYNC_LSF_WORD, randomized, dibits);
    err |= expect_frame_hash("LSF frame", dibits, M17_REF_LSF_FRAME_DIBIT_FNV64);
    return err;
}

static int
test_stream_reference(void) {
    int err = 0;
    uint8_t payload_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t parsed_bits[M17_STREAM_PAYLOAD_BITS];
    uint8_t parsed_bytes[16];
    uint8_t type1[M17_STREAM_TYPE1_FLUSH_BITS];
    uint16_t parsed_fn = 0U;

    build_payload_bits_from_bytes(M17_REF_STREAM_PAYLOAD_BYTES, payload_bits);
    m17_stream_build_type1_bits(M17_REF_STREAM_FN, payload_bits, type1);
    err |= expect_int("stream parse", m17_stream_parse_type1_bits(type1, &parsed_fn, parsed_bits), 0);
    err |= expect_u32("stream FN", parsed_fn, M17_REF_STREAM_FN);
    bits_to_bytes_padded(parsed_bits, M17_STREAM_PAYLOAD_BITS, parsed_bytes, sizeof(parsed_bytes));
    err |= expect_bytes("stream payload", parsed_bytes, M17_REF_STREAM_PAYLOAD_BYTES, sizeof(parsed_bytes));
    err |= build_stream_frame_hash(M17_REF_STREAM_FN, M17_REF_STREAM_PAYLOAD_BYTES, 0U,
                                   M17_REF_STREAM_FRAME_DIBIT_FNV64, "stream frame");
    return err;
}

static int
test_packet_reference(void) {
    int err = 0;
    uint8_t eof = 0U;
    uint8_t value = 0U;
    uint8_t chunk_bits[M17_PACKET_CHUNK_BITS];
    uint8_t type1[M17_PACKET_TYPE1_FLUSH_BITS];
    uint8_t randomized[M17_PAYLOAD_BITS];
    uint8_t dibits[M17_FRAME_SYMBOLS];
    uint16_t consumed = 0U;

    err |= expect_u32("packet CRC", m17_crc16(M17_REF_PACKET_APP_BYTES, sizeof(M17_REF_PACKET_APP_BYTES)),
                      M17_REF_PACKET_CRC);
    err |=
        expect_int("packet metadata parse", m17_packet_parse_metadata_byte(M17_REF_PACKET_METADATA, &eof, &value), 0);
    err |= expect_u32("packet eof", eof, 1U);
    err |= expect_u32("packet eof bytes", value, 8U);

    build_packet_chunk_bits(chunk_bits);
    m17_packet_build_type1_bits(chunk_bits, M17_REF_PACKET_METADATA, type1);
    err |= expect_int("packet RF encode bits", m17_packet_encode_type1_bits(type1, randomized, &consumed),
                      M17_PAYLOAD_BITS);
    err |= expect_int("packet RF consumed", consumed, M17_PACKET_TYPE2_BITS);
    m17_frame_build_dibits(M17_SYNC_PACKET_WORD, randomized, dibits);
    err |= expect_frame_hash("packet frame", dibits, M17_REF_PACKET_FRAME_DIBIT_FNV64);
    return err;
}

static int
test_bert_reference(void) {
    int err = 0;
    uint16_t lfsr = M17_REF_BERT_INITIAL_LFSR;
    uint8_t payload_bits[M17_BERT_PAYLOAD_BITS];
    uint8_t packed[sizeof(M17_REF_BERT_PAYLOAD_PACKED_BYTES)];
    uint8_t type1[M17_BERT_TYPE1_FLUSH_BITS];
    uint8_t randomized[M17_PAYLOAD_BITS];
    uint8_t dibits[M17_FRAME_SYMBOLS];
    uint16_t consumed = 0U;

    m17_prbs9_fill_bits(&lfsr, payload_bits, M17_BERT_PAYLOAD_BITS);
    err |= expect_u32("BERT final LFSR", lfsr, M17_REF_BERT_FINAL_LFSR_AFTER_PAYLOAD);
    bits_to_bytes_padded(payload_bits, M17_BERT_PAYLOAD_BITS, packed, sizeof(packed));
    err |= expect_bytes("BERT packed payload", packed, M17_REF_BERT_PAYLOAD_PACKED_BYTES, sizeof(packed));

    m17_bert_build_type1_bits(payload_bits, type1);
    err |=
        expect_int("BERT RF encode bits", m17_bert_encode_type1_bits(type1, randomized, &consumed), M17_PAYLOAD_BITS);
    err |= expect_int("BERT RF consumed", consumed, M17_BERT_TYPE2_BITS);
    m17_frame_build_dibits(M17_SYNC_BERT_WORD, randomized, dibits);
    err |= expect_frame_hash("BERT frame", dibits, M17_REF_BERT_FRAME_DIBIT_FNV64);
    return err;
}

static int
test_aes_reference(void) {
    int err = 0;
    uint8_t counter[M17_AES_COUNTER_BYTES];
    uint8_t data[sizeof(M17_REF_AES_PLAINTEXT)];
    uint8_t full_counter[M17_AES_COUNTER_BYTES];

    m17_aes_build_counter(M17_REF_AES_NONCE, M17_REF_AES_TRANSMITTED_FN, counter);
    err |= expect_bytes("AES masked counter", counter, M17_REF_AES_COUNTER, sizeof(counter));

    DSD_MEMCPY(data, M17_REF_AES_PLAINTEXT, sizeof(data));
    aes_ctr_xcrypt_bytes(counter, M17_REF_AES128_KEY, data, 0, sizeof(data));
    err |= expect_bytes("AES final-frame ciphertext", data, M17_REF_AES_CIPHERTEXT, sizeof(data));

    DSD_MEMCPY(full_counter, M17_REF_AES_NONCE, sizeof(M17_REF_AES_NONCE));
    full_counter[14] = 0xFFU;
    full_counter[15] = 0xFFU;
    DSD_MEMCPY(data, M17_REF_AES_PLAINTEXT, sizeof(data));
    aes_ctr_xcrypt_bytes(full_counter, M17_REF_AES128_KEY, data, 0, sizeof(data));
    err |= expect_int("AES full 16-bit counter differs", memcmp(data, M17_REF_AES_CIPHERTEXT, sizeof(data)) == 0, 0);
    err |= build_stream_frame_hash(M17_REF_AES_TRANSMITTED_FN, M17_REF_AES_CIPHERTEXT, 1U,
                                   M17_REF_AES_STREAM_FRAME_DIBIT_FNV64, "AES stream frame");
    return err;
}

static int
test_signature_reference(void) {
    int err = 0;
    struct m17_signature_collector collector;
    static const uint16_t signature_fns[4] = {M17_STREAM_SIGNATURE_FN0, M17_STREAM_SIGNATURE_FN1,
                                              M17_STREAM_SIGNATURE_FN2, M17_STREAM_SIGNATURE_FN3};

    m17_signature_collector_reset(&collector);
    for (size_t i = 0U; i < 4U; i++) {
        const int want = (i == 3U) ? 1 : 0;
        err |= expect_int("signature collector",
                          m17_signature_collector_push(&collector, signature_fns[i],
                                                       &M17_REF_SIGNATURE_BYTES[i * M17_SIGNATURE_DIGEST_BYTES]),
                          want);
    }
    err |= expect_u32("signature complete", collector.complete, 1U);
    err |= expect_bytes("signature collected", collector.signature, M17_REF_SIGNATURE_BYTES,
                        sizeof(M17_REF_SIGNATURE_BYTES));
    err |= expect_int("signature verify",
                      dsd_ecdsa_p256_verify_digest(M17_REF_SIGNATURE_DIGEST, sizeof(M17_REF_SIGNATURE_DIGEST),
                                                   M17_REF_SIGNATURE_PUBLIC_KEY, collector.signature),
                      1);
    err |= build_stream_frame_hash(M17_STREAM_SIGNATURE_FN0, M17_REF_SIGNATURE_BYTES, 2U,
                                   M17_REF_SIGNED_STREAM_FRAME_DIBIT_FNV64, "signed stream frame");
    return err;
}

int
main(void) {
    int err = 0;
    err |= test_reference_vector_metadata();
    err |= test_lsf_reference();
    err |= test_stream_reference();
    err |= test_packet_reference();
    err |= test_bert_reference();
    err |= test_aes_reference();
    err |= test_signature_reference();

    if (err == 0) {
        printf("M17_REFERENCE_VECTORS: OK\n");
    }
    return err;
}
