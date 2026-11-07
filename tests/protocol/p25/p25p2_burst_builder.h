// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test support: P25 Phase 2 windows built the way a site sends them, MAC CRC, Reed-Solomon parity and scrambling
 * included, in the p2bit layout p25p2_frame.c reads (issue #804).
 */

#ifndef DSD_NEO_TESTS_PROTOCOL_P25_P25P2_BURST_BUILDER_H_
#define DSD_NEO_TESTS_PROTOCOL_P25_P25P2_BURST_BUILDER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One timeslot of a window. @c duid is the data unit ID (0-15). For a FACCH (9, 15), SACCH (3, 12) or LCCH (4, 13)
   @c mac holds the payload MSB first: FACCH 156 bits, SACCH and LCCH 180. With @c fill_crc set the builder writes
   the MAC checksum over it (FACCH CRC-12 at bits 144-155, SACCH CRC-12 at 168-179, LCCH CRC-16 at 164-179);
   otherwise those bits come from @c mac as given. A voice (0, 6) or data (10) burst carries zero bits, scrambled. */
typedef struct {
    int duid;
    uint8_t mac[24];
    int fill_crc;
} p25p2_bb_slot;

/* Fills p2bit-layout bits (1440 per 4-timeslot window incl. ISCH) for one window starting at superframe slot
   t0 (0..11), with per-timeslot DUID and payload, MAC CRC filled, RS encoded, scrambled with code (0 = none).

   @c code packs WACN(20) << 24 | SYSID(12) << 12 | NAC(12), as p25p2_generate_scramble_bits() takes it. Scrambled
   bursts (DUID 0, 3, 4, 6, 9, 10) have every burst bit but the DUID XORed with the superframe sequence at
   (20 + 360 * T + b) % 4320, T = (t0 + timeslot) % 12; unscrambled ones (12, 13, 15) are sent as built. The ISCH
   after each timeslot is never scrambled: an S-ISCH after superframe slots 1, 2, 5, 6, 9 and 10, a channel-1 I-ISCH
   naming location 0, 1 or 2 after slots 0, 4 and 8 (which a decoder reads its window's position from), and a
   channel-0 I-ISCH after the others. */
void p25p2_bb_window(uint64_t code, int t0, const p25p2_bb_slot slots[4], uint8_t out_bits[1440]);

/* Corrupts one RS hexbit: inverts the six bits of RS(63,35) symbol @p rs_index (FACCH 9..53, SACCH and LCCH 5..56) of
   the burst in @p timeslot, whose kind the DUID already in @p bits says. Anything else is left alone. */
void p25p2_bb_flip_symbol(uint8_t bits[1440], int timeslot, int rs_index);

/* The burst bit offsets (0..319, from the timeslot's start) of RS symbol @p rs_index's six bits, MSB first, for a
   burst of data unit ID @p duid. Returns 1, or 0 when the DUID is no FACCH, SACCH or LCCH or the symbol is not sent. */
int p25p2_bb_symbol_offsets(int duid, int rs_index, int offsets[6]);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TESTS_PROTOCOL_P25_P25P2_BURST_BUILDER_H_ */
