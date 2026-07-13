// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frame sync helper APIs.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <time.h> // IWYU pragma: keep

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Stable indices stored in @c dsd_state::sps_hunt_idx. */
typedef enum {
    DSD_FRAME_SYNC_SPS_PROFILE_4800_4 = 0,
    DSD_FRAME_SYNC_SPS_PROFILE_2400_4 = 1,
    DSD_FRAME_SYNC_SPS_PROFILE_9600_2 = 2,
    DSD_FRAME_SYNC_SPS_PROFILE_6000_4 = 3,
    DSD_FRAME_SYNC_SPS_PROFILE_4800_2 = 4,
    DSD_FRAME_SYNC_SPS_PROFILE_COUNT = 5,
} dsd_frame_sync_sps_profile_index;

/** @brief NXDN air-interface variant selected by frame-sync profile matching. */
typedef enum {
    DSD_NXDN_VARIANT_NONE = 0,
    DSD_NXDN_VARIANT_48 = 48,
    DSD_NXDN_VARIANT_96 = 96,
} dsd_nxdn_variant;

/**
 * @brief Reset modulation auto-detect state used by frame sync.
 */
void dsd_frame_sync_reset_mod_state(void);

/**
 * @brief Return the NXDN variant selected by the enabled mode and active SPS hunt profile.
 */
dsd_nxdn_variant dsd_frame_sync_active_nxdn_variant(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return non-zero when alternate-protocol sync should be suppressed during active P25 trunking.
 */
int dsd_frame_sync_suppress_p25_alt_sync(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return non-zero when TCP no-signal diagnostics should stay off the console.
 */
int dsd_frame_sync_suppress_tcp_no_signal_console(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return the number of no-sync buffer passes to dwell before SPS hunt advances.
 */
int dsd_frame_sync_sps_hunt_dwell_passes(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Scan for a valid frame sync pattern and return its type.
 */
int getFrameSync(dsd_opts* opts, dsd_state* state);

/**
 * @brief Emit diagnostic information about detected frame sync.
 *
 * @param frametype Human-friendly frame type string.
 * @param offset Bit offset into the buffer where sync was found.
 * @param modulation Modulation label (e.g., C4FM, QPSK).
 */
void printFrameSync(const dsd_opts* opts, const dsd_state* state, const char* frametype, int offset,
                    const char* modulation);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_H */
