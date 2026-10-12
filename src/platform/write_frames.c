// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/file_compat.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "write_frames_internal.h"

static int
write_frames_would_block(int err) {
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (err == EWOULDBLOCK) {
        return 1;
    }
#endif
    return err == EAGAIN;
}

/* A write that would block left part of a frame written. Dropping the rest would shift every later sample, so wait for
   room to finish it, a bounded number of times. Returns 1 to write again, 0 with errno set when the stream cannot stay
   aligned. */
static int
write_frames_wait_to_finish_frame(const dsd_write_frames_ops* ops, int* tail_waits, int would_block_errno) {
    if (ops->wait_writable == NULL || *tail_waits >= DSD_WRITE_FRAMES_TAIL_WAITS) {
        errno = would_block_errno;
        return 0;
    }
    (*tail_waits)++;
    const int ready = ops->wait_writable(ops->ctx, DSD_WRITE_FRAMES_TAIL_WAIT_MS);
    if (ready < 0) {
        return 0;
    }
    if (ready == 0 && *tail_waits >= DSD_WRITE_FRAMES_TAIL_WAITS) {
        errno = EAGAIN;
        return 0;
    }
    return 1;
}

/* What a failed write means: 1 to write again (interrupted, or a half-written frame finished), else the status. */
static int
write_frames_on_error(const dsd_write_frames_ops* ops, size_t done, size_t frame_bytes, int* tail_waits,
                      dsd_write_frames_status* status) {
    const int err = errno;
    if (err == EINTR) {
        return 1;
    }
    if (!write_frames_would_block(err)) {
        errno = err;
        *status = DSD_WRITE_FRAMES_ERROR;
        return 0;
    }
    if (done % frame_bytes == 0U) {
        /* A full descriptor at a frame boundary: the rest of the buffer is dropped whole, so the stream stays aligned
           and the caller is never held up. */
        *status = DSD_WRITE_FRAMES_WOULD_BLOCK;
        return 0;
    }
    if (write_frames_wait_to_finish_frame(ops, tail_waits, err)) {
        return 1;
    }
    *status = DSD_WRITE_FRAMES_ERROR;
    return 0;
}

dsd_write_frames_status
dsd_write_frames_loop(const dsd_write_frames_ops* ops, const void* buf, size_t bytes, size_t frame_bytes,
                      size_t* written) {
    const uint8_t* data = (const uint8_t*)buf;
    size_t done = 0;
    int tail_waits = 0;
    dsd_write_frames_status status = DSD_WRITE_FRAMES_OK;

    if (frame_bytes == 0) {
        frame_bytes = 1;
    }
    if (ops == NULL || ops->write == NULL || (data == NULL && bytes > 0)) {
        errno = EINVAL;
        status = DSD_WRITE_FRAMES_ERROR;
        bytes = 0;
    }

    while (done < bytes) {
        errno = 0;
        const ssize_t n = ops->write(ops->ctx, data + done, bytes - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == 0) {
            /* Nothing taken and no error: never spin on it. */
            errno = EIO;
            status = DSD_WRITE_FRAMES_ERROR;
            break;
        }
        if (!write_frames_on_error(ops, done, frame_bytes, &tail_waits, &status)) {
            break;
        }
    }

    if (written != NULL) {
        *written = done;
    }
    return status;
}
