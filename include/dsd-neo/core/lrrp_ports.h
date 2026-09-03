// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 DSD-neo contributors
 *
 * @file
 * @brief Extra UDP ports routed onto the DMR LRRP decoder.
 *
 * `--lrrp-extra-port` and the `mode.dmr_lrrp_ports` config key both fill
 * `dsd_opts::lrrp_extra_ports`; these helpers own the range check, the table
 * cap and duplicate suppression so the two entry points cannot drift apart.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_LRRP_PORTS_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_LRRP_PORTS_H

#include <dsd-neo/core/parse.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_LRRP_PORT_ADDED = 0, /**< Port appended to the table. */
    DSD_LRRP_PORT_DUPLICATE, /**< Port already present; nothing changed. */
    DSD_LRRP_PORT_RANGE,     /**< Port outside 1..65535. */
    DSD_LRRP_PORT_FULL       /**< Table already holds `cap` ports. */
} dsd_lrrp_port_add_result;

/**
 * @brief Append one port to a port table, refusing out-of-range and duplicate values.
 */
static inline dsd_lrrp_port_add_result
dsd_lrrp_port_list_add(uint16_t* ports, int* count, int cap, unsigned long port) {
    if (!ports || !count || port == 0UL || port > 65535UL) {
        return DSD_LRRP_PORT_RANGE;
    }
    for (int i = 0; i < *count; i++) {
        if (ports[i] == (uint16_t)port) {
            return DSD_LRRP_PORT_DUPLICATE;
        }
    }
    if (*count >= cap) {
        return DSD_LRRP_PORT_FULL;
    }
    ports[(*count)++] = (uint16_t)port;
    return DSD_LRRP_PORT_ADDED;
}

/*
 * Copy the next comma-separated entry of *cursor into token, trimming blanks around it and
 * skipping empty entries. Returns 0 at the end of the input, 1 with a token, -1 when the entry
 * does not fit (the cursor still moves past it).
 */
static inline int
dsd_lrrp_port_list_next_token(const char** cursor, char* token, size_t cap) {
    const char* s = *cursor;
    while (*s == ' ' || *s == '\t' || *s == ',') {
        s++;
    }
    if (!*s) {
        *cursor = s;
        token[0] = '\0';
        return 0;
    }
    size_t len = 0;
    int too_long = 0;
    while (*s && *s != ',') {
        if (len + 1 < cap) {
            token[len++] = *s;
        } else {
            too_long = 1;
        }
        s++;
    }
    while (len > 0 && (token[len - 1] == ' ' || token[len - 1] == '\t')) {
        len--;
    }
    token[len] = '\0';
    *cursor = s;
    return too_long ? -1 : 1;
}

/**
 * @brief Parse a comma-separated port list such as "5000,5001" into a port table.
 *
 * Blanks around entries and empty entries are ignored; duplicates are kept once.
 * The table is rebuilt from scratch. Returns the number of entries that could not
 * be taken: not a decimal number, outside 1..65535, or beyond `cap`.
 */
static inline int
dsd_lrrp_port_list_parse(const char* list, uint16_t* ports, int cap, int* count) {
    if (!ports || !count) {
        return 1;
    }
    *count = 0;
    int rejected = 0;
    const char* cursor = list ? list : "";
    char token[16];
    for (;;) {
        int have = dsd_lrrp_port_list_next_token(&cursor, token, sizeof token);
        if (have == 0) {
            break;
        }
        uint16_t port = 0;
        if (have < 0 || dsd_parse_uint16_strict(token, 10, 65535U, &port) != 0) {
            rejected++;
            continue;
        }
        dsd_lrrp_port_add_result r = dsd_lrrp_port_list_add(ports, count, cap, (unsigned long)port);
        if (r == DSD_LRRP_PORT_RANGE || r == DSD_LRRP_PORT_FULL) {
            rejected++;
        }
    }
    return rejected;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_LRRP_PORTS_H */
