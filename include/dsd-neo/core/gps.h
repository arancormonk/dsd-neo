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
 * @brief The DMR embedded GPS LC (TS 102 361-2 Table 7.3). The caller (dmr_flco) runs it only for an unprotected LC whose
 * embedded checksum passed. Its source is the slot's ACTIVE call's when a checked full LC of that call named it
 * (dsd_state::dmr_lc_checked_transmission / dmr_lc_checked_source); otherwise the report is unattributed.
 */
void dmr_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]);
void apx_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]);
/**
 * @brief A DMR USBD LIP Poll Response (TS 102 361-4 Table 6.79): the caller runs it only for an inbound burst (the
 * MS-sourced data sync) and inside the burst's event_crc_invalid scope, whose verdict decides whether the position was
 * checked. The source is an 8-bit hash, never a radio ID. An admitted report is staged for the slot's next data notice
 * and its text left in dsd_state::dmr_embedded_gps; it is never written to the location file (decision D8).
 */
void lip_protocol_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* input);
/**
 * @brief A LIP PDU carried by a DMR UDT or a UDP datagram, from @p src. Callers run it inside the carrier's
 * event_crc_invalid scope (the UDT header and continuation CRCs; the data packet's header, CRC32 and CRC9 checks),
 * whose verdict decides whether the report was checked. An admitted short report is staged for the slot's next data
 * notice (dsd_event_stage_position()), its text left in dsd_state::dmr_embedded_gps and written to the location file.
 */
void lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count, uint32_t src);
/**
 * @brief A binary NMEA report carried by a DMR UDT (TS 102 361-4 Annex B.3.6, Tables B.6/B.7; @p type 1 short, 2 long),
 * from @p src. As lip_pdu_decoder(): inside the UDT's event_crc_invalid scope, an admitted report staged for the slot's
 * next data notice, its text in dsd_state::dmr_embedded_gps.
 */
void nmea_iec_61162_1(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int type);
uint8_t nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot,
                              int len_bytes);
void nxdn_gps_report(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src);
void nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int slot);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_GPS_H_ */
