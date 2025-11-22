// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief P25 trunking state-machine interfaces and constants.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// High-level trunk SM mode (for UI/telemetry)
typedef enum {
    DSD_P25_SM_MODE_UNKNOWN = 0,
    DSD_P25_SM_MODE_ON_CC = 1,
    DSD_P25_SM_MODE_ON_VC = 2,
    DSD_P25_SM_MODE_HANG = 3,
    DSD_P25_SM_MODE_HUNTING = 4,
    // Extended states for richer UI/telemetry across P1/P2
    DSD_P25_SM_MODE_ARMED = 5,     // tuned to VC, awaiting PTT/ACTIVE
    DSD_P25_SM_MODE_FOLLOW = 6,    // following active voice
    DSD_P25_SM_MODE_RETURNING = 7, // teardown in progress back to CC
} dsd_p25_sm_mode_e;

/**
 * @brief Initialize any internal P25 trunking state (currently a no-op placeholder).
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_init(dsd_opts* opts, dsd_state* state);

/**
 * @brief Handle a group voice channel grant (explicit form).
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param channel Voice channel number.
 * @param svc_bits Service options associated with the grant.
 * @param tg Talkgroup.
 * @param src Source RID.
 */
void p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src);

/**
 * @brief Handle an individual (unit-to-unit/telephone) voice channel grant.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param channel Voice channel number.
 * @param svc_bits Service options associated with the grant.
 * @param dst Destination RID.
 * @param src Source RID.
 */
void p25_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src);

/**
 * @brief Handle an explicit release/end-of-call indication.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_on_release(dsd_opts* opts, dsd_state* state);

/**
 * @brief Update neighbor/alternate control channel list (optional).
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param freqs Array of candidate CC frequencies in Hz.
 * @param count Number of entries in `freqs`.
 */
void p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count);

/**
 * @brief Optional periodic heartbeat/tick for safety fallback.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_tick(dsd_opts* opts, dsd_state* state);

/**
 * @brief Fetch next candidate CC frequency discovered from neighbor/status PDUs.
 *
 * @param state Decoder state.
 * @param out_freq [out] Receives the candidate CC in Hz when available.
 * @return 1 and writes out_freq when available; 0 when none pending.
 */
int p25_sm_next_cc_candidate(dsd_state* state, long* out_freq);

// --- Patch group (P25 regroup/patch) tracking helpers ---
/**
 * @brief Record or update a P25 regroup/patch state for a Super Group ID (SGID).
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param is_patch 1 for two-way patch, 0 for simulselect (one-way regroup).
 * @param active 1 to activate, 0 to deactivate/clear.
 */
void p25_patch_update(dsd_state* state, int sgid, int is_patch, int active);

/**
 * @brief Compose a compact summary string for active patch SGIDs.
 *
 * Example output: "P: 069,142".
 *
 * @param state Decoder state (read-only).
 * @param out Destination buffer for summary string.
 * @param cap Capacity of destination buffer.
 * @return Length written (0 when none active).
 */
int p25_patch_compose_summary(const dsd_state* state, char* out, size_t cap);

/**
 * @brief Add a Working Group ID to an SGID entry (creates/activates if needed).
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param wgid Working Group ID to add.
 */
void p25_patch_add_wgid(dsd_state* state, int sgid, int wgid);

/**
 * @brief Add a Working Unit ID to an SGID entry (creates/activates if needed).
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param wuid Working Unit ID to add.
 */
void p25_patch_add_wuid(dsd_state* state, int sgid, uint32_t wuid);

/**
 * @brief Compose a detailed status string including WGID/WUID context.
 *
 * Example: "SG069[P] WG:2(0345,0789); SG142[S] U:3".
 *
 * @param state Decoder state (read-only).
 * @param out Destination buffer.
 * @param cap Capacity of destination buffer.
 * @return Length written.
 */
