// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The P25 patch table's listening clock and owner against the real engine, a rigctl peer stood in for by GNU ld --wrap:
 *  - a CRC-good control-channel block (p25_sm_note_validated_cc()) advances the clock;
 *  - a trunk excursion to a voice channel and back (dsd_engine_tune_leaves_carrier() on both tunes) adds nothing,
 *    however short it is;
 *  - a stream gap breaks continuity and keeps the members; the no-carrier reset breaks continuity;
 *  - every other carrier boundary (a user tune, a new source, an input switch, a scan step, a replay retune) empties
 *    the table and forgets its owner; a manual return to the control channel, which runs no boundary, keeps it when
 *    the control channel is the P25 system's own and empties it otherwise;
 *  - a trunking-off replay retune from system A's control channel to system B's voice channel naming the same
 *    supergroup starts B with no members.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/p25_patch_policy.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "commands_internal.h"

static int g_failures = 0;

static void
expect(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

static int
near(double a, double b) {
    return fabs(a - b) < 1e-6;
}

/* ---------------- The rigctl peer, stood in for ---------------- */

// GNU ld --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
bool __wrap_SetFreq(dsd_socket_t sockfd, long int freq);
bool __wrap_SetModulation(dsd_socket_t sockfd, int bandwidth);
int __wrap_io_control_set_freq_tagged(dsd_opts* opts, const dsd_state* state, uint32_t freq_hz,
                                      uint64_t* out_pending_id);

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

/* The app-control tunes reach the peer through io_control's rigctl leg, which takes every frequency here. */
int
__wrap_io_control_set_freq_tagged(dsd_opts* opts, const dsd_state* state, uint32_t freq_hz, uint64_t* out_pending_id) {
    (void)state;
    if (out_pending_id) {
        *out_pending_id = 0U;
    }
    if (!opts || freq_hz == 0U) {
        return -1;
    }
    opts->rigctl_sent_freq_hz = freq_hz;
    opts->rtlsdr_center_freq = freq_hz;
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

/* ---------------- A P25 session on a rigctl peer ---------------- */

enum { kSystemAWacn = 0xA1001, kSystemASysid = 0x101 };

static const long kCcHz = 460012500L;
static const long kVcHz = 460262500L;
static const uint64_t kT0Ns = 2000000000000ULL;

/* The decode clock's reading, kept so each case steps it on from where the last step left it. */
static uint64_t g_now_ns;

static void
clock_to(uint64_t ns) {
    g_now_ns = ns;
    dsd_decode_clock_test_set_ns(ns);
}

static void
clock_step_ms(uint64_t ms) {
    clock_to(g_now_ns + (ms * 1000000ULL));
}

static int
session_new(dsd_opts** out_opts, dsd_state** out_state, int trunk) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return -1;
    }
    initOpts(opts);
    initState(state);
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->setmod_bw = 0;
    opts->trunk_enable = trunk;
    opts->rtlsdr_center_freq = (uint32_t)kCcHz;
    state->p25_cc_freq = kCcHz;
    state->trunk_cc_freq = kCcHz;
    dsd_trunk_tuning_requests_reset();
    dsd_engine_rigctl_tune_cache_forget();
    dsd_decode_clock_use_test(kT0Ns);
    g_now_ns = kT0Ns;
    *out_opts = opts;
    *out_state = state;
    return 0;
}

static void
session_free(dsd_opts* opts, dsd_state* state) {
    freeState(state);
    free(state);
    free(opts);
    dsd_decode_clock_use_system();
}

/* @p count CRC-good control-channel blocks, 100 ms apart on the decode clock, the first at the current reading. */
static void
hear_cc(dsd_opts* opts, dsd_state* state, int count) {
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            clock_step_ms(100U);
        }
        p25_sm_note_validated_cc(opts, state);
    }
}

/* System A's control channel heard for 2 s, with SG 1400 patching WG 1401. */
static void
seed_system_a(dsd_opts* opts, dsd_state* state) {
    (void)p25_update_system_identity(state, kSystemAWacn, kSystemASysid);
    hear_cc(opts, state, 11);
    p25_patch_add_wgid(state, 1400, 1401);
    hear_cc(opts, state, 10);
}

