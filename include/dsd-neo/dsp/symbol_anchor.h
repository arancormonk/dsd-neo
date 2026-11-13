// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Sync-anchored correction of the open-loop FSK symbol grid inside a frame.
 *
 * The grid only slips on zero crossings while it hunts for a sync
 * (symbol_adjust_timing_index()). A decoder that reads burst after burst without
 * going back to the hunt -- DMR BS does for the length of a repeater call -- left
 * it open loop, so any offset between the transmitter's symbol clock and the
 * sample clock walked the sampling instant off the symbols until the burst
 * reader lost the next sync (issue #729).
 *
 * A protocol that reads a sync word it knows inside a frame names it here. One
 * symbol later, with room to look either side, the samples the grid decided
 * those symbols on are correlated against the sync word's levels at every
 * sub-symbol shift, and the grid moves one sample towards the best fit when that
 * fit is more than half a sample away. One sample per sync word bounds the
 * correction, and a sync word averages 24 symbols, so noise that makes the
 * per-crossing slip chatter does not move it.
 */

#ifndef DSD_NEO_DSP_SYMBOL_ANCHOR_H
#define DSD_NEO_DSP_SYMBOL_ANCHOR_H

#ifdef __cplusplus
extern "C" {
#endif

struct dsd_state;

/** Longest sync word an anchor takes, in symbols. */
#define DSD_SYMBOL_ANCHOR_MAX_SYMS 48

/**
 * @brief Start recording for a sync word that is about to be read.
 *
 * The grid records the samples a measurement needs only between this call and the measurement (or 64 symbols, if no
 * sync word is named), so a decoder pays for the anchor around its sync words, not on every sample. Call it at least
 * one symbol before the sync word's first symbol is read, so the measurement has samples to shift into.
 */
void dsd_symbol_arm_sync_anchor(struct dsd_state* state);

/**
 * @brief Start recording for a sync word the frame-sync hunt is about to read, and hold the grid for it.
 *
 * NXDN, YSF, DMR MS and P25 Phase 1 on C4FM read a frame or burst, then go back to the hunt for the next one's sync
 * word. The hunt's crossing loop slips the grid a sample at a time on that word's zero crossings, and a fixed word's
 * crossings pull it to a fixed place: the inner symbols in NXDN's and YSF's words (and an MS burst's first crossings
 * after the gap) sat clean frames one or two samples off their symbols, and the alternating crossings that end P25's
 * word, acted on a symbol late, a sample past them, a phase the frame then kept. Called where the frame or burst ends,
 * right ahead of the next word, this arms a recording as dsd_symbol_arm_sync_anchor() does and, once a measured word
 * has placed the grid, keeps the crossing loop off it until the word is measured or the recording lapses (64 symbols),
 * so the word is read on the grid the frame left and the measurement corrects that grid alone, as on a DMR BS call. A
 * grid no word placed (the first frame after acquisition, a recording that lapsed or could not be measured, a word
 * named with no recording armed, a reset) stays the crossing loop's. The hold is the arming protocol's (NXDN, YSF, DMR
 * or P25 Phase 1, from dsd_state::lastsynctype now): once lastsynctype names another protocol, the hunt having accepted
 * its sync, the hold ends. The decoder that accepts the word names it (dsd_symbol_note_sync_anchor()).
 */
void dsd_symbol_arm_hunt_sync_anchor(struct dsd_state* state);

/**
 * @brief Keep a recording going across a run that announces a sync word without saying when it comes.
 *
 * M17's preamble runs for up to 192 symbols ahead of its LSF or BERT sync word. Called once a symbol while the run
 * lasts, this arms a recording that is not running and keeps a running one from lapsing, without restarting it, so
 * the sync word's measurement has the run's last symbols to work with.
 */
void dsd_symbol_hold_sync_anchor(struct dsd_state* state);

/**
 * @brief Name the sync word the symbol grid just read.
 *
 * The last @p count symbols getSymbol() returned were @p sync_dibits ('0'..'3',
 * oldest first). A later symbol measures the grid against them and may move it
 * by up to two samples. Ignored on a grid without sub-symbol structure (one
 * sample per symbol), with a count outside 1..DSD_SYMBOL_ANCHOR_MAX_SYMS, without
 * a recording armed by dsd_symbol_arm_sync_anchor(), or before it holds that
 * many symbols. The measurement needs a symbol of samples before the first named
 * centre, so a word whose recording starts on its first symbol is named from its
 * second.
 */
void dsd_symbol_note_sync_anchor(const struct dsd_state* state, const char* sync_dibits, int count);

/**
 * @brief Name the P25 Phase 1 sync word the symbol grid just read (C4FM), as dsd_symbol_note_sync_anchor() does.
 *
 * The word is measured against the grid its symbols would have had without the slips the hunt's crossing loop took
 * while reading it, ending at its last centre: the first word after acquisition, a lapse or a reset is read on a grid
 * nothing holds, and the frame behind it goes on from that last centre. Only the P25 Phase 1 frame sync calls it; every
 * other caller's word is measured on the centres it was read on, whatever dsd_state::lastsynctype says.
 *
 * With no recording armed (the word that acquires a transmission, or the first after a lapse or a reset), the word is
 * measured on the raw samples the matched-filter seam keeps (dsd_matched_filter_seam), at a whole number of samples a
 * symbol, and the grid moves at the next symbol when the word sits at least three quarters of a sample off it (a dead
 * zone on the offset).
 */
void dsd_symbol_note_p25p1_sync_anchor(struct dsd_state* state, const char* sync_dibits, int count);

/**
 * @brief Where a known run of symbols sits against the grid that decided it.
 *
 * @param samples            Post-filter samples, oldest first.
 * @param sample_count       Number of samples.
 * @param centres            Index into @p samples of the sample the grid took as each symbol's centre.
 * @param dibits             The symbols' dibits ('0'..'3').
 * @param count              Number of symbols.
 * @param samples_per_symbol The grid's samples per symbol.
 * @param out_offset         Where the symbols' centres sit relative to the grid's, in samples (positive: later),
 *                           refined between samples. The levels' sign is taken from the data, so an inverted
 *                           signal measures the same.
 * @return 1 when @p out_offset was written; 0 when the samples cannot place the symbols (too few samples around
 *         them, a grid without sub-symbol structure, a flat trace).
 */
int dsd_symbol_anchor_measure_offset(const float* samples, int sample_count, const int* centres, const char* dibits,
                                     int count, int samples_per_symbol, double* out_offset);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_DSP_SYMBOL_ANCHOR_H */
