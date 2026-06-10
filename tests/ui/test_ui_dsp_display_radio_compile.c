// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ncurses_dsp_status_format.h"

#include <stddef.h>

#ifdef USE_RTLSDR
#error "This regression guard must compile the DSP panel with USE_RADIO only."
#endif

#ifndef USE_RADIO
#error "This regression guard must compile the DSP panel with USE_RADIO."
#endif

int
ui_dsp_format_squelch_status(double channel_power, double squelch_power, char* out, size_t out_size) {
    (void)channel_power;
    (void)squelch_power;
    (void)out;
    (void)out_size;
    return -1;
}

int
main(void) {
    return 0;
}
