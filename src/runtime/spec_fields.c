// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The fields of a colon-separated spec. See spec_fields.h for the contract. */

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/spec_fields.h>

#include <string.h>

int
dsd_spec_split_fields(const char* spec, char* buf, size_t buf_size, char** fields, size_t max_fields) {
    if (!spec || !buf || !fields || max_fields == 0U) {
        return -1;
    }
    const size_t len = strlen(spec);
    if (len >= buf_size) {
        return -1;
    }
    DSD_MEMCPY(buf, spec, len + 1U);

    int count = 0;
    char* field = buf;
    for (;;) {
        if ((size_t)count < max_fields) {
            fields[count] = field;
        }
        count++;
        char* colon = strchr(field, ':');
        if (!colon) {
            return count;
        }
        *colon = '\0';
        field = colon + 1;
    }
}

int
dsd_spec_parse_port(const char* field, int min_port, int* out) {
    if (!field || !out || field[0] == '\0') {
        return -1;
    }
    /* Digits only: strtol() would also take leading blanks and a sign. */
    for (const char* p = field; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
    }
    int port = 0;
    if (dsd_parse_int_strict(field, 10, min_port, DSD_SPEC_PORT_MAX, &port) != 0) {
        return -1;
    }
    *out = port;
    return 0;
}
