// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Demod → UI snapshot API. Provides a stable read-only view of state for
 * the UI thread. Initial implementation memcpy's full dsd_state for
 * simplicity; can be narrowed later.
 */

#pragma once

#include <dsd-neo/core/dsd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Publish a snapshot of the current demod state for the UI to render. */
void ui_publish_snapshot(const dsd_state* state);

/* Obtain the latest snapshot for drawing. Returns NULL if none published yet.
 * The returned pointer remains valid until the next call to this function
 * from the same thread; do not free.
 */
const dsd_state* ui_get_latest_snapshot(void);

#ifdef __cplusplus
}
#endif
