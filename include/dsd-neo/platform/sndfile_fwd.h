// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_SNDFILE_FWD_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_SNDFILE_FWD_H_H

/**
 * @file
 * @brief Forward declarations for libsndfile types.
 *
 * Public headers should not require external library headers when only opaque
 * pointers are needed.
 */

#if defined(__has_include)
#if __has_include(<sndfile.h>)
#include <sndfile.h> // IWYU pragma: export
#else
typedef struct sf_private_tag SNDFILE;
typedef struct SF_INFO SF_INFO;
#endif /* __has_include(<sndfile.h>) */
#else  /* !defined(__has_include) */
typedef struct sf_private_tag SNDFILE;
typedef struct SF_INFO SF_INFO;
#endif /* defined(__has_include) */

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_SNDFILE_FWD_H_H */
