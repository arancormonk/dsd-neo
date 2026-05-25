// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_snr.c
 * SNR history, sparkline, and meter rendering
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/ui/ncurses_internal.h>
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

/* SNR history buffers for sparkline (per modulation) */
enum { SNR_HIST_N = 48 };

static double snr_hist_c4fm[SNR_HIST_N];
static int snr_hist_len_c4fm = 0;
static int snr_hist_head_c4fm = 0;
static double snr_hist_qpsk[SNR_HIST_N];
static int snr_hist_len_qpsk = 0;
static int snr_hist_head_qpsk = 0;
static double snr_hist_gfsk[SNR_HIST_N];
static int snr_hist_len_gfsk = 0;
static int snr_hist_head_gfsk = 0;

enum { SNR_METER_BARS = 5, SNR_METER_WIDTH = (SNR_METER_BARS * 2) - 1 };

enum { SNR_BLOCK_LEVELS = 8 };

#if defined(DSD_USE_PDCURSES) && !defined(DSD_HAS_PDCURSES_WIDE_API)
#define SNR_USE_UNICODE(option_enabled, unicode_supported) ((void)(option_enabled), (void)(unicode_supported), 0)
#else
#define SNR_USE_UNICODE(option_enabled, unicode_supported) ((option_enabled) && (unicode_supported))
#endif

#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
static const wchar_t snr_block_glyphs[SNR_BLOCK_LEVELS][2] = {
    {(wchar_t)0x2581, 0}, {(wchar_t)0x2582, 0}, {(wchar_t)0x2583, 0}, {(wchar_t)0x2584, 0},
    {(wchar_t)0x2585, 0}, {(wchar_t)0x2586, 0}, {(wchar_t)0x2587, 0}, {(wchar_t)0x2588, 0},
};
#else
static const char* const snr_block_glyphs[SNR_BLOCK_LEVELS] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
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
static short
snr_quality_color_pair(double snr_db, int mod) {
    const short C_GOOD = 11, C_MOD = 12, C_POOR = 13;
    double thr1 = 12.0, thr2 = 18.0; /* fallback */
    if (mod == 0) {                  /* C4FM */
        thr1 = 4.0;
        thr2 = 10.0;
    } else if (mod == 1 || mod == 2) { /* QPSK or GFSK */
        thr1 = 10.0;
        thr2 = 16.0;
    }
    return (snr_db < thr1) ? C_POOR : (snr_db < thr2) ? C_MOD : C_GOOD;
}
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
dsd_ncurses_snr_use_unicode_for_test(int option_enabled, int unicode_supported) {
    return SNR_USE_UNICODE(option_enabled, unicode_supported);
}
#endif

void
snr_hist_push(int mod, double snr) {
    if (snr < -50.0) {
        return;
    }
    if (snr > 60.0) {
        snr = 60.0;
    }
    int* head = NULL;
    int* len = NULL;
    double* buf = NULL;
    if (mod == 0) {
        head = &snr_hist_head_c4fm;
        len = &snr_hist_len_c4fm;
        buf = snr_hist_c4fm;
    } else if (mod == 1) {
        head = &snr_hist_head_qpsk;
        len = &snr_hist_len_qpsk;
        buf = snr_hist_qpsk;
    } else {
        head = &snr_hist_head_gfsk;
        len = &snr_hist_len_gfsk;
        buf = snr_hist_gfsk;
    }
    int h = *head;
    buf[h] = snr;
    h = (h + 1) % SNR_HIST_N;
    *head = h;
    if (*len < SNR_HIST_N) {
        (*len)++;
    }
}

struct snr_hist_view {
    const double* buf;
    int len;
    int head;
};

static struct snr_hist_view
snr_hist_view_for_mod(int mod) {
    struct snr_hist_view view = {snr_hist_gfsk, snr_hist_len_gfsk, snr_hist_head_gfsk};
    if (mod == 0) {
        view.buf = snr_hist_c4fm;
        view.len = snr_hist_len_c4fm;
        view.head = snr_hist_head_c4fm;
    } else if (mod == 1) {
        view.buf = snr_hist_qpsk;
        view.len = snr_hist_len_qpsk;
        view.head = snr_hist_head_qpsk;
    }
    return view;
}

