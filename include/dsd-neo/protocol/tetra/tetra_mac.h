// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA downlink upper MAC and basic-link LLC decoder (EN 300 392-2 clauses 21.2, 21.4 and 23.4).
 *
 * Decodes the type-1 bits of one CRC-passed signalling MAC block (SCH/HD, STCH or SCH/F, pi/4-DQPSK). A block may
 * hold several associated PDUs; each is bounded by its length indication or implicit length (clause 23.4.3.3) and
 * loses its fill bits (clause 23.4.3.2) before its TM-SDU is used. Fragmented TM-SDUs are rebuilt per downlink
 * timeslot (clause 23.4.3.1.1). Clear TM-SDUs pass through the LLC (clause 21.2) to the MLE; encrypted ones are
 * counted and never parsed as plaintext (EN 300 392-7 clause 6.7.1.2).
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_MAC_H
#define DSD_NEO_PROTOCOL_TETRA_MAC_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAC PDU type, table 21.38. */
#define TETRA_MAC_TYPE_RESOURCE    0 /* MAC-RESOURCE */
#define TETRA_MAC_TYPE_FRAG_END    1 /* MAC-FRAG or MAC-END */
#define TETRA_MAC_TYPE_BROADCAST   2 /* TMB-SAP broadcast */
#define TETRA_MAC_TYPE_SUPPL       3 /* MAC-D-BLCK on a full slot, MAC-U-SIGNAL on STCH */

/* Broadcast type, table 21.64. */
#define TETRA_MAC_BC_SYSINFO       0 /* SYSINFO */
#define TETRA_MAC_BC_ACCESS_DEF    1 /* ACCESS-DEFINE */
#define TETRA_MAC_BC_SYSINFO_DA    2 /* SYSINFO-DA (direct-access QAM cells) */
#define TETRA_MAC_BC_RESERVED      3

/* MAC PDU subtype of MAC PDU type 01, tables 21.57 and 21.59. */
#define TETRA_MAC_FRAGE_FRAG       0 /* MAC-FRAG: continuation fragment */
#define TETRA_MAC_FRAGE_END        1 /* MAC-END: last fragment */

/* Address type of MAC-RESOURCE, table 21.55. */
#define TETRA_MAC_ADDR_NULL        0 /* Null PDU */
#define TETRA_MAC_ADDR_SSI         1 /* SSI (24 bits) */
#define TETRA_MAC_ADDR_EVENT_LABEL 2 /* event label (10 bits) */
#define TETRA_MAC_ADDR_USSI        3 /* USSI, migrating MS un-exchanged address (24 bits) */
#define TETRA_MAC_ADDR_SMI         4 /* SMI, management address (24 bits) */
#define TETRA_MAC_ADDR_SSI_EVENT   5 /* SSI + event label assignment (34 bits) */
#define TETRA_MAC_ADDR_SSI_USAGE   6 /* SSI + usage marker assignment (30 bits) */
#define TETRA_MAC_ADDR_SMI_EVENT   7 /* SMI + event label assignment (34 bits) */

/* Encryption mode element of MAC-RESOURCE and MAC-D-BLCK (EN 300 392-7 clause 6.5.1, tables 6.6 and 6.7). Any value
 * other than TETRA_ENC_MODE_NONE means the TM-SDU and the channel allocation element are not plaintext (the reserved
 * value cannot be read either) and the SSI part of the address is an encrypted short identity. */
#define TETRA_ENC_MODE_NONE        0 /* PDU not encrypted */
#define TETRA_ENC_MODE_RESERVED    1 /* reserved */
#define TETRA_ENC_MODE_EVEN        2 /* PDU encrypted, SCK-VN or CCK-id even */
#define TETRA_ENC_MODE_ODD         3 /* PDU encrypted, SCK-VN or CCK-id odd */

/** 1 for the encryption modes that say the PDU is air interface encrypted (EN 300 392-7 tables 6.6 and 6.7). */
static inline int
tetra_enc_mode_encrypted(uint8_t mode) {
    return mode == TETRA_ENC_MODE_EVEN || mode == TETRA_ENC_MODE_ODD;
}

/** Display name of an encryption mode element. */
static inline const char*
tetra_enc_mode_name(uint8_t mode) {
    switch (mode & 0x03u) {
        case TETRA_ENC_MODE_NONE: return "none";
        case TETRA_ENC_MODE_RESERVED: return "reserved";
        case TETRA_ENC_MODE_EVEN: return "on(even)";
        default: return "on(odd)";
    }
}

/**
 * Decode one downlink signalling MAC block.
 *
 * @param bits  type-1 bits of the block, one bit per byte, MSB first; the caller has checked the block CRC
 * @param nbits number of type-1 bits (124 for SCH/HD and STCH, 268 for SCH/F)
 * @param cc    colour code of the burst, used in log lines and to keep fragments of different cells apart
 * @param opts  decoder options; may be NULL
 * @param state decoder state; nothing is decoded without it
 *
 * Fragment reconstruction is keyed by the TDMA timeslot of the block (the TETRA state's tn when the TDMA clock is
 * valid). Blocks received while the decoder follows an uplink are not decoded with the downlink layouts.
 */
void tetra_mac_parse_schd(const uint8_t* bits, int nbits, int cc, dsd_opts* opts, dsd_state* state);

/**
 * A downlink block of the current timeslot failed its SCH CRC: the MAC drops the partial TM-SDU it was rebuilding on
 * that timeslot's control channel (EN 300 392-2 clause 23.4.3.1.1 ii). Applied only while the TDMA clock places the
 * block; without it the N.203 limit still ends the TM-SDU. Uplink blocks are ignored.
 */
void tetra_mac_block_failed(dsd_state* state);

/** Drop every partial TM-SDU: the carrier they were being rebuilt on was left. Never creates TETRA state. */
void tetra_mac_forget_fragments(dsd_state* state);

/**
 * The 32-bit LLC frame check sequence of EN 300 392-2 annex C over @p nbits bits (one bit per byte, MSB first):
 * the first 32 bits complemented, the result multiplied by x^32 and divided by the generator polynomial of
 * equation C.1, and the remainder complemented. Returns the FCS with its first transmitted bit in bit 31.
 */
uint32_t tetra_llc_fcs32(const uint8_t* bits, int nbits);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_MAC_H */
