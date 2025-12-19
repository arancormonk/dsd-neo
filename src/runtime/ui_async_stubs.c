// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// Weak, no-op stubs for telemetry hooks used by hot decode paths.
// These satisfy unit tests and libraries that do not link the UI module.
// When the terminal UI is linked, strong definitions in
// src/ui/terminal/ui_async.c and ui_snapshot.c override these.

#include <dsd-neo/runtime/telemetry.h>

#ifdef __GNUC__
#define DSD_WEAK __attribute__((weak))
#else
#define DSD_WEAK
#endif

DSD_WEAK void
ui_request_redraw(void) { /* no-op for non-UI builds */ }

DSD_WEAK void
ui_publish_snapshot(const dsd_state* state) {
    (void)state; /* no-op for non-UI builds */
}

DSD_WEAK void
ui_publish_opts_snapshot(const dsd_opts* opts) {
    (void)opts; /* no-op for non-UI builds */
}

DSD_WEAK void
ui_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
}
