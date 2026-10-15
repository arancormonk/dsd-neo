// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Test probes for trunk_scan_document.cpp (module-private).
 *
 * Counts of the work dsd_app_trunk_scan_validate_bundle() does on the files a targets list names, so a test can show
 * that each referenced file is checked once however many rows name it. Counts only: no path or content crosses here.
 * Built only with DSD_NEO_TEST_HOOKS: a test compiles trunk_scan_document.cpp into itself with that define, and
 * production builds carry neither the probes nor their counting.
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_TRUNK_SCAN_DOCUMENT_INTERNAL_H_
#define DSD_NEO_SRC_APP_CONTROL_TRUNK_SCAN_DOCUMENT_INTERNAL_H_

#ifdef DSD_NEO_TEST_HOOKS
#ifdef __cplusplus
extern "C" {
#endif

/** Zero both counts. */
void dsd_app_trunk_scan_test_reset_counts(void);

/** Referenced channel maps inspected (parsed for their own companions) since the last reset. */
unsigned dsd_app_trunk_scan_test_channel_inspections(void);

/** Referenced files dry-run validated (a channel map, or one of its companions) since the last reset. */
unsigned dsd_app_trunk_scan_test_file_validations(void);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_TEST_HOOKS */

#endif /* DSD_NEO_SRC_APP_CONTROL_TRUNK_SCAN_DOCUMENT_INTERNAL_H_ */
