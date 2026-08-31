// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether a ProVoice transmission has proved itself well enough to hold a hunt profile.
 *
 * ProVoice has nothing checkable in it. processProVoice() spends 736 dibits per call and the
 * frame carries no CRC, no BCH and no parity anywhere: the 64-bit opening field, the 16-bit
 * LID and the 64-bit secondary are read into locals and printed under -p, and the two IMBE
 * pairs behind them yield only mbelib correction counts, which map every input to some
 * codeword and so never fail. Its one early return fires when the dibit callback fails, never
 * on bad data. On the 9600/2 hunt profile it shares with EDACS -- which does report its BCH
 * verdict -- a false ProVoice match therefore bought 736 symbols of dwell that EDACS's own
 * gate would have refused (issues #391, #421).
 *
 * What is checkable is that the stream keeps coming. A real voice channel emits frames
 * back to back, each behind a fresh 32-symbol sync word matched exactly, with no error
 * budget; noise does not supply a second one. So a frame counts as weak evidence and has to
 * repeat: two in a row confirm, and getFrameSync() runs the no-carrier hook -- which resets
 * this module -- after DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS matchless symbols, so "in a row"
 * cannot span a dead channel. Against noise that is roughly 4 patterns x 1800 symbols x
 * 2^-32, under two in a million per false match. A real call confirms on its second frame,
 * about 77 ms in, and a single dropped sync mid-call does not break the streak because one
 * missed frame is far short of a whole matchless pass.
 *
 * The LID would be the obvious content to key on -- it should repeat within a transmission --
 * but there is no committed ProVoice fixture to measure against and EDACS ESK masking of
 * these fields is unverified. Keying a verdict on it risks the failure #391 names explicitly:
 * live traffic that decodes but never reports it, rotating the hunt away.
 *
 * Sibling of src/protocol/dstar/dstar_confirm.{c,h}, src/protocol/nxdn/nxdn_confirm.{c,h},
 * src/protocol/m17/m17_confirm.{c,h} and src/protocol/dmr/dmr_confidence.{c,h}, which do the
 * same job for those protocols.
 */

#ifndef DSD_NEO_PROTOCOL_PROVOICE_PROVOICE_CONFIRM_H
#define DSD_NEO_PROTOCOL_PROVOICE_PROVOICE_CONFIRM_H

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How much a passing check is worth. */
typedef enum {
    PROVOICE_EVIDENCE_WEAK = 1, /**< A frame that decoded behind its own exact sync word. */
    /** Reserved: no ProVoice field carries a check that can fail. Kept so this module reads
     *  the same as its siblings and so a future checkable field has somewhere to report. */
    PROVOICE_EVIDENCE_STRONG = 2
} provoice_evidence;

/** @brief Frames of weak evidence in a row that together confirm a transmission. */
#define PROVOICE_CONFIRM_WEAK_OBSERVES 2U

/** @brief Forget everything learned about the current transmission. */
void provoice_confirm_reset(dsd_state* state);

/** @brief Start a frame's evidence accounting; call once before decoding its contents. */
void provoice_confirm_begin_frame(dsd_state* state);

/**
 * @brief Report a check this frame passed.
 *
 * Strong evidence confirms immediately. Weak evidence counts once per frame however many
 * times it is reported, and confirms on the PROVOICE_CONFIRM_WEAK_OBSERVES'th frame in a row.
 */
void provoice_confirm_note_evidence(dsd_state* state, provoice_evidence evidence);

/** @brief Close a frame's accounting; a frame that proved nothing breaks the weak streak. */
void provoice_confirm_end_frame(dsd_state* state);

/** @brief Whether the current transmission has proved itself. */
int provoice_confirm_is_confirmed(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_PROVOICE_PROVOICE_CONFIRM_H */
