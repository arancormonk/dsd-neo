// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared decode preset helpers for CLI/config/snapshot paths.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
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
typedef enum { DSD_DECODE_PRESET_PROFILE_CONFIG = 0, DSD_DECODE_PRESET_PROFILE_CLI } dsdDecodePresetProfile;

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
 * @param profile Caller profile controlling legacy behavior differences.
 * @param opts Decoder options.
 * @param state Decoder state.
 * @return 0 on success, -1 for invalid args or unsupported mode.
 */
int dsd_apply_decode_mode_preset(dsdneoUserDecodeMode mode, dsdDecodePresetProfile profile, dsd_opts* opts,
                                 dsd_state* state);

/**
 * @brief Infer a user decode mode from active opts flags.
 *
 * Mirrors config snapshot classification behavior.
 *
 * @param opts Decoder options.
 * @return Inferred decode mode; `DSDCFG_MODE_AUTO` when no exact preset match.
 */
dsdneoUserDecodeMode dsd_infer_decode_mode_preset(const dsd_opts* opts);

#ifdef __cplusplus
}
#endif
