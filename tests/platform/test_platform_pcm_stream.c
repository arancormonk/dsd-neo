// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * The buffered PCM16LE reader the stdin and TCP inputs share (issue #667 D12), on a real pipe (POSIX pipe(), a Windows
 * anonymous pipe through PeekNamedPipe()), a real loopback socket and a regular file: a part of a sample is kept
 * across reads, buffered samples come back without a wait, an empty stream times out after the bound, the end and a
 * part-sample end are the end, and a socket whose receive timeout runs out is a timeout, never the end (B7).
 */
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/pcm_stream.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <fcntl.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int g_failures = 0;

static void
check(int condition, const char* what) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static int
make_pipe(int fds[2]) {
#if DSD_PLATFORM_WIN_NATIVE
    return _pipe(fds, 65536U, _O_BINARY);
#else
    return pipe(fds);
#endif
}

static void
put_bytes(int fd, const uint8_t* bytes, size_t count) {
    check(dsd_write(fd, bytes, count) == (ssize_t)count, "write to the pipe");
}

/* The milliseconds a bounded read of @p timeout_ms took, and what it returned in @p got. */
static uint64_t
timed_read_wait(dsd_pcm_stream* stream, int16_t* out, unsigned int timeout_ms, int* got) {
    const uint64_t start = dsd_time_monotonic_ms();
    *got = dsd_pcm_stream_read_wait(stream, out, timeout_ms);
    return dsd_time_monotonic_ms() - start;
}

/* A pipe: a part of a sample is kept, buffered samples need no wait, an empty pipe times out after the bound, and the
   writer's close with half a sample left is the end. */
static void
test_pipe(void) {
    int fds[2] = {-1, -1};
    if (make_pipe(fds) != 0) {
        check(0, "pipe");
        return;
    }
    dsd_pcm_stream* stream = dsd_pcm_stream_open_fd(fds[0]);
    check(stream != NULL, "open on the pipe");
    int16_t s = 0;
    int got = 0;

    const uint8_t first[] = {0x34U, 0x12U, 0x00U}; /* 0x1234, then the low byte of 0x8000 */
    put_bytes(fds[1], first, sizeof first);
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 0x1234, "a whole sample");
    const uint64_t partial_ms = timed_read_wait(stream, &s, 150U, &got);
    check(got == DSD_PCM_STREAM_TIMEOUT, "half a sample is no sample");
    check(partial_ms >= 100U && partial_ms < 2000U, "the half-sample wait ran to its bound");

    const uint8_t rest[] = {0x80U, 0xFFU, 0x7FU, 0x01U, 0x00U}; /* its high byte, 0x7FFF, 0x0001 */
    put_bytes(fds[1], rest, sizeof rest);
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == INT16_MIN,
          "the kept byte completes its sample");
    /* Buffered: the same refill brought them, so they come back with no wait at all. */
    check(dsd_pcm_stream_read_wait(stream, &s, 0U) == DSD_PCM_STREAM_SAMPLE && s == INT16_MAX, "buffered sample 1");
    check(dsd_pcm_stream_read_wait(stream, &s, 0U) == DSD_PCM_STREAM_SAMPLE && s == 1, "buffered sample 2");

    const uint64_t empty_ms = timed_read_wait(stream, &s, 200U, &got);
    check(got == DSD_PCM_STREAM_TIMEOUT, "an empty pipe times out");
    check(empty_ms >= 150U && empty_ms < 2000U, "the empty wait ran to its bound");

    const uint8_t half[] = {0x55U};
    put_bytes(fds[1], half, sizeof half);
    (void)dsd_close(fds[1]);
    /* One refill per wait: the half sample may come in one, the end in the next. */
    got = dsd_pcm_stream_read_wait(stream, &s, 1000U);
    if (got == DSD_PCM_STREAM_TIMEOUT) {
        got = dsd_pcm_stream_read_wait(stream, &s, 1000U);
    }
    check(got == DSD_PCM_STREAM_END, "the end, half a sample left");
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_END, "the end stays");
    check(!dsd_pcm_stream_failed(stream), "an end is no failure");
    dsd_pcm_stream_close(stream);
    (void)dsd_close(fds[0]);
}

