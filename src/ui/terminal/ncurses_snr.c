// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_snr.c
 * SNR meter rendering
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/unicode.h>
#include <dsd-neo/ui/ncurses_snr.h>
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API) && !defined(PDC_WIDE)
#define PDC_WIDE
#endif
#include <curses.h>
#include <math.h>
#include <stddef.h>
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
#include <wchar.h>
#endif

#include "dsd-neo/core/opts_fwd.h"

enum { SNR_METER_BARS = 5, SNR_METER_WIDTH = (SNR_METER_BARS * 2) - 1 };

#ifdef DSD_NEO_TEST_HOOKS
struct snr_emit_hooks {
    dsd_ncurses_snr_emit_ch_fn emit_ch;
    dsd_ncurses_snr_emit_str_fn emit_str;
    dsd_ncurses_snr_emit_attr_fn emit_attron;
    dsd_ncurses_snr_emit_attr_fn emit_attroff;
    dsd_ncurses_snr_emit_attr_get_fn emit_attr_get;
    dsd_ncurses_snr_emit_attr_set_fn emit_attr_set;
};

static struct snr_emit_hooks g_snr_emit_hooks;

// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
dsd_ncurses_snr_set_emit_hooks_for_test(dsd_ncurses_snr_emit_ch_fn emit_ch, dsd_ncurses_snr_emit_str_fn emit_str,
                                        dsd_ncurses_snr_emit_attr_fn emit_attron,
                                        dsd_ncurses_snr_emit_attr_fn emit_attroff,
                                        dsd_ncurses_snr_emit_attr_get_fn emit_attr_get,
                                        dsd_ncurses_snr_emit_attr_set_fn emit_attr_set) {
    g_snr_emit_hooks.emit_ch = emit_ch;
    g_snr_emit_hooks.emit_str = emit_str;
    g_snr_emit_hooks.emit_attron = emit_attron;
    g_snr_emit_hooks.emit_attroff = emit_attroff;
    g_snr_emit_hooks.emit_attr_get = emit_attr_get;
    g_snr_emit_hooks.emit_attr_set = emit_attr_set;
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed
#endif

static int
snr_emit_ch(chtype ch) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_snr_emit_hooks.emit_ch) {
        return g_snr_emit_hooks.emit_ch((unsigned long)ch);
    }
#endif
    return addch(ch);
}

static int
snr_emit_str(const char* text) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_snr_emit_hooks.emit_str) {
        return g_snr_emit_hooks.emit_str(text);
    }
#endif
    return addstr(text);
}

#if defined(PRETTY_COLORS)
static int
snr_emit_attron(attr_t attrs) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_snr_emit_hooks.emit_attron) {
        return g_snr_emit_hooks.emit_attron((unsigned long)attrs);
    }
#endif
    return attron(attrs);
}

static int
snr_emit_attroff(attr_t attrs) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_snr_emit_hooks.emit_attroff) {
        return g_snr_emit_hooks.emit_attroff((unsigned long)attrs);
    }
#endif
    return attroff(attrs);
}

static int
snr_emit_attr_get(attr_t* attrs, short* pair) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_snr_emit_hooks.emit_attr_get) {
        unsigned long tmp_attrs = 0;
        int ret = g_snr_emit_hooks.emit_attr_get(&tmp_attrs, pair);
        if (attrs) {
            *attrs = (attr_t)tmp_attrs;
        }
        return ret;
    }
#endif
    return attr_get(attrs, pair, NULL);
}

static int
snr_emit_attr_set(attr_t attrs, short pair) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_snr_emit_hooks.emit_attr_set) {
        return g_snr_emit_hooks.emit_attr_set((unsigned long)attrs, pair);
    }
#endif
    return attr_set(attrs, pair, NULL);
}
#endif

#if defined(DSD_USE_PDCURSES) && !defined(DSD_HAS_PDCURSES_WIDE_API)
#define SNR_USE_UNICODE(option_enabled, block_glyphs_supported)                                                        \
    ((void)(option_enabled), (void)(block_glyphs_supported), 0)
