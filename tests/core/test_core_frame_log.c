// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/file_compat.h"
#include "test_support.h"

#if !defined(_WIN32)
#include <fcntl.h> // IWYU pragma: keep
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

static int
test_frame_log_writes_entry(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    initOpts(&opts);

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_frame_log");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    (void)dsd_close(fd);

    DSD_SNPRINTF(opts.frame_log_file, sizeof opts.frame_log_file, "%s", path);
    opts.frame_log_file[sizeof opts.frame_log_file - 1] = '\0';

    dsd_frame_logf(&opts, "frame=%d", 42);
    dsd_frame_log_close(&opts);

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        (void)remove(path);
        return 1;
    }

    char buf[512];
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    if (n == 0 && ferror(fp)) {
        DSD_FPRINTF(stderr, "fread(%s) failed: %s\n", path, strerror(errno));
        fclose(fp);
        (void)remove(path);
        return 1;
    }
    buf[n] = '\0';
    fclose(fp);
    (void)remove(path);

    if (strstr(buf, "frame=42") == NULL) {
        DSD_FPRINTF(stderr, "frame log did not contain expected payload\n");
        return 1;
    }
    return 0;
}

static int
test_p25_sm_log_writes_entry(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    initOpts(&opts);

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_p25_sm_log");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    (void)dsd_close(fd);

    DSD_SNPRINTF(opts.p25_sm_log_file, sizeof opts.p25_sm_log_file, "%s", path);
    opts.p25_sm_log_file[sizeof opts.p25_sm_log_file - 1] = '\0';

    dsd_p25_sm_logf(&opts, "event=test\tvalue=%d\n", 42);
    dsd_p25_sm_log_close(&opts);

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        (void)remove(path);
        return 1;
    }

    char buf[512];
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    if (n == 0 && ferror(fp)) {
        DSD_FPRINTF(stderr, "fread(%s) failed: %s\n", path, strerror(errno));
        fclose(fp);
        (void)remove(path);
        return 1;
    }
    buf[n] = '\0';
    fclose(fp);
    (void)remove(path);

    if (strstr(buf, "event=test value=42") == NULL) {
        DSD_FPRINTF(stderr, "P25 SM log did not contain expected sanitized payload: %s\n", buf);
        return 1;
    }
    return 0;
}

#if !defined(_WIN32)
typedef struct stderr_capture_s {
    FILE* stream;
    int saved_fd;
    int redirected;
} stderr_capture_t;

static int
set_failure(const char** failure, int* saved_errno, const char* message, int err) {
    if (failure) {
        *failure = message;
    }
    if (saved_errno) {
        *saved_errno = err;
    }
    return 1;
}

static void
stderr_capture_init(stderr_capture_t* capture) {
    capture->stream = NULL;
    capture->saved_fd = -1;
    capture->redirected = 0;
}

static int
stderr_capture_begin(stderr_capture_t* capture, const char** failure, int* saved_errno) {
    capture->stream = tmpfile();
    if (!capture->stream) {
        return set_failure(failure, saved_errno, "tmpfile failed", errno);
    }

    capture->saved_fd = dup(fileno(stderr));
    if (capture->saved_fd < 0) {
        return set_failure(failure, saved_errno, "dup(stderr) failed", errno);
    }

    if (fflush(stderr) != 0) {
        return set_failure(failure, saved_errno, "fflush(stderr) failed", errno);
    }

    if (dup2(fileno(capture->stream), fileno(stderr)) < 0) {
        return set_failure(failure, saved_errno, "dup2(capture, stderr) failed", errno);
    }
    capture->redirected = 1;
    return 0;
}

static int
stderr_capture_read(stderr_capture_t* capture, char* buf, size_t buf_size, const char** failure, int* saved_errno) {
    if (!buf || buf_size == 0) {
        return set_failure(failure, saved_errno, "invalid stderr capture buffer", 0);
    }
    if (fflush(stderr) != 0) {
        return set_failure(failure, saved_errno, "fflush(captured stderr) failed", errno);
    }
    if (fseek(capture->stream, 0, SEEK_SET) != 0) {
        return set_failure(failure, saved_errno, "fseek(captured stderr) failed", errno);
    }

    size_t n = fread(buf, 1, buf_size - 1, capture->stream);
    if (n == 0 && ferror(capture->stream)) {
        return set_failure(failure, saved_errno, "fread(captured stderr) failed", errno);
    }
    buf[n] = '\0';
    return 0;
}

