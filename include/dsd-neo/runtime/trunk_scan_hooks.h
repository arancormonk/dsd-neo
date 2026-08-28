// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for single-tuner trunk scan coordination.
 *
 * Protocol code can report periodic scan ticks and decoded conventional DMR
 * and NXDN activity without depending on engine-owned scan coordinator headers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_SCAN_HOOKS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_SCAN_HOOKS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void* (*p25_ctx)(void);
    void* (*dmr_ctx)(void);
    void (*tick)(dsd_opts* opts, dsd_state* state);
    void (*dmr_conventional_activity)(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                                      int is_private, int encrypted, int data_call);
    void (*nxdn_conventional_activity)(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                                       int is_private, int encrypted, int data_call);
    void (*enc_lockout_clear_snapshots)(const dsd_state* state);
} dsd_trunk_scan_hooks;

void dsd_trunk_scan_hooks_set(dsd_trunk_scan_hooks hooks);

void* dsd_trunk_scan_hook_p25_ctx(void);
void* dsd_trunk_scan_hook_dmr_ctx(void);
void dsd_trunk_scan_hook_tick(dsd_opts* opts, dsd_state* state);
void dsd_trunk_scan_hook_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                   uint32_t source, int is_private, int encrypted, int data_call);
void dsd_trunk_scan_hook_nxdn_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                    uint32_t source, int is_private, int encrypted, int data_call);

/**
 * @brief Drop the encrypted-target lockout ledger held in every scan-target snapshot.
 *
 * dsd_enc_lockout_clear_all() only purges the live ledger; each trunk-scan
 * target parks its own copy, so a user purge must scrub those too or switching
 * targets restores the entries the user just forgot. No-op when trunk scan is
 * not installed.
 */
void dsd_trunk_scan_hook_enc_lockout_clear_snapshots(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_SCAN_HOOKS_H_ */
