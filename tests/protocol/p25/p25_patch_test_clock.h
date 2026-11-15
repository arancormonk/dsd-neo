// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
#ifndef DSD_NEO_TEST_P25_PATCH_TEST_CLOCK_H
#define DSD_NEO_TEST_P25_PATCH_TEST_CLOCK_H

#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/state.h>

/* Hear the control channel for @p seconds: CRC-good PDUs every 100 ms advance the patch listening clock. */
static inline void
p25_patch_test_listen_for(dsd_state* st, double seconds) {
    double now = st->p25_patch_listen_last_m;
    if (now <= 0.0) {
        now = 1000.0;
        dsd_p25_patch_listen_note(st, now);
    }
    const int steps = (int)(seconds * 10.0 + 0.5);
    for (int i = 0; i < steps; i++) {
        now += 0.1;
        dsd_p25_patch_listen_note(st, now);
    }
}

#endif /* DSD_NEO_TEST_P25_PATCH_TEST_CLOCK_H */