int p25_patch_compose_details(const dsd_state* state, char* out, size_t cap);

/**
 * @brief Remove a WGID membership or clear the entire SG record.
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param wgid Working Group ID to remove.
 */
void p25_patch_remove_wgid(dsd_state* state, int sgid, int wgid);
/**
 * @brief Remove a WUID membership from an SG record.
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param wuid Working Unit ID to remove.
 */
void p25_patch_remove_wuid(dsd_state* state, int sgid, uint32_t wuid);
/**
 * @brief Clear all membership and status for an SG record.
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID to clear.
 */
void p25_patch_clear_sg(dsd_state* state, int sgid);

/**
 * @brief Set optional Key/Alg/SSN context for an SG.
 *
 * Values of -1 leave the existing field unchanged.
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param key Key ID (or -1 to leave unchanged).
 * @param alg Algorithm ID (or -1 to leave unchanged).
 * @param ssn SSN ID (or -1 to leave unchanged).
 */
void p25_patch_set_kas(dsd_state* state, int sgid, int key, int alg, int ssn);

/**
 * @brief Return 1 if TG is a WGID within an active SG that is explicitly clear.
 *
 * Used to override ENC lockout when GRG commands indicate a clear operation
 * for a patch/regroup.
 *
 * @param state Decoder state (read-only).
 * @param tg Talkgroup to test.
 * @return 1 if the TG maps to a clear SG, 0 otherwise.
 */
int p25_patch_tg_key_is_clear(const dsd_state* state, int tg);
/**
 * @brief Return 1 if an SGID has explicit KEY=0 (clear) policy and is active.
 *
 * @param state Decoder state (read-only).
 * @param sgid Super Group ID to query.
 * @return 1 if active and explicitly clear; 0 otherwise.
 */
int p25_patch_sg_key_is_clear(const dsd_state* state, int sgid);

// --- Affiliation (RID) tracking ---
/**
 * @brief Record a RID as affiliated/registered (updates last_seen or adds new entry).
 *
 * @param state Decoder state holding affiliation table.
 * @param rid Radio ID to register.
 */
void p25_aff_register(dsd_state* state, uint32_t rid);

/**
 * @brief Remove a RID from the affiliation table (explicit deregistration or aging).
 *
 * @param state Decoder state holding affiliation table.
 * @param rid Radio ID to remove.
 */
void p25_aff_deregister(dsd_state* state, uint32_t rid);

/**
 * @brief Periodic aging/cleanup of the affiliation table (call at ~1 Hz).
 *
 * @param state Decoder state holding affiliation table.
 */
void p25_aff_tick(dsd_state* state);

// Group Affiliation (RID ↔ TG) helpers
/**
 * @brief Add a group affiliation (RID to TG).
 *
 * @param state Decoder state holding group affiliation table.
 * @param rid Radio ID.
 * @param tg Talkgroup to associate.
 */
void p25_ga_add(dsd_state* state, uint32_t rid, uint16_t tg);
/**
 * @brief Remove a group affiliation (RID to TG).
 *
 * @param state Decoder state holding group affiliation table.
 * @param rid Radio ID.
 * @param tg Talkgroup to remove.
 */
void p25_ga_remove(dsd_state* state, uint32_t rid, uint16_t tg);
/**
 * @brief Age/cleanup group affiliation entries (call at ~1 Hz).
 *
 * @param state Decoder state holding group affiliation table.
 */
void p25_ga_tick(dsd_state* state);

/**
 * @brief Emit a single encryption lockout event for a talkgroup.
 *
 * Marks the TG as encrypted (mode "DE") if not already and pushes the
 * corresponding event to history/log exactly once per TG until scrubbed.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param slot 0 for FDMA/left, 1 for TDMA/right.
 * @param tg Talkgroup.
 * @param svc_bits Optional service bits (pass 0 if unknown).
 */
void p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc_bits);

#ifdef __cplusplus
}
#endif
