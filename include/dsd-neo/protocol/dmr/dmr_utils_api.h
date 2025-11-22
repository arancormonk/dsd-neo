// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief Minimal DMR utility CRC/bit helper API used by tests.
 */

/** @brief Pack a bit buffer into bytes (little-endian bits). */
uint64_t ConvertBitIntoBytes(uint8_t* BufferIn, uint32_t BitLength);

/** @brief Compute CCITT CRC-16 over DMR data buffer. */
uint16_t ComputeCrcCCITT(uint8_t* DMRData);
/** @brief Compute 9-bit CRC used by certain DMR payloads. */
uint16_t ComputeCrc9Bit(uint8_t* DMRData, uint32_t NbData);

/** @brief Compute 3-bit CRC for provided bit array. */
uint8_t crc3(uint8_t bits[], unsigned int len);
/** @brief Compute 4-bit CRC for provided bit array. */
uint8_t crc4(uint8_t bits[], unsigned int len);
/** @brief Compute 7-bit CRC for provided bit array. */
uint8_t crc7(uint8_t bits[], unsigned int len);
/** @brief Compute 8-bit CRC for provided bit array. */
uint8_t crc8(uint8_t bits[], unsigned int len);

#ifdef __cplusplus
}
#endif
