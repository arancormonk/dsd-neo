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

/**
 * @brief Operator-driven scan controls, routed from app_control to the coordinator.
 *
 * One op-coded slot rather than four: the coordinator is the only implementer and the
 * command queue the only caller, so the op keeps the table small while the return
 * codes below give the caller enough to choose a status message.
 */
typedef enum {
    DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE = 1,  /**< Pause/resume the dwell on the parked target */
    DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE = 2, /**< Avoid the parked target for the session and move on */
    DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR = 3,  /**< Put every avoided target back into the rotation */
    DSD_TRUNK_SCAN_CONTROL_ADVANCE = 4,      /**< Move to the next eligible target now */
} dsd_trunk_scan_control_op;

enum {
    DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE = -3, /**< Trunk scan is not installed */
    DSD_TRUNK_SCAN_CONTROL_BUSY = -2,        /**< The coordinator's tick guard is held; try again */
    DSD_TRUNK_SCAN_CONTROL_REFUSED = -1,     /**< Would leave no usable target, or nothing to do */
    /* >= 0 is op-specific: HOLD_TOGGLE returns the new hold state; AVOID_ACTIVE and ADVANCE
     * return 0 when the receiver moved and 1 when it stayed; AVOID_CLEAR returns how many
     * avoids it cleared. */
};

typedef struct {
    void* (*p25_ctx)(void);
    void* (*dmr_ctx)(void);
    void (*tick)(dsd_opts* opts, dsd_state* state);
    void (*dmr_conventional_activity)(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                                      int is_private, int encrypted, int data_call);
    void (*nxdn_conventional_activity)(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                                       int is_private, int encrypted, int data_call);
    const char* (*active_chan_csv)(const dsd_state* state);
    void (*enc_lockout_clear_snapshots)(const dsd_state* state);
    int (*control)(dsd_opts* opts, dsd_state* state, int op);
} dsd_trunk_scan_hooks;

void dsd_trunk_scan_hooks_set(dsd_trunk_scan_hooks hooks);

void* dsd_trunk_scan_hook_p25_ctx(void);
void* dsd_trunk_scan_hook_dmr_ctx(void);
void dsd_trunk_scan_hook_tick(dsd_opts* opts, dsd_state* state);
/**
 * @brief Report decoded conventional DMR/NXDN activity to the scan coordinator.
 *
 * Lets protocol code refresh the parked target's activity hold without
 * depending on engine-owned scan headers. No-op when trunk scan is not
 * installed, and ignored by the coordinator unless the parked target is of the
 * matching conventional type. Only call these with identity that has already
 * cleared the protocol's FEC/CRC gate, and pass the corroborated encryption
 * classification rather than a single frame's raw cipher field.
 */
void dsd_trunk_scan_hook_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                   uint32_t source, int is_private, int encrypted, int data_call);
/** @copydoc dsd_trunk_scan_hook_dmr_conventional_activity */
void dsd_trunk_scan_hook_nxdn_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                    uint32_t source, int is_private, int encrypted, int data_call);

/**
 * @brief Path of the channel map belonging to the currently parked scan target.
 *
 * Trunk scan rejects a global `-C` channel map and loads each target's `chan_csv` through
 * throwaway options, so `opts->chan_in_file` is empty while scanning and protocol code cannot
 * name the map it is missing entries from. Returns NULL when trunk scan is not installed or the
 * parked target has no channel map.
 */
const char* dsd_trunk_scan_hook_active_chan_csv(const dsd_state* state);

/**
 * @brief Drop the encrypted-target lockout ledger held in every scan-target snapshot.
 *
 * dsd_enc_lockout_clear_all() only purges the live ledger; each trunk-scan
 * target parks its own copy, so a user purge must scrub those too or switching
 * targets restores the entries the user just forgot. No-op when trunk scan is
 * not installed.
 */
void dsd_trunk_scan_hook_enc_lockout_clear_snapshots(const dsd_state* state);

/**
 * @brief Apply one dsd_trunk_scan_control_op to the parked target list.
 *
 * Returns DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE when trunk scan is not installed;
 * otherwise the coordinator's result (see the enum above). Call it on the decoder
 * thread, the way the command queue does: it may retune.
 */
int dsd_trunk_scan_hook_control(dsd_opts* opts, dsd_state* state, int op);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_SCAN_HOOKS_H_ */
