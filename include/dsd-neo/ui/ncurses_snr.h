// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file ncurses_snr.h
 * @brief SNR display API for ncurses UI.
 *
 * Provides SNR history tracking, sparkline visualization, and compact meter
 * rendering for different modulation types.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_SNR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_SNR_H_

#include <dsd-neo/core/opts_fwd.h>

#ifdef DSD_NEO_TEST_HOOKS
#include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Push a new SNR sample to the history ring for the given modulation (0=C4FM, 1=QPSK, 2=GFSK). */
void snr_hist_push(int mod, double snr);

/** Render a sparkline showing recent SNR history. */
void print_snr_sparkline(const dsd_opts* opts, int mod);

/** Render a compact SNR meter. */
void print_snr_meter(const dsd_opts* opts, double snr_db, int mod);

#ifdef DSD_NEO_TEST_HOOKS
int dsd_ncurses_snr_meter_bar_count_for_test(double snr_db);
void dsd_ncurses_snr_meter_ascii_for_test(double snr_db, char* out, size_t out_size);
int dsd_ncurses_snr_use_unicode_for_test(int option_enabled, int block_glyphs_supported);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_SNR_H_ */
