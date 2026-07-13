// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/unicode.h>
#include <dsd-neo/ui/ncurses_snr.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

#if defined(DSD_NEO_FAST_MATH) || defined(__FAST_MATH__) || defined(_M_FP_FAST)
#define DSD_NEO_TEST_FAST_MATH 1
#elif defined(__FINITE_MATH_ONLY__) && (__FINITE_MATH_ONLY__ != 0)
#define DSD_NEO_TEST_FAST_MATH 1
#else
#define DSD_NEO_TEST_FAST_MATH 0
#endif

struct render_capture {
    char out[128];
    size_t len;
    int attr_get_calls;
    int attr_set_calls;
    int attron_count;
    int attroff_count;
    unsigned long attron_values[64];
    unsigned long attroff_values[64];
    unsigned long saved_attrs;
    short saved_pair;
    unsigned long last_attr_set_attrs;
    short last_attr_set_pair;
};

static struct render_capture g_capture;

int
dsd_unicode_block_glyphs_supported(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

static void
capture_reset(struct render_capture* cap) {
    DSD_MEMSET(cap, 0, sizeof(*cap));
    cap->saved_attrs = 0x1234UL;
    cap->saved_pair = 5;
}

static int
capture_ch(unsigned long ch) {
    struct render_capture* cap = &g_capture;
    if (cap->len + 1 < sizeof(cap->out)) {
        cap->out[cap->len++] = (char)(ch & A_CHARTEXT);
        cap->out[cap->len] = '\0';
    }
    return 0;
}

static int
capture_str(const char* text) {
    struct render_capture* cap = &g_capture;
    if (!text) {
        return 0;
    }
    while (*text && cap->len + 1 < sizeof(cap->out)) {
        cap->out[cap->len++] = *text++;
    }
    cap->out[cap->len] = '\0';
    return 0;
}

static int
capture_attron(unsigned long attrs) {
    struct render_capture* cap = &g_capture;
    if (cap->attron_count < (int)(sizeof(cap->attron_values) / sizeof(cap->attron_values[0]))) {
        cap->attron_values[cap->attron_count] = attrs;
    }
    cap->attron_count++;
    return 0;
}

static int
capture_attroff(unsigned long attrs) {
    struct render_capture* cap = &g_capture;
    if (cap->attroff_count < (int)(sizeof(cap->attroff_values) / sizeof(cap->attroff_values[0]))) {
        cap->attroff_values[cap->attroff_count] = attrs;
    }
    cap->attroff_count++;
    return 0;
}

static int
capture_attr_get(unsigned long* attrs, short* pair) {
    struct render_capture* cap = &g_capture;
    cap->attr_get_calls++;
    if (attrs) {
        *attrs = cap->saved_attrs;
    }
    if (pair) {
        *pair = cap->saved_pair;
    }
    return 0;
}

static int
capture_attr_set(unsigned long attrs, short pair) {
    struct render_capture* cap = &g_capture;
    cap->attr_set_calls++;
    cap->last_attr_set_attrs = attrs;
    cap->last_attr_set_pair = pair;
    return 0;
}

static void
install_capture(void) {
    dsd_ncurses_snr_set_emit_hooks_for_test(capture_ch, capture_str, capture_attron, capture_attroff, capture_attr_get,
                                            capture_attr_set);
}

static void
clear_capture_hooks(void) {
    dsd_ncurses_snr_set_emit_hooks_for_test(NULL, NULL, NULL, NULL, NULL, NULL);
}

static void
assert_bars(double snr_db, int expected) {
    int actual = dsd_ncurses_snr_meter_bar_count_for_test(snr_db);
    if (actual != expected) {
        DSD_FPRINTF(stderr, "bar count for %.1f dB: expected %d, got %d\n", snr_db, expected, actual);
    }
    assert(actual == expected);
}

static void
assert_ascii(double snr_db, const char* expected) {
    char actual[16];
    DSD_MEMSET(actual, 'X', sizeof(actual));
    dsd_ncurses_snr_meter_ascii_for_test(snr_db, actual, sizeof(actual));
    if (strcmp(actual, expected) != 0) {
        DSD_FPRINTF(stderr, "ASCII meter for %.1f dB: expected '%s', got '%s'\n", snr_db, expected, actual);
    }
    assert(strcmp(actual, expected) == 0);
    assert(actual[9] == '\0');
}

static void
assert_double_eq(double actual, double expected) {
    if (fabs(actual - expected) > 0.000001) {
        DSD_FPRINTF(stderr, "double mismatch: expected %.6f, got %.6f\n", expected, actual);
    }
    assert(fabs(actual - expected) <= 0.000001);
}

static void
test_snr_history_routes_clamps_and_wraps(void) {
    dsd_ncurses_snr_hist_reset_for_test();
    assert(dsd_ncurses_snr_hist_len_for_test(0) == 0);
    assert(dsd_ncurses_snr_hist_head_for_test(0) == 0);
    assert(dsd_ncurses_snr_hist_len_for_test(1) == 0);
    assert(dsd_ncurses_snr_hist_len_for_test(2) == 0);

    snr_hist_push(0, -51.0);
    assert(dsd_ncurses_snr_hist_len_for_test(0) == 0);

    snr_hist_push(0, -50.0);
    assert(dsd_ncurses_snr_hist_len_for_test(0) == 1);
    assert(dsd_ncurses_snr_hist_head_for_test(0) == 1);
    assert_double_eq(dsd_ncurses_snr_hist_value_for_test(0, 0), -50.0);

    snr_hist_push(1, 61.0);
    assert(dsd_ncurses_snr_hist_len_for_test(1) == 1);
    assert(dsd_ncurses_snr_hist_head_for_test(1) == 1);
    assert_double_eq(dsd_ncurses_snr_hist_value_for_test(1, 0), 60.0);

    snr_hist_push(2, 7.0);
    snr_hist_push(99, 8.0);
    assert(dsd_ncurses_snr_hist_len_for_test(2) == 2);
    assert(dsd_ncurses_snr_hist_len_for_test(99) == 2);
    assert(dsd_ncurses_snr_hist_head_for_test(2) == 2);
    assert_double_eq(dsd_ncurses_snr_hist_value_for_test(2, 0), 7.0);
    assert_double_eq(dsd_ncurses_snr_hist_value_for_test(2, 1), 8.0);

    dsd_ncurses_snr_hist_reset_for_test();
    for (int i = 0; i < 51; i++) {
        snr_hist_push(0, (double)i);
    }
    assert(dsd_ncurses_snr_hist_len_for_test(0) == 48);
    assert(dsd_ncurses_snr_hist_head_for_test(0) == 3);
    assert_double_eq(dsd_ncurses_snr_hist_value_for_test(0, 3), 3.0);
    assert_double_eq(dsd_ncurses_snr_hist_value_for_test(0, 2), 50.0);
}

static void
test_snr_sparkline_window_helpers(void) {
    assert(dsd_ncurses_snr_hist_render_count_for_test(0, 24) == 0);
    assert(dsd_ncurses_snr_hist_render_count_for_test(7, 24) == 7);
    assert(dsd_ncurses_snr_hist_render_count_for_test(48, 24) == 24);

    assert(dsd_ncurses_snr_hist_render_start_for_test(3, 48, 48) == 3);
    assert(dsd_ncurses_snr_hist_render_start_for_test(3, 48, 24) == 27);
    assert(dsd_ncurses_snr_hist_render_start_for_test(10, 5, 3) == 7);

    assert(dsd_ncurses_snr_level_index_for_test(-20.0, -15.0, 45.0, 8) == 0);
    assert(dsd_ncurses_snr_level_index_for_test(30.0, -15.0, 45.0, 8) == 7);
    assert(dsd_ncurses_snr_level_index_for_test(100.0, -15.0, 45.0, 8) == 7);
    assert(dsd_ncurses_snr_level_index_for_test(0.0, -15.0, 45.0, 8) == 2);
}

static void
test_snr_public_meter_rendering_ascii_and_color_restore(void) {
    struct render_capture* cap = &g_capture;
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));

    capture_reset(cap);
    install_capture();
    print_snr_meter(&opts, 12.0, 0);
    assert(strcmp(cap->out, "| | | |  ") == 0);
    assert(cap->attr_get_calls == 1);
    assert(cap->attron_count == 4);
    assert(cap->attroff_count == 4);
    for (int i = 0; i < cap->attron_count; i++) {
        assert(cap->attron_values[i] == (unsigned long)COLOR_PAIR(6));
        assert(cap->attroff_values[i] == (unsigned long)COLOR_PAIR(6));
    }
    assert(cap->attr_set_calls == 5);
    assert(cap->last_attr_set_attrs == cap->saved_attrs);
    assert(cap->last_attr_set_pair == cap->saved_pair);
    clear_capture_hooks();
}

