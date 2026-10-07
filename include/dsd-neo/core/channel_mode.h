// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Positional channel-map mode metadata, owned by core extension slot 5. */
#ifndef DSD_NEO_CORE_CHANNEL_MODE_H
#define DSD_NEO_CORE_CHANNEL_MODE_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Missing slots inherit the configured decoder settings. */
dsd_scan_mode dsd_channel_mode_get(const dsd_state* state, size_t row);
/** Set one scan-list slot, growing its heap store. Returns -1 on allocation failure. */
int dsd_channel_mode_set(dsd_state* state, size_t row, dsd_scan_mode mode);
/** Release all row modes. Does not restore active decoder settings. */
void dsd_channel_modes_clear(dsd_state* state);
/** Transfer row definitions, replacing destination modes and clearing the source extension.
 * Restore both states' global group policies first; active/suspended scopes are not moved. */
void dsd_channel_modes_move(dsd_state* dst, dsd_state* src);
/** Nonzero if at least one row declares a mode or carries scoped row options (scan_profile.h),
 * i.e. the typed scanner must run the list. */
int dsd_channel_modes_present(const dsd_state* state);
/** Nonzero if scan-list slot @p row runs received-tone detection while it is on air, so the tone policy can act on it
 * (issue #527): an nfm row, on which the row's own policy or the configured one applies, or, when
 * @p configured_fm_monitor says the configured decode mode is the analog FM monitor
 * (dsd_scan_mode_configured_fm_monitor()), a row that runs that mode -- one declaring no mode (INHERIT), as every row
 * of a list without a mode column does. A typed row of any other mode runs its own mode, never the configured one.
 * Asks nothing of the row's frequency. */
int dsd_channel_mode_hears_tones(const dsd_state* state, size_t row, int configured_fm_monitor);
/** Nonzero if a row the scanner tunes (one with a frequency) runs received-tone detection
 * (dsd_channel_mode_hears_tones()). */
int dsd_channel_modes_hear_tones(const dsd_state* state, int configured_fm_monitor);
/** Nonzero if a session that is not a trunk scan runs received-tone detection, where the tone policy applies (issue
 * #527): a -Y scan with a list by its rows (dsd_channel_modes_hear_tones()), anything else -- a -Y scan whose list has
 * no rows, or none yet, included, which stays on the configured decode mode -- by that mode
 * (dsd_scan_mode_configured_fm_monitor()). The one rule the command line, the engine's channel-map import and
 * dsd_engine_scan_hears_tones() weigh the "no effect" warning by. 0 for NULL @p opts or @p state. */
int dsd_channel_modes_conventional_hear_tones(const dsd_opts* opts, const dsd_state* state);
/** Nonzero if a passband this client asks of a rigctl peer now is the session's, not a scan row's (issue #621): the one
 * answer every rigctl leg marks a request by (RigctlMarkSessionPassband()), the engine's tunes, live apply, start and
 * reconnect ask and legacy leg, and the manual tune (io_control_set_freq()). It is what the scan scope says
 * (dsd_scan_mode_rigctl_request_is_session()), except that a typed -Y list (dsd_channel_modes_present()) configured on
 * @p state is a scan's before its first row's scope is entered too: that row is tuned, and the start ask made, before
 * the scope exists, and the list's rows ask for their own and its leave restore resets their readings. The untyped
 * list holds no scope and has no leave restore, so its requests are the session's. Without @p state the scope's answer
 * stands (any configured -Y list counts as a scan). 0 for NULL @p opts. Decoder thread. */
int dsd_channel_modes_rigctl_request_is_session(const dsd_opts* opts, const dsd_state* state);
#ifdef __cplusplus
}
#endif
#endif
