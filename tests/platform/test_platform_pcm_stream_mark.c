// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * Issue #834: a backlog mark taken on the stdin or TCP reader while the decoder refills it. The mark counts the bytes
 * the reader read in and those the pipe or socket holds ready; a refill moves bytes from the one to the other. The
 * readiness query is wrapped (GNU ld --wrap) so that the decoder thread tries its refill between the query and the
 * count: the mark must still count each byte once, and the drop that follows must keep every sample that came after
 * the mark.
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

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
long __real_dsd_fd_readable_bytes(int fd);
long __wrap_dsd_fd_readable_bytes(int fd);
long __real_dsd_socket_readable_bytes(dsd_socket_t sock);
long __wrap_dsd_socket_readable_bytes(dsd_socket_t sock);
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

static int g_failures = 0;

static void
check(int condition, const char* what) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
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
__wrap_dsd_fd_readable_bytes(int fd) {
    const long ready = __real_dsd_fd_readable_bytes(fd);
    refill_during_query();
    return ready;
}

long
__wrap_dsd_socket_readable_bytes(dsd_socket_t sock) {
    const long ready = __real_dsd_socket_readable_bytes(sock);
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

int
main(void) {
    test_pipe_mark_during_refill();
    test_socket_mark_during_refill();
    return g_failures == 0 ? 0 : 1;
}
