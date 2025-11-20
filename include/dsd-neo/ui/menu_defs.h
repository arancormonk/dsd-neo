// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Public surface for data-driven menu trees */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <dsd-neo/ui/menu_core.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque to callers outside menu subsystem
typedef struct UiCtx UiCtx;

// Expose top-level menu items for the async overlay/menu system
void ui_menu_get_main_items(const NcMenuItem** out_items, size_t* out_n, UiCtx* ctx);

// Predicates/actions referenced by the top-level tree
bool io_rtl_active(void* ctx);
void act_exit(void* v);

#ifdef __cplusplus
}
#endif
