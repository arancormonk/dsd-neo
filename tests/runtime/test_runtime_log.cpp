// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)
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
}

/* Must run before anything else resolves the sink: the environment default is
   latched exactly once per process. */
static void
test_log_sink_resolves_from_environment_once(void) {
    set_env_flag("DSD_NEO_LOG_SINK", "platform");
    assert(dsd_neo_log_get_sink() == DSD_NEO_LOG_SINK_PLATFORM);

    /* Already resolved: a later environment change must not take effect. */
    set_env_flag("DSD_NEO_LOG_SINK", "stderr");
    assert(dsd_neo_log_get_sink() == DSD_NEO_LOG_SINK_PLATFORM);

    set_env_flag("DSD_NEO_LOG_SINK", nullptr);
}

static void
test_log_sink_setter_round_trips(void) {
    dsd_neo_log_set_sink(DSD_NEO_LOG_SINK_STDERR);
    assert(dsd_neo_log_get_sink() == DSD_NEO_LOG_SINK_STDERR);

    dsd_neo_log_set_sink(DSD_NEO_LOG_SINK_PLATFORM);
    assert(dsd_neo_log_get_sink() == DSD_NEO_LOG_SINK_PLATFORM);

    dsd_neo_log_set_sink(DSD_NEO_LOG_SINK_STDERR);
    assert(dsd_neo_log_get_sink() == DSD_NEO_LOG_SINK_STDERR);
}

#ifndef __ANDROID__
static void
test_platform_sink_falls_back_to_stderr_off_android(void) {
    char out[128];
    set_env_flag("DSD_FORCE_ASCII", nullptr);
    set_env_flag("DSD_FORCE_UTF8", "1");
    dsd_neo_log_set_sink(DSD_NEO_LOG_SINK_PLATFORM);

    capture_stderr capture = capture_stderr_begin();
    dsd_neo_log_write(LOG_LEVEL_ERROR, "Fallback %d\n", 5);
    capture_stderr_end(&capture, out, sizeof(out));

    dsd_neo_log_set_sink(DSD_NEO_LOG_SINK_STDERR);
    assert(strcmp(out, "Fallback 5\n") == 0);
}
#endif

static void
test_log_write_ignores_null_format(void) {
    char out[16];
    set_env_flag("DSD_FORCE_ASCII", nullptr);
    set_env_flag("DSD_FORCE_UTF8", "1");

    capture_stderr capture = capture_stderr_begin();
    dsd_neo_log_write(LOG_LEVEL_ERROR, nullptr);
    capture_stderr_end(&capture, out, sizeof(out));

    assert(out[0] == '\0');
}

int
main(void) {
    test_log_sink_resolves_from_environment_once();
    test_log_sink_setter_round_trips();
#ifndef __ANDROID__
    test_platform_sink_falls_back_to_stderr_off_android();
#endif
    test_log_write_formats_and_preserves_utf8_when_supported();
    test_log_write_applies_ascii_fallback_when_unicode_disabled();
    test_log_write_ignores_null_format();
    set_env_flag("DSD_FORCE_ASCII", nullptr);
    set_env_flag("DSD_FORCE_UTF8", nullptr);

    std::puts("RUNTIME_LOG: OK");
    return 0;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno,misc-use-internal-linkage)
// NOLINTEND(cert-dcl03-c,misc-static-assert)
