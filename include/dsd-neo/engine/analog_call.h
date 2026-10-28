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
/** Accepted, processed conventional NFM samples, independent of the speaker sink. */
void dsd_engine_analog_call_block(dsd_opts* opts, dsd_state* state, const float* samples, size_t count, int sample_rate,
                                  int accepted);
void dsd_engine_analog_call_end(dsd_opts* opts, dsd_state* state);
/** Close and publish the conventional NFM call's recording now, as its end would (issue #708), keeping the call and its
 * lifecycle epoch: per-call recording was switched off mid-call. */
void dsd_engine_analog_call_stop_recording(dsd_opts* opts, dsd_state* state);
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