static int
snr_hist_render_count(int len, int width) {
    return len < width ? len : width;
}

static int
snr_hist_render_start(int head, int len, int count) {
    int start = (head - len + SNR_HIST_N) % SNR_HIST_N;
    return (start + (len - count)) % SNR_HIST_N;
}

static int
snr_hist_level_index(double value, double clip_lo, double span, int levels) {
    double t = (value - clip_lo) / span;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    int li = (int)floor(t * (levels - 1) + 0.5);
    if (li < 0) {
        li = 0;
    } else if (li >= levels) {
        li = levels - 1;
    }
    return li;
}

static void
snr_draw_level_glyph(double sample, int mod, int use_unicode, const char* ascii8, int li) {
#ifdef PRETTY_COLORS
    short cp = snr_quality_color_pair(sample, mod);
    attron(COLOR_PAIR(cp));
#else
    (void)sample;
    (void)mod;
#endif
    if (use_unicode) {
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
        addwstr(snr_block_glyphs[li]);
#else
        addstr(snr_block_glyphs[li]);
#endif
    } else {
        addch(ascii8[li]);
    }
#ifdef PRETTY_COLORS
    attroff(COLOR_PAIR(cp));
#endif
}

void
print_snr_sparkline(const dsd_opts* opts, int mod) {
    /* Preserve the current color pair so our temporary colors don't clear it */
#ifdef PRETTY_COLORS
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    attr_get(&saved_attrs, &saved_pair, NULL);
#endif
    /* Make the lowest ASCII level visible (no leading space) */
    static const char ascii8[] = ".:;-=+*#"; /* 8 levels */
    /* Respect the UI toggle: only use Unicode blocks when enabled and locale supports it */
    int use_unicode = SNR_USE_UNICODE(opts && opts->eye_unicode, ui_unicode_supported());
    const int levels = SNR_BLOCK_LEVELS;
    const int W = 24;                             /* sparkline width */
    const double clip_lo = -15.0, clip_hi = 30.0; /* dB window (allow negatives) */
    const double span = clip_hi - clip_lo;

    struct snr_hist_view view = snr_hist_view_for_mod(mod);
    if (view.len <= 0) {
        return;
    }
    int count = snr_hist_render_count(view.len, W);
    /* Map most recent to the right; older to the left */
    int idx = snr_hist_render_start(view.head, view.len, count);

    for (int x = 0; x < count; x++) {
        double v = view.buf[idx];
        idx = (idx + 1) % SNR_HIST_N;
        int li = snr_hist_level_index(v, clip_lo, span, levels);
        snr_draw_level_glyph(v, mod, use_unicode, ascii8, li);
    }
#ifdef PRETTY_COLORS
    /* Restore previously active color pair (e.g., green call banner) */
    attr_set(saved_attrs, saved_pair, NULL);
#endif
}

/* Render a compact ascending signal-bar meter for current SNR. */
void
print_snr_meter(const dsd_opts* opts, double snr_db, int mod) {
    /* Preserve the current color pair so our temporary colors don't clear it */
#ifdef PRETTY_COLORS
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    attr_get(&saved_attrs, &saved_pair, NULL);
#endif
    const int bars = snr_meter_bar_count(snr_db);
    int use_unicode = SNR_USE_UNICODE(opts && opts->eye_unicode, ui_unicode_supported());
#ifdef PRETTY_COLORS
    short cp = snr_quality_color_pair(snr_db, mod);
#else
    (void)mod;
#endif
    for (int i = 0; i < SNR_METER_BARS; i++) {
        if (i > 0) {
            addch(' ');
        }
        if (i >= bars) {
            addch(' ');
            continue;
        }
#ifdef PRETTY_COLORS
        attron(COLOR_PAIR(cp));
#endif
        if (use_unicode) {
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
            addwstr(snr_block_glyphs[i]);
#else
            addstr(snr_block_glyphs[i]);
#endif
        } else {
            addch('|');
        }
#ifdef PRETTY_COLORS
        attroff(COLOR_PAIR(cp));
        attr_set(saved_attrs, saved_pair, NULL);
#endif
    }
#ifdef PRETTY_COLORS
    /* Padding belongs to the caller's line styling, not the temporary SNR color. */
    attr_set(saved_attrs, saved_pair, NULL);
#endif
}
