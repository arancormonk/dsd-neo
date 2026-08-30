// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether an NXDN frame has proved itself well enough to be acted on.
 *
 * Everything upstream of this is weak. The sync search slices on sign alone, so its
 * 10-symbol frame sync word matches about 1% of receiver noise; the LICH that follows
 * carries one parity bit and is checked against a table holding 55 of the 128 possible
 * values, so roughly a fifth of those noise syncs reach the frame body. P25 and DMR do
 * not behave this way on an open squelch because their equivalent gates -- a 63-bit BCH
 * on the P25 NID, Golay and a two-observation colour-code lock on DMR -- are strong
 * enough to stand alone. NXDN's are not, and on a quiet channel the result was a stream
 * of invented RANs, synthesized voice, and a conventional scan that would not move on
 * (issue #398).
 *
 * So the frame body has to say something checkable before the decoder treats a frame as
 * real. A CRC of 12 bits or more is proof by itself. The 6- and 7-bit CRCs on SACCH and
 * SCCH are not -- one in 64 noise frames passes a CRC-6 -- so they have to repeat: two
 * frames running, which a real transmission does continuously and noise does not.
 *
 * Sibling of src/protocol/dmr/dmr_confidence.{c,h}, which does the same job for DMR.
 */

#ifndef DSD_NEO_PROTOCOL_NXDN_NXDN_CONFIRM_H
#define DSD_NEO_PROTOCOL_NXDN_NXDN_CONFIRM_H

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How much a passing check is worth. */
typedef enum {
    NXDN_EVIDENCE_WEAK = 1,  /**< A 6- or 7-bit CRC: SACCH, SACCH2, SCCH. */
    NXDN_EVIDENCE_STRONG = 2 /**< A CRC of 12 bits or more: FACCH, CAC, UDCH, PICH/TCH, full SACCH. */
} nxdn_evidence;

/** @brief Frames of weak evidence in a row that together confirm a transmission. */
#define NXDN_CONFIRM_WEAK_OBSERVES 2U

/** @brief Forget everything learned about the current transmission. */
void nxdn_confirm_reset(dsd_state* state);

/** @brief Start a frame's evidence accounting; call once before decoding its channels. */
void nxdn_confirm_begin_frame(dsd_state* state);

/**
 * @brief Report a check this frame passed.
 *
 * Strong evidence confirms immediately. Weak evidence counts once per frame however many
 * short CRCs pass, and confirms on the NXDN_CONFIRM_WEAK_OBSERVES'th frame in a row.
 */
void nxdn_confirm_note_evidence(dsd_state* state, nxdn_evidence evidence);

/** @brief Close a frame's accounting; a frame that proved nothing breaks the weak streak. */
void nxdn_confirm_end_frame(dsd_state* state);

/** @brief Whether the current transmission has proved itself. */
int nxdn_confirm_is_confirmed(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_NXDN_NXDN_CONFIRM_H */
