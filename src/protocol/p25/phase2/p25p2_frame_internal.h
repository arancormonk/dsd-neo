// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief Private P25 Phase 2 frame-processing helpers.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_FRAME_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_FRAME_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
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
/* Descramble the buffered superframe with the seed in force (WACN, SYSID, p2_cc), which a burst decoded from it then
   proves (issue #575); processP2() runs it before p25p2_process_duid(). */
void p25p2_process_frame_scramble(dsd_opts* opts, const dsd_state* state);
void p25p2_generate_scramble_bits(uint64_t wacn, uint64_t sysid, uint64_t nac, uint8_t* out_bits, size_t bit_count);

/* What says the receiver has left the channel a superframe was collected on: the completed trunk-tuning generation,
   the newest unresolved tune request (a PENDING retune counts at once) and the count of p25_p2_frame_reset() calls
   (issue #651). A retune accepted while a superframe's bursts are dispatched (a grant or a return to the control
   channel in a MAC PDU, or a rekey commit that locks the call out) changes it, and nothing more of that superframe
   may act on the assignment the receiver moved to. */
typedef struct {
    uint64_t tune_generation;
    uint64_t pending_request;
    uint32_t frame_reset_generation;
} p25p2_retune_token;

p25p2_retune_token p25p2_retune_token_now(void);
/* p25_p2_frame_reset() calls this: the reset is part of the token. */
void p25p2_retune_note_frame_reset(void);
/* Non-zero when the receiver retuned since @p since was taken. */
int p25p2_retune_token_changed(const p25p2_retune_token* since);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_P25_PHASE2_P25P2_FRAME_INTERNAL_H_ */
