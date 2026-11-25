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

/** @brief Map a serialized VD type-2 bit index in [0, 103] to its deinterleaved position. */
uint8_t dsd_ysf_vd2_interleave_index(size_t serial_bit_index);
uint8_t dsd_ysf_pn95_bit(size_t bit_index);
void dsd_ysf_dewhiten_bits(uint8_t* bits, size_t bit_count);

bool dsd_ysf_event_text_should_print(const dsd_state* state);

/**
 * @brief Format a FICH's SQL code text ("SQL CODE: nnn ") into @p out.
 *
 * The FICH decode verdict @p fich_err says whether the SQL fields were read: a failed FICH (nonzero) gives no text.
 * Every 7-bit code is a real value, so none of them means "unset". The text is given while @p sql_type is set.
 *
 * @return The text length, or 0 when there is no text (or it does not fit); @p out is then empty when it has room.
 */
size_t dsd_ysf_format_fich_sql(int fich_err, uint8_t sql_type, uint8_t sql_code, char* out, size_t out_size);

void dsd_ysf_unpack_full_rate_imbe(const uint8_t imbe_raw[144], uint8_t imbe_vch[144], char imbe_fr[8][23]);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_YSF_YSF_FRAME_H_ */
