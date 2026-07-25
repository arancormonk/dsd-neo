// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Private P25 trunking state-machine helpers.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_TRUNK_SM_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_P25_P25_TRUNK_SM_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void p25_sm_note_encrypted_call_typed(dsd_opts* opts, dsd_state* state, int target, int is_group);

/**
 * Emit a decoded P25P2 MAC_PTT with the raw metadata needed to coalesce
 * equivalent SACCH/FACCH retransmissions.
 */
int p25_sm_emit_ptt_call_metadata(dsd_opts* opts, dsd_state* state, int slot, int tg, int dst, int src, int is_group,
                                  int svc_bits, const uint8_t signature[17], double observed_m, int facch);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_TRUNK_SM_INTERNAL_H_ */
