// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The audible-audio stamp (issue #574): see <dsd-neo/core/audio_activity.h>. */

#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_activity_internal.h"

static dsd_atomic_u64 g_audio_activity_armed = {0U};
/* The stamp in real-time monotonic milliseconds, 0 for none. */
static dsd_atomic_u64 g_audio_activity_stamp_ms = {0U};
/* Tests only (dsd_audio_activity_set_clock_for_test()); NULL in production. */
static dsd_audio_activity_clock_fn g_audio_activity_clock = NULL;

static uint64_t
audio_activity_now_ms(void) {
    return g_audio_activity_clock != NULL ? g_audio_activity_clock() : dsd_realtime_mono_ms();
}

void
dsd_audio_activity_set_clock_for_test(dsd_audio_activity_clock_fn clock) {
    g_audio_activity_clock = clock;
}

void
dsd_audio_activity_arm(void) {
    dsd_atomic_u64_store_release(&g_audio_activity_armed, 1U);
}

int
dsd_audio_activity_armed(void) {
    return dsd_atomic_u64_load_relaxed(&g_audio_activity_armed) != 0U;
}

void
dsd_audio_activity_note(void) {
    if (!dsd_audio_activity_armed()) {
        return;
    }
    const uint64_t now_ms = audio_activity_now_ms();
    dsd_atomic_u64_store_release(&g_audio_activity_stamp_ms, now_ms != 0U ? now_ms : 1U);
}

void
dsd_audio_activity_read(uint64_t* stamp, int32_t* age_ms) {
    const uint64_t stamp_ms = dsd_atomic_u64_load_acquire(&g_audio_activity_stamp_ms);
    if (stamp != NULL) {
        *stamp = stamp_ms;
    }
    if (age_ms == NULL) {
        return;
    }
    *age_ms = -1;
    if (stamp_ms == 0U) {
        return;
    }
    /* A note on another thread can land between the load and this clock read; the age is still that of the stamp
       loaded, and a clock reading behind it ages it 0. */
    const uint64_t now_ms = audio_activity_now_ms();
    const uint64_t elapsed_ms = now_ms > stamp_ms ? now_ms - stamp_ms : 0U;
    if (elapsed_ms <= (uint64_t)DSD_AUDIO_ACTIVITY_MAX_AGE_MS) {
        *age_ms = (int32_t)elapsed_ms;
    }
}

void
dsd_audio_activity_reset(void) {
    dsd_atomic_u64_store_release(&g_audio_activity_stamp_ms, 0U);
}
