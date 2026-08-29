// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine-owned single-tuner trunk scan coordinator.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_TRUNK_SCAN_DWELL_MIN_MS = 250,
    DSD_TRUNK_SCAN_DWELL_MAX_MS = 600000,
    DSD_TRUNK_SCAN_IDLE_DWELL_DEFAULT_MS = 3000,
    DSD_TRUNK_SCAN_ACTIVITY_HOLD_DEFAULT_MS = 1200,
};

typedef enum {
    DSD_TRUNK_SCAN_TARGET_P25_TRUNK = 0,
    DSD_TRUNK_SCAN_TARGET_DMR_TRUNK = 1,
    DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL = 2,
    DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK = 3,
    DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL = 4,
    DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL = 5,
} dsd_trunk_scan_target_type;

typedef enum {
    DSD_TRUNK_SCAN_MODULATION_UNSET = 0,
    DSD_TRUNK_SCAN_MODULATION_AUTO = 1,
    DSD_TRUNK_SCAN_MODULATION_C4FM = 2,
    DSD_TRUNK_SCAN_MODULATION_CQPSK = 3,
    DSD_TRUNK_SCAN_MODULATION_GFSK = 4,
} dsd_trunk_scan_modulation;

typedef struct {
    char id[64];
    dsd_trunk_scan_target_type type;
    uint32_t frequency_hz;
    char chan_csv[1024];
    int dwell_ms;
    int activity_hold_ms;
    dsd_trunk_scan_modulation modulation;
    int rtl_gain_is_set;
    int rtl_gain_db;
} dsd_trunk_scan_target;

typedef struct {
    dsd_trunk_scan_target* targets; /**< Owned heap array; NULL when empty. */
    size_t count;
    size_t capacity;
} dsd_trunk_scan_target_list;

/**
 * @brief Release a target list's owned storage and zero it.
 *
 * The list returned by a successful dsd_trunk_scan_load_targets_csv() owns its
 * `targets` array; callers must reset it once the coordinator has copied the
 * targets out. A failed load never writes `*out`, so nothing to reset there.
 *
 * @param list List to clear; NULL is ignored.
 */
void dsd_trunk_scan_target_list_reset(dsd_trunk_scan_target_list* list);

int dsd_trunk_scan_load_targets_csv(const char* path, const dsd_opts* opts, dsd_trunk_scan_target_list* out, char* err,
                                    size_t err_sz);

int dsd_engine_trunk_scan_init(dsd_opts* opts, dsd_state* state, char* err, size_t err_sz);
void dsd_engine_trunk_scan_shutdown(dsd_opts* opts, dsd_state* state);
void dsd_engine_trunk_scan_tick(dsd_opts* opts, dsd_state* state);
void* dsd_engine_trunk_scan_active_p25_ctx(void);
void* dsd_engine_trunk_scan_active_dmr_ctx(void);
/**
 * @brief Channel-map path of the currently parked scan target, or NULL when it has none.
 *
 * Backs the `active_chan_csv` runtime hook: per-target `chan_csv` files are imported through
 * throwaway options, so this is the only place the path survives for protocol code to name.
 */
const char* dsd_engine_trunk_scan_active_chan_csv(const dsd_state* state);
/**
 * @brief Report decoded conventional activity so the active target keeps its park.
 *
 * Each entry point only acts when the target currently parked belongs to its own
 * conventional family; anything else (including trunk targets, or trunk scan not
 * being installed) is ignored. The NXDN entry point serves both NXDN conventional
 * types: NXDN48 and NXDN96 share a sync word and every decoded element, so the
 * protocol layer cannot tell them apart and must not have to. The call identity is run through the global
 * talkgroup policy, and only an allowed call refreshes the target's
 * activity hold, so allow/block lists, private-call, data-call and
 * encrypted-call tuning controls all apply to the park decision.
 *
 * Callers must pass identity that has already cleared the protocol's own
 * FEC/CRC gate: an unverified header would park the coordinator on noise.
 *
 * @param opts       Decoder options (policy and hold controls).
 * @param state      Decoder state owning the scan coordinator.
 * @param target     Destination talkgroup (group call) or destination unit (private call).
 * @param source     Source unit id, or 0 when unknown.
 * @param is_private Non-zero for a private/individual call, zero for a group call.
 * @param encrypted  Non-zero when the call is encrypted; pass the protocol's corroborated
 *                   classification, not a single frame's raw cipher field.
 * @param data_call  Non-zero for data traffic rather than voice.
 */
void dsd_engine_trunk_scan_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                     uint32_t source, int is_private, int encrypted, int data_call);
/** @copydoc dsd_engine_trunk_scan_dmr_conventional_activity */
void dsd_engine_trunk_scan_nxdn_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                      uint32_t source, int is_private, int encrypted, int data_call);
size_t dsd_engine_trunk_scan_target_count(const dsd_state* state);
int dsd_engine_trunk_scan_saved_tuner_autogain(const dsd_state* state, int* out_on);
int dsd_engine_trunk_scan_active_p25_cqpsk_request(const dsd_state* state, int* out_enable);
/**
 * @brief Symbol rate of the parked four-level GFSK scan target, or 0 when there is none.
 *
 * The tuning layer cannot derive this on its own. `state->rf_mod == 2` is true for both the
 * 4800 sym/s targets (DMR, NXDN96) and the 2400 sym/s ones (NXDN48), and `state->sps_hunt_idx`
 * is rotated by the no-sync SPS hunt, so a plain `-T` session can be sitting on the 2400 profile
 * during dead air. The coordinator's own target type is the only stable answer.
 *
 * @param state Decoder state owning the scan coordinator.
 * @return 2400 or 4800 for a parked GFSK-family target; 0 when trunk scan is not installed or the
 *         parked target is P25.
 */
int dsd_engine_trunk_scan_active_gfsk_symbol_rate(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_ */