static int
patch_idx(const dsd_state* state, int sg) {
    for (int i = 0; i < state->p25_patch_count && i < DSD_P25_PATCH_MAX; i++) {
        if (state->p25_patch_active[i] && state->p25_patch_sgid[i] == (uint16_t)sg) {
            return i;
        }
    }
    return -1;
}

static int
table_empty_unowned(const dsd_state* state) {
    return state->p25_patch_count == 0 && patch_idx(state, 1400) < 0 && state->p25_patch_owner_known == 0
           && near(state->p25_patch_listen_s, 0.0) && near(state->p25_patch_listen_last_m, 0.0);
}

/* Control-channel listening: each CRC-good block adds its gap, at most 0.25 s. */
static void
test_cc_blocks_advance(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state, 1) != 0) {
        expect("cc blocks: a session", 0);
        return;
    }
    hear_cc(opts, state, 21);
    expect("cc blocks: 2 s of control channel listen 2 s", near(state->p25_patch_listen_s, 2.0));
    session_free(opts, state);
}

/* A trunk excursion of @p away_ms to a voice channel and back: the clock adds nothing for it, even when it is shorter
   than the 0.25 s gap bound. The patch is current throughout. */
static void
test_trunk_excursion(uint64_t away_ms) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state, 1) != 0) {
        expect("excursion: a session", 0);
        return;
    }
    seed_system_a(opts, state);
    const double before = state->p25_patch_listen_s;
    expect("excursion: the follow is tuned",
           dsd_engine_trunk_tune_to_freq_request(opts, state, kVcHz, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    expect("excursion: the voice channel tune breaks continuity", near(state->p25_patch_listen_last_m, 0.0));
    clock_step_ms(away_ms);
    expect("excursion: the return is tuned",
           dsd_engine_trunk_tune_to_cc_request(opts, state, kCcHz, 0, 0U) == DSD_TRUNK_TUNE_RESULT_OK);
    hear_cc(opts, state, 2);
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "excursion of %llu ms: only the block after the first counts",
                 (unsigned long long)away_ms);
    expect(label, near(state->p25_patch_listen_s, before + 0.1));
    expect("excursion: the table stays", patch_idx(state, 1400) >= 0 && state->p25_patch_owner_known == 1);
    session_free(opts, state);
}

/* A stream gap is the same carrier: it breaks continuity and keeps the members. */
static void
test_stream_gap_keeps_members(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state, 0) != 0) {
        expect("stream gap: a session", 0);
        return;
    }
    seed_system_a(opts, state);
    const double before = state->p25_patch_listen_s;
    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_STREAM_GAP, 0);
    expect("stream gap: breaks continuity", near(state->p25_patch_listen_last_m, 0.0));
    clock_step_ms(100U);
    hear_cc(opts, state, 2);
    expect("stream gap: the block after the gap only stores its reading",
           near(state->p25_patch_listen_s, before + 0.1));
    const int idx = patch_idx(state, 1400);
    expect("stream gap: keeps the patch", idx >= 0);
    expect("stream gap: keeps the members", idx >= 0 && dsd_p25_patch_has_current_wgid(state, idx, 1401));
    expect("stream gap: keeps the owner",
           state->p25_patch_owner_known == 1 && state->p25_patch_owner_wacn == (uint32_t)kSystemAWacn);
    session_free(opts, state);
}

/* The no-carrier reset breaks continuity: what follows is another reception. */
static void
test_no_carrier_breaks(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state, 0) != 0) {
        expect("no carrier: a session", 0);
        return;
    }
    seed_system_a(opts, state);
    dsd_engine_reset_no_carrier_state(opts, state);
    expect("no carrier: breaks continuity", near(state->p25_patch_listen_last_m, 0.0));
    expect("no carrier: keeps the table", patch_idx(state, 1400) >= 0);
    session_free(opts, state);
}

