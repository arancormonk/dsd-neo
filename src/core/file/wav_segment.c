// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/log.h>
#include <sndfile.h>
#include <stddef.h>
#include "wav_segment.h"
#include "wav_segment_test_support.h"

/* The libsndfile calls the per-call recordings make, replaced by a test that fails a write or a close (issue #708). */
static dsd_wav_test_write_fn s_wav_test_write;
static dsd_wav_test_close_fn s_wav_test_close;

void
dsd_wav_test_set_io(dsd_wav_test_write_fn write_fn, dsd_wav_test_close_fn close_fn) {
    s_wav_test_write = write_fn;
    s_wav_test_close = close_fn;
}

/* Opens left before the test seam fails every open after them; -1 for none. */
static long s_wav_test_opens_left = -1;

void
dsd_wav_test_fail_opens_after(long successful_opens) {
    s_wav_test_opens_left = successful_opens;
}

int
wav_segment_test_refuses_open(void) {
    if (s_wav_test_opens_left < 0) {
        return 0;
    }
    if (s_wav_test_opens_left == 0) {
        return 1;
    }
    s_wav_test_opens_left--;
    return 0;
}

static sf_count_t
wav_io_write(SNDFILE* file, const short* samples, sf_count_t count) {
    if (s_wav_test_write) {
        return (sf_count_t)s_wav_test_write(file, samples, (long long)count);
    }
    return sf_write_short(file, samples, count);
}

/* The open per-call segments (open_wav_file() to its close), each with whether a write to it fell short (issue #708):
   the first short write latches the segment, and nothing more is written to it, so a disk that filled mid-call leaves
   a file that ends where the disk did instead of one with holes, which libsndfile finalizes at the close as usual. The
   decoder and the watchdog both open and close segments, so the table is locked; the write path only looks when some
   segment is latched. A handle the table does not hold (the static and raw WAVs) is written as before. */
enum { WAV_SEGMENT_SLOTS = 16 };

typedef struct {
    const SNDFILE* file;
    int failed;
} wav_segment;

static wav_segment s_wav_segments[WAV_SEGMENT_SLOTS];
static atomic_int s_wav_segments_failed;
static dsd_mutex_t s_wav_segment_mu;
/* 0 = uninitialized, 1 = initialization in flight, 2 = ready. */
static atomic_int s_wav_segment_mu_state;

static void
wav_segment_lock(void) {
    if (atomic_load(&s_wav_segment_mu_state) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&s_wav_segment_mu_state, &expected, 1)) {
            (void)dsd_mutex_init(&s_wav_segment_mu);
            atomic_store(&s_wav_segment_mu_state, 2);
        } else {
            while (atomic_load(&s_wav_segment_mu_state) != 2) {
                dsd_thread_yield();
            }
        }
    }
    dsd_mutex_lock(&s_wav_segment_mu);
}

static wav_segment*
wav_segment_find_locked(const SNDFILE* file) {
    for (int i = 0; i < WAV_SEGMENT_SLOTS; i++) {
        if (s_wav_segments[i].file == file) {
            return &s_wav_segments[i];
        }
    }
    return NULL;
}

static void
wav_segment_forget_locked(wav_segment* segment) {
    if (segment->failed) {
        (void)atomic_fetch_add(&s_wav_segments_failed, -1);
    }
    segment->file = NULL;
    segment->failed = 0;
}

void
wav_segment_opened(const SNDFILE* file) {
    if (file == NULL) {
        return;
    }
    wav_segment_lock();
    wav_segment* segment = wav_segment_find_locked(file);
    if (segment == NULL) {
        segment = wav_segment_find_locked(NULL);
    }
    if (segment != NULL) {
        wav_segment_forget_locked(segment);
        segment->file = file;
    }
    dsd_mutex_unlock(&s_wav_segment_mu);
    /* More segments open at once than the decoder ever holds (two slots, an NFM call) means closes are being missed;
       such a segment is written as a static WAV is, without the latch. */
    if (segment == NULL && wav_recording_table_full_first_this_run()) {
        LOG_WARN("Call recording: more than %d recordings open at once; a short write to the extra ones is logged, not "
                 "latched\n",
                 WAV_SEGMENT_SLOTS);
    }
}

int
wav_segment_close(SNDFILE* file, int* failed) {
    if (failed) {
        *failed = 0;
    }
    if (file == NULL) {
        return 0;
    }
    wav_segment_lock();
    wav_segment* segment = wav_segment_find_locked(file);
    if (segment != NULL) {
        if (failed) {
            *failed = segment->failed;
        }
        wav_segment_forget_locked(segment);
    }
    dsd_mutex_unlock(&s_wav_segment_mu);
    if (s_wav_test_close) {
        return s_wav_test_close(file);
    }
    return sf_close(file);
}

/* -1 for a handle that is no per-call segment, else whether the segment is latched. */
static int
wav_segment_failed(const SNDFILE* file) {
    wav_segment_lock();
    const wav_segment* segment = wav_segment_find_locked(file);
    const int failed = segment ? segment->failed : -1;
    dsd_mutex_unlock(&s_wav_segment_mu);
    return failed;
}

/* Latch @p file's segment failed: 1 when it was not latched before, 0 when it was, -1 for a handle that is no per-call
   segment. */
static int
wav_segment_latch(const SNDFILE* file) {
    wav_segment_lock();
    wav_segment* segment = wav_segment_find_locked(file);
    int latched = -1;
    if (segment != NULL) {
        latched = !segment->failed;
        if (latched) {
            segment->failed = 1;
            (void)atomic_fetch_add(&s_wav_segments_failed, 1);
        }
    }
    dsd_mutex_unlock(&s_wav_segment_mu);
    return latched;
}

void
dsd_wav_segment_write(SNDFILE* file, const short* samples, size_t count, const char* context) {
    if (file == NULL || samples == NULL || count == 0) {
        return;
    }
    if (atomic_load(&s_wav_segments_failed) > 0 && wav_segment_failed(file) == 1) {
        return;
    }
    const sf_count_t want = (sf_count_t)count;
    const sf_count_t written = wav_io_write(file, samples, want);
    if (written == want) {
        return;
    }
    const int latched = wav_segment_latch(file);
    if (latched < 0) {
        LOG_WARN("%s: wrote %lld/%lld samples to WAV output\n", context, (long long)written, (long long)want);
        return;
    }
    /* Once per run of each kind, however many segments a full disk fails; the recording status counts them all. */
    if (latched == 1 && dsd_wav_recording_note_failure(DSD_WAV_RECORDING_ERROR_WRITE)) {
        LOG_WARN("%s: wrote %lld/%lld samples to a call recording; it ends here, and later segments that fail to write "
                 "are counted, not logged\n",
                 context, (long long)written, (long long)want);
    }
}
