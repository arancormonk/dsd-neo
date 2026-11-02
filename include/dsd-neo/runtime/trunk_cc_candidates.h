// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared trunking control-channel candidate tracking.
 *
 * Stores validated CC candidate frequencies in a small struct attached to
 * `dsd_state` via `dsd_state_ext`.
 *
 * This keeps protocol libraries from expanding the core `dsd_state` struct with
 * protocol-specific fields while preserving existing candidate list behavior.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_CC_CANDIDATES_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_CC_CANDIDATES_H_H

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DSD_TRUNK_CC_CANDIDATES_MAX = 16 };

enum {
    DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE = 0x01,
};

typedef struct {
    long candidates[DSD_TRUNK_CC_CANDIDATES_MAX];
    uint8_t flags[DSD_TRUNK_CC_CANDIDATES_MAX];
    int count;
    int idx;
    double cool_until[DSD_TRUNK_CC_CANDIDATES_MAX];
    unsigned int added;
    unsigned int used;
} dsd_trunk_cc_candidates;

/**
 * @brief Where the candidate walk stands: the next index to examine and the use counter.
 *
 * Taken with dsd_trunk_cc_candidates_checkpoint() before a pick and handed back with
 * dsd_trunk_cc_candidates_restore() when the tune for the pick is deferred, which the
 * tuning hooks define as "retry without advancing state": the next pick is the same
 * candidate, and nothing counts it as used.
 */
typedef struct {
    int present; /* 0: the state had no candidate list when it was taken */
    int idx;
    unsigned int used;
} dsd_trunk_cc_candidates_cursor;

dsd_trunk_cc_candidates* dsd_trunk_cc_candidates_get(dsd_state* state);

const dsd_trunk_cc_candidates* dsd_trunk_cc_candidates_peek(const dsd_state* state);

int dsd_trunk_cc_candidates_add(dsd_state* state, long freq_hz, int bump_added, uint8_t flags);

/**
 * @brief Hand out the next eligible candidate after the last one handed out, wrapping.
 *
 * Advances the walk and counts the use. Finding none leaves the walk where it was, so an
 * index of 0 still means that nothing was handed out.
 *
 * @return 1 with @p out_freq_hz set; 0 when no candidate is eligible.
 */
int dsd_trunk_cc_candidates_next(dsd_state* state, double now_monotonic_s, uint8_t required_flags, long* out_freq_hz);

/**
 * @brief Hand out the eligible candidate before the last one handed out, wrapping (issue #666).
 *
 * The walk backward from dsd_trunk_cc_candidates_next()'s: the entries before the last one handed out are examined
 * nearest first, wrapping, and that entry itself last; with none handed out yet (index 0), from the last entry down.
 * The one found becomes the last handed out, so the next walk either way goes on from it, and its use is counted.
 * Finding none leaves the walk where it was.
 *
 * @return 1 with @p out_freq_hz set; 0 when no candidate is eligible.
 */
int dsd_trunk_cc_candidates_prev(dsd_state* state, double now_monotonic_s, uint8_t required_flags, long* out_freq_hz);

/** @brief Record where the candidate walk of @p state stands in @p out (see dsd_trunk_cc_candidates_cursor). */
void dsd_trunk_cc_candidates_checkpoint(const dsd_state* state, dsd_trunk_cc_candidates_cursor* out);

/** @brief Put the candidate walk of @p state back where @p cursor recorded it. */
void dsd_trunk_cc_candidates_restore(const dsd_state* state, const dsd_trunk_cc_candidates_cursor* cursor);

void dsd_trunk_cc_candidates_set_cooldown(const dsd_state* state, long freq_hz, double until_monotonic_s);

/**
 * @brief Forget every CC candidate for @p state, including the add/use counters.
 *
 * Used when the session is re-pointed at a different system: the old system's
 * candidates are not merely stale, they are wrong. No-op when the state has no
 * candidate extension yet (this never allocates one).
 */
void dsd_trunk_cc_candidates_reset(const dsd_state* state);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TRUNK_CC_CANDIDATES_H_H */
