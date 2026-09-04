// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_TRUNK_DIAG_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_TRUNK_DIAG_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Channels observed without a frequency mapping, as a movable value.
 *
 * The live ledger hangs off `dsd_state`, but single-tuner trunk scan rotates several NXDN
 * targets through one decoder state, so the coordinator parks each target's ledger alongside the
 * rest of that target's snapshot.
 */
typedef struct {
    uint8_t missing_seen[(0xFFFFu + 7u) / 8u];
    uint32_t missing_unique;
} nxdn_trunk_diag_ledger;

/**
 * @brief Copy the ledger held in @p state, or zero @p out when there is none.
 */
void nxdn_trunk_diag_ledger_save(const dsd_state* state, nxdn_trunk_diag_ledger* out);

/**
 * @brief Install @p ledger as the ledger held in @p state, replacing any current one.
 *
 * An empty ledger against a state that has none is a no-op, so rotating past targets that never
 * decoded a grant costs no allocation.
 */
void nxdn_trunk_diag_ledger_restore(dsd_state* state, const nxdn_trunk_diag_ledger* ledger);

/**
 * @brief Path of the channel map the diagnostics should report against, or NULL when there is none.
 *
 * A global `-C` map wins wherever it is set; under trunk scan the parked target's `chan_csv`
 * answers instead, since the global option is rejected there.
 */
const char* nxdn_trunk_diag_chan_map_path(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Log an exit summary for one ledger/channel-map pair.
 *
 * Lets the scan coordinator report each parked target's own missing channels from its snapshot.
 * No-op without a channel map path or when nothing is missing.
 *
 * @p lookup resolves a channel number to its mapped frequency, or 0 when the channel is still
 * unmapped, and receives @p ctx unchanged. Taking a callback rather than a dense 64K-entry array
 * lets a caller that stores its channel map sparsely answer the question without materialising
 * one; pass nxdn_trunk_diag_dense_chan_lookup with a `const long int*` for a dense map.
 */
typedef long int (*nxdn_trunk_diag_chan_freq_fn)(const void* ctx, uint16_t channel);

/** nxdn_trunk_diag_chan_freq_fn over a dense `const long int[DSD_TRUNK_CHAN_MAP_SIZE]` map. */
long int nxdn_trunk_diag_dense_chan_lookup(const void* ctx, uint16_t channel);

void nxdn_trunk_diag_log_summary_for(const char* chan_csv, const nxdn_trunk_diag_ledger* ledger,
                                     nxdn_trunk_diag_chan_freq_fn lookup, const void* ctx);

/**
 * @brief Record that a trunked NXDN channel had no known frequency mapping.
 *
 * This is deduplicated per-channel for the lifetime of @p state.
 *
 * @return 1 if the channel was newly recorded, 0 otherwise.
 */
int nxdn_trunk_diag_note_missing_channel(dsd_state* state, uint16_t channel);

/**
 * @brief Collect channels that were observed missing and are still unmapped.
 *
 * @param state    Decoder state.
 * @param out      Optional output array (may be NULL).
 * @param out_cap  Capacity of @p out (0 allowed).
 *
 * @return Total number of channels still unmapped (may be > out_cap).
 */
size_t nxdn_trunk_diag_collect_unmapped_channels(const dsd_state* state, uint16_t* out, size_t out_cap);

/**
 * @brief Log a rate-limited notice for a missing channel mapping.
 *
 * Logs only once per channel for the lifetime of @p state, and only when
 * a `chan_csv` was configured.
 */
void nxdn_trunk_diag_log_missing_channel_once(const dsd_opts* opts, dsd_state* state, uint16_t channel,
                                              const char* context);

/**
 * @brief Log an exit summary of channels still missing from the channel map.
 */
void nxdn_trunk_diag_log_summary(const dsd_opts* opts, const dsd_state* state);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_TRUNK_DIAG_H_H */
