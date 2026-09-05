// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Positional channel-map mode metadata, owned by core extension slot 5. */
#ifndef DSD_NEO_CORE_CHANNEL_MODE_H
#define DSD_NEO_CORE_CHANNEL_MODE_H
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Missing slots inherit the configured decoder settings. */
dsd_scan_mode dsd_channel_mode_get(const dsd_state* state, size_t row);
/** Set one scan-list slot, growing its heap store. Returns -1 on allocation failure. */
int dsd_channel_mode_set(dsd_state* state, size_t row, dsd_scan_mode mode);
/** Release all row modes. Does not restore active decoder settings. */
void dsd_channel_modes_clear(dsd_state* state);
/** Transfer row definitions, replacing destination modes and clearing the source extension.
 * Restore both states' global group policies first; active/suspended scopes are not moved. */
void dsd_channel_modes_move(dsd_state* dst, dsd_state* src);
/** Nonzero if at least one row declares a mode or carries scoped row options (scan_profile.h),
 * i.e. the typed scanner must run the list. */
int dsd_channel_modes_present(const dsd_state* state);
#ifdef __cplusplus
}
#endif
#endif