/* A writer that sends and closes at once: every sample, then the end. */
static void
test_pipe_samples_then_end(void) {
    int fds[2] = {-1, -1};
    if (make_pipe(fds) != 0) {
        check(0, "pipe");
        return;
    }
    dsd_pcm_stream* stream = dsd_pcm_stream_open_fd(fds[0]);
    const uint8_t bytes[] = {0x01U, 0x00U, 0xFEU, 0xFFU};
    put_bytes(fds[1], bytes, sizeof bytes);
    (void)dsd_close(fds[1]);
    int16_t s = 0;
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 1, "sample 1");
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == -2, "sample 2");
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_END, "then the end");
    dsd_pcm_stream_close(stream);
    (void)dsd_close(fds[0]);
}

/* A regular file: ready at once on every platform, its samples, then the end. */
static void
test_regular_file(void) {
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_pcm_stream");
    check(fd >= 0, "temporary file");
    const uint8_t bytes[] = {0x10U, 0x27U, 0xF0U, 0xD8U}; /* 10000, -10000 */
    put_bytes(fd, bytes, sizeof bytes);
    (void)dsd_close(fd);
    FILE* file = dsd_fopen_existing_regular_file(path, "rb");
    check(file != NULL, "reopen the file");
    dsd_pcm_stream* stream = file ? dsd_pcm_stream_open_fd(dsd_fileno(file)) : NULL;
    int16_t s = 0;
    check(dsd_pcm_stream_read_wait(stream, &s, 100U) == DSD_PCM_STREAM_SAMPLE && s == 10000, "file sample 1");
    check(dsd_pcm_stream_read_wait(stream, &s, 100U) == DSD_PCM_STREAM_SAMPLE && s == -10000, "file sample 2");
    check(dsd_pcm_stream_read_wait(stream, &s, 100U) == DSD_PCM_STREAM_END, "file: the end");
    dsd_pcm_stream_close(stream);
    if (file) {
        (void)fclose(file);
    }
    (void)remove(path);
}

/* --- Sockets: a loopback connection, accepted on this thread. --- */

typedef struct {
    dsd_socket_t listener;
    dsd_socket_t client; /* what the reader reads */
    dsd_socket_t server; /* what the test writes to */
} socket_pair;

static int
socket_pair_open(socket_pair* pair) {
    pair->listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    pair->client = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    pair->server = DSD_INVALID_SOCKET;
    if (pair->listener == DSD_INVALID_SOCKET || pair->client == DSD_INVALID_SOCKET) {
        return -1;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct sockaddr_in bound;
    DSD_MEMSET(&bound, 0, sizeof bound);
#if DSD_PLATFORM_WIN_NATIVE
    int bound_len = (int)sizeof bound;
#else
    socklen_t bound_len = (socklen_t)sizeof bound;
#endif
    if (dsd_socket_bind(pair->listener, (struct sockaddr*)&addr, sizeof addr) != 0
        || getsockname(pair->listener, (struct sockaddr*)&bound, &bound_len) != 0
        || dsd_socket_listen(pair->listener, 1) != 0
        || dsd_socket_connect(pair->client, (struct sockaddr*)&bound, sizeof bound) != 0) {
        return -1;
    }
    pair->server = dsd_socket_accept(pair->listener, NULL, NULL);
    return pair->server == DSD_INVALID_SOCKET ? -1 : 0;
}

static void
socket_pair_close(const socket_pair* pair) {
    if (pair->server != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(pair->server);
    }
    (void)dsd_socket_close(pair->client);
    (void)dsd_socket_close(pair->listener);
}

/* B7: a quiet sender past the socket's receive timeout is a timeout to the bounded read, over and over, and the
   connection's next sample is read on it; its close is the end. */
static void
test_socket_idle_then_close(void) {
    socket_pair pair;
    check(socket_pair_open(&pair) == 0, "loopback connection");
    (void)dsd_socket_set_recv_timeout(pair.client, 100U);
    dsd_pcm_stream* stream = dsd_pcm_stream_open_socket(pair.client);
    check(stream != NULL, "open on the socket");
    int16_t s = 0;
    const uint64_t start = dsd_time_monotonic_ms();
    int timeouts = 0;
    for (int i = 0; i < 5; i++) {
        timeouts += dsd_pcm_stream_read_wait(stream, &s, 100U) == DSD_PCM_STREAM_TIMEOUT;
    }
    check(timeouts == 5, "a quiet sender times out, the stream goes on");
    check(dsd_time_monotonic_ms() - start >= 400U, "quiet for four receive timeouts");
    const uint8_t sample[] = {0xE8U, 0x03U}; /* 1000 */
    check(dsd_socket_send(pair.server, sample, sizeof sample, 0) == (int)sizeof sample, "send");
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 1000, "the sample after it");
    (void)dsd_socket_close(pair.server);
    pair.server = DSD_INVALID_SOCKET;
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_END, "the close is the end");
    dsd_pcm_stream_close(stream);
    socket_pair_close(&pair);
}

