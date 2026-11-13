// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The P25 Phase 2 sync anchor: a sync word within a few bit errors, where the window after the last one a
 * received I-ISCH placed must start (dsd_p25p2_sync_anchor).
 *
 * The hunt matches the 20-dibit P25p2 sync word (the S-ISCH) exactly. On a simulcast carrier one symbol of it past a
 * slicer threshold loses the window behind it: processP2() reads four timeslots after each sync. On a continuous carrier
 * the next window's sync ends 20 dibits after a window, so after a window whose own I-ISCH said where in the superframe
 * it stood, the hunt knows where the next sync must be and which timeslot the window behind it starts at, and on the RTL
 * CQPSK 6000/4 path it accepts a word there within DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS bit errors. That window then
 * dispatches only when its own ISCHs and DUIDs agree with that start (the window verdict, processP2()): until then it
 * earns and publishes nothing.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_P25P2_SYNC_ANCHOR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_P25P2_SYNC_ANCHOR_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bit errors (of the word's 40) a P25p2 sync word at the anchor's expected position may have and be accepted.
 *
 * OP25's Phase 2 framer and sdrtrunk's hunting detector accept 4. Exact syncs a slip of 1 to
 * DSD_P25P2_TOLERANT_SYNC_NO_STEAL_SHIFTS symbols put after the expected position are not pre-empted.
 */
#define DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS  4

/**
 * @brief The slips (in symbols) after the expected position an exact P25p2 sync word is never pre-empted over.
 *
 * Under the anchor's map and polarity, the word's distance to itself shifted by 1 to 12 symbols is at least 5 bits (7,
 * 8, 7, 7, 6 for 1 to 5) even when the symbols shifted in match, so the expected-position window then lies more than 4
 * bits off and the exact word is matched where it is. Under another map or the other polarity it is not: shifted 10 to
 * 12 symbols, the word's head under the P1200 map or inverted lies 1 to 4 bits from the word's tail, and the symbols
 * before it may match; nor does it hold for another protocol's word. So a window within the tolerance is refused too
 * when it ends in the head of an exact word of any matcher that runs there, under each map and polarity it reads it,
 * starting 1 to 12 symbols after the expected position (frame_sync_window_ends_in_exact_head()).
 */
#define DSD_P25P2_TOLERANT_SYNC_NO_STEAL_SHIFTS 12

/**
 * @brief Bit errors between the 20-dibit window @p window, read under CQPSK dibit map @p map_idx, and the P25p2 sync
 * word (its inverted form when @p inverted), counted as dsd_p25p1_sync_bit_distance() counts them. Returns 40 when
 * @p window holds fewer than 20 dibits.
 */
int dsd_p25p2_sync_bit_distance(const char* window, uint8_t map_idx, int inverted);

/**
 * @brief The superframe timeslot (0-11) the window being decoded must start at, when its sync was accepted within the
 * tolerance; -1 after an exact sync.
 *
 * @p out_bit_errors (may be NULL) receives the bits that window's sync word was off by: 0 after an exact (normal,
 * inverted or rotated) match.
 */
int dsd_frame_sync_p25p2_tolerant_start(const dsd_state* state, unsigned int* out_bit_errors);

/**
 * @brief Whether the stream the window being decoded was read on is still the one its sync was accepted on: the same
 * clock epoch, input, carrier, tuning generation, sync history drops, SPS profile and modulation, no off-thread tune
 * pending. 0 when no sync-time context was captured.
 */
int dsd_frame_sync_p25p2_window_context_holds(const dsd_state* state);

/**
 * @brief Tell the frame sync what the P25p2 window its last sync opened was, once processP2() is done with it.
 *
 * @p dispatched is 0 for a window opened by a tolerant sync whose verdict failed (nothing of it dispatched), or which an
 * input interruption or a carrier boundary cut; 1 otherwise. @p anchor_start is the superframe timeslot the window
 * started at when it may anchor the next one (its position proven and not contradicted, an I-ISCH of its own matching
 * that start and none contradicting it, no retune while it was dispatched), -1 otherwise: the anchor is installed when
 * the window's sync-time context still holds and the decoder has not read past the window. Either way the window's sync
 * context is spent. A window behind a tolerant sync reported dispatched counts in
 * dsd_p25p2_sync_anchor::tolerant_windows.
 */
void dsd_frame_sync_p25p2_window_decoded(dsd_state* state, int dispatched, int anchor_start);

/**
 * @brief The window a tolerant sync opened failed its verdict, or a carrier boundary or an input interruption cut it
 * before its verdict: what the accept changed goes back.
 *
 * processP2() calls it before it reports the window (dsd_frame_sync_p25p2_window_decoded()). On the stream the sync was
 * accepted on (dsd_frame_sync_p25p2_window_context_holds()), what the accept wrote before the verdict could say whether
 * the window was one at all goes back as it was (dsd_p25p2_sync_candidate: the lock, the slicer extremes and the
 * references derived from them, lastsynctype, dsd_opts::inverted_p2, the CQPSK dibit map). No-op behind an exact sync.
 */
void dsd_frame_sync_p25p2_window_dropped(dsd_opts* opts, dsd_state* state);

/**
 * @brief What an accepted P25p2 sync says about the channel: on a followed voice channel the trunk state machine's
 * VC-sync hook, otherwise the control channel's last sync time. The frame sync notes it when it accepts an exact sync;
 * processP2() notes it for a tolerant one once the window's verdict passes.
 */
void dsd_frame_sync_p25p2_note_sync_activity(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_P25P2_SYNC_ANCHOR_H_ */
