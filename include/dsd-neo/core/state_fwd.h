// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Forward declaration of core decoder state type.
 * Provides an incomplete dsd_state type for headers that only
 * need pointers/references without pulling in the full dsd.h.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd_state dsd_state;

#ifdef __cplusplus
}
#endif
