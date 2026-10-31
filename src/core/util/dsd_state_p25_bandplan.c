// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief User-supplied P25 band plan: seeding the live IDEN tables and collecting them back.
 *
 * Kept beside dsd_state_trunk_lcn.c so test targets that link individual core
 * sources (P25 frequency, trunk scan, menu services) can attach the real
 * implementation without the initState/freeState dependency chain.
 */

#include <dsd-neo/core/state.h>
#include <stdint.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_p25_iden_params_usable(long base, int spacing) {
    return base > 0 && base <= DSD_TRUNK_FREQ_MAX_HZ / 5 && spacing > 0;
}

int
dsd_state_p25_iden_tdma_applies(const dsd_state* state, int iden) {
    if (!state || iden < 0 || iden >= 16) {
        return 0;
    }
    const p25_iden_entry_t* e = &state->p25_iden_tdma[iden];
    return e->populated && dsd_p25_iden_params_usable(e->base_freq, e->chan_spac)
           && ((e->wacn == 0ULL && e->sysid == 0ULL) || (e->wacn == state->p2_wacn && e->sysid == state->p2_sysid));
}

int
dsd_state_p25_iden_reconcile(dsd_state* state, int iden) {
    if (!dsd_state_p25_iden_tdma_applies(state, iden)) {
        return 0;
    }
    p25_iden_entry_t* e = &state->p25_iden_fdma[iden];
    if (!e->populated || !(e->flags & P25_IDEN_F_UNTYPED)) {
        return 0;
    }
    DSD_MEMSET(e, 0, sizeof *e);
    state->p25_chan_tdma_explicit[iden] &= 0xFEU; // clear the FDMA bit
    return 1;
}

void
dsd_state_p25_invalidate_iden_chan_map(dsd_state* state, int iden) {
    if (!state || iden < 0 || iden >= 16) {
        return;
    }
    const int start = iden << 12;
    int end = (iden + 1) << 12;
    const int map_count = (int)(sizeof state->trunk_chan_map / sizeof state->trunk_chan_map[0]);
    if (end > map_count) {
        end = map_count;
    }
    for (int ch = start; ch < end; ch++) {
        dsd_state_set_trunk_chan_freq(state, (uint32_t)ch, 0);
    }
}

static int
bandplan_row_is_global(const p25_bandplan_row_t* row) {
    return row->entry.wacn == 0ULL && row->entry.sysid == 0ULL;
}

static int
bandplan_slot_is_seeded_global(const p25_iden_entry_t* slot) {
    return (slot->flags & P25_IDEN_F_SEEDED) && slot->wacn == 0ULL && slot->sysid == 0ULL;
}

/*
 * A typed row may take a slot that holds an untyped entry (an LCW Channel Identifier Update names no channel type, nor
 * does a row with an empty `type`), but never a typed over-the-air one. Among seeded rows scope comes first: a row that
 * names this system takes over a global row's slot, typed or not, and a global row never displaces a system's row.
 */
static int
bandplan_can_replace(const p25_bandplan_row_t* row, const p25_iden_entry_t* slot, int global) {
    if (!slot->populated) {
        return 1;
    }
    const int seeded = (slot->flags & P25_IDEN_F_SEEDED) != 0;
    if (seeded && !global && bandplan_slot_is_seeded_global(slot)) {
        return 1;
    }
    if (seeded && global && !bandplan_slot_is_seeded_global(slot)) {
        return 0;
    }
    return !(row->entry.flags & P25_IDEN_F_UNTYPED) && (slot->flags & P25_IDEN_F_UNTYPED);
}

static void
bandplan_write_slot(dsd_state* state, const p25_bandplan_row_t* row, p25_iden_entry_t* slot) {
    *slot = row->entry;
    slot->flags |= P25_IDEN_F_SEEDED;
    slot->trust = 1;
    slot->populated = 1;
    slot->rfss = 0ULL;
    slot->site = 0ULL;
    state->p25_chan_tdma_explicit[row->iden] |= row->is_tdma ? 0x02 : 0x01;
}

