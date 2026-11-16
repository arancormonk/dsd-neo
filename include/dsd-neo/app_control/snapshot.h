// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-facing access to the published decoder snapshots.
 *
 * The decode threads publish deep copies of `dsd_state`/`dsd_opts` through the
 * telemetry hooks installed by @ref dsd_app_frontend_runtime_start. Frontends read
 * those copies here instead of touching the live objects.
 *
 * Threading contract: both accessors are **single-consumer**. They return a pointer
 * to a shared consume buffer after releasing the internal lock, so exactly one
 * thread in the process may call them; a second concurrent reader observes a buffer
 * mid-overwrite. Frontends must funnel every snapshot read through one polling
 * thread (the terminal UI uses its async thread; a GUI uses its main/UI thread).
 * A thread that must read the history rings besides that one copies them through
 * <dsd-neo/app_control/history_tap.h> instead.
 *
 * Lifetime: the returned pointers stay valid until the next call from the same
 * consumer thread. Both return NULL until the first publish.
 *
 * Staleness: stopping the frontend runtime clears the hooks but not the published
 * snapshots — the last values keep being returned, all but their key material,
 * which the stop wipes. Never infer running/stopped state from snapshot contents.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Latest published decoder state snapshot.
 * @return Read-only snapshot, or NULL before the first publish.
 */
const dsd_state* dsd_app_get_latest_snapshot(void);

/**
 * @brief Wipe the key material of the buffer dsd_app_get_latest_snapshot() returns (DSD_SECURE_ZERO), as the stop
 * wipes the published copy's. Called by the reading thread once it reads no more (the terminal UI thread as it exits):
 * the stop leaves that buffer to the thread's next read, which never comes. A later read copies the published state
 * again.
 */
void dsd_app_snapshot_consumer_wipe_keys(void);

/**
 * @brief Latest published options snapshot.
 * @return Read-only snapshot, or NULL before the first publish.
 */
const dsd_opts* dsd_app_get_latest_opts_snapshot(void);

/**
 * @brief Wipe the API keys (rdio_api_key, rr_app_key, api_token) of the buffer dsd_app_get_latest_opts_snapshot()
 * returns (DSD_SECURE_ZERO), as the stop wipes the published copy's. Called by the reading thread once it reads no
 * more, as dsd_app_snapshot_consumer_wipe_keys() is. A later read copies the published options again.
 */
void dsd_app_opts_snapshot_consumer_wipe_keys(void);
/** Read configured decoder settings from matching frontend snapshots. The state snapshot also
 * supports dsd_scan_mode_active() and dsd_scan_mode_effective_profile(), including combined P25. */
void dsd_app_snapshot_configured_mode(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_ */
