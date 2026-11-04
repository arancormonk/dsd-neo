// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_UI_TERMINAL_UI_SNR_READOUT_H_
#define DSD_NEO_SRC_UI_TERMINAL_UI_SNR_READOUT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_snr_readout {
    int valid;
    double snr_db;
    const char* mod_label;
} ui_snr_readout;

/* The SNR of @p rf_mod from the frame's snapshot pair (issue #672, B9: no acquisition of its own). */
ui_snr_readout ui_snr_readout_for_mod(const dsd_opts* opts, const dsd_state* state, int rf_mod);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_UI_SNR_READOUT_H_ */
