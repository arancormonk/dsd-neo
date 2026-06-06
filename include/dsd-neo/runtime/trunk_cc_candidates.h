// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared trunking control-channel candidate tracking.
 *
 * Stores CC candidate frequencies (discovered from neighbors/adjacent/network
 * messages) in a small struct attached to `dsd_state` via `dsd_state_ext`.
 *
 * This keeps protocol libraries from expanding the core `dsd_state` struct with
 * protocol-specific fields while preserving existing candidate list behavior.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_CC_CANDIDATES_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_CC_CANDIDATES_H_H

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DSD_TRUNK_CC_CANDIDATES_MAX = 16 };

typedef struct {
    long candidates[DSD_TRUNK_CC_CANDIDATES_MAX];
    int count;
    int idx;
    double cool_until[DSD_TRUNK_CC_CANDIDATES_MAX];
    unsigned int added;
    unsigned int used;
} dsd_trunk_cc_candidates;

dsd_trunk_cc_candidates* dsd_trunk_cc_candidates_get(dsd_state* state);

const dsd_trunk_cc_candidates* dsd_trunk_cc_candidates_peek(const dsd_state* state);

int dsd_trunk_cc_candidates_add(dsd_state* state, long freq_hz, int bump_added);

int dsd_trunk_cc_candidates_next(dsd_state* state, double now_monotonic_s, long* out_freq_hz);

void dsd_trunk_cc_candidates_set_cooldown(const dsd_state* state, long freq_hz, double until_monotonic_s);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_CC_CANDIDATES_H_H */
