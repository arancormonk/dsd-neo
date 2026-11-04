// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Environment variable helpers for menu subsystem.
 *
 * This header is internal to src/ui/terminal/ and should NOT be installed.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_MENU_ENV_H_
#define DSD_NEO_SRC_UI_TERMINAL_MENU_ENV_H_

// Common helpers to get/set numeric env values with defaults.
int env_get_int(const char* name, int defv);
double env_get_double(const char* name, double defv);
void env_set_int(const char* name, int v);
void env_set_double(const char* name, double v);

// After changing env-backed runtime config, re-parse it and ask the decoder to apply it to its options.
void env_reparse_runtime_cfg(void);

#endif /* DSD_NEO_SRC_UI_TERMINAL_MENU_ENV_H_ */
