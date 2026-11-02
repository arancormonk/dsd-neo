// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The trunk-scan target lists a session loads (issue #697).
 *
 * `--trunk-scan` and `--trunk-scan-off` add one list each, `[trunk_scan] targets_csv`, `targets_csv_2` ...
 * `targets_csv_8` name them in a config and `lists_off` says which start switched off. dsd_opts, its next-start hold
 * and the user config keep the same fixed array, so the CLI, the loader, a save and a config apply cannot drift apart
 * on what a list is or which list numbers a config may name.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_TRUNK_SCAN_LISTS_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_TRUNK_SCAN_LISTS_H

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* At most this many lists in one session; list N is the Nth --trunk-scan/--trunk-scan-off, or targets_csv_N. */
#define DSD_TRUNK_SCAN_MAX_LISTS 8

/** One configured target list: its CSV, and whether the session starts with it switched off. */
typedef struct {
    char path[1024];
    uint8_t start_off;
} dsd_trunk_scan_list_spec;

/**
 * @brief How many list numbers @p lists uses: the highest that has a path or starts off.
 *
 * A config may name a list number with no path (a gap, or a lists_off entry past the last list); counting it keeps
 * that visible to the start rules (dsd_trunk_scan_lists_check()) instead of dropping it.
 */
static inline int
dsd_trunk_scan_lists_span(const dsd_trunk_scan_list_spec* lists) {
    int span = 0;
    for (int i = 0; lists != NULL && i < DSD_TRUNK_SCAN_MAX_LISTS; i++) {
        if (lists[i].path[0] != '\0' || lists[i].start_off != 0U) {
            span = i + 1;
        }
    }
    return span;
}

/** Whether two list specs are the same: the same paths and start states in the same order. */
static inline int
dsd_trunk_scan_lists_equal(const dsd_trunk_scan_list_spec* a, int a_count, const dsd_trunk_scan_list_spec* b,
                           int b_count) {
    if (a_count != b_count) {
        return 0;
    }
    for (int i = 0; i < a_count && i < DSD_TRUNK_SCAN_MAX_LISTS; i++) {
        if (strcmp(a[i].path, b[i].path) != 0 || (a[i].start_off != 0U) != (b[i].start_off != 0U)) {
            return 0;
        }
    }
    return 1;
}

/** What keeps a list spec from starting a trunk scan (dsd_trunk_scan_lists_check()). */
typedef enum {
    DSD_TRUNK_SCAN_LISTS_OK = 0,
    DSD_TRUNK_SCAN_LISTS_NO_FIRST,  /**< List 1 has no path. */
    DSD_TRUNK_SCAN_LISTS_GAP,       /**< A list has no path, but a later one has. */
    DSD_TRUNK_SCAN_LISTS_OFF_RANGE, /**< A list past the last one with a path starts off. */
    DSD_TRUNK_SCAN_LISTS_ALL_OFF,   /**< Every list starts off. */
} dsd_trunk_scan_lists_fault;

