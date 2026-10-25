// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether the session is an EDACS one: the session fact EDACS-only behaviour keys on.
 *
 * dsd_opts::frame_provoice only says the EDACS/ProVoice decoder is enabled, and Auto enables it beside every other
 * digital decoder. Startup and configuration policy written for an EDACS session (the raw analog sink and the UDP
 * socket on port + 2, the -8 override under trunking, the EDACS squelch policy, the "Pulse Analog Output" line) keys
 * on dsd_edacs_session() instead. TG lockout and Skip key on the call's own protocol, not on the session. The EA/ESK
 * toggles and the EDACS menu rows keep the capability flag: they are how an Auto session selects EDACS addressing.
 *
 * Header-only, so runtime code below the core library (the squelch policy) reads it without a link dependency, and
 * <dsd-neo/core/state.h> keeps its forward-declared dsd_opts.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_EDACS_SESSION_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_EDACS_SESSION_H_H

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return 1 for a selected EDACS session: the EDACS/ProVoice decoder enabled, and EDACS addressing selected
 * (dsd_state::ea_mode >= 0) or ProVoice the one digital decoder enabled.
 *
 * Every EDACS/ProVoice preset qualifies: -fh/-fH/-fe/-fE select an addressing mode, and -fp, which selects none, runs
 * ProVoice alone. Auto qualifies once EDACS addressing is selected (-fh -fa, an EDACS-to-Auto switch, Auto with
 * edacs_ea, the EA toggle); plain Auto, which leaves ea_mode at -1, does not. A NULL @p state counts as no addressing
 * selected.
 */
static inline int
dsd_edacs_session(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || opts->frame_provoice != 1) {
        return 0;
    }
    return ((state && state->ea_mode >= 0) || dsd_opts_digital_decoder_count(opts) == 1) ? 1 : 0;
}

/**
 * @brief The -8 override of a trunked EDACS session on UDP output: with trunking on, UDP output (port + 2 carrying the
 * analog audio) and an EDACS session, turn the -8 source monitor off. Engine output setup applies it at start
 * (`-fh -T -8 -o udp`); the EA toggle, a decode-mode switch and a config apply when they turn a running session into an
 * EDACS one (`-fa -T -8 -o udp`, then the toggle, a switch to EDACS/ProVoice, or a config with edacs_ea).
 *
 * @return 1 when it turned -8 off, else 0.
 */
static inline int
dsd_edacs_session_trunked_monitor_off(dsd_opts* opts, const dsd_state* state) {
    if (!opts || opts->monitor_input_audio != 1 || opts->audio_out_type != 8 || opts->trunk_enable != 1
        || !dsd_edacs_session(opts, state)) {
        return 0;
    }
    opts->monitor_input_audio = 0;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_EDACS_SESSION_H_H */
