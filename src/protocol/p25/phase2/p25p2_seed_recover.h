// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 2 scrambling code recovery from a carrier's own signalling (issue #804).
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_SEED_RECOVER_H_
#define DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_SEED_RECOVER_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * processP2() calls this once per window, after the window is positioned and before it is descrambled, with whether
 * the window's own channel-1 I-ISCH located it and whether that ISCH contradicted continuity. On a carrier whose code
 * is unproven, not hand-set and not trunked, the window's scrambled FACCH and SACCH bursts that fail under the code in
 * force are tried against the candidate waiting for confirmation, the Signal search session's learned codes and a
 * code solved from the burst itself. A candidate is confirmed by a burst of another window its own I-ISCH located, at
 * another superframe slot; the code is then committed (p2_wacn, p2_sysid, p2_cc) and this window descrambles with it.
 * Nothing else of the decoder's state changes before then.
 */
void p25p2_seed_recover_window(const dsd_opts* opts, dsd_state* state, int isch_located, int position_contradicted);

/* Drops the candidate waiting for confirmation: the receiver left the carrier it came from. It writes the module's
   state extension (DSD_STATE_EXT_PROTO_P25P2_SEED_RECOVER), which a const dsd_state still reaches, and no field of
   dsd_state itself. */
void p25p2_seed_recover_forget(const dsd_state* state);

/* A burst on this carrier proved @p code (WACN(20) << 24 | SYSID(12) << 12 | NAC(12)), after the carrier's proven NAC
   was recorded (p25p2_note_seed_proven()). When it is the code recovery committed on this carrier, the Signal search
   now hears of the system its WACN and SYSID name, judged by that NAC (issue #802 B11). The owed note goes with the
   first proof either way, and with p25p2_seed_recover_forget(). */
void p25p2_seed_recover_note_proven(const dsd_state* state, uint64_t code);

/* Whether a candidate waits for confirmation, and which (tests). */
int p25p2_seed_recover_pending(const dsd_state* state, uint64_t* code);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_SEED_RECOVER_H_ */
