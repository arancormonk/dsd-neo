// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The P25 Phase 1 sync anchor (dsd_p25p1_sync_anchor, <dsd-neo/dsp/p25p1_sync_anchor.h>): its sync-time context, its
 * installation, the position the hunt scores a tolerant word at, and the distances it scores. The matchers that accept
 * a tolerant word are frame_sync_try_tolerant_p25p1() and frame_sync_try_acquisition_p25p1() in dsd_frame_sync.c. On the C4FM
 * chain the same sync and the same report place the symbol grid on each data unit's sync word
 * (<dsd-neo/dsp/symbol_anchor.h>), and a tolerant candidate's accept is held until its NID speaks
 * (dsd_p25p1_sync_candidate). */

#include <dsd-neo/core/safe_api.h>
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

int
dsd_p25p1_sync_sign_distance(const char* window, int inverted) {
    const char* word = inverted ? INV_P25P1_SYNC : P25P1_SYNC;
    int errors = 0;
    for (int i = 0; i < P25P1_SYNC_DIBITS; i++) {
        if (window == NULL || window[i] == '\0') {
            return P25P1_SYNC_DIBITS;
        }
        errors += window[i] != word[i];
    }
    return errors;
}

/* Whether @p ctx still describes the stream (frame_sync_p25_sync_context_holds()) with no off-thread tune awaiting
   reconciling. */
static int
p25p1_anchor_context_current(const dsd_state* state, const dsd_p25_sync_context* ctx) {
    return frame_sync_p25_sync_context_holds(state, ctx) && !dsd_frame_sync_off_thread_tune_pending();
}

void
frame_sync_p25p1_anchor_note_sync(dsd_state* state, int synctype, int kind, int anchor_nac, int errors) {
    dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    /* How the sync was accepted stands whatever becomes of the context: a tolerant one's frame decodes only past the
       strict check, which nothing may skip. */
    rec->frame_kind = (uint8_t)kind;
    rec->frame_dropped = 0U;
    rec->frame_nac = kind == DSD_P25P1_SYNC_ANCHORED && anchor_nac >= 0 ? (uint16_t)anchor_nac : 0U;
    rec->sync_bit_errors = (uint8_t)errors;
    rec->frame_valid = 0U;
    /* On the C4FM chain the hunt's crossing loop leaves the grid a sample past the word's symbols, where the frame
       behind it would be read (docs/code_map.md, symbol anchor). The previous unit's report armed a recording at its
       end (dsd_frame_sync_p25p1_frame_decoded()), which starts on the word's first symbol, so the word is named from
       its second: the measurement needs a symbol of samples before the first centre it reads. The word that acquires
       a transmission has no recording behind it and is measured on the raw history the matched-filter seam keeps. A
       word within a tolerance may be no word at all until its NID passes: it is measured then, on that raw history,
       when it was read with no recording armed (dsd_frame_sync_p25p1_candidate_validated()); one read into a
       recording names nothing, as the recording has moved past it by then, onto the NID. */
    rec->frame_word.valid = 0U;
    if (state->rf_mod == 0) {
        if (kind == DSD_P25P1_SYNC_EXACT) {
            const char* word = synctype == DSD_SYNC_P25P1_NEG ? INV_P25P1_SYNC : P25P1_SYNC;
            dsd_symbol_note_p25p1_sync_anchor(state, word + 1, P25P1_SYNC_DIBITS - 1);
        } else {
            dsd_symbol_mark_p25p1_sync_anchor(state, P25P1_SYNC_DIBITS - 1, &rec->frame_word);
        }
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
        state->p25_p1_sync_anchor.frame_kind = (uint8_t)DSD_P25P1_SYNC_EXACT;
        state->p25_p1_sync_anchor.frame_dropped = 0U;
        state->p25_p1_sync_anchor.frame_word.valid = 0U;
        state->p25_p1_sync_candidate.valid = 0U;
    }
}

