// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief GPS/NMEA decode interfaces.
 *
 * Declares GPS helper entrypoints implemented in `src/core/gps/dsd_gps.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_GPS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_GPS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dsd_position_fix;
struct dsd_position_origin;

/** Octets in an L3Harris GPS update body: Part A octets 2-8, then Part B octets 2-8 (TIA-102.AABF-D-1 §7.3.35-36). */
#define DSD_HARRIS_GPS_BODY_OCTETS 14

/**
 * Where a P25 link-control or MAC GPS report came from and which checks it passed (issue #800). The decoder admits
 * the report with these verdicts (dsd_position_admit()); nothing reaches `-L`, history or the feed without
 * @c pos_verified, and the source, target and group flag only with @c source_verified.
 */
typedef struct dsd_gps_report_ctx {
    /** The reception captured with the report's first fragment (L3Harris Part A); NULL for a single message, which
     * takes the current reception. */
    const struct dsd_position_origin* origin;
    /** The call the message carrying the report has just ended, captured before it did
     * (dsd_event_capture_closing_call(); a P25 Phase 1 TDULC, spec N2): when valid, the history row is written through
     * dsd_event_enrich_position_closing(). NULL otherwise (Phase 2). */
    const struct dsd_event_closing_call* closing;
    /** The call the report belongs to: its epoch (dsd_call_snapshot::epoch); 0 enriches the slot's ACTIVE call, if
     * any. Ignored when @c closing is valid. */
    uint64_t call_epoch;
    uint32_t source_id;      /**< the call's source; 0 = none */
    uint32_t target_id;      /**< the call's target; 0 = none */
    int protocol;            /**< DSD_SYNC_* of the carrier */
    uint8_t slot;            /**< lane: 0 or 1 */
    uint8_t pos_verified;    /**< 1 when the message's own check passed (hard RS decode, MAC CRC, whole segment) */
    uint8_t source_verified; /**< 1 when source, target and group flag came from checked provenance */
    uint8_t target_group;    /**< 1 when @c target_id is a talkgroup */
} dsd_gps_report_ctx;

/**
 * @brief A LIP short location report (ETSI TS 100 392-18-1 V1.7.2 Table 6.1), decoded and range-checked into the
 * position feed's units (<dsd-neo/core/position.h>).
 */
typedef struct {
    int32_t lat_e7;              /**< latitude, degrees x 1e7, north positive (Table 6.1, 24-bit two's complement) */
    int32_t lon_e7;              /**< longitude, degrees x 1e7, east positive (25-bit two's complement) */
    uint32_t err_m;              /**< Table 6.93: codes 0..5 are 2 m..200 km, 6 is 200 km (a lower bound), 7 is 0 */
    uint16_t speed_dkmh;         /**< 0.1 km/h (Table 6.51); 126 is 1043 km/h (a lower bound), 127 is unknown */
    uint16_t heading_ddeg;       /**< 0.1 degree (Table 6.45): code x 225; 15 is 3375 and ambiguous */
    uint16_t flags;              /**< dsd_position_flag: SPEED_LOWER_BOUND, ERR_LOWER_BOUND, HEADING_AMBIGUOUS and
                                   VALIDITY_UNKNOWN (position error 7, decision D11) */
    uint8_t time_elapsed;        /**< Table 6.111 code 0..3 (dsd_position_fix::age_class) */
    uint8_t position_error;      /**< Table 6.93 code 0..7 */
    uint8_t velocity_code;       /**< Table 6.51 code 0..127 */
    uint8_t direction_code;      /**< Table 6.45 code 0..15 */
    uint8_t reason;              /**< reason for sending (Table 6.94), or user-defined data */
    uint8_t reason_is_user_data; /**< type of additional data: 1 = user-defined data, 0 = reason for sending */
} lip_short_report;

/** The fixed length in bits of a LIP short location report (Table 6.1). */
#define LIP_SHORT_REPORT_BITS 76U

/**
 * @brief Decode a LIP short location report from @p bits (one bit per element, MSB first, starting at the 2-bit PDU
 * type). Pure: no options, no state, no output. Every value is range-checked before it is converted, and an unknown
 * one is left unknown.
 *
 * @return 1 when decoded into @p out; 0 when the PDU is not a short report (PDU type other than 0) or has fewer than
 *         LIP_SHORT_REPORT_BITS bits (@p out untouched); -1 for NULL arguments.
 */
int lip_parse_short_report(const uint8_t* bits, size_t bit_count, lip_short_report* out);

/** Copy a decoded short report's position fields into @p fix: lat_e7, lon_e7, err_m, speed_dkmh, heading_ddeg,
 * age_class, and its flags (ORed in). Leaves every other field alone. NULL does nothing. */
void lip_short_report_to_fix(const lip_short_report* report, struct dsd_position_fix* fix);

/**
 * @brief The text of a decoded short report, as history rows and the console show it:
 * "LIP: <lat> <lon>; <position error>; Spd: <speed>; Dir: <direction>", with unknown and bounded codes said as such
 * (position error 7 reads "fix may be old or invalid"). Print it only for a report dsd_position_admit() admitted.
 * @p out is always terminated when @p cap > 0; a NULL @p report leaves it empty.
 */
void lip_short_report_text(const lip_short_report* report, char* out, size_t cap);

/**
 * @brief Write the location file (-L) row of an admitted LIP short report from its admitted record @p fix: its source
 * (0 when admission cleared it), coordinates, speed in whole km/h (0 when unknown, 1043 for the lower bound) and
 * direction in whole degrees (0 when ambiguous). Only when -L is on. Never call it for a refused report or a hashed
 * source (decision D8).
 */
void lip_write_location_row(const dsd_opts* opts, const struct dsd_position_fix* fix);

/**
 * @brief The DMR embedded GPS LC (TS 102 361-2 Table 7.3). The caller (dmr_flco) runs it only for an unprotected LC
 * whose embedded checksum passed. Its source is the slot's ACTIVE call's when a checked full LC of that call named it
 * (dsd_state::dmr_lc_checked_transmission / dmr_lc_checked_source), and its destination with the group flag when that
 * LC also named the call's target (dmr_lc_checked_target / dmr_lc_checked_group); otherwise the report is unattributed.
 */
void dmr_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]);
/**
 * Motorola MFID $90 LCO $06 embedded GPS (layout from one non-official source: FORMAT_PROVISIONAL, D12).
 * @return the dsd_position_admit() result (0 when nothing was admitted).
 */
int apx_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[], const dsd_gps_report_ctx* ctx);
/**
 * @brief A DMR USBD LIP Poll Response (TS 102 361-4 Table 6.79): the caller runs it only for an inbound burst (the
 * MS-sourced data sync) and inside the burst's event_crc_invalid scope, whose verdict decides whether the position was
 * checked. The source is an 8-bit hash, never a radio ID. An admitted report is staged for the slot's next data notice
 * and its text left in dsd_state::dmr_embedded_gps; it is never written to the location file (decision D8).
 */
void lip_protocol_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* input);

/** The addressing the checked carrier of a report gave it (a UDT header, a data packet): its source, its destination
 *  (0 = none) and whether that destination is a talkgroup. Both IDs are covered by the carrier's own check. */
typedef struct {
    uint32_t source_id;
    uint32_t target_id;
    uint8_t target_group;
} dsd_gps_address;

/**
 * @brief A LIP PDU carried by a DMR UDT or a UDP datagram, addressed by @p addr (NULL: none). Callers run it inside the carrier's
 * event_crc_invalid scope (the UDT header and continuation CRCs; the data packet's header, CRC32 and CRC9 checks),
 * whose verdict decides whether the report was checked. An admitted short report is staged for the slot's next data
 * notice (dsd_event_stage_position()), its text left in dsd_state::dmr_embedded_gps and written to the location file.
 */
void lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count,
                     const dsd_gps_address* addr);
/**
 * @brief A binary NMEA report carried by a DMR UDT (TS 102 361-4 Annex B.3.6, Tables B.6/B.7; @p type 1 short, 2 long),
 * addressed by @p addr (the UDT header's source, destination and I/G bit). @p bit_count is the user data the UDT
 * carried, its pad nibbles left out: a report shorter than its format's fields (NMEA_UDT_SHORT_BITS,
 * NMEA_UDT_LONG_BITS) is truncated and gives nothing. As lip_pdu_decoder(): inside the UDT's event_crc_invalid scope,
 * an admitted report staged for the slot's next data notice, its text in dsd_state::dmr_embedded_gps.
 */
void nmea_iec_61162_1(const dsd_opts* opts, dsd_state* state, const uint8_t* input, size_t bit_count,
                      const dsd_gps_address* addr, int type);

/** @brief The user data bits a binary UDT NMEA report's fields span: short format through the seconds code, long format
 * through the course over ground (TS 102 361-4 Figures B.23, B.24). */
enum { NMEA_UDT_SHORT_BITS = 80, NMEA_UDT_LONG_BITS = 112 };

/**
 * @brief Decode the ASCII NMEA 0183 sentences of a data payload (P25 SAP 48, NXDN data call format 0x06).
 *
 * Each sentence is checked against its own checksum; only checked sentences are printed and kept as the history text.
 * The first RMC with a measured fix (else the first such GGA) is the packet's position report (DSD_POS_NMEA).
 *
 * @param input The payload, one bit per byte, MSB first; it starts at the first sentence.
 * @param slot The slot whose history the notice goes to.
 * @param len_bytes Octets of payload (no CRC).
 * @param packet_verified 1 when the packet's own check passed: then the position and the packet's source and
 *        destination (the slot's dmr_lrrp_source / dmr_lrrp_target, qualified by dmr_data_checked_group) may be
 *        published; otherwise no position and no sentence text is kept.
 * @param origin The reception the packet's first fragment was heard on (a multi-fragment packet), or NULL.
 * @return 1 when at least one sentence passed its checksum, else 0.
 */
uint8_t nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot, int len_bytes,
                              int packet_verified, const struct dsd_position_origin* origin);
/**
 * @brief Whether a data payload is framed as NMEA 0183 the way nmea_sentence_checker() reads it: after any line ends
 * (CR, LF), its first octet starts a sentence ('$' or '!'). Such a payload is NMEA whether or not its sentences pass
 * their checksums, so none of it is ever kept as plain text.
 *
 * @param input The payload, one bit per byte, MSB first.
 * @param len_bytes Octets of payload.
 */
int nmea_payload_framed(const uint8_t* input, int len_bytes);
/** The bits an NXDN GPS report (nxdn_gps_report()) reads after its 0xFFFC marker: through the UTC seconds. */
#define DSD_NXDN_GPS_REPORT_BITS 264U
/**
 * @brief Decode an NXDN GPS report (vendor format, one non-official source: published FORMAT_PROVISIONAL).
 *
 * @param input The report after its 0xFFFC marker, one bit per byte.
 * @param input_bits Bits available at @p input; fewer than DSD_NXDN_GPS_REPORT_BITS gives no position.
 * @param src The source of the checked data call header bound to this assembly (0 when none); the record's
 *        destination is that header's (dsd_state::dmr_lrrp_target[0], dmr_data_checked_group[0]).
 * @param origin The reception that header was heard on (the assembly's first fragment), or NULL.
 */
void nxdn_gps_report(const dsd_opts* opts, dsd_state* state, const uint8_t* input, size_t input_bits, uint32_t src,
                     const struct dsd_position_origin* origin);
/**
 * L3Harris GPS update (TIA-102.AABF-D-1 §7.3.35-36), Phase 1 link control or Phase 2 MAC.
 *
 * @param body DSD_HARRIS_GPS_BODY_OCTETS octets: Part A octets 2-8 then Part B octets 2-8 (on Phase 2, the 14 octets
 *        after the `AA A4 11` header).
 * @param ctx Provenance and verdicts; NULL decodes and prints only.
 * @return the dsd_position_admit() result: 0 when nothing was admitted (no fix, invalid fields, unchecked message).
 */
int nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* body, const dsd_gps_report_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_GPS_H_ */
