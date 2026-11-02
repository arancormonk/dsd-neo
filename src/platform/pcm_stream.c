// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/pcm_stream.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* About 100 ms of 48 kHz stereo PCM16: a refill takes whatever the stream holds, up to this. */
#define DSD_PCM_STREAM_BUF_BYTES (48000U * 2U * 2U / 10U)

/* How the stream stands: open, at its end, or ended by a read error. */
enum { PCM_STREAM_OPEN = 0, PCM_STREAM_EOF = 1, PCM_STREAM_FAILED = 2 };

/* What a failed read means: an interrupted call to repeat, a read that would block (a socket's receive timeout ran out,
   or the readiness it followed was spurious), or a failure. */
enum { PCM_STREAM_RETRY = 0, PCM_STREAM_WOULD_BLOCK = 1, PCM_STREAM_FAILURE = 2 };

struct dsd_pcm_stream {
    int fd;            /* the descriptor read: stdin, or a POSIX socket's; -1 for a Windows socket */
    dsd_socket_t sock; /* the socket waited on, or DSD_INVALID_SOCKET for a descriptor */
    int ended;
    size_t pos; /* the next unread byte */
    size_t len; /* the bytes held */
    uint8_t buf[DSD_PCM_STREAM_BUF_BYTES];
};

static dsd_pcm_stream*
pcm_stream_alloc(int fd, dsd_socket_t sock) {
    dsd_pcm_stream* stream = (dsd_pcm_stream*)calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return NULL;
    }
    stream->fd = fd;
    stream->sock = sock;
    stream->ended = PCM_STREAM_OPEN;
    return stream;
}

dsd_pcm_stream*
dsd_pcm_stream_open_fd(int fd) {
    if (fd < 0) {
        return NULL;
    }
    /* Windows reads a descriptor in text mode unless told otherwise, which would turn CR LF byte pairs of the audio
       into LF. */
    (void)dsd_set_binary_mode(fd);
    return pcm_stream_alloc(fd, DSD_INVALID_SOCKET);
}

dsd_pcm_stream*
dsd_pcm_stream_open_socket(dsd_socket_t sock) {
    if (sock == DSD_INVALID_SOCKET) {
        return NULL;
    }
#if DSD_PLATFORM_WIN_NATIVE
    return pcm_stream_alloc(-1, sock);
#else
    return pcm_stream_alloc((int)sock, sock);
#endif
}

void
dsd_pcm_stream_close(dsd_pcm_stream* stream) {
    free(stream);
}

int
dsd_pcm_stream_failed(const dsd_pcm_stream* stream) {
    return stream != NULL && stream->ended == PCM_STREAM_FAILED;
}

/* One little-endian sample from the buffer, when it holds a whole one. */
static int
pcm_stream_take(dsd_pcm_stream* stream, int16_t* out) {
    if (stream->len - stream->pos < 2U) {
        return 0;
    }
    const unsigned int lo = stream->buf[stream->pos];
    const unsigned int hi = stream->buf[stream->pos + 1U];
    stream->pos += 2U;
    *out = (int16_t)(uint16_t)(lo | (hi << 8));
    return 1;
}

static int
pcm_stream_read_error(const dsd_pcm_stream* stream) {
#if DSD_PLATFORM_WIN_NATIVE
    if (stream->sock != DSD_INVALID_SOCKET) {
        const int error = dsd_socket_get_error();
        if (error == WSAEINTR) {
            return PCM_STREAM_RETRY;
        }
        return (error == WSAEWOULDBLOCK || error == WSAETIMEDOUT) ? PCM_STREAM_WOULD_BLOCK : PCM_STREAM_FAILURE;
    }
#else
    (void)stream;
#endif
    if (errno == EINTR) {
        return PCM_STREAM_RETRY;
    }
    if (errno == EAGAIN) {
        return PCM_STREAM_WOULD_BLOCK;
    }
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (errno == EWOULDBLOCK) {
        return PCM_STREAM_WOULD_BLOCK;
    }
#endif
    return PCM_STREAM_FAILURE;
}

static long
pcm_stream_raw_read(const dsd_pcm_stream* stream, uint8_t* dst, size_t room) {
#if DSD_PLATFORM_WIN_NATIVE
    if (stream->sock != DSD_INVALID_SOCKET) {
        return (long)dsd_socket_recv(stream->sock, dst, room, 0);
    }
#endif
    return (long)dsd_read(stream->fd, dst, room);
}

/* One read into the buffer's free room, after the part of a sample left over moves to its front. Returns 1 when bytes
   came, 0 when the read would block, and -1 when the stream ended (stream->ended says how). An interrupted read is
   repeated. */
static int
pcm_stream_fill(dsd_pcm_stream* stream) {
    const size_t left = stream->len - stream->pos;
    if (stream->pos > 0U && left > 0U) {
        DSD_MEMMOVE(stream->buf, stream->buf + stream->pos, left);
    }
    stream->pos = 0U;
    stream->len = left;
    for (;;) {
        const long got = pcm_stream_raw_read(stream, stream->buf + stream->len, sizeof(stream->buf) - stream->len);
        if (got > 0) {
            stream->len += (size_t)got;
            return 1;
        }
        if (got == 0) {
            stream->ended = PCM_STREAM_EOF;
            return -1;
        }
        const int error = pcm_stream_read_error(stream);
        if (error == PCM_STREAM_WOULD_BLOCK) {
            return 0;
        }
        if (error != PCM_STREAM_RETRY) {
            stream->ended = PCM_STREAM_FAILED;
            return -1;
        }
    }
}

/* Whether the stream can be read without blocking within @p timeout_ms: 1, 0 on timeout, -1 on failure. */
static int
pcm_stream_wait(const dsd_pcm_stream* stream, unsigned int timeout_ms) {
    if (stream->sock != DSD_INVALID_SOCKET) {
        const int ready = dsd_socket_wait(stream->sock, DSD_SOCKET_WAIT_READ, timeout_ms);
        return ready < 0 ? -1 : (ready != 0);
    }
    return dsd_fd_wait_readable(stream->fd, timeout_ms);
}

int
dsd_pcm_stream_read_wait(dsd_pcm_stream* stream, int16_t* out, unsigned int timeout_ms) {
    if (stream == NULL || out == NULL) {
        return DSD_PCM_STREAM_END;
    }
    if (pcm_stream_take(stream, out)) {
        return DSD_PCM_STREAM_SAMPLE;
    }
    if (stream->ended != PCM_STREAM_OPEN) {
        return DSD_PCM_STREAM_END;
    }
    /* Readiness is checked here only, when the buffer ran dry, and the buffer refills at most once per wait. */
    const int ready = pcm_stream_wait(stream, timeout_ms);
    if (ready < 0) {
        stream->ended = PCM_STREAM_FAILED;
        return DSD_PCM_STREAM_END;
    }
    if (ready == 0 || pcm_stream_fill(stream) == 0) {
        return DSD_PCM_STREAM_TIMEOUT;
    }
    if (stream->ended != PCM_STREAM_OPEN) {
        return DSD_PCM_STREAM_END;
    }
    return pcm_stream_take(stream, out) ? DSD_PCM_STREAM_SAMPLE : DSD_PCM_STREAM_TIMEOUT;
}
