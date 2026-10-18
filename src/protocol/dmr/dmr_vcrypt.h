// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR voice crypto continuity per slot (issue #729).
 *
 * Every DMR voice cipher keys a frame by the superframe's IV/MI and the frame's position 0..17 in the superframe.
 * The voice readers only see the superframes and bursts they decode, so a burst the gate skipped, or a drop-out of
 * the burst loop, used to leave the MI a superframe or more behind and the keystream offset wrong for the rest of
 * the superframe. This module keeps, per slot, which superframe the current IV belongs to (on the DSP symbol clock),
 * catches the MI up when the readers come back, publishes each frame's position to the vocoder, and says when the
 * chain cannot be known (then encrypted voice is muted until fresh signalling re-keys the slot).
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_VCRYPT_H
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_VCRYPT_H

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** dsd_state::dmr_vc_ref_state values. */
enum {
    DMR_VC_REF_NONE = 0,  /**< Nothing known: the slot's next voice sync starts no chain by itself. */
    DMR_VC_REF_FIRST = 1, /**< A PI or voice-header LC prepared superframe 0's IV; ref clock is where it was read. */
    DMR_VC_REF_VALID = 2, /**< ref clock is the voice sync of the superframe the current IV is for. */
};

/** Symbols of one slot's voice superframe: six bursts, each followed by the other slot's, 144 dibits apiece. */
#define DMR_VC_SUPERFRAME_SYMBOLS 1728
/** A slot's VC6 burst to its next VC1 burst, at the same point in each. */
#define DMR_VC_BURST_PAIR_SYMBOLS 288
/** Superframes a continuous timeline may be caught up across; a longer silence leaves the chain unknown. */
#define DMR_VC_CATCHUP_MAX        10000

/** Whether @p algid keys its keystream by a per-superframe MI (AES, DES, RC4, Hytera Enhanced, Kirisun). */
int dmr_vcrypt_algid_uses_mi(int algid);

/** The slot's cipher, key id or MI was replaced by material that anchors no superframe (an SB identifier naming another
    cipher, a PI accepted without a passing CRC): the chain is unknown until the slot is re-keyed. Collected late-entry
    fragments stay, to re-key it. */
void dmr_vcrypt_lose(dsd_state* state, uint8_t slot);

/** A CRC-verified PI header (or a voice-header LC that carries the MI) prepared superframe 0's IV on @p slot. */
void dmr_vcrypt_note_first(dsd_state* state, uint8_t slot);

/** A verified source installed the MI that the slot's next superframe uses (a late-entry MI, an embedded LC). The slot
    is trusted from its next voice sync. */
void dmr_vcrypt_note_next_superframe_mi(dsd_state* state, uint8_t slot);

/** Whether a PI header prepared the slot's superframe 0 and its voice sync is still due on this timeline: the PI is
    the call now starting's, which call-boundary signalling read after it must not discard. */
int dmr_vcrypt_pi_is_fresh(const dsd_state* state, uint8_t slot);

/**
 * @brief The slot's voice sync (burst A, VC1) was read: account for the superframes missed since the last one.
 *
 * Advances the MI the right number of superframes when the timeline is continuous, starts the chain on a FIRST
 * reference, or marks the chain unknown. Call once per VC1, after its sync was read and before its frames decode.
 */
void dmr_vcrypt_superframe_start(dsd_state* state, uint8_t slot);

/**
 * @brief Hytera Enhanced's MI step, which the BS reader takes before the slot's VC6 frames decode.
 *
 * The superframe's MI stays available to the vocoder until dmr_vcrypt_superframe_end(), so a keystream generated again
 * for VC6 (a key that changed mid-superframe) uses this superframe's MI, not the next one's.
 */
void dmr_vcrypt_hytera_vc6_step(dsd_state* state, uint8_t slot);

/** The slot's VC6 was processed and its per-superframe MI step ran: the current IV is now the next superframe's. */
void dmr_vcrypt_superframe_end(dsd_state* state, uint8_t slot);

/** The slot's call ended or its carrier left: whatever chain it had is gone. */
void dmr_vcrypt_forget(dsd_state* state, uint8_t slot);

/** Publish the position (0..17) of the frame about to decode; vc is 1..6, frame 0..2. */
void dmr_vcrypt_set_frame(dsd_state* state, uint8_t slot, uint8_t vc, uint8_t frame);

/** The frame decoded; later voice frames on the slot are not positioned until set again. */
void dmr_vcrypt_clear_frame(dsd_state* state, uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_VCRYPT_H */