/* Wait until @p sock holds @p bytes ready to read. The bytes were sent already: the bound (10 s) only turns a lost send
   into a failure, never a slow host. */
static int
socket_holds(dsd_socket_t sock, long bytes) {
    for (int tries = 0; tries < 1000; tries++) {
        if (dsd_socket_readable_bytes(sock) >= bytes) {
            return 1;
        }
        dsd_sleep_ms(10U);
    }
    return 0;
}

/* Issue #834: a discard drops the whole samples the reader held at the mark and those the socket had ready then,
   without a wait, keeps a part of a sample so the samples after it stay whole, and keeps what arrived after the mark,
   before the discard or after it. */
static void
test_socket_discard_through(void) {
    socket_pair pair;
    check(socket_pair_open(&pair) == 0, "loopback connection for the discard");
    dsd_pcm_stream* stream = dsd_pcm_stream_open_socket(pair.client);
    check(stream != NULL, "open on the socket for the discard");
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    check(cut.measured == 1U && cut.arrived == 0U, "the stream is the input marked on, holding nothing");
    check(dsd_pcm_stream_discard_through(NULL, &cut) == 0, "a NULL discard drops nothing");
    /* Nothing held at the mark: nothing dropped, and no read made. A discard that waited would block on the idle socket
       for good (it has no receive timeout), so the test would not end: no bound on the clock decides it. */
    check(dsd_pcm_stream_discard_through(stream, &cut) == 0, "nothing held at the mark: nothing dropped");
    /* 1, 2, 3, then the low byte of 0x0504: one sample the reader buffers, two and a half ready on the socket. */
    const uint8_t before[] = {0x01U, 0x00U, 0x02U, 0x00U, 0x03U, 0x00U, 0x04U};
    check(dsd_socket_send(pair.server, before, 2U, 0) == 2, "send the first sample");
    int16_t s = 0;
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 1, "the first sample");
    check(dsd_socket_send(pair.server, before + 2U, sizeof before - 2U, 0) == (int)(sizeof before - 2U), "send");
    check(socket_holds(pair.client, (long)(sizeof before - 2U)), "the bytes arrived");
    dsd_input_backlog_mark(&cut);
    check(cut.measured == 1U && cut.arrived == sizeof before, "the mark counts the bytes read in and ready");
    /* 0x0605 arrives after the mark, before the discard: the new channel's. */
    const uint8_t late[] = {0x05U, 0x06U};
    check(dsd_socket_send(pair.server, late, sizeof late, 0) == (int)sizeof late, "send after the mark");
    check(socket_holds(pair.client, (long)(sizeof before - 2U + sizeof late)), "it arrived");
    check(dsd_pcm_stream_discard_through(stream, &cut) == 2, "the two whole samples held at the mark go");
    const uint8_t after[] = {0x00U};
    check(dsd_socket_send(pair.server, after, sizeof after, 0) == (int)sizeof after, "send after the discard");
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 0x0504,
          "the part of a sample is kept: the next sample is whole");
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 6,
          "and what came after the mark is kept");
    check(dsd_pcm_stream_discard_through(stream, &cut) == 0, "a second discard on the same mark drops nothing");
    dsd_pcm_stream_close(stream);
    check(dsd_input_backlog_cut_applies(&cut, stream) == 0, "a closed stream is no input to drop on");
    socket_pair_close(&pair);
}

