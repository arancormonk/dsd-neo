// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_SNAPSHOT_INTERNAL_H_
#define DSD_NEO_SRC_APP_CONTROL_SNAPSHOT_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef DSD_NEO_TEST_HOOKS
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

const dsd_state* dsd_app_get_latest_snapshot(void);
const dsd_opts* dsd_app_get_latest_opts_snapshot(void);

void dsd_app_install_telemetry_hooks(void);
void dsd_app_telemetry_publish_snapshot(const dsd_state* state);
void dsd_app_telemetry_publish_opts_snapshot(const dsd_opts* opts);

void dsd_app_request_redraw(void);

#ifdef DSD_NEO_TEST_HOOKS
typedef struct {
    uint64_t source_to_published[2];
    uint64_t published_to_consumer[2];
} dsd_app_snapshot_event_history_copy_counts;

void dsd_app_snapshot_test_reset_event_history_copy_counts(void);
void dsd_app_snapshot_test_get_event_history_copy_counts(dsd_app_snapshot_event_history_copy_counts* counts);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_SNAPSHOT_INTERNAL_H_ */
