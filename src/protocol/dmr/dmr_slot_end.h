// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The end of one DMR slot's call over the air, and the slot's live crypto it takes with it: a terminator LC
   (dmr_flco.c) and a P_CLEAR addressed to the slot's call (dmr_csbk.c) end it the same way, and only that slot's. */
#ifndef DSD_NEO_PROTOCOL_DMR_SLOT_END_H
#define DSD_NEO_PROTOCOL_DMR_SLOT_END_H

#include <dsd-neo/core/call_state.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

// The per-slot live crypto fields the vocoder decrypts and gates with, mapped once so the
// terminator reset and the heal stash (dmr_slot_end.c) and restore (dmr_flco.c) all walk the same list. A
// field added to the stash but missed by a reset (or vice versa) would be stashed and healed
// but never cleared on terminator, leaking one call's crypto into the next.
typedef struct {
    unsigned int* fid;
    unsigned int* so;
    int* algid;
    int* keyid;
    unsigned long long int* mi;
} dmr_slot_crypto;

/* Slot @p idx's (0 or 1) live crypto fields. */
dmr_slot_crypto dmr_slot_crypto_fields(dsd_state* state, uint8_t idx);

/* Whether slot @p idx's live crypto (@p live, its fields) is all clear: the shape a reset leaves, which a heal alone
   restores into. */
int dmr_slot_crypto_is_clear(const dsd_state* state, uint8_t idx, const dmr_slot_crypto* live);

/* Stash slot @p slot's live crypto for epoch @p epoch, for the heal of a recoverable end to restore. */
void dmr_slot_crypto_stash(dsd_state* state, uint8_t slot, uint64_t epoch);

/* End slot @p slot's canonical call for @p reason and sync its row, then reset that slot's live crypto, its talkgroup
   key autoload and, when @p metadata_readable, its alias/superframe/GPS metadata; the companion slot keeps its own. A
   recoverable @p reason stashes the crypto first so a heal restores it; a final end drops any stash. */
void dmr_slot_end_call(dsd_opts* opts, dsd_state* state, uint8_t slot, dsd_call_end_reason reason,
                       int metadata_readable);

#endif /* DSD_NEO_PROTOCOL_DMR_SLOT_END_H */
