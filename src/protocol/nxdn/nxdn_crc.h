// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_CRC_H_
#define DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_CRC_H_

#include <stddef.h>
#include <stdint.h>

uint32_t nxdn_crc32_bits(const uint8_t* bits, size_t bit_count);

#endif
