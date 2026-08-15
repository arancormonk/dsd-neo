// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Ownership, growth and small parsing helpers for the RadioReference types.
 *
 * Compiled unconditionally: none of this needs curl or expat, and the Qt model
 * and the generators use the free functions whether or not a transport exists.
 */

#include "rr_internal.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

int
dsd_rr_available(void) {
#if defined(USE_CURL) && defined(USE_EXPAT)
    return 1;
#else
    return 0;
#endif
}

int
rr_array_reserve(void** items, size_t* cap, size_t needed, size_t elem_size) {
    if (items == NULL || cap == NULL || elem_size == 0U) {
        return -1;
    }
    if (needed <= *cap) {
        return 0;
    }

    size_t next = (*cap == 0U) ? 16U : *cap;
    while (next < needed) {
        if (next > SIZE_MAX / 2U) {
            next = needed;
            break;
        }
        next *= 2U;
    }
    if (next > SIZE_MAX / elem_size) {
        return -1;
    }

    void* grown = realloc(*items, next * elem_size);
    if (grown == NULL) {
        return -1;
    }
    DSD_MEMSET((char*)grown + (*cap * elem_size), 0, (next - *cap) * elem_size);
    *items = grown;
    *cap = next;
    return 0;
}

/**
 * @brief Skip leading ASCII whitespace.
 *
 * @param p Input pointer.
 * @return First non-space character.
 */
static const char*
rr_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') {
        p++;
    }
    return p;
}

int
rr_parse_long_strict(const char* text, long* out) {
    if (text == NULL || out == NULL) {
        return -1;
    }

    const char* start = rr_skip_ws(text);
    if (*start == '\0') {
        return -1;
    }

    char* end = NULL;
    errno = 0;
    const long value = strtol(start, &end, 10);
    if (errno != 0 || end == start) {
        return -1;
    }
    if (*rr_skip_ws(end) != '\0') {
        return -1;
    }

    *out = value;
    return 0;
}

int
rr_parse_hex_strict(const char* text, unsigned long long* out) {
    if (text == NULL || out == NULL) {
        return -1;
    }

    const char* start = rr_skip_ws(text);
    if (*start == '\0' || *start == '-' || *start == '+') {
        return -1;
    }

    char* end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(start, &end, 16);
    if (errno != 0 || end == start) {
        return -1;
    }
    if (*rr_skip_ws(end) != '\0') {
        return -1;
    }

    *out = value;
    return 0;
}

void
rr_copy_field(char* dst, size_t dst_sz, const char* src) {
    if (dst == NULL || dst_sz == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    (void)DSD_SNPRINTF(dst, dst_sz, "%s", src);
}

/**
 * @brief Consume a run of ASCII digits.
 *
 * @param p Address of the cursor, advanced past the digits.
 * @return Number of digits consumed.
 */
static size_t
rr_scan_digits(const char** p) {
    const char* start = *p;
    while (**p >= '0' && **p <= '9') {
        (*p)++;
    }
    return (size_t)(*p - start);
}

/**
 * @brief Convert a run of digits to a value, assuming it is already bounded.
 *
 * @param start  First digit.
 * @param digits Digit count.
 * @return The value.
 */
static long long
rr_digits_value(const char* start, size_t digits) {
    long long value = 0;
    for (size_t i = 0; i < digits; i++) {
        value = (value * 10) + (start[i] - '0');
    }
    return value;
}

/**
 * @brief Convert a decimal fraction to whole microunits.
 *
 * Trailing zeros past the sixth digit are harmless and accepted; real precision
 * past six digits is rejected rather than silently rounded away.
 *
 * @param frac   First fractional digit.
 * @param digits Fractional digit count.
 * @param out    Receives the value scaled to 1e-6.
 * @return 0 on success, -1 when the fraction carries more precision than fits.
 */
static int
rr_fraction_to_micro(const char* frac, size_t digits, long long* out) {
    size_t significant = digits;
    while (significant > 6U && frac[significant - 1U] == '0') {
        significant--;
    }
    if (significant > 6U) {
        return -1;
    }

    long long micro = 0;
    for (size_t i = 0; i < 6U; i++) {
        micro = (micro * 10) + ((i < digits) ? (frac[i] - '0') : 0);
    }
    *out = micro;
    return 0;
}

int
dsd_rr_mhz_to_hz(const char* mhz_text, long long* out_hz) {
    if (mhz_text == NULL || out_hz == NULL) {
        return -1;
    }

    const char* p = rr_skip_ws(mhz_text);
    /* A sign is never valid for a frequency, and would silently invert the math. */
    if (*p == '+' || *p == '-') {
        return -1;
    }

    const char* int_start = p;
    const size_t int_digits = rr_scan_digits(&p);
    if (int_digits == 0U || int_digits > 6U) {
        return -1;
    }

    long long micro = 0;
    if (*p == '.') {
        p++;
        const char* frac_start = p;
        const size_t frac_digits = rr_scan_digits(&p);
        if (frac_digits == 0U || rr_fraction_to_micro(frac_start, frac_digits, &micro) != 0) {
            return -1;
        }
    }

    if (*rr_skip_ws(p) != '\0') {
        return -1;
    }

    *out_hz = (rr_digits_value(int_start, int_digits) * 1000000LL) + micro;
    return 0;
}

const char*
dsd_rr_support_lookup(const dsd_rr_support_list* list, int stype, int id) {
    if (list == NULL || list->items == NULL) {
        return "";
    }
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].stype == stype && list->items[i].id == id) {
            return list->items[i].descr;
        }
    }
    return "";
}

int
dsd_rr_warning_list_add(dsd_rr_warning_list* list, const char* text) {
    if (list == NULL || text == NULL) {
        return -1;
    }

    dsd_rr_warning* grown = (dsd_rr_warning*)realloc(list->items, (list->count + 1U) * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    list->items = grown;

    dsd_rr_warning* slot = &list->items[list->count];
    DSD_MEMSET(slot, 0, sizeof(*slot));
    /* Truncation is acceptable: this is display text, not data. */
    rr_copy_field(slot->text, sizeof(slot->text), text);
    list->count++;
    return 0;
}

void
dsd_rr_warning_list_free(dsd_rr_warning_list* list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void
dsd_rr_country_list_free(dsd_rr_country_list* list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void
dsd_rr_state_list_free(dsd_rr_state_list* list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void
dsd_rr_county_list_free(dsd_rr_county_list* list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void
dsd_rr_trs_list_free(dsd_rr_trs_list* list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void
dsd_rr_support_list_free(dsd_rr_support_list* list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void
dsd_rr_support_maps_free(dsd_rr_support_maps* maps) {
    if (maps == NULL) {
        return;
    }
    dsd_rr_support_list_free(&maps->types);
    dsd_rr_support_list_free(&maps->flavors);
    dsd_rr_support_list_free(&maps->voices);
}

void
dsd_rr_site_list_free(dsd_rr_site_list* list) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].freqs);
        list->items[i].freqs = NULL;
        list->items[i].freq_count = 0;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void
dsd_rr_talkgroup_list_free(dsd_rr_talkgroup_list* list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void
dsd_rr_talkgroup_cat_list_free(dsd_rr_talkgroup_cat_list* list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void
dsd_rr_trs_details_free(dsd_rr_trs_details* details) {
    if (details == NULL) {
        return;
    }
    free(details->sysids);
    details->sysids = NULL;
    details->sysid_count = 0;
}
