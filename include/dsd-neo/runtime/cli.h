// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// Minimal CLI surface for DSD-neo
#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parse CLI arguments and environment into opts/state.
// Returns 0 to continue normal run; non-zero means the CLI handled a one-shot
// action (e.g., calculators or listings) and the process should exit with that
// status code.
// Return codes for dsd_parse_args
#define DSD_PARSE_CONTINUE 0
#define DSD_PARSE_ONE_SHOT 1

// out_argc receives the effective argc after long-option compaction so that
// downstream code (e.g., -r playback) can iterate safely over argv.
// If a one-shot action (like the LCN calculator) is handled, the function
// returns DSD_PARSE_ONE_SHOT and, when non-NULL, out_oneshot_rc contains the
// desired process exit status (0 for success, non-zero for failure).
int dsd_parse_args(int argc, char** argv, dsd_opts* opts, dsd_state* state, int* out_argc, int* out_oneshot_rc);

// Print the CLI usage/help text.
void dsd_cli_usage(void);

// Optional bootstrap helpers
void dsd_bootstrap_enable_ftz_daz_if_enabled(void);
void dsd_bootstrap_choose_audio_output(dsd_opts* opts);
void dsd_bootstrap_choose_audio_input(dsd_opts* opts);

#ifdef __cplusplus
}
#endif