/* Every boundary that leaves the carrier empties the table and forgets its owner. */
static void
test_boundary_clears(dsd_carrier_boundary_kind kind, const char* name) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state, 0) != 0) {
        expect("boundary: a session", 0);
        return;
    }
    seed_system_a(opts, state);
    dsd_engine_carrier_boundary(opts, state, kind, 0);
    char label[96];
    DSD_SNPRINTF(label, sizeof label, "%s boundary: empties the table and forgets its owner", name);
    expect(label, table_empty_unowned(state));
    session_free(opts, state);
}

/* I/Q replay with trunking off: a recorded retune from system A's control channel to system B's voice channel, whose
   signalling names the same supergroup (an SG-only refresh, as a regroup voice user is), starts B with no members. */
static void
test_replay_retune_new_system(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state, 0) != 0) {
        expect("replay: a session", 0);
        return;
    }
    seed_system_a(opts, state);
    dsd_engine_leave_replay_carrier(opts, state);
    expect("replay: the retune empties the table", table_empty_unowned(state));
    p25_patch_update(state, 1400, 1, 1);
    const int idx = patch_idx(state, 1400);
    expect("replay: B's SG is a row", idx >= 0);
    expect("replay: B's SG has no members", idx >= 0 && !dsd_p25_patch_has_current_wgid(state, idx, 1401));
    expect("replay: no member reaches the grant path", dsd_p25_patch_media_target(opts, state, 1400, 1401, 0) == 1400U
                                                           && !dsd_p25_patch_is_current_member(state, 1400, 1401));
    session_free(opts, state);
}

/* A manual return to the control channel under trunking runs no carrier boundary. To the control channel of the P25
   system the table belongs to, it is a same-system move: the table and its owner stay, and only the clock's continuity
   breaks. To another control channel (here a trunk control channel that is not the P25 system's), it names no system
   and empties the table as a user tune does. */
static void
test_manual_return(int same_system) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state, 1) != 0) {
        expect("manual return: a session", 0);
        return;
    }
    seed_system_a(opts, state);
    if (!same_system) {
        state->trunk_cc_freq = kVcHz;
    }
    dsd_app_command_session_set_open(1);
    expect("manual return: applied", dsd_app_command_action(DSD_APP_CMD_RETURN_CC) == DSD_APP_COMMAND_SUBMIT_QUEUED
                                         && dsd_app_drain_cmds(opts, state) == 1);
    dsd_app_command_session_set_open(0);
    if (same_system) {
        const int idx = patch_idx(state, 1400);
        expect("same-system return: keeps the members", idx >= 0 && dsd_p25_patch_has_current_wgid(state, idx, 1401));
        expect("same-system return: keeps the owner",
               state->p25_patch_owner_known == 1 && state->p25_patch_owner_wacn == (uint32_t)kSystemAWacn);
        expect("same-system return: breaks continuity", near(state->p25_patch_listen_last_m, 0.0));
    } else {
        expect("return to another control channel: empties the table and forgets its owner",
               table_empty_unowned(state));
    }
    session_free(opts, state);
}

int
main(void) {
    test_cc_blocks_advance();
    test_trunk_excursion(200U);
    // Preservation: a gap past the 0.25 s bound never counted, with or without the departure's break.
    test_trunk_excursion(600U);
    test_stream_gap_keeps_members();
    test_no_carrier_breaks();
    test_boundary_clears(DSD_CARRIER_BOUNDARY_TUNE, "tune");
    test_boundary_clears(DSD_CARRIER_BOUNDARY_SOURCE, "source");
    test_boundary_clears(DSD_CARRIER_BOUNDARY_INPUT_SWITCH, "input switch");
    test_boundary_clears(DSD_CARRIER_BOUNDARY_SCAN_STEP, "scan step");
    test_boundary_clears(DSD_CARRIER_BOUNDARY_REPLAY_RETUNE, "replay retune");
    test_replay_retune_new_system();
    test_manual_return(1);
    test_manual_return(0);
    if (g_failures == 0) {
        printf("ENGINE_P25_PATCH_OWNER: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
