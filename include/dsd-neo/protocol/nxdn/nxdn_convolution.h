// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief NXDN convolutional codec helpers.
 *
 * Declares the convolutional encoder/decoder routines implemented in
 * `src/protocol/nxdn/nxdn_convolution.c`. Several protocols reuse these
 * helpers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_CONVOLUTION_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_CONVOLUTION_H_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void CNXDNConvolution_start(void);
void CNXDNConvolution_decode(uint8_t s0, uint8_t s1);
void CNXDNConvolution_decode_soft(uint8_t s0, uint8_t s1, uint8_t r0, uint8_t r1);
void CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits);
void CNXDNConvolution_encode(const unsigned char* in, unsigned char* out, unsigned int nBits);
void CNXDNConvolution_init(void);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_CONVOLUTION_H_H */
