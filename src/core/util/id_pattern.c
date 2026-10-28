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

/* One reachable state (i, tight) of id_pattern_matches_width() takes the next digit (at least @p lo, at most @p bound
 * while tight): pattern character @p pc at i marks the states it reaches in @p next. Returns 1 when it reaches any. */
static int
id_pattern_step(char pc, size_t i, int tight, int lo, int bound, uint8_t next[][2]) {
    const int hi = tight ? bound : 9;
    /* A star consumes the digit and stays; x or a literal consumes it and moves on. */
    const size_t to = (pc == '*') ? i : i + 1;
    const int any_digit = pc == '*' || pc == 'x';
    int any = 0;
    for (int d = lo; d <= hi; d++) {
        if (!any_digit && d != pc - '0') {
            continue;
        }
        next[to][(tight && d == bound) ? 1 : 0] = 1U;
        any = 1;
    }
    return any;
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
                if (reach[i][tight] != 0U) {
                    any |= id_pattern_step(text[i], i, tight, lo, bound, next);
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

/* The token without surrounding ASCII space: [*begin, *end). */
static void
id_trim(const char* token, const char** begin, const char** end) {
    const char* b = token;
    while (*b != '\0' && id_is_ascii_space(*b)) {
        b++;
    }
    const char* e = b;
    while (*e != '\0') {
        e++;
    }
    while (e > b && id_is_ascii_space(e[-1])) {
        e--;
    }
    *begin = b;
    *end = e;
}

/* Normalise [begin, end) into @p out (lower-case x, single stars) and count what it holds. Returns 1 for a character
 * that is not a digit, x or star, or for more than ID_PATTERN_CANON_MAX characters. */
static int
id_pattern_normalize(const char* begin, const char* end, dsd_id_pattern* out, size_t* len_out, int* has_x) {
    size_t len = 0;
    for (const char* c = begin; c < end; c++) {
        const char ch = (*c == 'X') ? 'x' : *c;
        if (ch == '*' && len > 0 && out->text[len - 1] == '*') {
            continue;
        }
        if (ch == '*') {
            out->has_star = 1U;
        } else if (ch == 'x') {
            *has_x = 1;
        } else if (id_is_digit(ch)) {
            out->literals++;
        } else {
            return 1;
        }
        if (len >= ID_PATTERN_CANON_MAX) {
            return 1;
        }
        out->text[len++] = ch;
    }
    out->text[len] = '\0';
    *len_out = len;
    return 0;
}

/* Whether some ID in 0..4294967295, written in decimal, matches the canonical pattern @p text. */
static int
id_pattern_matches_some_id(const char* text, size_t len) {
    for (size_t width = 1; width <= ID_DIGITS_MAX; width++) {
        if (id_pattern_matches_width(text, len, width)) {
            return 1;
        }
    }
    return 0;
}

int
dsd_id_pattern_parse(const char* token, dsd_id_pattern* out) {
    if (out != NULL) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (token == NULL || out == NULL) {
        return 1;
    }

    const char* begin = NULL;
    const char* end = NULL;
    id_trim(token, &begin, &end);
    dsd_id_pattern parsed;
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    size_t len = 0;
    int has_x = 0;
    if (id_pattern_normalize(begin, end, &parsed, &len, &has_x) != 0 || len == 0 || (parsed.has_star == 0U && !has_x)
        || parsed.text[0] == '0' || !id_pattern_matches_some_id(parsed.text, len)) {
        return 1;
    }
    *out = parsed;
    return 0;
}

void
dsd_id_pattern_from_canonical(const char* text, dsd_id_pattern* out) {
    if (out == NULL) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (text == NULL) {
        return;
    }
    for (size_t i = 0; i + 1U < sizeof(out->text) && text[i] != '\0'; i++) {
        const char c = text[i];
        out->text[i] = c;
        if (c == '*') {
            out->has_star = 1U;
        } else if (id_is_digit(c)) {
            out->literals++;
        }
    }
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

/* Length of a pattern's text, at most ID_PATTERN_CANON_MAX. */
static size_t
id_pattern_text_len(const dsd_id_pattern* p) {
    size_t plen = 0;
    while (plen < ID_PATTERN_CANON_MAX && p->text[plen] != '\0') {
        plen++;
    }
    return plen;
}

/* Whether non-star pattern character @p pc (none: NUL) matches digit @p d. */
static int
id_glob_char_matches(char pc, char d) {
    return pc != '\0' && (pc == 'x' ? id_is_digit(d) : pc == d);
}

int
dsd_id_pattern_match_decimal(const dsd_id_pattern* p, const char* digits, size_t len) {
    if (p == NULL || digits == NULL || len == 0U) {
        return 0;
    }
    const size_t plen = id_pattern_text_len(p);
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
        } else if (id_glob_char_matches(pc, digits[si])) {
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
