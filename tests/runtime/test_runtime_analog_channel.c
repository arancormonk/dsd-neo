// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The analog channel contract every entry point shares: per-kind ranges and
 * defaults, the strict width parser, the width/rate validator with its
 * actionable text, and the kHz formatter. The validator mirrors the DSP design
 * (cutoff W/2 + 600 Hz inside 0.45 x rate, Blackman taps within the 288-tap
 * analog capacity), so these rows are also the table of which widths each DSP
 * rate can realize.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

static void
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %d want %d\n", label, got, want);
        g_failures++;
    }
}

static void
expect_str(const char* label, const char* got, const char* want) {
    if (!got || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: got \"%s\" want \"%s\"\n", label, got ? got : "(null)", want);
        g_failures++;
    }
}

static void
expect_contains(const char* label, const char* text, const char* needle) {
    if (!text || !strstr(text, needle)) {
        DSD_FPRINTF(stderr, "FAIL: %s: \"%s\" does not contain \"%s\"\n", label, text ? text : "(null)", needle);
        g_failures++;
    }
}

static void
test_ranges_and_defaults(void) {
    expect_int("FM is enum zero", DSD_ANALOG_DEMOD_FM, 0);
    expect_int("AM is enum one", DSD_ANALOG_DEMOD_AM, 1);
    expect_int("digital family is zero", DSD_RX_FAMILY_DIGITAL, 0);
    expect_int("analog family is one", DSD_RX_FAMILY_ANALOG, 1);

    expect_int("NFM min", dsd_analog_width_min_hz(DSD_ANALOG_DEMOD_FM), 8000);
    expect_int("NFM max", dsd_analog_width_max_hz(DSD_ANALOG_DEMOD_FM), 25000);
    expect_int("NFM default", dsd_analog_width_default_hz(DSD_ANALOG_DEMOD_FM), 16000);
    expect_int("AM min", dsd_analog_width_min_hz(DSD_ANALOG_DEMOD_AM), 5000);
    expect_int("AM max", dsd_analog_width_max_hz(DSD_ANALOG_DEMOD_AM), 20000);
    expect_int("AM default", dsd_analog_width_default_hz(DSD_ANALOG_DEMOD_AM), 6000);
    expect_int("unknown kind has no default", dsd_analog_width_default_hz(7), 0);

    expect_int("FM is valid", dsd_analog_demod_is_valid(DSD_ANALOG_DEMOD_FM), 1);
    expect_int("AM is valid", dsd_analog_demod_is_valid(DSD_ANALOG_DEMOD_AM), 1);
    expect_int("negative kind invalid", dsd_analog_demod_is_valid(-1), 0);
    expect_int("kind 2 invalid", dsd_analog_demod_is_valid(2), 0);
    expect_str("FM label", dsd_analog_demod_label(DSD_ANALOG_DEMOD_FM), "NFM");
    expect_str("AM label", dsd_analog_demod_label(DSD_ANALOG_DEMOD_AM), "AM");

    /* 0 is "default, not requested": it resolves to the kind's default and never
     * counts as an explicit width. */
    expect_int("NFM 0 resolves to default", dsd_analog_width_effective_hz(DSD_ANALOG_DEMOD_FM, 0), 16000);
    expect_int("AM 0 resolves to default", dsd_analog_width_effective_hz(DSD_ANALOG_DEMOD_AM, 0), 6000);
    expect_int("explicit width kept", dsd_analog_width_effective_hz(DSD_ANALOG_DEMOD_FM, 12500), 12500);

    expect_int("NFM lower edge in range", dsd_analog_width_in_range(DSD_ANALOG_DEMOD_FM, 8000), 1);
    expect_int("NFM upper edge in range", dsd_analog_width_in_range(DSD_ANALOG_DEMOD_FM, 25000), 1);
    expect_int("NFM below range", dsd_analog_width_in_range(DSD_ANALOG_DEMOD_FM, 7999), 0);
    expect_int("NFM above range", dsd_analog_width_in_range(DSD_ANALOG_DEMOD_FM, 25001), 0);
    expect_int("AM lower edge in range", dsd_analog_width_in_range(DSD_ANALOG_DEMOD_AM, 5000), 1);
    expect_int("AM above range", dsd_analog_width_in_range(DSD_ANALOG_DEMOD_AM, 20001), 0);
    expect_int("0 is never an in-range explicit width", dsd_analog_width_in_range(DSD_ANALOG_DEMOD_FM, 0), 0);

    /* The runtime copies of the DSP design constants. demod_pipeline.cpp
     * static-asserts its own against these, so this pins the values. */
    expect_int("transition", DSD_ANALOG_CHANNEL_TRANSITION_HZ, 1200);
    expect_int("guard", DSD_ANALOG_CHANNEL_GUARD_HZ, 600);
    expect_int("analog tap capacity", DSD_ANALOG_CHANNEL_MAX_TAPS, 288);
}

