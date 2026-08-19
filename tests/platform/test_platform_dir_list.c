// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/platform/platform.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#define DSD_TEST_RMDIR _rmdir
#else
#include <unistd.h>
#define DSD_TEST_RMDIR rmdir
#endif

#define DIR_LIST_SINK_MAX 8

typedef struct {
    char names[DIR_LIST_SINK_MAX][64];
    int count;
    int stop_after; /* 0 = never stop */
} DirListSink;

static int
sink_cb(const char* name, void* user) {
    DirListSink* s = (DirListSink*)user;
    if (s->count < DIR_LIST_SINK_MAX) {
        DSD_SNPRINTF(s->names[s->count], sizeof s->names[0], "%s", name);
    }
    s->count++;
    if (s->stop_after > 0 && s->count >= s->stop_after) {
        return 1;
    }
    return 0;
}

static int
sink_has(const DirListSink* s, const char* name) {
    for (int i = 0; i < s->count && i < DIR_LIST_SINK_MAX; i++) {
        if (strcmp(s->names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Creates <scratch>/a.txt, b.txt, c.txt. paths[] must be zeroed by the caller. */
static int
make_three_files(const char* scratch, char paths[3][DSD_TEST_PATH_MAX]) {
    static const char* const leaves[3] = {"a.txt", "b.txt", "c.txt"};
    int rc = 0;
    for (int i = 0; i < 3; i++) {
        if (dsd_test_path_join(paths[i], DSD_TEST_PATH_MAX, scratch, leaves[i]) != 0) {
            paths[i][0] = '\0';
            rc = 1;
            continue;
        }
        FILE* fp = dsd_fopen_private(paths[i], "w");
        if (!fp) {
            rc = 1;
            continue;
        }
        if (fputs("x\n", fp) < 0) {
            rc = 1;
        }
        if (fclose(fp) != 0) {
            rc = 1;
        }
    }
    return rc;
}

static void
remove_three_files(char paths[3][DSD_TEST_PATH_MAX]) {
    for (int i = 0; i < 3; i++) {
        if (paths[i][0] != '\0') {
            (void)remove(paths[i]);
        }
    }
}

static int
expect_lists_regular_files(void) {
    char scratch[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(scratch, sizeof scratch, "dsd_neo_dir_list") == NULL) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char paths[3][DSD_TEST_PATH_MAX];
    DSD_MEMSET(paths, 0, sizeof paths);
    int rc = make_three_files(scratch, paths);

    DirListSink sink;
    DSD_MEMSET(&sink, 0, sizeof sink);
    rc |= dsd_dir_list(scratch, sink_cb, &sink) == 0 ? 0 : 1;
    rc |= sink.count == 3 ? 0 : 1;
    rc |= sink_has(&sink, "a.txt") ? 0 : 1;
    rc |= sink_has(&sink, "b.txt") ? 0 : 1;
    rc |= sink_has(&sink, "c.txt") ? 0 : 1;
    rc |= sink_has(&sink, ".") ? 1 : 0;
    rc |= sink_has(&sink, "..") ? 1 : 0;

    remove_three_files(paths);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_callback_can_stop(void) {
    char scratch[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(scratch, sizeof scratch, "dsd_neo_dir_stop") == NULL) {
        return 1;
    }

    char paths[3][DSD_TEST_PATH_MAX];
    DSD_MEMSET(paths, 0, sizeof paths);
    int rc = make_three_files(scratch, paths);

    DirListSink sink;
    DSD_MEMSET(&sink, 0, sizeof sink);
    sink.stop_after = 1;
    rc |= dsd_dir_list(scratch, sink_cb, &sink) == 0 ? 0 : 1;
    rc |= sink.count == 1 ? 0 : 1;

    remove_three_files(paths);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_rejects_bad_arguments(void) {
    char scratch[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(scratch, sizeof scratch, "dsd_neo_dir_bad") == NULL) {
        return 1;
    }

    char missing[DSD_TEST_PATH_MAX];
    int rc = dsd_test_path_join(missing, sizeof missing, scratch, "no_such_dir") != 0 ? 1 : 0;

    DirListSink sink;
    DSD_MEMSET(&sink, 0, sizeof sink);
    rc |= dsd_dir_list(missing, sink_cb, &sink) == -1 ? 0 : 1;
    rc |= sink.count == 0 ? 0 : 1;

    errno = 0;
    rc |= dsd_dir_list(NULL, sink_cb, &sink) == -1 && errno == EINVAL ? 0 : 1;
    errno = 0;
    rc |= dsd_dir_list("", sink_cb, &sink) == -1 && errno == EINVAL ? 0 : 1;
    errno = 0;
    rc |= dsd_dir_list(scratch, NULL, &sink) == -1 && errno == EINVAL ? 0 : 1;

    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= expect_lists_regular_files();
    rc |= expect_callback_can_stop();
    rc |= expect_rejects_bad_arguments();
    return rc;
}
