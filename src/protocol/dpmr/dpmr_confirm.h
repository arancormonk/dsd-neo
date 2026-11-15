// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether a dPMR frame has proved itself well enough to be acted on.
 *
 * Everything upstream of this is weak. The sync search slices on sign alone, so the
 * 12-symbol FS2 word matches about one window in 2048 of receiver noise -- roughly one
 * false frame a second at 2400 baud -- and there is no repeat guard behind it. What the
 * frame body then offered was not a check: identity publishing rode on
 * dpmr_ids_are_strong(), which accepts a CCH whose two leading Hamming(12,8) blocks
 * merely report correctable. Thirteen of the sixteen syndromes are correctable, so a
 * random block passes about 81% of the time and the predicate passes (13/16)^4, about
 * 44%, of noise superframes. Voice playback was gated on less still: a decoded 3-bit
 * CommunicationMode landing in {0, 1, 5}, three values in eight. On an open squelch the
 * result was invented talkgroups, invented source IDs, and synthesized speech (#407).
 *
 * The CCH CRC-7 is the check that was there all along. It covers the 41 payload bits
 * behind all six Hamming blocks, so passing it means the whole half decoded, not just
 * the two blocks the old predicate looked at. One half passing is one chance in 128 --
 * enough to be worth something, not enough to stand alone -- so it counts as weak and
 * has to repeat, two frames running, which a real transmission does continuously and
 * noise does not. "Running" means the second frame's FS2 follows the first's by one frame
 * (dpmr_confirm_note_frame_start()): the streak once counted any two dPMR frames with no
 * carrier reset between them, and under Auto, where noise hands 2400/4 hundreds of FS2
 * matches an hour between other protocols' syncs, two weak halves seconds apart confirmed
 * a transmission of noise, published a call and decoded its voice. Both halves of one
 * frame passing is one in 16384 and confirms at once. At the noise sync rate that is a
 * false confirmation roughly once an hour, against the 44% of superframes that used to
 * publish.
 *
 * The Hamming flags contribute no evidence at any strength. The CRC is computed over the
 * bits they produced, so it already subsumes them.
 *
 * Note that an all-zero CCH passes CRC-7, the zero codeword being a valid one. Reaching
 * it from the air needs 72 received bits equal to the scrambler keystream, which is
 * 2^-72 on noise, so it is not a way in; it is why the zero-stream case in
 * DPMR_VOICE_BRIDGE confirms.
 *
 * Sibling of src/protocol/nxdn/nxdn_confirm.{c,h}, which does the same job for NXDN.
 */

#ifndef DSD_NEO_PROTOCOL_DPMR_DPMR_CONFIRM_H
#define DSD_NEO_PROTOCOL_DPMR_DPMR_CONFIRM_H

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How much a passing check is worth. */
typedef enum {
    DPMR_EVIDENCE_WEAK = 1,  /**< One CCH half passed its CRC-7: one chance in 128. */
    DPMR_EVIDENCE_STRONG = 2 /**< Both halves of one frame passed: one chance in 16384. */
} dpmr_evidence;

/** @brief Frames of weak evidence in a row that together confirm a transmission. */
#define DPMR_CONFIRM_WEAK_OBSERVES      2U

/** @brief Symbols from one FS2 frame's start to the next's in a transmission (a 160 ms superframe part at 2400 sym/s),
 * and the slip either way a run allows. */
#define DPMR_CONFIRM_FRAME_SYMBOLS      384U
#define DPMR_CONFIRM_FRAME_SYMBOLS_SLIP 1U

/** @brief Forget everything learned about the current transmission. */
void dpmr_confirm_reset(dsd_state* state);

/**
 * @brief Say where an FS2 frame whose CCH is about to be read begins, before dpmr_confirm_begin_frame().
 *
 * @p frame_symbol is dsd_state::symbolcnt at the same point of every frame. A frame runs on only from one read
 * DPMR_CONFIRM_FRAME_SYMBOLS (within the slip) before it; one that does not starts the weak streak over, so two weak
 * frames confirm only when the second follows the first. Without this call a frame runs on (the unit tests' frames,
 * one after another).
 */
void dpmr_confirm_note_frame_start(dsd_state* state, uint32_t frame_symbol);

/** @brief Start a frame's evidence accounting; call once before decoding its CCH halves. */
void dpmr_confirm_begin_frame(dsd_state* state);

/**
 * @brief Report a check this frame passed.
 *
 * Strong evidence confirms immediately. Weak evidence counts once per frame however many
 * halves pass, and confirms on the DPMR_CONFIRM_WEAK_OBSERVES'th frame in a row.
 */
void dpmr_confirm_note_evidence(dsd_state* state, dpmr_evidence evidence);

/**
 * @brief Close a frame's accounting; a frame that proved nothing breaks the weak streak.
 *
 * A frame whose own evidence confirmed the transmission is reported to a signal search here,
 * once, after the frame has published what its confirmation unlocked: the search reads the
 * call and colour code from state when it hears it.
 */
void dpmr_confirm_end_frame(dsd_state* state);

/** @brief Whether the current transmission has proved itself. */
int dpmr_confirm_is_confirmed(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_DPMR_DPMR_CONFIRM_H */