/* Issue #834: stdin from another program (a pipe) drops what it held at the mark; a regular file, which nothing feeds,
   drops nothing; and a mark made on another input drops nothing of this one. */
static void
test_fd_discard_through(void) {
    int fds[2] = {-1, -1};
    if (make_pipe(fds) != 0) {
        check(0, "pipe for the discard");
        return;
    }
    dsd_pcm_stream* stream = dsd_pcm_stream_open_fd(fds[0]);
    check(stream != NULL, "open on the pipe for the discard");
    const uint8_t held[] = {0x01U, 0x00U, 0x02U, 0x00U, 0x03U};
    put_bytes(fds[1], held, sizeof held);
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    check(cut.measured == 1U && cut.arrived == sizeof held, "the mark counts what the pipe holds");
    check(dsd_pcm_stream_discard_through(stream, &cut) == 2, "a pipe drops its two whole samples");
    const uint8_t after[] = {0x00U, 0x07U, 0x00U};
    put_bytes(fds[1], after, sizeof after);
    int16_t s = 0;
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 3, "the part sample is kept");
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 7, "and what came after");
    /* Another input attached since: its mark is no mark of this one. */
    int other[2] = {-1, -1};
    check(make_pipe(other) == 0, "a second pipe");
    dsd_pcm_stream* other_stream = dsd_pcm_stream_open_fd(other[0]);
    const uint8_t more[] = {0x08U, 0x00U};
    put_bytes(fds[1], more, sizeof more);
    dsd_input_backlog_mark(&cut);
    check(dsd_pcm_stream_discard_through(stream, &cut) == 0, "a mark on another input drops nothing of this one");
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 8, "it reads on");
    dsd_pcm_stream_close(other_stream);
    (void)dsd_close(other[0]);
    (void)dsd_close(other[1]);
    dsd_pcm_stream_close(stream);
    (void)dsd_close(fds[0]);
    (void)dsd_close(fds[1]);

    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsd-neo-pcm-discard");
    check(fd >= 0, "temp file for the discard");
    if (fd < 0) {
        return;
    }
    const uint8_t file_bytes[] = {0x05U, 0x00U, 0x06U, 0x00U};
    put_bytes(fd, file_bytes, sizeof file_bytes);
    (void)dsd_close(fd);
    FILE* file = dsd_fopen_existing_regular_file(path, "rb");
    check(file != NULL, "reopen the file for the discard");
    dsd_pcm_stream* file_stream = file ? dsd_pcm_stream_open_fd(dsd_fileno(file)) : NULL;
    dsd_input_backlog_mark(&cut);
    check(dsd_pcm_stream_discard_through(file_stream, &cut) == 0, "a regular file drops nothing");
    check(dsd_pcm_stream_read_wait(file_stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 5, "it reads on");
    dsd_pcm_stream_close(file_stream);
    if (file) {
        (void)fclose(file);
    }
    (void)remove(path);
}

/* A retune confirmed off the decoder thread the moment it was asked for: its mark, posted at once. */
static void
post_now(void) {
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    dsd_input_backlog_post(&cut);
}

/* Issue #834: a retune posted while no input is open, and none is being opened (the backoff before a TCP reconnect),
   moved the peer before the next input began: that input's first samples arrived after it, so it is not measured on
   that input and its drop never runs. */
