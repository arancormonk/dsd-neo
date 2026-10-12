// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"
#include "file_compat_internal.h"
#include "test_support.h"
#if DSD_PLATFORM_WIN_NATIVE
#include <locale.h>
#else
#include <unistd.h>
#endif

static int
expect_posix_compat_wrappers(void) {
    int rc = 0;

    errno = 0;
    rc |= dsd_open_serial_write(NULL) == -1 && errno == EINVAL ? 0 : 1;
    errno = 0;
    rc |= dsd_open_serial_write("") == -1 && errno == EINVAL ? 0 : 1;

    const char* name = "dsd_neo_serial_open_write_test.tmp";
    (void)remove(name);
    FILE* fp = dsd_fopen_private(name, "w");
    if (!fp) {
        return 1;
    }
    rc |= fclose(fp) == 0 ? 0 : 1;
    errno = 0;
    int fd = dsd_open_serial_write(name);
#if DSD_PLATFORM_WIN_NATIVE
    /* Serial control is not supported on Windows: any valid path is refused as such. */
    rc |= fd == -1 && errno == ENOSYS ? 0 : 1;
#else
    if (fd < 0) {
        rc = 1;
    } else {
        rc |= dsd_close(fd) == 0 ? 0 : 1;
    }
#endif
    (void)remove(name);

    void* ptr = dsd_aligned_alloc(sizeof(void*), 64);
    rc |= ptr != NULL ? 0 : 1;
    dsd_aligned_free(ptr);

    char file_tmpl[] = "dsd_neo_mkstemp_test_XXXXXX";
    fd = dsd_mkstemp(file_tmpl);
    if (fd < 0) {
        rc = 1;
    } else {
        rc |= dsd_close(fd) == 0 ? 0 : 1;
        rc |= strstr(file_tmpl, "XXXXXX") == NULL ? 0 : 1;
        (void)remove(file_tmpl);
    }

    char dir_tmpl[] = "dsd_neo_mkdtemp_test_XXXXXX";
    char* dir = dsd_mkdtemp(dir_tmpl);
    if (!dir) {
        rc = 1;
    } else {
        dsd_stat_t st;
        rc |= dsd_stat_path(dir, &st) == 0 && !dsd_stat_is_regular(&st) ? 0 : 1;
        rc |= dsd_test_rmdir(dir) == 0 ? 0 : 1;
    }

    return rc;
}

/* Temp names must be unpredictable and never reused while a file holds them, as POSIX mkstemp() gives. Windows'
 * _mktemp_s() gave one letter plus the process ID: 26 names per template, and a name back as soon as its file was
 * removed or renamed, so a second temp file renamed to the same target collided. */
enum { kManyTempNames = 40 };

static int
expect_many_unique_temp_files(void) {
    int rc = 0;
    static char names[kManyTempNames][64];
    int made = 0;
    for (int i = 0; i < kManyTempNames; i++) {
        DSD_SNPRINTF(names[i], sizeof names[i], "%s", "dsd_neo_mkstemp_many_XXXXXX");
        int fd = dsd_mkstemp(names[i]);
        if (fd < 0) {
            rc = 1;
            break;
        }
        rc |= dsd_close(fd) == 0 ? 0 : 1;
        made++;
    }
    for (int i = 0; i < made; i++) {
        for (int j = i + 1; j < made; j++) {
            rc |= strcmp(names[i], names[j]) != 0 ? 0 : 1;
        }
    }
    for (int i = 0; i < made; i++) {
        (void)remove(names[i]);
    }

    made = 0;
    for (int i = 0; i < kManyTempNames; i++) {
        DSD_SNPRINTF(names[i], sizeof names[i], "%s", "dsd_neo_mkdtemp_many_XXXXXX");
        if (!dsd_mkdtemp(names[i])) {
            rc = 1;
            break;
        }
        made++;
    }
    for (int i = 0; i < made; i++) {
        rc |= dsd_test_rmdir(names[i]) == 0 ? 0 : 1;
    }

    static FILE* streams[kManyTempNames];
    static char tmp_names[kManyTempNames][96];
    made = 0;
    for (int i = 0; i < kManyTempNames; i++) {
        streams[i] =
            dsd_fopen_private_temp_for_replace("dsd_neo_replace_many.csv", tmp_names[i], sizeof tmp_names[i], "w");
        if (!streams[i]) {
            rc = 1;
            break;
        }
        made++;
    }
    for (int i = 0; i < made; i++) {
        rc |= fclose(streams[i]) == 0 ? 0 : 1;
        (void)remove(tmp_names[i]);
    }
    return rc;
}