static void
test_parse(void) {
    char err[DSD_ANALOG_ERROR_TEXT_MAX];
    int width = -1;

    expect_int("parse 12500", dsd_analog_width_parse(DSD_ANALOG_DEMOD_FM, "12500", &width, err, sizeof err), 0);
    expect_int("parsed 12500", width, 12500);
    expect_int("parse NFM min", dsd_analog_width_parse(DSD_ANALOG_DEMOD_FM, "8000", &width, err, sizeof err), 0);
    expect_int("parse AM 5000", dsd_analog_width_parse(DSD_ANALOG_DEMOD_AM, "5000", &width, err, sizeof err), 0);
    expect_int("parsed AM 5000", width, 5000);

    static const char* const malformed[] = {"", "12.5", "12500Hz", "12k", " 12500", "+12500", "-8000", "0x3000", "1e4"};
    for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        width = 4242;
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "reject \"%s\"", malformed[i]);
        expect_int(label, dsd_analog_width_parse(DSD_ANALOG_DEMOD_FM, malformed[i], &width, err, sizeof err), -1);
        expect_int("rejected parse leaves the output alone", width, 4242);
        expect_contains("malformed text names the range", err, "8000 to 25000");
    }

    width = 4242;
    expect_int("reject out-of-range NFM", dsd_analog_width_parse(DSD_ANALOG_DEMOD_FM, "30000", &width, err, sizeof err),
               -1);
    expect_int("out-of-range leaves output", width, 4242);
    expect_contains("range text names the kind", err, "NFM");
    expect_contains("range text names the value", err, "30000");
    expect_contains("range text names the range", err, "8000 to 25000");

    expect_int("AM rejects an NFM-only width",
               dsd_analog_width_parse(DSD_ANALOG_DEMOD_AM, "25000", &width, err, sizeof err), -1);
    expect_contains("AM range text", err, "5000 to 20000");
    expect_int("AM rejects 0", dsd_analog_width_parse(DSD_ANALOG_DEMOD_AM, "0", &width, err, sizeof err), -1);
    expect_int("null text", dsd_analog_width_parse(DSD_ANALOG_DEMOD_FM, NULL, &width, err, sizeof err), -1);
    expect_int("null out", dsd_analog_width_parse(DSD_ANALOG_DEMOD_FM, "12500", NULL, err, sizeof err), -1);
    expect_int("bad kind", dsd_analog_width_parse(5, "12500", &width, err, sizeof err), -1);
    /* A missing error buffer must not stop validation. */
    expect_int("no error buffer", dsd_analog_width_parse(DSD_ANALOG_DEMOD_FM, "99", &width, NULL, 0), -1);

    /* Arbitrarily long input (a CLI argument or INI value) is echoed as a bounded prefix, so the message still fits
     * DSD_ANALOG_ERROR_TEXT_MAX whole: it ends with the closing quote and parenthesis, not mid-input. */
    static char long_text[600];
    DSD_MEMSET(long_text, 'x', sizeof long_text - 1U);
    long_text[sizeof long_text - 1U] = '\0';
    expect_int("reject long input", dsd_analog_width_parse(DSD_ANALOG_DEMOD_FM, long_text, &width, err, sizeof err),
               -1);
    const size_t err_len = strlen(err);
    expect_int("long input message fits", err_len < sizeof err - 1U, 1);
    expect_int("long input message complete", err_len >= 2U && strcmp(err + err_len - 2U, "\")") == 0, 1);
    expect_contains("long input marked as cut", err, "xxx...\")");
    expect_contains("long input message still names the range", err, "8000 to 25000");
}

static void
test_format(void) {
    char text[DSD_ANALOG_WIDTH_TEXT_MAX];

    struct {
        int hz;
        const char* want;
    } rows[] = {
        {16000, "16 kHz"},     {12500, "12.5 kHz"}, {11250, "11.25 kHz"}, {20400, "20.4 kHz"},
        {46875, "46.875 kHz"}, {6000, "6 kHz"},     {25000, "25 kHz"},    {8001, "8.001 kHz"},
    };

    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        expect_int("format rc", dsd_analog_width_format(rows[i].hz, text, sizeof text), 0);
        expect_str("format text", text, rows[i].want);
    }
    expect_int("format rejects a short buffer", dsd_analog_width_format(12500, text, 4), -1);
    expect_int("format rejects null", dsd_analog_width_format(12500, NULL, 16), -1);
    expect_int("format rejects negative", dsd_analog_width_format(-1, text, sizeof text), -1);
}

