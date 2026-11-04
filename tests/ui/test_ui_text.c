// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The text model the status row and the chooser share (ui_text.h, issues #669 and #672): what is drawn as '?', how many
 * cells text takes, where a cut falls and which marker follows it - on ncurses (wcwidth() in a UTF-8 locale), and,
 * built again with DSD_USE_PDCURSES, on a wide PDCurses (one cell per Basic Multilingual Plane code point) and a narrow
 * one (ASCII only, whatever the locale or the user asks).
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include "test_env.h"
#include "ui_text.h"

#if defined(DSD_USE_PDCURSES) && !defined(DSD_HAS_PDCURSES_WIDE_API)
#define UI_TEXT_TEST_NARROW 1
#else
#define UI_TEXT_TEST_NARROW 0
#endif

/* "漢字" (two wide characters), "é" as e + U+0301 (one cell), U+1F600 (outside the BMP), a surrogate's encoding. */
static const char kCjk[] = "\xE6\xBC\xA2\xE5\xAD\x97";
static const char kCombining[] = "e\xCC\x81";
static const char kEmoji[] = "\xF0\x9F\x98\x80";
static const char kSurrogate[] = "\xED\xA0\x80";

static void
set_env(const char* name, const char* value) {
    if (value) {
        (void)dsd_test_setenv(name, value, 1);
    } else {
        (void)dsd_test_unsetenv(name);
    }
}

static void
expect_sanitized(int unicode, const char* in, const char* want) {
    char out[64];
    const size_t n = ui_text_sanitize(unicode, in, strlen(in), out, sizeof out);
    if (strcmp(out, want) != 0 || n != strlen(want)) {
        DSD_FPRINTF(stderr, "sanitize(%d) '%s' -> '%s' (%zu), want '%s'\n", unicode, in, out, n, want);
        assert(0);
    }
}

static size_t
fit(int unicode, const char* text, int budget, const char** marker) {
    return ui_text_fit(unicode, text, strlen(text), budget, marker);
}

/* Without UTF-8 output (every build): ASCII only, three periods. */
static void
test_ascii_model(void) {
    assert(strcmp(ui_text_marker(0), "...") == 0);
    assert(ui_text_marker_cells(0) == 3);
    expect_sanitized(0,
                     "a\tb\x01"
                     "c\x7f"
                     "d \xC3\xA9 \xFF;",
                     "a?b?c?d ? ?;");
    expect_sanitized(0, kCjk, "??");
    assert(ui_text_cells(0, kCjk, strlen(kCjk)) == 2);
    assert(ui_text_cells(0, kEmoji, strlen(kEmoji)) == 1);

    const char* marker = NULL;
    assert(fit(0, "short", 10, &marker) == 5 && marker == NULL);
    assert(fit(0, "exactly ten", 11, &marker) == 11 && marker == NULL);
    /* Ten cells for "a long line here": seven of text, then the marker; the blank before the cut is dropped. */
    assert(fit(0, "a long line here", 10, &marker) == 6 && marker != NULL && strcmp(marker, "...") == 0);
    /* No room for a marker beside any text: a bare cut. */
    assert(fit(0, "abcdef", 3, &marker) == 3 && marker == NULL);
    assert(fit(0, "abcdef", 0, &marker) == 0 && marker == NULL);
    assert(ui_text_fit(0, NULL, 0, 5, &marker) == 0 && marker == NULL);
    char out[4];
    assert(ui_text_sanitize(0, "abc", 3, out, 0) == 0);
}

