// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_FILE_MANAGED_FILE_TEST_SUPPORT_H_
#define DSD_NEO_SRC_CORE_FILE_MANAGED_FILE_TEST_SUPPORT_H_

#include <dsd-neo/core/managed_file.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test seam for the managed-file lock; not part of any public header. @p hook (NULL clears it) runs at each
   dsd_managed_file_step_reached(), inside the lock, on the thread that holds it: a test starts a competing writer
   there and waits until dsd_managed_file_test_waiters() shows it blocked on the lock. */
typedef void (*dsd_managed_file_test_hook)(dsd_managed_file_step step, void* user);
void dsd_managed_file_test_set_hook(dsd_managed_file_test_hook hook, void* user);

/* Threads waiting in dsd_managed_file_lock() right now. */
int dsd_managed_file_test_waiters(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_CORE_FILE_MANAGED_FILE_TEST_SUPPORT_H_ */