static int
expect_temp_name_not_reused_after_rename(void) {
    int rc = 0;
    char renamed[2][80] = {{0}};
    for (int round = 0; round < 2; round++) {
        char tmpl[64] = "dsd_neo_mkstemp_rename_XXXXXX";
        int fd = dsd_mkstemp(tmpl);
        if (fd < 0) {
            rc = 1;
            continue;
        }
        rc |= dsd_close(fd) == 0 ? 0 : 1;
        DSD_SNPRINTF(renamed[round], sizeof renamed[round], "%s.ini", tmpl);
        if (rename(tmpl, renamed[round]) != 0) {
            rc = 1;
            (void)remove(tmpl);
            renamed[round][0] = '\0';
        }
    }
    for (int round = 0; round < 2; round++) {
        if (renamed[round][0] != '\0') {
            (void)remove(renamed[round]);
        }
    }
    return rc;
}

/* -1 is the usual "no descriptor" value. POSIX answers it with EBADF; the Windows CRT treats it as a caller bug and ends
 * the process from its invalid-parameter handler, so the wrappers must answer it before the CRT sees it. */
static int
expect_negative_descriptor_wrappers(void) {
    int rc = 0;
    char buf[1] = {0};
    dsd_stat_t st;

    errno = 0;
    rc |= dsd_isatty(-1) == 0 && errno == EBADF ? 0 : 1;
    errno = 0;
    rc |= dsd_set_binary_mode(-1) == -1 && errno == EBADF ? 0 : 1;
    errno = 0;
    rc |= dsd_dup(-1) == -1 && errno == EBADF ? 0 : 1;
    errno = 0;
    rc |= dsd_dup2(-1, 0) == -1 && errno == EBADF ? 0 : 1;
    errno = 0;
    rc |= dsd_dup2(0, -1) == -1 && errno == EBADF ? 0 : 1;
    errno = 0;
    rc |= dsd_close(-1) == -1 && errno == EBADF ? 0 : 1;
    errno = 0;
    rc |= dsd_fsync(-1) == -1 && errno == EBADF ? 0 : 1;
    errno = 0;
    rc |= dsd_fstat(-1, &st) == -1 && errno == EBADF ? 0 : 1;
    errno = 0;
    rc |= dsd_read(-1, buf, sizeof(buf)) == -1 && errno == EBADF ? 0 : 1;
    errno = 0;
    rc |= dsd_write(-1, buf, sizeof(buf)) == -1 && errno == EBADF ? 0 : 1;
    return rc;
}

static int
expect_descriptor_wrappers(void) {
    const char* name = "dsd_neo_descriptor_wrappers_test.tmp";
    (void)remove(name);

    FILE* fp = dsd_fopen_private(name, "w+");
    if (!fp) {
        return 1;
    }
    int fd = dsd_fileno(fp);
    int ok = fd >= 0 && dsd_fileno(NULL) == -1 && dsd_isatty(-1) == 0;
    if (dsd_write(fd, "abc", 3) != 3 || dsd_fsync(fd) != 0) {
        ok = 0;
    }

    dsd_stat_t st;
    if (dsd_fstat(fd, &st) != 0 || !dsd_stat_is_regular(&st)) {
        ok = 0;
    }
    if (dsd_fchmod(fd, 0600) != 0) {
        ok = 0;
    }

    int dupfd = dsd_dup(fd);
    if (dupfd < 0) {
        ok = 0;
    } else {
        int target_fd = dsd_dup2(dupfd, dupfd);
        if (target_fd != dupfd) {
            ok = 0;
        }
        if (dsd_close(dupfd) != 0) {
            ok = 0;
        }
    }

    rewind(fp);
    char buf[4] = {0};
    if (dsd_read(fd, buf, 3) != 3 || strcmp(buf, "abc") != 0) {
        ok = 0;
    }
    if (fclose(fp) != 0) {
        ok = 0;
    }
    (void)remove(name);
    return ok ? 0 : 1;
}

