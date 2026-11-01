// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Terminal mouse wheel: whether this terminal can offer it.
 *
 * Terminal-private.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_UI_MOUSE_H
#define DSD_NEO_SRC_UI_TERMINAL_UI_MOUSE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Menu row predicate (is_enabled) for "Mouse wheel steps channels": true when the terminal can report wheel events.
 * Always true until the mouse capture lands.
 *
 * @param ctx UiCtx pointer (unused).
 * @return true when the row is offered.
 */
bool ui_mouse_wheel_available(const void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_UI_MOUSE_H */
