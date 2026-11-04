// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#ifndef DSD_NEO_TEST_SCAN_MODE_LABEL_STUBS_H
#define DSD_NEO_TEST_SCAN_MODE_LABEL_STUBS_H
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>
#include <stdint.h>

void dsd_test_tg_avoids(size_t count, uint64_t context);
/* The frame's published pair the menu context holds (issue #672, B9): the structs the setters below write and every
   stubbed reader expects. Bound to a default pair until a case binds its own; NULL rebinds the default. */
void dsd_test_scan_labels_frame(dsd_opts* opts, dsd_state* state);
/* The scan class on air (dsd_scan_mode_active(), dsd_scan_mode_row()). */
void dsd_test_scan_labels_set(dsd_scan_mode mode);
void dsd_test_scan_labels_configured(const dsd_scan_settings* settings);
/* The row options dsd_scan_mode_row_options() hands the snapshot (NULL: none installed). */
void dsd_test_scan_labels_row_options(const dsd_scan_option_values* values);
/* The input type the frame's options carries (AUDIO_IN_*; 0, Pulse, until set). */
void dsd_test_scan_labels_input_type(int audio_in_type);
/* The tone policy in force the frame's options carries (dsd_tone_filter_mode and its list; NULL: none). */
void dsd_test_scan_labels_tone_policy(int mode, const dsd_tone_set* set);
/* The scan row on air the frame's pair carries ("this channel", issue #518): @p scanner a
 * dsd_scan_row_scanner (NONE takes it down, and the scanner with it), the session, row, its fields
 * (DSD_SCAN_ROW_FIELD_*) and a trunk target's id. Turns the snapshot options' trunk scan or -Y on for it. */
void dsd_test_scan_labels_scan_row(int scanner, uint32_t session, int row, uint32_t editable, uint32_t listed,
                                   uint32_t edited, const char* target_id);
/* The rigctl peer the frame's options carries (-U, issue #621): connected (a live socket) or not, and -B,
 * the FM passband in Hz that stands in for an unset NFM width. */
void dsd_test_scan_labels_rigctl(int connected, int setmod_bw_hz);
/* The analog preset the frame's options carries (-fA, and its dsd_analog_demod). */
void dsd_test_scan_labels_analog(int analog_only, int analog_demod);
/* The effective tuner gain the frame's options carries. */
void dsd_test_scan_labels_rtl_gain(int gain);
/* The configured squelch the frame's options carries (dsd_squelch_mode, margin, level). */
void dsd_test_scan_labels_squelch(int mode, int margin_db, double level);
/* The row scope stamps the frame's options and state carry (dsd_opts/dsd_state::scan_row_scope_seq):
 * equal unless a case reads the pair between the decoder's two publishes. */
void dsd_test_scan_labels_scope_seq(uint32_t opts_seq, uint32_t state_seq);

#endif
