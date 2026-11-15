// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO_GATE_INTERNAL_H
#define DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO_GATE_INTERNAL_H

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>

#include <stdint.h>

/*
 * The talkgroup part of the media gates' audio verdict on @p decision: a matching Hold that overrides the talkgroup
 * policy (never a session avoid or call skip), or the talkgroup part's audio without an allow-list denial. The gates
 * play a call when this and the radio part (`radio_audio_allowed`) allow it; the P25 patch resolver
 * (<dsd-neo/core/p25_patch_policy.h>) ranks and admits members for media on it. 0 for NULL.
 */
int dsd_audio_policy_tg_allows_audio(const dsd_tg_policy_decision* decision);

/*
 * The rdio-scanner export verdict for the recording of @p call (issue #773 D7): 1 when the talkgroup and radio policy
 * let it be exported, 0 when a `stream` flag that applies is off.
 *
 * The call is judged as the media gates judge it (dsd_audio_record_policy_gate_slot()): a private call on both its
 * parties, a group call on its policy target with the over-the-air target's final blocks
 * (dsd_tg_policy_apply_ota_final_blocks()), then the radio part. A Hold lifts what it lifts for recording -- an
 * allow-list miss, and a stream flag only derived off (a B/DE or audio=off row) -- but never an explicit stream=off on
 * an audible talkgroup row, nor a radio row's stream=off. @p policy_target, when non-zero, is the talkgroup a group
 * call is judged on; 0 resolves it as the gates do: an active P25 call's keep-or-resolve target
 * (dsd_p25_patch_media_resolve(): its stored target while it may play, else the patch resolver's answer
 * for media), else the call's policy target, else its over-the-air target. @p judged_target receives the
 * talkgroup the call was judged on (a private call's over-the-air target), 0 when it was not judged.
 *
 * Recordings the talkgroup policy never gates are not judged and export: conventional analog calls, M17, and calls
 * whose identities do not fit 32 bits.
 */
int dsd_audio_stream_export_allowed(const dsd_opts* opts, const dsd_state* state, const dsd_call_snapshot* call,
                                    uint32_t policy_target, uint32_t* judged_target);

#endif /* DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO_GATE_INTERNAL_H */
