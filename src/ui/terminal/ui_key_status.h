// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_UI_TERMINAL_UI_KEY_STATUS_H
#define DSD_NEO_UI_TERMINAL_UI_KEY_STATUS_H

#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

/* A DMR Basic Privacy and Hytera key: K, K1, K2, K3, K4, H, then basic_key_present and hytera_key_segments. */
typedef struct {
    unsigned long long int words[6];
    uint8_t flags[2];
} ui_dmr_key;

/* The working key, the one a forced key decrypts with. */
static inline void
ui_dmr_key_working(const dsd_state* state, ui_dmr_key* key) {
    key->words[0] = state->K;
    key->words[1] = state->K1;
    key->words[2] = state->K2;
    key->words[3] = state->K3;
    key->words[4] = state->K4;
    key->words[5] = state->H;
    key->flags[0] = state->basic_key_present;
    key->flags[1] = state->hytera_key_segments;
}

/* The key the user loaded: the working key, or while the talkgroup key autoload's key is the working key, the key
   state it replaced (a key set directly, or none). A talkgroup's key is its call's, not a loaded key. */
static inline void
ui_dmr_key_loaded(const dsd_state* state, ui_dmr_key* key) {
    if (!dsd_dmr_tg_autoload_baseline(state, key->words, key->flags)) {
        ui_dmr_key_working(state, key);
    }
}

static inline void
ui_dmr_key_clear(ui_dmr_key* key) {
    DSD_SECURE_ZERO(key, sizeof(*key));
}

static inline unsigned int
ui_dmr_key_hytera_segment_count(const ui_dmr_key* key) {
    const unsigned long long int* w = key->words;
    if (w[1] == 0ULL && w[2] == 0ULL && w[3] == 0ULL && w[4] == 0ULL) {
        return 0U;
    }
    if (key->flags[1] == 1U || key->flags[1] == 2U || key->flags[1] == 4U) {
        return key->flags[1];
    }
    if (w[3] != 0ULL || w[4] != 0ULL) {
        return 4U;
    }
    if (w[2] != 0ULL) {
        return 2U;
    }
    return 1U;
}

#endif
