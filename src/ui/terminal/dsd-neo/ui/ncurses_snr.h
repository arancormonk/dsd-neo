// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file ncurses_snr.h
 * @brief SNR display API for ncurses UI.
 *
 * Provides compact SNR meter rendering for different modulation types.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_SNR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_SNR_H_

#include <dsd-neo/core/opts_fwd.h>

#ifdef DSD_NEO_TEST_HOOKS
#include <stddef.h>

typedef int (*dsd_ncurses_snr_emit_ch_fn)(unsigned long ch);
typedef int (*dsd_ncurses_snr_emit_str_fn)(const char* text);
typedef int (*dsd_ncurses_snr_emit_attr_fn)(unsigned long attrs);
typedef int (*dsd_ncurses_snr_emit_attr_get_fn)(unsigned long* attrs, short* pair);
typedef int (*dsd_ncurses_snr_emit_attr_set_fn)(unsigned long attrs, short pair);
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Render a compact SNR meter. */
void print_snr_meter(const dsd_opts* opts, double snr_db, int mod);

#ifdef DSD_NEO_TEST_HOOKS
int dsd_ncurses_snr_meter_bar_count_for_test(double snr_db);
void dsd_ncurses_snr_meter_ascii_for_test(double snr_db, char* out, size_t out_size);
int dsd_ncurses_snr_use_unicode_for_test(int option_enabled, int block_glyphs_supported);
void dsd_ncurses_snr_set_emit_hooks_for_test(dsd_ncurses_snr_emit_ch_fn emit_ch, dsd_ncurses_snr_emit_str_fn emit_str,
                                             dsd_ncurses_snr_emit_attr_fn emit_attron,
                                             dsd_ncurses_snr_emit_attr_fn emit_attroff,
                                             dsd_ncurses_snr_emit_attr_get_fn emit_attr_get,
                                             dsd_ncurses_snr_emit_attr_set_fn emit_attr_set);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_NCURSES_SNR_H_ */
