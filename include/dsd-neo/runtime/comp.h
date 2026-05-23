// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief qsort() comparator helpers.
 *
 * Declares the float comparator implemented in `src/runtime/comp.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_COMP_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_COMP_H_

#ifdef __cplusplus
extern "C" {
#endif

int comp(const void* a, const void* b);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_COMP_H_ */
