// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR voice crypto continuity per slot (issue #729); see dmr_vcrypt.h.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include "dmr_vcrypt.h"
#include "dsd-neo/core/state_fwd.h"

/* A voice sync this close after a PI header (or the voice header that carried the MI) is superframe 0's: the PI is
   the last header before voice, and up to three repeats of it may come between. Further away, superframes may have
   gone by unseen, and nothing tells how many. */
#define DMR_VC_FIRST_WINDOW_SYMBOLS (4 * DMR_VC_BURST_PAIR_SYMBOLS)

static int
vcrypt_slot_algid(const dsd_state* state, uint8_t slot) {
    return slot == 0U ? state->payload_algid : state->payload_algidR;
}

int
dmr_vcrypt_algid_uses_mi(int algid) {
    switch (algid) {
        case 0x02:
        case 0x21:
        case 0x22:
        case 0x24:
        case 0x25:
        case 0x35:
        case 0x36:
        case 0x37: return 1;
        default: return 0;
    }
}

static void
vcrypt_stamp(dsd_state* state, uint8_t slot) {
    state->dmr_vc_ref_clock[slot] = state->symbol_clock;
    state->dmr_vc_ref_epoch[slot] = state->symbol_clock_epoch;
    state->dmr_vc_ref_carrier[slot] = state->carrier_seq;
}

static int
vcrypt_same_timeline(const dsd_state* state, uint8_t slot) {
    return state->dmr_vc_ref_epoch[slot] == state->symbol_clock_epoch
           && state->dmr_vc_ref_carrier[slot] == state->carrier_seq;
}

/* The chain is unknown from here: encrypted voice on the slot is muted until fresh signalling re-keys it, rather than
   decrypted with an MI that belongs to some other superframe. Whatever the ALG ID says now: --dmr-force-algid installs
   one only when a frame decodes, after this superframe's voice sync was read with none. */
static void
vcrypt_lose_chain(dsd_state* state, uint8_t slot) {
    state->dmr_vc_ref_state[slot] = DMR_VC_REF_NONE;
    state->dmr_vc_untrusted[slot] = 1U;
    state->dmr_le_candidate_valid[slot] = 0U;
    /* Nor are the identifiers a PI checked known to be the call's any more (a timeline break: a replay starting over, a
       stream flush; a PI that led to no voice of its own): an SB identifier may name the call's again. */
    state->dmr_crypto_pi_verified[slot] = 0U;
}

void
dmr_vcrypt_lose(dsd_state* state, uint8_t slot) {
    if (state == NULL || slot > 1U) {
        return;
    }
    vcrypt_lose_chain(state, slot);
}

void
dmr_vcrypt_note_first(dsd_state* state, uint8_t slot) {
    if (state == NULL || slot > 1U) {
        return;
    }
    state->dmr_vc_ref_state[slot] = DMR_VC_REF_FIRST;
    state->dmr_vc_untrusted[slot] = 0U;
    /* A late-entry MI held against the old chain is no candidate against this one. */
    state->dmr_le_candidate_valid[slot] = 0U;
    vcrypt_stamp(state, slot);
}

void
dmr_vcrypt_note_next_superframe_mi(dsd_state* state, uint8_t slot) {
    if (state == NULL || slot > 1U) {
        return;
    }
    /* The reference still names this superframe's voice sync; dmr_vcrypt_superframe_end() moves it on to the next
       one, whose IV the installed MI is. Without a voice sync on this timeline there is nothing to count from. The
       slot is trusted from that next voice sync on (dmr_vcrypt_superframe_start()): frames of this superframe still to
       decode (a Hytera LC read ahead of VC6's) have a keystream from the old MI. */
    if (state->dmr_vc_ref_state[slot] == DMR_VC_REF_FIRST || !vcrypt_same_timeline(state, slot)) {
        return;
    }
    state->dmr_vc_ref_state[slot] = DMR_VC_REF_VALID;
}

