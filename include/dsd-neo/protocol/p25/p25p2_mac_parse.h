// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief P25 Phase 2 MAC parser interfaces.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 MAC VPDU/TSBK parsing helpers.
 *
 * Provides a small typed result struct describing the MAC header and
 * message lengths so higher-level code can make decisions without
 * re-implementing table lookups and MCO fallbacks.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_MAC_PARSE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_MAC_PARSE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P25P2_MAC_MAX_SEGMENTS 3

/*
 * MAC message octets a transport carries: MAC[1]..MAC[span], after the one-octet MAC header (opcode, offset) and before
 * the CRC. FACCH: 156 bits, CRC-12 over the first 144 (8 header + 136 message bits). SACCH: 180 bits, CRC-12 over the
 * first 168 (8 + 160). LCCH: 180 bits, CRC-16 over the first 164, the last 12 of which are the NAC (8 + 144).
 */
#define P25P2_MAC_SPAN_FACCH   17
#define P25P2_MAC_SPAN_SACCH   20
#define P25P2_MAC_SPAN_LCCH    18

struct p25p2_mac_segment {
    /* Offset, in octets, from MAC[1] to the segment opcode. */
    int offset;
    /* Segment length in octets, including opcode/vendor/length octets. */
    int length;
};

struct p25p2_mac_result {
    /* Channel type: 0 = FACCH, 1 = SACCH. */
    int type;
    /* MAC message octets the transport carries (P25P2_MAC_SPAN_*): every segment ends at or before MAC[span], except
       a first segment whose own length claims more. */
    int span;
    /* MFID and opcode from the MAC header. */
    uint8_t mfid;
    uint8_t opcode;
    int segment_count;
    struct p25p2_mac_segment segments[P25P2_MAC_MAX_SEGMENTS];
};

struct p25p2_mac_voice_identity {
    int tg;
    int dst;
    int src;
    int is_group;
    int svc_bits;
    int source_optional;
};

struct p25p2_iden_update {
    uint8_t iden;
    uint8_t chan_type;
    uint8_t bw_vu;
    int bandwidth;
    int trans_off;
    int chan_spac;
    long int base_freq;
};

/**
 * Derive MAC message lengths and header fields from a VPDU/TSBK buffer.
 *
 * This helper centralizes table lookups and MCO-based fallbacks. It does not
 * depend on full dsd_state/opts to keep the interface test-friendly.
 *
 * @param type   Channel type: 0 = FACCH, 1 = SACCH.
 * @param mac    Pointer to up to 24 MAC words (one octet per entry).
 * @param out    Result struct to fill on success.
 *
 * @return 0 on success, negative on error.
 */
int p25p2_mac_parse(int type, const unsigned long long mac[24], struct p25p2_mac_result* out);
/**
 * MAC message span of a transport: P25P2_MAC_SPAN_LCCH for an LCCH (SACCH-type burst with a CRC-16 and NAC), else
 * P25P2_MAC_SPAN_SACCH for type 1 and P25P2_MAC_SPAN_FACCH for type 0.
 */
int p25p2_mac_span(int type, int is_lcch);
/**
 * As p25p2_mac_parse(), segmenting the MAC messages within @p span octets (p25p2_mac_span()). p25p2_mac_parse() uses
 * the SACCH or FACCH span of @p type.
 */
int p25p2_mac_parse_span(int type, int span, const unsigned long long mac[24], struct p25p2_mac_result* out);
/**
 * Decode the last standard Group Voice, Unit-to-Unit Voice, Telephone Interconnect Voice, or Group Regroup Voice
 * structure in a MAC PDU.
 *
 * @return 1 when an identity was decoded, 0 when no voice structure is present,
 *         or a negative value on invalid input.
 */
int p25p2_mac_decode_voice_identity(int type, const unsigned long long mac[24], struct p25p2_mac_voice_identity* out);
int p25p2_mac_decode_iden_standard(const unsigned long long mac[24], int pos, struct p25p2_iden_update* out);
int p25p2_mac_decode_iden_vuhf(const unsigned long long mac[24], int pos, struct p25p2_iden_update* out);
int p25p2_mac_decode_iden_tdma(const unsigned long long mac[24], int pos, struct p25p2_iden_update* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_MAC_PARSE_H_ */
