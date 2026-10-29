// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/freq_parse.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "freq_parse_internal.h"

/* Strips a K, M or G suffix from buf and returns the decimal places it shifts the number by (9, 6, 3, or 0 without
   one); -1 for an empty buf. */
static int
freq_suffix_places(char* buf) {
    size_t len = strlen(buf);
    if (len == 0) {
        return -1;
    }

    char last = buf[len - 1];
    if (last == 'g' || last == 'G') {
        buf[len - 1] = '\0';
        return 9;
    }
    if (last == 'm' || last == 'M') {
        buf[len - 1] = '\0';
        return 6;
    }
    if (last == 'k' || last == 'K') {
        buf[len - 1] = '\0';
        return 3;
    }

    return 0;
}

static double
freq_places_factor(int places) {
    switch (places) {
        case 9: return 1e9;
        case 6: return 1e6;
        case 3: return 1e3;
        default: return 1.0;
    }
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
    const int places = freq_suffix_places(buf);
    if (places < 0) {
        return 0;
    }
    /* Plain decimal text rounds exactly: binary64 cannot hold the half-Hz ceiling's neighbours ("4294.9672955M" scales
       to 4294967295.4999995). Signs, exponents, hex and inf/nan keep the strtod() path below. */
    uint32_t exact = 0;
    if (dsd_freq_decimal_round_hz(buf, strlen(buf), (unsigned)places, &exact)) {
        return exact;
    }

    double val = 0.0;
    if (!parse_positive_double(buf, &val)) {
        return 0;
    }

    double hz = val * freq_places_factor(places);
    /* Written so NaN fails the test; the upper bound is the largest value that rounds to UINT32_MAX. */
    if (!(hz >= 0.5 && hz < 4294967295.5)) {
        return 0;
    }
    return (uint32_t)(hz + 0.5);
}

/* A bare number whose integer part has at most this many digits (leading zeros aside) is MHz, so
 * every bare MHz value is below DSD_FREQ_ENTRY_BARE_HZ_MIN (999999 is the largest 6-digit value). */
#define FREQ_ENTRY_BARE_MHZ_DIGITS 6u
_Static_assert(DSD_FREQ_ENTRY_BARE_HZ_MIN == 1000000u && FREQ_ENTRY_BARE_MHZ_DIGITS == 6u,
               "the bare-number digit rule and DSD_FREQ_ENTRY_BARE_HZ_MIN are 10^digits");

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

/* Bounds the entry to its first DSD_FREQ_ENTRY_MAX_LEN bytes, reading never more than one byte past the limit, and
 * trims ASCII spaces: [*begin, *end) is the rest. Returns DSD_FREQ_ENTRY_OK, TOO_LONG or EMPTY. */
static int
freq_entry_trim(const char* text, size_t* begin, size_t* end) {
    size_t len = 0;
    while (len <= (size_t)DSD_FREQ_ENTRY_MAX_LEN && text[len] != '\0') {
        len++;
    }
    if (len > (size_t)DSD_FREQ_ENTRY_MAX_LEN) {
        return DSD_FREQ_ENTRY_TOO_LONG;
    }

    size_t b = 0;
    size_t e = len;
    while (b < e && text[b] == ' ') {
        b++;
    }
    while (e > b && text[e - 1] == ' ') {
        e--;
    }
    if (b == e) {
        return DSD_FREQ_ENTRY_EMPTY;
    }
    *begin = b;
    *end = e;
    return DSD_FREQ_ENTRY_OK;
}

/* Where the number's digits sit in the entry text: [int_begin, int_end) before the '.', [frac_begin, frac_end) after
 * it (empty without one), and next, the first byte after the number. */
typedef struct {
    size_t int_begin;
    size_t int_end;
    size_t frac_begin;
    size_t frac_end;
    size_t next;
} freq_entry_number;

/* Scans the number [0-9]*('.'[0-9]*)? at text[begin..end). Returns 1 when it has at least one digit, else 0. */
static int
freq_entry_scan_number(const char* text, size_t begin, size_t end, freq_entry_number* num) {
    size_t pos = begin;
    num->int_begin = pos;
    while (pos < end && freq_entry_is_digit(text[pos])) {
        pos++;
    }
    num->int_end = pos;
    num->frac_begin = pos;
    num->frac_end = pos;
    if (pos < end && text[pos] == '.') {
        pos++;
        num->frac_begin = pos;
        while (pos < end && freq_entry_is_digit(text[pos])) {
            pos++;
        }
        num->frac_end = pos;
    }
    num->next = pos;
    return num->int_end != num->int_begin || num->frac_end != num->frac_begin;
}

/* The places of a number with no unit: 6 (MHz) when its integer part has at most FREQ_ENTRY_BARE_MHZ_DIGITS digits
 * (leading zeros aside), else 0 (Hz). Returns DSD_FREQ_ENTRY_OK, or AMBIGUOUS for an integer part too large for MHz. */
static int
freq_entry_bare_places(const char* text, const freq_entry_number* num, size_t* places) {
    size_t lead = num->int_begin;
    while (lead < num->int_end && text[lead] == '0') {
        lead++;
    }
    if (num->int_end - lead > FREQ_ENTRY_BARE_MHZ_DIGITS) {
        *places = 0;
        return DSD_FREQ_ENTRY_OK;
    }
    /* MHz. Refuse an integer part that cannot be MHz before any arithmetic, so a sub-Hz or fractional tail never
     * changes the answer. */
    uint32_t whole = 0;
    for (size_t i = lead; i < num->int_end; i++) {
        whole = whole * 10u + (uint32_t)(text[i] - '0');
    }
    if (whole > UINT32_MAX / 1000000u) {
        return DSD_FREQ_ENTRY_AMBIGUOUS;
    }
    *places = 6;
    return DSD_FREQ_ENTRY_OK;
}

