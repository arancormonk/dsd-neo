// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_PROTOCOL_DSTAR_HEADER_H
#define DSD_NEO_PROTOCOL_DSTAR_HEADER_H

#include <dsd-neo/protocol/dstar/dstar_header_utils.h>

struct dsd_state;

void dstar_header_decode(struct dsd_state* state, int radioheaderbuffer[DSD_DSTAR_HEADER_CODED_BITS]);

#endif /* DSD_NEO_PROTOCOL_DSTAR_HEADER_H */
