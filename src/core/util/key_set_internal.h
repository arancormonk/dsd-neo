// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_UTIL_KEY_SET_INTERNAL_H_
#define DSD_NEO_SRC_CORE_UTIL_KEY_SET_INTERNAL_H_

#include <dsd-neo/core/state_fwd.h>

/* Compiler-resistant wipe of the inline key material held by a state. */
void dsd_key_state_secure_wipe(dsd_state* state);

#endif /* DSD_NEO_SRC_CORE_UTIL_KEY_SET_INTERNAL_H_ */
