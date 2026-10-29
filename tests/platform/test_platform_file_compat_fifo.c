// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_fopen_existing_regular_file() must refuse a FIFO with no writer at once instead of blocking in open(2).
 * The call runs in a child process so a regression fails the test within seconds rather than hanging the suite.
 */

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dsd-neo/core/safe_api.h"
#include "test_support.h"

#define OPEN_BUDGET_MS 2000u

enum {
    CHILD_OK = 0,
    CHILD_OPENED = 10,
    CHILD_BAD_ERRNO = 11,
};

/* Returns 0 when the child finished inside the budget; fills *code with its exit status. */
static int
run_open_in_child(const char* path, int* code) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        errno = 0;
        FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
        if (fp) {
            (void)fclose(fp);
            _exit(CHILD_OPENED);
        }
        _exit(errno == EINVAL ? CHILD_OK : CHILD_BAD_ERRNO);
    }

    const uint64_t start = dsd_realtime_mono_ms();
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            *code = WIFEXITED(status) ? WEXITSTATUS(status) : 100 + WTERMSIG(status);
            return 0;
        }
        assert(r == 0);
        if (dsd_realtime_mono_ms() - start > OPEN_BUDGET_MS) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            return -1;
        }
        usleep(5000);
    }
}

int
main(void) {
    char dir[1024];
    char fifo[1200];
    char reg[1200];

    assert(dsd_test_mkdtemp(dir, sizeof dir, "dsd_neo_fifo_open") != NULL);
    assert(dsd_test_path_join(fifo, sizeof fifo, dir, "pipe") == 0);
    assert(dsd_test_path_join(reg, sizeof reg, dir, "plain.bin") == 0);
    assert(mkfifo(fifo, 0600) == 0);

    int code = -1;
    int rc = run_open_in_child(fifo, &code);
    if (rc != 0) {
        fprintf(stderr, "FAIL: opening a writerless FIFO blocked past %u ms\n", OPEN_BUDGET_MS);
    } else if (code != CHILD_OK) {
        fprintf(stderr, "FAIL: FIFO open returned a stream or wrong errno (child code %d)\n", code);
        rc = -1;
    }

    /* A regular file still opens, reads back, and is left in blocking mode. */
    FILE* w = dsd_fopen_private(reg, "wb");
    assert(w != NULL);
    assert(fwrite("tetra", 1, 5, w) == 5);
    assert(fclose(w) == 0);
    FILE* r = dsd_fopen_existing_regular_file(reg, "rb");
    assert(r != NULL);
    char buf[8] = {0};
    assert(fread(buf, 1, 5, r) == 5);
    assert(memcmp(buf, "tetra", 5) == 0);
    assert((fcntl(fileno(r), F_GETFL) & O_NONBLOCK) == 0);
    (void)fclose(r);

    errno = 0;
    assert(dsd_fopen_existing_regular_file(dir, "rb") == NULL && errno == EINVAL);

    (void)remove(fifo);
    (void)remove(reg);
    (void)dsd_test_rmdir(dir);
    return rc == 0 ? 0 : 1;
}
