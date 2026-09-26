// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA BSCH (Broadcast Synchronisation CHannel) PDU parser.
 *
 * Extracts MCC, MNC and colour code from 60 decoded type-1 bits, then
 * updates dsd_state so subsequent NDB blocks use the correct scrambling seed.
 *
 * Bit layout (from osmo-tetra tetra_lower_mac.c / ETSI EN 300 392-2 §21.3.3):
 *   offset  +4 : 6-bit colour code
 *   offset +10 : 2-bit timeslot number (TN, add 1 → range 1-4)
 *   offset +12 : 5-bit frame number   (FN, range 0-17)
 *   offset +17 : 6-bit multiframe number (MN, range 0-59)
 *   offset +31 : 10-bit MCC
 *   offset +41 : 14-bit MNC
 */

#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/opts.h>

#include <stdio.h>
#include <string.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>

/* Phase 81: bits_to_uint unified — see tetra_bits.h */
#define bits_to_uint  tetra_bits_to_uint

int tetra_bsch_parse(const uint8_t *bits, int len,
                     dsd_opts *opts, dsd_state *state)
{
    if (!bits || !state) return 0;
    if (len < 60) {
        fprintf(stderr, "[TETRA BSCH] short PDU len=%d, need 60\n", len);
        return 0;
    }

    /* Extract fields from decoded type-1 bits */
    uint8_t  colour = (uint8_t) bits_to_uint(bits, 4,  6);   /* 6-bit colour code   */
    uint8_t  tn_raw = (uint8_t) bits_to_uint(bits, 10, 2);   /* 2-bit TN (0-based)  */
    uint8_t  fn     = (uint8_t) bits_to_uint(bits, 12, 5);   /* 5-bit frame number  */
    uint8_t  mn     = (uint8_t) bits_to_uint(bits, 17, 6);   /* 6-bit multiframe    */
    uint16_t mcc    = (uint16_t)bits_to_uint(bits, 31, 10);  /* 10-bit MCC          */
    uint16_t mnc    = (uint16_t)bits_to_uint(bits, 41, 14);  /* 14-bit MNC          */

    uint32_t seed   = tetra_compute_scramb_seed(mcc, mnc, colour);

    /* Phase 56: clear colour-change flag at the start of each BSCH parse
     * so it only stays 1 for one frame/iteration. */
    state->tetra_bsch_colour_changed = 0;

    /* Phase 24: detect colour code change before updating state */
    if (state->tetra_net_known && state->tetra_colour != colour)
        state->tetra_bsch_colour_changed = 1;

    /* Update state */
    state->tetra_mcc       = mcc;
    state->tetra_mnc       = mnc;
    state->tetra_colour    = colour;
    state->tetra_lfsr_seed = seed;
    state->tetra_net_known = 1;
    state->tetra_bsch_count++;

    /* Phase 43: TDMA timestamps */
    state->tetra_tn         = tn_raw + 1u;  /* convert to 1-based (1-4) */
    state->tetra_fn         = fn;
    state->tetra_mn         = mn;
    state->tetra_tdma_valid = 1;

    fprintf(stderr, "[TETRA BSCH] MCC=%u MNC=%u CC=0x%02x seed=0x%08x  TN=%u FN=%u MN=%u\n",
            mcc, mnc, colour, seed, state->tetra_tn, fn, mn);
    (void)opts;
    return 1;
}
