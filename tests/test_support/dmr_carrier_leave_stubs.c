// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* For the trunk state machine fixtures that compile dmr_trunk_sm.c without the DMR encryption sources: the machine's
 * leave of a carrier (dmr_sm_leave_carrier(), dmr_sm_note_carrier_tune()) forgets the left calls' privacy and MI chain
 * and holds the next carrier's voice through these. The fixtures test tuning, so the stubs forget only the service
 * options, the FID and each slot's chain state, and set each slot's hold. */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include "dmr_vcrypt.h"
#include "dsd-neo/core/state_fwd.h"

void
dmr_forget_call_privacy(dsd_state* state) {
    state->dmr_so = state->dmr_soR = 0U;
    state->dmr_fid = state->dmr_fidR = 0U;
}

void
dmr_vcrypt_forget(dsd_state* state, uint8_t slot) {
    state->dmr_vc_ref_state[slot] = DMR_VC_REF_NONE;
    state->dmr_vc_untrusted[slot] = 1U;
}

void
dmr_enc_class_hold(dsd_state* state) {
    state->dmr_enc_class_held[0] = state->dmr_enc_class_held[1] = 1U;
}
