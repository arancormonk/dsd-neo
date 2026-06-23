// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/timing.h"

static int
expect_cc_zero(const dsd_state* state) {
    return state && state->last_cc_sync_time == 0 && state->last_cc_sync_time_m == 0.0;
}

static int
expect_vc_zero(const dsd_state* state) {
    return state && state->last_vc_sync_time == 0 && state->last_vc_sync_time_m == 0.0;
}

static int
expect_str(const char* label, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", label, got, want);
        return 0;
    }
    return 1;
}

static int
is_digit_char(char ch) {
    return ch >= '0' && ch <= '9';
}

static int
expect_time_shape(const char* label, const char* value, int with_colons) {
    size_t len = strlen(value);
    if (len != (with_colons ? 8u : 6u)) {
        DSD_FPRINTF(stderr, "%s: unexpected length %zu in '%s'\n", label, len, value);
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (with_colons && (i == 2 || i == 5)) {
            if (value[i] != ':') {
                DSD_FPRINTF(stderr, "%s: missing colon at %zu in '%s'\n", label, i, value);
                return 0;
            }
        } else if (!is_digit_char(value[i])) {
            DSD_FPRINTF(stderr, "%s: non-digit at %zu in '%s'\n", label, i, value);
            return 0;
        }
    }
    return 1;
}

static int
expect_date_shape(const char* label, const char* value, char separator) {
    size_t len = strlen(value);
    size_t want_len = separator ? 10u : 8u;
    if (len != want_len) {
        DSD_FPRINTF(stderr, "%s: unexpected length %zu in '%s'\n", label, len, value);
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (separator && (i == 4 || i == 7)) {
            if (value[i] != separator) {
                DSD_FPRINTF(stderr, "%s: missing separator at %zu in '%s'\n", label, i, value);
                return 0;
            }
        } else if (!is_digit_char(value[i])) {
            DSD_FPRINTF(stderr, "%s: non-digit at %zu in '%s'\n", label, i, value);
            return 0;
        }
    }
    return 1;
}

static int
test_time_format_wrappers(void) {
    int rc = 0;
    const time_t fixed = (time_t)1700000000;
    struct tm local_tm;
    if (dsd_localtime(&fixed, &local_tm) != 0) {
        return 10;
    }

    char expected_time[9];
    char expected_time_compact[7];
    char expected_date_hyphen[11];
    char expected_date_compact[9];
    DSD_SNPRINTF(expected_time, sizeof expected_time, "%02d:%02d:%02d", local_tm.tm_hour, local_tm.tm_min,
                 local_tm.tm_sec);
    DSD_SNPRINTF(expected_time_compact, sizeof expected_time_compact, "%02d%02d%02d", local_tm.tm_hour, local_tm.tm_min,
                 local_tm.tm_sec);
    DSD_SNPRINTF(expected_date_hyphen, sizeof expected_date_hyphen, "%04u-%02u-%02u",
                 (unsigned)(local_tm.tm_year + 1900), (unsigned)(local_tm.tm_mon + 1), (unsigned)local_tm.tm_mday);
    DSD_SNPRINTF(expected_date_compact, sizeof expected_date_compact, "%04u%02u%02u",
                 (unsigned)(local_tm.tm_year + 1900), (unsigned)(local_tm.tm_mon + 1), (unsigned)local_tm.tm_mday);

    char time_colon[9];
    char time_compact[7];
    char date_hyphen[11];
    char date_compact[9];
    getTimeN_buf(fixed, time_colon);
    getTimeF_buf(fixed, time_compact);
    getDateN_buf(fixed, date_hyphen);
    getDateF_buf(fixed, date_compact);

    if (!expect_str("fixed getTimeN_buf", time_colon, expected_time)) {
        rc = 11;
    }
    if (!expect_str("fixed getTimeF_buf", time_compact, expected_time_compact)) {
        rc = 12;
    }
    if (!expect_str("fixed getDateN_buf", date_hyphen, expected_date_hyphen)) {
        rc = 13;
    }
    if (!expect_str("fixed getDateF_buf", date_compact, expected_date_compact)) {
        rc = 14;
    }

    char now_time[7];
    char now_time_colon[9];
    char now_date[9];
    char now_date_hyphen[11];
    char now_date_slash[11];
    getTime_buf(now_time);
    getTimeC_buf(now_time_colon);
    getDate_buf(now_date);
    getDateH_buf(now_date_hyphen);
    getDateS_buf(now_date_slash);

    if (!expect_time_shape("current getTime_buf", now_time, 0)) {
        rc = 15;
    }
    if (!expect_time_shape("current getTimeC_buf", now_time_colon, 1)) {
        rc = 16;
    }
    if (!expect_date_shape("current getDate_buf", now_date, '\0')) {
        rc = 17;
    }
    if (!expect_date_shape("current getDateH_buf", now_date_hyphen, '-')) {
        rc = 18;
    }
    if (!expect_date_shape("current getDateS_buf", now_date_slash, '/')) {
        rc = 19;
    }

    return rc;
}

int
main(void) {
    int rc = test_time_format_wrappers();
    if (rc != 0) {
        return rc;
    }

    exitflag = 0;
    cleanupAndExit(NULL, NULL);
    if (exitflag != 1) {
        return 20;
    }

    // dsd_state is a multi-megabyte struct; avoid Windows' default ~1MB stack.
    dsd_state* state = calloc(1, sizeof(*state));
    if (!state) {
        return 100;
    }

    if (!expect_cc_zero(state) || !expect_vc_zero(state)) {
        free(state);
        return 1;
    }

    dsd_mark_cc_sync(state);
    if (state->last_cc_sync_time == 0) {
        free(state);
        return 2;
    }
    if (state->last_cc_sync_time_m == 0.0) {
        dsd_sleep_ms(1);
        dsd_mark_cc_sync(state);
        if (state->last_cc_sync_time_m == 0.0) {
            free(state);
            return 3;
        }
    }

    dsd_clear_cc_sync(state);
    if (!expect_cc_zero(state)) {
        free(state);
        return 4;
    }

    dsd_mark_vc_sync(state);
    if (state->last_vc_sync_time == 0) {
        free(state);
        return 5;
    }
    if (state->last_vc_sync_time_m == 0.0) {
        dsd_sleep_ms(1);
        dsd_mark_vc_sync(state);
        if (state->last_vc_sync_time_m == 0.0) {
            free(state);
            return 6;
        }
    }

    dsd_clear_vc_sync(state);
    if (!expect_vc_zero(state)) {
        free(state);
        return 7;
    }

    dsd_mark_cc_sync(NULL);
    dsd_mark_vc_sync(NULL);
    dsd_clear_cc_sync(NULL);
    dsd_clear_vc_sync(NULL);

    free(state);
    return 0;
}
