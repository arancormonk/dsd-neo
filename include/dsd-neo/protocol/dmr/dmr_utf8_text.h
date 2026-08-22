// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR text decode helpers shared across modules.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_UTF8_TEXT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_UTF8_TEXT_H_

#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input);

/**
 * @brief Print @p len octets of UTF-16BE text to stderr, with the ASCII subset copied into
 *        the slot's event text when @p wr is 1.
 *
 * Surrogate pairs are rendered as one character and unpaired halves as U+FFFD; a trailing odd
 * octet is ignored.
 */
void utf16_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_UTF8_TEXT_H_ */
