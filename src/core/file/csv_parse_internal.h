// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Line and token helpers shared by the core CSV importers.
 *
 * Module-private: dsd_import.c (channel map, group list, key lists, mapping
 * CSVs) and p25_bandplan_csv.c parse with the same trimming, splitting and
 * strict number rules so every file kind rejects the same malformed cells.
 */

#ifndef DSD_NEO_SRC_CORE_FILE_CSV_PARSE_INTERNAL_H_
#define DSD_NEO_SRC_CORE_FILE_CSV_PARSE_INTERNAL_H_

#include <ctype.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/path_policy.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BSIZE               999
#define CSV_IMPORT_PATH_MAX 2048

static inline FILE*
csv_open_user_read_file(const char* label, const char* requested, char* resolved, size_t resolved_size) {
    if (!label || !requested || requested[0] == '\0' || !resolved || resolved_size == 0) {
        LOG_ERROR("CSV import path is missing.\n");
        return NULL;
    }

    FILE* fp = dsd_path_fopen_user_read_file(requested, resolved, resolved_size);
    if (fp == NULL) {
        LOG_ERROR("Unable to open %s '%s'\n", label, requested);
        return NULL;
    }
    return fp;
}

static inline void
trim_eol(char* s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static inline int
is_ascii_space(unsigned char c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
}

/*
 * A blank line is filler, not a data row. Editors and several exporters leave a
 * trailing empty line; counting it would make the dry-run validators report a
 * clean file as "N rows skipped" (and the group importer warn about it).
 */
static inline int
csv_line_is_blank(const char* s) {
    if (!s) {
        return 1;
    }
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; p++) {
        if (!is_ascii_space(*p)) {
            return 0;
        }
    }
    return 1;
}

static inline char*
trim_ws(char* s) {
    if (s == NULL) {
        return NULL;
    }
    size_t start = 0;
    size_t len = strlen(s);
    while (start < len && is_ascii_space((unsigned char)s[start])) {
        start++;
    }
    while (len > start && is_ascii_space((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s + start;
}

static inline const char*
skip_ascii_space(const char* token) {
    while (*token != '\0' && is_ascii_space((unsigned char)*token)) {
        token++;
    }
    return token;
}

static inline int
parse_hex_u64_strict(const char* token, unsigned long long* out) {
    if (token == NULL || out == NULL) {
        return 0;
    }

    token = skip_ascii_space(token);
    if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        token += 2;
    }

    const unsigned char* p = (const unsigned char*)token;
    const unsigned char* end = p;
    while (*end != '\0' && !is_ascii_space(*end)) {
        end++;
    }
    p = end;
    while (*p != '\0') {
        if (is_ascii_space(*p)) {
            p++;
            continue;
        }
        return 0;
    }

    uint64_t parsed = 0U;
    if (dsd_parse_hex_u64_n(token, (size_t)(end - (const unsigned char*)token), &parsed) != 0) {
        return 0;
    }
    *out = (unsigned long long)parsed;
    return 1;
}

static inline int
parse_dec_u64_strict(const char* token, unsigned long long* out) {
    char* end = NULL;
    unsigned long long v = 0ULL;
    if (token == NULL || out == NULL) {
        return 0;
    }
    while (*token != '\0' && is_ascii_space((unsigned char)*token)) {
        token++;
    }
    if (*token == '\0' || *token == '-' || *token == '+') {
        return 0;
    }
    errno = 0;
    v = strtoull(token, &end, 10);
    if (errno != 0 || end == token) {
        return 0;
    }
    while (*end != '\0' && is_ascii_space((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

static inline int
parse_dec_long_strict(const char* token, long int* out) {
    char* end = NULL;
    long int v = 0;
    if (token == NULL || out == NULL) {
        return 0;
    }
    while (*token != '\0' && is_ascii_space((unsigned char)*token)) {
        token++;
    }
    if (*token == '\0') {
        return 0;
    }
    errno = 0;
    v = strtol(token, &end, 10);
    if (errno != 0 || end == token) {
        return 0;
    }
    while (*end != '\0' && is_ascii_space((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

static inline size_t
csv_split_preserve_empty(char* line, char** fields, size_t max_fields) {
    size_t count = 0;
    char* p = line;
    if (!line || !fields || max_fields == 0) {
        return 0;
    }
    fields[count++] = p;
    while (*p != '\0') {
        if (*p == ',') {
            *p = '\0';
            if (count < max_fields) {
                fields[count++] = p + 1;
            }
        }
        p++;
    }
    return count;
}

static inline int
csv_ascii_casecmp(const char* a, const char* b) {
    if (!a || !b) {
        return (a == b) ? 0 : 1;
    }
    while (*a && *b) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/*
 * P25 band-plan CSV parse loop, shared by the importer and the dry-run validator
 * (dsd_csv_validate_p25_bandplan_file). Parses `path` into `rows` (capacity
 * DSD_P25_BANDPLAN_MAX_ROWS) and returns the stored row count, or -1 when the file
 * could not be opened. `stats`, when non-NULL, receives total/accepted counts and
 * silences the per-row diagnostics. `filename` receives the resolved path.
 */
struct p25_bandplan_row;
struct dsd_csv_validation;
int csv_p25_bandplan_parse_file(const char* path, struct p25_bandplan_row* rows, struct dsd_csv_validation* stats,
                                char* filename, size_t filename_size);

#endif /* DSD_NEO_SRC_CORE_FILE_CSV_PARSE_INTERNAL_H_ */
