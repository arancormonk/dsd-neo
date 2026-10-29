// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/freq_parse.h>

#define ENTRY_SENTINEL 0xDEADBEEFu

typedef struct {
    const char* text;
    int result;
    uint32_t hz;
} entry_case;

static const entry_case k_entry_cases[] = {
    /* Bare numbers below 1000000 are MHz, at or above are Hz (D2); units in any case. */
    {"146.52", DSD_FREQ_ENTRY_OK, 146520000u},
    {"146.52M", DSD_FREQ_ENTRY_OK, 146520000u},
    {"146.52 MHz", DSD_FREQ_ENTRY_OK, 146520000u},
    {"146.52mhz", DSD_FREQ_ENTRY_OK, 146520000u},
    {"  146.52  ", DSD_FREQ_ENTRY_OK, 146520000u},
    {"446", DSD_FREQ_ENTRY_OK, 446000000u},
    {"4294", DSD_FREQ_ENTRY_OK, 4294000000u},
    {"4294.967295", DSD_FREQ_ENTRY_OK, 4294967295u},
    {"4294967295", DSD_FREQ_ENTRY_OK, 4294967295u},
    {"4294967295Hz", DSD_FREQ_ENTRY_OK, 4294967295u},
    {"4.294967295G", DSD_FREQ_ENTRY_OK, 4294967295u},
    {"1000000", DSD_FREQ_ENTRY_OK, 1000000u},
    {"162550000", DSD_FREQ_ENTRY_OK, 162550000u},
    {"1Hz", DSD_FREQ_ENTRY_OK, 1u},
    {"1 hz", DSD_FREQ_ENTRY_OK, 1u},
    {"0.000001", DSD_FREQ_ENTRY_OK, 1u},
    {"1.000Hz", DSD_FREQ_ENTRY_OK, 1u},
    {"146.520000000", DSD_FREQ_ENTRY_OK, 146520000u},
    {"128155.625k", DSD_FREQ_ENTRY_OK, 128155625u},
    {".5", DSD_FREQ_ENTRY_OK, 500000u},
    {"146.", DSD_FREQ_ENTRY_OK, 146000000u},
    {"000146.52", DSD_FREQ_ENTRY_OK, 146520000u},
    {"0000146", DSD_FREQ_ENTRY_OK, 146000000u},
    {"146520k", DSD_FREQ_ENTRY_OK, 146520000u},
    {"162550000.0", DSD_FREQ_ENTRY_OK, 162550000u},
    {"1.5GHz", DSD_FREQ_ENTRY_OK, 1500000000u},
    {"146520KHZ", DSD_FREQ_ENTRY_OK, 146520000u},
    /* 32 bytes before trimming is still read. */
    {"                      146.52 MHz", DSD_FREQ_ENTRY_OK, 146520000u},

    /* Bare 4295..999999: too large as MHz, too small as Hz, decided before any arithmetic. */
    {"4295", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},
    {"14652", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},
    {"146520", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},
    {"146560", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},
    {"851012", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},
    {"999999", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},
    {"5000.5", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},
    {"99999.999999", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},
    {"4295.0000001", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},
    {"851012.5", DSD_FREQ_ENTRY_AMBIGUOUS, 0u},

    /* Above UINT32_MAX Hz in any unit; the range wins over a sub-Hz tail. */
    {"4294.967296", DSD_FREQ_ENTRY_TOO_HIGH, 0u},
    {"4294967296", DSD_FREQ_ENTRY_TOO_HIGH, 0u},
    {"4.294967296 GHz", DSD_FREQ_ENTRY_TOO_HIGH, 0u},
    {"4294967296.1 Hz", DSD_FREQ_ENTRY_TOO_HIGH, 0u},
    {"4294967295.1 Hz", DSD_FREQ_ENTRY_TOO_HIGH, 0u},
    {"4294.9672951 MHz", DSD_FREQ_ENTRY_TOO_HIGH, 0u},
    {"99999999999999999999999999999999", DSD_FREQ_ENTRY_TOO_HIGH, 0u},

    /* A non-zero digit below 1 Hz. */
    {"0.0000001", DSD_FREQ_ENTRY_SUB_HZ, 0u},
    {"1.5Hz", DSD_FREQ_ENTRY_SUB_HZ, 0u},
    {"146.5200001", DSD_FREQ_ENTRY_SUB_HZ, 0u},
    {"128155.6255k", DSD_FREQ_ENTRY_SUB_HZ, 0u},
    {"162550000.5", DSD_FREQ_ENTRY_SUB_HZ, 0u},

    {"0", DSD_FREQ_ENTRY_ZERO, 0u},
    {"0.0", DSD_FREQ_ENTRY_ZERO, 0u},
    {"0 MHz", DSD_FREQ_ENTRY_ZERO, 0u},
    {"0.0000000", DSD_FREQ_ENTRY_ZERO, 0u},

    {"", DSD_FREQ_ENTRY_EMPTY, 0u},
    {"   ", DSD_FREQ_ENTRY_EMPTY, 0u},

    {"146,52", DSD_FREQ_ENTRY_DECIMAL_COMMA, 0u},
    {"146,52 MHz", DSD_FREQ_ENTRY_DECIMAL_COMMA, 0u},

    {"-146.52", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"+146.52", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"nan", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"inf", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"1e6", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"0x10", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"MHz", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {".", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"146.52.1", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"146 .52", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"146. 52", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"146.52 M Hz", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"\t146.52", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"146?52", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"146.52?", DSD_FREQ_ENTRY_SYNTAX, 0u},
    /* Non-ASCII bytes (a UTF-8 lead byte here) are never letters or digits. */
    {"146\xC2", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"\xC2", DSD_FREQ_ENTRY_SYNTAX, 0u},
    {"146.52 M\xC2", DSD_FREQ_ENTRY_SYNTAX, 0u},

    {"146.52Mhzz", DSD_FREQ_ENTRY_UNIT, 0u},
    {"146.52 mm", DSD_FREQ_ENTRY_UNIT, 0u},

    /* Length counts before trimming: 33 bytes is refused. */
    {"123456789012345678901234567890123", DSD_FREQ_ENTRY_TOO_LONG, 0u},
    {"                       146.52 MHz", DSD_FREQ_ENTRY_TOO_LONG, 0u},
};

static void
test_entry_table(void) {
    for (size_t i = 0; i < sizeof k_entry_cases / sizeof k_entry_cases[0]; i++) {
        const entry_case* c = &k_entry_cases[i];
        uint32_t hz = ENTRY_SENTINEL;
        int rc = dsd_parse_freq_entry_hz(c->text, &hz);
        if (rc != c->result || (rc == DSD_FREQ_ENTRY_OK ? hz != c->hz : hz != ENTRY_SENTINEL)) {
            DSD_FPRINTF(stderr, "entry case %zu \"%s\": result %d (want %d), hz %u (want %u)\n", i, c->text, rc,
                        c->result, (unsigned)hz, rc == DSD_FREQ_ENTRY_OK ? (unsigned)c->hz : (unsigned)ENTRY_SENTINEL);
        }
        assert(rc == c->result);
        if (rc == DSD_FREQ_ENTRY_OK) {
            assert(hz == c->hz);
        } else {
            assert(hz == ENTRY_SENTINEL);
        }
    }
}

static void
test_entry_null(void) {
    uint32_t hz = ENTRY_SENTINEL;
    assert(dsd_parse_freq_entry_hz(NULL, &hz) == DSD_FREQ_ENTRY_EMPTY);
    assert(hz == ENTRY_SENTINEL);
    assert(dsd_parse_freq_entry_hz("146.52", NULL) == DSD_FREQ_ENTRY_EMPTY);
    assert(dsd_parse_freq_entry_hz(NULL, NULL) == DSD_FREQ_ENTRY_EMPTY);
}

static void
test_entry_lengths(void) {
    char text[DSD_FREQ_ENTRY_MAX_LEN + 2];
    uint32_t hz = ENTRY_SENTINEL;

    /* 32 digits is read (and is above the range); one more is too long. */
    DSD_MEMSET(text, '9', sizeof text);
    text[DSD_FREQ_ENTRY_MAX_LEN] = '\0';
    assert(strlen(text) == 32u);
    assert(dsd_parse_freq_entry_hz(text, &hz) == DSD_FREQ_ENTRY_TOO_HIGH);
    text[DSD_FREQ_ENTRY_MAX_LEN] = '9';
    text[DSD_FREQ_ENTRY_MAX_LEN + 1] = '\0';
    assert(strlen(text) == 33u);
    assert(dsd_parse_freq_entry_hz(text, &hz) == DSD_FREQ_ENTRY_TOO_LONG);
    assert(hz == ENTRY_SENTINEL);

    /* A long text is refused without reading past the limit's next byte. */
    char longer[200];
    DSD_MEMSET(longer, ' ', sizeof longer);
    longer[sizeof longer - 1] = '\0';
    assert(dsd_parse_freq_entry_hz(longer, &hz) == DSD_FREQ_ENTRY_TOO_LONG);
    assert(hz == ENTRY_SENTINEL);
}

static void
test_entry_messages(void) {
    static const struct {
        int result;
        const char* text;
    } k_messages[] = {
        {DSD_FREQ_ENTRY_TOO_LONG, "Too long: at most 32 characters"},
        {DSD_FREQ_ENTRY_DECIMAL_COMMA, "Use '.' for decimals, e.g. 146.52"},
        {DSD_FREQ_ENTRY_SYNTAX, "Not a frequency. Type MHz, e.g. 146.52"},
        {DSD_FREQ_ENTRY_UNIT, "Unknown unit. Use Hz, k, M or G"},
        {DSD_FREQ_ENTRY_SUB_HZ, "Finer than 1 Hz. Use at most 6 decimals in MHz"},
        {DSD_FREQ_ENTRY_ZERO, "Frequency must be above 0"},
        {DSD_FREQ_ENTRY_AMBIGUOUS, "Too large as MHz. Add a unit, e.g. 146520k or 146.52M"},
        {DSD_FREQ_ENTRY_TOO_HIGH, "Above the 4294.967295 MHz input limit"},
    };

    for (size_t i = 0; i < sizeof k_messages / sizeof k_messages[0]; i++) {
        const char* text = dsd_freq_entry_result_text(k_messages[i].result);
        assert(text != NULL);
        assert(strcmp(text, k_messages[i].text) == 0);
        assert(strlen(text) <= 56u);
    }
    /* Every refusal has a message; OK, EMPTY and unknown results have none. */
    for (int r = DSD_FREQ_ENTRY_OK; r <= DSD_FREQ_ENTRY_TOO_HIGH; r++) {
        const char* text = dsd_freq_entry_result_text(r);
        assert(text != NULL);
        assert(strlen(text) <= 56u);
        if (r == DSD_FREQ_ENTRY_OK || r == DSD_FREQ_ENTRY_EMPTY) {
            assert(text[0] == '\0');
        } else {
            assert(text[0] != '\0');
        }
    }
    assert(strcmp(dsd_freq_entry_result_text(-1), "") == 0);
    assert(strcmp(dsd_freq_entry_result_text(DSD_FREQ_ENTRY_TOO_HIGH + 1), "") == 0);
}

static void
test_mhz_text(void) {
    char out[32];
    assert(dsd_freq_hz_mhz_text(0u, out, sizeof out) == 8);
    assert(strcmp(out, "0.000000") == 0);
    assert(dsd_freq_hz_mhz_text(1u, out, sizeof out) == 8);
    assert(strcmp(out, "0.000001") == 0);
    assert(dsd_freq_hz_mhz_text(999999u, out, sizeof out) == 8);
    assert(strcmp(out, "0.999999") == 0);
    assert(dsd_freq_hz_mhz_text(1000000u, out, sizeof out) == 8);
    assert(strcmp(out, "1.000000") == 0);
    assert(dsd_freq_hz_mhz_text(851012500u, out, sizeof out) == 10);
    assert(strcmp(out, "851.012500") == 0);
    assert(dsd_freq_hz_mhz_text(UINT32_MAX, out, sizeof out) == 11);
    assert(strcmp(out, "4294.967295") == 0);

    /* A short buffer truncates safely and reports the full length, as snprintf does. */
    char small[8];
    DSD_MEMSET(small, 'x', sizeof small);
    assert(dsd_freq_hz_mhz_text(851012500u, small, 5) == 10);
    assert(strcmp(small, "851.") == 0);
    assert(small[5] == 'x');

    assert(dsd_freq_hz_mhz_text(1u, NULL, sizeof out) == -1);
    DSD_MEMSET(small, 'x', sizeof small);
    assert(dsd_freq_hz_mhz_text(1u, small, 0) == -1);
    assert(small[0] == 'x');
}

int
main(void) {
    test_entry_table();
    test_entry_null();
    test_entry_lengths();
    test_entry_messages();
    test_mhz_text();

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
    DSD_MEMSET(longtxt, '0', sizeof longtxt);
    DSD_MEMCPY(longtxt, "146.52", 6);
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
