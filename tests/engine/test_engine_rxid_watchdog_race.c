// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The received identity (dsd_state::rx_id) is the decoder thread's. The P25 watchdog's accepted retunes reach the
 * engine's trunk tuning from another thread, under the P25 SM tick guard, and end the calls the record names there,
 * while the decoder notes, runs its no-carrier passes and reads the record: run both at once, which ThreadSanitizer
 * checks in its build. Only the rigctl peer is replaced. */
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include "engine_hooks_install.h"

enum { RACE_ROUNDS = 400 };

static dsd_opts g_opts;
static dsd_state g_state;
static atomic_int g_stop;
static int g_failed;

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
bool __wrap_SetFreq(dsd_socket_t sockfd, long int freq);
bool __wrap_SetModulation(dsd_socket_t sockfd, int bandwidth);
bool __wrap_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth);
bool __wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth);
void __wrap_RigctlMarkSessionPassband(dsd_socket_t sockfd, int kind);

bool
__wrap_SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
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

/* What the P25 watchdog's tick does to the receiver: follow a voice channel, return to the control channel, each an
   accepted retune made under the tick guard on its own thread. */
static DSD_THREAD_RETURN_TYPE
watchdog_retunes(void* opaque) {
    (void)opaque;
    for (int i = 0; i < RACE_ROUNDS && atomic_load(&g_stop) == 0; i++) {
        p25_sm_tick_guard_enter();
        if ((i & 1) == 0) {
            (void)dsd_engine_trunk_tune_to_freq_request(&g_opts, &g_state, 852000000L, 0, 0U);
        } else {
            (void)dsd_engine_return_to_cc_request(&g_opts, &g_state, 0U);
        }
        p25_sm_tick_guard_leave();
        dsd_thread_yield();
    }
    DSD_THREAD_RETURN;
}

/* What the decoder thread does meanwhile: a frame under the tick guard (processFrame(): the pending drop applied, the
   dispatch marked, the checked notes and a retune of its own, which applies its drop at once, a call the release then
   ends), the no-carrier pass's identity step, and a frontend publication's read of the record, which takes no guard.
   The watchdog's retunes read the dispatch mark under the guard. */
static uint32_t
decoder_rounds(void) {
    uint32_t seen = 0U;
    const dsd_call_observation call = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1234U,
        .policy_target_id = 1234U,
        .ota_source_id = 42U,
    };
    for (int i = 0; i < RACE_ROUNDS; i++) {
        p25_sm_tick_guard_enter();
        dsd_rxid_apply_trunk_tune(&g_state);
        g_state.frame_dispatch_active = 1;
        dsd_rxid_note_access(&g_state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
        dsd_rxid_note_p25_network(&g_state, 0xBEE00U, 0x1A3U);
        (void)dsd_call_state_observe(&g_state, &call, DSD_CALL_BOUNDARY_BEGIN);
        if ((i & 3) == 0) {
            dsd_rxid_request_trunk_tune(&g_state);
            if (g_state.rx_id_trunk_tune_pending != 0U || (g_state.rx_id.valid & DSD_RXID_ACCESS) != 0U) {
                DSD_FPRINTF(stderr, "FAIL: a retune inside the dispatch left the access code\n");
                g_failed = 1;
            }
        }
        g_state.frame_dispatch_active = 0;
        p25_sm_tick_guard_leave();
        dsd_engine_no_carrier_identity_stale(&g_state, 0);
        dsd_received_identity published;
        DSD_MEMCPY(&published, &g_state.rx_id, sizeof(published));
        seen |= published.valid;
        dsd_thread_yield();
    }
    return seen;
}

/* The decoder thread's own retunes between frames drop the access code at once, under the tick guard: a P25 grant it
   follows outside a frame dispatch (the guard taken for the drop), then the frame sync's no-sync release of that voice
   channel, whose return to the control channel runs in the frame sync's hold on the guard. What the decoder publishes
   next shows neither carrier's code, the system stays, and the next code noted is the reached carrier's first. */
static int
check_decoder_retunes_between_frames(void) {
    int failed = 0;
    g_opts.trunk_tune_group_calls = 1;
    g_state.trunk_chan_map[0x1234] = 852000000L;
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    dsd_rxid_note_p25_network(&g_state, 0xBEE00U, 0x1A3U);
    dsd_rxid_note_access(&g_state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
    const uint32_t epoch = g_state.rx_id.epoch;
    const uint32_t system = DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID;

    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, 852000000L, 1234, 42, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
    dsd_received_identity published = g_state.rx_id;
    if (ctx->state != P25_SM_TUNED || g_state.rx_id_trunk_tune_pending != 0U || published.valid != system) {
        DSD_FPRINTF(stderr, "FAIL: the followed grant left the control channel's access code\n");
        failed = 1;
    }
    dsd_rxid_note_access(&g_state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x2F0U);

    dsd_engine_frame_sync_hooks_install();
    dsd_frame_sync_hook_p25_sm_release(&g_opts, &g_state);
    dsd_frame_sync_hooks_set(NULL);
    published = g_state.rx_id;
    if (ctx->state == P25_SM_TUNED || g_state.rx_id_trunk_tune_pending != 0U || published.valid != system
        || published.p25_sysid != 0x1A3U) {
        DSD_FPRINTF(stderr, "FAIL: the frame sync's release left the voice channel's access code\n");
        failed = 1;
    }
    dsd_rxid_note_access(&g_state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
    if (g_state.rx_id.valid != (system | DSD_RXID_ACCESS) || g_state.rx_id.epoch != epoch) {
        DSD_FPRINTF(stderr, "FAIL: the control channel's code read as another system\n");
        failed = 1;
    }
    return failed;
}

int
main(void) {
    initOpts(&g_opts);
    initState(&g_state);
    g_opts.audio_in_type = AUDIO_IN_NULL;
    g_opts.use_rigctl = 1;
    g_opts.trunk_enable = 1;
    g_opts.frame_p25p1 = 1;
    g_state.p25_cc_freq = g_state.trunk_cc_freq = 851000000L;
    dsd_trunk_recovery_note_protocol(&g_state, DSD_TRUNK_RECOVERY_P25);
    dsd_engine_trunk_tuning_hooks_install();
    dsd_engine_set_decoder_thread(1); /* this thread decodes; the one below stands in for the watchdog's */
    g_failed = check_decoder_retunes_between_frames();
    atomic_store(&g_stop, 0);

    dsd_thread_t watchdog;
    if (dsd_thread_create(&watchdog, watchdog_retunes, NULL) != 0) {
        DSD_FPRINTF(stderr, "FAIL: watchdog thread did not start\n");
        return 1;
    }
    const uint32_t seen = decoder_rounds();
    atomic_store(&g_stop, 1);
    dsd_thread_join(watchdog);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    freeState(&g_state);
    if ((seen & DSD_RXID_P25_SYSID) == 0U) {
        DSD_FPRINTF(stderr, "FAIL: the decoder's notes never reached the record\n");
        return 1;
    }
    return g_failed ? 1 : 0;
}
