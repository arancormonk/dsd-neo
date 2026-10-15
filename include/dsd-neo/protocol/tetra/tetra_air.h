// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA trunked mode (TMO) air interface decryption of speech with an operator key.
 *
 * TMO air interface encryption (EN 300 392-7 clause 6) runs a TEA key stream generator from an initial value built of
 * the slot, frame, multiframe and hyperframe numbers. The algorithm (KSG number) comes from D-OTAR SCK/GCK Provide;
 * the 80-bit cipher key (SCK or CCK) is loaded by the operator into dsd_state::tetra_air_keys per network, and the
 * generator takes the encryption cipher key TB5 derives from it with the carrier number, location area and colour
 * code of the carrier being received (clause 6.3.2.2). Sealed OTAR keys are never unwrapped.
 *
 * Direct mode air interface encryption (EN 300 396-6) is a different scheme: its key stream generator is named in the
 * DMAC-SYNC and starts from the time variant parameter there, not from the TMO frame numbering, with a static cipher
 * key. It is not implemented, so direct mode speech is never passed through here; tetra_dmo_call_crypt() tells the
 * voice path whether a direct mode call is clear.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_AIR_H
#define DSD_NEO_PROTOCOL_TETRA_AIR_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* First key stream bit of each speech channel in its slot's key stream segment (EN 300 392-7 table 6.4): a full-slot
 * TCH/S takes KSS(0..273); the speech half of an STCH+TCH/S slot takes KSS(216..352). The encryption sits above the
 * channel coding (clause 6.1), so the key stream covers the speech frame bits B1..B137 in the order the codec made
 * them, frame A then frame B, not the sensitivity-class order the speech channel coding gives them (EN 300 395-2
 * clause 5.2, tables 5 and 6). */
#define TETRA_AIR_KSS_TCH_S      0
#define TETRA_AIR_KSS_TCH_S_HALF 216

/* Remember KSG number 0..3 (TEA1..TEA4, EN 300 392-7 table 6.2) for this network. Returns 1 when stored. */
int tetra_air_ksg_remember(dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t ksg);

/* Returns 0..3, or -1 when this network has no remembered algorithm. */
int tetra_air_ksg_lookup(const dsd_state* state, uint16_t mcc, uint16_t mnc);

/* "TEA1".."TEA4", or NULL when ksg is outside 0..3. */
const char* tetra_air_ksg_name(int ksg);

/* The initial value of EN 300 392-7 clause 6.3.2.1. tn/fn/mn are the air values (1..4, 1..18, 1..60); only the low 15
 * bits of the hyperframe number hn are used; dir is 0 downlink, 1 uplink. Returns 0 when a field is out of range. */
uint32_t tetra_air_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir);

/* 1 when trunked mode speech of this network could be decrypted: the network identity and its location area are known,
 * its KSG is TEA1..TEA3 and a cipher key is loaded for it. */
int tetra_air_key_usable(const dsd_state* state);

/* XOR the speech bits of a call whose air interface encryption mode is enc_mode (TETRA_ENC_MODE_*, from the
 * MAC-RESOURCE PDUs of the call) with the key stream when this is trunked mode, enc_mode is an encrypted one, the
 * network KSG is TEA1..TEA3, a cipher key is loaded, the carrier number (opts may be NULL; see tetra_calls_carrier()),
 * location area and colour code TB5 needs are known, and the hyperframe and TDMA clock are known. The uplink carries
 * no BSCH, so its clock is never set and uplink speech stays as received. ks_bit_offset is the first key stream bit for
 * this speech fragment (TETRA_AIR_KSS_*). Returns 1 when the bits were decrypted, 0 when they are unchanged. */
int tetra_air_decrypt_voice(const dsd_opts* opts, const dsd_state* state, uint8_t enc_mode, uint8_t* bits, int nbits,
                            int ks_bit_offset);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_AIR_H */
