// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The gates a TETRA speech frame passes before it plays: the followed channel and timeslot, its encryption class and
 * the talkgroup the policy judges it by.
 */

#include "tetra_voice_gate.h"

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <stdint.h>

#include "tetra_state.h"

int
tetra_voice_channel_gate_passes(const dsd_opts* opts) {
    return !opts || !opts->trunk_enable || opts->trunk_is_tuned;
}

/* The TDMA timeslot comes from the BSCH and advances with every normal burst (tetra_tdma_advance_ndb()). A channel
   allocation's timeslot bitmap is sent TN1 first (EN 300 392-2 clause 21.5.2), so its bit 3 is TN1 and bit 0 TN4.
   Until both are known every timeslot passes, so single-carrier monitoring still plays. */
int
tetra_voice_slot_gate_passes(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
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

dsd_voice_crypto_class
tetra_voice_crypto_class(const dsd_state* state, int keystream_applied) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts) {
        return DSD_VOICE_CRYPTO_CLEAR;
    }
    if (ts->dmo) {
        /* Direct mode air encryption (EN 300 396-6) is not decrypted here, and end-to-end encryption never is; until a
           DM-SETUP or DM-OCCUPIED has given the call's security class, the speech may be either. */
        return tetra_dmo_call_crypt(state) == TETRA_DMO_CRYPT_CLEAR ? DSD_VOICE_CRYPTO_CLEAR
                                                                    : DSD_VOICE_CRYPTO_UNDECRYPTABLE;
    }
    if (ts->enc_mode == TETRA_ENC_MODE_NONE) {
        return DSD_VOICE_CRYPTO_CLEAR;
    }
    return keystream_applied ? DSD_VOICE_CRYPTO_DECRYPTABLE : DSD_VOICE_CRYPTO_UNDECRYPTABLE;
}

unsigned long
tetra_voice_talkgroup(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || ts->dmo) {
        return 0UL;
    }
    return (unsigned long)ts->gssi;
}