static inline int
dsd_trunk_scan_lists_has_path_from(const dsd_trunk_scan_list_spec* lists, int count, int from) {
    for (int j = from; j < count; j++) {
        if (lists[j].path[0] != '\0') {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief The first rule @p count lists break that startup refuses a trunk scan by.
 *
 * @param out_list [out] The 1-based list the fault names (0 for none or every list); may be NULL.
 */
static inline dsd_trunk_scan_lists_fault
dsd_trunk_scan_lists_check(const dsd_trunk_scan_list_spec* lists, int count, int* out_list) {
    if (out_list != NULL) {
        *out_list = 0;
    }
    if (lists == NULL || count <= 0 || lists[0].path[0] == '\0') {
        if (out_list != NULL) {
            *out_list = 1;
        }
        return DSD_TRUNK_SCAN_LISTS_NO_FIRST;
    }
    int on = 0;
    for (int i = 0; i < count && i < DSD_TRUNK_SCAN_MAX_LISTS; i++) {
        if (lists[i].path[0] == '\0') {
            if (out_list != NULL) {
                *out_list = i + 1;
            }
            return dsd_trunk_scan_lists_has_path_from(lists, count, i + 1) ? DSD_TRUNK_SCAN_LISTS_GAP
                                                                           : DSD_TRUNK_SCAN_LISTS_OFF_RANGE;
        }
        on += lists[i].start_off == 0U;
    }
    return on > 0 ? DSD_TRUNK_SCAN_LISTS_OK : DSD_TRUNK_SCAN_LISTS_ALL_OFF;
}

/**
 * @brief Say why startup refuses @p count lists (dsd_trunk_scan_lists_check()), as the CLI and the engine report it.
 *
 * @return 0 when nothing is refused (@p why is then ""); -1 otherwise.
 */
static inline int
dsd_trunk_scan_lists_describe_fault(const dsd_trunk_scan_list_spec* lists, int count, char* why, size_t why_size) {
    int list = 0;
    const dsd_trunk_scan_lists_fault fault = dsd_trunk_scan_lists_check(lists, count, &list);
    if (why == NULL || why_size == 0U) {
        return fault == DSD_TRUNK_SCAN_LISTS_OK ? 0 : -1;
    }
    switch (fault) {
        case DSD_TRUNK_SCAN_LISTS_OK: why[0] = '\0'; return 0;
        case DSD_TRUNK_SCAN_LISTS_NO_FIRST:
            DSD_SNPRINTF(why, why_size, "--trunk-scan requires a target CSV path");
            break;
        case DSD_TRUNK_SCAN_LISTS_GAP:
            DSD_SNPRINTF(why, why_size,
                         "trunk-scan list %d has no target CSV ([trunk_scan] targets_csv_%d); lists are "
                         "numbered without gaps",
                         list, list);
            break;
        case DSD_TRUNK_SCAN_LISTS_OFF_RANGE:
            DSD_SNPRINTF(why, why_size, "[trunk_scan] lists_off names list %d, which has no target CSV", list);
            break;
        default: DSD_SNPRINTF(why, why_size, "every trunk-scan list starts off; at least one must start on"); break;
    }
    return -1;
}

static inline const char*
dsd_trunk_scan_lists_skip_blanks(const char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* One lists_off entry at *cursor, up to a ',' or the end; *more says a ',' followed it (and was stepped over). */
static inline int
dsd_trunk_scan_lists_off_entry(const char** cursor, int* out_list, int* more) {
    const char* s = dsd_trunk_scan_lists_skip_blanks(*cursor);
    char token[8];
    size_t len = 0U;
    while (*s != '\0' && *s != ',' && len + 1U < sizeof token) {
        token[len++] = *s++;
    }
    if (*s != '\0' && *s != ',') {
        return -1; /* longer than any list number */
    }
    *more = *s == ',';
    *cursor = *more ? s + 1 : s;
    while (len > 0U && (token[len - 1U] == ' ' || token[len - 1U] == '\t')) {
        len--;
    }
    token[len] = '\0';
    if (token[0] < '0' || token[0] > '9') {
        return -1; /* empty, or a sign strtol would take */
    }
    return dsd_parse_int_strict(token, 10, 1, DSD_TRUNK_SCAN_MAX_LISTS, out_list);
}

/**
 * @brief Parse a lists_off value ("2, 3"): list numbers 1..DSD_TRUNK_SCAN_MAX_LISTS, comma-separated, each once.
 *
 * Blanks around a number are allowed; an empty value names no list. Whether each number has a list is the start rules'
 * question, asked of the composed config.
 *
 * @param out_mask [out] Bit N-1 set for list N; left alone on failure.
 * @return 0 on success; -1 on an entry that is no list number, an empty entry or a repeat.
 */
static inline int
dsd_trunk_scan_lists_off_parse(const char* text, uint32_t* out_mask) {
    uint32_t mask = 0U;
    const char* s = dsd_trunk_scan_lists_skip_blanks(text != NULL ? text : "");
    int more = *s != '\0';
    while (more) {
        int list = 0;
        if (dsd_trunk_scan_lists_off_entry(&s, &list, &more) != 0 || (mask & (1U << (list - 1))) != 0U) {
            return -1;
        }
        mask |= 1U << (list - 1);
    }
    if (out_mask != NULL) {
        *out_mask = mask;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_TRUNK_SCAN_LISTS_H */
