// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_snr.c
 * SNR history, sparkline, and meter rendering
 */

#include <curses.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/ui/ncurses_internal.h>
#include <dsd-neo/ui/ncurses_snr.h>
#include <math.h>
#include <stddef.h>

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

enum { SNR_METER_BARS = 5 };

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

#ifdef DSD_NEO_TEST_HOOKS
static void
snr_meter_ascii(double snr_db, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    size_t n = out_size - 1;
    if (n > SNR_METER_BARS) {
        n = SNR_METER_BARS;
    }

    int bars = snr_meter_bar_count(snr_db);
    for (size_t i = 0; i < n; i++) {
        out[i] = ((int)i < bars) ? '|' : ' ';
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

void
print_snr_sparkline(const dsd_opts* opts, int mod) {
    /* Preserve the current color pair so our temporary colors don't clear it */
#ifdef PRETTY_COLORS
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    attr_get(&saved_attrs, &saved_pair, NULL);
#endif
    static const char* uni8[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    /* Make the lowest ASCII level visible (no leading space) */
    static const char ascii8[] = ".:;-=+*#"; /* 8 levels */
    /* Respect the UI toggle: only use Unicode blocks when enabled and locale supports it */
    int use_unicode = (opts && opts->eye_unicode && ui_unicode_supported());
    const int levels = 8;
    const int W = 24;                             /* sparkline width */
    const double clip_lo = -15.0, clip_hi = 30.0; /* dB window (allow negatives) */
    const double span = (clip_hi - clip_lo) > 1e-6 ? (clip_hi - clip_lo) : 1.0;

    const double* buf = NULL;
    int len = 0;
    int head = 0;
    if (mod == 0) {
        buf = snr_hist_c4fm;
        len = snr_hist_len_c4fm;
        head = snr_hist_head_c4fm;
    } else if (mod == 1) {
        buf = snr_hist_qpsk;
        len = snr_hist_len_qpsk;
        head = snr_hist_head_qpsk;
    } else {
        buf = snr_hist_gfsk;
        len = snr_hist_len_gfsk;
        head = snr_hist_head_gfsk;
    }
    if (len <= 0) {
        return;
    }
    int start = (head - len + SNR_HIST_N) % SNR_HIST_N;
    int count = len < W ? len : W;
    /* Map most recent to the right; older to the left */
    int idx = (start + (len - count)) % SNR_HIST_N;

    /* Color bands (per modulation, unbiased SNR):
       - C4FM: poor<4, 4..10 moderate, >10 good
       - QPSK/GFSK: poor<10, 10..16 moderate, >16 good */
    const short C_GOOD = 11, C_MOD = 12, C_POOR = 13;
    double thr1 = 12.0, thr2 = 18.0; /* fallback */
    if (mod == 0) {                  /* C4FM */
        thr1 = 4.0;
        thr2 = 10.0;
    } else if (mod == 1 || mod == 2) { /* QPSK or GFSK */
        thr1 = 10.0;
        thr2 = 16.0;
    }
    for (int x = 0; x < count; x++) {
        double v = buf[idx];
        idx = (idx + 1) % SNR_HIST_N;
        double t = (v - clip_lo) / span;
        if (t < 0.0) {
            t = 0.0;
        }
        if (t > 1.0) {
            t = 1.0;
        }
        int li = (int)floor(t * (levels - 1) + 0.5);
        if (li < 0) {
            li = 0;
        }
        if (li >= levels) {
            li = levels - 1;
        }
        short cp = (v < thr1) ? C_POOR : (v < thr2) ? C_MOD : C_GOOD;
#ifdef PRETTY_COLORS
        attron(COLOR_PAIR(cp));
#endif
        if (use_unicode) {
            addstr(uni8[li]);
        } else {
            addch(ascii8[li]);
        }
#ifdef PRETTY_COLORS
        attroff(COLOR_PAIR(cp));
#endif
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
    static const char* uni_bars[SNR_METER_BARS] = {"▁", "▂", "▃", "▄", "▅"};
    const int bars = snr_meter_bar_count(snr_db);
    int use_unicode = (opts && opts->eye_unicode && ui_unicode_supported());
    short cp = snr_quality_color_pair(snr_db, mod);
#ifdef PRETTY_COLORS
    if (bars > 0) {
        attron(COLOR_PAIR(cp));
    }
#endif
    for (int i = 0; i < SNR_METER_BARS; i++) {
        if (i >= bars) {
            break;
        }
        if (use_unicode) {
            addstr(uni_bars[i]);
        } else {
            addch('|');
        }
    }
#ifdef PRETTY_COLORS
    if (bars > 0) {
        attroff(COLOR_PAIR(cp));
    }
    /* Padding belongs to the caller's line styling, not the temporary SNR color. */
    attr_set(saved_attrs, saved_pair, NULL);
#endif
    for (int i = bars; i < SNR_METER_BARS; i++) {
        addch(' ');
    }
}