/* With UTF-8 output: each code point the library can draw, in the cells the library gives it. */
static void
test_unicode_model(void) {
    assert(strcmp(ui_text_marker(1), "\xE2\x80\xA6") == 0);
    assert(ui_text_marker_cells(1) == 1);
    /* C0/C1 controls, DEL, stray bytes, overlong forms and surrogates are '?', one per byte that is not a sequence. */
    expect_sanitized(1, "a\tb \xC2\x85 \x7f \xFF \xC0\xAF", "a?b ? ? ? ??");
    expect_sanitized(1, kSurrogate, "???");
    expect_sanitized(1, kCjk, kCjk);
    expect_sanitized(1, kCombining, kCombining);
    assert(ui_text_cells(1, "\xFF", 1) == 1);
    /* A full buffer stops before a code point that does not fit, never inside one. */
    char out[4];
    assert(ui_text_sanitize(1, "ab\xC3\xA9", 4, out, sizeof out) == 2 && strcmp(out, "ab") == 0);
#if defined(DSD_USE_PDCURSES)
    /* PDCurses: one cell per BMP code point (a combining mark too); outside the BMP is '?'. */
    assert(ui_text_cells(1, kCjk, strlen(kCjk)) == 2);
    assert(ui_text_cells(1, kCombining, strlen(kCombining)) == 2);
    expect_sanitized(1, kEmoji, "?");
    assert(ui_text_cells(1, kEmoji, strlen(kEmoji)) == 1);
    const char* marker = NULL;
    assert(fit(1, "abcdef", 4, &marker) == 3 && marker != NULL && strcmp(marker, "\xE2\x80\xA6") == 0);
#else
    assert(ui_text_cells(1, kCombining, strlen(kCombining)) == 1);
    assert(ui_text_cells(1, kCjk, strlen(kCjk)) == 4);
    expect_sanitized(1, kEmoji, kEmoji);
    assert(ui_text_cells(1, kEmoji, strlen(kEmoji)) == 2);

    /* A cut never splits a wide character: "漢字" in three cells keeps one. */
    assert(ui_text_cut(1, kCjk, strlen(kCjk), 3) == 3);
    const char* marker = NULL;
    /* Four cells for "漢字漢字" (eight): one character, then the marker, three cells in all. */
    char cjk2[16];
    DSD_SNPRINTF(cjk2, sizeof cjk2, "%s%s", kCjk, kCjk);
    assert(fit(1, cjk2, 4, &marker) == 3 && marker != NULL && strcmp(marker, "\xE2\x80\xA6") == 0);
    /* An emoji is two cells too, and never split. */
    char emoji2[16];
    DSD_SNPRINTF(emoji2, sizeof emoji2, "%s%s", kEmoji, kEmoji);
    assert(fit(1, emoji2, 3, &marker) == 4 && marker != NULL);
    /* A combining mark stays with its letter at the cut. */
    assert(ui_text_cut(1, "xe\xCC\x81y", 5, 2) == 4);
    assert(fit(1, "xe\xCC\x81yz", 3, &marker) == 4 && marker != NULL);
    /* A fitting text is whole, without a marker. */
    assert(fit(1, kCombining, 1, &marker) == strlen(kCombining) && marker == NULL);
#endif
}

static int
use_utf8_locale(void) {
    static const char* const names[] = {"C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (setlocale(LC_CTYPE, names[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* The output decision: the user's preference and what the library and locale can draw. */
static void
test_unicode_output(int have_utf8) {
    set_env("DSD_FORCE_ASCII", NULL);
    set_env("DSD_FORCE_UTF8", NULL);
#if UI_TEXT_TEST_NARROW
    /* A narrow PDCurses writes each byte of a sequence as a character of its own: never UTF-8, even when asked. */
    (void)have_utf8;
    (void)use_utf8_locale();
    assert(ui_text_unicode_output() == 0);
    set_env("DSD_FORCE_UTF8", "1");
    assert(ui_text_unicode_output() == 0);
    set_env("DSD_FORCE_UTF8", NULL);
#elif defined(DSD_USE_PDCURSES)
    /* A wide PDCurses draws UTF-8 whenever the user's preference allows it, whatever the C library's locale. */
    (void)have_utf8;
    set_env("DSD_FORCE_UTF8", "1");
    assert(ui_text_unicode_output() == 1);
    set_env("DSD_FORCE_ASCII", "1"); /* both forced: ASCII wins */
    assert(ui_text_unicode_output() == 0);
    set_env("DSD_FORCE_ASCII", NULL);
    set_env("DSD_FORCE_UTF8", NULL);
#else
    if (have_utf8) {
        (void)use_utf8_locale();
        assert(ui_text_unicode_output() == 1);
        set_env("DSD_FORCE_ASCII", "1");
        assert(ui_text_unicode_output() == 0);
        set_env("DSD_FORCE_ASCII", NULL);
    } else {
        DSD_FPRINTF(stderr, "no UTF-8 locale installed; skipping the UTF-8 output case\n");
    }
    /* Under a C locale the C library cannot encode the marker, so a forced UTF-8 stays ASCII on ncurses. */
    (void)setlocale(LC_CTYPE, "C");
    set_env("DSD_FORCE_UTF8", "1");
    assert(ui_text_unicode_output() == 0);
    set_env("DSD_FORCE_UTF8", NULL);
#endif
}

int
main(void) {
    const int have_utf8 = use_utf8_locale();
#if defined(DSD_USE_PDCURSES)
    /* The PDCurses model does not ask the C library: it always runs. */
    test_unicode_model();
#else
    if (have_utf8) {
        test_unicode_model();
    } else {
        DSD_FPRINTF(stderr, "no UTF-8 locale installed; skipping the wcwidth() cases\n");
    }
#endif
    (void)setlocale(LC_CTYPE, "C");
    test_ascii_model();
    test_unicode_output(have_utf8);
    printf("UI_TEXT: OK\n");
    return 0;
}
