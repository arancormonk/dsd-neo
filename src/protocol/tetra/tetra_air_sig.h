// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Air interface decryption of TETRA trunked mode downlink signalling with an operator's key.
 *
 * With air interface encryption on, everything after the channel allocation flag of a MAC-RESOURCE and of a downlink
 * MAC-END is ciphertext, the channel allocation element and the TM-SDU (EN 300 392-7 clause 6.7.1.2), and so is a
 * MAC-FRAG's TM-SDU fragment and a MAC-D-BLCK's TM-SDU. The MAC header and the fill bits stay clear (clause 6.4.0). The
 * key stream is the slot's: the frame numbering initial value (clause 6.3.2.1) under the key of the addressed party
 * through TB5 (clause 6.3.2.2), restarted for each PDU, from KSS(0) in a full slot or a first half slot and from
 * KSS(216) in a second half slot (clause 6.4.2.2, table 6.4). The SSI of an encrypted PDU's address is an encrypted short
 * identity, TA61 of the SSI under the SCK or CCK (clause 4.2.6).
 *
 * Basic link signalling carries no check of its plaintext, so a wrong key, a wrong TEA or a PDU under a key the user
 * does not hold (a DCK, an MGCK, another key version) decrypts to noise the PDU readers would act on. A decrypted
 * TM-SDU is acted on only once its own plaintext proved itself: an earlier TM-SDU with the same clear header and length,
 * taken in another slot, so under another key stream, decrypted under the same key to the same plaintext. The SwMI
 * repeats its allocation signalling, the D-SETUPs of late entry included. A wrong key matches with a chance of about
 * 2^-L per comparison for L encrypted bits (and 2^-64 for the digest), so only TETRA_AIR_PROOF_MIN_BITS or more count.
 *
 * Decoder thread only; the state lives in tetra_state_t (air_evidence, air_epoch and the cell key fields).
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_AIR_SIG_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_AIR_SIG_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#include "tetra_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Seconds an encrypted TM-SDU is kept for a repeat to prove: allocations repeat within a frame and late-entry
 * D-SETUPs within three in the networks seen; 18 frames leave room. */
#define TETRA_AIR_EVIDENCE_S     1.0
/** Encrypted bits a TM-SDU needs before a repeat of it proves anything. */
#define TETRA_AIR_PROOF_MIN_BITS 48

/* tetra_air_sig_header::kind */
enum {
    TETRA_AIR_SIG_RESOURCE = 1, /* a whole TM-SDU in one MAC-RESOURCE */
    TETRA_AIR_SIG_FRAGMENTED,   /* a TM-SDU rebuilt from a first fragment, MAC-FRAGs and a MAC-END */
    TETRA_AIR_SIG_D_BLCK,       /* a MAC-D-BLCK */
};

/** The clear part of an encrypted PDU: what tells one TM-SDU from another before it is decrypted. */
typedef struct {
    uint32_t addr;  /**< SSI, ESI, USSI or SMI as sent; 0 for none */
    uint16_t label; /**< event label as sent */
    uint8_t kind;   /**< TETRA_AIR_SIG_* */
    uint8_t addr_type;
    uint8_t enc_mode;
    uint8_t alloc; /**< a channel allocation is in the ciphertext */
    uint8_t um;    /**< usage marker as sent */
} tetra_air_sig_header;

/** The slot the block being decoded came in as an encrypted piece: its initial value, the carrier being received and
 * the key stream bit @p offset its PDUs start at. 0 when it cannot be decrypted: no TDMA clock or hyperframe, direct
 * mode or the uplink, no carrier number, or no key for the network at all. */
int tetra_air_sig_slot(const dsd_opts* opts, const dsd_state* state, int offset, tetra_mac_piece* piece);

/**
 * @brief Decrypt the encrypted TM-SDU @p ct (@p nbits ciphertext bits made of @p npieces pieces, each under its own
 * slot's key stream) and decide whether its plaintext is acted on.
 *
 * Every candidate key of the network decrypts it, and the digest of each plaintext is kept as evidence. When an earlier
 * TM-SDU with header @p h and the same length, taken in another slot, decrypted to the same plaintext under a
 * candidate, that candidate proved it: its plaintext goes to @p pt (@p nbits bits), the candidate to @p used, its TEA
 * to the network's KSG evidence, and 1 is returned. 0 otherwise: the TM-SDU stays ciphertext.
 */
int tetra_air_sig_open(dsd_state* state, const tetra_air_sig_header* h, const uint8_t* ct, int nbits,
                       const tetra_mac_piece* pieces, int npieces, uint8_t* pt, tetra_air_cand* used);

/** The SSI of @p addr when it is an encrypted short identity and @p used is an 80-bit key: TA61 decrypts it under
 * that key (EN 300 392-7 clause 4.2.6), which is the SCK or CCK the operator loaded. A reduced key holds no 80-bit
 * key, so its PDUs keep the ESI. Returns 1 when the address now names its party. */
int tetra_air_sig_identity(const tetra_air_cand* used, tetra_mac_address* addr);

/** The encryption mode of an encrypted downlink MAC header: its low bit is the cell's key version parity (EN 300
 * 392-7 clause 6.5.1), which a run of headers settles; a settled parity switching moves the cell key epoch. */
void tetra_air_sig_note_parity(dsd_state* state, uint8_t enc_mode);

/** A SYSINFO: the SCK-VN or CCK-id of one with the CCK flag set (@p cck_flag, @p value; EN 300 392-7 clause
 * 6.3.2.1), and the SCKN and GCK support of one with the extended services broadcast (@p sec). Another version, SCKN
 * or GCK support than the last moves the cell key epoch. */
void tetra_air_sig_note_sysinfo(dsd_state* state, int cck_flag, uint16_t value, const tetra_cell_security* sec);

/** Another cell, location area or network: the cell key epoch moves and the evidence goes. */
void tetra_air_sig_forget_cell(dsd_state* state);

/** A carrier boundary: the evidence goes (it is about one carrier's TB5 binding). */
void tetra_air_sig_forget_carrier(dsd_state* state);

/** 1 while @p key, which proved a call's signalling, still applies: the cell key epoch it proved under, the parity the
 * cell's signalling uses, and the same key still loaded (with the same named TEA). */
int tetra_air_sig_key_current(const dsd_state* state, const tetra_air_cand* key);

/** XOR @p nbits speech bits from key stream bit @p offset of the slot being decoded under @p key, on the carrier
 * being received (a reduced key only on its own channel). Returns 1 when the key stream applied. */
int tetra_air_sig_decrypt_voice(const dsd_opts* opts, const dsd_state* state, const tetra_air_cand* key, uint8_t* bits,
                                int nbits, int offset);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_AIR_SIG_H_ */
