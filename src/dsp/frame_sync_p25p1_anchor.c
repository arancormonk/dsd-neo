// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The P25 Phase 1 sync anchor (dsd_p25p1_sync_anchor, <dsd-neo/dsp/p25p1_sync_anchor.h>): its sync-time context, its
 * installation, the position the hunt scores a tolerant word at, and the distance it scores. The matcher that accepts
 * a tolerant word is frame_sync_try_p25p1() in dsd_frame_sync.c. On the C4FM chain the same sync and the same report
 * place the symbol grid on each data unit's sync word (<dsd-neo/dsp/symbol_anchor.h>). */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <stddef.h>
#include <stdint.h>
#include "frame_sync_internal.h"

enum {
    P25P1_SYNC_DIBITS = 24,
    /* The most a data unit's decoder may leave unread before the next unit's sync: the null dibits that pad its last
       70-bit segment and the status symbol after them, less than a 36-dibit segment (p25p1_block_unit_dibits()). */
    P25P1_GRID_MAX_TAIL_DIBITS = 35,
};

int
dsd_p25p1_sync_bit_distance(const char* window, uint8_t map_idx, int inverted) {
    return frame_sync_p25_sync_bit_distance(window, inverted ? INV_P25P1_SYNC : P25P1_SYNC, P25P1_SYNC_DIBITS, map_idx);
}

/* Whether @p ctx still describes the stream (frame_sync_p25_sync_context_holds()) with no off-thread tune awaiting
   reconciling. */
static int
p25p1_anchor_context_current(const dsd_state* state, const dsd_p25_sync_context* ctx) {
    return frame_sync_p25_sync_context_holds(state, ctx) && !dsd_frame_sync_off_thread_tune_pending();
}

void
frame_sync_p25p1_anchor_note_sync(dsd_state* state, int synctype, int tolerant_nac, int bit_errors) {
    dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    /* Whether the sync was tolerant stands whatever becomes of the context: such a frame decodes only past the strict
       check, which nothing may skip. */
    rec->frame_tolerant = tolerant_nac >= 0 ? 1U : 0U;
    rec->frame_nac = tolerant_nac >= 0 ? (uint16_t)tolerant_nac : 0U;
    rec->sync_bit_errors = (uint8_t)bit_errors;
    rec->frame_valid = 0U;
    /* On the C4FM chain the hunt's crossing loop leaves the grid a sample past the word's symbols, where the frame
       behind it would be read (docs/code_map.md, symbol anchor). The previous unit's report armed a recording at its
       end (dsd_frame_sync_p25p1_frame_decoded()), which starts on the word's first symbol, so the word is named from
       its second: the measurement needs a symbol of samples before the first centre it reads. A word within the
       tolerance names nothing: until its NID validates it may be no word at all. */
    if (tolerant_nac < 0 && state->rf_mod == 0) {
        const char* word = synctype == DSD_SYNC_P25P1_NEG ? INV_P25P1_SYNC : P25P1_SYNC;
        dsd_symbol_note_p25p1_sync_anchor(state, word + 1, P25P1_SYNC_DIBITS - 1);
    }
    /* The window that matched ends on the latest symbol. */
    if (state->symbol_clock < (uint64_t)P25P1_SYNC_DIBITS || dsd_frame_sync_off_thread_tune_pending()) {
        return;
    }
    frame_sync_p25_sync_context_now(state, state->symbol_clock - (uint64_t)P25P1_SYNC_DIBITS, &rec->frame);
    rec->frame.inverted = synctype == DSD_SYNC_P25P1_NEG ? 1U : 0U;
    rec->frame_valid = 1U;
}

void
frame_sync_p25p1_anchor_note_accept(dsd_state* state, int synctype) {
    /* The window the anchor scores is the next data unit's first: any sync accepted before it reached that position
       says the stream is not the one the anchor measured, and one accepted at or after it is that window's. */
    state->p25_p1_sync_anchor.valid = 0U;
    if (!DSD_SYNC_IS_P25P1(synctype)) {
        state->p25_p1_sync_anchor.frame_valid = 0U;
        state->p25_p1_sync_anchor.frame_tolerant = 0U;
    }
}

int
frame_sync_p25p1_anchor_take(dsd_state* state, uint8_t* out_map_idx, int* out_inverted, int* out_nac) {
    dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    /* The cheap gate first: an integer compare a window. */
    if (!rec->valid || state->symbol_clock < rec->expected_clock + (uint64_t)P25P1_SYNC_DIBITS) {
        return 0;
    }
    /* The hunt has reached the expected position (or passed it unscored): one window per frame is scored, so the
       anchor is spent either way, and the rest of the hunt is exact. */
    rec->valid = 0U;
    if (state->symbol_clock != rec->expected_clock + (uint64_t)P25P1_SYNC_DIBITS
        || state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4
        || !p25p1_anchor_context_current(state, &rec->anchor)) {
        return 0;
    }
    *out_map_idx = rec->anchor.map_idx;
    *out_inverted = rec->anchor.inverted;
    *out_nac = rec->nac;
    return 1;
}

