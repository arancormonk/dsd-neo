// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Digit-pattern IDs (issue #773): parsing and canonical text, the "some ID in 0..4294967295 matches" rule, matching
 * against an ID written in decimal without leading zeros, and the specificity order rows are ranked by.
 */

#include <dsd-neo/core/id_pattern.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

_Static_assert(DSD_ID_PATTERN_TEXT_MAX == 16, "canonical pattern text is stored in char[16]");
_Static_assert(DSD_ID_DECIMAL_MAX == 11, "an ID in decimal is at most 10 digits plus the terminator");

static int
expect_parse(const char* token, const char* want_text, unsigned want_literals, unsigned want_star) {
    dsd_id_pattern p;
    DSD_MEMSET(&p, 0xA5, sizeof(p));
    const int rc = dsd_id_pattern_parse(token, &p);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "parse(\"%s\"): rc=%d, want 0\n", token, rc);
        return 1;
    }
    if (strcmp(p.text, want_text) != 0 || p.literals != want_literals || p.has_star != want_star) {
        DSD_FPRINTF(stderr, "parse(\"%s\"): got text=\"%s\" literals=%u star=%u, want \"%s\" %u %u\n", token, p.text,
                    (unsigned)p.literals, (unsigned)p.has_star, want_text, want_literals, want_star);
        return 1;
    }
    if (dsd_id_pattern_is_candidate(token) != 1) {
        DSD_FPRINTF(stderr, "is_candidate(\"%s\"): want 1\n", token);
        return 1;
    }
    return 0;
}

static int
expect_reject(const char* label, const char* token, int want_candidate) {
    dsd_id_pattern p;
    DSD_MEMSET(&p, 0xA5, sizeof(p));
    const int rc = dsd_id_pattern_parse(token, &p);
    if (rc != 1) {
        DSD_FPRINTF(stderr, "%s: parse rc=%d, want 1 (text \"%.15s\")\n", label, rc, p.text);
        return 1;
    }
    /* A rejected token leaves a cleared pattern, which matches nothing. */
    if (p.text[0] != '\0' || p.literals != 0U || p.has_star != 0U || dsd_id_pattern_match(&p, 0U) != 0) {
        DSD_FPRINTF(stderr, "%s: rejected parse left a non-empty pattern\n", label);
        return 1;
    }
    if (dsd_id_pattern_is_candidate(token) != want_candidate) {
        DSD_FPRINTF(stderr, "%s: is_candidate=%d, want %d\n", label, dsd_id_pattern_is_candidate(token),
                    want_candidate);
        return 1;
    }
    return 0;
}

static int
test_parse_examples(void) {
    int rc = 0;
    rc |= expect_parse("1xxx", "1xxx", 1U, 0U);
    rc |= expect_parse("12*", "12*", 2U, 1U);
    rc |= expect_parse("*05", "*05", 2U, 1U);
    rc |= expect_parse("*12*", "*12*", 2U, 1U);
    rc |= expect_parse("2xx*", "2xx*", 1U, 1U);
    rc |= expect_parse("*", "*", 0U, 1U);
    rc |= expect_parse("x", "x", 0U, 0U);
    rc |= expect_parse("*000000000", "*000000000", 9U, 1U); /* 1000000000 .. 4000000000 */
    rc |= expect_parse("1X*", "1x*", 1U, 1U);
    rc |= expect_parse("1**2", "1*2", 2U, 1U);
    rc |= expect_parse("429496729x", "429496729x", 9U, 0U);
    /* A literal 0 after a star is no leading zero: the pattern starts with the star, and "*0" is every ID ending 0. */
    rc |= expect_parse("*0", "*0", 1U, 1U);
    /* Fifteen canonical characters is the limit, and the limit applies after collapsing stars. */
    rc |= expect_parse("*1*2*3*4*5*6*7*", "*1*2*3*4*5*6*7*", 7U, 1U);
    rc |= expect_parse("1****************2", "1*2", 2U, 1U);
    return rc;
}

