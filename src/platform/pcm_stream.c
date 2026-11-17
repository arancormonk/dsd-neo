// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/pcm_stream.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* About 100 ms of 48 kHz stereo PCM16: a refill takes whatever the stream holds, up to this. */
#define DSD_PCM_STREAM_BUF_BYTES (48000U * 2U * 2U / 10U)

/* How the stream stands: open, at its end, or ended by a read error. */
enum { PCM_STREAM_OPEN = 0, PCM_STREAM_EOF = 1, PCM_STREAM_FAILED = 2 };

/* The most one discard (dsd_pcm_stream_discard_through()) takes: the buffer and 20 refills of it, about 4 s of 48 kHz
   mono (2 s of stereo), and no refill but the first after 250 ms on the monotonic clock. */
#define DSD_PCM_STREAM_DISCARD_MAX_FILLS 20U
#define DSD_PCM_STREAM_DISCARD_MAX_MS    250U

/* What a failed read means: an interrupted call to repeat, a read that would block (a socket's receive timeout ran out,
   or the readiness it followed was spurious), or a failure. */
enum { PCM_STREAM_RETRY = 0, PCM_STREAM_WOULD_BLOCK = 1, PCM_STREAM_FAILURE = 2 };

struct dsd_pcm_stream {
    int fd;            /* the descriptor read: stdin, or a POSIX socket's; -1 for a Windows socket */
    dsd_socket_t sock; /* the socket waited on, or DSD_INVALID_SOCKET for a descriptor */
    int ended;
    size_t pos;       /* the next unread byte */
    size_t len;       /* the bytes held */
    uint64_t read_in; /* the bytes read in so far: written by the reader with fill_mu held */
    /* Held around each read into the buffer and its read_in update, and around a backlog mark's count, so a mark sees a
       read's bytes either still ready on the descriptor or already in read_in, never both or neither. */
    dsd_mutex_t fill_mu;
    uint8_t buf[DSD_PCM_STREAM_BUF_BYTES];
};

/* The stream's count for a backlog mark (<dsd-neo/platform/input_backlog.h>): the bytes read in so far and those the
   descriptor or socket holds ready, between two refills. A descriptor that is not a stream holds none. */
static uint64_t
pcm_stream_arrived(const void* queue) {
    dsd_pcm_stream* stream = (dsd_pcm_stream*)queue;
    dsd_mutex_lock(&stream->fill_mu);
    const long ready = stream->sock != DSD_INVALID_SOCKET ? dsd_socket_readable_bytes(stream->sock)
                                                          : dsd_fd_readable_bytes(stream->fd);
    const uint64_t arrived = stream->read_in + (ready > 0 ? (uint64_t)ready : 0U);
    dsd_mutex_unlock(&stream->fill_mu);
    return arrived;
}

uint64_t
dsd_pcm_stream_socket_arrived(dsd_socket_t sock) {
    if (sock == DSD_INVALID_SOCKET) {
        return 0U;
    }
    const long ready = dsd_socket_readable_bytes(sock);
    return ready > 0 ? (uint64_t)ready : 0U;
}

static dsd_pcm_stream*
pcm_stream_alloc(int fd, dsd_socket_t sock) {
    dsd_pcm_stream* stream = (dsd_pcm_stream*)calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return NULL;
    }
    if (dsd_mutex_init(&stream->fill_mu) != 0) {
        free(stream);
        return NULL;
    }
    stream->fd = fd;
    stream->sock = sock;
    stream->ended = PCM_STREAM_OPEN;
    /* The input the decoder reads from now on: a rigctl retune's drop is measured on it. */
    dsd_input_backlog_attach(stream, pcm_stream_arrived);
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
    if (stream == NULL) {
        return;
    }
    /* Detached first: a mark that is counting it holds the backlog's lock, so none is when the lock is destroyed. */
    dsd_input_backlog_detach(stream);
    (void)dsd_mutex_destroy(&stream->fill_mu);
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

/* One read into the buffer's free room and its count (read_in), under fill_mu: a backlog mark counts the bytes it took
   on one side only (pcm_stream_arrived()). The read follows a readiness wait, so it does not block (after a spurious
   readiness a socket's read, and a mark with it, waits for its receive timeout at most). A failed read's error
   (pcm_stream_read_error()) is taken before the lock is released, into @p error. */
static long
pcm_stream_read_counted(dsd_pcm_stream* stream, int* error) {
    dsd_mutex_lock(&stream->fill_mu);
    const long got = pcm_stream_raw_read(stream, stream->buf + stream->len, sizeof(stream->buf) - stream->len);
    if (got > 0) {
        stream->len += (size_t)got;
        stream->read_in += (uint64_t)got;
    } else if (got < 0) {
        *error = pcm_stream_read_error(stream);
    }
    dsd_mutex_unlock(&stream->fill_mu);
    return got;
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
        int error = PCM_STREAM_FAILURE;
        const long got = pcm_stream_read_counted(stream, &error);
        if (got > 0) {
            return 1;
        }
        if (got == 0) {
            stream->ended = PCM_STREAM_EOF;
            return -1;
        }
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

long
dsd_pcm_stream_discard_through(dsd_pcm_stream* stream, const dsd_input_cut* cut) {
    /* A descriptor nothing feeds (a regular file, a console) reports ready whatever a read would do: it holds no
       queued stream, and a read could wait or take the rest of a file. */
    if (stream == NULL || (stream->sock == DSD_INVALID_SOCKET && !dsd_fd_is_stream(stream->fd))
        || !dsd_input_backlog_cut_applies(cut, stream)) {
        return 0;
    }
    /* The bytes whole samples took so far: those read in, less what the buffer still holds. Only whole samples that
       had arrived by the mark go; a part of one stays, so the samples after it stay whole. */
    const uint64_t used = stream->read_in - (uint64_t)(stream->len - stream->pos);
    uint64_t owed = cut->arrived > used ? (cut->arrived - used) & ~(uint64_t)1U : 0U;
    const uint64_t start_ms = dsd_time_monotonic_ms();
    long dropped = 0;
    for (unsigned int fills = 0U; owed > 0U && stream->ended == PCM_STREAM_OPEN; fills++) {
        size_t whole = (stream->len - stream->pos) & ~(size_t)1U;
        if ((uint64_t)whole > owed) {
            whole = (size_t)owed;
        }
        stream->pos += whole;
        owed -= (uint64_t)whole;
        dropped += (long)(whole / 2U);
        /* The rest was ready at the mark, so a refill does not wait for it. The time cap never skips the first refill:
           a pause of the decoder inside the drop does not decide what it takes. */
        if (owed == 0U || fills == DSD_PCM_STREAM_DISCARD_MAX_FILLS
            || (fills > 0U && dsd_time_monotonic_ms() - start_ms >= DSD_PCM_STREAM_DISCARD_MAX_MS)
            || pcm_stream_wait(stream, 0U) <= 0 || pcm_stream_fill(stream) <= 0) {
            break;
        }
    }
    return dropped;
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
