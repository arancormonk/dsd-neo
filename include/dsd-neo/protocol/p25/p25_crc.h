// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 CRC helper interfaces.
 *
 * Declares P25 CRC helpers implemented in `src/protocol/p25/p25_crc.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CRC_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CRC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t ComputeCrcCCITT16b(const uint8_t buf[], unsigned int len);
int crc16_lb_bridge(const int* payload, int len);
int crc12_xb_bridge(const int* payload, int len);
extern uint8_t lsd_parity[256];

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_CRC_H_ */
