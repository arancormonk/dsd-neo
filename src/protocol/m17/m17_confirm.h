// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether an M17 transmission has proved itself well enough to be acted on.
 *
 * M17's entry into the sync chain is a preamble, and a preamble is only an alternating symbol
 * run -- there is no sync word to check, so any signal that presents alternating symbols at
 * 4800 baud offers one. Under AUTO that is D-STAR's bit sync, and it is noise on an open
 * squelch. The chain that follows is stronger but not strong enough on its own: an LSF sync
 * word is eight symbols matched to within one error, which noise supplies often enough to reach
 * the frame body.
 *
 * So the frame body has to say something checkable before the decoder opens a call, synthesizes
 * voice, or tells the SPS hunt that this profile is carrying traffic (issue #399). The CRC-16s
 * on the LSF and on a packet's end are proof by themselves, as is a PRBS9 lock. A LICH whose
 * six Golay(24,12) blocks all clear is not: extended Golay corrects three errors and detects
 * four, so it accepts a meaningful share of random words, and the run of them a whole LICH needs
 * still turns up on noise. That has to repeat -- two frames running, which a real stream does
 * continuously and noise does not.
 *
 * Sibling of src/protocol/nxdn/nxdn_confirm.{c,h} and src/protocol/dmr/dmr_confidence.{c,h},
 * which do the same job for those protocols.
 */

#ifndef DSD_NEO_PROTOCOL_M17_M17_CONFIRM_H
#define DSD_NEO_PROTOCOL_M17_M17_CONFIRM_H

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How much a passing check is worth. */
typedef enum {
    M17_EVIDENCE_WEAK = 1,  /**< A LICH whose six Golay(24,12) blocks cleared. */
    M17_EVIDENCE_STRONG = 2 /**< A CRC-16: LSF or packet. Also a PRBS9 BERT lock. */
} m17_evidence;

/** @brief Frames of weak evidence in a row that together confirm a transmission. */
#define M17_CONFIRM_WEAK_OBSERVES 2U

/** @brief Forget everything learned about the current transmission. */
void m17_confirm_reset(dsd_state* state);

/** @brief Start a frame's evidence accounting; call once before decoding its contents. */
void m17_confirm_begin_frame(dsd_state* state);

/**
 * @brief Report a check this frame passed.
 *
 * Strong evidence confirms immediately. Weak evidence counts once per frame however many times
 * it is reported, and confirms on the M17_CONFIRM_WEAK_OBSERVES'th frame in a row.
 */
void m17_confirm_note_evidence(dsd_state* state, m17_evidence evidence);

/** @brief Close a frame's accounting; a frame that proved nothing breaks the weak streak. */
void m17_confirm_end_frame(dsd_state* state);

/** @brief Whether the current transmission has proved itself. */
int m17_confirm_is_confirmed(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_M17_M17_CONFIRM_H */
