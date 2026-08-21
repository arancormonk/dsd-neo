// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Menu item array declarations, one per root entry.
 *
 * The root itself is composed in menus/menu_defs.c (ui_menu_get_main_items()),
 * in the order below: the receiver's signal chain, then housekeeping.
 *
 * This header is internal to src/ui/terminal/ and should NOT be installed.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_MENU_ITEMS_H_
#define DSD_NEO_SRC_UI_TERMINAL_MENU_ITEMS_H_

#include <dsd-neo/ui/menu_core.h>
#include <stddef.h>

// ---- Input: source, input level, RTL-SDR tuning ----
extern const NcMenuItem INPUT_MENU_ITEMS[];
extern const size_t INPUT_MENU_ITEMS_LEN;

// ---- Decoder: protocol mode, modulation, filters, DMR/TDMA ----
extern const NcMenuItem DECODER_MENU_ITEMS[];
extern const size_t DECODER_MENU_ITEMS_LEN;

// ---- Trunking: on/off, follow rules, imports, P25, rig control ----
extern const NcMenuItem TRUNK_MENU_ITEMS[];
extern const size_t TRUNK_MENU_ITEMS_LEN;

// ---- Encryption: keys, key import, keystreams, encrypted-call policy ----
extern const NcMenuItem ENC_MENU_ITEMS[];
extern const size_t ENC_MENU_ITEMS_LEN;

// ---- Audio: output sink, mute, gains, tone shaping, call alerts ----
extern const NcMenuItem AUDIO_MENU_ITEMS[];
extern const size_t AUDIO_MENU_ITEMS_LEN;

// ---- Recording & logs: symbol capture, WAV, event log, LRRP, DSP dumps ----
extern const NcMenuItem REC_MENU_ITEMS[];
extern const size_t REC_MENU_ITEMS_LEN;

// ---- Display: compact view, sections, visualizers, event history ----
extern const NcMenuItem DISPLAY_MENU_ITEMS[];
extern const size_t DISPLAY_MENU_ITEMS_LEN;

// ---- Config: load/save settings and profiles ----
extern const NcMenuItem CONFIG_MENU_ITEMS[];
extern const size_t CONFIG_MENU_ITEMS_LEN;

// ---- Advanced: scheduling, threads, diagnostics, environment ----
extern const NcMenuItem ADV_MENU_ITEMS[];
extern const size_t ADV_MENU_ITEMS_LEN;

#endif /* DSD_NEO_SRC_UI_TERMINAL_MENU_ITEMS_H_ */
