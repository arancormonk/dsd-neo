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

struct dsd_position_origin;

void dmr_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]);
void apx_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]);
void lip_protocol_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* input);
void lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count, uint32_t src);
void nmea_iec_61162_1(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int type);
/**
 * @brief Decode the ASCII NMEA 0183 sentences of a data payload (P25 SAP 48, NXDN data call format 0x06).
 *
 * Each sentence is checked against its own checksum; only checked sentences are printed and kept as the history text.
 * The first RMC with a measured fix (else the first such GGA) is the packet's position report (DSD_POS_NMEA).
 *
 * @param input The payload, one bit per byte, MSB first; it starts at the first sentence.
 * @param slot The slot whose history the notice goes to.
 * @param len_bytes Octets of payload (no CRC).
 * @param packet_verified 1 when the packet's own check passed: then the position and the packet's source (the slot's
 *        dmr_lrrp_source) may be published; otherwise no position and no sentence text is kept.
 * @param origin The reception the packet's first fragment was heard on (a multi-fragment packet), or NULL.
 * @return 1 when at least one sentence passed its checksum, else 0.
 */
uint8_t nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot, int len_bytes,
                              int packet_verified, const struct dsd_position_origin* origin);
/** The bits an NXDN GPS report (nxdn_gps_report()) reads after its 0xFFFC marker: through the UTC seconds. */
#define DSD_NXDN_GPS_REPORT_BITS 264U
/**
 * @brief Decode an NXDN GPS report (vendor format, one non-official source: published FORMAT_PROVISIONAL).
 *
 * @param input The report after its 0xFFFC marker, one bit per byte.
 * @param input_bits Bits available at @p input; fewer than DSD_NXDN_GPS_REPORT_BITS gives no position.
 * @param src The source of the checked data call header bound to this assembly (0 when none).
 * @param origin The reception that header was heard on (the assembly's first fragment), or NULL.
 */
void nxdn_gps_report(const dsd_opts* opts, dsd_state* state, const uint8_t* input, size_t input_bits, uint32_t src,
                     const struct dsd_position_origin* origin);
void nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int slot);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_GPS_H_ */