static void
test_post_before_the_input_opens(void) {
    dsd_input_backlog_retire();
    post_now();
    socket_pair pair;
    check(socket_pair_open(&pair) == 0, "loopback connection for the early post");
    const uint8_t fresh[] = {0x65U, 0x00U, 0x66U, 0x00U};
    check(dsd_socket_send(pair.server, fresh, sizeof fresh, 0) == (int)sizeof fresh, "send the new channel's audio");
    check(socket_holds(pair.client, (long)sizeof fresh), "it arrived");
    dsd_pcm_stream* stream = dsd_pcm_stream_open_socket(pair.client);
    check(stream != NULL, "open on the socket after the early post");
    dsd_input_cut cut;
    DSD_MEMSET(&cut, 0, sizeof cut);
    const int held = dsd_input_backlog_take(&cut);
    char what[160];
    DSD_SNPRINTF(what, sizeof what, "a post made before the input began is not its drop (held %d, measured %u, %llu)",
                 held, (unsigned)cut.measured, (unsigned long long)cut.arrived);
    check(!held, what);
    int16_t s = 0;
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 0x65,
          "its first sample is read");
    dsd_pcm_stream_close(stream);
    socket_pair_close(&pair);
}

/* Issue #834: once a replacement input begins opening (its connection made), a retune posted before its reader
   attaches may find the old channel's audio on it: it is measured on the connection when posted, and stands on the
   reader that attaches. One posted before the opening began is retired by it, and an opening no reader took (a failed
   open, then dsd_input_backlog_forget()) leaves no post for the next input. */
static void
test_post_while_the_input_opens(void) {
    socket_pair pair;
    check(socket_pair_open(&pair) == 0, "loopback connection for the opening");
    const uint8_t old_audio[] = {0x01U, 0x00U, 0x02U, 0x00U};
    check(dsd_socket_send(pair.server, old_audio, sizeof old_audio, 0) == (int)sizeof old_audio, "send old audio");
    check(socket_holds(pair.client, (long)sizeof old_audio), "it arrived");

    dsd_input_backlog_retire();
    post_now();
    dsd_input_backlog_opening(&pair.client, dsd_pcm_stream_socket_arrived);
    check(dsd_input_backlog_take(NULL) == 0, "a post made before the opening began is retired by it");
    post_now();
    dsd_input_backlog_forget();
    check(dsd_input_backlog_take(NULL) == 0, "an opening that ended unopened leaves no post");
    dsd_input_cut cut;
    dsd_input_backlog_mark(&cut);
    check(cut.measured == 0U, "and its connection is no input any more");

    dsd_input_backlog_opening(&pair.client, dsd_pcm_stream_socket_arrived);
    post_now();
    dsd_pcm_stream* stream = dsd_pcm_stream_open_socket(pair.client);
    check(stream != NULL, "open on the socket being opened");
    dsd_input_backlog_forget();
    DSD_MEMSET(&cut, 0, sizeof cut);
    check(dsd_input_backlog_take(&cut) == 1 && cut.measured == 1U && cut.arrived == sizeof old_audio
              && dsd_input_backlog_cut_applies(&cut, stream),
          "a post made while it opened stands on its reader and survives the replacement");
    const uint8_t fresh[] = {0x03U, 0x00U};
    check(dsd_socket_send(pair.server, fresh, sizeof fresh, 0) == (int)sizeof fresh, "send new audio");
    check(socket_holds(pair.client, (long)(sizeof old_audio + sizeof fresh)), "it arrived too");
    check(dsd_pcm_stream_discard_through(stream, &cut) == 2, "the drop takes the old audio");
    int16_t s = 0;
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 3, "and keeps the new");
    dsd_pcm_stream_close(stream);
    socket_pair_close(&pair);
}

/* Issue #834: a retune posted while a replacement input opens is measured when it is posted, on the connection that
   exists: audio that reaches the connection after the post, before its reader attaches, is the new channel's and stays.
   One old sample is queued, the post is made, one new sample arrives, then the reader attaches: the drop takes the old
   sample only. */
