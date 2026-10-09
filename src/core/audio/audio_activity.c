// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The audible-audio stamp (issue #574): see <dsd-neo/core/audio_activity.h>. */

#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/platform/audio.h"

#ifdef DSD_NEO_TEST_HOOKS
#include "audio_activity_internal.h"
#endif

static dsd_atomic_u64 g_audio_activity_armed = {0U};
/* The stamp in real-time monotonic milliseconds, 0 for none. */
static dsd_atomic_u64 g_audio_activity_stamp_ms = {0U};

#ifdef DSD_NEO_TEST_HOOKS
/* The clock the test seam sets (dsd_audio_activity_set_clock_for_test()); NULL reads the real one. */
static dsd_audio_activity_clock_fn g_audio_activity_clock = NULL;

void
dsd_audio_activity_set_clock_for_test(dsd_audio_activity_clock_fn clock) {
    g_audio_activity_clock = clock;
}
#endif

static uint64_t
audio_activity_now_ms(void) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_audio_activity_clock != NULL) {
        return g_audio_activity_clock();
    }
#endif
    return dsd_realtime_mono_ms();
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

int
dsd_audio_activity_output_plays(const dsd_opts* opts, const dsd_audio_stream* stream, int fd_takes_block) {
    if (opts == NULL || opts->audio_out != 1) {
        return 0;
    }
    if (opts->audio_out_type == 0) {
        return stream != NULL;
    }
    if (opts->audio_out_type == 8) {
        return 1;
    }
    if (opts->audio_out_type == 1) {
        return fd_takes_block != 0;
    }
    return 0;
}
