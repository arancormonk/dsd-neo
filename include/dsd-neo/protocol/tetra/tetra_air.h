// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA trunked mode (TMO) air interface decryption of speech with an operator key.
 *
 * TMO air interface encryption (EN 300 392-7 clause 6) runs a TEA key stream generator from an initial value built of
 * the slot, frame, multiframe and hyperframe numbers. The operator loads keys into dsd_state::tetra_air_keys: an 80-bit
 * cipher key (SCK or CCK) of a network, from which TB5 derives the encryption cipher key of the carrier being received
 * with its carrier number, location area and colour code (clause 6.3.2.2); or a TEA1 reduced key -- the 32-bit register
 * TEA1 compresses that ECK into -- of one channel, which needs neither TB5 nor the compression and decrypts that
 * channel only. A key may name its algorithm; one that does not runs the algorithm the network's signalling named
 * (KSG evidence: D-OTAR SCK/GCK Provide, the ciphering parameters of D-LOCATION UPDATE COMMAND and REJECT), and only
 * while that signalling named one: a SwMI may support several (clause 6.3.1). Sealed OTAR keys are never unwrapped.
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

/* Note that CRC-passed signalling named KSG number @p ksg (EN 300 392-7 table 6.2: 0..3 TEA1..TEA4, 8..15
 * proprietary) for this network. The evidence of a network an air key is loaded for is never evicted; a full cache
 * replaces the oldest keyless network's. Returns 1 when stored, 0 for a reserved KSG (4..7) or a full cache. */
int tetra_air_ksg_remember(dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t ksg);

/* The KSG numbers the network's signalling named: bit k for KSG k; 0 for none. */
uint16_t tetra_air_ksg_seen(const dsd_state* state, uint16_t mcc, uint16_t mnc);

/* Note that a CRC-passed clear U-LOCATION UPDATE DEMAND on this network's uplink proposed KSG number @p ksg (EN 300
 * 392-7 clause 6.6.2.1: the MS declares its preferred KSG, which the SwMI accepts or rejects). A proposal is not the
 * network's word: it is kept apart from what the network's signalling named and never chooses a key's algorithm. The
 * entry, its eviction and the pinning of keyed networks are tetra_air_ksg_remember()'s. Returns 1 when stored, 0 for a
 * reserved KSG (4..7) or a full cache. */
int tetra_air_ksg_request(dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t ksg);

/* The KSG numbers radios proposed on the network's uplink (tetra_air_ksg_request()): bit k for KSG k; 0 for none. */
uint16_t tetra_air_ksg_requested(const dsd_state* state, uint16_t mcc, uint16_t mnc);

/* The network's one algorithm: the KSG number its signalling named, or -1 when it named none or more than one. */
int tetra_air_ksg_lookup(const dsd_state* state, uint16_t mcc, uint16_t mnc);

/* "TEA1".."TEA4", or NULL when ksg is outside 0..3. */
const char* tetra_air_ksg_name(int ksg);

/* The initial value of EN 300 392-7 clause 6.3.2.1. tn/fn/mn are the air values (1..4, 1..18, 1..60); only the low 15
 * bits of the hyperframe number hn are used; dir is 0 downlink, 1 uplink. Returns 0 when a field is out of range. */
uint32_t tetra_air_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir);

/* The key that decrypts trunked mode speech on @p carrier (0..4095, or -1 when the carrier is not known) of the cell
 * being received, once its network identity and location area are known: a TEA1 reduced key of that channel (MCC, MNC,
 * location area, colour code, carrier), else the network's 80-bit key with the algorithm it names, else with the
 * network's one TEA1..TEA3 (tetra_air_ksg_lookup()). An unknown carrier takes no reduced key. Returns 1 and sets
 * @p kind (DSD_TETRA_AIR_KEY_*) and @p tea (1..3 for TEA1..TEA3), either may be NULL; 0 when no key applies. The TDMA
 * clock is not asked: a key stays usable while a retune has it down. */
int tetra_air_key_for_carrier(const dsd_state* state, int carrier, uint8_t* kind, uint8_t* tea);

/* 1 when tetra_air_key_for_carrier() finds a key for @p carrier. */
int tetra_air_key_usable_on(const dsd_state* state, int carrier);

/* XOR the speech bits of a call whose air interface encryption mode is enc_mode (TETRA_ENC_MODE_*, from the
 * MAC-RESOURCE PDUs of the call) with the key stream when this is trunked mode, enc_mode is an encrypted one, the
 * carrier number (opts may be NULL; see tetra_calls_carrier()) is known, a key applies to it
 * (tetra_air_key_for_carrier()), and the hyperframe and TDMA clock are known. The uplink carries
 * no BSCH, so its clock is never set and uplink speech stays as received. ks_bit_offset is the first key stream bit for
 * this speech fragment (TETRA_AIR_KSS_*). Returns 1 when the bits were decrypted, 0 when they are unchanged. */
int tetra_air_decrypt_voice(const dsd_opts* opts, const dsd_state* state, uint8_t enc_mode, uint8_t* bits, int nbits,
                            int ks_bit_offset);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_AIR_H */
