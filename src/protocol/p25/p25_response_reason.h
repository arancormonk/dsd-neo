// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_RESPONSE_REASON_H
#define DSD_NEO_SRC_PROTOCOL_P25_P25_RESPONSE_REASON_H

#include <stdint.h>

const char* p25_queued_response_reason(uint8_t code);
const char* p25_deny_response_reason(uint8_t code);

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_RESPONSE_REASON_H */