/* dsd_firdes_compute_ntaps() for a Blackman window with a 1200 Hz transition. */
static void
test_tap_arithmetic(void) {
    expect_int("taps @48k", dsd_analog_channel_taps_for_rate(48000), 135);
    expect_int("taps @24k", dsd_analog_channel_taps_for_rate(24000), 67);
    expect_int("taps @46875", dsd_analog_channel_taps_for_rate(46875), 131);
    expect_int("taps @78125", dsd_analog_channel_taps_for_rate(78125), 219);
    expect_int("taps @96k", dsd_analog_channel_taps_for_rate(96000), 269);
    /* 13200 | rate makes the quotient exact: 37 * 7 = 259, already odd. */
    expect_int("taps @92400", dsd_analog_channel_taps_for_rate(92400), 259);
    expect_int("taps @102700 still fits", dsd_analog_channel_taps_for_rate(102700), 287);
    expect_int("taps @102800 exceed capacity", dsd_analog_channel_taps_for_rate(102800) > 288, 1);
}

static void
test_max_width_by_rate(void) {
    /* The decision record's table: 48k fits everything; 24k -> 20.4 kHz;
     * 16k -> 13.2 kHz; 12k -> 9.6 kHz; 8k -> 6 kHz; 6k -> 4.2 kHz. */
    expect_int("max @48k", dsd_analog_width_max_for_rate(48000), 42000);
    expect_int("max @24k", dsd_analog_width_max_for_rate(24000), 20400);
    expect_int("max @16k", dsd_analog_width_max_for_rate(16000), 13200);
    expect_int("max @12k", dsd_analog_width_max_for_rate(12000), 9600);
    expect_int("max @8k", dsd_analog_width_max_for_rate(8000), 6000);
    expect_int("max @6k", dsd_analog_width_max_for_rate(6000), 4200);
    expect_int("max @4k", dsd_analog_width_max_for_rate(4000), 2400);
    expect_int("max @96k", dsd_analog_width_max_for_rate(96000), 85200);
    expect_int("no width above the tap ceiling", dsd_analog_width_max_for_rate(128000), 0);
    expect_int("no width at rate 0", dsd_analog_width_max_for_rate(0), 0);
    expect_int("no width at a tiny rate", dsd_analog_width_max_for_rate(1000), 0);
}

typedef struct {
    int kind;
    int width_hz;
    int rate_hz;
    int want_ok;
} check_row;

static void
test_check_table(void) {
    static const check_row rows[] = {
        /* NFM across the RTL DSP bandwidths 4..48 kHz and forced rates up to 96 kHz. */
        {DSD_ANALOG_DEMOD_FM, 8000, 4000, 0},
        {DSD_ANALOG_DEMOD_FM, 8000, 6000, 0},
        {DSD_ANALOG_DEMOD_FM, 8000, 8000, 0},
        {DSD_ANALOG_DEMOD_FM, 8000, 12000, 1},
        {DSD_ANALOG_DEMOD_FM, 9600, 12000, 1},
        {DSD_ANALOG_DEMOD_FM, 9601, 12000, 0},
        {DSD_ANALOG_DEMOD_FM, 12500, 12000, 0},
        {DSD_ANALOG_DEMOD_FM, 12500, 16000, 1},
        {DSD_ANALOG_DEMOD_FM, 13200, 16000, 1},
        {DSD_ANALOG_DEMOD_FM, 16000, 16000, 0},
        {DSD_ANALOG_DEMOD_FM, 16000, 24000, 1},
        {DSD_ANALOG_DEMOD_FM, 20400, 24000, 1},
        {DSD_ANALOG_DEMOD_FM, 20401, 24000, 0},
        {DSD_ANALOG_DEMOD_FM, 25000, 24000, 0},
        {DSD_ANALOG_DEMOD_FM, 25000, 48000, 1},
        {DSD_ANALOG_DEMOD_FM, 16000, 46875, 1},
        {DSD_ANALOG_DEMOD_FM, 25000, 78125, 1},
        {DSD_ANALOG_DEMOD_FM, 25000, 96000, 1},
        {DSD_ANALOG_DEMOD_FM, 16000, 128000, 0},
        /* AM: all rejected at 6 kHz; 5..6 kHz fit at 8 kHz. */
        {DSD_ANALOG_DEMOD_AM, 5000, 6000, 0},
        {DSD_ANALOG_DEMOD_AM, 5000, 8000, 1},
        {DSD_ANALOG_DEMOD_AM, 6000, 8000, 1},
        {DSD_ANALOG_DEMOD_AM, 6001, 8000, 0},
        {DSD_ANALOG_DEMOD_AM, 20000, 24000, 1},
        {DSD_ANALOG_DEMOD_AM, 20000, 48000, 1},
        /* Out-of-range widths fail before the rate is considered. */
        {DSD_ANALOG_DEMOD_FM, 30000, 48000, 0},
        {DSD_ANALOG_DEMOD_AM, 25000, 48000, 0},
        {DSD_ANALOG_DEMOD_FM, 0, 48000, 0},
        {9, 16000, 48000, 0},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        char err[DSD_ANALOG_ERROR_TEXT_MAX];
        err[0] = 'x';
        err[1] = '\0';
        const int rc = dsd_analog_width_check(rows[i].kind, rows[i].width_hz, rows[i].rate_hz, err, sizeof err);
        char label[96];
        DSD_SNPRINTF(label, sizeof label, "check kind=%d width=%d rate=%d", rows[i].kind, rows[i].width_hz,
                     rows[i].rate_hz);
        expect_int(label, rc == 0, rows[i].want_ok);
        expect_int("realizable agrees with check for in-range rows",
                   rows[i].want_ok ? dsd_analog_width_realizable(rows[i].width_hz, rows[i].rate_hz) : 0,
                   rows[i].want_ok);
        if (rc == 0) {
            expect_str("accepted width clears the error text", err, "");
        } else {
            expect_int("rejected width explains itself", err[0] != '\0', 1);
        }
    }
}

