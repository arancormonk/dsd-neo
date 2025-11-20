// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * UI primitives for ncurses-based terminal frontend
 * - Window helpers (new/destroy)
 * - Transient status message utilities
 * - Simple drawing helpers (headers, rules, borders)
 * - Gamma LUT mapping for density visualization
 */

#pragma once

#include <stddef.h>
#include <time.h>

// ncurses forward declarations (opaque in header)
typedef struct _win_st WINDOW;

#ifdef __cplusplus
extern "C" {
#endif

// Window helpers
WINDOW* ui_make_window(int h, int w, int y, int x);
void ui_destroy_window(WINDOW** win);

// Transient status footer (small one-line messages)
void ui_statusf(const char* fmt, ...);
// Returns 1 and copies current status to buf when not expired at 'now'.
int ui_status_peek(char* buf, size_t n, time_t now);
// Clear status when expired at 'now'. No-op if still active.
void ui_status_clear_if_expired(time_t now);

// Simple drawing helpers (operate on stdscr or given WINDOW when applicable)
void ui_print_hr(void);                  // horizontal rule to end of line
void ui_print_header(const char* title); // section header "--Title-----"
void ui_print_lborder(void);             // left border in primary color
void ui_print_lborder_green(void);       // left border in active/green color
short ui_iden_color_pair(int iden);      // map IDEN nibble -> color pair (21..28)

// Gamma LUT mapping for [0,1] -> [0,1] brightness (sqrt gamma)
double ui_gamma_map01(double f);

#ifdef __cplusplus
}
#endif
