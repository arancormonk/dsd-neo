// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

typedef struct mbe_parameters {
    int dummy;
} mbe_parms;

void mbe_floattoshort(const float* in, short* out);
