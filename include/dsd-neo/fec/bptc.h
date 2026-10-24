// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief BPTC (block product turbo code) helpers for DMR payload extraction.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_FEC_BPTC_H_
#define DSD_NEO_INCLUDE_DSD_NEO_FEC_BPTC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t BPTCDeInterleavingIndex[196];
extern const uint8_t DeInterleaveReverseChannelBptc[32];
extern const uint8_t DeInterleaveReverseChannelBptcPlacement[32];

void BPTCDeInterleaveDMRData(const uint8_t* Input, uint8_t* Output);
uint32_t BPTC_196x96_Extract_Data(uint8_t InputDeInteleavedData[196], uint8_t DMRDataExtracted[96], uint8_t R[3]);
/*
 * Guaranteed decoding radius of the BPTC(196,96) product code (minimum distance 9): a valid codeword within this many
 * bits of the received word is the unique nearest one. BPTC_196x96_Decode_Codeword() is iterative, so a codeword it
 * reports further away may be wrong.
 */
#define DSD_BPTC_196x96_UNIQUE_RADIUS 4U

/* Full decode to a valid codeword: payload, R and the distance to the input; 0 when none is reached. */
int BPTC_196x96_Decode_Codeword(const uint8_t* input196, uint8_t* out96, uint8_t* r3, uint32_t* distance);
uint32_t BPTC_128x77_Extract_Data(uint8_t InputDataMatrix[8][16], uint8_t DMRDataExtracted[77]);
uint32_t BPTC_16x2_Extract_Data(uint8_t InputInterleavedData[32], uint8_t DMRDataExtracted[32],
                                uint32_t ParityCheckTypeOdd);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_FEC_BPTC_H_ */
