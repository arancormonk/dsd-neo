// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Protocol-internal DMR CRC provenance: where the burst handler's RAS verdict is handed on. */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_CRC_PROVENANCE_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_CRC_PROVENANCE_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

/*
 * dsd_state::data_block_crc_valid value for a confirmed data block whose CRC9 failed but which the RAS heuristic
 * accepted under -F. Anything other than 1 is not a verified block, so the location gates still refuse it; the event
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

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_CRC_PROVENANCE_H_ */
