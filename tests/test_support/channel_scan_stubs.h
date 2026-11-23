// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#ifndef DSD_NEO_TEST_CHANNEL_SCAN_STUBS_H
#define DSD_NEO_TEST_CHANNEL_SCAN_STUBS_H

#include <dsd-neo/core/power.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the last stubbed dsd_engine_channel_scan_leave() found as it began (issue #644 B-3): the -Y flag
   (dsd_opts::scanner_mode) and whether row keys were in force (dsd_state::scan_keys_active_set). -1 for each before any
   leave. */
void dsd_test_channel_scan_leave_seen(int* scanner_mode, int* row_keys);

/* The gate writes marked due since the last call (dsd_engine_receiver_gate_due(), issue #899: a stream open), with the
   gate the last one resolved to as it was marked (dsd_scan_mode_channel_squelch(), on the @p opts and @p state given to
   the call before) in @p out (may be NULL). The options and state are kept for the marks that follow. */
int dsd_test_channel_scan_gates_due(const dsd_opts* opts, const dsd_state* state, dsd_squelch_setting* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TEST_CHANNEL_SCAN_STUBS_H */