/* The places of the unit text[unit..end), which is not empty. Returns DSD_FREQ_ENTRY_OK, SYNTAX for anything but
 * letters, or UNIT for letters that are not a unit. */
static int
freq_entry_unit_text_places(const char* text, size_t unit, size_t end, size_t* places) {
    for (size_t i = unit; i < end; i++) {
        if (!freq_entry_is_letter(text[i])) {
            return DSD_FREQ_ENTRY_SYNTAX;
        }
    }
    const int unit_places = freq_entry_unit_places(text + unit, end - unit);
    if (unit_places < 0) {
        return DSD_FREQ_ENTRY_UNIT;
    }
    *places = (size_t)unit_places;
    return DSD_FREQ_ENTRY_OK;
}

/* *value = the number in units of 10^-places: the integer digits, then exactly `places` fraction digits (zero-padded).
 * Returns DSD_FREQ_ENTRY_OK, TOO_HIGH above UINT32_MAX, or SUB_HZ for a non-zero fraction digit past `places`. */
static int
freq_entry_accumulate(const char* text, const freq_entry_number* num, size_t places, uint32_t* value) {
    uint32_t v = 0;
    for (size_t i = num->int_begin; i < num->int_end; i++) {
        if (!freq_entry_push_digit(&v, (uint32_t)(text[i] - '0'))) {
            return DSD_FREQ_ENTRY_TOO_HIGH;
        }
    }
    for (size_t k = 0; k < places; k++) {
        const size_t i = num->frac_begin + k;
        const uint32_t digit = i < num->frac_end ? (uint32_t)(text[i] - '0') : 0u;
        if (!freq_entry_push_digit(&v, digit)) {
            return DSD_FREQ_ENTRY_TOO_HIGH;
        }
    }
    /* Fraction digits past the unit's places must be zeros. The range wins over the sub-Hz
     * refusal: at UINT32_MAX any non-zero tail is above the limit. */
    for (size_t i = num->frac_begin + places; i < num->frac_end; i++) {
        if (text[i] != '0') {
            return v == UINT32_MAX ? DSD_FREQ_ENTRY_TOO_HIGH : DSD_FREQ_ENTRY_SUB_HZ;
        }
    }
    *value = v;
    return DSD_FREQ_ENTRY_OK;
}

/* The number in whole Hz, scaled by 10^places and rounded half up on the first fraction digit past them (later digits
   cannot change a half-up decision); 0 when that is above UINT32_MAX. */
static uint32_t
freq_decimal_round(const char* text, const freq_entry_number* num, size_t places) {
    uint32_t v = 0;
    for (size_t i = num->int_begin; i < num->int_end; i++) {
        if (!freq_entry_push_digit(&v, (uint32_t)(text[i] - '0'))) {
            return 0;
        }
    }
    for (size_t k = 0; k < places; k++) {
        const size_t i = num->frac_begin + k;
        if (!freq_entry_push_digit(&v, i < num->frac_end ? (uint32_t)(text[i] - '0') : 0u)) {
            return 0;
        }
    }
    const size_t next = num->frac_begin + places;
    if (next < num->frac_end && text[next] >= '5') {
        if (v == UINT32_MAX) {
            return 0;
        }
        v++;
    }
    return v;
}

int
dsd_freq_decimal_round_hz(const char* text, size_t len, unsigned places, uint32_t* out_hz) {
    if (text == NULL || out_hz == NULL || places > 9u) {
        return 0;
    }
    freq_entry_number num = {0, 0, 0, 0, 0};
    if (!freq_entry_scan_number(text, 0, len, &num) || num.next != len) {
        return 0;
    }
    *out_hz = freq_decimal_round(text, &num, places);
    return 1;
}

int
dsd_parse_freq_entry_hz(const char* text, uint32_t* out_hz) {
    if (text == NULL || out_hz == NULL) {
        return DSD_FREQ_ENTRY_EMPTY;
    }

    size_t begin = 0;
    size_t end = 0;
    int result = freq_entry_trim(text, &begin, &end);
    if (result != DSD_FREQ_ENTRY_OK) {
        return result;
    }
    /* Before the grammar, so "146,52 MHz" gets the decimal-comma message. */
    if (memchr(text + begin, ',', end - begin) != NULL) {
        return DSD_FREQ_ENTRY_DECIMAL_COMMA;
    }

    freq_entry_number num = {0, 0, 0, 0, 0};
    if (!freq_entry_scan_number(text, begin, end, &num)) {
        return DSD_FREQ_ENTRY_SYNTAX;
    }
    size_t unit = num.next;
    while (unit < end && text[unit] == ' ') {
        unit++;
    }
    size_t places = 0;
    /* No unit when only spaces follow the number (trailing spaces are trimmed, so nothing does). */
    result = (unit == end) ? freq_entry_bare_places(text, &num, &places)
                           : freq_entry_unit_text_places(text, unit, end, &places);
    if (result != DSD_FREQ_ENTRY_OK) {
        return result;
    }

    uint32_t value = 0;
    result = freq_entry_accumulate(text, &num, places, &value);
    if (result != DSD_FREQ_ENTRY_OK) {
        return result;
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
