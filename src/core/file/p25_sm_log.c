// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/log.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

static int
p25_sm_log_setvbuf_size_sanitize(size_t requested_size, size_t* out_size) {
    if (!out_size) {
        return 0;
    }
    size_t size = requested_size;
    if (size < (size_t)2u) {
        size = (size_t)2u;
    }
    if (size > (size_t)INT_MAX) {
        size = (size_t)INT_MAX;
    }
    *out_size = size;
    return 1;
}

static void
p25_sm_log_configure_stream_buffer(FILE* stream, int mode, size_t requested_size) {
    if (!stream) {
        return;
    }
    size_t size = 0;
    if (!p25_sm_log_setvbuf_size_sanitize(requested_size, &size)) {
        return;
    }
    (void)setvbuf(stream, NULL, mode, size);
}

static void
p25_sm_log_sanitize_line(char* line) {
    if (!line) {
        return;
    }
    for (char* p = line; *p != '\0'; ++p) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            *p = ' ';
        }
    }
}

static int
p25_sm_log_ensure_open(dsd_opts* opts) {
    if (!opts || opts->p25_sm_log_file[0] == '\0') {
        return 0;
    }
    if (opts->p25_sm_log_f != NULL) {
        return 1;
    }
    opts->p25_sm_log_f = dsd_fopen_private(opts->p25_sm_log_file, "a");
    if (opts->p25_sm_log_f == NULL) {
        if (!opts->p25_sm_log_open_error_reported) {
            LOG_ERROR("Unable to open P25 SM log file: %s\n", opts->p25_sm_log_file);
            opts->p25_sm_log_open_error_reported = 1;
        }
        return 0;
    }
    opts->p25_sm_log_open_error_reported = 0;
    p25_sm_log_configure_stream_buffer(opts->p25_sm_log_f, _IOLBF, (size_t)BUFSIZ);
    return 1;
}

int
dsd_p25_sm_log_enabled(const dsd_opts* opts) {
    return (opts != NULL && opts->p25_sm_log_file[0] != '\0') ? 1 : 0;
}

void
dsd_p25_sm_log_close(dsd_opts* opts) {
    if (!opts || opts->p25_sm_log_f == NULL) {
        return;
    }
    fflush(opts->p25_sm_log_f);
    fclose(opts->p25_sm_log_f);
    opts->p25_sm_log_f = NULL;
}

void
dsd_p25_sm_logf(dsd_opts* opts, const char* format, ...) {
    if (!format || !dsd_p25_sm_log_enabled(opts)) {
        return;
    }
    if (!p25_sm_log_ensure_open(opts)) {
        return;
    }

    char line[4096] = {0};
    va_list args;
    va_start(args, format);
    DSD_VSNPRINTF(line, sizeof(line), format, args);
    va_end(args);
    p25_sm_log_sanitize_line(line);

    time_t now = time(NULL);
    char timestr[9];
    char datestr[11];
    (void)dsd_format_local_datetime(now, DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(now, DSD_LOCAL_DATETIME_DATE_HYPHEN, datestr, sizeof datestr);

    if (DSD_FPRINTF(opts->p25_sm_log_f, "%s %s %s\n", datestr, timestr, line) < 0) {
        if (!opts->p25_sm_log_write_error_reported) {
            LOG_ERROR("Failed writing P25 SM log file: %s\n", opts->p25_sm_log_file);
            opts->p25_sm_log_write_error_reported = 1;
        }
        dsd_p25_sm_log_close(opts);
        return;
    }
    opts->p25_sm_log_write_error_reported = 0;
}
