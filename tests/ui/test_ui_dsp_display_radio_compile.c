// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ncurses_dsp_status_format.h"

#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>

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

int
main(void) {
    return 0;
}
