// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_ENGINE_ANALOG_CALL_H
#define DSD_NEO_ENGINE_ANALOG_CALL_H
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
void dsd_engine_analog_call_tick(dsd_opts* opts, dsd_state* state);
int dsd_engine_analog_call_active(const dsd_state* state);
int dsd_engine_analog_call_due(const dsd_state* state);
#ifdef __cplusplus
}
#endif
#endif
