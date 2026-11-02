// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Buffered raw PCM16LE mono reader over a file descriptor (stdin) or a socket (TCP audio).
 *
 * One reader per stream, shared by every consumer of it, so no byte is read twice or lost between two buffers. A
 * blocking read waits as long as the stream does. A bounded read waits at most its timeout for a whole sample, and
 * checks readiness only when its buffer runs dry; a part of a sample read so far is kept for the next call. A socket
 * read that its receive timeout (SO_RCVTIMEO) ends is the end of the stream to the blocking read, as it always was,
 * and only a timeout to the bounded read.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_PCM_STREAM_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_PCM_STREAM_H_

#include <dsd-neo/platform/sockets.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd_pcm_stream dsd_pcm_stream;

/** What dsd_pcm_stream_read_wait() found. */
enum {
    DSD_PCM_STREAM_TIMEOUT = -1, /**< no whole sample within the wait; the stream goes on */
    DSD_PCM_STREAM_END = 0,      /**< end of stream or a read error (dsd_pcm_stream_failed()) */
    DSD_PCM_STREAM_SAMPLE = 1,   /**< one sample */
};

/** A reader on @p fd (not taken over: closing the reader leaves it open), or NULL when @p fd is negative or memory
 * runs out. On Windows the descriptor is switched to binary mode. */
dsd_pcm_stream* dsd_pcm_stream_open_fd(int fd);

/** A reader on the connected socket @p sock (not taken over), or NULL when it is invalid or memory runs out. */
dsd_pcm_stream* dsd_pcm_stream_open_socket(dsd_socket_t sock);

/** Free the reader (NULL is fine). The descriptor or socket stays open. */
void dsd_pcm_stream_close(dsd_pcm_stream* stream);

/** Read one sample, waiting for it as long as it takes. Returns 1 with a sample, 0 at the end of the stream. */
int dsd_pcm_stream_read(dsd_pcm_stream* stream, int16_t* out);

/** Read one sample, waiting at most @p timeout_ms for it: DSD_PCM_STREAM_SAMPLE, _TIMEOUT or _END. */
int dsd_pcm_stream_read_wait(dsd_pcm_stream* stream, int16_t* out, unsigned int timeout_ms);

/** Whether the stream ended on a read error rather than its end. */
int dsd_pcm_stream_failed(const dsd_pcm_stream* stream);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_PCM_STREAM_H_ */
