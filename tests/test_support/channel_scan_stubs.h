// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#ifndef DSD_NEO_TEST_CHANNEL_SCAN_STUBS_H
#define DSD_NEO_TEST_CHANNEL_SCAN_STUBS_H

#ifdef __cplusplus
extern "C" {
#endif

/* What the last stubbed dsd_engine_channel_scan_leave() found as it began (issue #644 B-3): the -Y flag
   (dsd_opts::scanner_mode) and whether row keys were in force (dsd_state::scan_keys_active_set). -1 for each before any
   leave. */
void dsd_test_channel_scan_leave_seen(int* scanner_mode, int* row_keys);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TEST_CHANNEL_SCAN_STUBS_H */
