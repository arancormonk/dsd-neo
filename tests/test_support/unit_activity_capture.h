// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A recording dsd_unit_activity_push() for tests that compile protocol sources without dsd-neo_core's ring (and its
 * alias lists): each push is kept, nothing is merged, and no name is ever found.
 */

#ifndef DSD_NEO_TESTS_TEST_SUPPORT_UNIT_ACTIVITY_CAPTURE_H_
#define DSD_NEO_TESTS_TEST_SUPPORT_UNIT_ACTIVITY_CAPTURE_H_

#include <dsd-neo/core/unit_activity.h>

/** Forget every push recorded. */
void dsd_test_ua_capture_reset(void);
/** Pushes recorded since the last reset. */
int dsd_test_ua_capture_count(void);
/** The latest push recorded (all zero before the first). */
const dsd_unit_activity* dsd_test_ua_capture_last(void);

#endif /* DSD_NEO_TESTS_TEST_SUPPORT_UNIT_ACTIVITY_CAPTURE_H_ */
