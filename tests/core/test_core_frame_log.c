// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/vocoder.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"
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

static void
fill_bits_from_bytes(char* bits, const uint8_t* bytes, size_t byte_count) {
    size_t out = 0;
    for (size_t i = 0; i < byte_count; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            bits[out++] = (char)((bytes[i] >> bit) & 1U);
        }
    }
}

static int
read_text_file(const char* path, char* buf, size_t buf_size) {
    if (!path || !buf || buf_size == 0) {
        return 1;
    }
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    size_t n = fread(buf, 1, buf_size - 1, fp);
    if (n == 0 && ferror(fp)) {
        DSD_FPRINTF(stderr, "fread(%s) failed: %s\n", path, strerror(errno));
        fclose(fp);
        return 1;
    }
    buf[n] = '\0';
    fclose(fp);
    return 0;
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

    char buf[512];
    if (read_text_file(path, buf, sizeof buf) != 0) {
        (void)remove(path);
        return 1;
    }
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

    char buf[512];
    if (read_text_file(path, buf, sizeof buf) != 0) {
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    if (strstr(buf, "event=test value=42") == NULL) {
        DSD_FPRINTF(stderr, "P25 SM log did not contain expected sanitized payload: %s\n", buf);
        return 1;
    }
    return 0;
}

static int
test_payload_detail_helpers_write_structured_frame_log(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    initOpts(&opts);

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_payload_frame_log");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    (void)dsd_close(fd);

    DSD_SNPRINTF(opts.frame_log_file, sizeof opts.frame_log_file, "%s", path);
    opts.frame_log_file[sizeof opts.frame_log_file - 1] = '\0';

    const uint8_t imbe_bytes[11] = {0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU, 0xDEU, 0xF0U, 0x01U, 0x23U, 0x45U};
    char imbe_bits[88];
    fill_bits_from_bytes(imbe_bits, imbe_bytes, sizeof imbe_bytes);
    state.currentslot = 0;
    state.errs = 0x0A;
    state.errs2 = 0x0B;
    PrintIMBEData(&opts, &state, imbe_bits);

    char ambe_bits[49];
    DSD_MEMSET(ambe_bits, 1, sizeof ambe_bits);
    state.currentslot = 1;
    state.errsR = 0x0C;
    state.errs2R = 0x0D;
    PrintAMBEData(&opts, &state, ambe_bits);

    dsd_frame_log_close(&opts);

    char buf[1024];
    int rc = read_text_file(path, buf, sizeof buf);
    (void)remove(path);
    if (rc != 0) {
        return 1;
    }

    if (strstr(buf, "FRAME IMBE slot=1 data=123456789ABCDEF0012345 err=[A] [B]") == NULL) {
        DSD_FPRINTF(stderr, "IMBE frame-log payload mismatch: %s\n", buf);
        return 1;
    }
    if (strstr(buf, "FRAME AMBE slot=2 data=FFFFFFFFFFFF80 err=[C] [D]") == NULL) {
        DSD_FPRINTF(stderr, "AMBE frame-log payload mismatch: %s\n", buf);
        return 1;
    }
    return 0;
}

static int
test_gated_soft_voice_helpers_log_without_media_processing(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    initOpts(&opts);

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_gated_voice_frame_log");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    (void)dsd_close(fd);

    DSD_SNPRINTF(opts.frame_log_file, sizeof opts.frame_log_file, "%s", path);
    opts.frame_log_file[sizeof opts.frame_log_file - 1] = '\0';

    dsd_vocoder_soft_bit imbe_soft[8][23] = {{{0}}};
    dsd_vocoder_soft_bit ambe_soft[4][24] = {{{0}}};
    for (int row = 0; row < 8; row++) {
        for (int bit = 0; bit < 23; bit++) {
            imbe_soft[row][bit].reliability = 255;
        }
    }
    for (int row = 0; row < 4; row++) {
        for (int bit = 0; bit < 24; bit++) {
            ambe_soft[row][bit].reliability = 255;
        }
    }

    state.p25vc = 7;
    state.audio_out_temp_buf[0] = 0.25F;
    dsd_mbe_log_imbe_soft_frame(&opts, &state, imbe_soft);
    state.currentslot = 1;
    dsd_mbe_log_ambe_soft_frame(&opts, &state, ambe_soft);
    dsd_frame_log_close(&opts);

    char buf[1024];
    int rc = read_text_file(path, buf, sizeof buf);
    (void)remove(path);
    if (rc != 0) {
        return 1;
    }
    if (strstr(buf, "FRAME IMBE slot=1") == NULL || strstr(buf, "FRAME AMBE slot=2") == NULL) {
        DSD_FPRINTF(stderr, "gated soft voice frame log mismatch: %s\n", buf);
        return 1;
    }
    if (state.p25vc != 7 || state.audio_out_temp_buf[0] != 0.25F) {
        DSD_FPRINTF(stderr, "gated soft voice logging changed media state\n");
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

static int
test_p25_sm_log_open_error_reported_once(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    initOpts(&opts);

    char dir_path[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir_path, sizeof dir_path, "dsdneo_p25_sm_log_dir")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    DSD_SNPRINTF(opts.p25_sm_log_file, sizeof opts.p25_sm_log_file, "%s", dir_path);
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

    if (count_occurrences(buf, "Unable to open P25 SM log file") != 1) {
        failure = "P25 SM open failure should be reported once";
        rc = 1;
        goto out;
    }

    if (opts.p25_sm_log_open_error_reported != 1) {
        failure = "P25 SM open error guard should remain set after repeated open failures";
        rc = 1;
        goto out;
    }

    if (opts.p25_sm_log_write_error_reported != 0) {
        failure = "P25 SM write error guard should not be set by open failure";
        rc = 1;
        goto out;
    }

    if (opts.p25_sm_log_f != NULL) {
        failure = "P25 SM failed open should not leave an active file handle";
        rc = 1;
        goto out;
    }

out:
    stderr_capture_end(&capture);
    (void)rmdir(dir_path);
    print_failure(failure, saved_errno);
    return rc;
}

static int
test_print_frame_info_formats_p25_identifiers(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    initOpts(&opts);

    int rc = 0;
    int saved_errno = 0;
    const char* failure = NULL;
    stderr_capture_t capture;
    stderr_capture_init(&capture);
    if (stderr_capture_begin(&capture, &failure, &saved_errno) != 0) {
        rc = 1;
        goto out;
    }

    state.p2_wacn = 0x12345ULL;
    state.p2_sysid = 0xABCU;
    state.p2_cc = 0x2F0U;
    state.p2_rfssid = 7;
    state.p2_siteid = 9;
    printFrameInfo(&opts, &state);

    DSD_MEMSET(&state, 0, sizeof state);
    state.nac = 0x2A3;
    printFrameInfo(&opts, &state);

    char buf[4096];
    if (stderr_capture_read(&capture, buf, sizeof buf, &failure, &saved_errno) != 0) {
        rc = 1;
        goto out;
    }

    if (strstr(buf, "WACN: 12345; ") == NULL || strstr(buf, "SYS: ABC; ") == NULL
        || strstr(buf, "NAC/CC: 2F0; ") == NULL || strstr(buf, "RFSS: 007; ") == NULL
        || strstr(buf, "Site: 009; ") == NULL) {
        failure = "P25 phase-2 frame identifiers were not formatted as expected";
        rc = 1;
        goto out;
    }
    if (strstr(buf, "NAC: 2A3; ") == NULL || count_occurrences(buf, "NAC/CC:") != 1) {
        failure = "P25 frame info should fall back to NAC when p2_cc is not set";
        rc = 1;
        goto out;
    }

out:
    stderr_capture_end(&capture);
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
    if (test_payload_detail_helpers_write_structured_frame_log() != 0) {
        return 1;
    }
    if (test_gated_soft_voice_helpers_log_without_media_processing() != 0) {
        return 1;
    }
#if !defined(_WIN32)
    if (test_frame_log_write_error_reported_once() != 0) {
        return 1;
    }
    if (test_p25_sm_log_write_error_reported_once() != 0) {
        return 1;
    }
    if (test_p25_sm_log_open_error_reported_once() != 0) {
        return 1;
    }
    if (test_print_frame_info_formats_p25_identifiers() != 0) {
        return 1;
    }
#endif
    return 0;
}
