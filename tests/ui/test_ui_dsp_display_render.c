// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
// NOLINTBEGIN(bugprone-suspicious-include)

/*
 * The DSP status panel as drawn (issue #669): every line is one screen row, cut between whole fields instead of
 * wrapped, its readings at fixed widths so the fields after them hold their columns, and a reading past its field
 * saturates instead of widening it. Built with USE_RADIO, as every radio build compiles the panel.
 */

#include <assert.h>
#include <curses.h> // IWYU pragma: keep (read before the getmaxyx override below)
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/number_text.h>
#include <dsd-neo/runtime/unicode.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/ui/ui_prims.h"

static char g_out[8192];
static size_t g_out_len;
static int g_cols = 120;

static void
out_reset(void) {
    g_out_len = 0U;
    g_out[0] = '\0';
}

static int test_printw(const char* fmt, ...) DSD_ATTR_FORMAT(printf, 1, 2);

static int
test_printw(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = DSD_VSNPRINTF(g_out + g_out_len, sizeof g_out - g_out_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        g_out_len += (size_t)n;
        if (g_out_len >= sizeof g_out) {
            g_out_len = sizeof g_out - 1U;
        }
    }
    return 0;
}

#undef getmaxyx
#define getmaxyx(win, y, x)                                                                                            \
    do {                                                                                                               \
        (void)(win);                                                                                                   \
        (y) = 40;                                                                                                      \
        (x) = g_cols;                                                                                                  \
    } while (0)
#define printw test_printw

#include "../../src/ui/terminal/ncurses_dsp_display.c"
#include "../../src/ui/terminal/ui_row.c"

#undef printw
#undef getmaxyx

/* ---- what the panel reads ---- */

static dsd_frontend_metrics g_metrics;
static int g_view_dynamic;
static int g_view_gate_open;

int
dsd_app_frontend_get_metrics(dsd_frontend_metrics* out) {
    *out = g_metrics;
    return 0;
}

int
dsd_app_squelch_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_squelch_view* out) {
    (void)opts;
    (void)state;
    DSD_MEMSET(out, 0, sizeof(*out));
    out->effective_noise = (uint8_t)g_view_dynamic;
    out->noise_gate_open = (uint8_t)g_view_gate_open;
    return 0;
}

int
dsd_app_squelch_view_format_ex(const dsd_app_squelch_view* view, unsigned flags, char* out, size_t out_size) {
    (void)view;
    (void)flags;
    DSD_SNPRINTF(out, out_size, "%s", "noise +12 dB (quieting   23 dB)             ");
    return 0;
}

double
pwr_to_dB(double mean_power) {
    if (mean_power <= 0.0) {
        return -120.0;
    }
    const double db = 10.0 * log10(mean_power);
    return db > 0.0 ? 0.0 : (db < -120.0 ? -120.0 : db);
}

int
dsd_squelch_format(double mean_power, const char* unit, char* out, size_t out_size) {
    if (dsd_squelch_is_off(mean_power)) {
        DSD_SNPRINTF(out, out_size, "%s", "off");
        return 0;
    }
    char db[32];
    (void)dsd_number_text(db, sizeof db, pwr_to_dB(mean_power), 0, 1, 0);
    DSD_SNPRINTF(out, out_size, "%s%s", db, unit ? unit : "");
    return 0;
}

const char*
dsd_unicode_or_ascii(const char* unicode_str, const char* ascii_str) {
    (void)unicode_str;
    return ascii_str;
}

void
ui_print_lborder(void) {
    test_printw("|");
}

void
ui_print_header(const char* title) {
    test_printw("--%s--\n", title);
}

void
ui_print_hr(void) {
    test_printw("-----\n");
}

/* ---- checks ---- */

/* The text of the line whose label is @p label, without its newline. */
static void
line_of(const char* label, char* out, size_t out_size) {
    char needle[32];
    DSD_SNPRINTF(needle, sizeof needle, "| %s:", label);
    const char* at = strstr(g_out, needle);
    assert(at != NULL);
    const char* end = strchr(at, '\n');
    assert(end != NULL);
    DSD_SNPRINTF(out, out_size, "%.*s", (int)(end - at), at);
}

