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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dmr_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]);
void apx_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]);
void lip_protocol_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* input);
void nmea_iec_61162_1(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int type);
uint8_t nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot,
                              int len_bytes);
void nxdn_gps_report(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src);
void nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int slot);
void harris_gps(const dsd_opts* opts, dsd_state* state, int slot, const uint8_t* input);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_GPS_H_ */
