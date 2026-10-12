// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_CORE_KEY_PRESENCE_H
#define DSD_NEO_CORE_KEY_PRESENCE_H

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Legacy writers supply nonzero material without presence metadata. Direct
// loaders also record explicit zero values; keyring activation replaces those flags.
static inline int
dsd_key_scalar_present(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    return state->scalar_key_present[slot] || (slot == 0 ? state->R : state->RR) != 0ULL;
}

static inline int
dsd_key_basic_present(const dsd_state* state) {
    return state && (state->basic_key_present || state->K != 0ULL);
}

static inline int
dsd_key_hytera_present(const dsd_state* state) {
    return state && (state->hytera_key_segments != 0U || state->K1 != 0ULL);
}

// DMR forced clear: Baofeng AP, CSI EE and the Kenwood scrambler are static keys forced onto every voice frame, so
// the vocoder's verdict counts a frame under them as decrypted (issue #655): an encrypted one plays, and reverse mute
// (-q) mutes only one that is clear.
static inline int
dsd_key_dmr_forced_clear(const dsd_state* state) {
    return state && (state->baofeng_ap == 1 || state->csi_ee == 1 || state->ken_sc == 1);
}

#ifdef __cplusplus
}
#endif

#endif
