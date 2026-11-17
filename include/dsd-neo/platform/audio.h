// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_AUDIO_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_AUDIO_H_H

/**
 * @file
 * @brief Cross-platform audio I/O abstraction for DSD-neo.
 *
 * Provides a unified API for audio input/output that works with
 * PulseAudio (Linux) and PortAudio (cross-platform/Windows).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque audio stream handle */
typedef struct dsd_audio_stream dsd_audio_stream;

/* Audio device information */
typedef struct dsd_audio_device {
    int index;
    char name[512];
    char description[256];
    int is_input;
    int is_output;
    int initialized;
} dsd_audio_device;

/* Audio stream parameters */
typedef struct dsd_audio_params {
    int sample_rate;      /* e.g., 8000, 48000 */
    int channels;         /* 1 (mono) or 2 (stereo) */
    int bits_per_sample;  /* 16 */
    const char* device;   /* Device name/identifier, or NULL for default */
    const char* app_name; /* Application name for audio server (optional) */
    int async_output;     /* nonzero enables low-latency async output with overrun drops */
} dsd_audio_params;

/** Android output device ID while an AAudio output is open; 0 while idle,
 * -1 when the backend does not publish Android routing information. */
int dsd_audio_output_device_id(void);

/**
 * @brief Initialize audio subsystem.
 *
 * Must be called before any audio operations.
 *
 * @return 0 on success, non-zero on failure.
 */
int dsd_audio_init(void);

/**
 * @brief Cleanup audio subsystem.
 *
 * Call at program termination.
 */
void dsd_audio_cleanup(void);

/**
 * @brief Enumerate available audio devices.
 *
 * @param inputs    Array to receive input devices (may be NULL).
 * @param outputs   Array to receive output devices (may be NULL).
 * @param max_count Maximum devices per array (typically 16).
 * @return 0 on success, non-zero on failure.
 */
int dsd_audio_enumerate_devices(dsd_audio_device* inputs, dsd_audio_device* outputs, int max_count);

/**
 * @brief Print available audio devices to stdout.
 *
 * Convenience function for CLI listing.
 *
 * @return 0 on success, non-zero on failure.
 */
int dsd_audio_list_devices(void);

/**
 * @brief Open an audio input stream.
 *
 * @param params    Stream parameters.
 * @return Stream handle, or NULL on failure.
 */
dsd_audio_stream* dsd_audio_open_input(const dsd_audio_params* params);

/**
 * @brief Open an audio output stream.
 *
 * @param params    Stream parameters.
 * @return Stream handle, or NULL on failure.
 */
dsd_audio_stream* dsd_audio_open_output(const dsd_audio_params* params);

/**
 * @brief Read audio samples from input stream.
 *
 * Blocks until requested samples are available.
 *
 * @param stream    Audio stream handle.
 * @param buffer    Buffer to receive samples (int16_t).
 * @param frames    Number of frames to read.
 * @return Number of frames read, or negative on error.
 */
int dsd_audio_read(dsd_audio_stream* stream, int16_t* buffer, size_t frames);

/**
 * @brief Drop the input samples the stream holds now, unread, but for those captured since @p marked_ns (issue #834: a
 * rigctl peer's audio from before its retune; the audio captured since the retune was confirmed is kept).
 *
 * A device captures in real time, so the newest frames it holds, as many as the time since the mark lasts, are the
 * audio captured since then. That time is read after the backend counts what it holds (or, on PulseAudio, after its
 * last read), so audio captured while the caller waited before the count is kept, and the frame count rounds up: a
 * drop keeps more rather than less. Per backend:
 * - PulseAudio (Linux, macOS), a best effort: what the client holds from the server (the rest of the record fragment
 *   being read, the queue behind it) is read until a read waits for audio; that read's frames count as held too. At
 *   most 2 s of frames at the stream's rate, exact to one 32-frame read (the cap is checked before each read, so up to
 *   31 frames past it), no read begun after 250 ms on the monotonic clock, and the last read waits up to one record
 *   block (about 100 ms) on top of that. pa_simple says nothing of what it holds before it is read, so the newest
 *   frames read are kept in the stream and returned first by dsd_audio_read().
 * - PortAudio (Windows): one snapshot of the frames the stream reports available at the call
 *   (Pa_GetStreamReadAvailable(), at most its input buffer), less those kept, read without a wait; no frame or time
 *   cap of its own.
 * - AAudio (Android): the frames the device wrote and no read took (AAudioStream_getFramesWritten() less
 *   AAudioStream_getFramesRead()), less those kept, read with no wait: at most 96000 frames (2 s at 48 kHz), no read
 *   begun after 250 ms.
 * Audio still on its way to DSD-neo (in the producer, or in the sound server) arrives after the call and is kept.
 *
 * @param stream     Input stream handle.
 * @param marked_ns  The retune's mark on the monotonic clock (dsd_time_monotonic_ns(), dsd_input_cut::marked_ns): the
 *                   audio captured since is kept (a mark later than the count keeps nothing).
 * @return Frames dropped, or negative on error (an output stream included).
 */
int dsd_audio_discard_input(const dsd_audio_stream* stream, uint64_t marked_ns);

/**
 * @brief Write audio samples to output stream.
 *
 * May block until buffer space is available.
 *
 * @param stream    Audio stream handle.
 * @param buffer    Samples to write (int16_t).
 * @param frames    Number of frames to write.
 * @return Number of frames written (queued, on an async stream), or negative on error, including while the
 *         stream's device is down and owed a reopen (dsd_audio_output_health_get()).
 */
int dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames);

/** Device health of an output stream (dsd_audio_output_health_get()). */
typedef struct dsd_audio_output_health {
    uint64_t failures;   /* device failures since the stream opened; each left the device unusable, owing a reopen */
    uint64_t recoveries; /* failures after which a reopened device took audio again */
    int down;            /* 1 while the device is unusable and a reopen is owed; writes fail meanwhile */
    char error[256];     /* what the latest failure reported */
} dsd_audio_output_health;

/**
 * @brief Report an output stream's device failures and recoveries.
 *
 * PulseAudio and PortAudio give up a device whose write fails and reopen it behind the same stream handle, retrying
 * with a growing wait while it cannot be reopened; dsd_audio_write() fails while it is down. (PulseAudio closes and
 * reopens it on its output thread; PortAudio on the thread that writes and opened the stream.) A caller compares the counts
 * between calls to report each failure and recovery once. Backends that recover on their own without reporting
 * (AAudio) and the null backend report zeros.
 *
 * @param stream    Output stream handle.
 * @param out       Receives the report (zeroed on error).
 * @return 0 on success, -1 for a NULL, input or otherwise invalid stream.
 */
int dsd_audio_output_health_get(dsd_audio_stream* stream, dsd_audio_output_health* out);

/**
 * @brief Close an audio stream.
 *
 * @param stream    Stream handle (safe to pass NULL).
 */
void dsd_audio_close(dsd_audio_stream* stream);

/**
 * @brief Flush audio output buffers.
 *
 * @param stream    Stream handle.
 * @return 0 on success, non-zero on failure.
 */
int dsd_audio_drain(dsd_audio_stream* stream);

/**
 * @brief Get human-readable error string.
 *
 * @return Last error message, or empty string if none.
 */
const char* dsd_audio_get_error(void);

/**
 * @brief Get the name of the active audio backend.
 *
 * @return Backend name string (e.g., "pulse", "portaudio").
 */
const char* dsd_audio_backend_name(void);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_AUDIO_H_H */
