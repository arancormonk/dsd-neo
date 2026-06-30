// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/safe_api.h"

static atomic_int g_ui_history_mode = 1;

static int
ui_history_normalize_mode(int mode) {
    int m = mode % 3;
    if (m < 0) {
        m += 3;
    }
    return m;
}

static int
ui_history_range_is_digits(const char* s, size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
        if (!isdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

static int
ui_history_has_canonical_timestamp_prefix(const char* s) {
    static const size_t k_digit_ranges[][2] = {
        {0, 4},   // YYYY
        {5, 7},   // MM
        {8, 10},  // DD
        {11, 13}, // HH
        {14, 16}, // MM
        {17, 19}, // SS
    };

    for (size_t i = 0; i < (sizeof k_digit_ranges / sizeof k_digit_ranges[0]); ++i) {
        if (!ui_history_range_is_digits(s, k_digit_ranges[i][0], k_digit_ranges[i][1])) {
            return 0;
        }
    }

    return s[4] == '-' && s[7] == '-' && s[10] == ' ' && s[13] == ':' && s[16] == ':' && s[19] == ' ';
}

static int
ui_history_has_full_datetime_prefix(const char* s) {
    if (s == NULL) {
        return 0;
    }

    const size_t min_len = 20; // "YYYY-MM-DD HH:MM:SS "
    if (strlen(s) < min_len) {
        return 0;
    }

    // Canonical prefix check produced by watchdog_event_current/datacall.
    return ui_history_has_canonical_timestamp_prefix(s);
}

static int
ui_history_parse_2_digits(const char* s, size_t off) {
    return ((s[off] - '0') * 10) + (s[off + 1] - '0');
}

static int
ui_history_parse_4_digits(const char* s, size_t off) {
    return ((s[off] - '0') * 1000) + ((s[off + 1] - '0') * 100) + ((s[off + 2] - '0') * 10) + (s[off + 3] - '0');
}

static int
ui_history_datetime_fields_in_range(int year, int month, int day, int hour, int minute, int second) {
    return year >= 1970 && month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour >= 0 && hour <= 23 && minute >= 0
           && minute <= 59 && second >= 0 && second <= 60;
}

int
ui_history_get_mode(void) {
    return ui_history_normalize_mode(atomic_load(&g_ui_history_mode));
}

void
ui_history_set_mode(int mode) {
    atomic_store(&g_ui_history_mode, ui_history_normalize_mode(mode));
}

int
ui_history_cycle_mode(void) {
    int current = atomic_load(&g_ui_history_mode);
    for (;;) {
        int next = ui_history_normalize_mode(current + 1);
        if (atomic_compare_exchange_strong(&g_ui_history_mode, &current, next)) {
            return next;
        }
    }
}

size_t
ui_history_compact_event_text(char* out, size_t out_size, const char* event_text, int mode) {
    if (out == NULL || out_size == 0) {
        return 0;
    }

    const char* src = event_text ? event_text : "";
    if (ui_history_normalize_mode(mode) == 1 && ui_history_has_full_datetime_prefix(src)) {
        src += 11; // drop "YYYY-MM-DD " while keeping time and payload
    }

    size_t n = strlen(src);
    if (n >= out_size) {
        n = out_size - 1;
    }
    DSD_MEMCPY(out, src, n);
    out[n] = '\0';
    return n;
}

time_t
ui_history_event_sort_time(const char* event_text, time_t fallback_time) {
    if (!ui_history_has_full_datetime_prefix(event_text)) {
        return fallback_time;
    }

    int year = ui_history_parse_4_digits(event_text, 0);
    int month = ui_history_parse_2_digits(event_text, 5);
    int day = ui_history_parse_2_digits(event_text, 8);
    int hour = ui_history_parse_2_digits(event_text, 11);
    int minute = ui_history_parse_2_digits(event_text, 14);
    int second = ui_history_parse_2_digits(event_text, 17);
    if (!ui_history_datetime_fields_in_range(year, month, day, hour, minute, second)) {
        return fallback_time;
    }

    struct tm tmv;
    DSD_MEMSET(&tmv, 0, sizeof tmv);
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = minute;
    tmv.tm_sec = second;
    tmv.tm_isdst = -1;

    time_t parsed = mktime(&tmv);
    if (parsed == (time_t)-1) {
        return fallback_time;
    }
    return parsed;
}
