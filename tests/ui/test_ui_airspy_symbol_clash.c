// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The terminal menu callbacks must not carry libairspy's exported names. This unit includes the
 * libairspy header the build uses together with the menu action header: a callback named like an
 * SDK call is a conflicting declaration and fails to compile. A global callback with an SDK name
 * would also shadow the library at link time and take the adapter's gain calls (#806).
 */

#include <airspy.h> // IWYU pragma: keep

#include "menu_actions.h" // IWYU pragma: keep

int
main(void) {
    return 0;
}
