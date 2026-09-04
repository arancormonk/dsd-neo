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

#include "dsd-neo/platform/platform.h"
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/stat.h>
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
    int fd = dsd_open_serial_write(name);
    if (fd < 0) {
        rc = 1;
    } else {
        rc |= dsd_close(fd) == 0 ? 0 : 1;
    }
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
        rc |= rmdir(dir) == 0 ? 0 : 1;
    }

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

static int
write_probe_file(const char* path) {
    FILE* fp = dsd_fopen_private(path, "w");
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
    (void)rmdir(dir_name);
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

    FILE* fp = dsd_fopen_existing_regular_file(name, "rb");
    if (!fp) {
        rc = 1;
    } else {
        char line[16];
        rc |= fgets(line, sizeof line, fp) != NULL && strcmp(line, "probe\n") == 0 ? 0 : 1;
        rc |= fclose(fp) == 0 ? 0 : 1;
    }

    if (mkdir(dir_name, 0700) != 0) {
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
    (void)rmdir(dir_name);
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

int
main(void) {
    int rc = 0;
    rc |= expect_posix_compat_wrappers();
    rc |= expect_descriptor_wrappers();
    rc |= expect_private_open_modes_and_read_fallback();
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
#if !DSD_PLATFORM_WIN_NATIVE
    rc |= expect_rejects_symlink();
#endif
    return rc;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno)
