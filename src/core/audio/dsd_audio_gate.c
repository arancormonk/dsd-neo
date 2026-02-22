// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Audio gating helpers used by mixers and tests.
 *
 * The helpers here centralize per-slot gating decisions so that
 * dsd_audio2.c only needs to invoke them rather than duplicate the
 * whitelist/TG-hold logic.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_dmr_voice_alg_can_decrypt(int algid, unsigned long long r_key, int aes_loaded) {
    switch (algid) {
        // RC4/DES-style families keyed from 40/56-bit key material.
        case 0x02: // Hytera Enhanced
        case 0x21: // DMR RC4
        case 0x22: // DMR DES
        case 0x81: // P25 DES
        case 0x9F: // P25 DES-XL
        case 0xAA: // P25 RC4
            return (r_key != 0ULL) ? 1 : 0;

        // AES/TDEA-style families keyed from loaded AES key segments.
        case 0x24: // DMR AES-128
        case 0x25: // DMR AES-256
        case 0x36: // Kirisun Advanced
        case 0x37: // Kirisun Universal
        case 0x83: // P25 TDEA
        case 0x84: // P25 AES-256
        case 0x89: // P25 AES-128
            return (aes_loaded == 1) ? 1 : 0;

        default: return 0;
    }
}

int
dsd_p25p2_mixer_gate(const dsd_state* state, int* encL, int* encR) {
    if (!state || !encL || !encR) {
        return -1;
    }
    *encL = state->p25_p2_audio_allowed[0] ? 0 : 1;
    *encR = state->p25_p2_audio_allowed[1] ? 0 : 1;
    return 0;
}

int
dsd_audio_group_gate_mono(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int enc_in, int* enc_out) {
    if (!opts || !state || !enc_out) {
        return -1;
    }

    int enc = enc_in;

    char mode[8];
    snprintf(mode, sizeof mode, "%s", "");

    // If using allow/whitelist mode, default to block until a matching
    // group explicitly marks this TG as allowed.
    if (opts->trunk_use_allow_list == 1) {
        snprintf(mode, sizeof mode, "%s", "B");
    }

    for (unsigned int gi = 0; gi < state->group_tally; gi++) {
        if (state->group_array[gi].groupNumber == tg) {
            strncpy(mode, state->group_array[gi].groupMode, sizeof(mode) - 1);
            mode[sizeof(mode) - 1] = '\0';
            break;
        }
    }

    // Block if this TG is explicitly lockout-tagged in the group list.
    // "DE" is treated as lockout in trunking policy and should match here
    // so audio/playback/record gates stay consistent.
    if (strcmp(mode, "B") == 0 || strcmp(mode, "DE") == 0) {
        enc = 1;
    }

    // TG Hold: when in effect, mute everything except the held TG. If the
    // held TG matches, force unmute to match existing behavior.
    if (state->tg_hold != 0 && state->tg_hold != (uint32_t)tg) {
        enc = 1;
    }
    if (state->tg_hold != 0 && state->tg_hold == (uint32_t)tg) {
        enc = 0;
    }

    *enc_out = enc;
    return 0;
}

int
dsd_audio_group_gate_dual(const dsd_opts* opts, const dsd_state* state, unsigned long tgL, unsigned long tgR,
                          int encL_in, int encR_in, int* encL_out, int* encR_out) {
    if (!encL_out || !encR_out) {
        return -1;
    }
    int rc = 0;
    rc |= dsd_audio_group_gate_mono(opts, state, tgL, encL_in, encL_out);
    rc |= dsd_audio_group_gate_mono(opts, state, tgR, encR_in, encR_out);
    return rc;
}
