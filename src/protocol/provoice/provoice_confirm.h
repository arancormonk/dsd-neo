// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether a ProVoice transmission has proved itself well enough to hold a hunt profile and play its voice.
 *
 * ProVoice has nothing checkable in it. processProVoice() spends 736 dibits per call and the
 * frame carries no CRC, no BCH and no parity outside the vocoder's own codes: the 64-bit opening
 * field, the 16-bit LID and the 64-bit secondary are read into locals and printed under -Z, and
 * the two IMBE pairs behind them are protected by Golay and Hamming codes that are perfect, so
 * their decoders map every input to some codeword and never fail. Its one early return fires when the dibit callback fails, never
 * on bad data. On the 9600/2 hunt profile it shares with EDACS -- which does report its BCH
 * verdict -- a false ProVoice match therefore bought 736 symbols of dwell that EDACS's own
 * gate would have refused (issues #391, #421).
 *
 * What is checkable is that the stream keeps coming. A real voice channel emits frames
 * back to back, each behind a fresh 32-symbol sync word matched exactly, with no error
 * budget; noise does not supply a second one. So a frame counts as weak evidence and has to
 * repeat: two in a row confirm. "In a row" means the second frame's sync follows the first
 * frame by one frame, 768 symbols (provoice_confirm_note_frame_start()), as the dPMR and
 * NXDN48 gates measure it; getFrameSync() also runs the no-carrier hook -- which resets this
 * module -- after DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS matchless symbols. Against noise a
 * second exact 32-symbol word one frame after the first is about 4 patterns x 2^-32 per
 * false match. A real call confirms on its second frame, about 77 ms in; a sync dropped
 * before that starts the streak over, and one dropped once the call is confirmed changes
 * nothing, the confirmation being sticky until the carrier goes.
 *
 * The same verdict gates the voice: processProVoice() hands a frame's IMBE pairs to the
 * vocoder, the speaker, the recordings and the call record only once the transmission is
 * confirmed, so a lone sync word on noise plays nothing, at the cost of a transmission's first
 * frame (four IMBE frames, 80 ms).
 *
 * The LID would be the obvious content to key on, if it repeated within a transmission. In the
 * committed provoice fixture, one transmission of 40 frames, the field read as the LID takes 16
 * values, and nothing protects the 64 bits after it, which differ by a bit or two in 10 frames;
 * EDACS ESK masking of these fields is unverified too. Keying a verdict on them risks the
 * failure #391 names explicitly: live traffic that decodes but never reports it, rotating the
 * hunt away.
 *
 * Sibling of src/protocol/dstar/dstar_confirm.{c,h}, src/protocol/nxdn/nxdn_confirm.{c,h},
 * src/protocol/m17/m17_confirm.{c,h} and src/protocol/dmr/dmr_confidence.{c,h}, which do the
 * same job for those protocols.
 */

#ifndef DSD_NEO_PROTOCOL_PROVOICE_PROVOICE_CONFIRM_H
#define DSD_NEO_PROTOCOL_PROVOICE_PROVOICE_CONFIRM_H

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

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
#define PROVOICE_CONFIRM_WEAK_OBSERVES      2U

/** @brief Symbols from one frame's start to the next's in a transmission (a 32-symbol sync word and the 736 symbols
 * behind it), and the slip either way a run allows. */
#define PROVOICE_CONFIRM_FRAME_SYMBOLS      768U
#define PROVOICE_CONFIRM_FRAME_SYMBOLS_SLIP 1U

/** @brief Forget everything learned about the current transmission. */
void provoice_confirm_reset(dsd_state* state);

/**
 * @brief Say where a frame whose contents are about to be read begins, before provoice_confirm_begin_frame().
 *
 * @p frame_symbol is dsd_state::symbolcnt at the same point of every frame. A frame runs on only from one read
 * PROVOICE_CONFIRM_FRAME_SYMBOLS (within the slip) before it; one that does not starts the weak streak over. Without
 * this call a frame runs on (the unit tests' frames, one after another).
 */
void provoice_confirm_note_frame_start(dsd_state* state, uint32_t frame_symbol);

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