static int
expect_private_open_modes_and_read_fallback(void) {
    const char* name = "dsd_neo_private_modes_test.tmp";
    (void)remove(name);

    errno = 0;
    if (dsd_fopen_private(NULL, "w") != NULL || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_fopen_private(name, NULL) != NULL || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_fopen_private(name, "") != NULL || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_fopen_private(name, "x") != NULL || errno != EINVAL) {
        return 1;
    }

    FILE* fp = dsd_fopen_private(name, "w");
    if (!fp) {
        return 1;
    }
    int ok = fputs("first\n", fp) >= 0 && fclose(fp) == 0;

    fp = dsd_fopen_private(name, "a+");
    if (!fp) {
        (void)remove(name);
        return 1;
    }
    ok = ok && fputs("second\n", fp) >= 0 && fclose(fp) == 0;

    fp = dsd_fopen_private(name, "r");
    if (!fp) {
        (void)remove(name);
        return 1;
    }
    char line[16];
    ok = ok && fgets(line, sizeof line, fp) != NULL && strcmp(line, "first\n") == 0;
    ok = ok && fclose(fp) == 0;
    (void)remove(name);
    return ok ? 0 : 1;
}

/* Issue #578: dsd_fopen_private_ex() says whether it opened the path, which a write mode creates or empties: 0 for
   arguments it refuses and a path it cannot open, 1 for a path it opened, written or read. */
static int
expect_private_open_reports_the_open(void) {
    const char* name = "dsd_neo_private_opened_test.tmp";
    (void)remove(name);
    int opened = -1;
    errno = 0;
    if (dsd_fopen_private_ex(NULL, "wb", &opened) != NULL || errno != EINVAL || opened != 0) {
        return 1;
    }
    opened = -1;
    if (dsd_fopen_private_ex(name, "x", &opened) != NULL || opened != 0) {
        return 1;
    }
    opened = -1;
    if (dsd_fopen_private_ex("dsd_neo_missing_private_dir/opened.tmp", "wb", &opened) != NULL || opened != 0) {
        return 1;
    }
    opened = -1;
    FILE* fp = dsd_fopen_private_ex(name, "wb", &opened);
    if (!fp) {
        return 1;
    }
    int ok = opened == 1 && fputs("probe\n", fp) >= 0;
    ok = fclose(fp) == 0 && ok;
    opened = -1;
    fp = dsd_fopen_private_ex(name, "r", &opened);
    ok = ok && fp != NULL && opened == 1;
    if (fp) {
        ok = fclose(fp) == 0 && ok;
    }
    fp = dsd_fopen_private_ex(name, "w", NULL);
    ok = ok && fp != NULL;
    if (fp) {
        ok = fclose(fp) == 0 && ok;
    }
    (void)remove(name);
    return ok ? 0 : 1;
}

static int
write_probe_file(const char* path) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    int rc = fputs("probe\n", fp) < 0 ? -1 : 0;
    if (fclose(fp) != 0) {
        rc = -1;
    }
    return rc;
}

static int
expect_existing_regular_file_guards(void) {
    const char* name = "dsd_neo_existing_regular_test.tmp";
    const char* dir_name = "dsd_neo_existing_regular_dir.tmp";
    (void)remove(name);
    (void)dsd_test_rmdir(dir_name);
    if (write_probe_file(name) != 0) {
        return 1;
    }

    int rc = 0;
    errno = 0;
    rc |= dsd_fopen_existing_regular_file(NULL, "r") == NULL && errno == EINVAL ? 0 : 1;
    errno = 0;
    rc |= dsd_fopen_existing_regular_file("", "r") == NULL && errno == EINVAL ? 0 : 1;
    errno = 0;
    rc |= dsd_fopen_existing_regular_file(name, NULL) == NULL && errno == EINVAL ? 0 : 1;
    errno = 0;
    rc |= dsd_fopen_existing_regular_file(name, "w") == NULL && errno == EINVAL ? 0 : 1;
    errno = 0;
    rc |= dsd_fopen_existing_regular_file(name, "r+") == NULL && errno == EINVAL ? 0 : 1;
    /* A missing file is ENOENT on every platform; callers create the file on that answer and refuse anything else. */
    const char* missing = "dsd_neo_existing_regular_missing.tmp";
    (void)remove(missing);
    errno = 0;
    rc |= dsd_fopen_existing_regular_file(missing, "r") == NULL && errno == ENOENT ? 0 : 1;

    FILE* fp = dsd_fopen_existing_regular_file(name, "rb");
    if (!fp) {
        rc = 1;
    } else {
        char line[16];
        rc |= fgets(line, sizeof line, fp) != NULL && strcmp(line, "probe\n") == 0 ? 0 : 1;
        rc |= fclose(fp) == 0 ? 0 : 1;
    }

    if (dsd_mkdir(dir_name, 0700) != 0) {
        rc = 1;
    } else {
        errno = 0;
        fp = dsd_fopen_existing_regular_file(dir_name, "r");
        const int open_errno = errno;
        if (fp) {
            fclose(fp);
            rc = 1;
        }
        rc |= open_errno == EINVAL ? 0 : 1;
    }

    (void)remove(name);
    (void)dsd_test_rmdir(dir_name);
    return rc;
}

