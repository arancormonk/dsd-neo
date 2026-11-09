// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_fopen_existing_regular_file() must refuse a FIFO with no writer, and dsd_fopen_private_append_regular() one with
 * no reader, at once instead of blocking in open(2). Each call runs in a child process so a regression fails the test
 * within seconds rather than hanging the suite. The appending open also refuses a FIFO something reads, a link and a
 * folder, and creates a missing file owner-only.
 */

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
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
run_open_in_child(const char* path, int append, int* code) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        errno = 0;
        FILE* fp = append ? dsd_fopen_private_append_regular(path) : dsd_fopen_existing_regular_file(path, "rb");
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

    int rc = 0;
    for (int append = 0; append <= 1; ++append) {
        int code = -1;
        if (run_open_in_child(fifo, append, &code) != 0) {
            DSD_FPRINTF(stderr, "FAIL: %s a FIFO nobody %s blocked past %u ms\n", append ? "appending to" : "opening",
                        append ? "reads" : "writes", OPEN_BUDGET_MS);
            rc = -1;
        } else if (code != CHILD_OK) {
            DSD_FPRINTF(stderr, "FAIL: %s FIFO open returned a stream or wrong errno (child code %d)\n",
                        append ? "appending" : "reading", code);
            rc = -1;
        }
    }

    /* A FIFO something reads opens for writing at once; the appending open still refuses it, untouched. */
    const int reader = open(fifo, O_RDONLY | O_NONBLOCK);
    assert(reader >= 0);
    errno = 0;
    assert(dsd_fopen_private_append_regular(fifo) == NULL && errno == EINVAL);
    char none = 0;
    assert(read(reader, &none, 1) == 0);
    (void)close(reader);
    struct stat st;
    assert(lstat(fifo, &st) == 0 && S_ISFIFO(st.st_mode));

    /* A missing file is created owner-only whatever the umask, and a link at the name is never followed. */
    char made[1200], link_path[1200];
    assert(dsd_test_path_join(made, sizeof made, dir, "made.bin") == 0);
    assert(dsd_test_path_join(link_path, sizeof link_path, dir, "link.bin") == 0);
    const mode_t umask_before = umask(022);
    FILE* a = dsd_fopen_private_append_regular(made);
    umask(umask_before);
    assert(a != NULL && fputs("x", a) >= 0 && fclose(a) == 0);
    assert(stat(made, &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_size == 1);
    a = dsd_fopen_private_append_regular(made);
    assert(a != NULL && (fcntl(fileno(a), F_GETFL) & O_NONBLOCK) == 0);
    (void)fclose(a);
    assert(symlink("made.bin", link_path) == 0);
    errno = 0;
    assert(dsd_fopen_private_append_regular(link_path) == NULL && (errno == ELOOP || errno == EMLINK)); // EMLINK: BSD
    assert(stat(made, &st) == 0 && st.st_size == 1);

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

    errno = 0;
    assert(dsd_fopen_private_append_regular(dir) == NULL && (errno == EISDIR || errno == EINVAL));

    (void)remove(fifo);
    (void)remove(reg);
    (void)remove(made);
    (void)remove(link_path);
    (void)dsd_test_rmdir(dir);
    return rc == 0 ? 0 : 1;
}
