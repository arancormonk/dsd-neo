// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Whether the carrier on air and the burst being decoded have proved themselves well enough to be acted on.
 *
 * TETRA bursts are found by their training sequences: 22 bits for a normal burst, which receiver noise matches often
 * enough under the eight hypotheses the frame sync tries, so a burst reaching its decoder proves nothing. What does is
 * a block whose CRC passed: the CRC-16 of a BSCH, SCH/HD, SCH/F or STCH block (EN 300 392-2 clause 8.2.3.3), or the
 * CRC of a speech frame (EN 300 395-2 clause 5.5) that also re-encodes like speech. So:
 *
 * - The dispatch verdict is PROFILE_PROVEN for a burst that decoded such a block and UNPRODUCTIVE otherwise.
 * - What the call records publish of the carrier (the heard call, speech no record names, notices) waits for a
 *   CRC-16 signalling block of it: a downlink BSCH or MAC block, or on the uplink, which has no BSCH, an SCH/HU or MAC
 *   block (tetra_confirm_carrier_proven()).
 * - A trunking grant is followed only once a CRC-passed downlink BSCH or MAC block was decoded on this carrier before
 *   the block that carried the grant (tetra_confirm_downlink_proven(); the trunking state machine asks): uplink
 *   signalling says the carrier is a TETRA uplink, not a control channel, and carries no grant.
 * - Speech of a followed call plays only after CRC-passed signalling of that call on the carrier it is on
 *   (tetra_confirm_followed_call_proven(); the speech path asks, the call tracking notes it).
 *
 * The evidence belongs to one carrier: tetra_confirm_reset() drops it at every carrier boundary, no-carrier pass and
 * trunking retune. A retune made while a burst is being decoded (a grant in its first block) leaves the rest of that
 * burst on the carrier that was left: the decoders stop there (tetra_confirm_burst_current()), and nothing the burst
 * decoded counts for the new carrier. The burst's own verdict still counts what it decoded.
 *
 * Sibling of src/protocol/nxdn/nxdn_confirm.{c,h}. Decoder thread only; the state lives in tetra_state_t::confirm.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_CONFIRM_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_CONFIRM_H_

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* tetra_confirm_reset(), which forgets everything proved about the carrier on air, is declared in
 * <dsd-neo/protocol/tetra/tetra.h> for the engine. */

/** Start a burst's accounting; call before decoding any of its blocks. */
void tetra_confirm_burst_begin(dsd_state* state);

/** Start the accounting of a burst captured on the carrier of evidence epoch @p epoch (tetra_confirm_carrier_epoch()):
 * when the carrier was reset since (the trunking state machine settled a tune or return at the burst's entry), the
 * burst is the last carrier's from its start, so tetra_confirm_burst_current() is 0 and none of it is decoded. */
void tetra_confirm_burst_begin_captured(dsd_state* state, uint32_t epoch);

/** Close the burst's accounting. @return 1 when the burst decoded a block whose CRC passed. */
int tetra_confirm_burst_end(dsd_state* state);

/** 1 unless the carrier was reset since tetra_confirm_burst_begin(): the rest of the burst is the last carrier's. */
int tetra_confirm_burst_current(const dsd_state* state);

/** A downlink BSCH or MAC block of this burst passed its CRC (after the block was decoded and acted on). */
void tetra_confirm_note_signalling(dsd_state* state);

/** An uplink SCH/HU or MAC block of this burst passed its CRC-16: evidence about the carrier for publication only. */
void tetra_confirm_note_uplink_signalling(dsd_state* state);

/** Another block of this burst passed its CRC: a speech frame, or direct-mode signalling. */
void tetra_confirm_note_block(dsd_state* state);

/** 1 when a CRC-16 signalling block of this carrier was decoded (before the block being decoded): a downlink BSCH or
 * MAC block, or an uplink SCH/HU or MAC block. What publication waits for. */
int tetra_confirm_carrier_proven(const dsd_state* state);

/** 1 when a CRC-passed downlink BSCH or MAC block was decoded on this carrier (before the block being decoded). What a
 * trunking grant waits for. */
int tetra_confirm_downlink_proven(const dsd_state* state);

/** CRC-passed signalling of the followed call (tetra_sm_followed()) was decoded on this carrier. */
void tetra_confirm_note_followed_call(dsd_state* state);

/** The followed call changed or ended: what proved the previous one does not prove the next. */
void tetra_confirm_forget_followed_call(dsd_state* state);

/** 1 when CRC-passed signalling of the followed call was decoded on this carrier since it became the followed call. */
int tetra_confirm_followed_call_proven(const dsd_state* state);

/** Which carrier the evidence is about: a count tetra_confirm_reset() advances. */
uint32_t tetra_confirm_carrier_epoch(const dsd_state* state);

/** A tune that failed puts the followed call back: its proof @p proven, read at @p epoch, returns with it unless the
 * carrier was reset since, which dropped the proof with the carrier. */
void tetra_confirm_restore_followed_call(dsd_state* state, uint32_t epoch, int proven);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_CONFIRM_H_ */
