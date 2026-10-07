// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #621: the live follow of a rigctl peer's passband. On audio input the peer demodulates, and a command that
 * changes the request in force (svc_rigctl_passband_now()) asks the peer for it strictly, through the engine's live
 * apply, under the P25 SM tick guard unless the caller holds it. Nothing is asked with no such peer, off the analog
 * monitor (FOLLOW: a tune asks for -B there), for an unchanged request, while a scan row's scope is suspended (the
 * resume follows) or while a -Y tune is staged (the restaged tune asks). The source counts: entering the FM monitor
 * from a digital mode is a change even where the wire request matches. A peer the change brings (none demodulated the
 * input before) is asked for a width only, as the start asks it.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/rigctl_passband.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdlib.h>
#include "services.h"

/* ---- stubs ---- */

static int g_apply_rc = 1; /* what the engine's live apply answers: 1 taken, 0 refused or reply lost, -1 no peer */
static int g_apply_calls;  /* live applies asked for */
static int g_apply_depth;  /* the guard depth the last apply ran at */
static dsd_rigctl_passband g_applied; /* the request the last apply sent, as the engine reads it */
static int g_scan_waiting;            /* a -Y tune or retry staged */
static int g_guard_depth;
static int g_guard_enters;

int
dsd_engine_scan_rigctl_apply_modulation(const dsd_opts* opts, const dsd_state* state) {
    g_apply_calls++;
    g_apply_depth = g_guard_depth;
    /* The request the engine sends: the row whose scope is in force. */
    g_applied = dsd_rigctl_passband_of(opts, dsd_scan_mode_row_options(state));
    return g_apply_rc;
}

int
dsd_engine_channel_scan_waiting(const dsd_state* state) {
    (void)state;
    return g_scan_waiting;
}

void
p25_sm_tick_guard_enter(void) {
    g_guard_depth++;
    g_guard_enters++;
    assert(g_guard_depth == 1); /* not re-entrant */
}

void
p25_sm_tick_guard_leave(void) {
    assert(g_guard_depth == 1);
    g_guard_depth--;
}

/* ---- helpers ---- */

static void
reset_stubs(void) {
    g_apply_rc = 1;
    g_apply_calls = 0;
    g_apply_depth = -1;
    DSD_MEMSET(&g_applied, 0, sizeof g_applied);
    g_scan_waiting = 0;
    g_guard_depth = 0;
    g_guard_enters = 0;
}

/* The analog FM monitor (-fA) on UDP audio input with a rigctl peer on socket 5. */
static void
seed_fm_monitor_on_peer(dsd_opts* opts) {
    opts->audio_in_type = AUDIO_IN_UDP;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = (dsd_socket_t)5;
    opts->analog_only = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_nfm_bandwidth_hz = 0;
    opts->analog_am_bandwidth_hz = 0;
    opts->setmod_bw = 0;
    opts->frame_dmr = 0;
}

/* The session before a change, as svc_rigctl_session_now() records it. */
static svc_rigctl_session
session_of(int peer, int kind, int bandwidth_hz, int source) {
    svc_rigctl_session session;
    session.peer = peer;
    session.request.kind = kind;
    session.request.bandwidth_hz = bandwidth_hz;
    session.request.source = source;
    return session;
}

static int
same_request(const dsd_rigctl_passband* p, int kind, int bandwidth_hz, int source) {
    return p->kind == kind && p->bandwidth_hz == bandwidth_hz && p->source == source;
}

