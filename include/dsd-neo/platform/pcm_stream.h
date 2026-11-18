// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Buffered raw PCM16LE mono reader over a file descriptor (stdin) or a socket (TCP audio).
 *
 * One reader per stream, shared by every consumer of it, so no byte is read twice or lost between two buffers. A read
 * waits at most its timeout for a whole sample, and checks readiness only when its buffer runs dry; a part of a sample
 * read so far is kept for the next call. A socket read that its receive timeout (SO_RCVTIMEO) ends is a timeout, not
 * the end of the stream: a quiet sender is no closed connection.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_PCM_STREAM_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_PCM_STREAM_H_

#include <dsd-neo/platform/input_backlog.h>
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
 * runs out. On Windows the descriptor is switched to binary mode. The reader is the input a rigctl retune's drop is
 * measured on from then on (dsd_input_backlog_attach()), until it is closed or another input attaches. */
dsd_pcm_stream* dsd_pcm_stream_open_fd(int fd);

/** A reader on the connected socket @p sock (not taken over), or NULL when it is invalid or memory runs out. */
dsd_pcm_stream* dsd_pcm_stream_open_socket(dsd_socket_t sock);

/** The count a reader on the connected socket @p sock starts from, before the reader exists: the bytes the socket holds
 * ready, as nothing has read it yet (0 for an invalid socket). A TCP reconnect's opening counts the new connection by it
 * (dsd_input_backlog_opening()) until its reader attaches and goes on from it. Any thread. */
uint64_t dsd_pcm_stream_socket_arrived(dsd_socket_t sock);

/** Detach and free the reader (NULL is fine). The descriptor or socket stays open. */
void dsd_pcm_stream_close(dsd_pcm_stream* stream);

/** Read one sample, waiting at most @p timeout_ms for it: DSD_PCM_STREAM_SAMPLE, _TIMEOUT or _END. */
int dsd_pcm_stream_read_wait(dsd_pcm_stream* stream, int16_t* out, unsigned int timeout_ms);

/**
 * Drop the whole samples the stream held when @p cut was marked (dsd_input_backlog_mark()) and the decoder has not
 * read: from the reader's buffer, then from the descriptor or socket, read without waiting: at most 21 buffers of
 * 19200 bytes (about 4 s of 48 kHz mono), and no refill but the first after 250 ms on the monotonic clock (a pause
 * of the decoder inside the drop never skips its first refill). What those bounds leave stays owed: the reader pays it
 * at its refills, a bounded pass per read (dsd_pcm_stream_read_wait() returns DSD_PCM_STREAM_TIMEOUT while it still
 * owes), before it returns a sample. A part of a sample is kept, so the samples after it
 * stay whole, and every byte that arrived after the mark is kept. A cut measured on another input or attachment
 * (dsd_input_backlog_cut_applies()) and a descriptor that is not a pipe, FIFO or socket (dsd_fd_is_stream(): a regular
 * file, a console) drop nothing. Returns the samples dropped (0 for NULL or an ended stream).
 */
long dsd_pcm_stream_discard_through(dsd_pcm_stream* stream, const dsd_input_cut* cut);

/** Whether the stream ended on a read error rather than its end. */
int dsd_pcm_stream_failed(const dsd_pcm_stream* stream);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_PCM_STREAM_H_ */
