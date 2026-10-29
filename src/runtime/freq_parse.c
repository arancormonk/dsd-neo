// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/freq_parse.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static double
freq_suffix_factor(char* buf) {
    size_t len = strlen(buf);
    if (len == 0) {
        return 0.0;
    }

    char last = buf[len - 1];
    if (last == 'g' || last == 'G') {
        buf[len - 1] = '\0';
        return 1e9;
    }
    if (last == 'm' || last == 'M') {
        buf[len - 1] = '\0';
        return 1e6;
    }
    if (last == 'k' || last == 'K') {
        buf[len - 1] = '\0';
        return 1e3;
    }

    return 1.0;
}

static int
parse_positive_double(const char* text, double* value) {
    char* end = NULL;
    double parsed = strtod(text, &end);
    if (end == text || (end && *end != '\0') || parsed <= 0.0) {
        return 0;
    }

    *value = parsed;
    return 1;
}

uint32_t
dsd_parse_freq_hz(const char* s) {
    if (!s || !*s) {
        return 0;
    }
    char buf[64];
    if (strlen(s) >= sizeof buf) {
        return 0;
    }
    DSD_SNPRINTF(buf, sizeof buf, "%s", s);
    buf[sizeof buf - 1] = '\0';
    double factor = freq_suffix_factor(buf);
    if (factor <= 0.0) {
        return 0;
    }

    double val = 0.0;
    if (!parse_positive_double(buf, &val)) {
        return 0;
    }

    double hz = val * factor;
    /* Written so NaN fails the test; the upper bound is the largest value that rounds to UINT32_MAX. */
    if (!(hz >= 0.5 && hz < 4294967295.5)) {
        return 0;
    }
    return (uint32_t)(hz + 0.5);
}

/* A bare number whose integer part has at most this many digits (leading zeros aside) is MHz, so
 * every bare MHz value is below DSD_FREQ_ENTRY_BARE_HZ_MIN (999999 is the largest 6-digit value). */
#define FREQ_ENTRY_BARE_MHZ_DIGITS 6u

static int
freq_entry_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int
freq_entry_is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char
freq_entry_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Whether text[0..len) is word (lowercase) in any case. */
static int
freq_entry_word_is(const char* text, size_t len, const char* word) {
    size_t i = 0;
    for (; i < len && word[i] != '\0'; i++) {
        if (freq_entry_lower(text[i]) != word[i]) {
            return 0;
        }
    }
    return i == len && word[i] == '\0';
}

/* Decimal places a unit shifts the number by (Hz 0, k 3, M 6, G 9), or -1 when it is not a unit. */
static int
freq_entry_unit_places(const char* unit, size_t len) {
    if (freq_entry_word_is(unit, len, "hz")) {
        return 0;
    }
    if (freq_entry_word_is(unit, len, "k") || freq_entry_word_is(unit, len, "khz")) {
        return 3;
    }
    if (freq_entry_word_is(unit, len, "m") || freq_entry_word_is(unit, len, "mhz")) {
        return 6;
    }
    if (freq_entry_word_is(unit, len, "g") || freq_entry_word_is(unit, len, "ghz")) {
        return 9;
    }
    return -1;
}

/* *value = *value * 10 + digit; returns 0, leaving *value, when that would pass UINT32_MAX. */
static int
freq_entry_push_digit(uint32_t* value, uint32_t digit) {
    if (*value > (UINT32_MAX - digit) / 10u) {
        return 0;
    }
    *value = *value * 10u + digit;
    return 1;
}