#else
#define SNR_USE_UNICODE(option_enabled, block_glyphs_supported) ((option_enabled) && (block_glyphs_supported))
#endif

#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
static const wchar_t snr_block_glyphs[SNR_METER_BARS][2] = {
    {(wchar_t)0x2581, 0}, {(wchar_t)0x2582, 0}, {(wchar_t)0x2583, 0}, {(wchar_t)0x2584, 0}, {(wchar_t)0x2585, 0},
};
#else
static const char* const snr_block_glyphs[SNR_METER_BARS] = {"▁", "▂", "▃", "▄", "▅"};
#endif

static int
snr_meter_bar_count(double snr_db) {
    if (!isfinite(snr_db) || snr_db <= -50.0) {
        return 0;
    }
    if (snr_db < -6.0) {
        return 1;
    }
    if (snr_db < 3.0) {
        return 2;
    }
    if (snr_db < 12.0) {
        return 3;
    }
    if (snr_db < 21.0) {
        return 4;
    }
    return SNR_METER_BARS;
}

#ifdef PRETTY_COLORS
enum { SNR_METER_COLOR_PAIR = 6 };
#endif

#ifdef DSD_NEO_TEST_HOOKS
static void
snr_meter_ascii(double snr_db, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    size_t n = out_size - 1;
    if (n > SNR_METER_WIDTH) {
        n = SNR_METER_WIDTH;
    }

    int bars = snr_meter_bar_count(snr_db);
    for (size_t i = 0; i < n; i++) {
        out[i] = ' ';
    }
    for (int i = 0; i < bars; i++) {
        size_t pos = (size_t)i * 2;
        if (pos >= n) {
            break;
        }
        out[pos] = '|';
    }
    out[n] = '\0';
}

int
dsd_ncurses_snr_meter_bar_count_for_test(double snr_db) {
    return snr_meter_bar_count(snr_db);
}

void
dsd_ncurses_snr_meter_ascii_for_test(double snr_db, char* out, size_t out_size) {
    snr_meter_ascii(snr_db, out, out_size);
}

int
dsd_ncurses_snr_use_unicode_for_test(int option_enabled, int block_glyphs_supported) {
    return SNR_USE_UNICODE(option_enabled, block_glyphs_supported);
}
#endif

/* Render a compact ascending signal-bar meter for current SNR. */
void
print_snr_meter(const dsd_opts* opts, double snr_db, int mod) {
    (void)mod;
    /* Preserve the current color pair so our temporary colors don't clear it */
#ifdef PRETTY_COLORS
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    snr_emit_attr_get(&saved_attrs, &saved_pair);
#endif
    const int bars = snr_meter_bar_count(snr_db);
    int use_unicode =
        SNR_USE_UNICODE(opts && opts->frontend_terminal_display.eye_unicode, dsd_unicode_block_glyphs_supported());
#ifdef PRETTY_COLORS
    short cp = SNR_METER_COLOR_PAIR;
#endif
    for (int i = 0; i < SNR_METER_BARS; i++) {
        if (i > 0) {
            snr_emit_ch(' ');
        }
        if (i >= bars) {
            snr_emit_ch(' ');
            continue;
        }
#ifdef PRETTY_COLORS
        snr_emit_attron(COLOR_PAIR(cp));
#endif
        if (use_unicode) {
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
            addwstr(snr_block_glyphs[i]);
#else
            snr_emit_str(snr_block_glyphs[i]);
#endif
        } else {
            snr_emit_ch('|');
        }
#ifdef PRETTY_COLORS
        snr_emit_attroff(COLOR_PAIR(cp));
        snr_emit_attr_set(saved_attrs, saved_pair);
#endif
    }
#ifdef PRETTY_COLORS
    /* Padding belongs to the caller's line styling, not the temporary SNR color. */
    snr_emit_attr_set(saved_attrs, saved_pair);
#endif
}
