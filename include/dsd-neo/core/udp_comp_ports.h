// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 DSD-neo contributors
 *
 * @file
 * @brief UDP ports linked to the manufacturer-specific DMR compressed-header port indices.
 *
 * ETSI TS 102 361-3 tables 7.17/7.18 leave port indices 95-127 to the manufacturer, and note 2 makes
 * their linkage to a UDP port a per-radio setting, so nothing on the air names the port. A site that
 * knows its radios' linkage supplies it with `--dmr-udp-comp-port <idx>=<port>` or the
 * `mode.dmr_udp_comp_ports` config key; both fill `dsd_opts::dmr_udp_comp_ports` through these
 * helpers so the two entry points cannot drift apart. One table serves the SPID and the DPID, whose
 * tables are identical.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_UDP_COMP_PORTS_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_UDP_COMP_PORTS_H

#include <dsd-neo/core/lrrp_ports.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** First and last manufacturer-specific port index (tables 7.17/7.18: 1011111-1111111). */
#define DSD_UDP_COMP_PORT_IDX_FIRST 95U
#define DSD_UDP_COMP_PORT_IDX_LAST  127U
/** Entries in `dsd_opts::dmr_udp_comp_ports`, one per manufacturer-specific index. */
#define DSD_UDP_COMP_PORT_MAP_SIZE  (DSD_UDP_COMP_PORT_IDX_LAST - DSD_UDP_COMP_PORT_IDX_FIRST + 1U)

typedef enum {
    DSD_UDP_COMP_PORT_ADDED = 0, /**< Mapping stored. */
    DSD_UDP_COMP_PORT_SAME,      /**< The index already maps to this port; nothing changed. */
    DSD_UDP_COMP_PORT_CONFLICT,  /**< The index already maps to another port; nothing changed. */
    DSD_UDP_COMP_PORT_BAD_INDEX, /**< Index outside 95..127 (0-2 are ETSI-defined, 3-94 reserved). */
    DSD_UDP_COMP_PORT_BAD_PORT,  /**< Port outside 1..65535. */
    DSD_UDP_COMP_PORT_SYNTAX     /**< Not "<index>=<port>" in decimal. */
} dsd_udp_comp_port_result;

/**
 * @brief The UDP port a manufacturer-specific index is mapped to, or 0 when it is not mapped.
 */
static inline uint16_t
dsd_udp_comp_port_lookup(const uint16_t* map, unsigned idx) {
    if (!map || idx < DSD_UDP_COMP_PORT_IDX_FIRST || idx > DSD_UDP_COMP_PORT_IDX_LAST) {
        return 0U;
    }
    return map[idx - DSD_UDP_COMP_PORT_IDX_FIRST];
}

/**
 * @brief Store one index -> port mapping. The first port given for an index stays.
 */
static inline dsd_udp_comp_port_result
dsd_udp_comp_port_map_add(uint16_t* map, unsigned long idx, unsigned long port) {
    if (!map || idx < DSD_UDP_COMP_PORT_IDX_FIRST || idx > DSD_UDP_COMP_PORT_IDX_LAST) {
        return DSD_UDP_COMP_PORT_BAD_INDEX;
    }
    if (port == 0UL || port > 65535UL) {
        return DSD_UDP_COMP_PORT_BAD_PORT;
    }
    uint16_t* slot = &map[idx - DSD_UDP_COMP_PORT_IDX_FIRST];
    if (*slot == (uint16_t)port) {
        return DSD_UDP_COMP_PORT_SAME;
    }
    if (*slot != 0U) {
        return DSD_UDP_COMP_PORT_CONFLICT;
    }
    *slot = (uint16_t)port;
    return DSD_UDP_COMP_PORT_ADDED;
}

/*
 * Parse the decimal number in [begin, end), blanks around it allowed: plain digits only (no sign), at
 * most 15 of them, so the value cannot overflow and an out-of-range index or port is reported as such
 * rather than as a syntax error. Returns 0 on success.
 */
