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

/**
 * @brief Non-zero when a symbol profile's level count admits only GFSK.
 *
 * Two-level slicing is frequency-shift keying; there is no two-level C4FM or
 * QPSK to choose. Stated once because three callers act on it and they have to
 * act the same way: the SPS hunt normalises @c dsd_state::rf_mod to GFSK on such
 * a profile, the SNR gate reads the GFSK estimator for it, and the UI's
 * modulation control has to stop offering the other two -- a control that lets a
 * two-level session be set to C4FM leaves @c dsd_opts::mod_c4fm saying one thing
 * while the demodulator the hunt rebuilt does another, and nothing ever
 * reconciles the pair.
 */
static inline int
dsd_frame_sync_profile_levels_force_gfsk(int levels) {
    return levels == 2;
}

/**
 * @brief The modulation @p requested_rf_mod becomes on a profile with @p levels levels.
 *
 * @param levels Slicer level count, 2 or 4.
 * @param requested_rf_mod Modulation asked for, as @c dsd_state::rf_mod
 *                         (0 C4FM, 1 QPSK, 2 GFSK). Pass 0 for "no opinion" to
 *                         get the profile's own default.
 * @return @p requested_rf_mod on a four-level profile, GFSK on a two-level one.
 */
static inline int
dsd_frame_sync_profile_modulation(int levels, int requested_rf_mod) {
    return dsd_frame_sync_profile_levels_force_gfsk(levels) ? 2 : requested_rf_mod;
}

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
 * @brief Return the symbol rate, in Hz, of the SPS hunt profile currently selected.
 *
 * The decoder's own timing authority on RTL-family FSK input: the front end applies
 * a hunt's profile request asynchronously, so its published symbol rate lags the
 * hunt and must not be what the slicer derives samples-per-symbol from.
 *
 * @param state Decoder state; NULL or an out-of-range index yields the 4800/4 default.
 */
int dsd_frame_sync_active_profile_symbol_rate_hz(const dsd_state* state);

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
