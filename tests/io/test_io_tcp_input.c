// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: TCP PCM input must preserve the caller-owned descriptor,
 * read raw little-endian PCM16 samples, and invalidate itself at EOF.
 */

#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/platform/platform.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/sockets.h"

#if !DSD_PLATFORM_WIN_NATIVE
#include <stdlib.h>
#include <unistd.h>
#endif

#if !DSD_PLATFORM_WIN_NATIVE
static int
write_all(int fd, const uint8_t* bytes, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, bytes + off, len - off);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int
create_pcm_fixture(void) {
    char path[] = "/tmp/dsd-neo-tcp-input-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    (void)unlink(path);

    const uint8_t pcm[] = {
        0x34u, 0x12u, /*  4660 */
        0x00u, 0x80u, /* -32768 */
        0xFFu, 0x7Fu, /*  32767 */
    };
    if (write_all(fd, pcm, sizeof(pcm)) != 0 || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif

int
main(void) {
    if (tcp_input_open(DSD_INVALID_SOCKET, 48000) != NULL) {
        DSD_FPRINTF(stderr, "tcp_input_open accepted an invalid descriptor\n");
        return 1;
    }
    if (tcp_input_is_valid(NULL) != 0) {
        DSD_FPRINTF(stderr, "NULL context reported valid\n");
        return 1;
    }
    if (tcp_input_get_socket(NULL) != DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "NULL context returned a real descriptor\n");
        return 1;
    }
    int16_t sample = 123;
    if (tcp_input_read_sample(NULL, &sample) != 0 || sample != 123) {
        DSD_FPRINTF(stderr, "NULL read changed output or reported success\n");
        return 1;
    }

#if DSD_PLATFORM_WIN_NATIVE
    DSD_FPRINTF(stderr, "tcp_input POSIX raw descriptor test skipped on native Windows\n");
    return 0;
#else
    int fd = create_pcm_fixture();
    if (fd < 0) {
        DSD_FPRINTF(stderr, "failed to create PCM fixture\n");
        return 1;
    }

    tcp_input_ctx* ctx = tcp_input_open((dsd_socket_t)fd, 48000);
    if (!ctx) {
        DSD_FPRINTF(stderr, "tcp_input_open failed for raw PCM descriptor\n");
        close(fd);
        return 1;
    }
    if (!tcp_input_is_valid(ctx)) {
        DSD_FPRINTF(stderr, "new TCP input context was not valid\n");
        tcp_input_close(ctx);
        close(fd);
        return 1;
    }
    if (tcp_input_get_socket(ctx) != (dsd_socket_t)fd) {
        DSD_FPRINTF(stderr, "TCP input did not preserve caller descriptor\n");
        tcp_input_close(ctx);
        close(fd);
        return 1;
    }

    const int16_t expected[] = {4660, INT16_MIN, INT16_MAX};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        sample = 0;
        if (!tcp_input_read_sample(ctx, &sample)) {
            DSD_FPRINTF(stderr, "read_sample failed at sample %zu\n", i);
            tcp_input_close(ctx);
            close(fd);
            return 1;
        }
        if (sample != expected[i]) {
            DSD_FPRINTF(stderr, "sample %zu mismatch: got %d expected %d\n", i, (int)sample, (int)expected[i]);
            tcp_input_close(ctx);
            close(fd);
            return 1;
        }
    }

    sample = 321;
    if (tcp_input_read_sample(ctx, &sample) != 0) {
        DSD_FPRINTF(stderr, "EOF read unexpectedly succeeded\n");
        tcp_input_close(ctx);
        close(fd);
        return 1;
    }
    if (tcp_input_is_valid(ctx) != 0) {
        DSD_FPRINTF(stderr, "EOF did not invalidate TCP input context\n");
        tcp_input_close(ctx);
        close(fd);
        return 1;
    }
    if (tcp_input_read_sample(ctx, &sample) != 0) {
        DSD_FPRINTF(stderr, "invalidated context read unexpectedly succeeded\n");
        tcp_input_close(ctx);
        close(fd);
        return 1;
    }

    tcp_input_close(ctx);
    if (lseek(fd, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "tcp_input_close closed the caller-owned descriptor\n");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
#endif
}
