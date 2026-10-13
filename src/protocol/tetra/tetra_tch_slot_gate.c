// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA TCH/FS audio gate.
 *
 * Kept as a separate translation unit so unit tests can link it without
 * pulling in the full ACELP pipeline (which depends on platform audio,
 * libsndfile, and the vocoder subprocess).
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/tetra/tetra_acelp.h>
#include "tetra_state.h"

int
tetra_acelp_channel_gate_passes(const dsd_opts* opts) {
    return !opts || !opts->trunk_enable || opts->trunk_is_tuned;
}

/*
 * tetra_acelp_slot_gate_passes()
 *
 * TCH/FS is decoded from both 216-bit NDB halves as one 432-bit codeword.
 * block_idx identifies that combined decoder input; the actual TDMA slot is
 * state->tetra_tn, maintained from BSCH and advanced for every NDB.  Slot bits
 * are transmitted TN1 first, so numeric bit 3 maps to TN1 and bit 0 to TN4.
 * Before both timing and a channel allocation are known, remain permissive so
 * conventional single-carrier monitoring still works.
 */
int
tetra_acelp_slot_gate_passes(int block_idx, const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    (void)block_idx;
    if (!ts || !ts->vc_assignment_valid) {
        return 1;
    }

    const uint8_t bitmap = (uint8_t)(ts->vc_timeslot_bitmap & 0x0Fu);
    if (bitmap == 0) {
        return 0;
    }
    if (!ts->tdma_valid || ts->tn < 1 || ts->tn > 4) {
        return 1;
    }

    const uint8_t mask = (uint8_t)(1u << (4u - ts->tn));
    return (bitmap & mask) != 0;
}
