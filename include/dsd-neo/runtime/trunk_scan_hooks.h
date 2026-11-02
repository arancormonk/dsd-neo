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

#include <dsd-neo/core/dmr_key_map.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Operator-driven scan controls, routed from app_control to the coordinator.
 *
 * One op-coded slot rather than one per op: the coordinator is the only implementer and the
 * command queue the only caller, so the op keeps the table small while the return
 * codes below give the caller enough to choose a status message.
 */
typedef enum {
    DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE = 1,  /**< Pause/resume the dwell on the parked target */
    DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE = 2, /**< Avoid the parked target for the session and move on */
    DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR = 3,  /**< Put every avoided target back into the rotation */
    DSD_TRUNK_SCAN_CONTROL_ADVANCE = 4,      /**< Move to the next eligible target now */
    DSD_TRUNK_SCAN_CONTROL_RETREAT = 5,      /**< Move to the previous eligible target now (issue #666) */
} dsd_trunk_scan_control_op;

enum { DSD_TRUNK_KEY_MATERIAL = 1U, DSD_TRUNK_KEY_MAP = 2U, DSD_TRUNK_KEY_FORCE = 4U };

enum {
    DSD_TRUNK_KEY_APPLIED = 1,
    DSD_TRUNK_KEY_INVALID = -1,
    DSD_TRUNK_KEY_STALE = -2,
    DSD_TRUNK_KEY_UNAVAILABLE = -3,
    DSD_TRUNK_KEY_BUSY = -4
};

enum {
    DSD_TRUNK_SCAN_CONTROL_ALREADY_AVOIDED = -7, /**< AVOID_ACTIVE on a target already avoided (the parked fallback) */
    DSD_TRUNK_SCAN_CONTROL_NOT_AVOIDED = -6,     /**< Unavoid: the target named is not avoided (already put back) */
    DSD_TRUNK_SCAN_CONTROL_STALE = -5,           /**< Unavoid: no such target in this scan session */
    DSD_TRUNK_SCAN_CONTROL_PAUSED = -4,          /**< The scan is paused (issue #671): nothing to hold, avoid or step */
    DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE = -3,     /**< Trunk scan is not installed */
    DSD_TRUNK_SCAN_CONTROL_BUSY = -2,            /**< The coordinator's tick guard is held; try again */
    DSD_TRUNK_SCAN_CONTROL_REFUSED = -1, /**< Would leave no usable target, a single-target step, or unknown op */
    DSD_TRUNK_SCAN_CONTROL_OK = 0,       /**< Unavoid: the target is back in the rotation */
    /* >= 0 is op-specific: HOLD_TOGGLE returns the new hold state; AVOID_ACTIVE, ADVANCE and
     * RETREAT return 0 when the receiver moved and 1 when it stayed; AVOID_CLEAR returns how
     * many avoids it cleared. */
};

/**
 * @brief What names a --trunk-scan target to a person (issue #672): its 1-based position in the target list, as the
 * avoid list leads with it, and its label column ("" when it has none). The id alone names nothing on Android, whose
 * target ids are entry UIDs.
 */
typedef struct {
    uint32_t ordinal;
    char label[64]; /**< dsd_trunk_scan_target::label, NUL-terminated */
} dsd_trunk_scan_target_name;

/**
 * @brief What switching a trunk-scan target list on or off did (dsd_trunk_scan_hook_list_set(), issue #697).
 *
 * Its own codes, apart from the control ops' above: a list toggle names a session and a list, so it can be stale.
 */
typedef enum {
    DSD_TRUNK_SCAN_LIST_SET_OK = 0,            /**< The list is switched as asked */
    DSD_TRUNK_SCAN_LIST_SET_UNCHANGED = 1,     /**< It already was (every list, for DSD_TRUNK_SCAN_LIST_ALL) */
    DSD_TRUNK_SCAN_LIST_SET_REFUSED_LAST = -1, /**< Switching it off would leave nothing to scan */
    DSD_TRUNK_SCAN_LIST_SET_BUSY = -2,         /**< The coordinator's tick guard is held; try again */
    DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE = -3,  /**< Trunk scan is not installed */
    DSD_TRUNK_SCAN_LIST_SET_PAUSED = -4,       /**< The scan is paused (issue #671): resume it first */
    DSD_TRUNK_SCAN_LIST_SET_STALE = -5,        /**< The session is not this scan's, or it has no such list */
} dsd_trunk_scan_list_set_result;

/** The list dsd_trunk_scan_hook_list_set() takes for "every list": switched on, all of them at once. */
enum { DSD_TRUNK_SCAN_LIST_ALL = -1 };

