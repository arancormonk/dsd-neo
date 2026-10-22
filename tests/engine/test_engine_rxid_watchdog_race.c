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
   checked notes, a call the release then ends), the no-carrier pass's identity step, and a frontend publication's read
   of the record, which takes no guard. */
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
        dsd_rxid_note_access(&g_state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
        dsd_rxid_note_p25_network(&g_state, 0xBEE00U, 0x1A3U);
        (void)dsd_call_state_observe(&g_state, &call, DSD_CALL_BOUNDARY_BEGIN);
        p25_sm_tick_guard_leave();
        dsd_engine_no_carrier_identity_stale(&g_state, 0);
        dsd_received_identity published;
        DSD_MEMCPY(&published, &g_state.rx_id, sizeof(published));
        seen |= published.valid;
        dsd_thread_yield();
    }
    return seen;
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
    return 0;
}
