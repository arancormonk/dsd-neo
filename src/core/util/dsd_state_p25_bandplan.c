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

static int
bandplan_row_is_global(const p25_bandplan_row_t* row) {
    return row->entry.wacn == 0ULL && row->entry.sysid == 0ULL;
}

static p25_iden_entry_t*
bandplan_row_slot(dsd_state* state, const p25_bandplan_row_t* row) {
    const int iden = row->iden & 0xF;
    return row->is_tdma ? &state->p25_iden_tdma[iden] : &state->p25_iden_fdma[iden];
}

static int
bandplan_entry_numbers_equal(const p25_iden_entry_t* a, const p25_iden_entry_t* b) {
    return a->base_freq == b->base_freq && a->chan_spac == b->chan_spac && a->chan_type == b->chan_type
           && a->trans_off == b->trans_off && a->bw_vu == b->bw_vu;
}

/*
 * True when `slot` still holds exactly what a global row of this plan seeded into it:
 * unconfirmed, no provenance at all, and the same numbers as a global row for the same
 * identifier and table. An over-the-air entry learned before the WACN/SYS was known also
 * carries zero provenance, so the numbers have to match too before a system row may
 * replace it.
 */
static int
bandplan_slot_is_seeded_global(const dsd_state* state, const p25_bandplan_row_t* row, const p25_iden_entry_t* slot) {
    if (slot->trust != 1 || slot->wacn != 0ULL || slot->sysid != 0ULL || slot->rfss != 0ULL || slot->site != 0ULL) {
        return 0;
    }
    for (int i = 0; i < state->p25_bandplan_row_count; i++) {
        const p25_bandplan_row_t* g = &state->p25_bandplan_rows[i];
        if (g->iden == row->iden && g->is_tdma == row->is_tdma && bandplan_row_is_global(g)
            && bandplan_entry_numbers_equal(&g->entry, slot)) {
            return 1;
        }
    }
    return 0;
}

static void
bandplan_write_slot(dsd_state* state, const p25_bandplan_row_t* row, p25_iden_entry_t* slot) {
    *slot = row->entry;
    slot->trust = 1;
    slot->populated = 1;
    slot->rfss = 0ULL;
    slot->site = 0ULL;
    state->p25_chan_tdma_explicit[row->iden & 0xF] |= row->is_tdma ? 0x02 : 0x01;
}

int
dsd_state_p25_bandplan_seed(dsd_state* state) {
    if (!state || state->p25_bandplan_row_count <= 0) {
        return 0;
    }
    const int count = state->p25_bandplan_row_count < DSD_P25_BANDPLAN_MAX_ROWS ? state->p25_bandplan_row_count
                                                                                : DSD_P25_BANDPLAN_MAX_ROWS;
    const int identity_known = (state->p2_wacn != 0ULL || state->p2_sysid != 0ULL);
    int changed = 0;

    // Rows that name this system come first: they may take over a slot a global row seeded.
    if (identity_known) {
        for (int i = 0; i < count; i++) {
            const p25_bandplan_row_t* row = &state->p25_bandplan_rows[i];
            if (bandplan_row_is_global(row) || row->entry.wacn != state->p2_wacn
                || row->entry.sysid != state->p2_sysid) {
                continue;
            }
            p25_iden_entry_t* slot = bandplan_row_slot(state, row);
            if (slot->populated && !bandplan_slot_is_seeded_global(state, row, slot)) {
                continue;
            }
            bandplan_write_slot(state, row, slot);
            changed++;
        }
    }

    // Global rows fill whatever is still empty; they never displace anything.
    for (int i = 0; i < count; i++) {
        const p25_bandplan_row_t* row = &state->p25_bandplan_rows[i];
        if (!bandplan_row_is_global(row)) {
            continue;
        }
        p25_iden_entry_t* slot = bandplan_row_slot(state, row);
        if (slot->populated) {
            continue;
        }
        bandplan_write_slot(state, row, slot);
        changed++;
    }
    return changed;
}

static int
bandplan_rows_contain(const p25_bandplan_row_t* rows, int count, int iden, int is_tdma, const p25_iden_entry_t* e) {
    for (int i = 0; i < count; i++) {
        if (rows[i].iden == iden && rows[i].is_tdma == is_tdma && rows[i].entry.wacn == e->wacn
            && rows[i].entry.sysid == e->sysid) {
            return 1;
        }
    }
    return 0;
}

static int
bandplan_append_table(p25_bandplan_row_t* rows, int count, int cap, const p25_iden_entry_t* table, int is_tdma) {
    for (int iden = 0; iden < 16 && count < cap; iden++) {
        const p25_iden_entry_t* e = &table[iden];
        if (!e->populated || e->base_freq == 0 || e->chan_spac == 0) {
            continue;
        }
        if (bandplan_rows_contain(rows, count, iden, is_tdma, e)) {
            continue;
        }
        p25_bandplan_row_t* row = &rows[count++];
        DSD_MEMSET(row, 0, sizeof *row);
        row->iden = (uint8_t)iden;
        row->is_tdma = (uint8_t)is_tdma;
        row->entry = *e;
        row->entry.trust = 1;
        row->entry.populated = 1;
        row->entry.rfss = 0ULL;
        row->entry.site = 0ULL;
    }
    return count;
}

int
dsd_p25_bandplan_append_tables(p25_bandplan_row_t* rows, int count, int cap, const p25_iden_entry_t* fdma,
                               const p25_iden_entry_t* tdma) {
    if (!rows || cap <= 0 || count < 0 || count > cap) {
        return count < 0 ? 0 : count;
    }
    if (fdma) {
        count = bandplan_append_table(rows, count, cap, fdma, 0);
    }
    if (tdma) {
        count = bandplan_append_table(rows, count, cap, tdma, 1);
    }
    return count;
}
