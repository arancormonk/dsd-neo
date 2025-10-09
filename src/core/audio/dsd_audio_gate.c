// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Minimal audio gating helpers used by tests.
 */

#include <dsd-neo/core/dsd.h>

int
dsd_p25p2_mixer_gate(const dsd_state* state, int* encL, int* encR) {
    if (!state || !encL || !encR) {
        return -1;
    }
    *encL = state->p25_p2_audio_allowed[0] ? 0 : 1;
    *encR = state->p25_p2_audio_allowed[1] ? 0 : 1;
    return 0;
}
