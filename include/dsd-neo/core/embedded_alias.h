// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Embedded alias decoder interfaces.
 *
 * Declares embedded alias helpers implemented in `src/core/util/dsd_alias.c`
 * as a standalone API.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_EMBEDDED_ALIAS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_EMBEDDED_ALIAS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dmr_talker_alias_lc_header(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void dmr_talker_alias_lc_blocks(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t block_num,
                                const uint8_t* lc_bits);
void dmr_talker_alias_lc_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t block_num, uint8_t char_size,
                                uint16_t max_chars);

void apx_embedded_alias_test_phase1(dsd_opts* opts, dsd_state* state);
void apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void apx_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t num_bits, uint8_t* input);
void apx_embedded_alias_dump(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint16_t num_bytes,
                             const uint8_t* input, const uint8_t* decoded);

void l3h_embedded_alias_blocks_phase1(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
void l3h_embedded_alias_decode(const dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, const uint8_t* input);
void tait_iso7_embedded_alias_decode(const dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len,
                                     const uint8_t* input);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_EMBEDDED_ALIAS_H_ */
