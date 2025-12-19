// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Minimal CLI surface for parsing args/env into dsd_opts/dsd_state.
 */
#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Return codes for dsd_parse_args. */
#define DSD_PARSE_CONTINUE 0
#define DSD_PARSE_ONE_SHOT 1

/**
 * @brief Parse CLI arguments and environment into opts/state.
 *
 * Populates opts/state, compacts argv for downstream processing, and handles
 * one-shot helpers (calculators/listings). Returns DSD_PARSE_ONE_SHOT when a
 * one-shot was executed; out_oneshot_rc (when non-NULL) receives desired exit status.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param opts Decoder options to populate.
 * @param state Decoder state to populate.
 * @param out_argc [out] Effective argc after compaction (may be NULL).
 * @param out_oneshot_rc [out] Exit status when returning ONE_SHOT (may be NULL).
 * @return DSD_PARSE_CONTINUE to run decoder; DSD_PARSE_ONE_SHOT when handled.
 */
int dsd_parse_args(int argc, char** argv, dsd_opts* opts, dsd_state* state, int* out_argc, int* out_oneshot_rc);

/** @brief Print the CLI usage/help text and exit(0). */
void dsd_cli_usage(void);

/**
 * @brief Run the DMR TIII LCN calculator one-shot utility.
 *
 * Parses frequencies from a CSV file and emits an LCN mapping to stdout.
 * Uses environment variables for optional configuration:
 *   - DSD_NEO_DMR_T3_STEP_HZ: Override inferred channel step
 *   - DSD_NEO_DMR_T3_CC_FREQ: Anchor CC frequency
 *   - DSD_NEO_DMR_T3_CC_LCN: Anchor CC LCN
 *   - DSD_NEO_DMR_T3_START_LCN: Starting LCN when no anchor
 *
 * @param path Path to CSV file containing frequencies.
 * @return 0 on success, non-zero on error.
 */
int dsd_cli_calc_dmr_t3_lcn_from_csv(const char* path);

/** @brief Enable FTZ/DAZ CPU modes when requested via env. */
void dsd_bootstrap_enable_ftz_daz_if_enabled(void);
/** @brief Select default audio output device when not explicitly set. */
void dsd_bootstrap_choose_audio_output(dsd_opts* opts);
/** @brief Select default audio input device when not explicitly set. */
void dsd_bootstrap_choose_audio_input(dsd_opts* opts);

#ifdef __cplusplus
}
#endif
