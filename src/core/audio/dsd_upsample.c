// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * dsd_upsample.c
 * Simplified 8k to 48k Upsample Functions
 * Uses linear interpolation for smooth transitions without ringing.
 *
 *
 *
 * LWVMOBILE
 * 2024-03 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/state.h>

#include "dsd-neo/core/state_fwd.h"

// Six-fold sample repetition for the analog-monitor float buffer.
void
upsample(dsd_state* state, float invalue) {

    if (!state) {
        return;
    }

    float* outbuf1 = state->audio_out_float_buf_p;
    if (state->dmr_stereo == 1) {
        outbuf1 = (state->currentslot == 1) ? state->audio_out_float_buf_pR : state->audio_out_float_buf_p;
    }
    if (!outbuf1) {
        return; // nothing to write to
    }

    *outbuf1 = invalue;
    outbuf1++;
    *outbuf1 = invalue;
    outbuf1++;
    *outbuf1 = invalue;
    outbuf1++;
    *outbuf1 = invalue;
    outbuf1++;
    *outbuf1 = invalue;
    outbuf1++;
    *outbuf1 = invalue;
}
