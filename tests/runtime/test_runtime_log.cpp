// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno,misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/log.h>

typedef struct capture_stderr {
    FILE* file;
    int saved_fd;
} capture_stderr;

typedef struct log_sink_capture {
    int calls;
    dsd_neo_log_level_t level;
    char message[256];
} log_sink_capture;

static void
capture_log_sink_write(dsd_neo_log_level_t level, const char* message, void* context) {
    auto* capture = static_cast<log_sink_capture*>(context);
    assert(capture != nullptr);
    capture->calls++;
    capture->level = level;
    std::snprintf(capture->message, sizeof(capture->message), "%s", message ? message : "");
}

static void
set_env_flag(const char* name, const char* value) {
    int rc = value != nullptr ? dsd_setenv(name, value, 1) : dsd_unsetenv(name);
    assert(rc == 0);
}

static capture_stderr
capture_stderr_begin(void) {
    capture_stderr capture;
    capture.file = tmpfile();
    capture.saved_fd = -1;
    assert(capture.file != nullptr);

    fflush(stderr);
    capture.saved_fd = dsd_dup(dsd_fileno(stderr));
    assert(capture.saved_fd >= 0);
    assert(dsd_dup2(dsd_fileno(capture.file), dsd_fileno(stderr)) >= 0);
    return capture;
}

static void
capture_stderr_end(capture_stderr* capture, char* out, size_t out_size) {
    assert(capture != nullptr);
    assert(capture->file != nullptr);
    assert(out != nullptr);
    assert(out_size > 0);
    out[0] = '\0';

    int flush_rc = fflush(stderr);
    assert(flush_rc == 0);
    if (flush_rc != 0) {
        fclose(capture->file);
        capture->file = nullptr;
        return;
    }
    int restore_rc = dsd_dup2(capture->saved_fd, dsd_fileno(stderr));
    if (restore_rc < 0) {
        assert(restore_rc >= 0);
        fclose(capture->file);
        capture->file = nullptr;
        return;
    }
    int close_rc = dsd_close(capture->saved_fd);
    assert(close_rc == 0);
    if (close_rc != 0) {
        fclose(capture->file);
        capture->file = nullptr;
        return;
    }
    capture->saved_fd = -1;

    int seek_rc = fseek(capture->file, 0, SEEK_SET);
    assert(seek_rc == 0);
    if (seek_rc != 0) {
        out[0] = '\0';
        fclose(capture->file);
        capture->file = nullptr;
        return;
    }
    clearerr(capture->file);
    size_t nread = fread(out, 1, out_size - 1, capture->file);
    assert(ferror(capture->file) == 0);
    if (ferror(capture->file) != 0) {
        out[0] = '\0';
    } else {
        out[nread] = '\0';
    }
    fclose(capture->file);
    capture->file = nullptr;
}

static void
test_log_write_formats_and_preserves_utf8_when_supported(void) {
    char out[128];
    set_env_flag("DSD_FORCE_ASCII", nullptr);
    set_env_flag("DSD_FORCE_UTF8", "1");

    capture_stderr capture = capture_stderr_begin();
    dsd_neo_log_write(LOG_LEVEL_WARN, "Temp %s %d\n", "\xC2\xB0", 7);
    capture_stderr_end(&capture, out, sizeof(out));

    assert(strcmp(out, "Temp \xC2\xB0 7\n") == 0);
    dsd_neo_log_sink_reset();
}

static void
test_log_write_applies_ascii_fallback_when_unicode_disabled(void) {
    char out[128];
    set_env_flag("DSD_FORCE_ASCII", "1");
    set_env_flag("DSD_FORCE_UTF8", nullptr);

    capture_stderr capture = capture_stderr_begin();
    dsd_neo_log_write(LOG_LEVEL_INFO, "Glyphs %s %d\n", "\xC2\xB0 \xE2\x80\x93 \xE2\x98\x83", 3);
    capture_stderr_end(&capture, out, sizeof(out));

    assert(strcmp(out, "Glyphs  deg - ? 3\n") == 0);
    dsd_neo_log_sink_reset();
}

