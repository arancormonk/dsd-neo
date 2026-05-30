// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief dPMR data helper interfaces.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_DATA_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_DATA_H_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t GetdPmrColorCode(uint8_t ChannelCodeBit[24]);
void dpmr_scrambled_pmr_bits(uint32_t* lfsr_value, const uint8_t* input, uint8_t* output, uint32_t bit_count);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_DATA_H_H */