static int
expect_resolves_existing_file(void) {
    const char* name = "dsd_neo_resolve_existing_local_test.tmp";
    (void)remove(name);
    if (write_probe_file(name) != 0) {
        return 1;
    }

    char resolved[1024];
    int rc = dsd_resolve_existing_local_file(name, resolved, sizeof resolved);
    (void)remove(name);
    if (rc != 0) {
        return 1;
    }
    return strcmp(resolved, name) == 0 ? 0 : 1;
}

static int
expect_opens_existing_file(void) {
    const char* name = "dsd_neo_open_existing_local_test.tmp";
    (void)remove(name);
    if (write_probe_file(name) != 0) {
        return 1;
    }

    char resolved[1024];
    FILE* fp = dsd_fopen_existing_local_file(name, resolved, sizeof resolved);
    if (!fp) {
        (void)remove(name);
        return 1;
    }

    char line[16];
    int ok = fgets(line, sizeof line, fp) != NULL && strcmp(line, "probe\n") == 0 && strcmp(resolved, name) == 0;
    if (fclose(fp) != 0) {
        ok = 0;
    }
    (void)remove(name);
    return ok ? 0 : 1;
}

static int
expect_rejects_unsafe_name(const char* name) {
    char resolved[1024];
    errno = 0;
    int rc = dsd_resolve_existing_local_file(name, resolved, sizeof resolved);
    return rc == -1 && errno == EINVAL ? 0 : 1;
}

static int
expect_rejects_missing_file(void) {
    const char* name = "dsd_neo_resolve_missing_local_test.tmp";
    char resolved[1024];
    (void)remove(name);
    errno = 0;
    int rc = dsd_resolve_existing_local_file(name, resolved, sizeof resolved);
    return rc == -1 && errno != 0 ? 0 : 1;
}

static int
expect_rejects_small_buffer(void) {
    const char* name = "dsd_neo_resolve_small_buffer_test.tmp";
    (void)remove(name);
    if (write_probe_file(name) != 0) {
        return 1;
    }

    char resolved[4];
    errno = 0;
    int rc = dsd_resolve_existing_local_file(name, resolved, sizeof resolved);
    (void)remove(name);
    return rc == -1 && errno == ENAMETOOLONG ? 0 : 1;
}

static int
expect_private_temp_replace(void) {
    const char* name = "dsd_neo_atomic_replace_test.tmp";
    char tmp[1024];
    (void)remove(name);

    FILE* fp = dsd_fopen_private_temp_for_replace(name, tmp, sizeof tmp, "wb");
    if (!fp) {
        return 1;
    }
    int ok = strcmp(tmp, name) != 0 && strstr(tmp, ".tmp.") != NULL;
    if (fputs("replacement\n", fp) < 0) {
        ok = 0;
    }
    if (fflush(fp) != 0) {
        ok = 0;
    }
    int fd = dsd_fileno(fp);
    if (fd < 0 || dsd_fsync(fd) != 0) {
        ok = 0;
    }
    if (fclose(fp) != 0) {
        ok = 0;
    }
    if (!ok || dsd_replace_file_with_temp(tmp, name) != 0) {
        (void)remove(tmp);
        (void)remove(name);
        return 1;
    }

    dsd_stat_t st;
    if (dsd_stat_path(tmp, &st) == 0) {
        (void)remove(tmp);
        (void)remove(name);
        return 1;
    }
    if (dsd_stat_path(name, &st) != 0 || !dsd_stat_is_regular(&st)) {
        (void)remove(name);
        return 1;
    }
#if !DSD_PLATFORM_WIN_NATIVE
    if ((st.st_mode & 0777) != 0600) {
        (void)remove(name);
        return 1;
    }
#endif

    FILE* in = fopen(name, "rb");
    if (!in) {
        (void)remove(name);
        return 1;
    }
    char line[32];
    ok = fgets(line, sizeof line, in) != NULL && strcmp(line, "replacement\n") == 0;
    if (fclose(in) != 0) {
        ok = 0;
    }
    (void)remove(name);
    return ok ? 0 : 1;
}