static void
stderr_capture_end(stderr_capture_t* capture) {
    if (capture->redirected) {
        (void)fflush(stderr);
        (void)dup2(capture->saved_fd, fileno(stderr));
    }
    if (capture->saved_fd >= 0) {
        close(capture->saved_fd);
    }
    if (capture->stream) {
        fclose(capture->stream);
    }
}

static void
print_failure(const char* failure, int saved_errno) {
    if (!failure) {
        return;
    }
    if (saved_errno != 0) {
        DSD_FPRINTF(stderr, "%s: %s\n", failure, strerror(saved_errno));
    } else {
        DSD_FPRINTF(stderr, "%s\n", failure);
    }
}

static int
dev_full_available(const char* sink_path) {
    FILE* probe = dsd_fopen_private(sink_path, "a");
    if (!probe) {
        return 0;
    }
    fclose(probe);
    return 1;
}

static int
test_frame_log_write_error_reported_once(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    initOpts(&opts);

    const char* sink_path = "/dev/full";
    if (!dev_full_available(sink_path)) {
        return 0;
    }

    /*
     * /dev/full gives a deterministic write failure while still opening normally.
     * stderr is redirected only around the two writes so the test can assert that
     * the guard suppresses duplicate diagnostics from the same failing sink.
     */
    DSD_SNPRINTF(opts.frame_log_file, sizeof opts.frame_log_file, "%s", sink_path);
    opts.frame_log_file[sizeof opts.frame_log_file - 1] = '\0';

    int rc = 0;
    int saved_errno = 0;
    const char* failure = NULL;
    stderr_capture_t capture;
    stderr_capture_init(&capture);
    if (stderr_capture_begin(&capture, &failure, &saved_errno) != 0) {
        rc = 1;
        goto out;
    }

    // The first write reports the failure and the second write should stay quiet.
    dsd_frame_logf(&opts, "frame=%d", 1);
    dsd_frame_logf(&opts, "frame=%d", 2);

    char buf[4096];
    if (stderr_capture_read(&capture, buf, sizeof buf, &failure, &saved_errno) != 0) {
        rc = 1;
        goto out;
    }

    if (count_occurrences(buf, "Failed writing frame log file") != 1) {
        failure = "write failure should be reported once";
        rc = 1;
        goto out;
    }

    if (opts.frame_log_write_error_reported != 1) {
        failure = "write error guard should remain set after repeated write failures";
        rc = 1;
        goto out;
    }

out:
    stderr_capture_end(&capture);
    dsd_frame_log_close(&opts);
    print_failure(failure, saved_errno);
    return rc;
}

static int
test_p25_sm_log_write_error_reported_once(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    initOpts(&opts);

    const char* sink_path = "/dev/full";
    if (!dev_full_available(sink_path)) {
        return 0;
    }

    DSD_SNPRINTF(opts.p25_sm_log_file, sizeof opts.p25_sm_log_file, "%s", sink_path);
    opts.p25_sm_log_file[sizeof opts.p25_sm_log_file - 1] = '\0';

    int rc = 0;
    int saved_errno = 0;
    const char* failure = NULL;
    stderr_capture_t capture;
    stderr_capture_init(&capture);
    if (stderr_capture_begin(&capture, &failure, &saved_errno) != 0) {
        rc = 1;
        goto out;
    }

    dsd_p25_sm_logf(&opts, "event=%d", 1);
    dsd_p25_sm_logf(&opts, "event=%d", 2);

    char buf[4096];
    if (stderr_capture_read(&capture, buf, sizeof buf, &failure, &saved_errno) != 0) {
        rc = 1;
        goto out;
    }

    if (count_occurrences(buf, "Failed writing P25 SM log file") != 1) {
        failure = "P25 SM write failure should be reported once";
        rc = 1;
        goto out;
    }

    if (opts.p25_sm_log_write_error_reported != 1) {
        failure = "P25 SM write error guard should remain set after repeated write failures";
        rc = 1;
        goto out;
    }

out:
    stderr_capture_end(&capture);
    dsd_p25_sm_log_close(&opts);
    print_failure(failure, saved_errno);
    return rc;
}
#endif

int
main(void) {
    if (test_frame_log_writes_entry() != 0) {
        return 1;
    }
    if (test_p25_sm_log_writes_entry() != 0) {
        return 1;
    }
#if !defined(_WIN32)
    if (test_frame_log_write_error_reported_once() != 0) {
        return 1;
    }
    if (test_p25_sm_log_write_error_reported_once() != 0) {
        return 1;
    }
#endif
    return 0;
}