static int
bandplan_untyped_waits(const dsd_state* state, const p25_bandplan_row_t* row) {
    return !row->is_tdma && (row->entry.flags & P25_IDEN_F_UNTYPED)
           && dsd_state_p25_iden_tdma_applies(state, row->iden);
}

static int
bandplan_row_count(const dsd_state* state) {
    return state->p25_bandplan_row_count < DSD_P25_BANDPLAN_MAX_ROWS ? state->p25_bandplan_row_count
                                                                     : DSD_P25_BANDPLAN_MAX_ROWS;
}

/*
 * A global row does not apply to a system that has its own row for the identifier and class, even while that row
 * waits (an untyped row beside an applicable TDMA entry): the system's row names what the identifier is there.
 */
static int
bandplan_global_shadowed(const dsd_state* state, int iden, int is_tdma) {
    if (state->p2_wacn == 0ULL && state->p2_sysid == 0ULL) {
        return 0;
    }
    const int count = bandplan_row_count(state);
    for (int i = 0; i < count; i++) {
        const p25_bandplan_row_t* row = &state->p25_bandplan_rows[i];
        if (row->iden == iden && row->is_tdma == is_tdma && !bandplan_row_is_global(row)
            && row->entry.wacn == state->p2_wacn && row->entry.sysid == state->p2_sysid) {
            return 1;
        }
    }
    return 0;
}

/* Drop global seeds a system's own row shadows, so the result does not depend on what was learned first. */
static void
bandplan_drop_shadowed_globals(dsd_state* state, uint16_t* mask) {
    for (int iden = 0; iden < 16; iden++) {
        for (int is_tdma = 0; is_tdma < 2; is_tdma++) {
            p25_iden_entry_t* slot = is_tdma ? &state->p25_iden_tdma[iden] : &state->p25_iden_fdma[iden];
            if (!slot->populated || !bandplan_slot_is_seeded_global(slot)
                || !bandplan_global_shadowed(state, iden, is_tdma)) {
                continue;
            }
            DSD_MEMSET(slot, 0, sizeof *slot);
            state->p25_chan_tdma_explicit[iden] &= is_tdma ? 0xFDU : 0xFEU; // clear this class's bit
            *mask |= (uint16_t)(1U << iden);
        }
    }
}

static int
bandplan_seed_pass(dsd_state* state, int is_tdma, int global, uint16_t* mask) {
    const int count = bandplan_row_count(state);
    int writes = 0;
    for (int i = 0; i < count; i++) {
        const p25_bandplan_row_t* row = &state->p25_bandplan_rows[i];
        if (row->iden >= 16 || row->is_tdma != is_tdma || bandplan_row_is_global(row) != global) {
            continue;
        }
        if (!global && (row->entry.wacn != state->p2_wacn || row->entry.sysid != state->p2_sysid)) {
            continue;
        }
        if ((global && bandplan_global_shadowed(state, row->iden, is_tdma)) || bandplan_untyped_waits(state, row)) {
            continue;
        }
        p25_iden_entry_t* slot = is_tdma ? &state->p25_iden_tdma[row->iden] : &state->p25_iden_fdma[row->iden];
        if (!bandplan_can_replace(row, slot, global)) {
            continue;
        }
        if (slot->populated) {
            *mask |= (uint16_t)(1U << row->iden);
        }
        bandplan_write_slot(state, row, slot);
        writes++;
    }
    return writes;
}

int
dsd_state_p25_bandplan_seed_mask(dsd_state* state, uint16_t* changed_idens) {
    uint16_t mask = 0;
    int writes = 0;
    if (state) {
        bandplan_drop_shadowed_globals(state, &mask);
        writes += bandplan_seed_pass(state, 1, 0, &mask);
        writes += bandplan_seed_pass(state, 1, 1, &mask);
        for (int iden = 0; iden < 16; iden++) {
            if (dsd_state_p25_iden_reconcile(state, iden)) {
                mask |= (uint16_t)(1U << iden);
            }
        }
        writes += bandplan_seed_pass(state, 0, 0, &mask);
        writes += bandplan_seed_pass(state, 0, 1, &mask);
    }
    if (changed_idens) {
        *changed_idens = mask;
    }
    return writes;
}