typedef struct {
    void* (*p25_ctx)(void);
    void* (*dmr_ctx)(void);
    void (*tick)(dsd_opts* opts, dsd_state* state);
    void (*dmr_conventional_activity)(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                                      int is_private, int encrypted, int data_call);
    void (*nxdn_conventional_activity)(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                                       int is_private, int encrypted, int data_call);
    void (*p25_conventional_activity)(const dsd_opts* opts, const dsd_state* state, int protocol, int slot,
                                      uint32_t target, uint32_t source, int is_private, int encrypted, int data_call);
    const char* (*active_chan_csv)(const dsd_state* state);
    void (*enc_lockout_clear_snapshots)(const dsd_state* state);
    int (*control)(dsd_opts* opts, dsd_state* state, int op);
    int (*unavoid)(const dsd_opts* opts, dsd_state* state, uint32_t session, const char* target_id,
                   dsd_trunk_scan_target_name* name);
    int (*list_set)(dsd_opts* opts, dsd_state* state, uint32_t session, int list, int enabled, uint32_t request);
    int (*decryption_apply)(dsd_opts* opts, dsd_state* state, const char* target_id, uint64_t generation,
                            uint32_t fields, const dsd_key_set* keys, const dsd_dmr_key_map* map, int force);
} dsd_trunk_scan_hooks;

/** Install a copy of @p hooks; NULL clears every slot. */
void dsd_trunk_scan_hooks_set(const dsd_trunk_scan_hooks* hooks);
int dsd_trunk_scan_hook_decryption_apply(dsd_opts* opts, dsd_state* state, const char* target_id, uint64_t generation,
                                         uint32_t fields, const dsd_key_set* keys, const dsd_dmr_key_map* map,
                                         int force);

void* dsd_trunk_scan_hook_p25_ctx(void);
void* dsd_trunk_scan_hook_dmr_ctx(void);

typedef enum {
    DSD_TRUNK_RECOVERY_UNKNOWN = 0,
    DSD_TRUNK_RECOVERY_P25,
    DSD_TRUNK_RECOVERY_DMR,
    DSD_TRUNK_RECOVERY_NXDN,
} dsd_trunk_recovery_protocol;

/** Publish validated protocol evidence under the decoder/SM guard (or while stopped).
 * Only P25/DMR control or grant evidence, and NXDN control (a CRC-good CAC or a confirmed Type-D SCCH), transfers
 * ownership. Other protocol decodes,
 * raw sync detection and no-carrier resets must not change this retained owner. */
void dsd_trunk_recovery_note_protocol(dsd_state* state, dsd_trunk_recovery_protocol protocol);

/** Recovery ownership: a parked target overrides retained standalone ownership.
 * Decoder-thread callers may inspect their retained owner directly; other threads
 * must hold the decoder/SM guard. These predicates do not read volatile
 * sync, modulation or CC hints written outside that guard by frame acquisition. */
int dsd_trunk_p25_recovery_allowed(const dsd_opts* opts, const dsd_state* state);
int dsd_trunk_dmr_recovery_allowed(const dsd_opts* opts, const dsd_state* state);
/** NXDN control-channel recovery (src/protocol/nxdn/nxdn_cc_hunt.c): trunking on, neither -Y nor --trunk-scan, NXDN48
 * or NXDN96 decoded, and a validated NXDN owner or, with none yet, a frame set without P25, DMR or ProVoice. */
int dsd_trunk_nxdn_recovery_allowed(const dsd_opts* opts, const dsd_state* state);

/** Whether P25 runs the startup control-channel hunt of a single-system trunked session that no protocol has proven
 * yet (issue #703): trunking on, neither -Y nor --trunk-scan, no validated owner, P25 decoded, and no selected EDACS
 * session (dsd_edacs_session(): EDACS control names no owner, so a P25 window would hunt away from a valid EDACS
 * control channel, as under -fh -fa). DMR and NXDN alongside (-ft, Auto) are fine: their validated
 * control (a DMR control message, an NXDN CAC or confirmed Type-D SCCH) names its owner and ends the hunt, and their
 * validated traffic holds its window. This grants no recovery ownership; the decoder thread's startup hunt, the
 * operator-move rule of a frequency entry and the probe profile choice read it. That choice
 * (dsd_engine_cc_is_p25()) reads it only while the P25 machine hunts (p25_sm_hunt_probe_in_flight()): another
 * control-channel tune of the session, such as the engine's no-carrier return, keeps the sync rule. */
