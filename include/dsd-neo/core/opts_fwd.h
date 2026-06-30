// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Forward declaration of core decoder options type (`dsd_opts`).
 *
 * Provides an incomplete type for headers that only need pointers/
 * references.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_OPTS_FWD_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_OPTS_FWD_H_H

#include <dsd-neo/platform/platform.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DSD_ATTR_PACKED { DSD_FRONTEND_NONE = 0, DSD_FRONTEND_TERMINAL = 1 } dsd_frontend_kind;

typedef struct dsd_opts dsd_opts;
/* Opaque TCP audio input context referenced by dsd_opts without requiring IO-layer includes. */
typedef struct tcp_input_ctx tcp_input_ctx;

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_OPTS_FWD_H_H */
