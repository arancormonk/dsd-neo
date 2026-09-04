// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Dibit-capture stubs for frame-sync tests that do not source-list src/core/frames/dsd_dibit.c. */

#include <dsd-neo/core/dibit.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
write_symbol_capture_record(dsd_opts* opts, dsd_state* state, int dibit, float symbol, const dsd_dibit_soft_t* soft) {
    (void)opts;
    (void)state;
    (void)dibit;
    (void)symbol;
    (void)soft;
}

uint8_t
dmr_compute_reliability(const dsd_state* st, float sym) {
    (void)st;
    (void)sym;
    return 255;
}
