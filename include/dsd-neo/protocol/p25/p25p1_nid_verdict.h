// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What a decoded P25 Phase 1 NID may do: its verdict against the NAC the carrier's NIDs established.
 *
 * The BCH(63,16) code under the NID corrects up to 11 bits, and the soft search flips up to 3 more, so a NID read off
 * a corrupted frame can decode to another codeword: a control channel TSDU's NID read as an LDU1 naming another NAC,
 * which then decodes voice from the TSDU and moves the received NAC. A NID naming a NAC other than the one the
 * carrier's NIDs established (0x000 and 0xFFF included) is taken at once only when it needed few corrections; a costly
 * one is held until the next NID naming a NAC on the same stream confirms or refutes it.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P1_NID_VERDICT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P1_NID_VERDICT_H_

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct p25p1_nid_result;

/** Whether a NAC a NID decoded off the air is one dsd-neo takes as the carrier's: twelve bits, neither 0x000 nor 0xFFF,
    which a dead or stuck input decodes as (an all-zero or all-one NID). Only a usable NAC moves the received NAC, the
    seed's or the frame's, or becomes the carrier's established NAC. */
static inline int
p25p1_nid_nac_usable(int nac) {
    return nac > 0x000 && nac < 0xFFF;
}

/** Corrections (BCH plus soft flips) a NID naming another NAC than the carrier's established one may need and still
    be taken at once: OP25's rx_sync NID limit. A parity disagreement does not count. */
#define P25P1_NID_NAC_CHANGE_MAX_CORRECTIONS 4

typedef enum {
    P25P1_NID_REJECTED = 0, /**< The BCH or the DUID check failed: nothing decodes. */
    P25P1_NID_ACCEPTED,     /**< The frame decodes and the NID names its NAC (unless borrowed or 0x000/0xFFF). */
    /** It names a NAC other than the carrier's established one (0x000 or 0xFFF included), with more than
        P25P1_NID_NAC_CHANGE_MAX_CORRECTIONS corrections and no earlier NID naming the same: the frame does not decode
        and nothing takes its NAC, which is held as the candidate the next NID naming a NAC settles. */
    P25P1_NID_PROVISIONAL,
} p25p1_nid_disposition;

/** A decoded NID, read against the NAC the carrier's NIDs established (dsd_state::p25_p1_carrier_nac). */
typedef struct {
    p25p1_nid_disposition disposition;
    int nac;               /**< The decoded NAC (meaningless when rejected). */
    uint8_t duid;          /**< The decoded DUID (meaningless when rejected). */
    int corrections;       /**< Bits the decode corrected: BCH corrections plus soft flips; 0 when rejected. */
    uint8_t parity_agreed; /**< The final parity bit agreed with the DUID (NID_OK). */
    uint8_t nac_borrowed;  /**< The NAC is the observed one a retry put in (p25p1_nid_result::nac_borrowed). */
    /** It names a usable NAC other than the established one (a change, at once, confirmed or held). A NID naming
        0x000 or 0xFFF is no change, whatever its disposition: neither ever becomes the carrier's NAC. */
    uint8_t nac_changed;
    /** A costly NID that names the NAC a held NID named before it: the NID is accepted, and for a usable NAC the
        change confirmed. */
    uint8_t nac_confirmed;
} p25p1_nid_verdict;

/**
 * @brief Read @p decoded against the carrier's established NAC. Changes nothing.
 *
 * Rejected when the decode failed. Otherwise accepted, except a NID that names a NAC off the air (not borrowed) other
 * than the established one, with more than P25P1_NID_NAC_CHANGE_MAX_CORRECTIONS corrections (a soft result past the BCH
 * radius included), and other than the candidate a held NID left: that one is provisional. 0x000 and 0xFFF, the words
 * a dead or stuck input decodes as, are held like any other NAC. With no established NAC (the first NID on the
 * carrier) every decode is accepted.
 */
p25p1_nid_verdict p25p1_nid_verdict_classify(const dsd_state* state, const struct p25p1_nid_result* decoded);

/**
 * @brief Record what @p verdict says about the carrier's NAC.
 *
 * An accepted NID naming a NAC off the air settles a held candidate: it drops it (an unchanged repeat, which refutes a
 * held change, and a confirming NID included) and, when its NAC is usable (p25p1_nid_nac_usable()), establishes that
 * NAC; 0x000 and 0xFFF never become the established NAC. A provisional one holds its NAC as the candidate, 0x000 and
 * 0xFFF included. A rejected or borrowed decode, or one a carrier boundary split (dsd_state_frame_read_lost()),
 * records nothing.
 */
void p25p1_nid_verdict_commit(dsd_state* state, const p25p1_nid_verdict* verdict);

/**
 * @brief The NAC the carrier's NIDs established, or -1 while none holds on the current stream.
 */
int p25p1_carrier_nac_established(const dsd_state* state);

/** Corrections (BCH plus soft flips) within the BCH(63,16) code's design radius: past it the soft search alone made the
    word a codeword. */
#define P25P1_NID_BCH_RADIUS                    11

/** Corrections a NID behind a sync word the hunt accepted within its tolerance may need (OP25 rx_sync's NID limit). */
#define P25P1_NID_TOLERANT_SYNC_MAX_CORRECTIONS 4

/** Whether @p duid is one of the seven data unit IDs TIA-102.BAAA-A Table 8-4 defines. */
static inline int
p25p1_nid_duid_defined(uint8_t duid) {
    switch (duid) {
        case 0x0: /* HDU */
        case 0x3: /* TDU */
        case 0x5: /* LDU1 */
        case 0x7: /* TSDU */
        case 0xA: /* LDU2 */
        case 0xC: /* PDU */
        case 0xF: /* TDULC */ return 1;
        default: return 0;
    }
}

/**
 * @brief Whether @p verdict validates its frame for the P25p1 sync anchor (dsd_p25p1_sync_anchor): accepted, its NAC
 * read off the air (not borrowed) and usable, within the BCH radius, naming a defined DUID.
 */
int p25p1_nid_verdict_validates(const p25p1_nid_verdict* verdict);

/**
 * @brief Whether @p verdict is a strict NID, the one a frame behind a sync word the hunt accepted within a tolerance must
 * have: it validates (p25p1_nid_verdict_validates(): accepted, unborrowed, a usable NAC, a defined DUID), its parity
 * agrees (NID_OK, no parity override), and it needed at most P25P1_NID_TOLERANT_SYNC_MAX_CORRECTIONS corrections, BCH
 * and soft flips together (so no soft recovery past the BCH radius). A word accepted hunt-wide on the C4FM chain, with no
 * anchor, needs this alone.
 */
int p25p1_nid_verdict_strict(const p25p1_nid_verdict* verdict);

/**
 * @brief Whether @p verdict lets the frame behind a sync word accepted within the tolerance at the anchor's expected
 * position decode: a strict NID (p25p1_nid_verdict_strict()) that names @p anchor_nac, the NAC of the frame the anchor
 * was taken from. Anything else drops the frame.
 */
int p25p1_nid_verdict_passes_tolerant_sync(const p25p1_nid_verdict* verdict, int anchor_nac);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P1_NID_VERDICT_H_ */
