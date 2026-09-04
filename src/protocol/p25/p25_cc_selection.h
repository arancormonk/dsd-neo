// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_CC_SELECTION_H_
#define DSD_NEO_SRC_PROTOCOL_P25_P25_CC_SELECTION_H_

/* Session policy, deliberately outside the acquisition/no-carrier reset state. */
typedef struct {
    int require_site_cache;
} p25_cc_selection;

#endif
