// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_PROTOCOL_TETRA_BSCH_H
#define DSD_NEO_PROTOCOL_TETRA_BSCH_H

/*
 * TETRA BSCH (Broadcast Synchronisation CHannel) PDU parser.
 *
 * The BSCH is carried in the SB1 block (first 120 type-5 bits, 60 type-1 bits)
 * of an SB (Synchronisation Burst).  After lower-MAC FEC it yields 60 information
 * bits (type-1) whose layout per ETSI EN 300 392-2 §21.3.3 / osmo-tetra is:
 *
 *   Offset  Length  Field
 *   ------  ------  -----
 *     0       4     Scrambling (reserved / last 2 bits of previous scrambling cycle)
 *     4       6     Colour code (full 6-bit CC; CB field carries bits [1:0])
 *    10       2     TN  (timeslot number, value+1)
 *    12       5     FN  (frame number 1-18)
 *    17       6     MN  (multiframe number 1-60)
 *    23       8     Reserved / HN partial
 *    31      10     MCC (Mobile Country Code)
 *    41      14     MNC (Mobile Network Code)
 *    55       5     Reserved
 *   [Total: 60 bits]
 *
 * Usage: call tetra_bsch_parse() with the 60 decoded bits; on CRC-OK the
 * function writes MCC, MNC and colour into the supplied dsd_state fields and
 * computes tetra_lfsr_seed + sets tetra_net_known = 1.
 */

#include <stdint.h>

/* Forward declarations — avoid header dependency cycles */
struct dsd_opts;
struct dsd_state;

/*
 * tetra_bsch_parse() — parse a decoded BSCH PDU and update dsd_state.
 *
 * @bits      : pointer to exactly 60 decoded type-1 bits (0/1 values)
 * @len       : must be exactly 60
 * @opts      : dsd_opts (used for logging)
 * @state     : dsd_state to update (tetra_mcc, tetra_mnc, tetra_colour,
 *              tetra_lfsr_seed, tetra_net_known)
 *
 * Returns 1 on success (fields written), 0 if len != 60.
 * No CRC check is performed here; the caller is responsible for only passing
 * CRC-verified bits.
 */
int tetra_bsch_parse(const uint8_t *bits, int len,
                     struct dsd_opts *opts, struct dsd_state *state);

#endif /* DSD_NEO_PROTOCOL_TETRA_BSCH_H */
