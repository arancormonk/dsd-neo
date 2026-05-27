// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for trunking tune side effects.
 *
 * Protocol state machines may need to request retunes without depending on
 * IO/control headers or linking IO backends. The engine (or tests) installs
 * real hook functions at startup; the runtime provides safe wrappers and
 * fallback behavior when hooks are not installed.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_TUNING_HOOKS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_TUNING_HOOKS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_TRUNK_TUNE_RESULT_OK = 0,
    /* No hardware retune was scheduled yet; callers should retry without advancing state. */
    DSD_TRUNK_TUNE_RESULT_DEFERRED = 1,
    /* Retune was accepted but completion is asynchronous; callers should commit intended state. */
    DSD_TRUNK_TUNE_RESULT_PENDING = 2,
    DSD_TRUNK_TUNE_RESULT_FAILED = -1,
    DSD_TRUNK_TUNE_RESULT_TIMEOUT = -2,
} dsd_trunk_tune_result;

typedef struct {
    void (*tune_to_freq)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
    void (*tune_to_cc)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
    void (*return_to_cc)(dsd_opts* opts, dsd_state* state);
    dsd_trunk_tune_result (*tune_to_freq_result)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
    dsd_trunk_tune_result (*tune_to_cc_result)(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
    dsd_trunk_tune_result (*return_to_cc_result)(dsd_opts* opts, dsd_state* state);
} dsd_trunk_tuning_hooks;

void dsd_trunk_tuning_hooks_set(dsd_trunk_tuning_hooks hooks);

dsd_trunk_tune_result dsd_trunk_tuning_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
dsd_trunk_tune_result dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps);
dsd_trunk_tune_result dsd_trunk_tuning_hook_return_to_cc(dsd_opts* opts, dsd_state* state);

static inline int
dsd_trunk_tune_result_is_ok(dsd_trunk_tune_result result) {
    return result == DSD_TRUNK_TUNE_RESULT_OK || result == DSD_TRUNK_TUNE_RESULT_PENDING;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_TUNING_HOOKS_H_ */
