// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_UTIL_KEY_SET_INTERNAL_H_
#define DSD_NEO_SRC_CORE_UTIL_KEY_SET_INTERNAL_H_

#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

/* Compiler-resistant wipe of the inline key material held by a state. */
void dsd_key_state_secure_wipe(dsd_state* state);

/* Like the public direct loader, additionally returning the validated hex width (zero
 * when absent). Both outputs are unchanged on failure; hex_digits may be NULL. */
dsd_key_direct_result dsd_key_set_load_direct_width(dsd_key_set* out, const char* single_hex, const char* single_dec,
                                                    size_t* hex_digits);

/*
 * Store already-parsed direct `-H` segments (10, 32 or 64 hex digits, big-endian 64-bit
 * words, unused words zero) into the scalar block: H/K1..K4, the Hytera segment count and,
 * for multi-segment keys, the AES slots. The single owner of that mapping; the CSV column
 * parser and the scoped-options materializer both route through it. Ignores other widths.
 */
void dsd_key_scalars_store_direct_hex(dsd_key_scalars* scalars, const uint64_t segments[4], size_t nhex);

#endif /* DSD_NEO_SRC_CORE_UTIL_KEY_SET_INTERNAL_H_ */
