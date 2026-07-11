// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Keyring helper API.
 *
 * Declares the keyring loader implemented in core.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_KEYRING_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_KEYRING_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Activate imported key material for an explicit decoder slot. */
void keyring_activate_slot(dsd_opts* opts, dsd_state* state, int slot);

/** Compatibility wrapper that activates state->currentslot. */
void keyring(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_KEYRING_H_H */