/* The request in force: FOLLOW without a peer that demodulates the input, else the rule for the row in force. */
static void
test_request_now(dsd_opts* opts, dsd_state* state) {
    seed_fm_monitor_on_peer(opts);
    opts->setmod_bw = 12500;
    dsd_rigctl_passband now = svc_rigctl_passband_now(opts, state);
    assert(same_request(&now, DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_SETMOD_BW));
    opts->analog_nfm_bandwidth_hz = 20000;
    now = svc_rigctl_passband_now(opts, state);
    assert(same_request(&now, DSD_ANALOG_DEMOD_FM, 20000, DSD_RIGCTL_PASSBAND_CONFIGURED));

    /* An nfm row with a width of its own on air: the row's, as the engine's live apply asks for it. */
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 11250;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    now = svc_rigctl_passband_now(opts, state);
    assert(same_request(&now, DSD_ANALOG_DEMOD_FM, 11250, DSD_RIGCTL_PASSBAND_ROW));
    dsd_scan_mode_leave(opts, state);

    /* No peer that demodulates the input: FOLLOW, whatever the monitor would ask. */
    opts->use_rigctl = 0;
    now = svc_rigctl_passband_now(opts, state);
    assert(now.source == DSD_RIGCTL_PASSBAND_FOLLOW);
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    now = svc_rigctl_passband_now(opts, state);
    assert(now.source == DSD_RIGCTL_PASSBAND_FOLLOW);
    opts->rigctl_sockfd = (dsd_socket_t)5;
    opts->audio_in_type = AUDIO_IN_RTL;
    now = svc_rigctl_passband_now(opts, state);
    assert(now.source == DSD_RIGCTL_PASSBAND_FOLLOW);
    opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
    now = svc_rigctl_passband_now(opts, state);
    assert(now.source == DSD_RIGCTL_PASSBAND_FOLLOW);
    now = svc_rigctl_passband_now(NULL, state);
    assert(now.source == DSD_RIGCTL_PASSBAND_FOLLOW && now.bandwidth_hz == 0);

    /* The session records whether a peer demodulates the input beside the request, which FOLLOW alone does not say. */
    svc_rigctl_session session = svc_rigctl_session_now(opts, state);
    assert(!session.peer && session.request.source == DSD_RIGCTL_PASSBAND_FOLLOW);
    opts->audio_in_type = AUDIO_IN_UDP;
    opts->analog_only = 0;
    opts->frame_dmr = 1;
    session = svc_rigctl_session_now(opts, state);
    assert(session.peer && session.request.source == DSD_RIGCTL_PASSBAND_FOLLOW);
    opts->analog_only = 1;
    opts->frame_dmr = 0;
    session = svc_rigctl_session_now(NULL, state);
    assert(!session.peer && session.request.source == DSD_RIGCTL_PASSBAND_FOLLOW);
}

/* Nothing to ask: no peer that demodulates the input, an RTL input, a digital mode (FOLLOW), an unchanged request. */
static void
test_nothing_to_ask(dsd_opts* opts, dsd_state* state) {
    reset_stubs();
    seed_fm_monitor_on_peer(opts);
    const svc_rigctl_session follow = session_of(1, DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_FOLLOW);
    opts->analog_nfm_bandwidth_hz = 12500;

    opts->use_rigctl = 0;
    assert(svc_rigctl_follow_passband(opts, state, &follow, 0) == 0);
    opts->use_rigctl = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    assert(svc_rigctl_follow_passband(opts, state, &follow, 0) == 0);
    opts->audio_in_type = AUDIO_IN_NULL;
    assert(svc_rigctl_follow_passband(opts, state, &follow, 0) == 0);
    opts->audio_in_type = AUDIO_IN_UDP;

    /* A digital mode asks for FOLLOW, which a tune sends best-effort: nothing for the follow. */
    opts->analog_only = 0;
    opts->frame_dmr = 1;
    const svc_rigctl_session before = svc_rigctl_session_now(opts, state);
    opts->setmod_bw = 20000;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    opts->analog_only = 1;
    opts->frame_dmr = 0;
    opts->setmod_bw = 0;

    /* The request in force is the one before. */
    const svc_rigctl_session same = svc_rigctl_session_now(opts, state);
    assert(svc_rigctl_follow_passband(opts, state, &same, 0) == 0);
    assert(svc_rigctl_follow_passband(NULL, state, &follow, 0) == 0);
    assert(g_apply_calls == 0 && g_guard_enters == 0);
}

/* A suspended scope (a scoped command runs) waits for the follow after its resume; a staged -Y tune or retry asks for
   the row it tunes itself. */
static void
test_waits_for_the_scope_and_the_staged_tune(dsd_opts* opts, dsd_state* state) {
    reset_stubs();
    seed_fm_monitor_on_peer(opts);
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(opts, state, NULL) == 0);
    const svc_rigctl_session before = svc_rigctl_session_now(opts, state);
    assert(before.peer && before.request.source == DSD_RIGCTL_PASSBAND_PEER_OWN);
    assert(dsd_scan_mode_suspend(opts, state) != 0);
    opts->analog_nfm_bandwidth_hz = 12500;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    (void)dsd_scan_mode_resume(opts, state);
    assert(opts->analog_nfm_bandwidth_hz == 12500);
    g_scan_waiting = 1;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    assert(g_apply_calls == 0);
    g_scan_waiting = 0;
    /* Once the scope is back and nothing is staged, the change is asked. */
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1);
    assert(g_apply_calls == 1 && same_request(&g_applied, DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_CONFIGURED));
    dsd_scan_mode_leave(opts, state);
}