int
dmr_vcrypt_pi_is_fresh(const dsd_state* state, uint8_t slot) {
    if (state == NULL || slot > 1U || state->dmr_vc_ref_state[slot] != DMR_VC_REF_FIRST
        || !vcrypt_same_timeline(state, slot)) {
        return 0;
    }
    return state->symbol_clock >= state->dmr_vc_ref_clock[slot]
           && state->symbol_clock - state->dmr_vc_ref_clock[slot] <= (uint64_t)DMR_VC_FIRST_WINDOW_SYMBOLS;
}

/* One superframe's MI step for the slot's cipher, as the VC6 processing takes it. */
static void
vcrypt_step_mi(dsd_state* state, uint8_t slot, int algid) {
    unsigned long long* mi = slot == 0U ? &state->payload_mi : &state->payload_miR;
    switch (algid) {
        case 0x24:
        case 0x25: *mi = dmr_aes_expand_iv((uint32_t)*mi, slot == 0U ? state->aes_iv : state->aes_ivR); break;
        case 0x22: {
            const uint64_t iv = dmr_des_expand_iv((uint32_t)*mi);
            *mi = iv & 0xFFFFFFFFULL;
            if (slot == 0U) {
                state->payload_miP = iv;
            } else {
                state->payload_miN = iv;
            }
            break;
        }
        case 0x21: *mi = dmr_mi_advance32((uint32_t)*mi); break;
        case 0x02: {
            const uint8_t saved = (uint8_t)state->currentslot;
            state->currentslot = slot;
            hytera_enhanced_alg_refresh(state);
            state->currentslot = saved;
            break;
        }
        case 0x35:
        case 0x36:
        case 0x37: *mi = kirisun_lfsr(*mi); break;
        default: break;
    }
}

static void
vcrypt_catch_up(dsd_state* state, uint8_t slot, uint64_t superframes) {
    const int algid = vcrypt_slot_algid(state, slot);
    if (!dmr_vcrypt_algid_uses_mi(algid)) {
        return;
    }
    for (uint64_t n = 0; n < superframes; n++) {
        vcrypt_step_mi(state, slot, algid);
    }
    /* A held late-entry MI expected the very next superframe's: superframes went by unseen. */
    state->dmr_le_candidate_valid[slot] = 0U;
    /* The keystream is the old superframe's: build it again from the caught-up material. */
    if (slot == 0U) {
        state->DMRvcL = 0;
        state->dropL = 256;
    } else {
        state->DMRvcR = 0;
        state->dropR = 256;
    }
    DSD_FPRINTF(stderr, "%s Slot %u MI caught up over %llu missed superframe%s;%s\n", KCYN, slot + 1U,
                (unsigned long long)superframes, superframes == 1U ? "" : "s", KNRM);
}

