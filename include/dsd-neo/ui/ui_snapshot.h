// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Demod → UI snapshot API for stable read-only state.
 *
 * Publishes `dsd_state`-shaped snapshots of the fields rendered by the UI
 * thread, without racing live decoder state. Decoder-private bulk buffers are
 * intentionally omitted from these snapshots.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_UI_SNAPSHOT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_UI_UI_SNAPSHOT_H_

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Obtain the latest snapshot for drawing.
 *
 * The returned pointer remains valid until the next call from the same
 * thread. Returns NULL if no snapshot has been published yet.
 */
const dsd_state* ui_get_latest_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_UI_SNAPSHOT_H_ */
