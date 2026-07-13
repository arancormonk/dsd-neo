// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief Private P25 Phase 2 frame-processing helpers.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_FRAME_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_FRAME_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void p25p2_teardown_call(dsd_opts* opts, dsd_state* state);
int p25p2_duid_lookup_soft(uint8_t received, const uint8_t* reliab8);
void p25p2_process_facchc(dsd_opts* opts, dsd_state* state, int timeslot_index);
void p25p2_process_sacchc(dsd_opts* opts, dsd_state* state, int timeslot_index);
void p25p2_process_isch(dsd_opts* opts, dsd_state* state, int framing_index);
void p25p2_process_ess(dsd_opts* opts, dsd_state* state, int defer_rekey);
void p25p2_duid_post_timeslot(dsd_opts* opts, dsd_state* state, int timeslot_index, int sacch_status);
void p25p2_process_duid(dsd_opts* opts, dsd_state* state);

#if defined(DSD_NEO_P25P2_TEST_STUB)
void p25p2_decode_voice_frame_for_lockout(dsd_opts* opts, dsd_state* state);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_FRAME_INTERNAL_H_ */