static int
column_in_line(const char* label, const char* field) {
    char line[256];
    line_of(label, line, sizeof line);
    const char* at = strstr(line, field);
    return at ? (int)(at - line) : -1;
}

static void
draw(dsd_opts* opts, dsd_state* state) {
    out_reset();
    print_dsp_status(opts, state);
}

static void
seed_metrics(void) {
    DSD_MEMSET(&g_metrics, 0, sizeof g_metrics);
    g_metrics.cqpsk_enable = 1;
    g_metrics.cqpsk_timing_active = 1;
    g_metrics.ted_sps = 10;
    g_metrics.ted_gain = 0.025f;
    g_metrics.cqpsk_timing_bias = -12;
    g_metrics.fll_band_edge_freq_hz = -0.04;
    g_metrics.cfo_hz = 12.5;
    g_metrics.carrier_lock = 1;
    g_metrics.costas_err_q14 = 120;
    g_metrics.costas.err_raw_avg_q14 = 340;
    g_metrics.costas.confidence_avg_q14 = 8192;
    g_metrics.costas.zero_conf_pct = 3;
    g_metrics.nco_q15 = -1500;
    g_metrics.demod_rate_hz = 48000;
}

static void
test_lines(dsd_opts* opts, dsd_state* state) {
    seed_metrics();
    draw(opts, state);
    char line[256];
    line_of("Front", line, sizeof line);
    assert(strcmp(line, "| Front:        IQBal:Off  IQ-DC:Off k: 0") == 0);
    line_of("Path", line, sizeof line);
    assert(strcmp(line, "| Path:         Mod:CQPSK  CQ:On") == 0);
    line_of("CQPSK Timing", line, sizeof line);
    assert(strcmp(line, "| CQPSK Timing: [On ] sps:10 g:0.025 bias:   -12") == 0);
    line_of("FLL BE", line, sizeof line);
    assert(strcmp(line, "| FLL BE:       Freq=    +0.0 Hz") == 0); /* -0.04 Hz rounds to zero: no "-0.0" */
    line_of("Carrier", line, sizeof line);
    assert(strcmp(line, "| Carrier:      NCO=   +12.5 Hz  Locked") == 0);
    line_of("Costas/NCO", line, sizeof line);
    assert(strcmp(line, "| Costas/NCO:   ErrS=   120 ErrR=   340 Conf=0.50 Fade=  3% NCO(q15)= -1500 Fs= 48000 Hz")
           == 0);
}

static void
test_columns_hold(dsd_opts* opts, dsd_state* state) {
    seed_metrics();
    draw(opts, state);
    const int lock_col = column_in_line("Carrier", "  Locked");
    const int errr_col = column_in_line("Costas/NCO", " ErrR=");
    const int fs_col = column_in_line("Costas/NCO", " Fs=");
    const int bias_col = column_in_line("CQPSK Timing", " bias:");
    const double cfo[] = {0.0, -0.04, 9.9, -123.4, 11459.2, -11459.2, 1.0e6, -1.0e6};
    const int ints[] = {0, -1, 99999, -99999, 1000000, INT_MAX, INT_MIN};
    for (size_t i = 0; i < sizeof cfo / sizeof cfo[0]; i++) {
        g_metrics.cfo_hz = cfo[i];
        g_metrics.carrier_lock = (int)(i % 2U);
        g_metrics.costas_err_q14 = ints[i % 7U];
        g_metrics.costas.zero_conf_pct = (int)(i * 15U);
        g_metrics.nco_q15 = ints[(i + 3U) % 7U];
        g_metrics.ted_sps = (int)(i * 3U);
        g_metrics.ted_gain = (float)(0.01 + 0.07 * (double)i);
        g_metrics.cqpsk_timing_bias = ints[(i + 1U) % 7U];
        draw(opts, state);
        assert(column_in_line("Carrier", g_metrics.carrier_lock ? "  Locked" : "  Acq") == lock_col);
        assert(column_in_line("Costas/NCO", " ErrR=") == errr_col);
        assert(column_in_line("Costas/NCO", " Fs=") == fs_col);
        assert(column_in_line("CQPSK Timing", " bias:") == bias_col);
    }
    /* Past their fields, readings saturate. */
    g_metrics.cfo_hz = -1.0e6;
    g_metrics.costas_err_q14 = INT_MIN;
    g_metrics.cqpsk_timing_bias = INT_MAX;
    g_metrics.ted_gain = 10.0f;
    draw(opts, state);
    char line[256];
    line_of("Carrier", line, sizeof line);
    assert(strstr(line, "NCO=<-9999.9 Hz") != NULL);
    line_of("Costas/NCO", line, sizeof line);
    assert(strstr(line, "ErrS=<-9999") != NULL);
    line_of("CQPSK Timing", line, sizeof line);
    /* A gain no setter allows (0.01..0.5) has no number that fits five places beside the marker: the marker fills them. */
    assert(strstr(line, " g:>>>>> ") != NULL && strstr(line, " bias:>+9999") != NULL);
}