int
dsd_parse_freq_entry_hz(const char* text, uint32_t* out_hz) {
    if (text == NULL || out_hz == NULL) {
        return DSD_FREQ_ENTRY_EMPTY;
    }

    /* Bounded: never reads more than one byte past the limit. */
    size_t len = 0;
    while (len <= (size_t)DSD_FREQ_ENTRY_MAX_LEN && text[len] != '\0') {
        len++;
    }
    if (len > (size_t)DSD_FREQ_ENTRY_MAX_LEN) {
        return DSD_FREQ_ENTRY_TOO_LONG;
    }

    size_t begin = 0;
    size_t end = len;
    while (begin < end && text[begin] == ' ') {
        begin++;
    }
    while (end > begin && text[end - 1] == ' ') {
        end--;
    }
    if (begin == end) {
        return DSD_FREQ_ENTRY_EMPTY;
    }
    /* Before the grammar, so "146,52 MHz" gets the decimal-comma message. */
    if (memchr(text + begin, ',', end - begin) != NULL) {
        return DSD_FREQ_ENTRY_DECIMAL_COMMA;
    }

    /* The number: [0-9]*('.'[0-9]*)? with at least one digit. */
    size_t pos = begin;
    const size_t int_begin = pos;
    while (pos < end && freq_entry_is_digit(text[pos])) {
        pos++;
    }
    const size_t int_end = pos;
    size_t frac_begin = pos;
    size_t frac_end = pos;
    if (pos < end && text[pos] == '.') {
        pos++;
        frac_begin = pos;
        while (pos < end && freq_entry_is_digit(text[pos])) {
            pos++;
        }
        frac_end = pos;
    }
    if (int_end == int_begin && frac_end == frac_begin) {
        return DSD_FREQ_ENTRY_SYNTAX;
    }

    size_t unit = pos;
    while (unit < end && text[unit] == ' ') {
        unit++;
    }
    size_t places = 0;
    if (unit == end) {
        /* No unit (trailing spaces are trimmed, so nothing follows the number). */
        size_t lead = int_begin;
        while (lead < int_end && text[lead] == '0') {
            lead++;
        }
        if (int_end - lead <= FREQ_ENTRY_BARE_MHZ_DIGITS) {
            /* MHz. Refuse an integer part that cannot be MHz before any arithmetic, so a sub-Hz
             * or fractional tail never changes the answer. */
            uint32_t whole = 0;
            for (size_t i = lead; i < int_end; i++) {
                whole = whole * 10u + (uint32_t)(text[i] - '0');
            }
            if (whole > UINT32_MAX / 1000000u) {
                return DSD_FREQ_ENTRY_AMBIGUOUS;
            }
            places = 6;
        }
    } else {
        for (size_t i = unit; i < end; i++) {
            if (!freq_entry_is_letter(text[i])) {
                return DSD_FREQ_ENTRY_SYNTAX;
            }
        }
        const int unit_places = freq_entry_unit_places(text + unit, end - unit);
        if (unit_places < 0) {
            return DSD_FREQ_ENTRY_UNIT;
        }
        places = (size_t)unit_places;
    }

    /* The integer digits, then exactly `places` fraction digits (zero-padded). */
    uint32_t value = 0;
    for (size_t i = int_begin; i < int_end; i++) {
        if (!freq_entry_push_digit(&value, (uint32_t)(text[i] - '0'))) {
            return DSD_FREQ_ENTRY_TOO_HIGH;
        }
    }
    for (size_t k = 0; k < places; k++) {
        const size_t i = frac_begin + k;
        const uint32_t digit = i < frac_end ? (uint32_t)(text[i] - '0') : 0u;
        if (!freq_entry_push_digit(&value, digit)) {
            return DSD_FREQ_ENTRY_TOO_HIGH;
        }
    }
    /* Fraction digits past the unit's places must be zeros. The range wins over the sub-Hz
     * refusal: at UINT32_MAX any non-zero tail is above the limit. */
    for (size_t i = frac_begin + places; i < frac_end; i++) {
        if (text[i] != '0') {
            return value == UINT32_MAX ? DSD_FREQ_ENTRY_TOO_HIGH : DSD_FREQ_ENTRY_SUB_HZ;
        }
    }
    if (value == 0u) {
        return DSD_FREQ_ENTRY_ZERO;
    }

    *out_hz = value;
    return DSD_FREQ_ENTRY_OK;
}

const char*
dsd_freq_entry_result_text(int result) {
    switch (result) {
        case DSD_FREQ_ENTRY_TOO_LONG: return "Too long: at most 32 characters";
        case DSD_FREQ_ENTRY_DECIMAL_COMMA: return "Use '.' for decimals, e.g. 146.52";
        case DSD_FREQ_ENTRY_SYNTAX: return "Not a frequency. Type MHz, e.g. 146.52";
        case DSD_FREQ_ENTRY_UNIT: return "Unknown unit. Use Hz, k, M or G";
        case DSD_FREQ_ENTRY_SUB_HZ: return "Finer than 1 Hz. Use at most 6 decimals in MHz";
        case DSD_FREQ_ENTRY_ZERO: return "Frequency must be above 0";
        case DSD_FREQ_ENTRY_AMBIGUOUS: return "Too large as MHz. Add a unit, e.g. 146520k or 146.52M";
        case DSD_FREQ_ENTRY_TOO_HIGH: return "Above the 4294.967295 MHz input limit";
        default: return "";
    }
}

int
dsd_freq_hz_mhz_text(uint32_t hz, char* out, size_t out_size) {
    if (out == NULL || out_size == 0u) {
        return -1;
    }
    return DSD_SNPRINTF(out, out_size, "%u.%06u", (unsigned)(hz / 1000000u), (unsigned)(hz % 1000000u));
}
