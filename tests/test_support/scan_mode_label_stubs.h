// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#ifndef DSD_NEO_TEST_SCAN_MODE_LABEL_STUBS_H
#define DSD_NEO_TEST_SCAN_MODE_LABEL_STUBS_H
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>
#include <stdint.h>

void dsd_test_tg_avoids(size_t count, uint64_t context);
void dsd_test_scan_labels_set(int available, dsd_scan_mode mode);
void dsd_test_scan_labels_configured(const dsd_scan_settings* settings);
/* The row options dsd_scan_mode_row_options() hands the snapshot (NULL: none installed). */
void dsd_test_scan_labels_row_options(const dsd_scan_option_values* values);

#endif
