// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Compatibility wrapper for app-control state snapshots.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_UI_SNAPSHOT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_UI_UI_SNAPSHOT_H_

#include "dsd-neo/app_control/snapshot.h" // IWYU pragma: export
#include "dsd-neo/core/state_fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

const dsd_state* ui_get_latest_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_UI_SNAPSHOT_H_ */
