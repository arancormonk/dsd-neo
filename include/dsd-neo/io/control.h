// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief IO control and tuning helpers.
 *
 * These functions form the bridge between protocol/UI decisions and the
 * underlying retune backend (rigctl or RTL-SDR).
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void return_to_cc(dsd_opts* opts, dsd_state* state);
void trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
void trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
int io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq);
void resumeScan(dsd_opts* opts, dsd_state* state);
void openSerial(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