/* A changed request is asked strictly: 1 taken, -1 refused (a lost reply too: the engine's apply answers 0 for both).
   The guard is taken around the request once, unless the caller holds it. */
static void
test_asks_a_changed_request(dsd_opts* opts, dsd_state* state) {
    reset_stubs();
    seed_fm_monitor_on_peer(opts);
    opts->setmod_bw = 12500;
    const svc_rigctl_session before = svc_rigctl_session_now(opts, state);
    opts->analog_nfm_bandwidth_hz = 20000;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1);
    assert(g_apply_calls == 1 && g_apply_depth == 1 && g_guard_enters == 1 && g_guard_depth == 0);
    assert(same_request(&g_applied, DSD_ANALOG_DEMOD_FM, 20000, DSD_RIGCTL_PASSBAND_CONFIGURED));

    /* Under a guard the caller holds (a config apply's), the request goes out without taking it again. */
    reset_stubs();
    g_guard_depth = 1;
    assert(svc_rigctl_follow_passband(opts, state, &before, 1) == 1);
    assert(g_apply_calls == 1 && g_apply_depth == 1 && g_guard_enters == 0 && g_guard_depth == 1);
    g_guard_depth = 0;

    /* Refused, or the reply lost: the peer runs either passband, so the caller treats both as a refusal. */
    reset_stubs();
    g_apply_rc = 0;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == -1);
    assert(g_apply_calls == 1 && g_guard_enters == 1 && g_guard_depth == 0);

    /* A peer gone between the check and the request asks nothing. */
    reset_stubs();
    g_apply_rc = -1;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);

    /* A NULL request before reads as unknown: the request in force is asked. */
    reset_stubs();
    assert(svc_rigctl_follow_passband(opts, state, NULL, 0) == 1 && g_apply_calls == 1);

    /* An AM request: the am row's monitor at the AM default, then a configured AM width. */
    reset_stubs();
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    const svc_rigctl_session am_before = svc_rigctl_session_now(opts, state);
    assert(same_request(&am_before.request, DSD_ANALOG_DEMOD_AM, DSD_ANALOG_AM_WIDTH_DEFAULT_HZ,
                        DSD_RIGCTL_PASSBAND_AM_DEFAULT));
    opts->analog_am_bandwidth_hz = 8000;
    assert(svc_rigctl_follow_passband(opts, state, &am_before, 0) == 1);
    assert(same_request(&g_applied, DSD_ANALOG_DEMOD_AM, 8000, DSD_RIGCTL_PASSBAND_CONFIGURED));
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_am_bandwidth_hz = 0;
}

/* Back onto the FM monitor from a digital mode after the configured width was cleared, with neither a width nor -B:
   the wire request (FM at 0) is the one FOLLOW made, but the peer's own passband is now asked of the monitor, and a
   peer an earlier width left on that width is put back on its own. */
static void
test_reentry_into_the_monitor_is_a_change(dsd_opts* opts, dsd_state* state) {
    reset_stubs();
    seed_fm_monitor_on_peer(opts);
    opts->analog_only = 0;
    opts->frame_dmr = 1;
    const svc_rigctl_session before = svc_rigctl_session_now(opts, state);
    assert(before.peer && same_request(&before.request, DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_FOLLOW));
    opts->frame_dmr = 0;
    opts->analog_only = 1;
    const dsd_rigctl_passband now = svc_rigctl_passband_now(opts, state);
    assert(same_request(&now, DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_PEER_OWN));
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1);
    assert(g_apply_calls == 1 && same_request(&g_applied, DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_PEER_OWN));
}

/* A peer the change brings, where none demodulated the input before (a first rigctl connect, a switch from a radio
   input onto audio input): the start and a reconnect ask it for a width only, and so does the follow. -B and the peer's
   own passband are what a session asked nothing of always ran, and a tune asks for them. A digital mode the peer
   follows is no such case: its FOLLOW request had the peer there (test_reentry_into_the_monitor_is_a_change()). */
