// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal API exposure for DMR utility CRC/bit helpers used by tests.

uint64_t ConvertBitIntoBytes(uint8_t* BufferIn, uint32_t BitLength);

uint16_t ComputeCrcCCITT(uint8_t* DMRData);
uint16_t ComputeCrc9Bit(uint8_t* DMRData, uint32_t NbData);

uint8_t crc3(uint8_t bits[], unsigned int len);
uint8_t crc4(uint8_t bits[], unsigned int len);
uint8_t crc7(uint8_t bits[], unsigned int len);
uint8_t crc8(uint8_t bits[], unsigned int len);

#ifdef __cplusplus
}
#endif
