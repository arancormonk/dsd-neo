// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief DMR trunking state-machine interfaces and constants.
 */
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

/**
 * @brief Initialize DMR trunking state (currently a no-op placeholder).
 *
 * Kept for symmetry with the P25 state machine entry points.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void dmr_sm_init(dsd_opts* opts, dsd_state* state);

/**
 * @brief Handle a group voice grant.
 *
 * Prefers an explicit `freq_hz` when provided; otherwise resolves the voice
 * channel from the LPCN using channel tables or defaults.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param freq_hz Voice channel frequency in Hz (0 to resolve from LPCN).
 * @param lpcn Logical channel number from the PDU.
 * @param tg Talkgroup ID.
 * @param src Source RID.
 */
void dmr_sm_on_group_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int tg, int src);

/**
 * @brief Handle an individual (unit-to-unit or telephone) voice grant.
 *
 * Prefers an explicit `freq_hz` when provided; otherwise resolves the voice
 * channel from the LPCN using channel tables or defaults.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param freq_hz Voice channel frequency in Hz (0 to resolve from LPCN).
 * @param lpcn Logical channel number from the PDU.
 * @param dst Destination RID.
 * @param src Source RID.
 */
void dmr_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, long freq_hz, int lpcn, int dst, int src);

/**
 * @brief Handle an explicit end/release message (e.g., P_CLEAR).
 *
 * Hang time and opposite slot activity may be considered by future iterations.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void dmr_sm_on_release(dsd_opts* opts, dsd_state* state);

/**
 * @brief Update neighbor/alternate control channel list (optional).
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param freqs Array of candidate CC frequencies in Hz.
 * @param count Number of entries in `freqs`.
 */
void dmr_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count);

/**
 * @brief Fetch the next candidate CC frequency discovered from DMR PDUs.
 *
 * @param state Decoder state holding candidate list.
 * @param out_freq [out] Receives the candidate CC in Hz when available.
 * @return 1 and writes out_freq when available; 0 when none pending.
 */
int dmr_sm_next_cc_candidate(dsd_state* state, long* out_freq);

#ifdef __cplusplus
}
#endif
