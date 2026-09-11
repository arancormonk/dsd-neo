// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Voice-gated scan for -Y and --trunk-scan (issue #381).
 *
 * The -Y scanner used to treat any sync as activity, so a repeater streaming
 * DMR IDLE/CSBK parked the scan indefinitely. This gate steps on unless decoded
 * voice frames hold it: qualify = window after sync in which voice must appear
 * or the scan moves on; hold = time to stay after the last voice frame.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SCAN_VOICE_GATE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SCAN_VOICE_GATE_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Policy-allowed decoded voice media visible to a scanner tick. */
typedef struct {
    /** Newest media time on an active call with media latched, or < 0 when none. */
    double active_media_m;
    /** Newest media time on an active or ended call, or < 0 when none. */
    double retained_media_m;
} dsd_scan_voice_probe_result;

/**
 * Probe both call slots for policy-allowed decoded voice media.
 *
 * The returned timestamps are raw call-state anchors and may predate the current
 * scanner visit or hold window. Callers must apply their own freshness bound.
 * Both result fields are initialized to -1. Returns -1 for invalid arguments,
 * 0 when no qualifying media exists, and 1 when retained media exists.
 */
int dsd_scan_voice_probe(const dsd_opts* opts, const dsd_state* state, dsd_scan_voice_probe_result* out);

/**
 * Non-zero when the operator's talkgroup hold is on a call that is being followed right now:
 * some slot carries an active, non-data call whose target -- after policy remapping -- is the
 * held talkgroup, or, on a private call, whose source is.
 *
 * Both scanners suspend the per-visit limit while this holds (issue #507), so the limit cannot
 * cut short the very call the hold exists to follow, and a fresh limit starts once that call
 * ends. Read-only and null-safe.
 */
int dsd_scan_tg_hold_call_active(const dsd_state* state);

/** Restart the per-visit gate memory on a scan hop. */
void dsd_scan_voice_gate_note_retune(dsd_state* state, double now_m);

/** Per-frame tick while parked on a -Y row; a no-op unless scanner_mode is on. */
void dsd_scan_voice_gate_tick(const dsd_opts* opts, dsd_state* state, int synced, double now_m);

/** Non-zero when the gate has a visit anchor and owns the scanner's step timing. */
int dsd_scan_voice_gate_owns_step(const dsd_opts* opts, const dsd_state* state);

/** Non-zero when the gate says the -Y visit is over and the scan should step. */
int dsd_scan_voice_gate_should_step(const dsd_opts* opts, const dsd_state* state, double now_m);

/** Take the scan timing publication back down (scan off, shutdown, between targets). */
void dsd_scan_timing_clear(dsd_state* state);

/** Publish one scan timing report for the row on air (issue #508). */
void dsd_scan_timing_publish(dsd_state* state, const dsd_scan_timing_publication* report);

/**
 * Publish the -Y scanner's stay reason and step deadline. A no-op unless
 * scanner_mode == 1 && trunk_scan_enabled != 1: under --trunk-scan the coordinator owns
 * the publication, exactly as it owns scan_voice_gate_phase.
 *
 * The legacy hangtime rule anchors on the wall-clock last_cc_sync_time (which NXDN can
 * stamp two seconds into the future), so the publisher takes both clocks, sampled
 * together, to express that anchor as a monotonic deadline the renderer can difference.
 */
void dsd_engine_scan_y_timing_tick(const dsd_opts* opts, dsd_state* state, double now_m, double now_wall_s);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SCAN_VOICE_GATE_H_H */