static int
expect_private_temp_rejects_invalid_arguments(void) {
    char tmp[32];
    errno = 0;
    if (dsd_fopen_private_temp_for_replace(NULL, tmp, sizeof tmp, "w") != NULL || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_fopen_private_temp_for_replace("", tmp, sizeof tmp, "w") != NULL || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_fopen_private_temp_for_replace("final.tmp", NULL, sizeof tmp, "w") != NULL || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_fopen_private_temp_for_replace("final.tmp", tmp, 0, "w") != NULL || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_fopen_private_temp_for_replace("final.tmp", tmp, sizeof tmp, "r") != NULL || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_fopen_private_temp_for_replace("this_name_is_too_long_for_the_tmp_buffer", tmp, sizeof tmp, "w") != NULL
        || errno != ENAMETOOLONG) {
        return 1;
    }
    return 0;
}

static int
expect_replace_rejects_invalid_arguments_and_missing_tmp(void) {
    errno = 0;
    if (dsd_replace_file_with_temp(NULL, "final.tmp") != -1 || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_replace_file_with_temp("", "final.tmp") != -1 || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_replace_file_with_temp("missing.tmp", NULL) != -1 || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_replace_file_with_temp("missing.tmp", "") != -1 || errno != EINVAL) {
        return 1;
    }
    errno = 0;
    if (dsd_replace_file_with_temp("missing_dsd_neo_replace.tmp", "final.tmp") != -1 || errno == 0) {
        (void)remove("final.tmp");
        return 1;
    }
    (void)remove("final.tmp");
    return 0;
}

static int
expect_null_device_is_readable(void) {
    const char* path = dsd_null_device();
    if (!path || path[0] == '\0') {
        return 1;
    }
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return 1;
    }
    return fclose(fp) == 0 ? 0 : 1;
}

#if !DSD_PLATFORM_WIN_NATIVE
static int
expect_rejects_symlink(void) {
    const char* target = "dsd_neo_resolve_symlink_target.tmp";
    const char* link_name = "dsd_neo_resolve_symlink_local_test.tmp";
    (void)remove(link_name);
    (void)remove(target);
    if (write_probe_file(target) != 0) {
        return 1;
    }
    if (symlink(target, link_name) != 0) {
        (void)remove(target);
        return 1;
    }

    char resolved[1024];
    errno = 0;
    FILE* fp = dsd_fopen_existing_local_file(link_name, resolved, sizeof resolved);
    if (fp) {
        fclose(fp);
    }
    (void)remove(link_name);
    (void)remove(target);
    return fp == NULL && errno != 0 ? 0 : 1;
}
#endif

static int
write_text_file(const char* path, const char* text) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return 1;
    }
    int ok = fputs(text, fp) >= 0;
    ok = fclose(fp) == 0 && ok;
    return ok ? 0 : 1;
}

static int
file_text_is(const char* path, const char* text) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    char buf[64];
    size_t n = fread(buf, 1, sizeof buf - 1U, fp);
    buf[n] = '\0';
    (void)fclose(fp);
    return strcmp(buf, text) == 0;
}

static int
file_exists(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    (void)fclose(fp);
    return 1;
}

/* Issue #661: "x" after a write mode creates exclusively. An existing file fails with EEXIST and keeps its bytes, so a
   capture whose name is taken never truncates an earlier one. */
static int
expect_private_exclusive_create(void) {
    const char* name = "dsd_neo_private_exclusive_test.tmp";
    (void)remove(name);
    int ok = 1;
    int opened = -1;
    FILE* fp = dsd_fopen_private_ex(name, "wbx", &opened);
    ok = ok && fp != NULL && opened == 1;
    if (fp) {
        ok = fputs("keep", fp) >= 0 && ok;
        ok = fclose(fp) == 0 && ok;
    }
    opened = -1;
    errno = 0;
    fp = dsd_fopen_private_ex(name, "wbx", &opened);
    ok = ok && fp == NULL && errno == EEXIST && opened == 0;
    if (fp) {
        (void)fclose(fp);
    }
    ok = ok && file_text_is(name, "keep");
    errno = 0;
    fp = dsd_fopen_private(name, "ax");
    ok = ok && fp == NULL && errno == EINVAL;
    if (fp) {
        (void)fclose(fp);
    }
    fp = dsd_fopen_private(name, "wx+");
    ok = ok && fp == NULL && errno == EEXIST;
    if (fp) {
        (void)fclose(fp);
    }
    (void)remove(name);
    return ok ? 0 : 1;
}