int
dsd_frame_sync_p25p1_tolerant_nac(const dsd_state* state, unsigned int* out_bit_errors) {
    const dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    if (out_bit_errors != NULL) {
        *out_bit_errors = rec->sync_bit_errors;
    }
    return rec->frame_tolerant ? (int)rec->frame_nac : -1;
}

int
dsd_frame_sync_p25p1_frame_context_holds(const dsd_state* state) {
    const dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    return rec->frame_valid && p25p1_anchor_context_current(state, &rec->frame);
}

/* A frame's report, as the grid's arming rows read it. */
typedef struct {
    const dsd_state* state;
    const dsd_p25p1_sync_anchor* rec;
    uint32_t unit_dibits;
} p25p1_grid_report;

/* The unit's end on the symbol clock: where the next unit's sync starts. Only once the length is known. */
static uint64_t
p25p1_grid_unit_end(const p25p1_grid_report* report) {
    return report->rec->frame.sync_clock + (uint64_t)report->unit_dibits;
}

static int
p25p1_grid_not_c4fm(const p25p1_grid_report* report) {
    return report->state->rf_mod != 0;
}

static int
p25p1_grid_no_length(const p25p1_grid_report* report) {
    return report->unit_dibits == 0U;
}

static int
p25p1_grid_no_stream(const p25p1_grid_report* report) {
    return !report->rec->frame_valid || !p25p1_anchor_context_current(report->state, &report->rec->frame);
}

static int
p25p1_grid_read_past(const p25p1_grid_report* report) {
    return report->state->symbol_clock > p25p1_grid_unit_end(report);
}

static int
p25p1_grid_short_read(const p25p1_grid_report* report) {
    return p25p1_grid_unit_end(report) - report->state->symbol_clock > (uint64_t)P25P1_GRID_MAX_TAIL_DIBITS;
}

/* Whether a reported frame arms the grid's recording of the next unit's sync word, one row per way it does not, in
   order; a frame that passes them all arms it. The units themselves are the dispatcher's rows (dispatch_p25p1.c): a
   fixed length (HDU, LDU1, LDU2, TDU, TDULC), a TSDU's from a last block that passed its CRC, a PDU's from a header
   that passed its CRC; a TSDU the trailing-null rule or a failed block ended, a NID that failed, was held or was cut
   reports no length. */
static const struct {
    int (*refuses)(const p25p1_grid_report* report);
    dsd_p25p1_grid_verdict verdict;
} k_p25p1_grid_rows[] = {
    /* Only the C4FM chain reads samples on a grid the hunt's crossing loop slips. */
    {p25p1_grid_not_c4fm, DSD_P25P1_GRID_NOT_C4FM},
    /* Where the next sync starts is not known. */
    {p25p1_grid_no_length, DSD_P25P1_GRID_NO_LENGTH},
    /* No sync-time context, or the stream moved since the sync: a handler's tune (a grant, a release, a RESET), a
       carrier boundary, a flush, a new input, a profile or modulation change, an off-thread tune pending. */
    {p25p1_grid_no_stream, DSD_P25P1_GRID_NO_STREAM},
    /* The decoder read into the next unit: its sync is behind the grid. */
    {p25p1_grid_read_past, DSD_P25P1_GRID_READ_PAST},
    /* More than the unit's pad is left: a PDU whose header announced blocks its reader did not collect, or a header
       recovered after the read that names more than was read. The recording would lapse before the word. */
    {p25p1_grid_short_read, DSD_P25P1_GRID_SHORT_READ},
};

static dsd_p25p1_grid_verdict
p25p1_grid_verdict(const p25p1_grid_report* report) {
    for (size_t i = 0; i < sizeof(k_p25p1_grid_rows) / sizeof(k_p25p1_grid_rows[0]); i++) {
        if (k_p25p1_grid_rows[i].refuses(report)) {
            return k_p25p1_grid_rows[i].verdict;
        }
    }
    return DSD_P25P1_GRID_ARMED;
}

dsd_p25p1_grid_verdict
dsd_frame_sync_p25p1_frame_decoded(dsd_state* state, int nac, uint32_t unit_dibits) {
    dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    const int captured = rec->frame_valid != 0U;
    const p25p1_grid_report report = {state, rec, unit_dibits};
    const dsd_p25p1_grid_verdict grid = p25p1_grid_verdict(&report);
    if (grid == DSD_P25P1_GRID_ARMED) {
        dsd_symbol_arm_hunt_sync_anchor(state);
    }
    if (rec->frame_tolerant && nac >= 0) {
        rec->tolerant_frames++;
    }
    rec->frame_valid = 0U;
    rec->frame_tolerant = 0U;
    if (!captured || unit_dibits == 0U || nac <= 0x000 || nac >= 0xFFF) {
        return grid;
    }
    /* The sync-time context must still hold: a tune the frame's decoder made, a flush, a profile step since its sync
       all move the stream off the position measured. So does a decoder that read past the unit's end, after which the
       hunt starts beyond the expected position. */
    const uint64_t end = rec->frame.sync_clock + (uint64_t)unit_dibits;
    if (!p25p1_anchor_context_current(state, &rec->frame) || state->symbol_clock > end) {
        return grid;
    }
    rec->anchor = rec->frame;
    rec->expected_clock = end;
    rec->nac = (uint16_t)nac;
    rec->valid = 1U;
    return grid;
}