void
frame_sync_p25p1_candidate_hold(dsd_state* state, int pass_symbols) {
    dsd_p25p1_sync_candidate* held = &state->p25_p1_sync_candidate;
    held->pass_symbols = pass_symbols;
    held->max = state->max;
    held->min = state->min;
    held->center = state->center;
    held->umid = state->umid;
    held->lmid = state->lmid;
    held->maxref = state->maxref;
    held->minref = state->minref;
    DSD_MEMCPY(held->maxbuf, state->maxbuf, sizeof(held->maxbuf));
    DSD_MEMCPY(held->minbuf, state->minbuf, sizeof(held->minbuf));
    held->midx = state->midx;
    held->jitter = state->jitter;
    held->carrier = state->carrier;
    held->offset = state->offset;
    held->lastsynctype = state->lastsynctype;
    held->synctype = state->synctype;
    held->dmrburstR = state->dmrburstR;
    held->payload_algidR = state->payload_algidR;
    held->dmr_stereo = state->dmr_stereo;
    DSD_MEMCPY(held->ftype, state->ftype, sizeof(held->ftype));
    held->c4fm_frame_symbolcnt = state->p25_p1_c4fm_frame_symbolcnt;
    held->c4fm_frame_valid = state->p25_p1_c4fm_frame_valid;
    held->c4fm_sync_levels = state->p25_p1_c4fm_sync_levels;
    held->p25_cqpsk_dibit_map_idx = state->p25_cqpsk_dibit_map_idx;
    held->p25_frame_nac_valid = state->p25_frame_nac_valid;
    /* Every other field the accept writes before the NID is read goes here and in p25p1_candidate_restore(), slicer
       level state above all (docs/code_map.md, the C4FM item). */
    held->valid = 1U;
}

/* Put back what the dropped candidate's accept changed (dsd_p25p1_sync_candidate). */
static void
p25p1_candidate_restore(dsd_state* state, const dsd_p25p1_sync_candidate* held) {
    state->max = held->max;
    state->min = held->min;
    state->center = held->center;
    state->umid = held->umid;
    state->lmid = held->lmid;
    state->maxref = held->maxref;
    state->minref = held->minref;
    DSD_MEMCPY(state->maxbuf, held->maxbuf, sizeof(state->maxbuf));
    DSD_MEMCPY(state->minbuf, held->minbuf, sizeof(state->minbuf));
    state->midx = held->midx;
    dsd_state_invalidate_minmax_sums(state);
    state->jitter = held->jitter;
    state->carrier = held->carrier;
    state->offset = held->offset;
    state->lastsynctype = held->lastsynctype;
    state->synctype = held->synctype;
    state->dmrburstR = held->dmrburstR;
    state->payload_algidR = held->payload_algidR;
    state->dmr_stereo = held->dmr_stereo;
    DSD_MEMCPY(state->ftype, held->ftype, sizeof(state->ftype));
    state->p25_p1_c4fm_frame_symbolcnt = held->c4fm_frame_symbolcnt;
    state->p25_p1_c4fm_frame_valid = held->c4fm_frame_valid;
    state->p25_p1_c4fm_sync_levels = held->c4fm_sync_levels;
    state->p25_cqpsk_dibit_map_idx = held->p25_cqpsk_dibit_map_idx;
    state->p25_frame_nac_valid = held->p25_frame_nac_valid;
}

void
dsd_frame_sync_p25p1_candidate_validated(dsd_state* state) {
    dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    const dsd_matched_filter_seam_mark mark = rec->frame_word;
    rec->frame_word.valid = 0U;
    if (!mark.valid || rec->frame_kind == (uint8_t)DSD_P25P1_SYNC_EXACT || state->rf_mod != 0 || !rec->frame_valid
        || !p25p1_anchor_context_current(state, &rec->frame)) {
        return;
    }
    const char* word = rec->frame.inverted ? INV_P25P1_SYNC : P25P1_SYNC;
    (void)dsd_symbol_note_p25p1_marked_sync_anchor(state, &mark, word + 1, P25P1_SYNC_DIBITS - 1);
}

/* Drop the frame behind a tolerant word: marked dropped, and its accept put back on the stream it was accepted on. 0
   when nothing of that stream remains to go on from. */
