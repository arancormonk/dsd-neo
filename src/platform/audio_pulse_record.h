// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A PulseAudio record stream whose queued audio can be read without waiting (issue #834).
 *
 * pa_simple's design on the threaded mainloop API: a context and a record stream connected as pa_simple_new() connects
 * them (but for its timing flags, which serve its latency query only), and a blocking read that waits on the mainloop
 * for audio as pa_simple_read() does. pa_simple hides its stream, so it cannot say what the client holds; this keeps
 * the stream in reach, and pulse_record_read_held() takes only what the client holds now and never waits. An input
 * discard reads through it, so a source that stops delivering (one suspended during a retune) cannot hold the decoder.
 */

#ifndef DSD_NEO_SRC_PLATFORM_AUDIO_PULSE_RECORD_H
#define DSD_NEO_SRC_PLATFORM_AUDIO_PULSE_RECORD_H

#include <pulse/def.h>
#include <pulse/sample.h>
#include <stddef.h>
#include <stdint.h>

struct pulse_record;

/** Open a record stream named "Audio Input" for @p app on source @p dev (NULL: the default), with @p spec and @p attr,
 * connected as pa_simple_new() connects one, with its latency flag (PA_STREAM_ADJUST_LATENCY). NULL on failure, with
 * the PulseAudio error code in @p err. */
struct pulse_record* pulse_record_open(const char* app, const char* dev, const pa_sample_spec* spec,
                                       const pa_buffer_attr* attr, int* err);

/** Read exactly @p bytes, waiting for the source as long as it takes, as pa_simple_read() does. 0, or -1 with the
 * PulseAudio error code in @p err (the context or the stream failed, or ended). */
int pulse_record_read(struct pulse_record* rec, void* data, size_t bytes, int* err);

/** Read up to @p bytes of what the client holds now, never waiting: the bytes read (0 when it holds none), or -1
 * with the PulseAudio error code in @p err. @p bytes and what it returns are whole frames: the stream hands its audio
 * over in whole frames. */
long pulse_record_read_held(struct pulse_record* rec, void* data, size_t bytes, int* err);

/** How much input the stream has taken in so far, in bytes (issue #834): what the reads took from the client and what
 * the client holds now. Takes the mainloop lock: any thread but the mainloop's own. */
uint64_t pulse_record_arrived(struct pulse_record* rec);

/** Drop what the client holds before byte @p position of its count (pulse_record_arrived()), never waiting for the
 * source, until @p deadline_ns on the monotonic clock (issue #834). What it does not get to, the next read drops before
 * it hands anything over. Returns the bytes of audio dropped, or -1 with the PulseAudio error code in @p err. */
long pulse_record_discard_through(struct pulse_record* rec, uint64_t position, uint64_t deadline_ns, int* err);

/** Close the stream and free it (NULL is a no-op). */
void pulse_record_free(struct pulse_record* rec);

#endif /* DSD_NEO_SRC_PLATFORM_AUDIO_PULSE_RECORD_H */
