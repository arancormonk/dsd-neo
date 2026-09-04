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
 * Both result fields are initialized to -1. Returns -1 for invalid arguments,
 * 0 when no qualifying media exists, and 1 when retained media exists.
 */
int dsd_scan_voice_probe(const dsd_opts* opts, const dsd_state* state, dsd_scan_voice_probe_result* out);

/** Restart the per-visit gate memory on a scan hop. */
void dsd_scan_voice_gate_note_retune(dsd_state* state, double now_m);

/** Per-frame tick while parked on a -Y row; a no-op unless scanner_mode is on. */
void dsd_scan_voice_gate_tick(const dsd_opts* opts, dsd_state* state, int synced, double now_m);

/** Non-zero when the gate says the -Y visit is over and the scan should step. */
int dsd_scan_voice_gate_should_step(const dsd_opts* opts, const dsd_state* state, double now_m);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SCAN_VOICE_GATE_H_H */