static int
p25p1_candidate_drop(dsd_state* state) {
    dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    dsd_p25p1_sync_candidate* held = &state->p25_p1_sync_candidate;
    const int holding = held->valid != 0U;
    held->valid = 0U;
    /* The word is not measured: the grid stays where the hunt left it. */
    rec->frame_word.valid = 0U;
    /* Whatever chain it was read on, the frame synced nothing (dsd_state_p25p1_frame_dropped()). */
    rec->frame_dropped = 1U;
    /* Off the stream the word was accepted on (a tune, a flush, a new input) nothing of the old one is worth putting
       back, and its symbols are not the next call's to hunt. */
    if (!holding || rec->frame_kind == (uint8_t)DSD_P25P1_SYNC_EXACT || !rec->frame_valid
        || !p25p1_anchor_context_current(state, &rec->frame) || state->symbol_clock < rec->frame.sync_clock) {
        return 0;
    }
    p25p1_candidate_restore(state, held);
    return 1;
}

void
dsd_frame_sync_p25p1_candidate_cut(dsd_state* state) {
    (void)p25p1_candidate_drop(state);
}

void
dsd_frame_sync_p25p1_candidate_rejected(dsd_state* state) {
    const dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    if (!p25p1_candidate_drop(state)) {
        return;
    }
    /* The C4FM chain's hunt goes on from the word's and the NID's symbols: one where a real sync word ends after the NID
       (an NXDN96 FSW, an M17 word, the P25 word the candidate's head swallowed) is matched as the hunt goes on. One that
       ended inside them is lost (docs/code_map.md, the C4FM tolerance's budget). The carry rebuilds a sign history,
       which only the C4FM hunt reads: on the CQPSK chain the hunt starts over, as behind any other frame. */
    if (state->rf_mod != 0) {
        return;
    }
    const dsd_p25p1_sync_candidate* held = &state->p25_p1_sync_candidate;
    /* The symbols the hunt did not count (the window's last, which the accept returned on, and the NID's) are charged
       to it as its own and its pass goes on past them, so the profile's dwell and the no-sync exits fall where they
       would have without the candidate. */
    const uint64_t taken64 = state->symbol_clock - rec->frame.sync_clock;
    const int taken = taken64 > (uint64_t)INT32_MAX ? INT32_MAX : (int)taken64;
    const int read_after = taken >= P25P1_SYNC_DIBITS ? taken - (P25P1_SYNC_DIBITS - 1) : 0;
    state->sps_hunt_counter =
        state->sps_hunt_counter < INT32_MAX - read_after ? state->sps_hunt_counter + read_after : INT32_MAX;
    frame_sync_history_carry_symbols(
        state, taken, held->pass_symbols < INT32_MAX - read_after ? held->pass_symbols + read_after : INT32_MAX);
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

dsd_p25p1_sync_kind
dsd_frame_sync_p25p1_sync_kind(const dsd_state* state, int* out_anchor_nac, unsigned int* out_errors) {
    const dsd_p25p1_sync_anchor* rec = &state->p25_p1_sync_anchor;
    const dsd_p25p1_sync_kind kind = rec->frame_kind == (uint8_t)DSD_P25P1_SYNC_ANCHORED ? DSD_P25P1_SYNC_ANCHORED
                                     : rec->frame_kind == (uint8_t)DSD_P25P1_SYNC_ACQUISITION
                                         ? DSD_P25P1_SYNC_ACQUISITION
                                         : DSD_P25P1_SYNC_EXACT;
    if (out_anchor_nac != NULL) {
        *out_anchor_nac = kind == DSD_P25P1_SYNC_ANCHORED ? (int)rec->frame_nac : -1;
    }
    if (out_errors != NULL) {
        *out_errors = rec->sync_bit_errors;
    }
    return kind;
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
    if (rec->frame_kind == (uint8_t)DSD_P25P1_SYNC_ANCHORED && nac >= 0) {
        rec->tolerant_frames++;
    } else if (rec->frame_kind == (uint8_t)DSD_P25P1_SYNC_ACQUISITION && nac >= 0) {
        rec->acquisition_frames++;
    }
    rec->frame_valid = 0U;
    rec->frame_kind = (uint8_t)DSD_P25P1_SYNC_EXACT;
    rec->frame_word.valid = 0U;
    /* Validated or not, the frame is over: a candidate's accept is kept, or was put back already. */
    state->p25_p1_sync_candidate.valid = 0U;
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
