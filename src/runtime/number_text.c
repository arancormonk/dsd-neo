// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Readout text without a negative zero. See number_text.h for the contract. */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/number_text.h>

#include <stddef.h>
#include <string.h>

/* "%.9f" of +/-DBL_MAX is a sign, 309 integer digits, a radix of up to a few bytes and nine decimals. */
#define NUMBER_TEXT_CAP 352

static int
number_text_is_letter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int
number_text_is_zero(const char* text) {
    const char* p = text;
    int saw_zero = 0;
    if (*p == '-' || *p == '+') {
        p++;
    }
    for (; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        if ((c >= '1' && c <= '9') || number_text_is_letter(c)) {
            return 0;
        }
        if (c == '0') {
            saw_zero = 1;
        }
    }
    return saw_zero;
}

int
dsd_number_text_drop_negative_zero(char* text, unsigned flags) {
    if (!text) {
        return -1;
    }
    if (text[0] != '-' || !number_text_is_zero(text)) {
        return 0;
    }
    if (flags & DSD_NUMBER_TEXT_PLUS) {
        text[0] = '+';
    } else {
        DSD_MEMMOVE(text, text + 1, strlen(text));
    }
    return 1;
}

static int
number_text_is_numeric(const char* text) {
    for (const char* p = text; *p != '\0'; p++) {
        if (number_text_is_letter((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

/* The radix printf wrote: the bytes between the integer digits and the decimals. */
static size_t
number_text_radix(const char* text, const char** radix) {
    const char* p = text;
    if (*p == '-' || *p == '+') {
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    const char* start = p;
    while (*p != '\0' && !(*p >= '0' && *p <= '9')) {
        p++;
    }
    *radix = start;
    return (size_t)(p - start);
}

/* '>' or '<', then the largest-magnitude number of the text's sign and precision that fits width - 1 bytes. */
static void
number_text_saturate(char* text, int width, int precision, unsigned flags) {
    const int negative = (text[0] == '-');
    const char marker = negative ? '<' : '>';
    const char* radix = NULL;
    const size_t radix_len = (precision > 0) ? number_text_radix(text, &radix) : 0;
    const int sign_len = (negative || (flags & DSD_NUMBER_TEXT_PLUS)) ? 1 : 0;
    const int fraction_len = (precision > 0) ? (int)radix_len + precision : 0;
    const int integer_digits = width - 1 - sign_len - fraction_len;
    char out[NUMBER_TEXT_CAP];
    size_t n = 0;
    if (integer_digits < 1) {
        for (int i = 0; i < width; i++) {
            out[n++] = marker;
        }
        out[n] = '\0';
        DSD_MEMCPY(text, out, n + 1);
        return;
    }
    out[n++] = marker;
    if (sign_len) {
        out[n++] = negative ? '-' : '+';
    }
    for (int i = 0; i < integer_digits; i++) {
        out[n++] = '9';
    }
    if (precision > 0) {
        DSD_MEMCPY(out + n, radix, radix_len);
        n += radix_len;
        for (int i = 0; i < precision; i++) {
            out[n++] = '9';
        }
    }
    out[n] = '\0';
    DSD_MEMCPY(text, out, n + 1);
}

int
dsd_number_text(char* out, size_t out_size, double value, int width, int precision, unsigned flags) {
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (precision < 0 || precision > 9 || width < 0 || width > 63
        || ((flags & DSD_NUMBER_TEXT_SATURATE) && width == 0)) {
        return -1;
    }
    char text[NUMBER_TEXT_CAP];
    const int len = (flags & DSD_NUMBER_TEXT_PLUS) ? DSD_SNPRINTF(text, sizeof text, "%+.*f", precision, value)
                                                   : DSD_SNPRINTF(text, sizeof text, "%.*f", precision, value);
    if (len < 0 || (size_t)len >= sizeof text) {
        return -1;
    }
    (void)dsd_number_text_drop_negative_zero(text, flags);
    if ((flags & DSD_NUMBER_TEXT_SATURATE) && strlen(text) > (size_t)width) {
        if (number_text_is_numeric(text)) {
            number_text_saturate(text, width, precision, flags);
        } else {
            text[width] = '\0';
        }
    }
    return DSD_SNPRINTF(out, out_size, "%*s", width, text);
}

int
dsd_count_text(char* out, size_t out_size, unsigned long long count, int width) {
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (width < 0 || width > 63) {
        return -1;
    }
    static const char units[] = {'M', 'G', 'T', 'P', 'E'};
    if (count <= 999999ULL) {
        return DSD_SNPRINTF(out, out_size, "%*llu", width, count);
    }
    /* Tenths of the unit, truncated: one million is 10 tenths of M. */
    unsigned long long tenth = 100000ULL;
    size_t unit = 0;
    while (unit + 1 < sizeof units && count / tenth > 9999ULL) {
        tenth *= 1000ULL;
        unit++;
    }
    const unsigned long long tenths = count / tenth;
    char text[16];
    DSD_SNPRINTF(text, sizeof text, "%llu.%llu%c", tenths / 10ULL, tenths % 10ULL, units[unit]);
    return DSD_SNPRINTF(out, out_size, "%*s", width, text);
}