/* Issue #661: a no-replace rename never writes over an existing name: EEXIST, both files as they were. */
static int
expect_rename_noreplace_in(const char* dir) {
    char from[512];
    char taken[512];
    char to[512];
    DSD_SNPRINTF(from, sizeof from, "%s%sdsd_neo_noreplace_from.tmp", dir, dir[0] ? "/" : "");
    DSD_SNPRINTF(taken, sizeof taken, "%s%sdsd_neo_noreplace_taken.tmp", dir, dir[0] ? "/" : "");
    DSD_SNPRINTF(to, sizeof to, "%s%sdsd_neo_noreplace_to.tmp", dir, dir[0] ? "/" : "");
    (void)remove(from);
    (void)remove(taken);
    (void)remove(to);
    if (write_text_file(from, "from") != 0 || write_text_file(taken, "taken") != 0) {
        (void)remove(from);
        (void)remove(taken);
        return 1;
    }
    int ok = 1;
    int left = -1;
    errno = 0;
    ok = ok && dsd_rename_noreplace(from, taken, &left) == -1 && errno == EEXIST && left == 0;
    ok = ok && file_text_is(from, "from") && file_text_is(taken, "taken");
    left = -1;
    ok = ok && dsd_rename_noreplace(from, to, &left) == 0 && left == 0;
    ok = ok && !file_exists(from) && file_text_is(to, "from");
    (void)remove(from);
    (void)remove(taken);
    (void)remove(to);
    return ok ? 0 : 1;
}

static int
expect_rename_noreplace(void) {
    int rc = 0;
    errno = 0;
    rc |= dsd_rename_noreplace(NULL, "x", NULL) == -1 && errno == EINVAL ? 0 : 1;
    errno = 0;
    rc |= dsd_rename_noreplace("x", "", NULL) == -1 && errno == EINVAL ? 0 : 1;
    rc |= expect_rename_noreplace_in("");
    return rc;
}

/* The same in a directory whose name is not ASCII, under the UTF-8 locale the runtime selects (a CRT that cannot take
   one, such as MinGW's msvcrt, skips the case). */
static int
expect_rename_noreplace_non_ascii_dir(void) {
#if DSD_PLATFORM_WIN_NATIVE
    if (setlocale(LC_CTYPE, ".UTF8") == NULL) {
        return 0;
    }
#endif
    const char* dir = "dsd_neo_noreplace_\xC3\xBC";
    (void)dsd_mkdir(dir, 0700);
    int rc = expect_rename_noreplace_in(dir);
    rc |= dsd_test_rmdir(dir) == 0 ? 0 : 1;
#if DSD_PLATFORM_WIN_NATIVE
    (void)setlocale(LC_CTYPE, "C");
#endif
    return rc;
}

#if !DSD_PLATFORM_WIN_NATIVE
/* Whether the filesystem holding @p dir takes hard links: some (FAT-like, some FUSE) do not, and the fallback then
   claims the name instead. */
static int
dir_takes_hard_links(const char* dir) {
    char a[256];
    char b[256];
    DSD_SNPRINTF(a, sizeof a, "%s/link_probe.a", dir);
    DSD_SNPRINTF(b, sizeof b, "%s/link_probe.b", dir);
    int ok = write_text_file(a, "p") == 0 && link(a, b) == 0;
    (void)remove(b);
    (void)remove(a);
    return ok;
}

/* The hard-link fallback: EEXIST on a taken name, and a link whose source cannot be unlinked still publishes, reporting
   the source left behind. */
