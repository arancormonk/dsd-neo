// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief NXDN protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nxdn_frame(dsd_opts* opts, dsd_state* state);

/* Cipher-classification hysteresis for the NXDN cipher field (values 0..3).
 * Backed by dsd_state.nxdn_cipher_class*; see src/protocol/nxdn/nxdn_enc_class.c. */
uint8_t nxdn_cipher_observe(dsd_state* state, uint8_t cipher, int strong);
void nxdn_cipher_force(dsd_state* state, uint8_t cipher);
void nxdn_cipher_class_reset(dsd_state* state);

/**
 * @brief Forget the CRC evidence gathered for the current NXDN transmission.
 *
 * Defined in src/protocol/nxdn/nxdn_confirm.c and declared here so the engine can clear it
 * with the carrier; the rest of that module's interface is private to the protocol.
 */
void nxdn_confirm_reset(dsd_state* state);
int nxdn_cipher_established_enc(const dsd_state* state);
int nxdn_cipher_established_clear(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_H_ */
