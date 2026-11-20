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
 * @brief utf8_to_text() on a line that opens with @p label instead of "UTF8 Text": the label of a service whose text
 *        may hold a position (the P25 location service), so the diagnostics log can recognise the line (issue #800).
 */
void utf8_to_text_labelled(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input, const char* label);

/**
 * @brief Print @p len octets of UTF-16BE text (ETSI TS 102 361-3 text messaging) to stderr, with
 *        the printable characters copied as UTF-8 into the slot's staged event text when @p wr is 1.
 *
 * Surrogate pairs are rendered as one character and unpaired halves as U+FFFD; a trailing odd
 * octet is ignored. Control characters print as markers and stay out of the event text.
 */
void utf16_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input);

/**
 * @brief The same for little-endian units, the UCS-2 LE that Motorola TMS carries (MOTOTRBO Text
 *        Messaging ADK Guide: "The MOTOTRBO radio only supports the UCS2-LE encoding schema").
 */
void utf16le_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_UTF8_TEXT_H_ */
