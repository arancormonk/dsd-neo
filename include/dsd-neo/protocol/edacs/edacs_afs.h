// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief EDACS helper interfaces.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_EDACS_EDACS_AFS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_EDACS_EDACS_AFS_H_

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

int getAfsString(const dsd_state* state, char* buffer, int a, int f, int s);
int getAfsStringLength(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_EDACS_EDACS_AFS_H_ */
