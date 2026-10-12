// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_write_frames() (issue #659): interrupted and short writes are finished, a full non-blocking descriptor drops the
 * rest of the buffer at a frame boundary without waiting, a frame left half written is finished or reported, a write of
 * 0 never spins, and a reader that has gone away is an error, never a SIGPIPE.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "write_frames_internal.h"

#if !DSD_PLATFORM_WIN_NATIVE
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

/* One scripted write(): take up to `take` bytes (-1: fail with `err`). */
typedef struct {
    ssize_t take;
    int err;
} script_step;

typedef struct {
    const script_step* steps;
    size_t count;
    size_t next;
    uint8_t out[64];
    size_t out_len;
    int write_calls;
    int wait_calls;
    int wait_result; /* what wait_writable() answers */
} script_ctx;

static ssize_t
script_write(void* ctx, const void* buf, size_t count) {
    script_ctx* c = (script_ctx*)ctx;
    c->write_calls++;
    assert(c->next < c->count);
    const script_step step = c->steps[c->next++];
    if (step.take < 0) {
        errno = step.err;
        return -1;
    }
    size_t n = (size_t)step.take < count ? (size_t)step.take : count;
    assert(c->out_len + n <= sizeof(c->out));
    DSD_MEMCPY(c->out + c->out_len, buf, n);
    c->out_len += n;
    return (ssize_t)n;
}

static int
script_wait(void* ctx, int timeout_ms) {
    script_ctx* c = (script_ctx*)ctx;
    assert(timeout_ms > 0);
    c->wait_calls++;
    if (c->wait_result < 0) {
        errno = EIO;
    }
    return c->wait_result;
}

static dsd_write_frames_status
run_script(script_ctx* c, const script_step* steps, size_t count, size_t bytes, size_t frame_bytes, int can_wait,
           size_t* written) {
    static const uint8_t data[24] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                     13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    DSD_MEMSET(c, 0, sizeof(*c));
    c->steps = steps;
    c->count = count;
    c->wait_result = 1;
    assert(bytes <= sizeof(data));
    const dsd_write_frames_ops ops = {script_write, can_wait ? script_wait : NULL, c};
    const dsd_write_frames_status status = dsd_write_frames_loop(&ops, data, bytes, frame_bytes, written);
    assert(memcmp(c->out, data, c->out_len) == 0);
    return status;
}

