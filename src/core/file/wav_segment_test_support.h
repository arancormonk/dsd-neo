// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_FILE_WAV_SEGMENT_TEST_SUPPORT_H_
#define DSD_NEO_SRC_CORE_FILE_WAV_SEGMENT_TEST_SUPPORT_H_

#include <dsd-neo/platform/sndfile_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test seams for the per-call recordings (issue #708); not part of any public header. */

/* Replace the sf_write_short() and sf_close() the per-call recordings make; NULL restores libsndfile's. */
typedef long long (*dsd_wav_test_write_fn)(SNDFILE* file, const short* samples, long long count);
typedef int (*dsd_wav_test_close_fn)(SNDFILE* file);
void dsd_wav_test_set_io(dsd_wav_test_write_fn write_fn, dsd_wav_test_close_fn close_fn);

/* Let @p successful_opens more per-call segments open, then fail every open_wav_file(); -1 disables. */
void dsd_wav_test_fail_opens_after(long successful_opens);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_CORE_FILE_WAV_SEGMENT_TEST_SUPPORT_H_ */
