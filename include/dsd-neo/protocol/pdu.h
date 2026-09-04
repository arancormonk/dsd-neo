// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared protocol PDU decode helpers.
 *
 * Declares generic PDU decoders implemented in `src/protocol/dmr/dmr_pdu.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_PDU_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_PDU_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode an IPv4 packet-data PDU and publish its data notice.
 *
 * @return 1 when the data notice was committed, or 0 when the input was
 *         rejected or no notice could be committed.
 */
int decode_ip_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* input);
void decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_PDU_H_ */
