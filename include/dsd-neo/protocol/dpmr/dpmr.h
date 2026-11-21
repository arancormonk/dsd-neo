// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief dPMR interfaces the layers above the protocol need.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forget the CRC evidence gathered for the current dPMR transmission.
 *
 * Defined in src/protocol/dpmr/dpmr_confirm.c and declared here so the engine can clear it
 * with the carrier; the rest of that module's interface is private to the protocol.
 */
void dpmr_confirm_reset(dsd_state* state);

/**
 * @brief Whether the current dPMR transmission has produced a CRC-verified CCH.
 *
 * The sticky per-transmission flag, not this frame's evidence. Dispatch reads it to decide
 * whether a sync alone may open a call row, which it may not until something has decoded.
 */
int dpmr_confirm_is_confirmed(const dsd_state* state);

/**
 * @brief A dPMR frame whose sync the decoder accepts but which carries no check of its own (FS1, FS3, FS4: dispatch
 * reads nothing beyond the sync) to a scan quiet check, through @p opts (issue #834): PARTIAL, held for
 * DPMR_CONFIRM_PARTIAL_HOLD_MS, so a key-up just after a quiet reading waits for its first CCH verdict. @p opts may be
 * NULL (no quiet check).
 */
void dpmr_confirm_note_unchecked_frame(const dsd_opts* opts, dsd_state* state);

/** @brief dPMRVoiceFS2Frame_t::Scrambled values (issue #655). */
enum {
    DPMR_HALF_CLEAR = 0U,      /**< Clear voice. */
    DPMR_HALF_SCRAMBLED = 1U,  /**< Scrambled voice (Version 3). */
    DPMR_HALF_UNVERIFIED = 2U, /**< No verified half says which since the transmission's boundary. */
};

/**
 * @brief Forget the scrambling (issue #655) and the voice mode (issue #903) the current transmission's CRC-verified
 * halves signalled.
 *
 * A half whose CRC-7 fails is played by the transmission's last verified half, so a transmission boundary the engine
 * sees (a header, end or other non-voice frame sync) must not hand one call's verdict to the next.
 */
void dpmr_scrambling_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DPMR_DPMR_H_ */
