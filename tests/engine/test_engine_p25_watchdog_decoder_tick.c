// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The P25 watchdog thread is a beat: the tick it asks for runs on the decoder thread (p25_sm_watchdog_service()), which
 * owns dsd_state and dsd_opts and makes every tune. The real watchdog thread runs beside this thread, which does what the
 * decoder does: it writes what a sync accept writes and reads what the hunt reads, outside the P25 SM tick guard, and
 * services the beats. A voice channel followed here is released by a watchdog-paced tick, which returns to the control
 * channel through the rigctl peer: every frequency the peer is sent comes from this thread, and so does every write of
 * the release (the symbol profile, the tuned flag, the active slot, the payload crypto fields of both slots). ThreadSanitizer checks the
 * rest in its build. Only the rigctl peer is replaced, so GNU ld and lld only. */
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include "engine_hooks_install.h"
#include "test_env.h"

enum { VOICE_CHANNEL = 0x1234, TALKGROUP = 1234, SOURCE = 42, WAIT_MS = 5000, NO_CARRIER_EVERY = 16 };

static const long CC_FREQ_HZ = 851000000L;
static const long VC_FREQ_HZ = 852000000L;

#if defined(_MSC_VER)
#define TEST_THREAD_LOCAL __declspec(thread)
#else
#define TEST_THREAD_LOCAL _Thread_local
#endif
/* Set on the thread that decodes: this test's main thread. */
static TEST_THREAD_LOCAL int t_decoder_thread = 0;
static atomic_int g_tunes;
static atomic_int g_off_thread_tunes;
static atomic_int g_last_tune_hz;

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
bool __wrap_SetFreq(dsd_socket_t sockfd, long int freq);
bool __wrap_SetModulation(dsd_socket_t sockfd, int bandwidth);
bool __wrap_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth);
bool __wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth);
void __wrap_RigctlMarkSessionPassband(dsd_socket_t sockfd, int kind);

bool
__wrap_SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    if (!t_decoder_thread) {
        atomic_store(&g_off_thread_tunes, 1);
    }
    atomic_store(&g_last_tune_hz, (int)freq);
    atomic_fetch_add(&g_tunes, 1);
    return true;
}

bool
__wrap_SetModulation(dsd_socket_t sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return true;
}

bool
__wrap_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth) {
    (void)sockfd;
    (void)kind;
    (void)bandwidth;
    return true;
}

bool
__wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth) {
    (void)sockfd;
    (void)kind;
    (void)bandwidth;
    return true;
}

void
__wrap_RigctlMarkSessionPassband(dsd_socket_t sockfd, int kind) {
    (void)sockfd;
    (void)kind;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

/* What the decoder does between frames, outside the guard: a sync accept's writes, the reads the hunt's matchers, VC
   timers and P25 tick gate make, and now and then a no-carrier pass's reset, which zeroes the payload crypto fields
   (ALG, KID, MI, both slots) a release zeroes too. Returns the reads folded together, so none is optimized away. */
static long
hunt_step(dsd_opts* opts, dsd_state* state, int round) {
    state->lastsynctype = (round & 1) != 0 ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS;
    dsd_mark_cc_sync(state);
    if (round % NO_CARRIER_EVERY == 0) {
        dsd_engine_reset_no_carrier_state(opts, state);
    }
    return (long)opts->trunk_is_tuned + state->rf_mod + state->samplesPerSymbol + state->symbolCenter
           + state->p25_p2_active_slot + state->sps_hunt_idx + state->p25_p2_audio_allowed[0]
           + (long)state->last_vc_sync_time + state->payload_algid + state->payload_keyidR;
}

/* A followed voice channel the watchdog's beat releases: every tune of it, the grant's and the return's, is this
   thread's. */
static int
check_release_tunes_on_decoder(dsd_opts* opts, dsd_state* state) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, opts, state);
    p25_sm_event_t grant = p25_sm_ev_group_grant(VOICE_CHANNEL, VC_FREQ_HZ, TALKGROUP, SOURCE, 0);
    p25_sm_tick_guard_enter();
    p25_sm_event(ctx, opts, state, &grant);
    p25_sm_tick_guard_leave();
    if (ctx->state != P25_SM_TUNED || atomic_load(&g_last_tune_hz) != VC_FREQ_HZ) {
        DSD_FPRINTF(stderr, "FAIL: the grant was not followed (state %d)\n", (int)ctx->state);
        return 1;
    }
    const int grant_tunes = atomic_load(&g_tunes);
    p25_sm_tick_guard_enter();
    state->p25_sm_force_release = 1;
    p25_sm_tick_guard_leave();

    p25_sm_watchdog_start(opts, state);
    const double started_s = dsd_realtime_mono_s();
    long folded = 0;
    int round = 0;
    while (atomic_load(&g_tunes) == grant_tunes && dsd_realtime_mono_s() - started_s < WAIT_MS / 1000.0) {
        folded += hunt_step(opts, state, round++);
        (void)p25_sm_watchdog_service(opts, state);
        dsd_thread_yield();
    }
    /* Some more beats on the control channel the release returned to. */
    const double returned_s = dsd_realtime_mono_s();
    while (dsd_realtime_mono_s() - returned_s < 0.2) {
        folded += hunt_step(opts, state, round++);
        (void)p25_sm_watchdog_service(opts, state);
        dsd_thread_yield();
    }
    p25_sm_watchdog_stop();

    int failed = 0;
    if (atomic_load(&g_tunes) == grant_tunes || atomic_load(&g_last_tune_hz) != CC_FREQ_HZ) {
        DSD_FPRINTF(stderr, "FAIL: no return to the control channel within %d ms (%ld)\n", WAIT_MS, folded);
        failed = 1;
    }
    if (atomic_load(&g_off_thread_tunes) != 0) {
        DSD_FPRINTF(stderr, "FAIL: the watchdog thread tuned the receiver itself\n");
        failed = 1;
    }
    if (ctx->state == P25_SM_TUNED || opts->trunk_is_tuned != 0) {
        DSD_FPRINTF(stderr, "FAIL: the release left the voice channel tuned\n");
        failed = 1;
    }
    return failed;
}

