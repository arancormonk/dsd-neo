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

/**
 * GNSS Position Data META (LSF/LSD META field with encryption type 00 and subtype 01, M17 specification v2.0.11).
 * The raw fields are what the transmitter sent; the display fields (degrees, metres, km/h) are filled only for the
 * parts marked valid and in range, and read zero otherwise.
 */
struct m17_gnss_result {
    uint8_t data_source;
    uint8_t station_type;
    uint8_t validity; /**< M17_GNSS_VALID_* bits */
    uint8_t radius_exponent;
    uint16_t bearing_deg; /**< whole degrees; 0 when velocity is not valid or the bearing is out of range */
    double latitude_deg;
    double longitude_deg;
    float radius_m;
    float speed_kmh;
    float altitude_m;
    uint16_t reserved;
    uint8_t invalid_zero_fields; /**< M17_GNSS_VALID_* bits not marked valid whose fields are not zero */
    /**
     * M17_GNSS_VALID_* bits marked valid whose value lies outside the specified range; the field is unknown:
     * M17_GNSS_VALID_LATLON for a coordinate of -2^23 (which the specification never uses), M17_GNSS_VALID_VELOCITY
     * for a bearing above 359 degrees (the bearing only; the speed stays).
     */
    uint8_t out_of_range_fields;
    int32_t latitude_raw;  /**< 24-bit two's complement, sign-extended; +-8388607 is +-90 degrees */
    int32_t longitude_raw; /**< 24-bit two's complement, sign-extended; +-8388607 is +-180 degrees */
    uint16_t altitude_raw; /**< 0.5 m steps, offset by 500 m */
    uint16_t speed_raw;    /**< 12 bits, 0.5 km/h steps */
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
int m17_address_decode_csd(unsigned long long address, char out_csd[10]);
int m17_lsf_type_reserved_bits_valid(const struct m17_lsf_result* lsf);
uint8_t m17_null_meta_protocol_for_subtype(uint8_t subtype);
int m17_can_filter_allows(int configured_can, uint8_t received_can);

/**
 * Parse the 14-byte GNSS Position Data META field (M17 specification v2.0.11): META byte k is @p meta[k], with no
 * packet protocol specifier in front. M17 defines no GNSS packet protocol; position data comes only from META.
 *
 * @param meta  The META bytes.
 * @param len   Number of bytes available; must be at least M17_META_BYTES.
 * @param out   Result struct to fill on success.
 *
 * @return 0 on success; -1 for NULL arguments, -2 for a short field, -4 when the reserved bits are not zero.
 */
int m17_parse_gnss_meta(const uint8_t* meta, size_t len, struct m17_gnss_result* out);

/**
 * Parse the 14-byte Extended Callsign Data META field: META byte k is @p meta[k], with no packet protocol specifier in
 * front.
 *
 * @return 0 on success; -1 for NULL arguments, -2 for a short field, -4 when the reserved tail is not zero, -5 or -6
 *         when a callsign field is no standard address.
 */
int m17_parse_extended_callsign_meta(const uint8_t* meta, size_t len, struct m17_extended_callsign_result* out);

int m17_meta_text_parse_block(const uint8_t meta[M17_META_BYTES], struct m17_meta_text_block* out);
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

/** Return the signature chunk index (0..3), or -1 for a non-signature frame. */
int m17_stream_signature_frame_index(uint16_t frame_number);
void m17_signature_digest_update(uint8_t digest[M17_SIGNATURE_DIGEST_BYTES],
                                 const uint8_t payload[M17_SIGNATURE_DIGEST_BYTES]);
/**
 * Fold stream frame @p frame_number's payload into @p acc, in any order.
 *
 * The voice-stream digest (M17 spec 2.0.11, Message Digest Algorithm for Voice Streams) XORs each frame's payload in
 * and rotates left one byte, frame by frame from frame 0: after N frames it is the XOR of each payload k rotated left
 * N - k bytes. That is the XOR of each payload rotated right k bytes, rotated left N bytes as a whole, so a frame can
 * be folded in whenever it arrives (a network reorders datagrams) and the digest read for any N
 * (m17_signature_digest_from_fold()). Each frame must be folded once.
 */
void m17_signature_digest_fold(uint8_t acc[M17_SIGNATURE_DIGEST_BYTES], uint16_t frame_number,
                               const uint8_t payload[M17_SIGNATURE_DIGEST_BYTES]);
/** The digest of frames 0 to @p frames - 1 from their fold @p acc (m17_signature_digest_fold()). */
void m17_signature_digest_from_fold(const uint8_t acc[M17_SIGNATURE_DIGEST_BYTES], uint32_t frames,
                                    uint8_t digest[M17_SIGNATURE_DIGEST_BYTES]);
/**
 * Store a signature chunk (frame numbers 0x7FFC, 0x7FFD, 0x7FFE, 0xFFFF). Returns 1 when this chunk completed the set,
 * whichever chunk it is (a network can reorder them), 0 when stored, -3 when stored out of sequence, -2 for a frame
 * number that is no signature chunk, -1 for NULL.
 */
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
