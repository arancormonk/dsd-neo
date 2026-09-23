// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The CTCSS table and its text (issue #522). The table is pinned value by value against the
 * published EIA/TIA 50-tone list, so a transposed digit cannot hide behind a count check, and
 * 150.0 Hz is pinned absent: a detector that snapped it would report 151.4 Hz.
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include <dsd-neo/runtime/analog_tones.h>

static const int k_expected_tenths[] = {
    670,  693,  719,  744,  770,  797,  825,  854,  885,  915,  948,  974,  1000, 1035, 1072, 1109, 1148,
    1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799,
    1835, 1862, 1899, 1928, 1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};

static void
test_table(void) {
    assert(dsd_ctcss_tone_count() == DSD_CTCSS_TONE_COUNT);
    assert((int)(sizeof(k_expected_tenths) / sizeof(k_expected_tenths[0])) == DSD_CTCSS_TONE_COUNT);
    for (int i = 0; i < DSD_CTCSS_TONE_COUNT; i++) {
        assert(dsd_ctcss_tone_tenths(i) == k_expected_tenths[i]);
        assert(dsd_ctcss_tone_index(k_expected_tenths[i]) == i);
        if (i > 0) {
            assert(dsd_ctcss_tone_tenths(i) > dsd_ctcss_tone_tenths(i - 1));
        }
    }
    assert(dsd_ctcss_tone_tenths(-1) == -1);
    assert(dsd_ctcss_tone_tenths(DSD_CTCSS_TONE_COUNT) == -1);
    assert(dsd_ctcss_tone_tenths(0) == 670);
    assert(dsd_ctcss_tone_tenths(DSD_CTCSS_TONE_COUNT - 1) == 2541);

    /* Not supported: 150.0 (the would-be 51st tone), 68.2 (between 67.0 and 69.3), and the
       values either side of the table. */
    assert(dsd_ctcss_tone_index(1500) == -1);
    assert(dsd_ctcss_tone_index(682) == -1);
    assert(dsd_ctcss_tone_index(669) == -1);
    assert(dsd_ctcss_tone_index(2542) == -1);
    assert(dsd_ctcss_tone_index(0) == -1);
    assert(dsd_ctcss_tone_index(-670) == -1);
    assert(dsd_ctcss_tone_index(1514) == 24);
}

static void
test_format(void) {
    char buf[DSD_CTCSS_LABEL_SIZE];

    assert(dsd_ctcss_format(1000, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "100.0") == 0);
    assert(dsd_ctcss_format(670, buf, sizeof(buf)) == 4);
    assert(strcmp(buf, "67.0") == 0);
    assert(dsd_ctcss_format(2541, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "254.1") == 0);
    /* Any value can be named, supported or not, so a rejection can say what it saw. */
    assert(dsd_ctcss_format(1500, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "150.0") == 0);
    assert(dsd_ctcss_format(1, buf, sizeof(buf)) == 3);
    assert(strcmp(buf, "0.1") == 0);

    assert(dsd_ctcss_format_label(1000, buf, sizeof(buf)) == 14);
    assert(strcmp(buf, "CTCSS 100.0 Hz") == 0);
    assert(dsd_ctcss_format_label(670, buf, sizeof(buf)) == 13);
    assert(strcmp(buf, "CTCSS 67.0 Hz") == 0);
    assert(dsd_ctcss_format_label(9999, buf, sizeof(buf)) == 14);
    assert(strcmp(buf, "CTCSS 999.9 Hz") == 0);

    /* Out-of-range values and short buffers leave an empty string and report failure. */
    buf[0] = 'x';
    assert(dsd_ctcss_format(0, buf, sizeof(buf)) == -1);
    assert(buf[0] == '\0');
    buf[0] = 'x';
    assert(dsd_ctcss_format(10000, buf, sizeof(buf)) == -1);
    assert(buf[0] == '\0');
    assert(dsd_ctcss_format(-1000, buf, sizeof(buf)) == -1);
    char tiny[5];
    assert(dsd_ctcss_format(1000, tiny, sizeof(tiny)) == -1);
    assert(tiny[0] == '\0');
    char label_tiny[14];
    assert(dsd_ctcss_format_label(1000, label_tiny, sizeof(label_tiny)) == -1);
    assert(label_tiny[0] == '\0');
    assert(dsd_ctcss_format(1000, NULL, 8) == -1);
    assert(dsd_ctcss_format(1000, buf, 0) == -1);
}

int
main(void) {
    test_table();
    test_format();
    return 0;
}
