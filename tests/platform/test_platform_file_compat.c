// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/file_compat.h>

#include "dsd-neo/platform/platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <unistd.h>
#endif

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
    rc |= expect_resolves_existing_file();
    rc |= expect_opens_existing_file();
    rc |= expect_rejects_unsafe_name("");
    rc |= expect_rejects_unsafe_name("../config.ini");
    rc |= expect_rejects_unsafe_name("dir/config.ini");
    rc |= expect_rejects_unsafe_name("config..ini");
    rc |= expect_rejects_missing_file();
    rc |= expect_rejects_small_buffer();
#if !DSD_PLATFORM_WIN_NATIVE
    rc |= expect_rejects_symlink();
#endif
    return rc;
}