static void
test_check_messages(void) {
    char err[DSD_ANALOG_ERROR_TEXT_MAX];

    /* Requested width, DSP rate, the largest width that rate fits, and the fix. */
    expect_int("25k at 24k rejected", dsd_analog_width_check(DSD_ANALOG_DEMOD_FM, 25000, 24000, err, sizeof err), -1);
    expect_contains("names the width", err, "NFM bandwidth 25 kHz");
    expect_contains("names the rate", err, "24 kHz DSP rate");
    expect_contains("names the largest width", err, "20.4 kHz");
    expect_contains("names the fix", err, "set the RTL DSP bandwidth to 48 kHz");

    expect_int("16k at 16k rejected", dsd_analog_width_check(DSD_ANALOG_DEMOD_FM, 16000, 16000, err, sizeof err), -1);
    expect_contains("16k names largest", err, "13.2 kHz");
    expect_contains("16k fix lists both rates", err, "set the RTL DSP bandwidth to 24 or 48 kHz");

    expect_int("12.5k at 12k rejected", dsd_analog_width_check(DSD_ANALOG_DEMOD_FM, 12500, 12000, err, sizeof err), -1);
    expect_contains("12.5k width text", err, "NFM bandwidth 12.5 kHz");
    expect_contains("12.5k fix lists every rate that fits", err, "set the RTL DSP bandwidth to 16, 24 or 48 kHz");

    /* No NFM width fits 8 kHz at all; the largest the rate fits is still named. */
    expect_int("8k at 8k rejected", dsd_analog_width_check(DSD_ANALOG_DEMOD_FM, 8000, 8000, err, sizeof err), -1);
    expect_contains("8k names largest", err, "6 kHz");
    expect_contains("8k fix", err, "set the RTL DSP bandwidth to 12, 16, 24 or 48 kHz");

    expect_int("AM at 6k rejected", dsd_analog_width_check(DSD_ANALOG_DEMOD_AM, 5000, 6000, err, sizeof err), -1);
    expect_contains("AM names the kind", err, "AM bandwidth 5 kHz");
    expect_contains("AM names largest", err, "4.2 kHz");
    expect_contains("AM fix", err, "set the RTL DSP bandwidth to 8, 12, 16, 24 or 48 kHz");

    /* A forced rate above the tap ceiling: nothing fits, and the text says why. */
    expect_int("128k rejected", dsd_analog_width_check(DSD_ANALOG_DEMOD_FM, 16000, 128000, err, sizeof err), -1);
    expect_contains("128k names rate", err, "128 kHz DSP rate");
    expect_contains("128k names the filter limit", err, "288");

    expect_int("range rejected", dsd_analog_width_check(DSD_ANALOG_DEMOD_FM, 30000, 48000, err, sizeof err), -1);
    expect_contains("range names value", err, "30 kHz");
    expect_contains("range names bounds", err, "8 to 25 kHz");

    expect_int("no rate", dsd_analog_width_check(DSD_ANALOG_DEMOD_FM, 16000, 0, err, sizeof err), -1);
    expect_contains("no rate text", err, "DSP rate");

    /* A truncating error buffer still terminates. */
    char tiny[8];
    expect_int("tiny buffer still rejects",
               dsd_analog_width_check(DSD_ANALOG_DEMOD_FM, 25000, 24000, tiny, sizeof tiny), -1);
    expect_int("tiny buffer terminated", (int)strlen(tiny) < (int)sizeof tiny, 1);
    expect_int("no buffer still rejects", dsd_analog_width_check(DSD_ANALOG_DEMOD_FM, 25000, 24000, NULL, 0), -1);
}

