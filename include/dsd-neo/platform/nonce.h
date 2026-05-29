// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_NONCE_H
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_NONCE_H

/**
 * @file
 * @brief Non-cryptographic nonce helpers for filenames and protocol stream IDs.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dsd_nonce_fill(void* out, size_t size);
uint16_t dsd_nonce_u16(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_NONCE_H */
