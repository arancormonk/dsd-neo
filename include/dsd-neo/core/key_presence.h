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

// Whether the slot's activated AES key (aes_key_loaded[], aes_key_segments[]) is at least @p words segments wide, as
// an AES ALG of that width needs. Every loader records the width; a legacy writer that set aes_key_loaded[] without
// it (width 0) carries none and keeps the old rule.
static inline int
dsd_key_aes_slot_complete(const dsd_state* state, int slot, unsigned int words) {
    if (!state || slot < 0 || slot > 1 || state->aes_key_loaded[slot] != 1) {
        return 0;
    }
    return state->aes_key_segments[slot] == 0U || state->aes_key_segments[slot] >= words;
}

// Whether the direct multi-word key (K1..K4: -H, a direct AES or TDEA key), which data paths fall back to when
// nothing was imported for the key ID, is at least @p words 64-bit words wide. Its loaders record the word count
// (hytera_key_segments), which then decides; material written without one carries no width and keeps the legacy
// rule, any non-zero word (a direct key's width was checked where it was entered).
static inline int
dsd_key_direct_words_complete(const dsd_state* state, unsigned int words) {
    if (!state || words == 0U || words > 4U) {
        return 0;
    }
    if (state->hytera_key_segments != 0U) {
        return state->hytera_key_segments >= words;
    }
    return (state->K1 | state->K2 | state->K3 | state->K4) != 0ULL;
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