static void
test_a_new_peer_is_asked_for_a_width_only(dsd_opts* opts, dsd_state* state) {
    reset_stubs();
    seed_fm_monitor_on_peer(opts);
    opts->use_rigctl = 0;
    svc_rigctl_session before = svc_rigctl_session_now(opts, state);
    assert(!before.peer && before.request.source == DSD_RIGCTL_PASSBAND_FOLLOW);
    opts->use_rigctl = 1;
    /* -B standing in for the unset width, then the peer's own passband: nothing. */
    opts->setmod_bw = 12500;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    opts->setmod_bw = 0;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    assert(g_apply_calls == 0 && g_guard_enters == 0);
    /* A configured width is asked, once. */
    opts->analog_nfm_bandwidth_hz = 20000;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1);
    assert(g_apply_calls == 1 && same_request(&g_applied, DSD_ANALOG_DEMOD_FM, 20000, DSD_RIGCTL_PASSBAND_CONFIGURED));
    /* So is the AM default, a width too. */
    reset_stubs();
    opts->analog_nfm_bandwidth_hz = 0;
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1);
    assert(
        same_request(&g_applied, DSD_ANALOG_DEMOD_AM, DSD_ANALOG_AM_WIDTH_DEFAULT_HZ, DSD_RIGCTL_PASSBAND_AM_DEFAULT));
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    /* A switch from an RTL input with rigctl on: the peer followed only the frequency there. */
    reset_stubs();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->setmod_bw = 12500;
    before = svc_rigctl_session_now(opts, state);
    assert(!before.peer);
    opts->audio_in_type = AUDIO_IN_TCP;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    opts->analog_nfm_bandwidth_hz = 16000;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1);
    assert(g_apply_calls == 1 && same_request(&g_applied, DSD_ANALOG_DEMOD_FM, 16000, DSD_RIGCTL_PASSBAND_CONFIGURED));
    opts->analog_nfm_bandwidth_hz = 0;
    opts->setmod_bw = 0;
}

/* Leaving the FM monitor for a digital mode with the peer there (issue #621 review): a passband this client set on the
   monitor (a configured width, -B standing in) is undone at once with the request a digital mode makes, FM at -B or at
   the peer's own passband (0), rather than left on the peer until another tune. A monitor at the peer's own passband
   set nothing, and a -B edit on a digital session (FOLLOW before as well) waits for the next tune. */
static void
test_leaving_the_monitor_undoes_its_passband(dsd_opts* opts, dsd_state* state) {
    reset_stubs();
    seed_fm_monitor_on_peer(opts);
    opts->analog_nfm_bandwidth_hz = 20000;
    opts->setmod_bw = 12500;
    svc_rigctl_session before = svc_rigctl_session_now(opts, state);
    assert(before.peer && before.request.source == DSD_RIGCTL_PASSBAND_CONFIGURED);
    opts->analog_only = 0;
    opts->frame_p25p1 = 1;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1);
    assert(g_apply_calls == 1 && g_guard_enters == 1 && g_apply_depth == 1);
    assert(same_request(&g_applied, DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_FOLLOW));
    /* Without -B: the peer's own passband. */
    reset_stubs();
    opts->setmod_bw = 0;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1);
    assert(same_request(&g_applied, DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_FOLLOW));
    /* A refusal, or a lost reply, is reported; the caller keeps the change. */
    reset_stubs();
    g_apply_rc = 0;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == -1 && g_apply_calls == 1);
    /* -B standing in on the monitor is a passband this client set too; an AM row's AM default as well. */
    reset_stubs();
    opts->setmod_bw = 12500;
    before = session_of(1, DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_SETMOD_BW);
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1 && g_apply_calls == 1);
    reset_stubs();
    before = session_of(1, DSD_ANALOG_DEMOD_AM, DSD_ANALOG_AM_WIDTH_DEFAULT_HZ, DSD_RIGCTL_PASSBAND_AM_DEFAULT);
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 1 && g_apply_calls == 1);
    /* Nothing to undo: the monitor ran the peer's own passband, no peer was there, or nothing is known of before. */
    reset_stubs();
    before = session_of(1, DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_PEER_OWN);
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    before = session_of(0, DSD_ANALOG_DEMOD_FM, 12500, DSD_RIGCTL_PASSBAND_FOLLOW);
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    assert(svc_rigctl_follow_passband(opts, state, NULL, 0) == 0);
    /* A -B edit on the digital session: FOLLOW before and after. */
    before = svc_rigctl_session_now(opts, state);
    opts->setmod_bw = 9000;
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    assert(g_apply_calls == 0 && g_guard_enters == 0);
    opts->frame_p25p1 = 0;
    opts->analog_only = 1;
    opts->analog_nfm_bandwidth_hz = 0;
    opts->setmod_bw = 0;
}

