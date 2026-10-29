// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_FILE_WAV_SEGMENT_H_
#define DSD_NEO_SRC_CORE_FILE_WAV_SEGMENT_H_

#include <dsd-neo/platform/sndfile_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A per-call segment opened (open_wav_file()): its writes latch at the first short one (dsd_wav_segment_write()). */
void wav_segment_opened(const SNDFILE* file);

/* 1 when the test seam fails this open (dsd_wav_test_fail_opens_after()). */
int wav_segment_test_refuses_open(void);

/* 1 the first time this decoder run that the segment table is full, the one that is logged (wav_recording.c, reset
   with the run's failure counts). */
int wav_recording_table_full_first_this_run(void);

/* Forget @p file's segment, if it is one, and close it through libsndfile (or the test seam); sf_close()'s result.
   @p failed (may be NULL) gets whether a short write had latched the segment (the journal's `failed` flag). */
int wav_segment_close(SNDFILE* file, int* failed);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_CORE_FILE_WAV_SEGMENT_H_ */
