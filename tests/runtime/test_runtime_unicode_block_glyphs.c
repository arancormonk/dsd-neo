// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/unicode.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#if DSD_PLATFORM_WIN_NATIVE
#include <windows.h>
#endif

static void
set_env_flag(const char* name, const char* value) {
    int rc = value ? dsd_setenv(name, value, 1) : dsd_unsetenv(name);
    assert(rc == 0);
}

static void
test_locale_init_force_ascii_and_c_locale_detection(void) {
    char out[64];
    char saved_locale[128] = {0};
    const char* current_locale = setlocale(LC_CTYPE, NULL);
    if (current_locale) {
        DSD_SNPRINTF(saved_locale, sizeof(saved_locale), "%s", current_locale);
    }

    set_env_flag("DSD_FORCE_ASCII", "1");
    set_env_flag("DSD_FORCE_UTF8", NULL);
    dsd_unicode_init_locale();
    assert(dsd_unicode_supported() == 0);
    dsd_unicode_init_locale();
    assert(strcmp(dsd_unicode_or_ascii("unicode", "ascii"), "ascii") == 0);

    set_env_flag("DSD_FORCE_ASCII", NULL);
    set_env_flag("DSD_FORCE_UTF8", NULL);
    if (setlocale(LC_CTYPE, "C") != NULL) {
        /* The C locale has no UTF-8. On Windows a UTF-8 console code page still renders UTF-8 bytes, and the console
         * is shared with whatever ran before this test, so there the answer follows the console. */
#if DSD_PLATFORM_WIN_NATIVE
        const int expect_unicode = GetConsoleOutputCP() == CP_UTF8;
#else
        const int expect_unicode = 0;
#endif
        assert(dsd_unicode_supported() == expect_unicode);
        if (!expect_unicode) {
            assert(dsd_unicode_block_glyphs_supported() == 0);
        }

        /* dsd_ascii_fallback() passes text through where UTF-8 renders and replaces non-ASCII with '?' where it
         * does not; the replacement itself is checked under DSD_FORCE_ASCII below, on every platform. */
        static const char mixed[] = "bad "
                                    "\xC2"
                                    "\xA9"
                                    " "
                                    "\x80"
                                    " end";
        assert(dsd_ascii_fallback(mixed, out, sizeof(out)) == out);
        if (expect_unicode) {
            assert(strcmp(out, mixed) == 0);
        } else {
            assert(strcmp(out, "bad ? ? end") == 0);
        }
    }

    if (saved_locale[0] != '\0') {
        assert(setlocale(LC_CTYPE, saved_locale) != NULL);
    }
}

static void
test_force_flags_and_selector_helpers(void) {
    char out[64];

    set_env_flag("DSD_FORCE_ASCII", "1");
    set_env_flag("DSD_FORCE_UTF8", "1");
    assert(dsd_unicode_supported() == 0);
    assert(dsd_unicode_block_glyphs_supported() == 0);
    assert(strcmp(dsd_unicode_or_ascii("unicode", "ascii"), "ascii") == 0);
    assert(strcmp(dsd_degrees_glyph(), " deg") == 0);

    assert(dsd_ascii_fallback("Temp \xC2\xB0 \xC2\xB5 \xC3\x97 \xE2\x89\x88", out, sizeof(out)) == out);
    assert(strcmp(out, "Temp  deg u x ~") == 0);

    set_env_flag("DSD_FORCE_ASCII", NULL);
    set_env_flag("DSD_FORCE_UTF8", "1");
    assert(dsd_unicode_supported() == 1);
    assert(dsd_unicode_block_glyphs_supported() == 1);
    assert(strcmp(dsd_unicode_or_ascii("unicode", "ascii"), "unicode") == 0);
    assert(strcmp(dsd_degrees_glyph(), "\xC2\xB0") == 0);

    assert(dsd_ascii_fallback("abcdef", out, 4) == out);
    assert(strcmp(out, "abc") == 0);
}

static void
test_ascii_fallback_replacements_and_bounds(void) {
    char out[64];

    set_env_flag("DSD_FORCE_ASCII", "1");
    set_env_flag("DSD_FORCE_UTF8", NULL);

    assert(dsd_ascii_fallback(NULL, out, sizeof(out)) == out);
    out[0] = 'x';
    assert(dsd_ascii_fallback("ignored", out, 0) == out);
    assert(out[0] == 'x');

    assert(dsd_ascii_fallback("A\xE2\x80\x93"
                              "B\xE2\x80\x94"
                              "C\xE2\x80\xA6"
                              "D",
                              out, sizeof(out))
           == out);
    assert(strcmp(out, "A-B-C...D") == 0);

    assert(dsd_ascii_fallback("bad \xE2\x98\x83 \xF0\x9F\x98\x80 end", out, sizeof(out)) == out);
    assert(strcmp(out, "bad ? ? end") == 0);

    /* A two-byte sequence and a stray continuation byte each become one '?'. */
    assert(dsd_ascii_fallback("bad "
                              "\xC2"
                              "\xA9"
                              " "
                              "\x80"
                              " end",
                              out, sizeof(out))
           == out);
    assert(strcmp(out, "bad ? ? end") == 0);

    assert(dsd_ascii_fallback("123456", out, 5) == out);
    assert(strcmp(out, "1234") == 0);
}

int
main(void) {
    set_env_flag("DSD_FORCE_ASCII", NULL);
    set_env_flag("DSD_FORCE_UTF8", NULL);

    set_env_flag("DSD_FORCE_ASCII", "1");
    set_env_flag("DSD_FORCE_UTF8", "1");
    assert(dsd_unicode_block_glyphs_supported() == 0);

    set_env_flag("DSD_FORCE_ASCII", NULL);
    set_env_flag("DSD_FORCE_UTF8", "1");
    assert(dsd_unicode_block_glyphs_supported() == 1);

    set_env_flag("DSD_FORCE_ASCII", NULL);
    set_env_flag("DSD_FORCE_UTF8", NULL);

    test_locale_init_force_ascii_and_c_locale_detection();
    test_force_flags_and_selector_helpers();
    test_ascii_fallback_replacements_and_bounds();

    set_env_flag("DSD_FORCE_ASCII", NULL);
    set_env_flag("DSD_FORCE_UTF8", NULL);

    printf("RUNTIME_UNICODE_BLOCK_GLYPHS: OK\n");
    return 0;
}
