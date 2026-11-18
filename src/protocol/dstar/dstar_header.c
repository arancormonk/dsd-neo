// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/protocol/dstar/dstar_header_utils.h"

/* A header callsign field of @p len octets as text: shown and put on the call, so printable only. The CRC is taken over
 * the raw header bytes. */
static void
dstar_header_copy_text(const char* field, size_t len, char* out) {
    DSD_MEMCPY(out, field, len);
    out[len] = '\0';
    dstar_text_make_printable(out);
}

int
dstar_header_decode_soft(dsd_state* state, const float soft_symbols[DSD_DSTAR_HEADER_CODED_BITS]) {
    uint16_t soft_costs[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_scrambled[DSD_DSTAR_HEADER_CODED_BITS];
    uint16_t soft_deinterlv[DSD_DSTAR_HEADER_CODED_BITS];
    int radioheaderbuffer2[DSD_DSTAR_HEADER_INFO_BITS];
    char radioheader[41];
    int octetcount, bitcount, loop;
    const unsigned char bit2octet[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};

    // Convert float symbols to uint16_t soft costs
    for (int i = 0; i < DSD_DSTAR_HEADER_CODED_BITS; i++) {
        soft_costs[i] = gmsk_soft_symbol_to_viterbi_cost(soft_symbols[i], state);
    }

    // Scramble and deinterleave soft costs
    dstar_scramble_soft_costs(soft_costs, soft_scrambled, DSD_DSTAR_HEADER_CODED_BITS);
    dstar_deinterleave_soft_costs(soft_scrambled, soft_deinterlv, DSD_DSTAR_HEADER_CODED_BITS);

    // Soft-decision Viterbi decode
    dstar_header_viterbi_decode_soft(soft_deinterlv, DSD_DSTAR_HEADER_CODED_BITS, radioheaderbuffer2,
                                     DSD_DSTAR_HEADER_INFO_BITS);

    DSD_MEMSET(radioheader, 0, 41);

    // note we receive 330 bits, but we only use 328 of them (41 octets)
    // bits 329 and 330 are unused
    octetcount = 0;
    bitcount = 0;
    for (loop = 0; loop < 328; loop++) {
        if (radioheaderbuffer2[loop]) {
            radioheader[octetcount] |= bit2octet[bitcount];
        }
        bitcount++;
        // increase octetcounter and reset bitcounter every 8 bits
        if (bitcount >= 8) {
            octetcount++;
            bitcount = 0;
        }
    }

    char str1[9];
    char str2[9];
    char str3[9];
    char str4[13];
    dstar_header_copy_text(radioheader + 3, 8U, str1);
    dstar_header_copy_text(radioheader + 11, 8U, str2);
    dstar_header_copy_text(radioheader + 19, 8U, str3);
    dstar_header_copy_text(radioheader + 27, 12U, str4);

    const uint16_t crc_ext = (uint16_t)(((uint8_t)radioheader[39] << 8U) | (uint8_t)radioheader[40]);
    const uint16_t crc_cmp = dstar_crc16((const uint8_t*)radioheader, 39U);

    DSD_FPRINTF(stderr, " RPT 2: %s", str1);
    DSD_FPRINTF(stderr, " RPT 1: %s", str2);
    DSD_FPRINTF(stderr, " DST: %s", str3);
    DSD_FPRINTF(stderr, " SRC: %s", str4);

    //check flags for info
    if (radioheader[0] & 0x80) {
        DSD_FPRINTF(stderr, " DATA");
    }
    if (radioheader[0] & 0x40) {
        DSD_FPRINTF(stderr, " REPEATER");
    }
    if (radioheader[0] & 0x20) {
        DSD_FPRINTF(stderr, " INTERRUPTED");
    }
    if (radioheader[0] & 0x10) {
        DSD_FPRINTF(stderr, " CONTROL SIGNAL");
    }
    if (radioheader[0] & 0x08) {
        DSD_FPRINTF(stderr, " URGENT");
    }

    if (crc_cmp != crc_ext) {
        return 0;
    }

    const int protocol = DSD_SYNC_IS_DSTAR(state->synctype) ? state->synctype : DSD_SYNC_DSTAR_HD_POS;
    dstar_header_observe(state, protocol, ((uint8_t)radioheader[0] & 0x80U) != 0U, str1, str2, str3, str4);
    return 1;
}

void
dstar_header_observe(dsd_state* state, int protocol, int data, const char* rpt2, const char* rpt1, const char* dst,
                     const char* my) {
    if (state == NULL || rpt2 == NULL || rpt1 == NULL || dst == NULL || my == NULL) {
        return;
    }
    const int named = dstar_callsign_present(my, 8U);
    dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = data ? DSD_CALL_KIND_DATA : DSD_CALL_KIND_VOICE,
    };
    /* MY and its suffix name the caller; the suffix alone names no one (a blank MY field). */
    if (named) {
        DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", my);
    }
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", dst);
    DSD_SNPRINTF(observation.route_text[0], sizeof(observation.route_text[0]), "%s", rpt1);
    DSD_SNPRINTF(observation.route_text[1], sizeof(observation.route_text[1]), "%s", rpt2);
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
    /* The transmission's own callsign is on the call now: slow data of this transmission may reach that call. A blank
       MY field names no one, and the call may still carry the last transmission's callsign (its terminator missed). */
    dsd_call_snapshot call;
    state->dstar_tx_call_epoch =
        (named && dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE) ? call.epoch : 0U;
}
