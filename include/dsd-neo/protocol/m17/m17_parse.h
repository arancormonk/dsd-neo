// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief M17 frame parsing helpers.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * M17 LSF and payload parsing helpers.
 *
 * Provides small typed result structs so higher-level protocol handlers
 * can consume decoded metadata without directly manipulating bit buffers
 * or printing to stderr.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_PARSE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_PARSE_H_

#include <stddef.h>
#include <stdint.h>

#define M17_STREAM_FRAME_END_MASK    0x8000U
#define M17_STREAM_FRAME_COUNTER_MAX 0x7FFFU
#define M17_STREAM_SIGNATURE_FN0     0x7FFCU
#define M17_STREAM_SIGNATURE_FN1     0x7FFDU
#define M17_STREAM_SIGNATURE_FN2     0x7FFEU
#define M17_STREAM_SIGNATURE_FN3     0xFFFFU

#define M17_META_BYTES               14U
#define M17_TEXT_BLOCK_BYTES         13U
#define M17_TEXT_MAX_BLOCKS          4U
#define M17_TEXT_MAX_BYTES           ((size_t)M17_TEXT_BLOCK_BYTES * (size_t)M17_TEXT_MAX_BLOCKS)
#define M17_PACKET_PROTOCOL_MAX      0x1FFFFFU
#define M17_SIGNATURE_DIGEST_BYTES   16U
#define M17_SIGNATURE_BYTES          64U
#define M17_ADDRESS_STANDARD_MAX     0xEE6B27FFFFFFULL
#define M17_ADDRESS_EXTENDED_MIN     0xEE6B28000000ULL
#define M17_ADDRESS_EXTENDED_MAX     0xFFFFFFFFFFFEULL
#define M17_ADDRESS_BROADCAST        0xFFFFFFFFFFFFULL

enum m17_address_kind {
    M17_ADDRESS_RESERVED = 0,
    M17_ADDRESS_STANDARD = 1,
    M17_ADDRESS_EXTENDED = 2,
    M17_ADDRESS_BROADCAST_KIND = 3,
};

#define M17_GNSS_VALID_LATLON   0x8U
#define M17_GNSS_VALID_ALTITUDE 0x4U
#define M17_GNSS_VALID_VELOCITY 0x2U
#define M17_GNSS_VALID_RADIUS   0x1U