static inline int
dsd_udp_comp_port_parse_span(const char* begin, const char* end, uint64_t* out) {
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        begin++;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    if (begin == end) {
        return -1;
    }
    char digits[16];
    const size_t len = (size_t)(end - begin);
    if (len >= sizeof digits) {
        return -1;
    }
    for (const char* p = begin; p < end; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
    }
    DSD_MEMCPY(digits, begin, len);
    digits[len] = '\0';
    return dsd_parse_uint64_strict(digits, 10, UINT64_MAX, out) == 0 ? 0 : -1;
}

/**
 * @brief Parse and store one "<index>=<port>" entry (decimal, blanks around either side allowed).
 */
static inline dsd_udp_comp_port_result
dsd_udp_comp_port_map_add_text(uint16_t* map, const char* text) {
    if (!map || !text) {
        return DSD_UDP_COMP_PORT_SYNTAX;
    }
    const char* eq = strchr(text, '=');
    if (!eq || strchr(eq + 1, '=') != NULL) {
        return DSD_UDP_COMP_PORT_SYNTAX;
    }
    uint64_t idx = 0;
    uint64_t port = 0;
    if (dsd_udp_comp_port_parse_span(text, eq, &idx) != 0
        || dsd_udp_comp_port_parse_span(eq + 1, eq + 1 + strlen(eq + 1), &port) != 0) {
        return DSD_UDP_COMP_PORT_SYNTAX;
    }
    if (idx < DSD_UDP_COMP_PORT_IDX_FIRST || idx > DSD_UDP_COMP_PORT_IDX_LAST) {
        return DSD_UDP_COMP_PORT_BAD_INDEX;
    }
    if (port == 0U || port > 65535U) {
        return DSD_UDP_COMP_PORT_BAD_PORT;
    }
    return dsd_udp_comp_port_map_add(map, (unsigned long)idx, (unsigned long)port);
}

/**
 * @brief Parse a comma-separated list such as "97=4005,98=4007" into a cleared map.
 *
 * Blanks and empty entries are ignored, and an entry repeating a stored mapping is kept once.
 * Returns the number of entries that could not be taken: malformed, an index outside 95..127, a
 * port outside 1..65535, or a second port for an index already mapped (the first one stays).
 */
static inline int
dsd_udp_comp_port_list_parse(const char* list, uint16_t* map) {
    if (!map) {
        return 1;
    }
    DSD_MEMSET(map, 0, sizeof(uint16_t) * DSD_UDP_COMP_PORT_MAP_SIZE);
    int rejected = 0;
    const char* cursor = list ? list : "";
    char token[32];
    for (;;) {
        int have = dsd_lrrp_port_list_next_token(&cursor, token, sizeof token);
        if (have == 0) {
            break;
        }
        if (have < 0) {
            rejected++;
            continue;
        }
        dsd_udp_comp_port_result r = dsd_udp_comp_port_map_add_text(map, token);
        if (r != DSD_UDP_COMP_PORT_ADDED && r != DSD_UDP_COMP_PORT_SAME) {
            rejected++;
        }
    }
    return rejected;
}

/**
 * @brief Render a map as "95=4001,97=4005" in index order; an empty map renders "".
 */
static inline void
dsd_udp_comp_port_list_format(const uint16_t* map, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!map) {
        return;
    }
    size_t used = 0;
    for (unsigned i = 0; i < DSD_UDP_COMP_PORT_MAP_SIZE; i++) {
        if (map[i] == 0U) {
            continue;
        }
        int n = DSD_SNPRINTF(out + used, out_size - used, "%s%u=%u", used ? "," : "", DSD_UDP_COMP_PORT_IDX_FIRST + i,
                             (unsigned)map[i]);
        if (n < 0 || (size_t)n >= out_size - used) {
            out[used] = '\0';
            return;
        }
        used += (size_t)n;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_UDP_COMP_PORTS_H */
