// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_PARSE_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_PARSE_H

/**
 * @file
 * @brief Strict scalar parsing helpers for project-owned input handling.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

static inline int
dsd_parse_base_is_valid(int base) {
    return base == 0 || (base >= 2 && base <= 36);
}

static inline int
dsd_parse_long_strict(const char* text, int base, long min_value, long max_value, long* out) {
    if (!text || !out || text[0] == '\0' || min_value > max_value || !dsd_parse_base_is_valid(base)) {
        return -1;
    }

    errno = 0;
    char* end = NULL;
    long value = strtol(text, &end, base);
    if (errno != 0 || end == text || !end || *end != '\0' || value < min_value || value > max_value) {
        return -1;
    }

    *out = value;
    return 0;
}

static inline int
dsd_parse_int_strict(const char* text, int base, int min_value, int max_value, int* out) {
    if (!out || min_value > max_value) {
        return -1;
    }

    long parsed = 0;
    if (dsd_parse_long_strict(text, base, (long)min_value, (long)max_value, &parsed) != 0) {
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static inline int
dsd_parse_uint64_strict(const char* text, int base, uint64_t max_value, uint64_t* out) {
    if (!text || !out || text[0] == '\0' || text[0] == '-' || !dsd_parse_base_is_valid(base)) {
        return -1;
    }

    errno = 0;
    char* end = NULL;
    unsigned long long value = strtoull(text, &end, base);
    if (errno != 0 || end == text || !end || *end != '\0' || (uint64_t)value > max_value) {
        return -1;
    }

    *out = (uint64_t)value;
    return 0;
}

static inline int
dsd_parse_uint32_strict(const char* text, int base, uint32_t max_value, uint32_t* out) {
    if (!out) {
        return -1;
    }

    uint64_t parsed = 0;
    if (dsd_parse_uint64_strict(text, base, (uint64_t)max_value, &parsed) != 0) {
        return -1;
    }
    *out = (uint32_t)parsed;
    return 0;
}

static inline int
dsd_parse_uint16_strict(const char* text, int base, uint16_t max_value, uint16_t* out) {
    if (!out) {
        return -1;
    }

    uint64_t parsed = 0;
    if (dsd_parse_uint64_strict(text, base, (uint64_t)max_value, &parsed) != 0) {
        return -1;
    }
    *out = (uint16_t)parsed;
    return 0;
}

static inline int
dsd_parse_uint8_strict(const char* text, int base, uint8_t max_value, uint8_t* out) {
    if (!out) {
        return -1;
    }

    uint64_t parsed = 0;
    if (dsd_parse_uint64_strict(text, base, (uint64_t)max_value, &parsed) != 0) {
        return -1;
    }
    *out = (uint8_t)parsed;
    return 0;
}

static inline int
dsd_parse_double_strict(const char* text, double min_value, double max_value, double* out) {
    if (!text || !out || text[0] == '\0' || min_value > max_value) {
        return -1;
    }

    errno = 0;
    char* end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || !end || *end != '\0' || value < min_value || value > max_value) {
        return -1;
    }

    *out = value;
    return 0;
}

static inline int
dsd_parse_binary_u64_n(const char* bits, size_t bit_count, uint64_t* out) {
    if (!bits || !out || bit_count == 0 || bit_count > 64) {
        return -1;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < bit_count; i++) {
        unsigned int bit = 0;
        if (bits[i] == '0' || bits[i] == 0) {
            bit = 0;
        } else if (bits[i] == '1' || bits[i] == 1) {
            bit = 1;
        } else {
            return -1;
        }
        value = (value << 1U) | (uint64_t)bit;
    }

    *out = value;
    return 0;
}

static inline int
dsd_parse_binary_u32_n(const char* bits, size_t bit_count, uint32_t* out) {
    if (!out || bit_count > 32) {
        return -1;
    }

    uint64_t parsed = 0;
    if (dsd_parse_binary_u64_n(bits, bit_count, &parsed) != 0) {
        return -1;
    }
    *out = (uint32_t)parsed;
    return 0;
}

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_PARSE_H */
