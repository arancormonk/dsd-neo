// SPDX-License-Identifier: ISC
/* Protocol-internal DMR PDU entry points. */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_PDU_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_PDU_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

// packet_crc_valid governs only the defined-short-data text decode. Callers must run
// inside an event_crc_invalid save/set/restore scope for the slot; that scope gates
// LOCN location rows (see dmr_block_type1_process_payload in dmr_block.c).
void dmr_sd_pdu_process(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* dmr_pdu,
                        uint8_t packet_crc_valid);

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_PDU_INTERNAL_H_ */