int dsd_trunk_p25_cold_start_presumed(const dsd_opts* opts, const dsd_state* state);
void dsd_trunk_scan_hook_tick(dsd_opts* opts, dsd_state* state);
/**
 * @brief Report decoded conventional DMR/NXDN/P25 activity to the scan coordinator.
 *
 * Lets protocol code refresh the parked target's activity hold without
 * depending on engine-owned scan headers. No-op when trunk scan is not
 * installed, and ignored by the coordinator unless the parked target is of the
 * matching conventional type. Only call these with identity that has already
 * cleared the protocol's FEC/CRC gate, and pass the corroborated encryption
 * classification rather than a single frame's raw cipher field. One deliberate
 * exception: under -F a DMR voice LC accepted as RAS counts as checked,
 * because RAS systems never let the standard CRC verify and -F opts
 * into that acceptance; without it a scanned RAS repeater would never hold.
 * That exception is the scan hold's alone: a signal search hears the protocol's
 * confirmation from every report, but takes its target and source only while
 * the reporting slot's dsd_state::event_crc_invalid is clear, so a RAS-accepted
 * or failed-CRC identity never names a detection.
 */
void dsd_trunk_scan_hook_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                   uint32_t source, int is_private, int encrypted, int data_call);
/** @copydoc dsd_trunk_scan_hook_dmr_conventional_activity */
void dsd_trunk_scan_hook_nxdn_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                    uint32_t source, int is_private, int encrypted, int data_call);
/** @copydoc dsd_trunk_scan_hook_dmr_conventional_activity
 * P25 carries Phase 1 and Phase 2 conventional traffic, so the report names its call's protocol (a DSD_SYNC_P25*
 * value) and slot (0 for Phase 1, 0/1 for Phase 2) for the receive policy. */
void dsd_trunk_scan_hook_p25_conventional_activity(const dsd_opts* opts, const dsd_state* state, int protocol, int slot,
                                                   uint32_t target, uint32_t source, int is_private, int encrypted,
                                                   int data_call);

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
 * otherwise the coordinator's result (see the enum above): a paused scan takes AVOID_CLEAR
 * alone. Call it on the decoder
 * thread, the way the command queue does: it may retune.
 */
int dsd_trunk_scan_hook_control(dsd_opts* opts, dsd_state* state, int op);

/**
 * @brief Put one avoided target back into the rotation (issue #672), named by its identity: the scan session
 * (dsd_state::trunk_scan_session, nonzero) and the target's id.
 *
 * Only the target's avoid flag changes. Nothing retunes, and the hold, every cooldown and the dwell stay as they are;
 * the visit cap restarts only when the target is the first eligible alternate again, as AVOID_CLEAR's does. Allowed
 * while the scan is paused (the timing stays cleared then). Returns DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE when trunk
 * scan is not installed; otherwise DSD_TRUNK_SCAN_CONTROL_OK, DSD_TRUNK_SCAN_CONTROL_STALE when the session is not
 * this scan's or has no such target, or DSD_TRUNK_SCAN_CONTROL_NOT_AVOIDED when the target is not avoided. On
 * DSD_TRUNK_SCAN_CONTROL_OK and DSD_TRUNK_SCAN_CONTROL_NOT_AVOIDED, @p name (may be NULL) receives the target's
 * position and label for the caller's message; otherwise it is left zeroed. Decoder thread only, like
 * dsd_trunk_scan_hook_control().
 */
int dsd_trunk_scan_hook_unavoid(const dsd_opts* opts, dsd_state* state, uint32_t session, const char* target_id,
                                dsd_trunk_scan_target_name* name);

/**
 * @brief Switch trunk-scan target list @p list (1-based, CLI/INI order) on or off for the session (issue #697), or
 * every list on (DSD_TRUNK_SCAN_LIST_ALL with @p enabled 1).
 *
 * @p session names the scan as the frontend saw it (dsd_state::trunk_scan_session, the session a put back names too,
 * kept while paused): a request made of one scan never lands on the next. Switching off the list that holds the target on air leaves that target at once, as `b`
 * and `L` do (its call ends and commits under its own name, its carrier is handed back, the hold carries over); when
 * no other target can take the receiver yet, the scan leaves as soon as one can. A switch that would leave no target
 * to scan is refused, and so is any while the scan is paused. Nothing is written to a list file or a config. @p request
 * is the caller's id for the request, which the caller matches the result to; the switch is the same whoever asks.
 *
 * Returns a dsd_trunk_scan_list_set_result; DSD_TRUNK_SCAN_LIST_SET_UNAVAILABLE when trunk scan is not installed.
 * Decoder thread only, like dsd_trunk_scan_hook_control(): it may retune. It takes the P25 SM tick guard (try-enter,
 * BUSY when held) itself, so the caller must not hold it.
 */
int dsd_trunk_scan_hook_list_set(dsd_opts* opts, dsd_state* state, uint32_t session, int list, int enabled,
                                 uint32_t request);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_SCAN_HOOKS_H_ */
