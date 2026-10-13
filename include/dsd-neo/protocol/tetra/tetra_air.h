// SPDX-License-Identifier: GPL-3.0-or-later
/* Air-interface algorithm identity and operator-key decrypt.
 * The KSG number comes from D-OTAR SCK/GCK Provide. The 80-bit key is
 * loaded by the operator. Sealed OTAR keys are never unwrapped. */
#ifndef DSD_NEO_PROTOCOL_TETRA_AIR_H
#define DSD_NEO_PROTOCOL_TETRA_AIR_H

#include <stdint.h>

struct dsd_state;

/* Remember KSG 0..3 for this network. Returns 1 when stored. */
int tetra_air_ksg_remember(struct dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t ksg);

/* Returns 0..3, or -1 when this network has no remembered algorithm. */
int tetra_air_ksg_lookup(const struct dsd_state* state, uint16_t mcc, uint16_t mnc);

/* "TEA1".."TEA4", or NULL when ksg is outside 0..3. */
const char* tetra_air_ksg_name(int ksg);

/* Store a 10-byte key for this network, replacing an earlier one. */
int tetra_air_key_set(struct dsd_state* state, uint16_t mcc, uint16_t mnc, const uint8_t key[10]);

/* EN 300 392-7 clause 6.3.2.1. tn/fn/mn are the air values (1..4, 1..18, 1..60).
 * dir is 0 downlink, 1 uplink. Returns 0 when a field is outside that range. */
uint32_t tetra_air_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir);

/* XOR speech bits with the keystream when this is trunked mode, the call is
 * encrypted, the network KSG is TEA1..TEA3, an operator key is loaded, and
 * the hyperframe and TDMA clock are known. Otherwise the bits are unchanged.
 * ks_bit_offset is the first keystream bit for this speech fragment. */
int tetra_air_decrypt_voice(struct dsd_state* state, uint8_t* bits, int nbits, int ks_bit_offset);

#endif /* DSD_NEO_PROTOCOL_TETRA_AIR_H */
