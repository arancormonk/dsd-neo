// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The P25 Phase 1 sync anchor: a sync word within a few bit errors, where the last validated frame says the
 * next data unit starts (dsd_p25p1_sync_anchor).
 *
 * The hunt matches the 24-dibit P25p1 sync word exactly (normal, inverted, or under a rotated CQPSK dibit map). On a
 * simulcast carrier one symbol of the word past a slicer threshold loses the whole frame. On a continuous outbound
 * carrier every data unit ends on a status symbol and the next one's sync starts right after it (TIA-102.BAAA-A 8.2),
 * so after a validated frame the hunt knows where the next sync must start, and on the RTL CQPSK 4800/4 path it accepts
 * a word there within DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS bit errors. On the C4FM chain, whose hunt reads only
 * each symbol's sign, it accepts one there within DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS sign errors. That frame then
 * decodes only when its NID passes a stricter check than an exact sync's (the dispatcher's): until then it earns and
 * publishes nothing, and what its accept changed is put back when it fails (dsd_frame_sync_p25p1_candidate_rejected()).
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_P25P1_SYNC_ANCHOR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_P25P1_SYNC_ANCHOR_H_

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bit errors (of the word's 48) a P25p1 sync word at the anchor's expected position may have and be accepted.
 *
 * OP25's dedicated P1 framer (p25_framer.cc) accepts 6. Exact syncs a slip of 1 to
 * DSD_P25P1_TOLERANT_SYNC_NO_STEAL_SHIFTS symbols put after the expected position are not pre-empted.
 */
#define DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS  6

/**
 * @brief The slips (in symbols) after the expected position an exact P25p1 sync word is never pre-empted over.
 *
 * Under the anchor's map and polarity, the word's distance to itself shifted by 1 to 13 symbols is at least 7 bits (9,
 * 10, 9, 8 and 9 for 1 to 5) even when the symbols shifted in match, so the expected-position window then lies more
 * than 6 bits off and the exact word is matched where it is. Under another map or the other polarity it is not:
 * shifted 11 to 13 symbols, the word's head under the P1200 map or inverted lies 3 to 5 bits from the word's tail, and
 * the symbols before it may match; nor does it hold for another protocol's word (an X2-TDMA base voice word five
 * symbols late, behind the anchor word's first five, lies 6 bits off). So a window within the tolerance is refused too
 * when it ends in the head of an exact word of any matcher that runs there, under each map and polarity it reads it,
 * starting 1 to 13 symbols after the expected position (frame_sync_window_ends_in_exact_head()).
 */
#define DSD_P25P1_TOLERANT_SYNC_NO_STEAL_SHIFTS 13

/**
 * @brief Sign errors (of the word's 24 symbols) a P25p1 sync word at the anchor's expected position may have on the C4FM
 * chain and be accepted.
 *
 * The C4FM hunt slices each symbol by its sign alone (every symbol of the word is an outer one), so the word read there
 * is 24 signs, not 48 bits. Every symbol of the word being an outer one, a sign error is one bit, so the word's
 * distance to itself shifted by 1 to 13 symbols is the same 7 or more as on the CQPSK chain even when the symbols
 * shifted in match: a slipped exact word is matched where it is, and the exact-word guard
 * (frame_sync_window_ends_in_exact_head()) covers the other polarity and the other protocols' words, as there.
 */
#define DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS 6

/**
 * @brief Sign errors (of 24) a P25p1 sync word may have anywhere on the C4FM chain's 4800/4 hunt, with no anchor, and be
 * accepted as an acquisition candidate, either polarity.
 *
 * On receiver noise a window lies within one sign of either polarity's word once in about 336,000 (25 of 2^24 each), so
 * the hunt meets such a window about 51 times an hour on the 4800/4 profile; the strict NID behind it is what keeps
 * those from decoding (the budget in docs/code_map.md).
 */
#define DSD_P25P1_ACQUISITION_MAX_SIGN_ERRORS   1

/**
 * @brief The slips (in symbols) after an acquisition candidate's first symbol a P25p1 sync word of its own polarity is
 * never pre-empted over: every one that overlaps it.
 *
 * Anywhere on the hunt nothing says where the word must start, so a window one sign off can be a real word's head with
 * the symbols before it: the window's last 1 to 23 symbols are then that word's first. Past the 13 the exact-word guard
 * covers (DSD_P25P1_TOLERANT_SYNC_NO_STEAL_SHIFTS), the window's tail is the word's head for 1 to 10 symbols, which the
 * word's own tail is 1 to 5 signs from, so within one sign of the word it is only for one tail symbol: a word whose one
 * sign error is its last symbol is refused, and a real word starting 14 to 23 symbols on is never swallowed by the
 * candidate's NID read.
 */
#define DSD_P25P1_ACQUISITION_NO_STEAL_SHIFTS   23

/** How the hunt accepted the sync word of the P25p1 frame being decoded, which says what its NID must pass. */
typedef enum {
    /** Matched exactly (normal, inverted, or rotated on the CQPSK chain): the NID's own rules. */
    DSD_P25P1_SYNC_EXACT = 0,
    /** Within the tolerance at the anchor's expected position: the strict NID (p25p1_nid_verdict_passes_tolerant_sync()),
        naming the anchor's NAC. */
    DSD_P25P1_SYNC_ANCHORED,
    /** Within one sign error anywhere on the C4FM hunt, with no anchor: the strict NID (p25p1_nid_verdict_strict()),
        whatever NAC it names. */
    DSD_P25P1_SYNC_ACQUISITION,
} dsd_p25p1_sync_kind;

/**
 * @brief Bit errors between the 24-dibit window @p window, read under CQPSK dibit map @p map_idx, and the P25p1 sync
 * word (its inverted form when @p inverted).
 *
 * Each dibit counts the bits it differs by (P25's dibits are Gray coded over the four levels: +3 01, +1 00, -1 10, -3
 * 11), so one level off counts 1 and a sign error 1 or 2. A byte outside '0'..'3' counts 2. Returns 48 when @p window
 * holds fewer than 24 dibits.
 */
int dsd_p25p1_sync_bit_distance(const char* window, uint8_t map_idx, int inverted);

/**
 * @brief Sign errors between the 24-symbol C4FM hunt window @p window ('1' positive, '3' negative) and the P25p1 sync
 * word (its inverted form when @p inverted). A byte other than the word's counts as an error; 24 when @p window holds
 * fewer than 24 symbols.
 */
int dsd_p25p1_sync_sign_distance(const char* window, int inverted);

/**
 * @brief How the hunt accepted the sync word of the frame being decoded (DSD_P25P1_SYNC_EXACT once it was reported).
 *
 * @p out_anchor_nac (may be NULL) receives the NAC an anchored frame's NID must name, -1 for the other kinds.
 * @p out_errors (may be NULL) receives what the word was off by: bits on the CQPSK chain, signs on the C4FM chain, 0
 * after an exact match.
 */
dsd_p25p1_sync_kind dsd_frame_sync_p25p1_sync_kind(const dsd_state* state, int* out_anchor_nac,
                                                   unsigned int* out_errors);

/**
 * @brief The dispatcher dropped the frame behind a tolerant sync word: its NID failed the strict check.
 *
 * On either chain, while the stream is still the one the word was accepted on, what the accept changed before the NID
 * could speak goes back as it was (dsd_p25p1_sync_candidate): the slicer levels its warm start and lock wrote (the NID
 * was read on them) and the crossing the warm start dropped, the carrier flag, offset and lastsynctype, the P25p1 frame
 * span that stands NXDN96 and M17 down, the DMR slot 2 fields and frame label the accept writes, the dibit map and the
 * frame NAC. On the C4FM chain the hunt then starts from the symbols the word and its NID took, so a sync word the
 * candidate swallowed whose end is still to come is matched where it is (dsd_frame_sync_history_carry). Call before
 * dsd_frame_sync_p25p1_frame_decoded(), which spends the frame's context.
 */
void dsd_frame_sync_p25p1_candidate_rejected(dsd_state* state);

/**
 * @brief The dispatcher keeps the frame behind a tolerant sync word: its NID passed the strict check.
 *
 * On the C4FM chain a word read with no recording armed (the word that acquires a transmission, or the first after a
 * lapse or a reset) is measured now against the grid on the matched-filter seam's raw history, as an exact one is when
 * it is accepted (dsd_symbol_note_p25p1_marked_sync_anchor()), and the grid moves at the next symbol: the frame's data
 * after the NID is read on the word's symbols. Until here the word may be no word at all, so the accept measured
 * nothing, and a frame dropped behind it leaves the grid as the hunt left it. Call once the NID is read, before
 * dsd_frame_sync_p25p1_frame_decoded(), which spends the frame's context; nothing for an exact word.
 */
void dsd_frame_sync_p25p1_candidate_validated(dsd_state* state);

/**
 * @brief Whether the stream the P25p1 frame being decoded was read on is still the one its sync was accepted on: the
 * same clock epoch, input, carrier, tuning generation, sync history drops, SPS profile and modulation, no off-thread
 * tune pending. 0 when no sync-time context was captured, and once the frame was reported
 * (dsd_frame_sync_p25p1_frame_decoded()).
 */
int dsd_frame_sync_p25p1_frame_context_holds(const dsd_state* state);

/** What the report of a P25p1 frame (dsd_frame_sync_p25p1_frame_decoded()) did about the symbol grid's recording of
    the next data unit's sync word: armed it, or the first reason it did not. */
typedef enum {
    /** Armed (dsd_symbol_arm_hunt_sync_anchor()): the next unit's sync word places the grid. */
    DSD_P25P1_GRID_ARMED = 0,
    /** Not read on the C4FM chain, whose grid alone the hunt's crossing loop slips. */
    DSD_P25P1_GRID_NOT_C4FM,
    /** The frame did not validate, or its length on air is not known. */
    DSD_P25P1_GRID_NO_LENGTH,
    /** No sync-time context was captured, or the stream is no longer the one the sync was accepted on. */
    DSD_P25P1_GRID_NO_STREAM,
    /** The decoder read past the unit's end. */
    DSD_P25P1_GRID_READ_PAST,
    /** The decoder left more of the unit unread than its null pad and last status symbol. */
    DSD_P25P1_GRID_SHORT_READ,
} dsd_p25p1_grid_verdict;

/**
 * @brief Tell the frame sync what the P25p1 frame its last sync opened was, once decoded.
 *
 * When the frame validated, @p nac is the NAC its NID named and @p unit_dibits its length on air (sync word and status
 * symbols included): the anchor is installed at its sync plus that length, when the frame's sync-time context still
 * holds (the same clock epoch, input, carrier, tuning generation, sync history drops, SPS profile and modulation, no
 * off-thread tune pending) and the decoder has not read past that position. 0 for @p unit_dibits when the frame did
 * not validate or its length is not known: nothing is installed. Either way the frame's sync context is spent.
 * @p nac is -1 when the frame decoded no NID it may take (an input interruption cut it, or it was dropped behind a
 * tolerant sync); a frame behind a tolerant sync reported with a NAC passed the strict check, and counts in
 * dsd_p25p1_sync_anchor::tolerant_frames (anchored) or acquisition_frames.
 *
 * On the C4FM chain the same report arms the symbol grid's recording of the next unit's sync word, whatever the NAC,
 * when the frame validated with a known length on the stream its sync was accepted on and its decoder stopped at the
 * unit's end or inside its null pad: that word, which the frame sync names to the grid as it accepts it, then places
 * the grid for the unit behind it. Returns what the report did about that recording.
 */
dsd_p25p1_grid_verdict dsd_frame_sync_p25p1_frame_decoded(dsd_state* state, int nac, uint32_t unit_dibits);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_P25P1_SYNC_ANCHOR_H_ */
