// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Protocol-internal DMR CRC provenance: the burst handler's RAS signature and where its verdict is handed on. */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_CRC_PROVENANCE_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_CRC_PROVENANCE_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

/*
 * Largest distance between a received BPTC(196,96) burst and the codeword BPTC_196x96_Decode_Codeword() reaches that
 * still counts toward a RAS signature: the code's guaranteed decoding radius (minimum distance 9), so a codeword this
 * close is the unique nearest one. The decoder is iterative, so a codeword it reports further away may be wrong.
 */
enum { DMR_RAS_MAX_DISTANCE = 4 };

/*
 * 1 iff a BPTC burst is a RAS signature: an eligible burst type, nonzero reserved bits, a valid codeword within
 * DMR_RAS_MAX_DISTANCE, and a failed standard check of that codeword's payload (@p standard_check_ok 0; the RS(12,9)
 * syndrome for a voice LC header or terminator), and not Hytera where byte 1 is a FID/MFID. Eligible: PI 0x00 (FID
 * 0x10 only), voice LC header 0x01, terminator 0x02, CSBK 0x03, MBC header 0x04, data header 0x06, confirmed rate-1/2
 * block 0x07 (@p confirmed_data 1) and USBD 0x0B; never the MBC continuation, rate 3/4, rate 1 or idle bursts.
 * @p payload_bytes are the codeword's 12 payload octets and @p r_bits its R(2..0) value.
 */
int dmr_ras_signature(uint8_t databurst, uint8_t confirmed_data, const uint8_t* payload_bytes, uint8_t r_bits,
                      uint32_t distance, int standard_check_ok);

/*
 * dsd_state::data_block_crc_valid value for a confirmed data block whose CRC9 failed but which was accepted as RAS
 * under -F. Anything other than 1 is not a verified block, so the location gates still refuse it; the event
 * marker reads it as RAS rather than as a failure.
 */
enum { DMR_DATA_BLOCK_CRC_RAS = 2 };

/*
 * dmr_dheader() with the burst's CRC verdict given as DSD_CRC_* bits (0 = the header CRC checked) instead of a 0/1
 * verdict, so a RAS-accepted header is told apart from a failed one. The burst handler classifies the burst itself and
 * passes that classification here; dmr_dheader() maps a failed verdict to DSD_CRC_FAILED.
 */
void dmr_dheader_classified(dsd_opts* opts, dsd_state* state, uint8_t* dheader, uint8_t* dheader_bits,
                            uint8_t crc_flags, uint32_t IrrecoverableErrors);

/*
 * The P_CLEAR the encryption lockout synthesizes to release the voice channel: dmr_cspdu() handles it as a checked CSBK
 * (FID 0xFF), so the release runs as a received one's would, but nothing in it was received, and it names nothing in
 * the received identity record (dsd_state::rx_id).
 */
void dmr_cspdu_release_channel(dsd_opts* opts, dsd_state* state);

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_CRC_PROVENANCE_H_ */
