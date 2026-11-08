// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_SRC_CORE_UTIL_TALKGROUP_POLICY_INTERNAL_H_
#define DSD_NEO_SRC_CORE_UTIL_TALKGROUP_POLICY_INTERNAL_H_

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stdint.h>

/* Standalone CSV imports need only a policy store, not a decoder state. The created
 * reference is released with dsd_tg_policy_release(). Append has the public exact/range
 * insertion return convention: 0 stored, 1 invalid, -1 allocation failure. */
dsd_tg_policy_store* dsd_tg_policy_store_create(void);
int dsd_tg_policy_store_append(dsd_tg_policy_store* store, const dsd_tg_policy_entry* entry);
/* Mark a list loaded incomplete (dsd_tg_policy_load_report_incomplete()): a detached store, or the state's effective
 * one, created empty when the load stored no row at all. The mark lasts as long as the store; only a complete load
 * replaces it. The state form returns -1 when no store could be created to carry the mark, else 0. */
void dsd_tg_policy_store_mark_incomplete(dsd_tg_policy_store* store);
int dsd_tg_policy_mark_incomplete(dsd_state* state);

/* Mode rules shared by the group list and the radio list's policy columns. Mode `B` or `DE` blocks: no tuning and
 * every medium off. */
int dsd_tg_policy_mode_blocks(const char* mode);
/* Make media flags consistent with the mode: each flag 0/1, a blocking mode forces all three off, and audio off forces
 * record and stream off. Does nothing when a flag pointer is NULL. */
void dsd_tg_policy_media_normalize(const char* mode, uint8_t* audio, uint8_t* record, uint8_t* stream);

#endif