static void
test_scripted_writes(void) {
    script_ctx c;
    size_t written = 99;

    /* Interrupted, then short counts that split frames: the whole buffer goes out, in order. */
    const script_step interrupted[] = {{-1, EINTR}, {3, 0}, {-1, EINTR}, {5, 0}, {16, 0}};
    assert(run_script(&c, interrupted, 5, 24, 4, 1, &written) == DSD_WRITE_FRAMES_OK);
    assert(written == 24U && c.out_len == 24U && c.wait_calls == 0);

    /* Full at a frame boundary after two whole frames: the rest is dropped at once, no waiting. */
    const script_step full_at_boundary[] = {{8, 0}, {-1, EAGAIN}};
    assert(run_script(&c, full_at_boundary, 2, 24, 4, 1, &written) == DSD_WRITE_FRAMES_WOULD_BLOCK);
    assert(written == 8U && c.wait_calls == 0 && c.write_calls == 2);

    /* Full from the start: nothing written, still aligned. */
    const script_step full_at_once[] = {{-1, EAGAIN}};
    assert(run_script(&c, full_at_once, 1, 24, 4, 1, &written) == DSD_WRITE_FRAMES_WOULD_BLOCK);
    assert(written == 0U);

#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    const script_step full_wouldblock[] = {{-1, EWOULDBLOCK}};
    assert(run_script(&c, full_wouldblock, 1, 24, 4, 1, &written) == DSD_WRITE_FRAMES_WOULD_BLOCK);
#endif

    /* Full in the middle of a frame: wait for room, finish the frame, then stop at the next boundary. */
    const script_step full_mid_frame[] = {{6, 0}, {-1, EAGAIN}, {2, 0}, {-1, EAGAIN}};
    assert(run_script(&c, full_mid_frame, 4, 24, 4, 1, &written) == DSD_WRITE_FRAMES_WOULD_BLOCK);
    assert(written == 8U && c.wait_calls == 1);

    /* Full in the middle of a frame and it never drains: an error, after a bounded number of waits. */
    const script_step stuck_mid_frame[] = {{6, 0},       {-1, EAGAIN}, {-1, EAGAIN}, {-1, EAGAIN},
                                           {-1, EAGAIN}, {-1, EAGAIN}, {-1, EAGAIN}, {-1, EAGAIN}};
    DSD_MEMSET(&c, 0, sizeof(c));
    c.steps = stuck_mid_frame;
    c.count = sizeof(stuck_mid_frame) / sizeof(stuck_mid_frame[0]);
    c.wait_result = 0;
    {
        static const uint8_t data[24] = {0};
        const dsd_write_frames_ops ops = {script_write, script_wait, &c};
        assert(dsd_write_frames_loop(&ops, data, 24, 4, &written) == DSD_WRITE_FRAMES_ERROR);
        assert(errno == EAGAIN);
        assert(written == 6U);
        assert(c.wait_calls == DSD_WRITE_FRAMES_TAIL_WAITS);
    }

    /* No way to wait (Windows): a half-written frame that would block is an error. */
    const script_step no_wait_mid_frame[] = {{6, 0}, {-1, EAGAIN}};
    assert(run_script(&c, no_wait_mid_frame, 2, 24, 4, 0, &written) == DSD_WRITE_FRAMES_ERROR);
    assert(errno == EAGAIN && written == 6U);

    /* A failed wait is an error with its own errno. */
    const script_step wait_fails[] = {{6, 0}, {-1, EAGAIN}};
    DSD_MEMSET(&c, 0, sizeof(c));
    c.steps = wait_fails;
    c.count = 2;
    c.wait_result = -1;
    {
        static const uint8_t data[24] = {0};
        const dsd_write_frames_ops ops = {script_write, script_wait, &c};
        assert(dsd_write_frames_loop(&ops, data, 24, 4, &written) == DSD_WRITE_FRAMES_ERROR);
        assert(errno == EIO);
    }

    /* A write of 0 is an error, never a spin. */
    const script_step zero[] = {{4, 0}, {0, 0}};
    assert(run_script(&c, zero, 2, 24, 4, 1, &written) == DSD_WRITE_FRAMES_ERROR);
    assert(errno == EIO && written == 4U && c.write_calls == 2);

    /* A closed reader or a full disk is an error with its errno. */
    const script_step epipe[] = {{8, 0}, {-1, EPIPE}};
    assert(run_script(&c, epipe, 2, 24, 4, 1, &written) == DSD_WRITE_FRAMES_ERROR);
    assert(errno == EPIPE && written == 8U);
    const script_step enospc[] = {{-1, ENOSPC}};
    assert(run_script(&c, enospc, 1, 24, 4, 1, &written) == DSD_WRITE_FRAMES_ERROR);
    assert(errno == ENOSPC && written == 0U);

    /* A frame size of 0 counts as 1; nothing to write is success. */
    const script_step one_byte_frames[] = {{3, 0}, {-1, EAGAIN}};
    assert(run_script(&c, one_byte_frames, 2, 24, 0, 1, &written) == DSD_WRITE_FRAMES_WOULD_BLOCK);
    assert(written == 3U && c.wait_calls == 0);
    assert(run_script(&c, NULL, 0, 0, 4, 1, &written) == DSD_WRITE_FRAMES_OK);
    assert(written == 0U);

    /* Bad arguments. */
    errno = 0;
    assert(dsd_write_frames_loop(NULL, "x", 1, 1, &written) == DSD_WRITE_FRAMES_ERROR);
    assert(errno == EINVAL && written == 0U);
}

