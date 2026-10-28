// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Digit-pattern IDs: parse, validate, match and rank (issue #773).
 */

#include <dsd-neo/core/id_pattern.h>

#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"

enum {
    ID_PATTERN_CANON_MAX = DSD_ID_PATTERN_TEXT_MAX - 1, /* canonical characters */
    ID_DIGITS_MAX = DSD_ID_DECIMAL_MAX - 1              /* digits of the largest ID */
};

/* The largest ID, which bounds a ten-digit candidate. */
static const char k_id_bound[ID_DIGITS_MAX + 1] = "4294967295";

static int
id_is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static int
id_is_digit(char c) {
    return c >= '0' && c <= '9';
}

int
dsd_id_pattern_is_candidate(const char* token) {
    if (token == NULL) {
        return 0;
    }
    for (const char* c = token; *c != '\0'; c++) {
        if (*c == 'x' || *c == 'X' || *c == '*') {
            return 1;
        }
    }
    return 0;
}

/* Close a reachable set over the stars' empty match: a star at i lets the pattern move on to i + 1 without a digit. */
static void
id_pattern_close_stars(const char* text, size_t len, uint8_t reach[][2]) {
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '*') {
            reach[i + 1][0] |= reach[i][0];
            reach[i + 1][1] |= reach[i][1];
        }
    }
}

/*
 * Whether some canonical decimal of exactly `width` digits (no leading zero unless it is the single digit 0, and at
 * most 4294967295 when it has ten digits) matches the pattern. A set of reachable states walks the digit positions:
 * state (i, tight) means the pattern's first i characters matched the digits so far and, for a ten-digit candidate,
 * whether those digits still equal the bound's prefix (so the next digit is capped by the bound's).
 */
static int
id_pattern_matches_width(const char* text, size_t len, size_t width) {
    uint8_t reach[ID_PATTERN_CANON_MAX + 1][2];
    uint8_t next[ID_PATTERN_CANON_MAX + 1][2];
    DSD_MEMSET(reach, 0, sizeof(reach));
    reach[0][width == ID_DIGITS_MAX ? 1 : 0] = 1U;
    id_pattern_close_stars(text, len, reach);

    for (size_t pos = 0; pos < width; pos++) {
        DSD_MEMSET(next, 0, sizeof(next));
        const int lo = (pos == 0 && width > 1) ? 1 : 0;
        const int bound = k_id_bound[pos] - '0';
        int any = 0;
        for (size_t i = 0; i < len; i++) {
            for (int tight = 0; tight < 2; tight++) {
                if (reach[i][tight] == 0U) {
                    continue;
                }
                const int hi = tight ? bound : 9;
                const char pc = text[i];
                /* A star consumes the digit and stays; x or a literal consumes it and moves on. */
                const size_t to = (pc == '*') ? i : i + 1;
                for (int d = lo; d <= hi; d++) {
                    if (pc != '*' && pc != 'x' && d != pc - '0') {
                        continue;
                    }
                    next[to][(tight && d == bound) ? 1 : 0] = 1U;
                    any = 1;
                }
            }
        }
        if (!any) {
            return 0;
        }
        id_pattern_close_stars(text, len, next);
        DSD_MEMCPY(reach, next, sizeof(reach));
    }
    return reach[len][0] != 0U || reach[len][1] != 0U;
}

int
dsd_id_pattern_parse(const char* token, dsd_id_pattern* out) {
    if (out != NULL) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (token == NULL || out == NULL) {
        return 1;
    }

    const char* begin = token;
    while (*begin != '\0' && id_is_ascii_space(*begin)) {
        begin++;
    }
    const char* end = begin;
    while (*end != '\0') {
        end++;
    }
    while (end > begin && id_is_ascii_space(end[-1])) {
        end--;
    }

    char text[DSD_ID_PATTERN_TEXT_MAX];
    size_t len = 0;
    unsigned literals = 0;
    int has_star = 0;
    int has_x = 0;
    for (const char* c = begin; c < end; c++) {
        char ch = *c;
        if (ch == 'X') {
            ch = 'x';
        }
        if (ch == '*') {
            if (len > 0 && text[len - 1] == '*') {
                continue;
            }
            has_star = 1;
        } else if (ch == 'x') {
            has_x = 1;
        } else if (id_is_digit(ch)) {
            literals++;
        } else {
            return 1;
        }
        if (len >= ID_PATTERN_CANON_MAX) {
            return 1;
        }
        text[len++] = ch;
    }
    if (len == 0 || (!has_star && !has_x) || text[0] == '0') {
        return 1;
    }

    int matches_some_id = 0;
    for (size_t width = 1; width <= ID_DIGITS_MAX && !matches_some_id; width++) {
        matches_some_id = id_pattern_matches_width(text, len, width);
    }
    if (!matches_some_id) {
        return 1;
    }

    DSD_MEMCPY(out->text, text, len);
    out->text[len] = '\0';
    out->literals = (uint8_t)literals;
    out->has_star = (uint8_t)has_star;
    return 0;
}

size_t
dsd_id_decimal(uint32_t id, char* out, size_t out_size) {
    if (out == NULL) {
        return 0;
    }
    char rev[ID_DIGITS_MAX];
    size_t n = 0;
    do {
        rev[n++] = (char)('0' + (id % 10U));
        id /= 10U;
    } while (id != 0U && n < ID_DIGITS_MAX);
    if (out_size < n + 1U) {
        if (out_size > 0U) {
            out[0] = '\0';
        }
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = rev[n - 1U - i];
    }
    out[n] = '\0';
    return n;
}

int
dsd_id_pattern_match_decimal(const dsd_id_pattern* p, const char* digits, size_t len) {
    if (p == NULL || digits == NULL || len == 0U) {
        return 0;
    }
    size_t plen = 0;
    while (plen < ID_PATTERN_CANON_MAX && p->text[plen] != '\0') {
        plen++;
    }
    if (plen == 0U) {
        return 0;
    }

    /* Iterative glob: on a mismatch, retry from the last star with it consuming one more digit. */
    size_t pi = 0;
    size_t si = 0;
    size_t star = SIZE_MAX;
    size_t star_si = 0;
    while (si < len) {
        const char pc = (pi < plen) ? p->text[pi] : '\0';
        if (pc == '*') {
            star = pi++;
            star_si = si;
        } else if (pc != '\0' && (pc == 'x' ? id_is_digit(digits[si]) : pc == digits[si])) {
            pi++;
            si++;
        } else if (star != SIZE_MAX) {
            pi = star + 1U;
            si = ++star_si;
        } else {
            return 0;
        }
    }
    while (pi < plen && p->text[pi] == '*') {
        pi++;
    }
    return pi == plen;
}

int
dsd_id_pattern_match(const dsd_id_pattern* p, uint32_t id) {
    if (p == NULL) {
        return 0;
    }
    char digits[DSD_ID_DECIMAL_MAX];
    const size_t len = dsd_id_decimal(id, digits, sizeof(digits));
    return dsd_id_pattern_match_decimal(p, digits, len);
}

int
dsd_id_pattern_more_specific(const dsd_id_pattern* a, const dsd_id_pattern* b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->literals != b->literals) {
        return a->literals > b->literals;
    }
    return a->has_star == 0U && b->has_star != 0U;
}
