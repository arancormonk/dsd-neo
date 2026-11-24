// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_FILE_SDRTRUNK_TEST_SUPPORT_H_
#define DSD_NEO_SRC_CORE_FILE_SDRTRUNK_TEST_SUPPORT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test seam (issue #888); not part of any public header. Called each time a played SDRTrunk file's shape check looks
   at the exit flag, just before it does, with @p offset, how many bytes of the file it has passed; a test raises Stop
   from it. NULL (the default) for none. */
typedef void (*dsd_sdrtrunk_test_poll_fn)(size_t offset);
void dsd_sdrtrunk_test_set_shape_poll_hook(dsd_sdrtrunk_test_poll_fn hook);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_CORE_FILE_SDRTRUNK_TEST_SUPPORT_H_ */
