// SPDX-License-Identifier: ISC
/* Protocol-internal DMR PDU entry points. */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_PDU_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_PDU_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

void dmr_sd_pdu_process(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* dmr_pdu,
                        uint8_t packet_crc_valid);

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_PDU_INTERNAL_H_ */
