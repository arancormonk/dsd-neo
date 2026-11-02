// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_ENGINE_ANALOG_CALL_H
#define DSD_NEO_ENGINE_ANALOG_CALL_H
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#ifdef __cplusplus
extern "C" {
#endif
/** A processed conventional NFM monitor block, independent of the speaker sink, and what it is to the call (@p block's
 * input, DSD_ANALOG_CALL_* in <dsd-neo/runtime/frame_sync_hooks.h>): a RECEPTION block is the call's audio, and begins
 * a call only when it heard the carrier itself (@p block's qualified_carrier, issue #668); a HOLDABLE block holds the
 * call through a pause up to opts->analog_record_max_gap_ms (issue #756), or ends it with that hold off; a BREAK ends
 * it. A block that carries another receiver boundaries epoch than the last block did, call or not, ends any call and
 * is dropped. Every end is stamped at the last RECEPTION block. */
void dsd_engine_analog_call_block(dsd_opts* opts, dsd_state* state, const dsd_analog_call_block* block);
void dsd_engine_analog_call_end(dsd_opts* opts, dsd_state* state);
/** Close and publish the conventional NFM call's recording now, as its end would (issue #708), keeping the call and its
 * lifecycle epoch: per-call recording was switched off mid-call. */
void dsd_engine_analog_call_stop_recording(const dsd_opts* opts, dsd_state* state);
void dsd_engine_analog_call_tick(dsd_opts* opts, dsd_state* state);
int dsd_engine_analog_call_active(const dsd_state* state);
int dsd_engine_analog_call_due(const dsd_state* state);
/** End every call now and commit it: the analog call through its own lifecycle first, so its per-call recording closes
 * under the row it was heard as, then each slot's call with @p reason at @p ended_m. A slot is committed
 * (dsd_event_sync_slot()) when its call ended, or always when @p sync_all. */
void dsd_engine_end_calls(dsd_opts* opts, dsd_state* state, double ended_m, dsd_call_end_reason reason, int sync_all);
#ifdef __cplusplus
}
#endif
#endif