void
dmr_vcrypt_superframe_start(dsd_state* state, uint8_t slot) {
    if (state == NULL || slot > 1U) {
        return;
    }
    const int same = vcrypt_same_timeline(state, slot);
    const uint64_t now = state->symbol_clock;
    const uint64_t ref = state->dmr_vc_ref_clock[slot];

    switch (state->dmr_vc_ref_state[slot]) {
        case DMR_VC_REF_FIRST:
            if (same && now >= ref && now - ref <= (uint64_t)DMR_VC_FIRST_WINDOW_SYMBOLS) {
                state->dmr_vc_ref_state[slot] = DMR_VC_REF_VALID;
            } else {
                /* The PI led to no voice of its own: neither its IV nor the identifiers it checked are this call's. */
                vcrypt_lose_chain(state, slot);
            }
            break;
        case DMR_VC_REF_VALID:
            if (!same) {
                vcrypt_lose_chain(state, slot);
                break;
            }
            /* Superframes from the one the IV is for to this one, to the nearest: the grid's slips move a voice sync
               by a few symbols either way, and the reference can lie ahead of it (dmr_vcrypt_superframe_end()
               projects it one superframe on). Signed, so a sync a symbol early is not 2^64 superframes late. */
            {
                const int64_t delta = (int64_t)(now - ref);
                if (delta >= -(int64_t)(DMR_VC_SUPERFRAME_SYMBOLS / 2)) {
                    const uint64_t superframes =
                        (uint64_t)(delta + DMR_VC_SUPERFRAME_SYMBOLS / 2) / (uint64_t)DMR_VC_SUPERFRAME_SYMBOLS;
                    if (superframes > (uint64_t)DMR_VC_CATCHUP_MAX) {
                        vcrypt_lose_chain(state, slot);
                    } else {
                        if (superframes > 0U) {
                            vcrypt_catch_up(state, slot, superframes);
                        }
                        state->dmr_vc_untrusted[slot] = 0U;
                    }
                } else {
                    vcrypt_lose_chain(state, slot);
                }
            }
            break;
        default: state->dmr_vc_untrusted[slot] = 1U; break;
    }
    vcrypt_stamp(state, slot);
    /* Late-entry fragments assemble one superframe's MI: none held from before this voice sync belongs to it. */
    state->dmr_le_frag_mask[slot] = 0U;
    /* A new superframe: the vocoder takes the key fingerprint again at its first frame, whichever burst that is. */
    state->dmr_ks_tag_burst[slot] = 0U;
}

void
dmr_vcrypt_hytera_vc6_step(dsd_state* state, uint8_t slot) {
    if (state == NULL || slot > 1U) {
        return;
    }
    state->dmr_vc6_seed[slot] = slot == 0U ? state->payload_mi : state->payload_miR;
    state->dmr_vc6_seed_valid[slot] = 1U;
    vcrypt_step_mi(state, slot, 0x02);
}

void
dmr_vcrypt_superframe_end(dsd_state* state, uint8_t slot) {
    if (state == NULL || slot > 1U) {
        return;
    }
    state->dmr_vc6_seed_valid[slot] = 0U;
    /* A held late-entry MI is confirmed only by the very next superframe's: one superframe on, it expires. */
    if (state->dmr_le_candidate_valid[slot] != 0U) {
        state->dmr_le_candidate_valid[slot]--;
    }
    if (state->dmr_vc_ref_state[slot] != DMR_VC_REF_VALID) {
        return;
    }
    state->dmr_vc_ref_clock[slot] += (uint64_t)DMR_VC_SUPERFRAME_SYMBOLS;
}

void
dmr_vcrypt_forget(dsd_state* state, uint8_t slot) {
    if (state == NULL || slot > 1U) {
        return;
    }
    state->dmr_vc_ref_state[slot] = DMR_VC_REF_NONE;
    /* Frames of this superframe still to decode (the VC6 an LC naming a new call was read ahead of) have the abandoned
       MI: they stay muted until the slot is re-keyed. A clear or static cipher is not affected (dsd_mbe.c). */
    state->dmr_vc_untrusted[slot] = 1U;
    state->dmr_le_frag_mask[slot] = 0U;
    state->dmr_le_candidate_valid[slot] = 0U;
    state->dmr_crypto_pi_verified[slot] = 0U;
    state->dmr_vc6_seed_valid[slot] = 0U;
}

void
dmr_vcrypt_set_frame(dsd_state* state, uint8_t slot, uint8_t vc, uint8_t frame) {
    if (state == NULL || slot > 1U) {
        return;
    }
    /* Stored one up, so a zeroed state reads as unpositioned. */
    state->dmr_voice_ks_pos[slot] = (vc >= 1U && vc <= 6U && frame <= 2U) ? (int)(vc - 1U) * 3 + (int)frame + 1 : 0;
}

void
dmr_vcrypt_clear_frame(dsd_state* state, uint8_t slot) {
    if (state == NULL || slot > 1U) {
        return;
    }
    state->dmr_voice_ks_pos[slot] = 0;
}
