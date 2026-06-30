// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Decoder → frontend snapshot API for stable read-only state.
 *
 * Publishes `dsd_state`-shaped snapshots of the fields rendered by the UI
 * thread, without racing live decoder state. Decoder-private bulk buffers are
 * intentionally omitted from these snapshots.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_

#include <dsd-neo/core/opts_fwd.h>
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
const dsd_state* dsd_app_get_latest_snapshot(void);

/**
 * @brief Get the latest published options snapshot.
 * @return Pointer to stable copy, or NULL if none published yet.
 */
const dsd_opts* dsd_app_get_latest_opts_snapshot(void);

/**
 * @brief Install app-control snapshot/redraw telemetry hooks into runtime.
 */
void dsd_app_install_telemetry_hooks(void);

/**
 * @brief Publish a state snapshot through the app-control telemetry sink.
 */
void dsd_app_telemetry_publish_snapshot(const dsd_state* state);

/**
 * @brief Publish an options snapshot through the app-control telemetry sink.
 */
void dsd_app_telemetry_publish_opts_snapshot(const dsd_opts* opts);

/**
 * @brief Mark frontends dirty so they redraw on their next frame.
 */
void dsd_app_request_redraw(void);

/**
 * @brief Consume and clear the app-control redraw request flag.
 * @return Non-zero if a redraw had been requested.
 */
int dsd_app_consume_redraw_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_ */
