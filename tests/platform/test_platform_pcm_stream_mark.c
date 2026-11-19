// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * Issue #834: a backlog mark taken on the stdin or TCP reader while the decoder refills it. The mark counts the bytes
 * the reader read in and those the pipe or socket holds ready; a refill moves bytes from the one to the other. The
 * readiness query is wrapped (GNU ld --wrap) so that the decoder thread tries its refill between the query and the
 * count: the mark must still count each byte once, and the drop that follows must keep every sample that came after
 * the mark. The monotonic clock is wrapped too, so a drop can be paused past its time cap before its first refill.
 * The read itself is wrapped as well, so a read of a descriptor that is not a stream can block as a Windows console's
 * does, after its readiness said it would not.
 */
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/pcm_stream.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "test_platform_pcm_stream_mark_seams.h"
#include "test_support.h"

static int g_failures = 0;

static void
check(int condition, const char* what) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* While set, every clock read is 300 ms after the one before: the decoder paused past a drop's 250 ms cap. */
static atomic_int g_clock_jumps;
static uint64_t g_clock_offset_ms;

uint64_t
mark_time_monotonic_ms(void) {
    if (atomic_load(&g_clock_jumps)) {
        g_clock_offset_ms += 300U;
    }
    return mark_real_time_monotonic_ms() + g_clock_offset_ms;
}

/* The descriptor whose readiness waits are recorded, and the longest wait asked on it. */
static atomic_int g_wait_fd = -1;
static atomic_int g_wait_longest_ms;

int
mark_fd_wait_readable(int fd, unsigned int timeout_ms) {
    if (fd == atomic_load(&g_wait_fd) && (int)timeout_ms > atomic_load(&g_wait_longest_ms)) {
        atomic_store(&g_wait_longest_ms, (int)timeout_ms);
    }
    return mark_real_fd_wait_readable(fd, timeout_ms);
}

/* Issue #834: a mutex initialization made to fail (once, while set), the mutex it failed on, and whether anything locked
   that mutex before an initialization of it succeeded. */
static atomic_int g_mutex_init_fails;
static dsd_mutex_t* g_failed_mutex;
static atomic_int g_locked_failed_mutex;

int
mark_mutex_init(dsd_mutex_t* mutex) {
    if (atomic_exchange(&g_mutex_init_fails, 0)) {
        g_failed_mutex = mutex;
        return -1;
    }
    if (mutex == g_failed_mutex) {
        g_failed_mutex = NULL;
    }
    return mark_real_mutex_init(mutex);
}

int
mark_mutex_lock(dsd_mutex_t* mutex) {
    if (mutex != NULL && mutex == g_failed_mutex) {
        atomic_store(&g_locked_failed_mutex, 1);
    }
    return mark_real_mutex_lock(mutex);
}

/* Issue #834: the backlog's lock, made on first use, fails to initialize: the call declines (a mark measures nothing)
   without locking it, and the next call makes it again. Run first, before anything else uses the backlog. */
static void
test_backlog_lock_that_fails_to_initialize(void) {
    dsd_input_cut cut;
    DSD_MEMSET(&cut, 0xFF, sizeof cut);
    atomic_store(&g_mutex_init_fails, 1);
    dsd_input_backlog_mark(&cut);
    check(atomic_load(&g_mutex_init_fails) == 0, "the backlog makes its lock on first use");
    check(!atomic_load(&g_locked_failed_mutex), "a lock that failed to initialize is never locked");
    check(cut.measured == 0U, "a mark with no lock measures nothing");
    dsd_input_backlog_mark(&cut);
    check(g_failed_mutex == NULL, "the next call makes the lock again");
}

/* The decoder thread's refill, started from inside the mark's readiness query. */
static dsd_pcm_stream* g_reader;
static atomic_int g_armed;   /* the next readiness query starts the refill */
static atomic_int g_go;      /* the refill may run */
static atomic_int g_done;    /* it ran */
static atomic_int g_got;     /* what it returned */
static atomic_int g_sample;  /* and the sample it read */
static atomic_int g_overlap; /* it finished inside the query */

/* How long the query lets the refill run before it returns: ample for a refill nothing holds back. */
#define MARK_REFILL_WAIT_MS 1000U

