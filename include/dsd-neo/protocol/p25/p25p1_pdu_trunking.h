// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 1 trunking PDU decoder interfaces.
 *
 * Declares the P25p1 trunking PDU decoder entrypoint implemented in the P25
 * protocol library.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P1_PDU_TRUNKING_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P1_PDU_TRUNKING_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Decode a bounded P25 trunking PDU (MPDU) and update decoder state. */
int p25_decode_pdu_trunking(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P1_PDU_TRUNKING_H_ */
