// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared decode preset helpers for CLI/config/snapshot paths.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DECODE_MODE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DECODE_MODE_H_

#include <dsd-neo/platform/platform.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/runtime/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller profile for decode preset application.
 *
 * Some presets intentionally differ between config and CLI paths to preserve
 * existing behavior.
 */
typedef enum DSD_ATTR_PACKED {
    DSD_DECODE_PRESET_PROFILE_CONFIG = 0,
    DSD_DECODE_PRESET_PROFILE_CLI,
    DSD_DECODE_PRESET_PROFILE_INTERACTIVE
} dsdDecodePresetProfile;

/**
 * @brief Map a core `-f` CLI preset character to a user decode mode enum.
 *
 * Supports the shared subset used by config/CLI (`a,A,d,x,t,1,2,s,i,n,y,m`).
 *
 * @param preset Single-character CLI `-f` selector.
 * @param out_mode Output mode enum.
 * @return 0 on success, -1 if unsupported or invalid args.
 */
int dsd_decode_mode_from_cli_preset(char preset, dsdneoUserDecodeMode* out_mode);

/**
 * @brief Apply a decode preset to opts/state.
 *
 * @param mode Decode mode preset.
 * @param profile Caller profile controlling preset semantics.
 * @param opts Decoder options.
 * @param state Decoder state.
 * @return 0 on success, -1 for invalid args or unsupported mode.
 */
int dsd_apply_decode_mode_preset(dsdneoUserDecodeMode mode, dsdDecodePresetProfile profile, dsd_opts* opts,
                                 dsd_state* state);

/**
 * @brief Rebuild preset symbol timing for a decode mode at a target PCM rate.
 *
 * Starts from the preset's canonical 48 kHz timing and rescales it to the
 * provided effective PCM rate. This is used when a mode preset is combined
 * with non-48 kHz file/socket input so the slicer starts with the correct SPS.
 *
 * @param mode Decode mode preset.
 * @param effective_input_rate_hz Effective PCM rate after any staged upsample.
 * @param state Decoder state receiving `samplesPerSymbol` and `symbolCenter`.
 */
void dsd_apply_decode_mode_symbol_timing(dsdneoUserDecodeMode mode, int effective_input_rate_hz, dsd_state* state);

/**
 * @brief The symbol profile a decode mode runs on.
 *
 * One answer for the three things that have to agree once a mode is live: the
 * symbol clock the slicer runs at, the number of levels the demodulator slices
 * to, and the frame-sync SPS profile the hunt searches from. Derived separately
 * they drift, and a mode ends up on one protocol's symbol clock with another
 * protocol's hunt profile and a third protocol's channel filter.
 *
 * This is the steady-state profile — what the SPS hunt will converge on — which
 * is not always where @c dsd_apply_decode_mode_symbol_timing() starts a mode off.
 */
typedef struct {
    int symbol_rate_hz;                                 /**< 2400, 4800, 6000 or 9600. */
    int levels;                                         /**< 2 or 4. */
    dsd_frame_sync_sps_profile_index sps_profile_index; /**< Hunt profile carrying this mode. */
} dsd_decode_mode_profile;

/**
 * @brief Return the symbol profile @p mode decodes on.
 *
 * Modes with no profile of their own — AUTO, analog monitor, and any mode set
 * that spans several symbol rates — answer with 4800/4, which is both the
 * commonest case and the hunt's own starting profile.
 *
 * @param mode Decode mode preset.
 * @return Symbol rate, level count and frame-sync profile index for @p mode.
 */
dsd_decode_mode_profile dsd_decode_mode_profile_for(dsdneoUserDecodeMode mode);

/**
 * @brief Return the RTL channel filter a symbol profile and modulation need.
 *
 * The one copy of this mapping. A modulation the operator picks and one the SPS
 * hunt lands on have to ask the front end for the same filter, or the two
 * disagree about what the front end is doing every time the hunt re-runs.
 *
 * @param opts Decoder options, consulted for the wide-4800 profile override.
 * @param symbol_rate_hz Symbol rate in Hz.
 * @param levels Number of slicer levels (2 or 4).
 * @param rf_mod Modulation, as @c dsd_state::rf_mod (0 C4FM, 1 QPSK, 2 GFSK).
 * @return A channel-profile selector. The `RTL_STREAM_CHANNEL_PROFILE_*` and
 *         `DSD_RTL_STREAM_CHANNEL_PROFILE_*` enumerations share these values, so
 *         either spelling may be compared against the result.
 */
int dsd_rtl_channel_profile_for(const dsd_opts* opts, int symbol_rate_hz, int levels, int rf_mod);

/**
 * @brief Infer a user decode mode from active opts flags.
 *
 * Mirrors config snapshot classification behavior.
 *
 * @param opts Decoder options.
 * @return Inferred decode mode; `DSDCFG_MODE_AUTO` when no exact preset match.
 */
dsdneoUserDecodeMode dsd_infer_decode_mode_preset(const dsd_opts* opts);

/**
 * @brief Human-readable name of a decode preset, for anything the operator reads.
 *
 * One table for every frontend that names a preset -- the picker, the label that
 * reads the current mode back, and the toast that confirms an applied mode -- so
 * they cannot drift apart. A second table is how "DMR" and "DMR (single slot)"
 * came to confirm themselves with the same word.
 *
 * @param mode Decode mode preset.
 * @return A static string such as "P25 Phase 1"; "Unset" for
 *         DSDCFG_MODE_UNSET; "Unknown" for any value outside the enum.
 */
const char* dsd_decode_mode_display_name(dsdneoUserDecodeMode mode);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DECODE_MODE_H_ */
