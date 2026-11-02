// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * The buffered PCM16LE reader the stdin and TCP inputs share (issue #667 D12), on a real pipe (POSIX pipe(), a Windows
 * anonymous pipe through PeekNamedPipe()), a real loopback socket and a regular file: a part of a sample is kept
 * across reads, buffered samples come back without a wait, an empty stream times out after the bound, the end and a
 * part-sample end are the end, and a socket whose receive timeout runs out is a timeout to the bounded read (B7) but
 * the end to the blocking one, as it always was.
 */
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
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
    check(dsd_pcm_stream_read(stream, &s) == 0, "the blocking read sees the end too");
    check(!dsd_pcm_stream_failed(stream), "an end is no failure");
    dsd_pcm_stream_close(stream);
    (void)dsd_close(fds[0]);
}

/* The blocking read on a pipe: every sample, then the end. */
static void
test_pipe_blocking(void) {
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
    check(dsd_pcm_stream_read(stream, &s) == 1 && s == 1, "blocking sample 1");
    check(dsd_pcm_stream_read(stream, &s) == 1 && s == -2, "blocking sample 2");
    check(dsd_pcm_stream_read(stream, &s) == 0, "blocking read: the end");
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

/* The blocking read keeps its contract (EDACS, the M17 encoder): a receive timeout ends it. */
static void
test_socket_blocking_read_times_out(void) {
    socket_pair pair;
    check(socket_pair_open(&pair) == 0, "loopback connection");
    (void)dsd_socket_set_recv_timeout(pair.client, 100U);
    dsd_pcm_stream* stream = dsd_pcm_stream_open_socket(pair.client);
    const uint8_t sample[] = {0x02U, 0x00U, 0x03U};
    check(dsd_socket_send(pair.server, sample, sizeof sample, 0) == (int)sizeof sample, "send");
    int16_t s = 0;
    check(dsd_pcm_stream_read(stream, &s) == 1 && s == 2, "blocking read on the socket");
    check(dsd_pcm_stream_read(stream, &s) == 0, "the receive timeout ends a blocking read");
    check(dsd_pcm_stream_failed(stream), "as a read error");
    check(dsd_pcm_stream_read_wait(stream, &s, 100U) == DSD_PCM_STREAM_END, "and the stream with it");
    dsd_pcm_stream_close(stream);
    socket_pair_close(&pair);
}

static void
test_no_stream(void) {
    int16_t s = 7;
    check(dsd_pcm_stream_open_fd(-1) == NULL, "no reader on a negative descriptor");
    check(dsd_pcm_stream_open_socket(DSD_INVALID_SOCKET) == NULL, "no reader on no socket");
    check(dsd_pcm_stream_read(NULL, &s) == 0 && s == 7, "a NULL blocking read");
    check(dsd_pcm_stream_read_wait(NULL, &s, 0U) == DSD_PCM_STREAM_END && s == 7, "a NULL bounded read");
    check(!dsd_pcm_stream_failed(NULL), "NULL never failed");
    dsd_pcm_stream_close(NULL);
}

int
main(void) {
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "socket init failed\n");
        return 1;
    }
    test_no_stream();
    test_pipe();
    test_pipe_blocking();
    test_regular_file();
    test_socket_idle_then_close();
    test_socket_blocking_read_times_out();
    dsd_socket_cleanup();
    if (g_failures != 0) {
        return 1;
    }
    printf("PLATFORM_PCM_STREAM: OK\n");
    return 0;
}
