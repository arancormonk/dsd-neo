// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine-owned trunk tuning policy entry points.
 *
 * These functions implement retune policy and bookkeeping and are installed
 * into the runtime trunk tuning hook table during engine startup.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_TUNING_H_
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_TUNING_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

dsd_trunk_tune_result dsd_engine_trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
dsd_trunk_tune_result dsd_engine_trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
dsd_trunk_tune_result dsd_engine_return_to_cc(dsd_opts* opts, dsd_state* state);
dsd_trunk_tune_result dsd_engine_trunk_tune_to_freq_request(dsd_opts* opts, dsd_state* state, long int freq,
                                                            int ted_sps, uint64_t request_id);
dsd_trunk_tune_result dsd_engine_trunk_tune_to_cc_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                          uint64_t request_id);
dsd_trunk_tune_result dsd_engine_return_to_cc_request(dsd_opts* opts, dsd_state* state, uint64_t request_id);
dsd_trunk_tune_result dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
dsd_trunk_tune_result dsd_engine_scan_tune_to_freq_with_id(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                           uint64_t* out_request_id);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_TUNING_H_ */
