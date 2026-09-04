// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file exitflag.c
 * @brief Global shutdown signaling flag for DSD-neo.
 *
 * Owns the shared exitflag variable used to signal graceful shutdown across
 * all modules. Signal handlers set it and processing loops observe it.
 */

#include <dsd-neo/runtime/exitflag.h>

volatile uint8_t exitflag = 0;
