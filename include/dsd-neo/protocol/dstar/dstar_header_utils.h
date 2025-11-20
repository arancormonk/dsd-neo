// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_PROTOCOL_DSTAR_HEADER_UTILS_H
#define DSD_NEO_PROTOCOL_DSTAR_HEADER_UTILS_H

#include <stddef.h>
#include <stdint.h>

// D-STAR radio header parameters (fixed by the spec).
#define DSD_DSTAR_HEADER_INFO_BITS  330
#define DSD_DSTAR_HEADER_CODED_BITS 660
#define DSD_DSTAR_SCRAMBLER_PERIOD  127

void dstar_scramble_header_bits(const int* in, int* out, size_t bit_count);
void dstar_deinterleave_header_bits(const int* in, int* out, size_t bit_count);
size_t dstar_header_viterbi_decode(const int* symbols, size_t symbol_count, int* out_bits, size_t out_capacity);
uint16_t dstar_crc16(const uint8_t* data, size_t len);

#endif // DSD_NEO_PROTOCOL_DSTAR_HEADER_UTILS_H
