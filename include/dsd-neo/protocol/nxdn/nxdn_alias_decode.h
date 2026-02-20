// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief NXDN alias decoding helpers (standard + ARIB segmented variants).
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reset NXDN alias assembly state.
 */
void nxdn_alias_reset(dsd_state* state);

/**
 * Decode/assemble a standard NXDN alias block (message type 0x3F).
 */
void nxdn_alias_decode_prop(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok);

/**
 * Decode/assemble an ARIB-style segmented alias payload.
 */
void nxdn_alias_decode_arib(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok);

/**
 * Return whether full Shift-JIS multibyte decoding support is available.
 *
 * When unavailable, decoding falls back to ASCII + half-width katakana.
 */
int nxdn_alias_shift_jis_full_available(void);

/**
 * Best-effort Shift-JIS style text decode to UTF-8.
 *
 * Uses full Shift-JIS conversion when available at build time. Otherwise,
 * supports ASCII and half-width katakana directly and replaces unsupported
 * multibyte codepoints with U+FFFD.
 *
 * @return Number of bytes written to @p out (excluding NUL terminator).
 */
size_t nxdn_alias_decode_shift_jis_like(const uint8_t* in, size_t in_len, char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif
