// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_YSF_YSF_FRAME_H_
#define DSD_NEO_SRC_PROTOCOL_YSF_YSF_FRAME_H_

#include <dsd-neo/core/state_fwd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t dsd_ysf_soft_viterbi_decode(const uint8_t* dibits, size_t dibit_count, size_t decoded_bytes,
                                     size_t offset_bits, size_t output_bits, uint8_t* out_bits, uint8_t* out_bytes);

uint8_t dsd_ysf_pn95_bit(size_t bit_index);
void dsd_ysf_dewhiten_bits(uint8_t* bits, size_t bit_count);

bool dsd_ysf_event_text_should_print(const dsd_state* state);

void dsd_ysf_unpack_full_rate_imbe(const uint8_t imbe_raw[144], uint8_t imbe_vch[144], char imbe_fr[8][23]);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_YSF_YSF_FRAME_H_ */
