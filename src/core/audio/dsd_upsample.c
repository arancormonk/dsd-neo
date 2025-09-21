// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * dsd_upsample.c
 * Simplified 8k to 48k Upsample Functions
 * Goodbye terrible ringing sound
 *
 *
 *
 * LWVMOBILE
 * 2024-03 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/dsd.h>

//produce 6 short samples (48k) for every 1 short sample (8k)
void
upsampleS(short invalue, short prev, short outbuf[6]) {
    UNUSED(prev);
    for (int i = 0; i < 6; i++) {
        outbuf[i] = invalue;
    }
}

//legacy 6x simplified version for the float_buf_p
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
