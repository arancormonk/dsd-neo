// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The PLANNED DSP rate policy of a raw I/Q stream (issue #743); see <dsd-neo/runtime/analog_channel.h>.
 *
 * Apart from analog_channel.c because it runs the I/Q rate planner, which the callers that build analog_channel.c on
 * its own do not link.
 */

#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <stddef.h>
#include <stdint.h>

dsd_input_dsp_rate
dsd_input_dsp_rate_planned(uint32_t stream_rate_hz, int dsp_bw_khz, int frame_tetra) {
    dsd_input_dsp_rate policy = {DSD_ANALOG_RATE_PLANNED, 0};
    const uint32_t target_hz = dsd_input_dsp_stream_target_hz(dsp_bw_khz, frame_tetra);
    dsd_iq_replay_rate_plan plan;
    if (target_hz > 0U
        && dsd_iq_plan_general_rate(DSD_IQ_PLAN_SOURCE_STREAM, stream_rate_hz, target_hz, &plan, NULL, 0U)
               == DSD_IQ_OK) {
        policy.rate_hz = (int)plan.demod_rate_hz;
    }
    return policy;
}
