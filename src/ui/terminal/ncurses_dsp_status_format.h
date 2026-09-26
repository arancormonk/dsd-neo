// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_UI_TERMINAL_NCURSES_DSP_STATUS_FORMAT_H_
#define DSD_NEO_SRC_UI_TERMINAL_NCURSES_DSP_STATUS_FORMAT_H_

#include <stddef.h>

/* "Closed ch:-70.0 dB sql:-60.0 dB": the gate, the channel power and the threshold in force.
 * @p row_override appends " (row)" while a scan row or target sets that threshold (issue #521),
 * as the RTL input line's SQL readout says it. */
int ui_dsp_format_squelch_status(double channel_power, double squelch_power, int row_override, char* out,
                                 size_t out_size);

#endif /* DSD_NEO_SRC_UI_TERMINAL_NCURSES_DSP_STATUS_FORMAT_H_ */