static int
expect_rename_noreplace_link_fallback(void) {
    const char* src_dir = "dsd_neo_noreplace_link_src";
    const char* dst_dir = "dsd_neo_noreplace_link_dst";
    const char* from = "dsd_neo_noreplace_link_src/f.tmp";
    const char* to = "dsd_neo_noreplace_link_dst/f.tmp";
    (void)remove(from);
    (void)remove(to);
    (void)dsd_mkdir(src_dir, 0700);
    (void)dsd_mkdir(dst_dir, 0700);
    int ok = write_text_file(from, "data") == 0 && write_text_file(to, "taken") == 0;
    int left = -1;
    errno = 0;
    ok = ok && dsd_rename_noreplace_by_link(from, to, &left) == -1 && errno == EEXIST && left == 0;
    ok = ok && file_text_is(from, "data") && file_text_is(to, "taken");
    (void)remove(to);
    if (geteuid() != 0 && dir_takes_hard_links(src_dir)) {
        /* A read-only source directory: the link lands, the unlink cannot. Without hard links there is no link to land
           (the rename onto the claim fails like the unlink), so this needs them. */
        ok = ok && chmod(src_dir, 0500) == 0;
        left = -1;
        ok = ok && dsd_rename_noreplace_by_link(from, to, &left) == 0 && left == 1;
        ok = ok && file_text_is(to, "data") && file_text_is(from, "data");
        (void)chmod(src_dir, 0700);
    }
    left = -1;
    (void)remove(to);
    ok = ok && dsd_rename_noreplace_by_link(from, to, &left) == 0 && left == 0;
    ok = ok && !file_exists(from) && file_text_is(to, "data");
    (void)remove(from);
    (void)remove(to);
    ok = dsd_test_rmdir(src_dir) == 0 && ok;
    ok = dsd_test_rmdir(dst_dir) == 0 && ok;
    return ok ? 0 : 1;
}

/* The fallback with no hard links: an exclusive create claims the name, so a taken name fails with EEXIST and keeps
   both files, a free one is published, and a rename that fails takes its claim back. */
static int
expect_rename_noreplace_reservation_fallback(void) {
    const char* dir = "dsd_neo_noreplace_claim";
    const char* from = "dsd_neo_noreplace_claim/f.tmp";
    const char* to = "dsd_neo_noreplace_claim/f.wav";
    const char* missing = "dsd_neo_noreplace_claim/missing.tmp";
    (void)remove(from);
    (void)remove(to);
    (void)dsd_mkdir(dir, 0700);
    int ok = write_text_file(from, "data") == 0 && write_text_file(to, "taken") == 0;
    errno = 0;
    ok = ok && dsd_rename_noreplace_by_reservation(from, to) == -1 && errno == EEXIST;
    ok = ok && file_text_is(from, "data") && file_text_is(to, "taken");

    (void)remove(to);
    ok = ok && dsd_rename_noreplace_by_reservation(from, to) == 0;
    ok = ok && !file_exists(from) && file_text_is(to, "data");

    (void)remove(to);
    errno = 0;
    ok = ok && dsd_rename_noreplace_by_reservation(missing, to) == -1 && errno == ENOENT;
    ok = ok && !file_exists(to);
    (void)remove(from);
    (void)remove(to);
    ok = dsd_test_rmdir(dir) == 0 && ok;
    return ok ? 0 : 1;
}
#endif

int
main(void) {
    int rc = 0;
    rc |= expect_posix_compat_wrappers();
    rc |= expect_many_unique_temp_files();
    rc |= expect_temp_name_not_reused_after_rename();
    rc |= expect_descriptor_wrappers();
    rc |= expect_negative_descriptor_wrappers();
    rc |= expect_private_open_modes_and_read_fallback();
    rc |= expect_private_open_reports_the_open();
    rc |= expect_existing_regular_file_guards();
    rc |= expect_resolves_existing_file();
    rc |= expect_opens_existing_file();
    rc |= expect_rejects_unsafe_name("");
    rc |= expect_rejects_unsafe_name("../config.ini");
    rc |= expect_rejects_unsafe_name("dir/config.ini");
    rc |= expect_rejects_unsafe_name("config..ini");
    rc |= expect_rejects_missing_file();
    rc |= expect_rejects_small_buffer();
    rc |= expect_private_temp_replace();
    rc |= expect_private_temp_rejects_invalid_arguments();
    rc |= expect_replace_rejects_invalid_arguments_and_missing_tmp();
    rc |= expect_null_device_is_readable();
    rc |= expect_private_exclusive_create();
    rc |= expect_rename_noreplace();
    rc |= expect_rename_noreplace_non_ascii_dir();
#if !DSD_PLATFORM_WIN_NATIVE
    rc |= expect_rejects_symlink();
    rc |= expect_rename_noreplace_link_fallback();
    rc |= expect_rename_noreplace_reservation_fallback();
#endif
    return rc;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno)
