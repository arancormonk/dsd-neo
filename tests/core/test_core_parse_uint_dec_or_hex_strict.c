// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_parse_uint_dec_or_hex_strict() reads a whole decimal or 0x-hex number. A leading zero stays decimal, and signs,
 * 0b, spaces, trailing text and values above the maximum are refused without touching the output.
 */

#include <assert.h>
#include <dsd-neo/core/parse.h>
#include <stddef.h>
#include <stdint.h>

static int
parses_as(const char* text, uint32_t max_value, uint32_t expected) {
    uint32_t v = 77;
    return dsd_parse_uint_dec_or_hex_strict(text, max_value, &v) == 0 && v == expected;
}

static int
refused(const char* text, uint32_t max_value) {
    uint32_t v = 77;
    return dsd_parse_uint_dec_or_hex_strict(text, max_value, &v) == -1 && v == 77;
}

int
main(void) {
    assert(parses_as("0", 15, 0));
    assert(parses_as("9", 15, 9));
    assert(parses_as("15", 15, 15));
    assert(parses_as("0x9", 15, 9));
    assert(parses_as("0X9", 15, 9));
    assert(parses_as("0xf", 15, 15));
    assert(parses_as("0xF", 15, 15));
    assert(parses_as("0x0F", 15, 15));
    assert(parses_as("010", 15, 10)); /* decimal, not octal 8 */
    assert(parses_as("08", 15, 8));
    assert(parses_as("0xFFFFFFFF", 0xFFFFFFFFu, 0xFFFFFFFFu));

    static const char* const k_bad[] = {"",
                                        "16",
                                        "0x10",
                                        "-1",
                                        "+1",
                                        "0b1",
                                        "0x",
                                        " 1",
                                        "1 ",
                                        "1x",
                                        "0xG",
                                        "1.5",
                                        "a",
                                        "f",
                                        "0x-1",
                                        "0x 1",
                                        "99999999999999999999"};
    for (size_t i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); i++) {
        assert(refused(k_bad[i], 15));
    }
    /* A maximum that is not a whole hex digit's range bounds the value, not the digits: 0x9 and 9 pass under 9,
       0xA and 10 do not, and 0x9 under 8 does not either. */
    assert(parses_as("0x9", 9, 9));
    assert(parses_as("9", 9, 9));
    assert(refused("0xA", 9));
    assert(refused("0xa", 9));
    assert(refused("10", 9));
    assert(refused("0x9", 8));
    assert(refused("0x100000000", 0xFFFFFFFFu));
    assert(refused(NULL, 15));
    assert(dsd_parse_uint_dec_or_hex_strict("1", 15, NULL) == -1);
    return 0;
}
