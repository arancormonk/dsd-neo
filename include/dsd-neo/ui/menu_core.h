// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <dsd-neo/core/dsd.h>

// Minimal, reusable ncurses menu framework for DSD-neo terminal UI.
// The UI layer renders and dispatches; business logic lives in callbacks.

typedef struct NcMenuItem NcMenuItem;

typedef bool (*nc_enabled_fn)(void* ctx);
typedef void (*nc_action_fn)(void* ctx);
typedef const char* (*nc_label_fn)(void* ctx, char* buf, size_t buf_len);

struct NcMenuItem {
    const char* id;            // stable identifier for the item
    const char* label;         // static label text (fallback if label_fn is NULL)
    nc_label_fn label_fn;      // optional dynamic label generator (writes into buf and returns it)
    const char* help;          // optional help text shown with 'h'
    nc_enabled_fn is_enabled;  // optional predicate; NULL -> enabled
    nc_action_fn on_select;    // action to run when selected; may open submenus
    const NcMenuItem* submenu; // optional nested items
    size_t submenu_len;        // length of submenu
};

// Run a simple vertical menu within the current ncurses screen.
// Returns when an item without a submenu finishes its action, or the user presses 'q'.
// If an item has a submenu, it is invoked recursively.
void ui_menu_run(const NcMenuItem* items, size_t n_items, void* ctx);

// Transient status footer (small one-line messages)
// Shows for a short time at the bottom of the menu window.
void ui_statusf(const char* fmt, ...);

// Legacy blocking prompt helpers have been removed in favor of
// nonblocking overlay prompts managed internally by the UI menu engine.

// IO submenu entry point using the core framework
void ui_menu_io_options(dsd_opts* opts, dsd_state* state);
// Additional grouped submenus
void ui_menu_logging_capture(dsd_opts* opts, dsd_state* state);
void ui_menu_trunking_control(dsd_opts* opts, dsd_state* state);
void ui_menu_keys_security(dsd_opts* opts, dsd_state* state);

// DSP submenu for RTL-SDR builds
void ui_menu_dsp_options(dsd_opts* opts, dsd_state* state);

// Key Entry and LRRP menus
void ui_menu_key_entry(dsd_opts* opts, dsd_state* state);
void ui_menu_lrrp_options(dsd_opts* opts, dsd_state* state);

// Top-level Main Menu
void ui_menu_main(dsd_opts* opts, dsd_state* state);

// Nonblocking menu overlay API
// Open the main menu as a nonblocking overlay. Subsequent draws happen via ui_menu_tick,
// and keys are routed via ui_menu_handle_key. The overlay preserves the same look/feel.
void ui_menu_open_async(dsd_opts* opts, dsd_state* state);
// Returns 1 when the overlay is currently open.
int ui_menu_is_open(void);
// Handle a key for the overlay. Returns 1 if the key was consumed by the menu.
int ui_menu_handle_key(int ch, dsd_opts* opts, dsd_state* state);
// Draw/update one frame of the overlay (no input read here).
void ui_menu_tick(dsd_opts* opts, dsd_state* state);
