// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The encryption verdict on a voice frame, judged by the protocol that owns the call (issue #655).
 *
 * The vocoder stores it per slot (dsd_state::mbe_slot_muted) for short staging, the per-call WAV, MBE capture and the
 * DMR mixes; the FDMA mixers ask dsd_voice_crypto_audible() for the frame they play. One rule, so the outputs agree.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_voice_crypto_class_audible(const dsd_opts* opts, int slot, dsd_voice_crypto_class cls) {
    if (!opts) {
        return cls != DSD_VOICE_CRYPTO_UNDECRYPTABLE ? 1 : 0;
    }
    // Reverse mute classifies the transmission, not what decryption made of it: a call a loaded key decrypts is
    // still encrypted and plays, as P25's rule has it.
    if (opts->reverse_mute == 1) {
        return cls != DSD_VOICE_CRYPTO_CLEAR ? 1 : 0;
    }
    if (cls != DSD_VOICE_CRYPTO_UNDECRYPTABLE) {
        return 1;
    }
    return ((slot == 1) ? opts->dmr_mute_encR : opts->dmr_mute_encL) == 0 ? 1 : 0;
}

int
dsd_voice_frame_owner(const dsd_state* state) {
    if (!state) {
        return DSD_SYNC_NONE;
    }
    if (state->frame_dispatch_active) {
        return state->frame_dispatch_synctype;
    }
    // Outside a dispatch (unit tests), a borrowed decoder is told by the call it decodes for: YSF and dPMR publish
    // their own call while their frames run through the P25 Phase 1 or NXDN decoder.
    if (DSD_SYNC_IS_P25P1(state->synctype) || DSD_SYNC_IS_NXDN(state->synctype)) {
        dsd_call_snapshot call;
        if (dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
            && (DSD_SYNC_IS_YSF(call.protocol) || DSD_SYNC_IS_DPMR(call.protocol))) {
            return call.protocol;
        }
    }
    return state->synctype;
}

dsd_voice_crypto_class
dsd_nxdn_voice_crypto_class(const dsd_state* state) {
    if (!state || state->nxdn_cipher_type == 0) {
        return DSD_VOICE_CRYPTO_CLEAR;
    }
    // The keystream mbe_process_nxdn() applies: cipher 1 (and forced privacy) and cipher 2 with a scalar key,
    // cipher 3 with an AES key.
    const int scalar = dsd_key_scalar_present(state, 0);
    const int applied = ((state->nxdn_cipher_type == 0x01 || state->nxdn_cipher_type == 0x02) && scalar)
                        || (state->M == 1 && scalar)
                        || (state->nxdn_cipher_type == 0x03 && state->aes_key_loaded[0] == 1);
    return applied ? DSD_VOICE_CRYPTO_DECRYPTABLE : DSD_VOICE_CRYPTO_UNDECRYPTABLE;
}

// P25 Phase 2: the decode gate that admitted the frame and P25's speaker rule, as the per-call WAV and the capture
// already used. The frame decoder decodes voice only on a site whose descrambler seed is set.
static int
dsd_p25p2_voice_crypto_audible(const dsd_opts* opts, const dsd_state* state, int slot) {
    return state->p25_p2_audio_allowed[slot] != 0 && p25_crypto_audio_output_permitted(opts, state, slot);
}

static int
dsd_voice_owner_has_vocoder_verdict(int owner) {
    return DSD_SYNC_IS_P25P1(owner) || DSD_SYNC_IS_P25P2(owner) || DSD_SYNC_IS_NXDN(owner) || DSD_SYNC_IS_DPMR(owner);
}

int
dsd_voice_crypto_audible(const dsd_opts* opts, const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    const int owner = dsd_voice_frame_owner(state);
    // SDRTrunk JSON playback admits its frames by its own decode and crypto checks.
    if (!state->frame_dispatch_active && state->mbe_file_type == 3) {
        return 1;
    }
    if (DSD_SYNC_IS_DMR(owner)) {
        return state->mbe_slot_muted[slot] == 0 ? 1 : 0;
    }
    // These live protocols always pass through the vocoder, which may reject a frame despite a loaded key (for
    // example an exhausted keystream). Preserve that frame verdict before consulting the current call's metadata.
    // YSF V/D2 and SDRTrunk stage independently, so they must not inherit another decoder's cached mute.
    if (dsd_voice_owner_has_vocoder_verdict(owner) && state->mbe_slot_muted[slot] != 0) {
        return 0;
    }
    if (DSD_SYNC_IS_P25P1(owner)) {
        return p25_crypto_audio_output_permitted(opts, state, 0);
    }
    if (DSD_SYNC_IS_P25P2(owner)) {
        return dsd_p25p2_voice_crypto_audible(opts, state, slot);
    }
    if (DSD_SYNC_IS_NXDN(owner) || DSD_SYNC_IS_DPMR(owner)) {
        return dsd_voice_crypto_class_audible(opts, 0, dsd_nxdn_voice_crypto_class(state));
    }
    return 1;
}
