// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The P25 Phase 1 sync anchor: a sync word within a few bit errors, where the last validated frame says the
 * next data unit starts (dsd_p25p1_sync_anchor).
 *
 * The hunt matches the 24-dibit P25p1 sync word exactly (normal, inverted, or under a rotated CQPSK dibit map). On a
 * simulcast carrier one symbol of the word past a slicer threshold loses the whole frame. On a continuous outbound
 * carrier every data unit ends on a status symbol and the next one's sync starts right after it (TIA-102.BAAA-A 8.2),
 * so after a validated frame the hunt knows where the next sync must start, and on the RTL CQPSK 4800/4 path it accepts
 * a word there within DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS bit errors. That frame then decodes only when its NID
 * passes a stricter check than an exact sync's (the dispatcher's): until then it earns and publishes nothing.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_P25P1_SYNC_ANCHOR_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_P25P1_SYNC_ANCHOR_H_

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bit errors (of the word's 48) a P25p1 sync word at the anchor's expected position may have and be accepted.
 *
 * OP25's dedicated P1 framer (p25_framer.cc) accepts 6. Exact syncs a slip of 1 to 5 symbols put near the expected
 * position are not pre-empted: the word's distance to itself shifted by 1, 2, 3, 4 and 5 symbols is 9, 10, 9, 8 and 9
 * bits even when the symbols shifted in match, so the expected-position window then lies more than 6 bits off and the
 * exact word is matched where it is.
 */
#define DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS 6

/**
 * @brief Bit errors between the 24-dibit window @p window, read under CQPSK dibit map @p map_idx, and the P25p1 sync
 * word (its inverted form when @p inverted).
 *
 * Each dibit counts the bits it differs by (P25's dibits are Gray coded over the four levels: +3 01, +1 00, -1 10, -3
 * 11), so one level off counts 1 and a sign error 1 or 2. A byte outside '0'..'3' counts 2. Returns 48 when @p window
 * holds fewer than 24 dibits.
 */
int dsd_p25p1_sync_bit_distance(const char* window, uint8_t map_idx, int inverted);

/**
 * @brief The NAC the NID of the frame being decoded must name, when its sync was accepted within the tolerance; -1
 * after an exact sync.
 */
int dsd_frame_sync_p25p1_tolerant_nac(const dsd_state* state);

/**
 * @brief Tell the frame sync what the P25p1 frame its last sync opened was, once decoded.
 *
 * When the frame validated, @p nac is the NAC its NID named and @p unit_dibits its length on air (sync word and status
 * symbols included): the anchor is installed at its sync plus that length, when the frame's sync-time context still
 * holds (the same clock epoch, input, carrier, tuning generation, sync history drops, SPS profile and modulation, no
 * off-thread tune pending) and the decoder has not read past that position. 0 for @p unit_dibits when the frame did
 * not validate or its length is not known: nothing is installed. Either way the frame's sync context is spent.
 */
void dsd_frame_sync_p25p1_frame_decoded(dsd_state* state, int nac, uint32_t unit_dibits);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_P25P1_SYNC_ANCHOR_H_ */
