// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>

#define DSD_NEO_MAIN
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>

#undef DSD_NEO_MAIN

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

static int
count_occurrences(const char* haystack, const char* needle) {
    int count = 0;
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return 0;
    }
    const char* p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

int
main(void) {
#if defined(_WIN32)
    return 0;
#else
    static dsd_opts opts;
    memset(&opts, 0, sizeof opts);
    initOpts(&opts);

    const char* sink_path = "/dev/full";
    FILE* probe = fopen(sink_path, "a");
    if (!probe) {
        return 0;
    }
    fclose(probe);

    snprintf(opts.frame_log_file, sizeof opts.frame_log_file, "%s", sink_path);
    opts.frame_log_file[sizeof opts.frame_log_file - 1] = '\0';

    int rc = 0;
    int saved_errno = 0;
    const char* failure = NULL;
    FILE* capture = NULL;
    int saved_stderr_fd = -1;
    int stderr_redirected = 0;

    capture = tmpfile();
    if (!capture) {
        failure = "tmpfile failed";
        saved_errno = errno;
        rc = 1;
        goto out;
    }

    saved_stderr_fd = dup(fileno(stderr));
    if (saved_stderr_fd < 0) {
        failure = "dup(stderr) failed";
        saved_errno = errno;
        rc = 2;
        goto out;
    }

    if (fflush(stderr) != 0) {
        failure = "fflush(stderr) failed";
        saved_errno = errno;
        rc = 3;
        goto out;
    }

    if (dup2(fileno(capture), fileno(stderr)) < 0) {
        failure = "dup2(capture, stderr) failed";
        saved_errno = errno;
        rc = 4;
        goto out;
    }
    stderr_redirected = 1;

    dsd_frame_logf(&opts, "frame=%d", 1);
    dsd_frame_logf(&opts, "frame=%d", 2);

    if (fflush(stderr) != 0) {
        failure = "fflush(captured stderr) failed";
        saved_errno = errno;
        rc = 5;
        goto out;
    }

    if (fseek(capture, 0, SEEK_SET) != 0) {
        failure = "fseek(captured stderr) failed";
        saved_errno = errno;
        rc = 6;
        goto out;
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, capture);
    if (n == 0 && ferror(capture)) {
        failure = "fread(captured stderr) failed";
        saved_errno = errno;
        rc = 7;
        goto out;
    }
    buf[n] = '\0';

    if (count_occurrences(buf, "Failed writing frame log file") != 1) {
        failure = "write failure should be reported once";
        rc = 8;
        goto out;
    }

    if (opts.frame_log_write_error_reported != 1) {
        failure = "write error guard should remain set after repeated write failures";
        rc = 9;
        goto out;
    }

out:
    if (stderr_redirected) {
        (void)fflush(stderr);
        (void)dup2(saved_stderr_fd, fileno(stderr));
    }
    if (saved_stderr_fd >= 0) {
        close(saved_stderr_fd);
    }
    if (capture) {
        fclose(capture);
    }
    dsd_frame_log_close(&opts);

    if (failure) {
        if (saved_errno != 0) {
            fprintf(stderr, "%s: %s\n", failure, strerror(saved_errno));
        } else {
            fprintf(stderr, "%s\n", failure);
        }
    }
    return rc;
#endif
}
