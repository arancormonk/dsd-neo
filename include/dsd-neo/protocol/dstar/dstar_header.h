// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_PROTOCOL_DSTAR_HEADER_H
#define DSD_NEO_PROTOCOL_DSTAR_HEADER_H

#include <dsd-neo/protocol/dstar/dstar_header_utils.h>

struct dsd_state;

/**
 * @file
 * @brief D-STAR header decode interface.
 */

/**
 * @brief Decode a coded D-STAR header using soft-decision Viterbi decoding.
 *
 * @return 1 when the header's CRC-16/X.25 over its 39 payload octets matched, 0 otherwise.
 *         This is the only real check on the D-STAR data path, and it fires before
 *         processDSTAR() consumes the 1992 symbols that follow (#391).
 */
int dstar_header_decode_soft(struct dsd_state* state, const float soft_symbols[DSD_DSTAR_HEADER_CODED_BITS]);

/**
 * @brief Put a header whose CRC-16/X.25 passed on the slot 0 call: the RF header or the header slow data repeats.
 *
 * Its fields, printable text: @p rpt2 and @p rpt1 (the route), @p dst (UR) and @p my (the 8-octet MY callsign and the
 * 4-octet suffix, 12 octets). The source is the MY field only when it names a station (dstar_callsign_present() of its
 * first 8 octets): a suffix alone is no caller. The call then becomes the transmission's
 * (dsd_state::dstar_tx_call_epoch, which slow data checks before anything of it reaches a call); a blank MY field
 * leaves the transmission naming none. @p protocol is the D-STAR sync type the header was decoded under; @p data is 1
 * for a data header (flag 0x80).
 */
void dstar_header_observe(struct dsd_state* state, int protocol, int data, const char* rpt2, const char* rpt1,
                          const char* dst, const char* my);

#endif /* DSD_NEO_PROTOCOL_DSTAR_HEADER_H */
