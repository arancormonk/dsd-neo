// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The input DSP rate policy (dsd_input_dsp_rate_for(), _policy(), _planned()); see
 * <dsd-neo/runtime/analog_channel.h>.
 *
 * Apart from analog_channel.c because a raw I/Q stream's rate comes from its spec and the I/Q rate planner
 * (dsd-neo_io_iq), which the callers that build analog_channel.c on its own do not link.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/io/iq_net_spec.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

uint32_t
dsd_input_dsp_stream_target_hz(int dsp_bw_khz, int frame_tetra) {
    if (dsp_bw_khz > INT_MAX / 1000) {
        return 0U;
    }
    /* A stream records no bandwidth, so the request in force (explicit, a mode preset or the default) is the target. */
    return dsd_input_dsp_target_hz(dsp_bw_khz, 1, 0, frame_tetra);
}

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

/* A raw I/Q stream named by @p audio_in_dev: its spec's RATE planned to the target, or no rate for a spec that does not
   parse (its start refuses it with the parser's reason). */
static dsd_input_dsp_rate
input_dsp_rate_stream(const char* audio_in_dev, int rtl_bw_khz, int frame_tetra) {
    dsd_iq_net_spec spec;
    if (dsd_iq_net_spec_parse(audio_in_dev, &spec, NULL, 0U) != 0) {
        const dsd_input_dsp_rate none = {DSD_ANALOG_RATE_PLANNED, 0};
        return none;
    }
    return dsd_input_dsp_rate_planned(spec.rate_hz, rtl_bw_khz, frame_tetra);
}

/* dsd_input_dsp_rate_for(), with the TETRA setting a raw I/Q stream's target follows. */
static dsd_input_dsp_rate
input_dsp_rate_classify(const char* audio_in_dev, int audio_in_type, int iq_replay_active, int rtl_bw_khz,
                        int frame_tetra) {
    dsd_input_dsp_rate policy = {DSD_ANALOG_RATE_RTL_BW, 0};
    if (iq_replay_active || dsd_opts_audio_in_dev_is_iqreplay_spec(audio_in_dev)) {
        policy.source = DSD_ANALOG_RATE_CAPTURE;
        return policy;
    }
    if (dsd_opts_audio_in_dev_is_iqtcp_spec(audio_in_dev) || dsd_opts_audio_in_dev_is_iqudp_spec(audio_in_dev)) {
        return input_dsp_rate_stream(audio_in_dev, rtl_bw_khz, frame_tetra);
    }
    const int device =
        dsd_opts_audio_in_dev_is_soapy_spec(audio_in_dev) || dsd_opts_audio_in_dev_is_airspy_spec(audio_in_dev);
    const int live_radio = device || audio_in_type == AUDIO_IN_RTL || dsd_opts_audio_in_dev_is_rtl_spec(audio_in_dev)
                           || dsd_opts_audio_in_dev_is_rtltcp_spec(audio_in_dev);
    if (device) {
        policy.source = DSD_ANALOG_RATE_DEVICE;
    }
    if (live_radio && rtl_bw_khz > 0) {
        policy.rate_hz = rtl_bw_khz > INT_MAX / 1000 ? INT_MAX : rtl_bw_khz * 1000;
    }
    return policy;
}

dsd_input_dsp_rate
dsd_input_dsp_rate_for(const char* audio_in_dev, int audio_in_type, int iq_replay_active, int rtl_bw_khz) {
    return input_dsp_rate_classify(audio_in_dev, audio_in_type, iq_replay_active, rtl_bw_khz, 0);
}

dsd_input_dsp_rate
dsd_input_dsp_rate_policy(const dsd_opts* opts) {
    if (!opts) {
        return dsd_input_dsp_rate_for(NULL, AUDIO_IN_RTL, 0, 0);
    }
    return input_dsp_rate_classify(opts->audio_in_dev, opts->audio_in_type, dsd_opts_input_is_iq_replay(opts),
                                   opts->rtl_dsp_bw_khz, opts->frame_tetra == 1);
}
