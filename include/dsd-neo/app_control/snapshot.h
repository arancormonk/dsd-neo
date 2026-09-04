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
 *
 * Lifetime: the returned pointers stay valid until the next call from the same
 * consumer thread. Both return NULL until the first publish.
 *
 * Staleness: stopping the frontend runtime clears the hooks but not the published
 * snapshots — the last values keep being returned. Never infer running/stopped
 * state from snapshot contents.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Latest published decoder state snapshot.
 * @return Read-only snapshot, or NULL before the first publish.
 */
const dsd_state* dsd_app_get_latest_snapshot(void);

/**
 * @brief Latest published options snapshot.
 * @return Read-only snapshot, or NULL before the first publish.
 */
const dsd_opts* dsd_app_get_latest_opts_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_ */
