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
enum { SNR_METER_COLOR_PAIR = 6 };

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
dsd_ncurses_snr_use_unicode_for_test(int option_enabled, int block_glyphs_supported) {
    return SNR_USE_UNICODE(option_enabled, block_glyphs_supported);
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

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_ncurses_snr_hist_reset_for_test(void) {
    for (int i = 0; i < SNR_HIST_N; i++) {
        snr_hist_c4fm[i] = 0.0;
        snr_hist_qpsk[i] = 0.0;
        snr_hist_gfsk[i] = 0.0;
    }
    snr_hist_len_c4fm = 0;
    snr_hist_head_c4fm = 0;
    snr_hist_len_qpsk = 0;
    snr_hist_head_qpsk = 0;
    snr_hist_len_gfsk = 0;
    snr_hist_head_gfsk = 0;
}

int
dsd_ncurses_snr_hist_len_for_test(int mod) {
    return snr_hist_view_for_mod(mod).len;
}

int
dsd_ncurses_snr_hist_head_for_test(int mod) {
    return snr_hist_view_for_mod(mod).head;
}

double
dsd_ncurses_snr_hist_value_for_test(int mod, int index) {
    if (index < 0 || index >= SNR_HIST_N) {
        return 0.0;
    }
    return snr_hist_view_for_mod(mod).buf[index];
}

int
dsd_ncurses_snr_hist_render_count_for_test(int len, int width) {
    return snr_hist_render_count(len, width);
}

int
dsd_ncurses_snr_hist_render_start_for_test(int head, int len, int count) {
    return snr_hist_render_start(head, len, count);
}

int
dsd_ncurses_snr_level_index_for_test(double value, double clip_lo, double span, int levels) {
    return snr_hist_level_index(value, clip_lo, span, levels);
}
#endif

static void
snr_draw_level_glyph(double sample, int mod, int use_unicode, const char* ascii8, int li) {
#ifdef PRETTY_COLORS
    short cp = snr_quality_color_pair(sample, mod);
    snr_emit_attron(COLOR_PAIR(cp));
#else
    (void)sample;
    (void)mod;
#endif
    if (use_unicode) {
#if defined(DSD_USE_PDCURSES) && defined(DSD_HAS_PDCURSES_WIDE_API)
        addwstr(snr_block_glyphs[li]);
#else
        snr_emit_str(snr_block_glyphs[li]);
#endif
    } else {
        snr_emit_ch(ascii8[li]);
    }
#ifdef PRETTY_COLORS
    snr_emit_attroff(COLOR_PAIR(cp));
#endif
}

void
print_snr_sparkline(const dsd_opts* opts, int mod) {
    /* Preserve the current color pair so our temporary colors don't clear it */
#ifdef PRETTY_COLORS
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    snr_emit_attr_get(&saved_attrs, &saved_pair);
#endif
    /* Make the lowest ASCII level visible (no leading space) */
    static const char ascii8[] = ".:;-=+*#"; /* 8 levels */
    /* Respect the UI toggle and require renderable block glyphs, not just UTF-8 bytes. */
    int use_unicode = SNR_USE_UNICODE(opts && opts->frontend_display.eye_unicode, ui_block_glyphs_supported());
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
    snr_emit_attr_set(saved_attrs, saved_pair);
#endif
}

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
    int use_unicode = SNR_USE_UNICODE(opts && opts->frontend_display.eye_unicode, ui_block_glyphs_supported());
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
