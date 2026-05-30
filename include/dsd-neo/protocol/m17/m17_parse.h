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

#ifdef __cplusplus
extern "C" {
#endif

struct m17_lsf_result {
    /* Raw destination and source IDs (numeric). */
    unsigned long long dst;
    unsigned long long src;

    /* Raw type word and decoded LSF layout version. */
    uint16_t type_word;
    uint8_t version;

    /* Decoded type fields from the LSF type word. */
    uint8_t dt;
    uint8_t et;
    uint8_t es;
    uint8_t cn;
    uint8_t rs;

    /*
     * V3 type-word fields. For V2, payload_contents mirrors dt and
     * meta_contents mirrors the legacy meta protocol selector.
     */
    uint8_t payload_contents;
    uint8_t encryption_type;
    uint8_t signature;
    uint8_t meta_contents;
    uint8_t meta_is_iv;

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

/**
 * Return a human-readable name for an M17 packet protocol identifier.
 *
 * @param protocol 8-bit protocol code from packet payload octet 0.
 *
 * @return Constant string for known protocol IDs, or NULL if unknown/reserved.
 */
const char* m17_packet_protocol_name(uint8_t protocol);

/**
 * Return nonzero when an M17 stream frame number carries signature payload
 * instead of voice/data payload.
 */
int m17_stream_frame_is_signature(uint16_t frame_number);

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
