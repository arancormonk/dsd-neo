// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <dsd-neo/core/dsd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize any internal P25 trunking state (noop for now)
void p25_sm_init(dsd_opts* opts, dsd_state* state);

// Handle group voice channel grants (explicit form)
void p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src);

// Handle individual (unit-to-unit/telephone) voice channel grants (explicit form)
void p25_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src);

// Handle explicit release/end-of-call indications
void p25_sm_on_release(dsd_opts* opts, dsd_state* state);

// Optional neighbor/alternate CC update hook (frequencies in Hz)
void p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count);

// Optional periodic heartbeat (safety fallback)
void p25_sm_tick(dsd_opts* opts, dsd_state* state);

// Fetch next candidate CC frequency discovered from P25 neighbor/status PDUs.
// Returns 1 and writes to out_freq if available; returns 0 otherwise.
int p25_sm_next_cc_candidate(dsd_state* state, long* out_freq);

#ifdef __cplusplus
}
#endif
