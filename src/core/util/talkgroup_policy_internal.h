// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_SRC_CORE_UTIL_TALKGROUP_POLICY_INTERNAL_H_
#define DSD_NEO_SRC_CORE_UTIL_TALKGROUP_POLICY_INTERNAL_H_

#include <dsd-neo/core/talkgroup_policy.h>
#include <stdint.h>

/* Standalone CSV imports need only a policy store, not a decoder state. The created
 * reference is released with dsd_tg_policy_release(). Append has the public exact/range
 * insertion return convention: 0 stored, 1 invalid, -1 allocation failure. */
dsd_tg_policy_store* dsd_tg_policy_store_create(void);
int dsd_tg_policy_store_append(dsd_tg_policy_store* store, const dsd_tg_policy_entry* entry);

/* Mode rules shared by the group list and the radio list's policy columns. Mode `B` or `DE` blocks: no tuning and
 * every medium off. */
int dsd_tg_policy_mode_blocks(const char* mode);
/* Make media flags consistent with the mode: each flag 0/1, a blocking mode forces all three off, and audio off forces
 * record and stream off. Does nothing when a flag pointer is NULL. */
void dsd_tg_policy_media_normalize(const char* mode, uint8_t* audio, uint8_t* record, uint8_t* stream);

#endif
