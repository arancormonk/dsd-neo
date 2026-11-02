// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_ENGINE_ANALOG_CALL_H
#define DSD_NEO_ENGINE_ANALOG_CALL_H
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/** How long a reception runs before it is a call (issue #667 D1): its RECEPTION blocks' spans, and before the first
 * of them a lead-in of PENDING blocks, counted in 48 kHz samples. A shorter one leaves no row and no recording. */
#define DSD_ANALOG_CALL_MIN_MS 500

/** A processed conventional NFM monitor block, independent of the speaker sink, and what it is to the call (@p input,
 * DSD_ANALOG_CALL_* in <dsd-neo/runtime/frame_sync_hooks.h>): a RECEPTION block is the call's audio; a HOLDABLE (or
 * PENDING) block holds the call through a pause up to opts->analog_record_max_gap_ms (issue #756), or ends it with that
 * hold off; a BREAK, or a block after the receiver's boundaries epoch moved, ends it. Every end is stamped at the last
 * RECEPTION block. A reception becomes a call only once it ran DSD_ANALOG_CALL_MIN_MS; until then it is a candidate
 * nothing else sees, and one that ends first is discarded with its recording (issue #667). */
void dsd_engine_analog_call_block(dsd_opts* opts, dsd_state* state, const float* samples, size_t count, int sample_rate,
                                  int input);
void dsd_engine_analog_call_end(dsd_opts* opts, dsd_state* state);
/** Close and publish the conventional NFM call's recording now, as its end would (issue #708), keeping the call and its
 * lifecycle epoch: per-call recording was switched off mid-call. A candidate's recording is discarded instead. */
void dsd_engine_analog_call_stop_recording(const dsd_opts* opts, dsd_state* state);
void dsd_engine_analog_call_tick(dsd_opts* opts, dsd_state* state);
/** Whether a call is running: a candidate is not one (issue #667). */
int dsd_engine_analog_call_active(const dsd_state* state);
/** Whether a running call or candidate is past the received tone's stale deadline. */
int dsd_engine_analog_call_due(const dsd_state* state);
/** End every call now and commit it: the analog call through its own lifecycle first, so its per-call recording closes
 * under the row it was heard as, then each slot's call with @p reason at @p ended_m. A slot is committed
 * (dsd_event_sync_slot()) when its call ended, or always when @p sync_all. */
void dsd_engine_end_calls(dsd_opts* opts, dsd_state* state, double ended_m, dsd_call_end_reason reason, int sync_all);
#ifdef __cplusplus
}
#endif
#endif
