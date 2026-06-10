// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_UI_TERMINAL_NCURSES_DSP_STATUS_FORMAT_H_
#define DSD_NEO_SRC_UI_TERMINAL_NCURSES_DSP_STATUS_FORMAT_H_

#include <stddef.h>

int ui_dsp_format_squelch_status(double channel_power, double squelch_power, char* out, size_t out_size);

#endif /* DSD_NEO_SRC_UI_TERMINAL_NCURSES_DSP_STATUS_FORMAT_H_ */
