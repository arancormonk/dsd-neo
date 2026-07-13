// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Private P25 trunking state-machine helpers.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_TRUNK_SM_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_P25_P25_TRUNK_SM_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void p25_sm_note_encrypted_call_typed(dsd_opts* opts, dsd_state* state, int target, int is_group);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_TRUNK_SM_INTERNAL_H_ */
