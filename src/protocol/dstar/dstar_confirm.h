// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether a D-STAR transmission has proved itself well enough to hold a hunt profile.
 *
 * processDSTAR() spends 1992 symbols on any voice sync and, before this module, could say
 * nothing about them: no return value, no error accumulator, no frame-count check. It
 * overwrites state->errs/errs2 twenty-one times and reads them never, and those are soft
 * AMBE correction counts in any case -- mbelib's Golay(23,12) maps every input to some
 * codeword, so there is no hard validity verdict hiding in them. On the 4800/2 hunt profile
 * D-STAR is the only candidate, so a false match held the profile for the largest block any
 * handler consumes (issues #391, #421).
 *
 * Two kinds of evidence answer that. A CRC-16/X.25 is proof on its own, and D-STAR offers
 * two of them: the RF header, and the header rebroadcast that ICOM radios fold into slow
 * data through a transmission. So is the "$$CRC" fixed-form literal, which needs the 0x35
 * type byte and forty more exact bits behind it.
 *
 * A superframe that carries only filler proves none of that, so it counts as weak evidence
 * and has to repeat. That is a real check rather than a guess about quality: two counted
 * frames in a row mean a second exact 24-symbol sync word was matched -- strcmp, no error
 * budget -- within the matchless pass that would otherwise have torn the carrier down, since
 * getFrameSync() runs the no-carrier hook after DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS symbols
 * and that hook resets this module. Against noise that is roughly 4 patterns x 1800 symbols
 * x 2^-24, about four in ten thousand per false match, and a false confirmation still dies at
 * the next matchless pass. A real call pays at most one superframe of withheld credit, and
 * none at all when it opens with a decodable RF header.
 *
 * Sibling of src/protocol/provoice/provoice_confirm.{c,h}, src/protocol/nxdn/nxdn_confirm.{c,h},
 * src/protocol/m17/m17_confirm.{c,h} and src/protocol/dmr/dmr_confidence.{c,h}, which do the
 * same job for those protocols.
 */

#ifndef DSD_NEO_PROTOCOL_DSTAR_DSTAR_CONFIRM_H
#define DSD_NEO_PROTOCOL_DSTAR_DSTAR_CONFIRM_H

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How much a passing check is worth. */
typedef enum {
    DSTAR_EVIDENCE_WEAK = 1,  /**< A superframe that decoded but checked nothing. */
    DSTAR_EVIDENCE_STRONG = 2 /**< A CRC-16/X.25: RF header or slow-data header. Also "$$CRC". */
} dstar_evidence;

/** @brief Frames of weak evidence in a row that together confirm a transmission. */
#define DSTAR_CONFIRM_WEAK_OBSERVES 2U

/** @brief Forget everything learned about the current transmission. */
void dstar_confirm_reset(dsd_state* state);

/** @brief Start a frame's evidence accounting; call once before decoding its contents. */
void dstar_confirm_begin_frame(dsd_state* state);

/**
 * @brief Report a check this frame passed.
 *
 * Strong evidence confirms immediately. Weak evidence counts once per frame however many
 * times it is reported, and confirms on the DSTAR_CONFIRM_WEAK_OBSERVES'th frame in a row.
 */
void dstar_confirm_note_evidence(dsd_state* state, dstar_evidence evidence);

/** @brief Close a frame's accounting; a frame that proved nothing breaks the weak streak. */
void dstar_confirm_end_frame(dsd_state* state);

/** @brief Whether the current transmission has proved itself. */
int dstar_confirm_is_confirmed(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_DSTAR_DSTAR_CONFIRM_H */
