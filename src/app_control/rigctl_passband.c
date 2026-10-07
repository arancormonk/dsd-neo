// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The live follow of a rigctl peer's passband (issue #621). On audio input the peer demodulates what DSD-neo hears, so
 * a command that changes the passband the session asks of it (a width, -B, a decode-mode or input switch, a config) is
 * asked of the peer at once, as a tune would ask it, rather than at the next retune.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/rigctl_passband.h>
#include <dsd-neo/runtime/scan_mode.h>
#include "services.h"

dsd_rigctl_passband
svc_rigctl_passband_now(const dsd_opts* opts, const dsd_state* state) {
    if (!dsd_opts_rigctl_peer_demodulates(opts)) {
        const dsd_rigctl_passband follow = {DSD_ANALOG_DEMOD_FM, opts ? opts->setmod_bw : 0,
                                            DSD_RIGCTL_PASSBAND_FOLLOW};
        return follow;
    }
    /* The row whose scope is in force, as the engine's live apply reads it, not a -Y tune staged since. */
    return dsd_rigctl_passband_of(opts, dsd_scan_mode_row_options(state));
}

svc_rigctl_session
svc_rigctl_session_now(const dsd_opts* opts, const dsd_state* state) {
    svc_rigctl_session session;
    session.peer = dsd_opts_rigctl_peer_demodulates(opts) ? 1 : 0;
    session.request = svc_rigctl_passband_now(opts, state);
    return session;
}

/* Whether the request @p now, after a change, is asked of the peer, given the session @p before it (NULL: unknown). */
static int
rigctl_follow_asks(const svc_rigctl_session* before, const dsd_rigctl_passband* now) {
    if (now->source == DSD_RIGCTL_PASSBAND_FOLLOW) {
        /* Off the monitor the peer follows at -B, which a tune sends best-effort. A passband this client set on the
           monitor before the change (a width, -B standing in) is undone with that request at once, so the peer does not
           keep it until another tune; FOLLOW before as well (a -B edit on a digital session) waits for the tune. */
        return (before && before->peer && dsd_rigctl_passband_sets_passband(&before->request)) ? 1 : 0;
    }
    if (before && !before->peer) {
        /* A peer that came to demodulate the input with the change (a first rigctl connect, a switch onto audio input)
           is asked for a width only, as the start and a reconnect ask it (dsd_engine_rigctl_ask_session_passband()):
           -B and the peer's own passband are what a session asked nothing of always ran, and a tune asks for them. */
        return dsd_rigctl_passband_is_width(now);
    }
    /* The source counts: entering the monitor from FOLLOW with the peer there is a change even where the wire request
       matches, and the peer's record of what this client asked of it skips a request it is known to run. */
    return (before && dsd_rigctl_passband_equal(&before->request, now)) ? 0 : 1;
}

/* Whether the request @p now reads the setting an explicit edit changed (svc_rigctl_follow_edit()): the configured
   width of analog kind @p edit, or -B (SVC_RIGCTL_EDIT_SETMOD_BW), which stands in for an unset NFM width (the peer's
   own passband without it). A row's own width shadows a width edit, a configured NFM width a -B edit, and off the
   monitor (FOLLOW) neither setting is what the peer runs. */
static int
rigctl_request_reads_edit(const dsd_rigctl_passband* now, int edit) {
    if (edit == SVC_RIGCTL_EDIT_SETMOD_BW) {
        return (now->source == DSD_RIGCTL_PASSBAND_SETMOD_BW || now->source == DSD_RIGCTL_PASSBAND_PEER_OWN) ? 1 : 0;
    }
    return (now->kind == edit && now->source != DSD_RIGCTL_PASSBAND_ROW && now->source != DSD_RIGCTL_PASSBAND_FOLLOW)
               ? 1
               : 0;
}

/* svc_rigctl_follow_passband(), and with @p edit (SVC_RIGCTL_EDIT_NONE for none) an explicit edit of a setting: asked
   whether or not the request changed while the request in force reads that setting. */
static int
rigctl_follow(const dsd_opts* opts, const dsd_state* state, const svc_rigctl_session* before, int edit, int guarded) {
    if (!dsd_opts_rigctl_peer_demodulates(opts)) {
        return 0;
    }
    /* A suspended scope puts the row's options back at its resume, which the dispatcher follows; a staged -Y tune or
       retry asks the peer for the row it tunes. */
    if (dsd_scan_mode_updating(state) || dsd_engine_channel_scan_waiting(state)) {
        return 0;
    }
    const dsd_rigctl_passband now = svc_rigctl_passband_now(opts, state);
    const int edited = edit != SVC_RIGCTL_EDIT_NONE && rigctl_request_reads_edit(&now, edit);
    if (!edited && !rigctl_follow_asks(before, &now)) {
        return 0;
    }
    /* The P25 watchdog's retunes use the rigctl socket and its peer record (as svc_rigctl_connect() replaces the
       connection under the guard). The guard is not re-entrant: a caller that holds it says so. */
    if (!guarded) {
        p25_sm_tick_guard_enter();
    }
    const int rc = dsd_engine_scan_rigctl_apply_modulation(opts, state);
    if (!guarded) {
        p25_sm_tick_guard_leave();
    }
    /* The engine logs a refusal. A lost reply leaves the peer on either passband, which a refusal's rollback asks
       again. */
    if (rc > 0) {
        return 1;
    }
    return rc == 0 ? -1 : 0;
}

int
svc_rigctl_follow_passband(const dsd_opts* opts, const dsd_state* state, const svc_rigctl_session* before,
                           int guarded) {
    return rigctl_follow(opts, state, before, SVC_RIGCTL_EDIT_NONE, guarded);
}

int
svc_rigctl_follow_edit(const dsd_opts* opts, const dsd_state* state, const svc_rigctl_session* before, int edit,
                       int guarded) {
    /* A retry of the setting a refused transition left unchanged (a switch onto the monitor the peer refused) must
       reach the peer; the peer's record skips a request it is confirmed to run, so an applied value sends nothing. */
    return rigctl_follow(opts, state, before, edit, guarded);
}