static void
refill_during_query(void) {
    if (!atomic_exchange(&g_armed, 0)) {
        return;
    }
    atomic_store(&g_go, 1);
    const uint64_t start = dsd_time_monotonic_ms();
    while (!atomic_load(&g_done) && dsd_time_monotonic_ms() - start < MARK_REFILL_WAIT_MS) {
        dsd_sleep_ms(1U);
    }
    atomic_store(&g_overlap, atomic_load(&g_done));
}

long
mark_fd_readable_bytes(int fd) {
    const long ready = mark_real_fd_readable_bytes(fd);
    refill_during_query();
    return ready;
}

long
mark_socket_readable_bytes(dsd_socket_t sock) {
    const long ready = mark_real_socket_readable_bytes(sock);
    refill_during_query();
    return ready;
}

static DSD_THREAD_RETURN_TYPE
decoder_refill(void* arg) {
    (void)arg;
    while (!atomic_load(&g_go)) {
        dsd_sleep_ms(1U);
    }
    int16_t s = 0;
    atomic_store(&g_got, dsd_pcm_stream_read_wait(g_reader, &s, 0U));
    atomic_store(&g_sample, s);
    atomic_store(&g_done, 1);
    DSD_THREAD_RETURN;
}

/* Three old samples ready on @p write_fd's stream, the mark taken while the decoder's refill reads them in, then
   three new ones: the mark counts 6 bytes, the decoder's read is the first old sample, and the drop takes the other
   two old ones and keeps all three new ones. */
static void
mark_during_refill(const char* name, dsd_pcm_stream* stream, int write_fd) {
    char what[160];
    g_reader = stream;
    atomic_store(&g_go, 0);
    atomic_store(&g_done, 0);
    atomic_store(&g_overlap, 0);
    const int16_t old_samples[3] = {10, 11, 12};
    const int16_t new_samples[3] = {100, 101, 102};
    DSD_SNPRINTF(what, sizeof what, "%s: the old samples are sent", name);
    check(dsd_write(write_fd, old_samples, sizeof old_samples) == (ssize_t)sizeof old_samples, what);
    dsd_thread_t decoder;
    if (dsd_thread_create(&decoder, decoder_refill, NULL) != 0) {
        check(0, "the decoder thread");
        return;
    }
    atomic_store(&g_armed, 1);
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    atomic_store(&g_go, 1);
    (void)dsd_thread_join(decoder);
    DSD_SNPRINTF(what, sizeof what, "%s: the mark counts each old byte once (%llu bytes, the refill %s the query)",
                 name, (unsigned long long)cut.arrived, atomic_load(&g_overlap) ? "ran inside" : "waited out");
    check(cut.measured == 1U && cut.arrived == sizeof old_samples, what);
    DSD_SNPRINTF(what, sizeof what, "%s: the decoder's read is the first old sample", name);
    check(atomic_load(&g_got) == DSD_PCM_STREAM_SAMPLE && atomic_load(&g_sample) == 10, what);
    DSD_SNPRINTF(what, sizeof what, "%s: the new samples are sent", name);
    check(dsd_write(write_fd, new_samples, sizeof new_samples) == (ssize_t)sizeof new_samples, what);
    const long dropped = dsd_pcm_stream_discard_through(stream, &cut);
    int16_t s = 0;
    const int got = dsd_pcm_stream_read_wait(stream, &s, 1000U);
    DSD_SNPRINTF(what, sizeof what, "%s: the drop takes the two old samples left (%ld) and the next is new (%d, %d)",
                 name, dropped, got, s);
    check(dropped == 2 && got == DSD_PCM_STREAM_SAMPLE && s == 100, what);
}

static void
test_pipe_mark_during_refill(void) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        check(0, "pipe");
        return;
    }
    dsd_pcm_stream* stream = dsd_pcm_stream_open_fd(fds[0]);
    check(stream != NULL, "open on the pipe");
    if (stream != NULL) {
        mark_during_refill("pipe", stream, fds[1]);
    }
    dsd_pcm_stream_close(stream);
    (void)close(fds[0]);
    (void)close(fds[1]);
}

static void
test_socket_mark_during_refill(void) {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        check(0, "socket pair");
        return;
    }
    dsd_pcm_stream* stream = dsd_pcm_stream_open_socket((dsd_socket_t)fds[0]);
    check(stream != NULL, "open on the socket");
    if (stream != NULL) {
        mark_during_refill("socket", stream, fds[1]);
    }
    dsd_pcm_stream_close(stream);
    (void)close(fds[0]);
    (void)close(fds[1]);
}