#ifdef __cplusplus
extern "C" {
#endif

struct m17_lsf_result {
    /* Raw destination and source IDs (numeric). */
    unsigned long long dst;
    unsigned long long src;

    /* Raw type word and decoded Air Interface v2.0.4 LSF layout version. */
    uint16_t type_word;
    uint8_t version;

    /* Decoded type fields from the LSF type word. */
    uint8_t packet_stream;
    uint8_t dt;
    uint8_t et;
    uint8_t es;
    uint8_t cn;
    uint8_t rs;

    uint8_t signature;
    uint8_t meta_is_iv;
    uint8_t dst_address_kind;
    uint8_t src_address_kind;
    uint8_t dst_is_valid;
    uint8_t src_is_valid;
    uint8_t type_reserved_valid;

    /* Decoded callsign strings (base-40) for dst/src. */
    char dst_csd[10];
    char src_csd[10];

    /* Optional 14-byte Meta/IV field when present. */
    uint8_t has_meta;
    uint8_t meta[14];
};

struct m17_gnss_result {
    uint8_t data_source;
    uint8_t station_type;
    uint8_t validity;
    uint8_t radius_exponent;
    uint16_t bearing_deg;
    double latitude_deg;
    double longitude_deg;
    float radius_m;
    float speed_kmh;
    float altitude_m;
    uint16_t reserved;
    uint8_t invalid_zero_fields;
};

struct m17_meta_text_block {
    uint8_t has_text;
    uint8_t total_blocks;
    uint8_t block_index;
    uint8_t length_bitmap;
    uint8_t block_bitmap;
    uint8_t text[M17_TEXT_BLOCK_BYTES];
};

struct m17_meta_text_assembler {
    uint8_t control_or;
    uint8_t expected_bitmap;
    uint8_t received_bitmap;
    uint8_t text[M17_TEXT_MAX_BYTES];
};

struct m17_extended_callsign_result {
    unsigned long long field1;
    unsigned long long field2;
    uint8_t has_field2;
    char field1_csd[10];
    char field2_csd[10];
};

struct m17_packet_protocol_result {
    uint32_t identifier;
    uint8_t length;
};

struct m17_signature_collector {
    uint8_t signature[M17_SIGNATURE_BYTES];
    uint8_t received_mask;
    uint8_t complete;
    uint8_t bad_sequence;
};

/**
 * Parse an M17 LSF bit buffer into a typed result.
 *
 * @param lsf_bits  Pointer to 240 bits (LSB=0/1) in MSB-first order.
 * @param bit_len   Number of bits available; must be at least 240.
 * @param out       Result struct to fill on success.
 *
 * @return 0 on success, negative on error (e.g., invalid args or length).
 */
int m17_parse_lsf(const uint8_t* lsf_bits, size_t bit_len, struct m17_lsf_result* out);

uint8_t m17_address_classify(unsigned long long address);
int m17_address_is_valid_destination(unsigned long long address);
int m17_address_is_valid_source(unsigned long long address);
int m17_address_decode_csd(unsigned long long address, char out_csd[10]);
int m17_lsf_type_reserved_bits_valid(const struct m17_lsf_result* lsf);
uint8_t m17_null_meta_protocol_for_subtype(uint8_t subtype);
int m17_can_filter_allows(int configured_can, uint8_t received_can);

/**
 * Parse an M17 GNSS metadata/PDU packet payload.
 *
 * @param input  Full packet payload including protocol byte 0x81 or 0x91.
 * @param len    Number of bytes available; must be at least 15.
 * @param out    Result struct to fill on success.
 *
 * @return 0 on success, negative on error.
 */
int m17_parse_gnss_v2(const uint8_t* input, size_t len, struct m17_gnss_result* out);

int m17_parse_extended_callsign_meta(const uint8_t* input, size_t len, struct m17_extended_callsign_result* out);

int m17_meta_text_parse_block(const uint8_t meta[M17_META_BYTES], struct m17_meta_text_block* out);
void m17_meta_text_assembler_reset(struct m17_meta_text_assembler* assembler);
int m17_meta_text_assembler_push(struct m17_meta_text_assembler* assembler, const struct m17_meta_text_block* block,
                                 char out_text[M17_TEXT_MAX_BYTES + 1U], uint8_t* out_len);

/**
 * Return a human-readable name for an M17 packet protocol identifier.
 *
 * @param protocol Protocol identifier from the decoded packet header.
 *
 * @return Constant string for known protocol IDs, or NULL if unknown/reserved.
 */
const char* m17_packet_protocol_name_u32(uint32_t protocol);
int m17_packet_protocol_decode(const uint8_t* input, size_t len, struct m17_packet_protocol_result* out);

/**
 * Return nonzero when a full 16-bit M17 stream frame-number field carries
 * signature payload instead of voice/data payload.
 */
int m17_stream_frame_is_signature(uint16_t frame_number);
int m17_stream_signature_frame_index(uint16_t frame_number);
void m17_signature_digest_init(uint8_t digest[M17_SIGNATURE_DIGEST_BYTES]);
void m17_signature_digest_update(uint8_t digest[M17_SIGNATURE_DIGEST_BYTES],
                                 const uint8_t payload[M17_SIGNATURE_DIGEST_BYTES]);
void m17_signature_collector_reset(struct m17_signature_collector* collector);
int m17_signature_collector_push(struct m17_signature_collector* collector, uint16_t frame_number,
                                 const uint8_t payload[M17_SIGNATURE_DIGEST_BYTES]);

/**
 * Assemble M17 1600 bps stream arbitrary-data chunks into a 0x99 payload.
 *
 * Each stream frame carries 8 arbitrary-data bytes. The frame number modulo 6
 * selects the chunk slot. When slot 5 is received, out_packet is filled with a
 * protocol byte followed by 48 assembled payload bytes.
 *
 * @return 1 when out_packet was completed, 0 when only a chunk was stored,
 *         negative on invalid arguments.
 */
int m17_stream_1600_arbitrary_assemble(uint8_t accumulator[48], uint16_t frame_number, const uint8_t chunk[8],
                                       uint8_t out_packet[49]);

/**
 * Decode M17 meta text segment control.
 *
 * Protocol 0x80 uses a bitmap control byte for up to four segments.
 * Protocol 0x83 uses high/low nibbles as length/segment directly.
 *
 * @return 0 on success, negative if protocol is not a supported meta text type.
 */
int m17_meta_text_segment_info(uint8_t protocol, uint8_t control, uint8_t* segment_num, uint8_t* segment_len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_M17_M17_PARSE_H_ */
