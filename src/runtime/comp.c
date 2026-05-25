// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/comp.h>

int
comp(const void* a, const void* b) {
    const float fa = *((const float*)a);
    const float fb = *((const float*)b);
    return (fa < fb) ? -1 : ((fb < fa) ? 1 : 0);
}
