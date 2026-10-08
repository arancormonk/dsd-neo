// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The shared access-code text (issue #575): a call history row's colour code, NAC, RAN or CAN as every frontend
 * spells it, short for a list row and long for a detail sheet, and nothing at all for a value no protocol carries. */

#include <assert.h>
#include <dsd-neo/app_control/access_code_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void
expect_view(uint8_t kind, uint32_t value, int visible, const char* short_text, const char* long_text) {
    dsd_app_access_code view;
    /* Garbage first: the view must fill every field itself. */
    DSD_MEMSET(&view, 0x5A, sizeof(view));
    dsd_app_access_code_view(kind, value, &view);
    if (view.visible != visible || strcmp(view.short_text, short_text) != 0 || strcmp(view.long_text, long_text) != 0) {
        DSD_FPRINTF(stderr, "kind %u value %u: got visible %d \"%.*s\" \"%.*s\", want visible %d \"%s\" \"%s\"\n",
                    (unsigned)kind, (unsigned)value, view.visible, (int)sizeof(view.short_text), view.short_text,
                    (int)sizeof(view.long_text), view.long_text, visible, short_text, long_text);
    }
    assert(view.visible == visible);
    assert(strcmp(view.short_text, short_text) == 0);
    assert(strcmp(view.long_text, long_text) == 0);

    /* One text at a time is the same text. */
    char text[sizeof(view.long_text)];
    DSD_MEMSET(text, 0x5A, sizeof(text));
    assert(dsd_app_access_code_format(kind, value, DSD_APP_ACCESS_CODE_SHORT, text, sizeof(text)) == visible);
    assert(strcmp(text, short_text) == 0);
    DSD_MEMSET(text, 0x5A, sizeof(text));
    assert(dsd_app_access_code_format(kind, value, DSD_APP_ACCESS_CODE_LONG, text, sizeof(text)) == visible);
    assert(strcmp(text, long_text) == 0);
}

static void
expect_hidden(uint8_t kind, uint32_t value) {
    expect_view(kind, value, 0, "", "");
}

static void
test_every_kind(void) {
    expect_view(DSD_ACCESS_CODE_COLOR_CODE, 1U, 1, "CC 1", "Color code 1");
    expect_view(DSD_ACCESS_CODE_NAC, 0x293U, 1, "NAC 293", "Network access code 293");
    expect_view(DSD_ACCESS_CODE_RAN, 5U, 1, "RAN 5", "Radio access number 5");
    expect_view(DSD_ACCESS_CODE_CAN, 0U, 1, "CAN 0", "Channel access number 0");
}

static void
test_nac_is_three_hex_digits(void) {
    expect_view(DSD_ACCESS_CODE_NAC, 0x05U, 1, "NAC 005", "Network access code 005");
    expect_view(DSD_ACCESS_CODE_NAC, 0xABCU, 1, "NAC ABC", "Network access code ABC");
    expect_view(DSD_ACCESS_CODE_NAC, 0x001U, 1, "NAC 001", "Network access code 001");
    expect_view(DSD_ACCESS_CODE_NAC, 0xFFEU, 1, "NAC FFE", "Network access code FFE");
}

static void
test_range_edges(void) {
    /* dPMR's colour code runs to 63; DMR's 0..15 is a subset. */
    expect_view(DSD_ACCESS_CODE_COLOR_CODE, 0U, 1, "CC 0", "Color code 0");
    expect_view(DSD_ACCESS_CODE_COLOR_CODE, 63U, 1, "CC 63", "Color code 63");
    expect_view(DSD_ACCESS_CODE_RAN, 0U, 1, "RAN 0", "Radio access number 0");
    expect_view(DSD_ACCESS_CODE_RAN, 63U, 1, "RAN 63", "Radio access number 63");
    expect_view(DSD_ACCESS_CODE_CAN, 15U, 1, "CAN 15", "Channel access number 15");
}

static void
test_out_of_range_is_hidden(void) {
    expect_hidden(DSD_ACCESS_CODE_COLOR_CODE, 64U);
    /* The P25 NID's reserved values: no carrier is ever heard with them. */
    expect_hidden(DSD_ACCESS_CODE_NAC, 0x000U);
    expect_hidden(DSD_ACCESS_CODE_NAC, 0xFFFU);
    expect_hidden(DSD_ACCESS_CODE_NAC, 0x1000U);
    expect_hidden(DSD_ACCESS_CODE_RAN, 64U);
    expect_hidden(DSD_ACCESS_CODE_CAN, 16U);
    /* Wider than any code: a uint16_t field never holds it, but a persisted store might. */
    expect_hidden(DSD_ACCESS_CODE_COLOR_CODE, 0x10001U);
    expect_hidden(DSD_ACCESS_CODE_NAC, UINT32_MAX);
}

static void
test_unknown_kind_is_hidden(void) {
    expect_hidden(DSD_ACCESS_CODE_NONE, 0U);
    expect_hidden(DSD_ACCESS_CODE_NONE, 5U);
    /* A kind a newer build may pin: shown by nobody until this one learns it. */
    expect_hidden(5U, 5U);
    expect_hidden(UINT8_MAX, 1U);
}

static void
test_null_is_safe(void) {
    dsd_app_access_code_view(DSD_ACCESS_CODE_NAC, 0x293U, NULL);
    assert(dsd_app_access_code_format(DSD_ACCESS_CODE_NAC, 0x293U, DSD_APP_ACCESS_CODE_SHORT, NULL, 16U) == 0);
    char text[4] = {'x', 'x', 'x', 'x'};
    assert(dsd_app_access_code_format(DSD_ACCESS_CODE_NAC, 0x293U, DSD_APP_ACCESS_CODE_SHORT, text, 0U) == 0);
    assert(text[0] == 'x');
}

static void
test_format_edges(void) {
    char text[40];
    /* A form this build does not know writes nothing visible. */
    DSD_MEMSET(text, 0x5A, sizeof(text));
    assert(dsd_app_access_code_format(DSD_ACCESS_CODE_NAC, 0x293U, 2, text, sizeof(text)) == 0);
    assert(text[0] == '\0');
    /* A short buffer is cut and terminated. */
    char small[5];
    assert(dsd_app_access_code_format(DSD_ACCESS_CODE_NAC, 0x293U, DSD_APP_ACCESS_CODE_LONG, small, sizeof(small))
           == 1);
    assert(strcmp(small, "Netw") == 0);
}

int
main(void) {
    test_every_kind();
    test_nac_is_three_hex_digits();
    test_range_edges();
    test_out_of_range_is_hidden();
    test_unknown_kind_is_hidden();
    test_null_is_safe();
    test_format_edges();
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