static int
test_parse_rejects(void) {
    int rc = 0;
    rc |= expect_reject("leading zero then x", "05x", 1);
    rc |= expect_reject("leading zero then star (only 0 itself, written 0)", "0*", 1);
    rc |= expect_reject("eleven digits after star", "*0000000000", 1);
    rc |= expect_reject("ten digits with leading zero after star", "*0xxxxxxxxx", 1);
    rc |= expect_reject("above 4294967295", "5xxxxxxxxx", 1);
    rc |= expect_reject("prefix above 4294967295", "4294967296*", 1);
    rc |= expect_reject("eleven digits", "xxxxxxxxxxx", 1);
    rc |= expect_reject("letter", "12a*", 1);
    rc |= expect_reject("empty", "", 0);
    rc |= expect_reject("blank", "   ", 0);
    /* Sixteen canonical characters: some ID would match (12345678), but the text does not fit. */
    rc |= expect_reject("sixteen characters", "*1*2*3*4*5*6*7*8", 1);
    rc |= expect_reject("long", "1xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 1);
    rc |= expect_reject("minus sign", "1*-2*", 1);
    rc |= expect_reject("no wildcard is an exact ID, not a pattern", "123", 0);
    rc |= expect_reject("internal space", "1 xxx", 1);
    rc |= expect_reject("question mark", "1?", 0);

    dsd_id_pattern p;
    if (dsd_id_pattern_parse(NULL, &p) != 1 || dsd_id_pattern_parse("1x", NULL) != 1) {
        DSD_FPRINTF(stderr, "parse with NULL arguments: want 1\n");
        rc = 1;
    }
    if (dsd_id_pattern_is_candidate(NULL) != 0) {
        DSD_FPRINTF(stderr, "is_candidate(NULL): want 0\n");
        rc = 1;
    }
    return rc;
}

static int
test_pattern_whitespace_and_case(void) {
    int rc = 0;
    dsd_id_pattern a;
    dsd_id_pattern b;
    if (dsd_id_pattern_parse(" 1XxX ", &a) != 0 || dsd_id_pattern_parse("1xxx", &b) != 0) {
        DSD_FPRINTF(stderr, "whitespace/case: parse failed\n");
        return 1;
    }
    if (strcmp(a.text, b.text) != 0 || a.literals != b.literals || a.has_star != b.has_star) {
        DSD_FPRINTF(stderr, "whitespace/case: \" 1XxX \" gave \"%s\", want \"%s\"\n", a.text, b.text);
        rc = 1;
    }
    rc |= expect_parse("\t*12*\r\n", "*12*", 2U, 1U);
    return rc;
}

static int
expect_match(const char* token, uint32_t id, int want) {
    dsd_id_pattern p;
    if (dsd_id_pattern_parse(token, &p) != 0) {
        DSD_FPRINTF(stderr, "match: parse(\"%s\") failed\n", token);
        return 1;
    }
    const int got = dsd_id_pattern_match(&p, id);
    char digits[DSD_ID_DECIMAL_MAX];
    const size_t len = dsd_id_decimal(id, digits, sizeof(digits));
    const int got_decimal = dsd_id_pattern_match_decimal(&p, digits, len);
    if (got != want || got_decimal != want) {
        DSD_FPRINTF(stderr, "match(\"%s\", %u): got %d (decimal %d), want %d\n", token, (unsigned)id, got, got_decimal,
                    want);
        return 1;
    }
    return 0;
}

static int
test_match_table(void) {
    int rc = 0;
    rc |= expect_match("1xxx", 999U, 0);
    rc |= expect_match("1xxx", 1000U, 1);
    rc |= expect_match("1xxx", 1999U, 1);
    rc |= expect_match("1xxx", 2000U, 0);
    rc |= expect_match("1xxx", 10000U, 0);

    rc |= expect_match("12*", 12U, 1);
    rc |= expect_match("12*", 120U, 1);
    rc |= expect_match("12*", 1299999U, 1);
    rc |= expect_match("12*", 112U, 0);

    rc |= expect_match("*05", 5U, 0);
    rc |= expect_match("*05", 105U, 1);
    rc |= expect_match("*05", 4294967205U, 1);

    rc |= expect_match("*12*", 412U, 1);
    rc |= expect_match("*12*", 12U, 1);
    rc |= expect_match("*12*", 3120U, 1);
    rc |= expect_match("*12*", 1U, 0);

    rc |= expect_match("x", 0U, 1);
    rc |= expect_match("x", 9U, 1);
    rc |= expect_match("x", 10U, 0);

    rc |= expect_match("*0", 0U, 1); /* the star matches no digits */
    rc |= expect_match("*0", 10U, 1);
    rc |= expect_match("*0", 4294967290U, 1);
    rc |= expect_match("*0", 5U, 0);
    rc |= expect_match("*0", 101U, 0);

    rc |= expect_match("*", 0U, 1);
    rc |= expect_match("*", 4294967295U, 1);

    rc |= expect_match("429496729x", 4294967295U, 1);
    rc |= expect_match("429496729x", 4294967289U, 0);

    /* Star backtracking: the first "1" is not the one that ends the match. */
    rc |= expect_match("*1x2", 13112U, 1);
    rc |= expect_match("*1x2", 1312U, 0);
    rc |= expect_match("1*2", 12U, 1);
    rc |= expect_match("1*2", 1232U, 1);
    rc |= expect_match("1*2", 1223U, 0);
    rc |= expect_match("2xx*", 20U, 0);
    rc |= expect_match("2xx*", 200U, 1);
    rc |= expect_match("2xx*", 2999999U, 1);

    /* A cleared pattern matches nothing, and NULL arguments match nothing. */
    dsd_id_pattern empty;
    DSD_MEMSET(&empty, 0, sizeof(empty));
    if (dsd_id_pattern_match(&empty, 0U) != 0 || dsd_id_pattern_match(NULL, 0U) != 0) {
        DSD_FPRINTF(stderr, "empty/NULL pattern matched\n");
        rc = 1;
    }
    dsd_id_pattern star;
    if (dsd_id_pattern_parse("*", &star) != 0 || dsd_id_pattern_match_decimal(&star, NULL, 1U) != 0
        || dsd_id_pattern_match_decimal(&star, "", 0U) != 0) {
        DSD_FPRINTF(stderr, "match_decimal with no digits matched\n");
        rc = 1;
    }
    return rc;
}

static int
expect_beats(const char* a_token, const char* b_token) {
    dsd_id_pattern a;
    dsd_id_pattern b;
    if (dsd_id_pattern_parse(a_token, &a) != 0 || dsd_id_pattern_parse(b_token, &b) != 0) {
        DSD_FPRINTF(stderr, "specificity: parse failed for \"%s\"/\"%s\"\n", a_token, b_token);
        return 1;
    }
    if (dsd_id_pattern_more_specific(&a, &b) != 1 || dsd_id_pattern_more_specific(&b, &a) != 0) {
        DSD_FPRINTF(stderr, "specificity: want \"%s\" to beat \"%s\" (got %d, reverse %d)\n", a_token, b_token,
                    dsd_id_pattern_more_specific(&a, &b), dsd_id_pattern_more_specific(&b, &a));
        return 1;
    }
    return 0;
}

static int
expect_tie(const char* a_token, const char* b_token) {
    dsd_id_pattern a;
    dsd_id_pattern b;
    if (dsd_id_pattern_parse(a_token, &a) != 0 || dsd_id_pattern_parse(b_token, &b) != 0) {
        DSD_FPRINTF(stderr, "specificity: parse failed for \"%s\"/\"%s\"\n", a_token, b_token);
        return 1;
    }
    if (dsd_id_pattern_more_specific(&a, &b) != 0 || dsd_id_pattern_more_specific(&b, &a) != 0) {
        DSD_FPRINTF(stderr, "specificity: want \"%s\" and \"%s\" equal\n", a_token, b_token);
        return 1;
    }
    return 0;
}

static int
test_specificity(void) {
    int rc = 0;
    rc |= expect_beats("12*", "1*");
    rc |= expect_beats("1xxx", "1*");
    rc |= expect_beats("*05", "1*");
    /* Equal literals: the pattern without a star wins. */
    rc |= expect_beats("1xx", "1x*");
    rc |= expect_beats("x", "*");
    rc |= expect_tie("12*", "*12");
    rc |= expect_tie("1xxx", "2xxx");
    rc |= expect_tie("12*", "12*");
    dsd_id_pattern p;
    if (dsd_id_pattern_parse("1*", &p) != 0 || dsd_id_pattern_more_specific(NULL, &p) != 0
        || dsd_id_pattern_more_specific(&p, NULL) != 0) {
        DSD_FPRINTF(stderr, "specificity with NULL: want 0\n");
        rc = 1;
    }
    return rc;
}

/* A stored canonical text ranks as the pattern it was parsed from, without parsing it again. */
static int
test_from_canonical(void) {
    static const char* const tokens[] = {"1XX**", "*05", "x", "*", "2xx*", "*000000000"};
    int rc = 0;
    for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++) {
        dsd_id_pattern parsed;
        dsd_id_pattern ranked;
        DSD_MEMSET(&ranked, 0xA5, sizeof(ranked));
        if (dsd_id_pattern_parse(tokens[i], &parsed) != 0) {
            DSD_FPRINTF(stderr, "from_canonical: \"%s\" did not parse\n", tokens[i]);
            return 1;
        }
        dsd_id_pattern_from_canonical(parsed.text, &ranked);
        if (strcmp(ranked.text, parsed.text) != 0 || ranked.literals != parsed.literals
            || ranked.has_star != parsed.has_star) {
            DSD_FPRINTF(stderr, "from_canonical(\"%s\"): got \"%s\" %u %u, want %u %u\n", parsed.text, ranked.text,
                        (unsigned)ranked.literals, (unsigned)ranked.has_star, (unsigned)parsed.literals,
                        (unsigned)parsed.has_star);
            rc = 1;
        }
    }
    dsd_id_pattern empty;
    DSD_MEMSET(&empty, 0xA5, sizeof(empty));
    dsd_id_pattern_from_canonical(NULL, &empty);
    if (empty.text[0] != '\0' || empty.literals != 0U || empty.has_star != 0U || dsd_id_pattern_match(&empty, 1U)) {
        DSD_FPRINTF(stderr, "from_canonical(NULL): want the empty pattern\n");
        rc = 1;
    }
    dsd_id_pattern_from_canonical("1x", NULL);
    return rc;
}