static void
test_post_while_the_input_opens_is_measured_when_posted(void) {
    dsd_input_backlog_retire();
    socket_pair pair;
    check(socket_pair_open(&pair) == 0, "loopback connection for the measured opening");
    const uint8_t old_audio[] = {0x01U, 0x00U};
    check(dsd_socket_send(pair.server, old_audio, sizeof old_audio, 0) == (int)sizeof old_audio, "send old audio");
    check(socket_holds(pair.client, (long)sizeof old_audio), "it arrived");
    dsd_input_backlog_opening(&pair.client, dsd_pcm_stream_socket_arrived);
    post_now();
    const uint8_t fresh[] = {0x03U, 0x00U};
    check(dsd_socket_send(pair.server, fresh, sizeof fresh, 0) == (int)sizeof fresh, "send new audio");
    check(socket_holds(pair.client, (long)(sizeof old_audio + sizeof fresh)), "it arrived before the reader");
    dsd_pcm_stream* stream = dsd_pcm_stream_open_socket(pair.client);
    check(stream != NULL, "open on the socket being opened");
    dsd_input_backlog_forget();
    dsd_input_cut cut;
    DSD_MEMSET(&cut, 0, sizeof cut);
    const int held = dsd_input_backlog_take(&cut);
    char what[192];
    DSD_SNPRINTF(what, sizeof what,
                 "the post counts what the connection held when it was posted (held %d, measured %u, %llu bytes, "
                 "expected 2)",
                 held, (unsigned)cut.measured, (unsigned long long)cut.arrived);
    check(held == 1 && cut.measured == 1U && cut.arrived == sizeof old_audio, what);
    check(dsd_pcm_stream_discard_through(stream, &cut) == 1, "the drop takes the old sample only");
    int16_t s = 0;
    check(dsd_pcm_stream_read_wait(stream, &s, 1000U) == DSD_PCM_STREAM_SAMPLE && s == 3,
          "and the new sample that came before the reader is read");
    dsd_pcm_stream_close(stream);
    socket_pair_close(&pair);
}

static void
test_no_stream(void) {
    int16_t s = 7;
    check(dsd_pcm_stream_open_fd(-1) == NULL, "no reader on a negative descriptor");
    check(dsd_pcm_stream_open_socket(DSD_INVALID_SOCKET) == NULL, "no reader on no socket");
    check(dsd_pcm_stream_read_wait(NULL, &s, 0U) == DSD_PCM_STREAM_END && s == 7, "a NULL bounded read");
    check(!dsd_pcm_stream_failed(NULL), "NULL never failed");
    dsd_pcm_stream_close(NULL);
}

/* Issue #834: each post carries the mark its retune took before it was asked for, so two watchdog retunes may post in
   either order: the later mark is held, as its drop takes at least as much. */
static void
test_later_mark_is_held(void) {
    dsd_input_backlog_retire();
    dsd_input_cut early;
    dsd_input_cut late;
    dsd_input_backlog_mark(&early);
    dsd_input_backlog_mark(&late);
    dsd_input_backlog_post(&late);
    dsd_input_backlog_post(&early);
    dsd_input_cut held;
    DSD_MEMSET(&held, 0, sizeof held);
    check(dsd_input_backlog_take(&held) == 1 && held.order == late.order, "the later mark is held, posted first");
    dsd_input_backlog_post(&early);
    dsd_input_backlog_post(&late);
    DSD_MEMSET(&held, 0, sizeof held);
    check(dsd_input_backlog_take(&held) == 1 && held.order == late.order, "and posted last");
}

int
main(void) {
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "socket init failed\n");
        return 1;
    }
    test_no_stream();
    test_pipe();
    test_pipe_samples_then_end();
    test_regular_file();
    test_socket_idle_then_close();
    test_socket_discard_through();
    test_fd_discard_through();
    test_post_before_the_input_opens();
    test_post_while_the_input_opens();
    test_post_while_the_input_opens_is_measured_when_posted();
    test_later_mark_is_held();
    dsd_socket_cleanup();
    if (g_failures != 0) {
        return 1;
    }
    printf("PLATFORM_PCM_STREAM: OK\n");
    return 0;
}
