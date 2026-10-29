// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <dsd-neo/runtime/freq_parse.h>

int
main(void) {
    assert(dsd_parse_freq_hz(NULL) == 0u);
    assert(dsd_parse_freq_hz("") == 0u);
    assert(dsd_parse_freq_hz("0") == 0u);
    assert(dsd_parse_freq_hz("-1") == 0u);
    assert(dsd_parse_freq_hz("abc") == 0u);

    assert(dsd_parse_freq_hz("1") == 1u);
    assert(dsd_parse_freq_hz("1.4") == 1u);
    assert(dsd_parse_freq_hz("1.6") == 2u);
    assert(dsd_parse_freq_hz("123abc") == 0u);
    assert(dsd_parse_freq_hz("1kjunk") == 0u);

    assert(dsd_parse_freq_hz("1k") == 1000u);
    assert(dsd_parse_freq_hz("1K") == 1000u);
    assert(dsd_parse_freq_hz("2m") == 2000000u);
    assert(dsd_parse_freq_hz("2M") == 2000000u);
    assert(dsd_parse_freq_hz("4g") == 4000000000u);
    assert(dsd_parse_freq_hz("4G") == 4000000000u);

    assert(dsd_parse_freq_hz("4294967295") == UINT32_MAX);
    assert(dsd_parse_freq_hz("4294967296") == 0u);
    assert(dsd_parse_freq_hz("5g") == 0u);
    assert(dsd_parse_freq_hz("4294967295.25") == UINT32_MAX);
    assert(dsd_parse_freq_hz("4294967295.5") == 0u);
    assert(dsd_parse_freq_hz("4294967295.75") == 0u);
    assert(dsd_parse_freq_hz("4294.9672956M") == 0u);

    assert(dsd_parse_freq_hz("nan") == 0u);
    assert(dsd_parse_freq_hz("-nan") == 0u);
    assert(dsd_parse_freq_hz("NAN") == 0u);
    assert(dsd_parse_freq_hz("nanM") == 0u);
    assert(dsd_parse_freq_hz("inf") == 0u);
    assert(dsd_parse_freq_hz("-inf") == 0u);
    assert(dsd_parse_freq_hz("infinity") == 0u);

    /* 64 bytes is refused, 63 still parses. */
    char longtxt[80];
    memset(longtxt, '0', sizeof longtxt);
    memcpy(longtxt, "146.52", 6);
    longtxt[63] = 'M';
    longtxt[64] = '\0';
    assert(strlen(longtxt) == 64u);
    assert(dsd_parse_freq_hz(longtxt) == 0u);
    longtxt[62] = 'M';
    longtxt[63] = '\0';
    assert(strlen(longtxt) == 63u);
    assert(dsd_parse_freq_hz(longtxt) == 146520000u);
    return 0;
}