/* The decoder pauses past the drop's 250 ms cap between its start and its first refill: three old samples were ready
   on the pipe at the mark, none read in yet, and all three still go, since the cap never skips the first refill. The
   new sample after them is read. */
static void
test_pause_before_the_first_refill(void) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        check(0, "pipe for the paused drop");
        return;
    }
    dsd_pcm_stream* stream = dsd_pcm_stream_open_fd(fds[0]);
    check(stream != NULL, "open on the pipe for the paused drop");
    const int16_t old_samples[3] = {20, 21, 22};
    const int16_t new_sample = 200;
    check(dsd_write(fds[1], old_samples, sizeof old_samples) == (ssize_t)sizeof old_samples, "the old samples");
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    check(cut.measured == 1U && cut.arrived == sizeof old_samples, "the mark counts them");
    atomic_store(&g_clock_jumps, 1);
    const long dropped = dsd_pcm_stream_discard_through(stream, &cut);
    atomic_store(&g_clock_jumps, 0);
    check(dsd_write(fds[1], &new_sample, sizeof new_sample) == (ssize_t)sizeof new_sample, "the new sample");
    int16_t s = 0;
    const int got = dsd_pcm_stream_read_wait(stream, &s, 1000U);
    char what[160];
    DSD_SNPRINTF(what, sizeof what, "a drop paused past its cap still takes its first refill (dropped %ld, next %d)",
                 dropped, got == DSD_PCM_STREAM_SAMPLE ? s : -1);
    check(dropped == 3 && got == DSD_PCM_STREAM_SAMPLE && s == new_sample, what);
    dsd_pcm_stream_close(stream);
    (void)close(fds[0]);
    (void)close(fds[1]);
}

/* A read of g_console_fd blocks, as an idle Windows console's _read() does once its readiness said it was ready, until
   the test releases it (or 60 s on the real clock, a deadlock safeguard only). */
static atomic_int g_console_fd = -1;
static atomic_int g_console_in_read;
static atomic_int g_console_release;

ssize_t
mark_read(int fd, void* buf, size_t count) {
    if (fd == atomic_load(&g_console_fd)) {
        atomic_store(&g_console_in_read, 1);
        const uint64_t start = mark_real_time_monotonic_ms();
        while (!atomic_load(&g_console_release) && mark_real_time_monotonic_ms() - start < 60000U) {
            dsd_sleep_ms(1U);
        }
    }
    return mark_real_read(fd, buf, count);
}

static DSD_THREAD_RETURN_TYPE
decoder_console_read(void* arg) {
    int16_t s = 0;
    (void)dsd_pcm_stream_read_wait((dsd_pcm_stream*)arg, &s, 0U);
    DSD_THREAD_RETURN;
}

/* The watchdog's mark, on a thread of its own. */
static atomic_int g_console_marked;
static dsd_input_cut g_console_cut;

static DSD_THREAD_RETURN_TYPE
watchdog_console_mark(void* arg) {
    (void)arg;
    dsd_input_backlog_mark(&g_console_cut);
    atomic_store(&g_console_marked, 1);
    DSD_THREAD_RETURN;
}

/* Issue #834: stdin that is no stream (a console, a regular file) holds no backlog a retune could drop, and a read of
   it may block while the reader holds its refill lock: a Windows console reads as ready at once, then its _read()
   waits for a key. A watchdog's mark of such an input must not wait on that read before it sends its tune: it counts
   nothing and takes no lock. The case checks the order, never a time: the read stays held until the mark has
   returned, and a mark that waited on it would never return while it is (30 s on the real clock end the wait, a
   deadlock safeguard only). */
