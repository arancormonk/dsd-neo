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

// Prompt helpers (standardized windows, input and validation)
// These helpers return 1 on success, 0 on user cancel/empty input.
int ui_prompt_string(const char* title, char* out, size_t out_cap);
int ui_prompt_int(const char* title, int* out);
int ui_prompt_double(const char* title, double* out);
int ui_prompt_confirm(const char* title); // 1 = yes, 0 = no

// Prefill variants that show current value in the input box
int ui_prompt_string_prefill(const char* title, const char* current, char* out, size_t out_cap);
int ui_prompt_int_prefill(const char* title, int current, int* out);
int ui_prompt_double_prefill(const char* title, double current, double* out);

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
