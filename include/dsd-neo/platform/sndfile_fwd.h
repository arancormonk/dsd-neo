// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

/**
 * @file
 * @brief Forward declarations for libsndfile types.
 *
 * Public headers should not require external library headers when only opaque
 * pointers are needed.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sf_private_tag SNDFILE;
typedef struct SF_INFO SF_INFO;

#ifdef __cplusplus
}
#endif