#if !DSD_PLATFORM_WIN_NATIVE
static int
sigpipe_pending(void) {
    sigset_t pending;
    assert(sigpending(&pending) == 0);
    return sigismember(&pending, SIGPIPE) == 1;
}

static int
sigpipe_blocked(void) {
    sigset_t mask;
    assert(pthread_sigmask(SIG_BLOCK, NULL, &mask) == 0);
    return sigismember(&mask, SIGPIPE) == 1;
}

static void
test_closed_reader_raises_no_sigpipe(void) {
    /* The default disposition: a SIGPIPE that got through would end this test. */
    void (*old_handler)(int) = signal(SIGPIPE, SIG_DFL);
    int fds[2];
    assert(pipe(fds) == 0);
    assert(close(fds[0]) == 0);

    const uint8_t block[640] = {0};
    size_t written = 99;
    errno = 0;
    assert(dsd_write_frames(fds[1], block, sizeof(block), 4, &written) == DSD_WRITE_FRAMES_ERROR);
    assert(errno == EPIPE);
    assert(written == 0U);
    /* The signal mask is as it was, and nothing is left pending. */
    assert(!sigpipe_blocked());
    assert(!sigpipe_pending());

    /* A SIGPIPE that was already pending (blocked by the host) is the host's: it stays. */
    sigset_t pipe_set;
    sigset_t old_mask;
    (void)sigemptyset(&pipe_set);
    (void)sigaddset(&pipe_set, SIGPIPE);
    assert(pthread_sigmask(SIG_BLOCK, &pipe_set, &old_mask) == 0);
    assert(raise(SIGPIPE) == 0);
    assert(sigpipe_pending());
    assert(dsd_write_frames(fds[1], block, sizeof(block), 4, &written) == DSD_WRITE_FRAMES_ERROR);
    assert(errno == EPIPE);
    assert(sigpipe_blocked());
    assert(sigpipe_pending());
    int sig = 0;
    assert(sigwait(&pipe_set, &sig) == 0 && sig == SIGPIPE);
    assert(!sigpipe_pending());

    /* Blocked by the host and not pending: the write's own is consumed, the mask stays blocked. */
    assert(dsd_write_frames(fds[1], block, sizeof(block), 4, &written) == DSD_WRITE_FRAMES_ERROR);
    assert(sigpipe_blocked());
    assert(!sigpipe_pending());
    assert(pthread_sigmask(SIG_SETMASK, &old_mask, NULL) == 0);

    assert(close(fds[1]) == 0);
    (void)signal(SIGPIPE, old_handler);
}

static void
test_full_nonblocking_pipe_drops_whole_blocks(void) {
    int fds[2];
    assert(pipe(fds) == 0);
    const int flags = fcntl(fds[1], F_GETFL);
    assert(flags >= 0 && fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) == 0);

    /* Fill the pipe to the last byte. */
    const uint8_t one = 0x55;
    size_t filled = 0;
    for (;;) {
        const ssize_t n = write(fds[1], &one, 1);
        if (n == 1) {
            filled++;
            continue;
        }
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        break;
    }
    assert(filled > 0U);

    const uint8_t block[3840] = {0};
    size_t written = 99;
    assert(dsd_write_frames(fds[1], block, sizeof(block), 4, &written) == DSD_WRITE_FRAMES_WOULD_BLOCK);
    assert(written % 4U == 0U);
    assert(written == 0U);

    /* Room again: the next block goes out whole. */
    uint8_t sink[4096];
    size_t drained = 0;
    while (drained < filled) {
        const ssize_t n = read(fds[0], sink, sizeof(sink));
        assert(n > 0);
        drained += (size_t)n;
    }
    assert(dsd_write_frames(fds[1], block, 640, 4, &written) == DSD_WRITE_FRAMES_OK);
    assert(written == 640U);

    assert(close(fds[0]) == 0);
    assert(close(fds[1]) == 0);
}
#endif

#if defined(__linux__)
static volatile sig_atomic_t g_alarms;

