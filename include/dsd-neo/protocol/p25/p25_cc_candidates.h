// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Control-channel candidates and neighbor list helpers for P25.
 * Provides small utilities to track announced neighbors, load/persist a
 * per-system candidate list, and expire stale entries.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build a per-system cache path (returns 1 on success)
int p25_cc_build_cache_path(const dsd_state* state, char* out, size_t out_len);

// Load/persist candidate CC list (best-effort)
void p25_cc_try_load_cache(dsd_opts* opts, dsd_state* state);
void p25_cc_persist_cache(dsd_opts* opts, dsd_state* state);

// Neighbor tracking (up to 32 entries)
void p25_nb_add(dsd_state* state, long freq_hz);
void p25_nb_tick(dsd_state* state);

#ifdef __cplusplus
}
#endif
