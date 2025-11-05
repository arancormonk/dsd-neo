// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <dsd-neo/core/dsd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Publish a snapshot of dsd_opts for UI thread consumption. */
void ui_publish_opts_snapshot(const dsd_opts* opts);

/* Get the latest published options snapshot. Returns NULL if none yet. */
const dsd_opts* ui_get_latest_opts_snapshot(void);

#ifdef __cplusplus
}
#endif
