// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* A trunk-scan list switch (issue #697) runs on the decoder thread and publishes the scan timing it leaves, which reads
 * the parked target's P25 machine. The P25 watchdog changes that machine from its own thread, under the P25 SM tick
 * guard: switch a list off and on again and again while a stand-in for the watchdog writes the machine under the
 * guard, which ThreadSanitizer checks in its build. Only the rigctl peer is replaced. */
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "engine_hooks_install.h"
#include "test_support.h"

enum { RACE_ROUNDS = 400, SWITCH_DEADLINE_MS = 30000 };

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

/* What the watchdog's tick does to the parked target's machine, on its own thread under the tick guard: it moves the
   machine (here it writes back the state and control-channel tune flag it finds, which the timing reads). */
static DSD_THREAD_RETURN_TYPE
watchdog_ticks(void* opaque) {
    (void)opaque;
    while (atomic_load(&g_stop) == 0) {
        p25_sm_tick_guard_enter();
        p25_sm_ctx_t* ctx = (p25_sm_ctx_t*)dsd_engine_trunk_scan_active_p25_ctx();
        if (ctx) {
            const p25_sm_state_e sm_state = ctx->state;
            const int cc_tune_pending = ctx->cc_tune_pending;
            ctx->state = sm_state;
            ctx->cc_tune_pending = cc_tune_pending;
        }
        p25_sm_tick_guard_leave();
        dsd_thread_yield();
    }
    DSD_THREAD_RETURN;
}

/* Write one list file named @p name into @p dir, path into @p path. */
static int
write_list(const char* dir, const char* name, const char* row, char* path, size_t path_size) {
    if (dsd_test_path_join(path, path_size, dir, name) != 0) {
        return -1;
    }
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    const int ok =
        fputs("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n", fp) >= 0 && fputs(row, fp) >= 0;
    return fclose(fp) == 0 && ok ? 0 : -1;
}

/* The decoder thread's switches: list 2 off and on again, list 1 holding the target on air throughout. A switch the
   watchdog's hold on the guard refuses (BUSY) is asked again until it applies, so every round lands, and publishes the
   timing, beside the watchdog's writes; a real-time deadline, not a count of tries, bounds the wait, so a watchdog the
   scheduler holds back only slows the rounds down. Returns the rounds applied, or -1. */
static int
decoder_switches(void) {
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + SWITCH_DEADLINE_MS;
    int applied = 0;
    for (int i = 0; i < RACE_ROUNDS; i++) {
        int rc = DSD_TRUNK_SCAN_LIST_SET_BUSY;
        while (rc == DSD_TRUNK_SCAN_LIST_SET_BUSY && dsd_realtime_mono_ms() < deadline_ms) {
            rc = dsd_engine_trunk_scan_list_set(&g_opts, &g_state, g_state.trunk_scan_session, 2, i & 1,
                                                (uint32_t)i + 1U);
            dsd_thread_yield();
        }
        if (rc != DSD_TRUNK_SCAN_LIST_SET_OK) {
            DSD_FPRINTF(stderr, "FAIL: list switch %d answered %d\n", i, rc);
            return -1;
        }
        applied++;
    }
    return applied;
}

int
main(void) {
    char dir[DSD_TEST_PATH_MAX];
    char first[DSD_TEST_PATH_MAX];
    char second[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_scan_list_race") == NULL
        || write_list(dir, "security.csv", "s1,p25-trunk,851000000,,250,,\n", first, sizeof first) != 0
        || write_list(dir, "hospital.csv", "h1,p25-trunk,852000000,,250,,\n", second, sizeof second) != 0) {
        DSD_FPRINTF(stderr, "FAIL: list files not written\n");
        return 1;
    }
    initOpts(&g_opts);
    initState(&g_state);
    g_opts.audio_in_type = AUDIO_IN_NULL;
    g_opts.use_rigctl = 1;
    g_opts.trunk_scan_enabled = 1;
    DSD_SNPRINTF(g_opts.trunk_scan_lists[0].path, sizeof g_opts.trunk_scan_lists[0].path, "%s", first);
    DSD_SNPRINTF(g_opts.trunk_scan_lists[1].path, sizeof g_opts.trunk_scan_lists[1].path, "%s", second);
    g_opts.trunk_scan_list_count = 2;
    dsd_engine_trunk_tuning_hooks_install();
    dsd_engine_set_decoder_thread(1); /* this thread decodes; the one below stands in for the watchdog's */
    char err[512] = {0};
    int failed = 0;
    if (dsd_engine_trunk_scan_init(&g_opts, &g_state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "FAIL: scan init: %s\n", err);
        failed = 1;
    } else {
        atomic_store(&g_stop, 0);
        dsd_thread_t watchdog;
        if (dsd_thread_create(&watchdog, watchdog_ticks, NULL) != 0) {
            DSD_FPRINTF(stderr, "FAIL: watchdog thread did not start\n");
            failed = 1;
        } else {
            const int applied = decoder_switches();
            atomic_store(&g_stop, 1);
            dsd_thread_join(watchdog);
            if (applied != RACE_ROUNDS) {
                DSD_FPRINTF(stderr, "FAIL: %d of %d list switches applied\n", applied, RACE_ROUNDS);
                failed = 1;
            }
        }
        dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    freeState(&g_state);
    (void)remove(first);
    (void)remove(second);
    (void)dsd_test_rmdir(dir);
    return failed;
}