static void
on_alarm(int sig) {
    (void)sig;
    g_alarms++;
}

/* A frame a short count left half written, on a pipe that stays full, while signals keep interrupting the wait: the
   write still gives up within its bound (DSD_WRITE_FRAMES_TAIL_WAITS waits of DSD_WRITE_FRAMES_TAIL_WAIT_MS), as an
   error, because an interrupted wait goes on only for what is left of it. Linux pipe internals make the half frame:
   two bytes merged into a last page that has room for exactly two. */
static void
test_interrupted_tail_wait_stays_bounded(void) {
    int fds[2];
    assert(pipe(fds) == 0);
    const int flags = fcntl(fds[1], F_GETFL);
    assert(flags >= 0 && fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) == 0);
    static uint8_t page[4096];
    for (int i = 0; i < 15; i++) {
        if (write(fds[1], page, sizeof(page)) != (ssize_t)sizeof(page)) {
            printf("PLATFORM_WRITE_FRAMES: interrupted tail wait skipped (pipe is not 16 pages of 4 KiB)\n");
            (void)close(fds[0]);
            (void)close(fds[1]);
            return;
        }
    }
    const ssize_t last = write(fds[1], page, sizeof(page) - 2U);
    static uint8_t block[8194];
    if (last != (ssize_t)(sizeof(page) - 2U)) {
        printf("PLATFORM_WRITE_FRAMES: interrupted tail wait skipped (could not leave two bytes free)\n");
        (void)close(fds[0]);
        (void)close(fds[1]);
        return;
    }

    struct sigaction sa;
    struct sigaction old_sa;
    DSD_MEMSET(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: poll() returns EINTR */
    assert(sigaction(SIGALRM, &sa, &old_sa) == 0);
    struct itimerval every_10ms;
    DSD_MEMSET(&every_10ms, 0, sizeof(every_10ms));
    every_10ms.it_interval.tv_usec = 10000;
    every_10ms.it_value.tv_usec = 10000;
    assert(setitimer(ITIMER_REAL, &every_10ms, NULL) == 0);

    struct timespec t0;
    struct timespec t1;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0); // nosemgrep: dsd-neo.no-direct-clock-read -- measures the test itself
    size_t written = 99;
    g_alarms = 0;
    const dsd_write_frames_status status = dsd_write_frames(fds[1], block, sizeof(block), 4, &written);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1); // nosemgrep: dsd-neo.no-direct-clock-read -- measures the test itself

    struct itimerval off;
    DSD_MEMSET(&off, 0, sizeof(off));
    assert(setitimer(ITIMER_REAL, &off, NULL) == 0);
    assert(sigaction(SIGALRM, &old_sa, NULL) == 0);

    const double elapsed_ms =
        ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) + ((double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0);
    if (written != 2U) {
        printf("PLATFORM_WRITE_FRAMES: interrupted tail wait skipped (the kernel took %zu bytes, not 2)\n", written);
    } else {
        assert(status == DSD_WRITE_FRAMES_ERROR);
        assert(errno == EAGAIN);
        assert(g_alarms > 0);
        assert(elapsed_ms < 2000.0);
    }
    (void)close(fds[0]);
    (void)close(fds[1]);
}
#endif

static void
test_bad_descriptor(void) {
    const uint8_t block[4] = {0};
    size_t written = 99;
    errno = 0;
    assert(dsd_write_frames(-1, block, sizeof(block), 2, &written) == DSD_WRITE_FRAMES_ERROR);
    assert(errno == EBADF);
    assert(written == 0U);
}

int
main(void) {
    test_scripted_writes();
#if !DSD_PLATFORM_WIN_NATIVE
    test_closed_reader_raises_no_sigpipe();
    test_full_nonblocking_pipe_drops_whole_blocks();
#endif
#if defined(__linux__)
    test_interrupted_tail_wait_stays_bounded();
#endif
    test_bad_descriptor();
    printf("PLATFORM_WRITE_FRAMES: OK\n");
    return 0;
}