/* An explicit edit of the setting the request in force reads (issue #621 review) asks the peer whether or not the
   request changed, so a retry of a width a refused transition left unchanged reaches the peer; an edit that the
   request in force does not read asks only a change, as any command does. */
static void
test_an_edit_in_force_is_asked_unchanged(dsd_opts* opts, dsd_state* state) {
    reset_stubs();
    seed_fm_monitor_on_peer(opts);
    opts->analog_nfm_bandwidth_hz = 20000;
    svc_rigctl_session before = svc_rigctl_session_now(opts, state);
    assert(svc_rigctl_follow_passband(opts, state, &before, 0) == 0);
    assert(svc_rigctl_follow_edit(opts, state, &before, DSD_ANALOG_DEMOD_FM, 0) == 1);
    assert(g_apply_calls == 1 && g_guard_enters == 1);
    assert(same_request(&g_applied, DSD_ANALOG_DEMOD_FM, 20000, DSD_RIGCTL_PASSBAND_CONFIGURED));
    /* Not read: the other kind's width, -B under a configured NFM width. */
    reset_stubs();
    assert(svc_rigctl_follow_edit(opts, state, &before, DSD_ANALOG_DEMOD_AM, 0) == 0);
    assert(svc_rigctl_follow_edit(opts, state, &before, SVC_RIGCTL_EDIT_SETMOD_BW, 0) == 0);
    assert(g_apply_calls == 0);
    /* -B standing in for the unset width, or the peer's own passband without it: both edits read it. */
    opts->analog_nfm_bandwidth_hz = 0;
    opts->setmod_bw = 12500;
    before = svc_rigctl_session_now(opts, state);
    assert(svc_rigctl_follow_edit(opts, state, &before, SVC_RIGCTL_EDIT_SETMOD_BW, 0) == 1);
    assert(svc_rigctl_follow_edit(opts, state, &before, DSD_ANALOG_DEMOD_FM, 0) == 1);
    opts->setmod_bw = 0;
    before = svc_rigctl_session_now(opts, state);
    assert(svc_rigctl_follow_edit(opts, state, &before, SVC_RIGCTL_EDIT_SETMOD_BW, 0) == 1);
    assert(g_apply_calls == 3 && same_request(&g_applied, DSD_ANALOG_DEMOD_FM, 0, DSD_RIGCTL_PASSBAND_PEER_OWN));
    /* The AM monitor at its default reads the AM width. */
    reset_stubs();
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    before = svc_rigctl_session_now(opts, state);
    assert(svc_rigctl_follow_edit(opts, state, &before, DSD_ANALOG_DEMOD_AM, 0) == 1);
    assert(svc_rigctl_follow_edit(opts, state, &before, DSD_ANALOG_DEMOD_FM, 0) == 0);
    assert(g_apply_calls == 1);
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    /* A row's own width shadows a width edit. */
    reset_stubs();
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 11250;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    before = svc_rigctl_session_now(opts, state);
    assert(svc_rigctl_follow_edit(opts, state, &before, DSD_ANALOG_DEMOD_FM, 0) == 0);
    dsd_scan_mode_leave(opts, state);
    /* Off the monitor, a -B edit on a digital session waits for a tune. */
    opts->analog_only = 0;
    opts->frame_dmr = 1;
    opts->setmod_bw = 12500;
    before = svc_rigctl_session_now(opts, state);
    assert(svc_rigctl_follow_edit(opts, state, &before, SVC_RIGCTL_EDIT_SETMOD_BW, 0) == 0);
    assert(svc_rigctl_follow_edit(opts, state, &before, DSD_ANALOG_DEMOD_FM, 0) == 0);
    /* Without a peer that demodulates the input, nothing. */
    opts->frame_dmr = 0;
    opts->analog_only = 1;
    opts->use_rigctl = 0;
    assert(svc_rigctl_follow_edit(opts, state, &before, SVC_RIGCTL_EDIT_SETMOD_BW, 0) == 0);
    assert(g_apply_calls == 0);
    opts->use_rigctl = 1;
    opts->setmod_bw = 0;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    test_request_now(opts, state);
    test_nothing_to_ask(opts, state);
    test_waits_for_the_scope_and_the_staged_tune(opts, state);
    test_asks_a_changed_request(opts, state);
    test_reentry_into_the_monitor_is_a_change(opts, state);
    test_a_new_peer_is_asked_for_a_width_only(opts, state);
    test_leaving_the_monitor_undoes_its_passband(opts, state);
    test_an_edit_in_force_is_asked_unchanged(opts, state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return 0;
}
