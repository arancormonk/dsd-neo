// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The access code a call was heard with: DMR/dPMR colour code, P25 NAC, NXDN RAN, M17 CAN.
 *
 * A call history row records the code of the carrier its call was decoded on (issue #575). This header names the
 * kinds and reads the live code for a protocol. It stays out of `state.h` so app-control's public views, which may
 * not include `state.h` or `opts.h`, can name the kinds.
 *
 * Each source the reader takes is set only from content the protocol checked, as below, and is cleared or reset to a
 * sentinel between carriers:
 * - DMR `dmr_color_code`: the slot type's Golay(20,8) (behind the BS colour-code lock) or the MS embedded
 *   signalling's QR(16,7,6); 16 until one decodes, and back to 16 at every carrier boundary.
 * - P25 Phase 1 `nac`: the BCH(63,16) network identifier; 0 between carriers. `p25p1_valid_decoded_nac()` refuses
 *   NAC 0x000 and 0xFFF, so a carrier whose NID decodes either leaves `nac` at 0.
 * - P25 Phase 2 `p2_cc`: the NAC the Phase 2 descrambler runs on (from the network status broadcast or the Phase 1
 *   NID, or set by hand), taken only while `p2_cc_verified` says this carrier proved it: a burst descrambled with it (a
 *   scrambled FACCH or SACCH, or an ESS) passed its Reed-Solomon check, or a checked network status broadcast named
 *   it. A call carried by unscrambled FACCH/SACCH never tests the seed, so a value -X or another carrier left is no NAC
 *   until then; the mark goes at every carrier boundary.
 * - NXDN `nxdn_last_ran`: the CRC-checked CAC, FACCH2/UDCH or SACCH, or a CRC-checked site information message's site
 *   code (Table 6.3-4); (unsigned)-1 until one decodes, and between transmissions. The field also holds values that
 *   are not a RAN, which the terminal shows in its place: an IDAS (Type-D) carrier's SCCH area bit or site type (the
 *   site type behind the SCCH's 7-bit CRC alone), and the fixed 7 a DCR transmission is given. The protocol marks
 *   those with `nxdn_last_ran_stand_in`, and the reader takes none of them: neither carrier has an access code.
 * - dPMR `dpmr_color_code`: the channel code's exact match in the colour-code table, set only once the transmission
 *   is confirmed; -1 between carriers.
 * - M17 CAN: the low four bits of the call's service options, which the CRC-checked LSF carries; only once the call
 *   has service metadata.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_ACCESS_CODE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_ACCESS_CODE_H_H

#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Which access code a value is. The values are pinned: `Event_History::access_code_kind` carries them, the control
 * API sends them, and the Qt call history persists them, so a value is never renumbered or reused.
 */
typedef enum {
    DSD_ACCESS_CODE_NONE = 0,       /**< No code, or none known. */
    DSD_ACCESS_CODE_COLOR_CODE = 1, /**< DMR (0..15) or dPMR (0..63) colour code. */
    DSD_ACCESS_CODE_NAC = 2,        /**< P25 network access code (0x001..0xFFE). */
    DSD_ACCESS_CODE_RAN = 3,        /**< NXDN radio access number (0..63). */
    DSD_ACCESS_CODE_CAN = 4,        /**< M17 channel access number (0..15). */
} dsd_access_code_kind;

/**
 * Say which access code a protocol carries.
 *
 * @param synctype A `DSD_SYNC_*` value.
 * @return COLOR_CODE for DMR (BS and MS) and dPMR, NAC for P25 Phase 1 and 2, RAN for NXDN, CAN for M17; NONE for
 *         everything else (D-STAR, YSF, EDACS, ProVoice, X2-TDMA, analog, no sync).
 */
dsd_access_code_kind dsd_access_code_kind_for_protocol(int synctype);

/**
 * Read a protocol's access code as the decoder holds it now.
 *
 * A P25 Phase 1 call reads `nac` only and a Phase 2 call `p2_cc` only. A fallback from one to the other would read a
 * stale `p2_cc` on a Phase 1 carrier whose NID decoded NAC 0x000 or 0xFFF, which leave `nac` at 0.
 *
 * @param state                Decoder state; NULL reports no code.
 * @param protocol             The call's `DSD_SYNC_*` value.
 * @param service_options      The call's service options (M17 carries its CAN there).
 * @param has_service_metadata Non-zero once the service options were observed.
 * @param kind_out             Receives the `dsd_access_code_kind`; NONE when there is no valid code. May be NULL.
 * @param value_out            Receives the code; 0 when there is no valid code. May be NULL.
 * @return 1 when the protocol's code is valid now, 0 otherwise (unknown, a sentinel, or a protocol without one).
 */
int dsd_access_code_current(const dsd_state* state, int protocol, uint16_t service_options,
                            uint8_t has_service_metadata, uint8_t* kind_out, uint16_t* value_out);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_ACCESS_CODE_H_H */