int
dsd_state_p25_bandplan_seed(dsd_state* state) {
    uint16_t mask = 0;
    const int writes = dsd_state_p25_bandplan_seed_mask(state, &mask);
    for (int iden = 0; iden < 16; iden++) {
        if (mask & (1U << iden)) {
            dsd_state_p25_invalidate_iden_chan_map(state, iden);
        }
    }
    return writes;
}

static int
bandplan_same_scope(const p25_bandplan_row_t* row, int iden, const p25_iden_entry_t* e) {
    return row->iden == iden && row->entry.wacn == e->wacn && row->entry.sysid == e->sysid;
}

static int
bandplan_find_key(const p25_bandplan_row_t* rows, int count, int iden, int is_tdma, const p25_iden_entry_t* e) {
    for (int i = 0; i < count; i++) {
        if (rows[i].is_tdma == is_tdma && bandplan_same_scope(&rows[i], iden, e)) {
            return i;
        }
    }
    return -1;
}

static void
bandplan_collect_entry(p25_bandplan_row_t* row, int iden, int is_tdma, const p25_iden_entry_t* e) {
    DSD_MEMSET(row, 0, sizeof *row);
    row->iden = (uint8_t)iden;
    row->is_tdma = (uint8_t)is_tdma;
    row->entry = *e;
    row->entry.trust = 1;
    row->entry.populated = 1;
    row->entry.rfss = 0ULL;
    row->entry.site = 0ULL;
}

static int
bandplan_remove_untyped(p25_bandplan_row_t* rows, int count, int iden, const p25_iden_entry_t* e) {
    int keep = 0;
    for (int i = 0; i < count; i++) {
        if (!rows[i].is_tdma && (rows[i].entry.flags & P25_IDEN_F_UNTYPED) && bandplan_same_scope(&rows[i], iden, e)) {
            continue;
        }
        rows[keep++] = rows[i];
    }
    return keep;
}

static int
bandplan_append_entry(p25_bandplan_row_t* rows, int count, int cap, int iden, int is_tdma, const p25_iden_entry_t* e,
                      int* skipped) {
    const int existing = bandplan_find_key(rows, count, iden, is_tdma, e);
    if (existing >= 0) {
        if ((rows[existing].entry.flags & P25_IDEN_F_UNTYPED) && !(e->flags & P25_IDEN_F_UNTYPED)) {
            bandplan_collect_entry(&rows[existing], iden, is_tdma, e);
        }
        return count;
    }
    if (is_tdma) {
        count = bandplan_remove_untyped(rows, count, iden, e);
    } else if ((e->flags & P25_IDEN_F_UNTYPED) && bandplan_find_key(rows, count, iden, 1, e) >= 0) {
        return count;
    }
    if (count == cap) {
        if (skipped) {
            (*skipped)++;
        }
        return count;
    }
    bandplan_collect_entry(&rows[count++], iden, is_tdma, e);
    return count;
}

static int
bandplan_append_table(p25_bandplan_row_t* rows, int count, int cap, const p25_iden_entry_t* table, int is_tdma,
                      int* skipped) {
    for (int iden = 0; iden < 16; iden++) {
        const p25_iden_entry_t* e = &table[iden];
        if (e->populated && dsd_p25_iden_params_usable(e->base_freq, e->chan_spac)) {
            count = bandplan_append_entry(rows, count, cap, iden, is_tdma, e, skipped);
        }
    }
    return count;
}

int
dsd_p25_bandplan_append_tables(p25_bandplan_row_t* rows, int count, int cap, const p25_iden_entry_t* fdma,
                               const p25_iden_entry_t* tdma, int* skipped) {
    if (!rows || cap <= 0 || count < 0 || count > cap) {
        return count < 0 ? 0 : count;
    }
    if (tdma) {
        count = bandplan_append_table(rows, count, cap, tdma, 1, skipped);
    }
    if (fdma) {
        count = bandplan_append_table(rows, count, cap, fdma, 0, skipped);
    }
    return count;
}