/* A beat that finds the guard held stays asked for: the holder here is another thread, which takes the guard, says so,
   and holds it until this thread has looked. Every wait is bounded. */
static atomic_int g_held;
static atomic_int g_looked;

static DSD_THREAD_RETURN_TYPE
hold_guard(void* opaque) {
    (void)opaque;
    p25_sm_tick_guard_enter();
    atomic_store(&g_held, 1);
    const double started_s = dsd_realtime_mono_s();
    while (atomic_load(&g_looked) == 0 && dsd_realtime_mono_s() - started_s < WAIT_MS / 1000.0) {
        dsd_sleep_ms(1U);
    }
    p25_sm_tick_guard_leave();
    DSD_THREAD_RETURN;
}

static int
check_busy_guard_keeps_the_beat(dsd_opts* opts, dsd_state* state) {
    atomic_store(&g_held, 0);
    atomic_store(&g_looked, 0);
    p25_sm_watchdog_request_tick();
    dsd_thread_t holder;
    if (dsd_thread_create(&holder, hold_guard, NULL) != 0) {
        DSD_FPRINTF(stderr, "FAIL: the guard holder did not start\n");
        return 1;
    }
    const double started_s = dsd_realtime_mono_s();
    while (atomic_load(&g_held) == 0 && dsd_realtime_mono_s() - started_s < WAIT_MS / 1000.0) {
        dsd_sleep_ms(1U);
    }
    int failed = 0;
    if (atomic_load(&g_held) == 0) {
        DSD_FPRINTF(stderr, "FAIL: the guard holder never took the guard\n");
        failed = 1;
    } else if (p25_sm_watchdog_service(opts, state) != 0 || !p25_sm_watchdog_tick_wanted()) {
        DSD_FPRINTF(stderr, "FAIL: a held guard lost the beat\n");
        failed = 1;
    }
    atomic_store(&g_looked, 1);
    dsd_thread_join(holder);
    if (p25_sm_watchdog_service(opts, state) != 1 || p25_sm_watchdog_tick_wanted()) {
        DSD_FPRINTF(stderr, "FAIL: the free guard did not run the beat's tick\n");
        failed = 1;
    }
    return failed;
}

int
main(void) {
    if (dsd_test_setenv("DSD_NEO_P25_WD_MS", "20", 1) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not set the watchdog cadence\n");
        return 1;
    }
    dsd_neo_config_init();
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "FAIL: out of memory\n");
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    t_decoder_thread = 1;
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->use_rigctl = 1;
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->frame_p25p1 = 1;
    state->p25_cc_freq = state->trunk_cc_freq = CC_FREQ_HZ;
    state->trunk_chan_map[VOICE_CHANNEL] = VC_FREQ_HZ;
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_P25);
    dsd_engine_trunk_tuning_hooks_install();
    dsd_engine_set_decoder_thread(1);

    int failed = check_release_tunes_on_decoder(opts, state);
    failed |= check_busy_guard_keeps_the_beat(opts, state);

    dsd_engine_set_decoder_thread(0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    freeState(state);
    free(state);
    free(opts);
    (void)dsd_test_unsetenv("DSD_NEO_P25_WD_MS");
    return failed;
}
