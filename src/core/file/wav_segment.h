// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_FILE_WAV_SEGMENT_H_
#define DSD_NEO_SRC_CORE_FILE_WAV_SEGMENT_H_

#include <dsd-neo/platform/sndfile_fwd.h>
#include <stdint.h>

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

/* 1 the first time this decoder run that an NFM recording is not exported to rdio-scanner for want of a talkgroup (no
   frequency, no --rdio-analog-talkgroup; issue #668), the one that is logged; reset with the run's failure counts. */
int wav_recording_analog_rdio_skip_first_this_run(void);

/* 1 the first time this decoder run that a digital voice call the fallback would number is not exported for want of a
   number (no frequency, no --rdio-unknown-talkgroup; issue #888 B3), the one that is logged; reset with the run's
   failure counts. */
int wav_recording_digital_rdio_skip_first_this_run(void);

/* 1 the first time this decoder run that a played file's recording is kept local (issue #888: the file held more than
   one call, named a call ID after its first voice frame, or its stream verdict blocked), the one that is logged; reset
   with the run's failure counts. */
int wav_recording_played_file_rdio_skip_first_this_run(void);

/* Keep open segment @p file local: its close publishes it, never exports it (issue #888: a played SDRTrunk file in which
   the call-state layer opened more than one call). A handle that is no per-call segment is left alone. */
void wav_segment_keep_local(const SNDFILE* file);

/* Whether open segment @p file was kept local (wav_segment_keep_local()); 0 for a handle that is no per-call segment. */
int wav_segment_kept_local(const SNDFILE* file);

/* Discard open segment @p file at its close: removed and journaled as discarded, never published (issue #888: a played
   SDRTrunk file whose call ID, read after its audio was written, turned its recording off). A handle that is no
   per-call segment is left alone. */
void wav_segment_discard_at_close(const SNDFILE* file);

/* Whether open segment @p file is to be discarded at its close (wav_segment_discard_at_close()); 0 for a handle that is
   no per-call segment. */
int wav_segment_discarded(const SNDFILE* file);

/* Forget @p file's segment, if it is one, and close it through libsndfile (or the test seam); sf_close()'s result.
   @p failed (may be NULL) gets whether a short write had latched the segment (the journal's `failed` flag). */
int wav_segment_close(SNDFILE* file, int* failed);

/* What libsndfile says of open segment @p file so far: 1 with its frames and sample rate, else 0 with both 0. */
int wav_segment_info(SNDFILE* file, int64_t* frames, int* rate);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_CORE_FILE_WAV_SEGMENT_H_ */