static void
test_mark_never_waits_on_a_console_read(void) {
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-pcm-console");
    if (fd < 0) {
        check(0, "a temp file for the console");
        return;
    }
    const int16_t sample = 7;
    check(dsd_write(fd, &sample, sizeof sample) == (ssize_t)sizeof sample, "a sample in the file");
    (void)lseek(fd, 0, SEEK_SET);
    dsd_pcm_stream* stream = dsd_pcm_stream_open_fd(fd);
    check(stream != NULL, "open on the file");
    atomic_store(&g_console_in_read, 0);
    atomic_store(&g_console_release, 0);
    atomic_store(&g_console_fd, fd);
    dsd_thread_t decoder;
    if (stream == NULL || dsd_thread_create(&decoder, decoder_console_read, stream) != 0) {
        check(0, "the decoder thread for the console");
        atomic_store(&g_console_fd, -1);
        dsd_pcm_stream_close(stream);
        (void)close(fd);
        (void)remove(path);
        return;
    }
    while (!atomic_load(&g_console_in_read)) {
        dsd_sleep_ms(1U);
    }
    atomic_store(&g_console_marked, 0);
    DSD_MEMSET(&g_console_cut, 0, sizeof g_console_cut);
    dsd_thread_t watchdog;
    const int watchdog_started = dsd_thread_create(&watchdog, watchdog_console_mark, NULL) == 0;
    check(watchdog_started, "the watchdog thread for the console's mark");
    const uint64_t safeguard_start = mark_real_time_monotonic_ms();
    while (watchdog_started && !atomic_load(&g_console_marked)
           && mark_real_time_monotonic_ms() - safeguard_start < 30000U) {
        dsd_sleep_ms(1U);
    }
    /* Read before the release: the console's read is still held here. */
    const int marked_while_held = atomic_load(&g_console_marked) && !atomic_load(&g_console_release);
    atomic_store(&g_console_release, 1);
    if (watchdog_started) {
        (void)dsd_thread_join(watchdog);
    }
    (void)dsd_thread_join(decoder);
    atomic_store(&g_console_fd, -1);
    char what[160];
    DSD_SNPRINTF(what, sizeof what, "a mark of a console in a read returns while the read is held (counted %llu)",
                 (unsigned long long)g_console_cut.arrived);
    check(marked_while_held && g_console_cut.arrived == 0U, what);
    dsd_pcm_stream_close(stream);
    (void)close(fd);
    (void)remove(path);
}

/* Issue #834: a drop whose bytes are owed but not ready yet: a read waits for input for the time it was given, as a
   read with nothing owed does, rather than returning at once (a decoder loop would spin on an empty input). The bytes
   then come, and go before the new sample. */
static void
test_owed_drop_waits_for_input(void) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        check(0, "pipe for the owed drop");
        return;
    }
    dsd_pcm_stream* stream = dsd_pcm_stream_open_fd(fds[0]);
    check(stream != NULL, "open on the pipe for the owed drop");
    const int16_t old_samples[2] = {1, 2};
    check(dsd_write(fds[1], old_samples, sizeof old_samples) == (ssize_t)sizeof old_samples, "the old samples");
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    int16_t hidden[2];
    check(mark_real_read(fds[0], hidden, sizeof hidden) == (ssize_t)sizeof hidden, "the counted bytes not ready yet");
    check(dsd_pcm_stream_discard_through(stream, &cut) == 0, "nothing ready to drop yet");
    atomic_store(&g_wait_longest_ms, 0);
    atomic_store(&g_wait_fd, fds[0]);
    int16_t s = 0;
    check(dsd_pcm_stream_read_wait(stream, &s, 40U) == DSD_PCM_STREAM_TIMEOUT, "an empty input times out");
    check(atomic_load(&g_wait_longest_ms) == 40, "and the read waited the time it was given");
    atomic_store(&g_wait_fd, -1);
    check(dsd_write(fds[1], old_samples, sizeof old_samples) == (ssize_t)sizeof old_samples, "the owed bytes come");
    const int16_t new_sample = 9;
    check(dsd_write(fds[1], &new_sample, sizeof new_sample) == (ssize_t)sizeof new_sample, "the new sample");
    int got = DSD_PCM_STREAM_TIMEOUT;
    for (int i = 0; i < 8 && got == DSD_PCM_STREAM_TIMEOUT; i++) {
        got = dsd_pcm_stream_read_wait(stream, &s, 100U);
    }
    check(got == DSD_PCM_STREAM_SAMPLE && s == 9, "the owed bytes go; the first sample is the new one");
    dsd_pcm_stream_close(stream);
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);
}

int
main(void) {
    test_backlog_lock_that_fails_to_initialize();
    test_mark_never_waits_on_a_console_read();
    test_pipe_mark_during_refill();
    test_socket_mark_during_refill();
    test_pause_before_the_first_refill();
    test_owed_drop_waits_for_input();
    return g_failures == 0 ? 0 : 1;
}