static int
expect_decimal(uint32_t id, const char* want) {
    char out[DSD_ID_DECIMAL_MAX];
    DSD_MEMSET(out, 'Z', sizeof(out));
    const size_t len = dsd_id_decimal(id, out, sizeof(out));
    if (len != strlen(want) || strcmp(out, want) != 0) {
        DSD_FPRINTF(stderr, "decimal(%u): got \"%.11s\" len %u, want \"%s\"\n", (unsigned)id, out, (unsigned)len, want);
        return 1;
    }
    return 0;
}

static int
test_decimal(void) {
    int rc = 0;
    rc |= expect_decimal(0U, "0");
    rc |= expect_decimal(7U, "7");
    rc |= expect_decimal(1000U, "1000");
    rc |= expect_decimal(4294967295U, "4294967295");

    /* Too small a buffer writes an empty string and returns 0. */
    char small[4];
    DSD_MEMSET(small, 'Z', sizeof(small));
    if (dsd_id_decimal(1000U, small, sizeof(small)) != 0U || small[0] != '\0') {
        DSD_FPRINTF(stderr, "decimal into a short buffer: want 0 and an empty string\n");
        rc = 1;
    }
    if (dsd_id_decimal(999U, small, sizeof(small)) != 3U || strcmp(small, "999") != 0) {
        DSD_FPRINTF(stderr, "decimal into an exact buffer: want \"999\"\n");
        rc = 1;
    }
    if (dsd_id_decimal(1U, NULL, 4U) != 0U) {
        DSD_FPRINTF(stderr, "decimal into NULL: want 0\n");
        rc = 1;
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_parse_examples();
    rc |= test_parse_rejects();
    rc |= test_pattern_whitespace_and_case();
    rc |= test_match_table();
    rc |= test_specificity();
    rc |= test_decimal();
    rc |= test_from_canonical();
    if (rc == 0) {
        printf("CORE_ID_PATTERN: OK\n");
    }
    return rc;
}
