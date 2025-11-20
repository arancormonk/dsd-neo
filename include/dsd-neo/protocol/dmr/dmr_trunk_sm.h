// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Minimal DMR Tier III trunking state machine API (aligned with P25 SM shape).
 * Centralizes VC tune and CC return decisions away from decoders.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize any internal DMR trunking state (noop for now)
void dmr_sm_init(dsd_opts* opts, dsd_state* state);

// Handle group voice grants. Prefer freq_hz when provided; otherwise resolve from LPCN.
void dmr_sm_on_group_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int tg, int src);

// Handle individual (unit-to-unit/telephone) voice grants. Prefer freq_hz when provided; otherwise resolve from LPCN.
void dmr_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int dst, int src);

// Explicit end/release (e.g., P_CLEAR) — will consider hang time and opposite slot activity in future iterations.
void dmr_sm_on_release(dsd_opts* opts, dsd_state* state);

// Optional neighbor/alternate CC update hook (frequencies in Hz)
void dmr_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count);

// Fetch next candidate CC frequency discovered from DMR neighbor/status PDUs.
// Returns 1 and writes to out_freq if available; returns 0 otherwise.
int dmr_sm_next_cc_candidate(dsd_state* state, long* out_freq);

#ifdef __cplusplus
}
#endif