/* A narrow panel cuts between whole fields: "Fs= 48000 Hz" is shown whole or not at all, and every line stays one
   row. */
static void
test_cut_between_fields(dsd_opts* opts, dsd_state* state) {
    seed_metrics();
    for (int cols = 40; cols <= 100; cols++) {
        g_cols = cols;
        draw(opts, state);
        for (const char* line = g_out; *line != '\0';) {
            const char* end = strchr(line, '\n');
            assert(end != NULL);
            assert((int)(end - line) <= cols - 1); /* never the last column */
            line = end + 1;
        }
        const char* fs = strstr(g_out, "Fs=");
        if (fs) {
            assert(strncmp(fs, "Fs= 48000 Hz", 12) == 0);
        }
        const char* conf = strstr(g_out, "Conf=");
        if (conf) {
            assert(strncmp(conf, "Conf=0.50", 9) == 0);
        }
    }
    g_cols = 82;
    draw(opts, state);
    char line[256];
    line_of("Costas/NCO", line, sizeof line);
    assert(strcmp(line, "| Costas/NCO:   ErrS=   120 ErrR=   340 Conf=0.50 Fade=  3% NCO(q15)= -1500...") == 0);
    g_cols = 120;
}

static void
test_squelch_lines(dsd_opts* opts, dsd_state* state) {
    seed_metrics();
    /* The level path: the shared formatter's fixed text. */
    g_view_dynamic = 0;
    opts->rtl_pwr = 0.995; /* -0.02 dB */
    opts->rtl_squelch_level = 0.0;
    draw(opts, state);
    char line[256];
    line_of("Squelch", line, sizeof line);
    assert(strcmp(line, "| Squelch:      Open   ch:   0.0 dB sql:off") == 0);
    /* The dynamic path: the gate in six places, the channel power in six, then the view's text (its padding trimmed at
       the end of the row). */
    g_view_dynamic = 1;
    g_view_gate_open = 0;
    opts->rtl_pwr = pow(10.0, -7.0);
    draw(opts, state);
    line_of("Squelch", line, sizeof line);
    assert(strcmp(line, "| Squelch:      Closed ch: -70.0 dB sql:noise +12 dB (quieting   23 dB)") == 0);
    g_view_gate_open = 1;
    opts->rtl_pwr = pow(10.0, -0.5);
    draw(opts, state);
    line_of("Squelch", line, sizeof line);
    assert(strcmp(line, "| Squelch:      Open   ch:  -5.0 dB sql:noise +12 dB (quieting   23 dB)") == 0);
    g_view_dynamic = 0;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    state->rf_mod = 1;
    test_lines(opts, state);
    test_columns_hold(opts, state);
    test_cut_between_fields(opts, state);
    test_squelch_lines(opts, state);
    free(state);
    free(opts);
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