static void
test_snr_public_sparkline_rendering_ascii_and_quality_colors(void) {
    struct render_capture* cap = &g_capture;
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));

    dsd_ncurses_snr_hist_reset_for_test();
    print_snr_sparkline(&opts, 0);

    capture_reset(cap);
    install_capture();
    snr_hist_push(0, -15.0);
    snr_hist_push(0, 4.0);
    snr_hist_push(0, 10.0);
    snr_hist_push(0, 30.0);
    print_snr_sparkline(&opts, 0);
    assert(strcmp(cap->out, ".-=#") == 0);
    assert(cap->attr_get_calls == 1);
    assert(cap->attron_count == 4);
    assert(cap->attroff_count == 4);
    assert(cap->attron_values[0] == (unsigned long)COLOR_PAIR(13));
    assert(cap->attron_values[1] == (unsigned long)COLOR_PAIR(12));
    assert(cap->attron_values[2] == (unsigned long)COLOR_PAIR(11));
    assert(cap->attron_values[3] == (unsigned long)COLOR_PAIR(11));
    for (int i = 0; i < cap->attron_count; i++) {
        assert(cap->attroff_values[i] == cap->attron_values[i]);
    }
    assert(cap->attr_set_calls == 1);
    assert(cap->last_attr_set_attrs == cap->saved_attrs);
    assert(cap->last_attr_set_pair == cap->saved_pair);

    capture_reset(cap);
    install_capture();
    dsd_ncurses_snr_hist_reset_for_test();
    snr_hist_push(1, 9.0);
    snr_hist_push(1, 10.0);
    snr_hist_push(1, 16.0);
    print_snr_sparkline(&opts, 1);
    assert(strcmp(cap->out, "==+") == 0);
    assert(cap->attron_count == 3);
    assert(cap->attron_values[0] == (unsigned long)COLOR_PAIR(13));
    assert(cap->attron_values[1] == (unsigned long)COLOR_PAIR(12));
    assert(cap->attron_values[2] == (unsigned long)COLOR_PAIR(11));
    clear_capture_hooks();
}

