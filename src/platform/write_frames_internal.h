// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The write loop behind dsd_write_frames(), with its system calls behind an ops table.
 *
 * Module-private: the POSIX and Win32 dsd_write_frames() drive it with the real write() (and poll() on POSIX), and
 * tests drive it with scripted writers to reach the interrupted, short, would-block and failed writes a real
 * descriptor rarely produces on demand.
 */

#ifndef DSD_NEO_SRC_PLATFORM_WRITE_FRAMES_INTERNAL_H
#define DSD_NEO_SRC_PLATFORM_WRITE_FRAMES_INTERNAL_H

#include <dsd-neo/platform/file_compat.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long one wait for room to finish a partly written frame may take, and how many such waits one call makes: at
   most 250 ms in all. */
enum { DSD_WRITE_FRAMES_TAIL_WAIT_MS = 50, DSD_WRITE_FRAMES_TAIL_WAITS = 5 };

typedef struct {
    /* write(2): bytes written, or -1 with errno set. */
    ssize_t (*write)(void* ctx, const void* buf, size_t count);
    /* Wait up to @p timeout_ms for the descriptor to take more bytes: 1 ready, 0 timed out, -1 failed (errno set).
       NULL where there is no way to wait; a partly written frame that would block is then an error. */
    int (*wait_writable)(void* ctx, int timeout_ms);
    void* ctx;
} dsd_write_frames_ops;

dsd_write_frames_status dsd_write_frames_loop(const dsd_write_frames_ops* ops, const void* buf, size_t bytes,
                                              size_t frame_bytes, size_t* written);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PLATFORM_WRITE_FRAMES_INTERNAL_H */
