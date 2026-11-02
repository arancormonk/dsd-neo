// SPDX-License-Identifier: GPL-3.0-or-later
/* Hands the engine's NFM call (engine/analog_call.c) one block, for the tests that drive it without the DSP. */
#ifndef DSD_NEO_TEST_ANALOG_CALL_TEST_SUPPORT_H
#define DSD_NEO_TEST_ANALOG_CALL_TEST_SUPPORT_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <stddef.h>

/* One block as the monitor hands it (issue #668): clean, with no wall time, and on a RECEPTION its own carrier. A test
   that needs another descriptor builds it and calls dsd_engine_analog_call_block() itself. */
static inline void
dsd_test_analog_call_feed(dsd_opts* opts, dsd_state* state, const float* samples, size_t count, int sample_rate,
                          int input) {
    const dsd_analog_call_block block = {
        .samples = samples,
        .count = count,
        .sample_rate = sample_rate,
        .input = input,
        .ramp_end = 0,
        .qualified_carrier = input == DSD_ANALOG_CALL_RECEPTION,
    };
    dsd_engine_analog_call_block(opts, state, &block);
}

#endif