int
main(void) {
    assert(dsd_ncurses_snr_use_unicode_for_test(0, 0) == 0);
    assert(dsd_ncurses_snr_use_unicode_for_test(0, 1) == 0);
    assert(dsd_ncurses_snr_use_unicode_for_test(1, 0) == 0);
#if defined(DSD_USE_PDCURSES) && !defined(DSD_HAS_PDCURSES_WIDE_API)
    assert(dsd_ncurses_snr_use_unicode_for_test(1, 1) == 0);
#else
    assert(dsd_ncurses_snr_use_unicode_for_test(1, 1) == 1);
#endif

#if !DSD_NEO_TEST_FAST_MATH
    assert(dsd_ncurses_snr_meter_bar_count_for_test(NAN) == 0);
#endif
    assert_bars(-50.0, 0);
    assert_bars(-20.0, 1);
    assert_bars(-15.0, 1);
    assert_bars(-6.0, 2);
    assert_bars(3.0, 3);
    assert_bars(12.0, 4);
    assert_bars(21.0, 5);
    assert_bars(30.0, 5);
    assert_bars(60.0, 5);

#if !DSD_NEO_TEST_FAST_MATH
    assert_ascii(NAN, "         ");
#endif
    assert_ascii(-50.0, "         ");
    assert_ascii(-15.0, "|        ");
    assert_ascii(-6.0, "| |      ");
    assert_ascii(3.0, "| | |    ");
    assert_ascii(12.0, "| | | |  ");
    assert_ascii(21.0, "| | | | |");

    {
        char tiny[3];
        DSD_MEMSET(tiny, 'X', sizeof(tiny));
        dsd_ncurses_snr_meter_ascii_for_test(21.0, tiny, sizeof(tiny));
        assert(strcmp(tiny, "| ") == 0);
        assert(tiny[2] == '\0');
    }

    test_snr_history_routes_clamps_and_wraps();
    test_snr_sparkline_window_helpers();
    test_snr_public_meter_rendering_ascii_and_color_restore();
    test_snr_public_sparkline_rendering_ascii_and_quality_colors();

    printf("UI_SNR_METER: OK\n");
    return 0;
}
