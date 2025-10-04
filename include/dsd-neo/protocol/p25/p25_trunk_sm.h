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

// --- Patch group (P25 regroup/patch) tracking helpers ---
// Record or update a P25 regroup/patch state for a Super Group ID (SGID).
// is_patch: 1=two-way patch, 0=simulselect (one-way regroup)
// active: 1=activated, 0=deactivated/cleared
void p25_patch_update(dsd_state* state, int sgid, int is_patch, int active);

// Compose a compact summary string for active patch SGIDs (e.g., "P: 069,142").
// Returns length written; writes empty string when none active.
int p25_patch_compose_summary(const dsd_state* state, char* out, size_t cap);

// Add a Working Group ID to an SGID entry (creates/activates entry if needed)
void p25_patch_add_wgid(dsd_state* state, int sgid, int wgid);

// Add a Working Unit ID to an SGID entry (creates/activates entry if needed)
void p25_patch_add_wuid(dsd_state* state, int sgid, uint32_t wuid);

// Compose a more detailed status string, including WGID/WUID counts or samples.
// Example: "SG069[P] WG:2(0345,0789); SG142[S] U:3"
int p25_patch_compose_details(const dsd_state* state, char* out, size_t cap);

// Remove membership or clear entire SG record
void p25_patch_remove_wgid(dsd_state* state, int sgid, int wgid);
void p25_patch_remove_wuid(dsd_state* state, int sgid, uint32_t wuid);
void p25_patch_clear_sg(dsd_state* state, int sgid);

// Set optional Key/Alg/SSN context for an SG
void p25_patch_set_kas(dsd_state* state, int sgid, int key, int alg, int ssn);

// Return 1 if TG is a WGID within an active SG whose explicitly signaled KEY
// is 0 (clear). Used to override ENC lockout when Harris GRG commands state
// clear operation for a patch/regroup.
int p25_patch_tg_key_is_clear(const dsd_state* state, int tg);
// Return 1 if an SGID has explicit KEY=0 (clear) policy and is active.
int p25_patch_sg_key_is_clear(const dsd_state* state, int sgid);

// --- Affiliation (RID) tracking ---
// Record a RID as affiliated/registered on this system (updates last_seen or adds new entry).
void p25_aff_register(dsd_state* state, uint32_t rid);

// Remove a RID from the affiliation table (on explicit deregistration or aging).
void p25_aff_deregister(dsd_state* state, uint32_t rid);

// Periodic aging/cleanup of affiliation table. Safe to call at 1 Hz from SM tick.
void p25_aff_tick(dsd_state* state);

// Group Affiliation (RID ↔ TG) helpers
void p25_ga_add(dsd_state* state, uint32_t rid, uint16_t tg);
void p25_ga_remove(dsd_state* state, uint32_t rid, uint16_t tg);
void p25_ga_tick(dsd_state* state);

// Emit a single encryption lockout event for a talkgroup.
// Marks the TG as encrypted (mode "DE") if not already and pushes the
// corresponding event to history/log exactly once per TG until scrubbed.
// slot: 0 for FDMA/left, 1 for TDMA/right; tg: talkgroup; svc_bits: service bits (optional, pass 0 if unknown).
void p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc_bits);

#ifdef __cplusplus
}
#endif
