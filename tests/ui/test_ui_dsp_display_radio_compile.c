// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The DSP panel in a radio build without librtlsdr (SoapySDR or Airspy only):
 * it must compile, and it must still read the front-end metrics for its
 * Front/Path/CQPSK lines.
 */

#include "ncurses_dsp_status_format.h"

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/unicode.h>
#include <dsd-neo/ui/ncurses_dsp_display.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef USE_RTLSDR
#error "This regression guard must compile the DSP panel with USE_RADIO only."
#endif

#ifndef USE_RADIO
#error "This regression guard must compile the DSP panel with USE_RADIO."
#endif

int
ui_dsp_format_squelch_status(double channel_power, double squelch_power, int row_override, char* out, size_t out_size) {
    (void)channel_power;
    (void)squelch_power;
    (void)row_override;
    (void)out;
    (void)out_size;
    return -1;
}

int
dsd_app_squelch_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_squelch_view* out) {
    (void)opts;
    (void)state;
    (void)out;
    return -1;
}

double
pwr_to_dB(double mean_power) {
    (void)mean_power;
    return -120.0;
}

int
dsd_app_squelch_view_format(const dsd_app_squelch_view* view, char* out, size_t out_size) {
    (void)view;
    (void)out;
    (void)out_size;
    return -1;
}

int
dsd_app_squelch_view_format_ex(const dsd_app_squelch_view* view, unsigned flags, char* out, size_t out_size) {
    (void)view;
    (void)flags;
    (void)out;
    (void)out_size;
    return -1;
}

const char*
dsd_unicode_or_ascii(const char* unicode_str, const char* ascii_str) {
    (void)unicode_str;
    return ascii_str;
}

static int g_metrics_reads;

int
dsd_app_frontend_get_metrics(dsd_frontend_metrics* out) {
    g_metrics_reads++;
    DSD_MEMSET(out, 0, sizeof(*out));
    return 0;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(state);
        free(opts);
        return 1;
    }
    /* Curses is never initialised here: every draw call meets a null stdscr and draws nothing. */
    print_dsp_status(opts, state);
    free(state);
    free(opts);
    if (g_metrics_reads != 1) {
        DSD_FPRINTF(stderr, "DSP panel read the front-end metrics %d times, want 1\n", g_metrics_reads);
        return 1;
    }
    return 0;
}
