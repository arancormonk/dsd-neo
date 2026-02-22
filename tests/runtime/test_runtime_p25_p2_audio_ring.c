// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <string.h>

static int
is_all_zero_160(const float* buf) {
    for (int i = 0; i < 160; i++) {
        if (buf[i] != 0.0f) {
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    static dsd_state state;
    memset(&state, 0, sizeof(state));

    float out[160];
    memset(out, 0xAA, sizeof(out));

    /* Pop on empty should return 0 and zero-fill output. */
    if (p25_p2_audio_ring_pop(&state, 0, out) != 0) {
        return 1;
    }
    if (!is_all_zero_160(out)) {
        return 2;
    }

    /* Push 5 frames into a depth-4 ring should drop the oldest (1). */
    float frame[160];
    for (int id = 1; id <= 5; id++) {
        memset(frame, 0, sizeof(frame));
        frame[0] = (float)id;
        if (!p25_p2_audio_ring_push(&state, 0, frame)) {
            return 3;
        }
    }
    if (state.p25_p2_audio_ring_count[0] != 4) {
        return 4;
    }

    for (int expected = 2; expected <= 5; expected++) {
        memset(out, 0xAA, sizeof(out));
        if (!p25_p2_audio_ring_pop(&state, 0, out)) {
            return 5;
        }
        if (out[0] != (float)expected) {
            return 6;
        }
    }

    /* Now empty again. */
    memset(out, 0xAA, sizeof(out));
    if (p25_p2_audio_ring_pop(&state, 0, out) != 0) {
        return 7;
    }
    if (!is_all_zero_160(out)) {
        return 8;
    }

    return 0;
}