static void
test_log_write_ignores_null_format(void) {
    char out[16];
    set_env_flag("DSD_FORCE_ASCII", nullptr);
    set_env_flag("DSD_FORCE_UTF8", "1");

    capture_stderr capture = capture_stderr_begin();
    dsd_neo_log_write(LOG_LEVEL_ERROR, nullptr);
    capture_stderr_end(&capture, out, sizeof(out));

    assert(out[0] == '\0');
    dsd_neo_log_sink_reset();
}

static void
test_log_sink_receives_message_and_severity_without_stderr_mirror(void) {
    char out[128];
    log_sink_capture sink_capture = {};
    dsd_neo_log_sink sink = {capture_log_sink_write, &sink_capture, 0};
    set_env_flag("DSD_FORCE_ASCII", nullptr);
    set_env_flag("DSD_FORCE_UTF8", "1");

    dsd_neo_log_sink_set(&sink);
    capture_stderr capture = capture_stderr_begin();
    dsd_neo_log_write(LOG_LEVEL_ERROR, "Sink %d\n", 12);
    capture_stderr_end(&capture, out, sizeof(out));

    assert(sink_capture.calls == 1);
    assert(sink_capture.level == LOG_LEVEL_ERROR);
    assert(strcmp(sink_capture.message, "Sink 12\n") == 0);
    assert(out[0] == '\0');
    dsd_neo_log_sink_reset();
}

static void
test_log_sink_can_mirror_stderr(void) {
    char out[128];
    log_sink_capture sink_capture = {};
    dsd_neo_log_sink sink = {capture_log_sink_write, &sink_capture, 1};
    set_env_flag("DSD_FORCE_ASCII", nullptr);
    set_env_flag("DSD_FORCE_UTF8", "1");

    dsd_neo_log_sink_set(&sink);
    capture_stderr capture = capture_stderr_begin();
    dsd_neo_log_write(LOG_LEVEL_WARN, "Mirror %s\n", "on");
    capture_stderr_end(&capture, out, sizeof(out));

    assert(sink_capture.calls == 1);
    assert(sink_capture.level == LOG_LEVEL_WARN);
    assert(strcmp(sink_capture.message, "Mirror on\n") == 0);
    assert(strcmp(out, "Mirror on\n") == 0);
    dsd_neo_log_sink_reset();
}

static void
test_log_sink_reset_restores_stderr_default(void) {
    char out[128];
    log_sink_capture sink_capture = {};
    dsd_neo_log_sink sink = {capture_log_sink_write, &sink_capture, 0};
    set_env_flag("DSD_FORCE_ASCII", nullptr);
    set_env_flag("DSD_FORCE_UTF8", "1");

    dsd_neo_log_sink_set(&sink);
    dsd_neo_log_sink_reset();
    capture_stderr capture = capture_stderr_begin();
    dsd_neo_log_write(LOG_LEVEL_INFO, "Default sink\n");
    capture_stderr_end(&capture, out, sizeof(out));

    assert(sink_capture.calls == 0);
    assert(strcmp(out, "Default sink\n") == 0);
}

static void
test_log_sink_gets_ascii_fallback_message(void) {
    log_sink_capture sink_capture = {};
    dsd_neo_log_sink sink = {capture_log_sink_write, &sink_capture, 0};
    set_env_flag("DSD_FORCE_ASCII", "1");
    set_env_flag("DSD_FORCE_UTF8", nullptr);

    dsd_neo_log_sink_set(&sink);
    dsd_neo_log_write(LOG_LEVEL_INFO, "Glyphs %s\n", "\xC2\xB0 \xE2\x80\x93");

    assert(sink_capture.calls == 1);
    assert(strcmp(sink_capture.message, "Glyphs  deg -\n") == 0);
    dsd_neo_log_sink_reset();
}

int
main(void) {
    test_log_write_formats_and_preserves_utf8_when_supported();
    test_log_write_applies_ascii_fallback_when_unicode_disabled();
    test_log_write_ignores_null_format();
    test_log_sink_receives_message_and_severity_without_stderr_mirror();
    test_log_sink_can_mirror_stderr();
    test_log_sink_reset_restores_stderr_default();
    test_log_sink_gets_ascii_fallback_message();

    set_env_flag("DSD_FORCE_ASCII", nullptr);
    set_env_flag("DSD_FORCE_UTF8", nullptr);

    std::puts("RUNTIME_LOG: OK");
    return 0;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno,misc-use-internal-linkage)
