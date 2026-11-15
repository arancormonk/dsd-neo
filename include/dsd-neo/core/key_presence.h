// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_CORE_KEY_PRESENCE_H
#define DSD_NEO_CORE_KEY_PRESENCE_H

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

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

// Whether the DMR talkgroup key autoload's key is still the working key: the Basic Privacy and Hytera key (K, K1..K4,
// H, basic_key_present, hytera_key_segments) is the one it installed (dsd_state::dmr_tg_autoload), changed by nothing
// else since (a key command, the no-carrier unload).
static inline int
dsd_dmr_tg_autoload_holds(const dsd_state* state) {
    if (!state || !state->dmr_tg_autoload.active) {
        return 0;
    }
    const unsigned long long int* in = state->dmr_tg_autoload.installed;
    const uint8_t* flags = state->dmr_tg_autoload.installed_flags;
    return state->K == in[0] && state->K1 == in[1] && state->K2 == in[2] && state->K3 == in[3] && state->K4 == in[4]
           && state->H == in[5] && state->basic_key_present == flags[0] && state->hytera_key_segments == flags[1];
}

// The operator's own Basic Privacy and Hytera key while the autoload's key is the working key: the key state the
// autoload replaced (a key set directly, or none), into @p words (K, K1..K4, H) and @p flags (basic_key_present,
// hytera_key_segments); returns 0 and fills nothing when the working key is not the autoload's.
static inline int
dsd_dmr_tg_autoload_baseline(const dsd_state* state, unsigned long long int words[6], uint8_t flags[2]) {
    if (!dsd_dmr_tg_autoload_holds(state)) {
        return 0;
    }
    for (int i = 0; i < 6; i++) {
        words[i] = state->dmr_tg_autoload.baseline[i];
    }
    flags[0] = state->dmr_tg_autoload.baseline_flags[0];
    flags[1] = state->dmr_tg_autoload.baseline_flags[1];
    return 1;
}

// The DMR Basic Privacy and Hytera key slot @p slot's call on talkgroup @p tg (0 for a call no LC has named yet) is
// decrypted with, as a frontend shows it from its snapshot, which holds no keyring: with a forced key or an ALG ID on
// the slot, the working key; else the key the slot's own call last keyed from (dsd_state::dmr_tg_autoload slot_key)
// while @p tg is that call's talkgroup or not known yet; else the key the autoload replaced while its key is the
// working key, else the working key. Never another slot's talkgroup key. Into @p words and @p flags as
// dsd_dmr_tg_autoload_baseline() fills them.
static inline void
dsd_dmr_slot_call_key(const dsd_state* state, uint8_t slot, uint32_t tg, unsigned long long int words[6],
                      uint8_t flags[2]) {
    if (!state) {
        return;
    }
    const uint8_t idx = slot & 1U;
    const int live_algid = idx == 0U ? state->payload_algid : state->payload_algidR;
    const uint8_t* keyed_flags = state->dmr_tg_autoload.slot_key_flags[idx];
    const int keyed = keyed_flags[0] != 0U || keyed_flags[1] != 0U;
    if (state->M == 0 && live_algid == 0 && keyed && (tg == 0U || tg == state->dmr_tg_autoload.slot_tg[idx])) {
        for (int i = 0; i < 6; i++) {
            words[i] = state->dmr_tg_autoload.slot_key[idx][i];
        }
        flags[0] = keyed_flags[0];
        flags[1] = keyed_flags[1];
        return;
    }
    if (state->M == 0 && live_algid == 0 && dsd_dmr_tg_autoload_baseline(state, words, flags)) {
        return;
    }
    words[0] = state->K;
    words[1] = state->K1;
    words[2] = state->K2;
    words[3] = state->K3;
    words[4] = state->K4;
    words[5] = state->H;
    flags[0] = state->basic_key_present;
    flags[1] = state->hytera_key_segments;
}

#ifdef __cplusplus
}
#endif

#endif
