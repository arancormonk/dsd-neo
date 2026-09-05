// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_SRC_CORE_UTIL_TALKGROUP_POLICY_INTERNAL_H_
#define DSD_NEO_SRC_CORE_UTIL_TALKGROUP_POLICY_INTERNAL_H_

#include <dsd-neo/core/talkgroup_policy.h>

/* Standalone CSV imports need only a policy store, not a decoder state. The created
 * reference is released with dsd_tg_policy_release(). Append has the public exact/range
 * insertion return convention: 0 stored, 1 invalid, -1 allocation failure. */
dsd_tg_policy_store* dsd_tg_policy_store_create(void);
int dsd_tg_policy_store_append(dsd_tg_policy_store* store, const dsd_tg_policy_entry* entry);

#endif