/* The selectable RTL DSP bandwidths, and the list of them that filter a width: the fix the validator names, and what
   the width and bandwidth refusals in app-control put in their toasts. */
static void
test_rtl_bandwidth_helpers(void) {
    static const int selectable[] = {4, 6, 8, 12, 16, 24, 48};
    for (size_t i = 0; i < sizeof selectable / sizeof selectable[0]; i++) {
        char label[48];
        DSD_SNPRINTF(label, sizeof label, "%d kHz selectable", selectable[i]);
        expect_int(label, dsd_analog_rtl_dsp_bw_is_selectable(selectable[i]), 1);
    }
    static const int not_selectable[] = {0, -24, 5, 20, 32, 96, 48000};
    for (size_t i = 0; i < sizeof not_selectable / sizeof not_selectable[0]; i++) {
        char label[48];
        DSD_SNPRINTF(label, sizeof label, "%d kHz not selectable", not_selectable[i]);
        expect_int(label, dsd_analog_rtl_dsp_bw_is_selectable(not_selectable[i]), 0);
    }

    char fits[64];
    expect_int("8k fits", dsd_analog_width_fitting_rtl_bandwidths(8000, fits, sizeof fits), 0);
    expect_str("8k list", fits, "12, 16, 24 or 48");
    expect_int("12.5k fits", dsd_analog_width_fitting_rtl_bandwidths(12500, fits, sizeof fits), 0);
    expect_str("12.5k list", fits, "16, 24 or 48");
    expect_int("16k fits", dsd_analog_width_fitting_rtl_bandwidths(16000, fits, sizeof fits), 0);
    expect_str("16k list", fits, "24 or 48");
    expect_int("25k fits", dsd_analog_width_fitting_rtl_bandwidths(25000, fits, sizeof fits), 0);
    expect_str("25k list", fits, "48");
    expect_int("AM 5k fits", dsd_analog_width_fitting_rtl_bandwidths(5000, fits, sizeof fits), 0);
    expect_str("AM 5k list", fits, "8, 12, 16, 24 or 48");
    /* No selectable bandwidth filters it (the 48 kHz rate stops at 42 kHz), and a width of 0 is no width: empty. */
    DSD_SNPRINTF(fits, sizeof fits, "%s", "stale");
    expect_int("50k fits none", dsd_analog_width_fitting_rtl_bandwidths(50000, fits, sizeof fits), 0);
    expect_str("50k list empty", fits, "");
    DSD_SNPRINTF(fits, sizeof fits, "%s", "stale");
    expect_int("0 fits none", dsd_analog_width_fitting_rtl_bandwidths(0, fits, sizeof fits), 0);
    expect_str("0 list empty", fits, "");
    /* A short buffer truncates and stays terminated. */
    char tiny[5];
    expect_int("tiny buffer", dsd_analog_width_fitting_rtl_bandwidths(8000, tiny, sizeof tiny), 0);
    expect_int("tiny buffer terminated", (int)strlen(tiny) < (int)sizeof tiny, 1);
    expect_int("NULL buffer", dsd_analog_width_fitting_rtl_bandwidths(8000, NULL, sizeof fits), -1);
    expect_int("empty buffer", dsd_analog_width_fitting_rtl_bandwidths(8000, fits, 0U), -1);
}

int
main(void) {
    test_ranges_and_defaults();
    test_parse();
    test_format();
    test_tap_arithmetic();
    test_max_width_by_rate();
    test_check_table();
    test_check_messages();
    test_rtl_bandwidth_helpers();
    if (g_failures) {
        DSD_FPRINTF(stderr, "RUNTIME_ANALOG_CHANNEL: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("RUNTIME_ANALOG_CHANNEL: OK\n");
    return 0;
}
