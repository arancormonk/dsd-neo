// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief NXDN alias decoding helpers (standard + ARIB segmented variants).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_ALIAS_DECODE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_ALIAS_DECODE_H_

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

struct nxdn_alias_assembly; /* <dsd-neo/core/state.h> */

/**
 * Decode/assemble a Kenwood NXDN alias fragment (message type 0x3F, MFID 0x68, 0x8204) into
 * dsd_state::nxdn_alias_prop. The alias is published (shown, and set on the open call's history row) only when the
 * fragment completes the assembly (nxdn_alias_complete()); a partial, spliced or other-count assembly is never
 * published, and the last complete alias stays shown until another complete one arrives.
 */
void nxdn_alias_decode_prop(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits);

/**
 * The alias text of a complete Kenwood alias assembly: all four fragments stored under one binding (slot, call
 * epoch, call source, tuning generation), cut to its 14 text octets, non-printable octets shown as spaces, trailing
 * spaces trimmed.
 *
 * @return 1 when @p assembly is complete and its text is not empty (written to @p alias, which needs room for 15
 *         octets), else 0 with @p alias empty.
 */
int nxdn_alias_complete(const struct nxdn_alias_assembly* assembly, char* alias, size_t alias_sz);

/**
 * Decode/assemble an ARIB-style segmented alias payload.
 */
void nxdn_alias_decode_arib(const dsd_opts* opts, dsd_state* state, const uint8_t* message_bits);

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

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_ALIAS_DECODE_H_ */
