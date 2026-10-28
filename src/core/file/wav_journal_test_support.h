// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_FILE_WAV_JOURNAL_TEST_SUPPORT_H_
#define DSD_NEO_SRC_CORE_FILE_WAV_JOURNAL_TEST_SUPPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Test seam for the recording journal (issue #708); not part of any public header. Let @p successful_writes more
   journal writes through, then fail every one as a full disk does; -1 disables. */
void dsd_wav_journal_test_fail_writes_after(long successful_writes);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_CORE_FILE_WAV_JOURNAL_TEST_SUPPORT_H_ */
