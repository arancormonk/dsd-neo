// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* A frontend publication a P25 watchdog tick put off reaches the frontends on a silent input. The tick holds the P25 SM
 * tick guard; the decoder's publication does not wait for it, and is put off. The decoder's frame-sync hunt waits on a
 * silent UDP input in slices, which it leaves for a queued command; it leaves for the put-off publication too, once the
 * tick has left the guard, and the control pump's retry publishes it. Real: the app-control publishers, the control
 * pump and the runtime queries it registers, the hunt read (getSymbol()) and the tick guard. Simulated: the UDP input
 * (silent) and the tick (a thread holding the guard). */
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/app_control/snapshot_internal.h"

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

enum {
    k_baseline_hz = 851000000,
    k_put_off_hz = 852000000,
    k_tick_ms = 800,      /* the tick outlasts a stream pause: the hunt's wait is ready to leave while it runs */
    k_sample_every = 30,  /* a sample now and then, so a read that never leaves still comes back to the test */
    k_deadline_ms = 3000, /* after the tick */
    k_bound_ms = DSD_ANALOG_STREAM_PAUSE_MIN_MS + 500,
};

static int
expect(int condition, const char* message) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

/* The silent UDP input: every wait times out, but for a sample now and then. */
static int g_waits;

static int
silent_udp_read_sample_wait(dsd_opts* opts, int16_t* out, unsigned int timeout_ms) {
    (void)opts;
    if (++g_waits % k_sample_every == 0) {
        *out = 1;
        return 1;
    }
    dsd_sleep_ms(timeout_ms);
    return -1;
}

/* The watchdog tick: holds the guard for k_tick_ms, then leaves it and notes when. */
static struct {
    dsd_mutex_t mutex;
    dsd_cond_t condition;
    int held;
    dsd_atomic_u64 released_ms; /* 0 while held */
} g_tick;

static DSD_THREAD_RETURN_TYPE
watchdog_tick(void* arg) {
    (void)arg;
    p25_sm_tick_guard_enter();
    dsd_mutex_lock(&g_tick.mutex);
    g_tick.held = 1;
    dsd_cond_broadcast(&g_tick.condition);
    dsd_mutex_unlock(&g_tick.mutex);
    dsd_sleep_ms(k_tick_ms);
    dsd_atomic_u64_store_release(&g_tick.released_ms, dsd_realtime_mono_ms());
    p25_sm_tick_guard_leave();
    DSD_THREAD_RETURN;
}

static int
put_off_publication_reaches_the_frontends(dsd_opts* opts, dsd_state* state) {
    state->trunk_cc_freq = k_baseline_hz;
    dsd_app_frontend_runtime_start(opts, state);
    const dsd_state* published = dsd_app_get_latest_snapshot();
    int rc = expect(published && published->trunk_cc_freq == k_baseline_hz, "the baseline is published");

    dsd_thread_t tick;
    if (dsd_thread_create(&tick, watchdog_tick, NULL) != 0) {
        dsd_app_frontend_runtime_stop();
        return expect(0, "tick thread started");
    }
    dsd_mutex_lock(&g_tick.mutex);
    while (!g_tick.held) {
        if (dsd_cond_timedwait(&g_tick.condition, &g_tick.mutex, 2000U) != 0) {
            break;
        }
    }
    const int held = g_tick.held;
    dsd_mutex_unlock(&g_tick.mutex);
    rc |= expect(held, "the tick holds the guard");

    /* A command's result, published while the tick runs: put off, not waited for. */
    state->trunk_cc_freq = k_put_off_hz;
    dsd_telemetry_publish_snapshot(state);
    published = dsd_app_get_latest_snapshot();
    rc |= expect(dsd_app_telemetry_snapshot_pending() && dsd_atomic_u64_load_acquire(&g_tick.released_ms) == 0U
                     && published && published->trunk_cc_freq == k_baseline_hz,
                 "the publication is put off while the tick holds the guard");

    /* The decode loop on a silent input: the hunt reads, and the pump runs when a read leaves its wait. */
    int interrupts_while_held = 0;
    uint64_t published_ms = 0U;
    for (;;) {
        (void)getSymbol(opts, state, 0);
        const uint64_t released_ms = dsd_atomic_u64_load_acquire(&g_tick.released_ms);
        if (state->input_interrupted) {
            state->input_interrupted = 0;
            if (released_ms == 0U) {
                ++interrupts_while_held;
            }
            dsd_runtime_pump_controls(opts, state);
        }
        published = dsd_app_get_latest_snapshot();
        if (published && published->trunk_cc_freq == k_put_off_hz) {
            published_ms = dsd_realtime_mono_ms();
            break;
        }
        if (released_ms != 0U && dsd_realtime_mono_ms() - released_ms > k_deadline_ms) {
            break;
        }
    }
    dsd_thread_join(tick);
    const uint64_t released_ms = dsd_atomic_u64_load_acquire(&g_tick.released_ms);
    if (published_ms == 0U || published_ms - released_ms > k_bound_ms) {
        DSD_FPRINTF(stderr, "published %s, %llu ms after the tick\n", published_ms ? "late" : "never",
                    (unsigned long long)(published_ms ? published_ms - released_ms : 0U));
    }
    rc |= expect(published_ms != 0U && published_ms - released_ms <= k_bound_ms,
                 "the put-off publication reaches the frontends soon after the tick, with the input still silent");
    rc |= expect(!dsd_app_telemetry_snapshot_pending(), "nothing is left pending");
    rc |= expect(interrupts_while_held == 0, "the hunt does not leave its read while the tick still holds the guard");
    dsd_app_frontend_runtime_stop();
    return rc;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(state);
        free(opts);
        return 1;
    }
    opts->audio_in_type = AUDIO_IN_UDP;
    opts->wav_sample_rate = 48000;
    opts->input_volume_multiplier = 1;
    state->samplesPerSymbol = 1;
    state->symbolCenter = 0;
    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.udp_read_sample_wait = silent_udp_read_sample_wait;
    dsd_net_audio_input_hooks_set(hooks);
    (void)dsd_mutex_init(&g_tick.mutex);
    (void)dsd_cond_init(&g_tick.condition);
    dsd_atomic_u64_init(&g_tick.released_ms, 0U);

    const int rc = put_off_publication_reaches_the_frontends(opts, state);

    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_cond_destroy(&g_tick.condition);
    dsd_mutex_destroy(&g_tick.mutex);
    free(state);
    free(opts);
    return rc;
}
