// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* UI command actions — audio domain */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/gain_setting.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>
#include "../command_dispatch.h"
#include "dsd-neo/app_control/commands.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
ui_handle_toggle_mute(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    opts->audio_out = (opts->audio_out == 0) ? 1 : 0;
    const char* msg = (opts->audio_out == 0) ? "Output: Muted" : "Output: On";
    if (opts->audio_out == 1) {
        if (opts->audio_out_type == 0) {
            closeAudioOutput(opts);
            if (openAudioOutput(opts, state) != 0) {
                opts->audio_out = 0;
                msg = "Output: open failed";
            }
        }
    }
    if (state) {
        DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "%s", msg);
        dsd_state_toast_publish(state, dsd_realtime_time() + 3);
    }
    return 1;
}

/* The digital gain setting, mirrored to both slots. The voice chain takes it at the next frame: a fixed gain at once,
   auto from what the speech AGC learned, starting at the fixed gain it leaves (issue #518). */
static void
store_digital_gain(dsd_opts* opts, double g) {
    opts->audio_gain = (float)g;
    opts->audio_gainR = opts->audio_gain;
}

/* A double payload of a full double, or 0 when it is shorter (the queue refuses those before dispatch). */
static int
read_double_payload(const struct dsd_app_command* c, double* out) {
    if (c->n < (int)sizeof(double)) {
        return 0;
    }
    DSD_MEMCPY(out, c->data, sizeof *out);
    return 1;
}

static int
ui_handle_gain_delta(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    int32_t d = 0;
    if (c->n >= (int)sizeof(int32_t)) {
        DSD_MEMCPY(&d, c->data, sizeof(int32_t));
    }
    if (d != 0) {
        store_digital_gain(opts, dsd_gain_setting_step(opts->audio_gain, d, DSD_DIGITAL_GAIN_MIN, DSD_DIGITAL_GAIN_MAX,
                                                       DSD_DIGITAL_GAIN_STEP_FROM_AUTO));
    }
    return 1;
}

static int
ui_handle_again_delta(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    int32_t d = 0;
    if (c->n >= (int)sizeof(int32_t)) {
        DSD_MEMCPY(&d, c->data, sizeof(int32_t));
    }
    opts->audio_gainA = (float)dsd_gain_setting_step(opts->audio_gainA, d, DSD_ANALOG_GAIN_MIN, DSD_ANALOG_GAIN_MAX,
                                                     DSD_ANALOG_GAIN_STEP_FROM_AUTO);
    return 1;
}

/* GAIN_SET and AGAIN_SET carry a double, checked here before it narrows to the float setting (this file keeps IEEE
   semantics, src/app_control/CMakeLists.txt): a value the gain's rule refuses leaves the setting as it was, never
   clamped; an analog value under 1 is auto. Every sender checks it first and says why; this is the decoder's own
   guard. */
static int
ui_handle_gain_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    double g = 0.0;
    if (read_double_payload(c, &g) && dsd_digital_gain_setting(g, &g)) {
        store_digital_gain(opts, g);
    }
    return 1;
}

static int
ui_handle_again_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    double g = 0.0;
    if (read_double_payload(c, &g) && dsd_analog_gain_setting(g, &g)) {
        opts->audio_gainA = (float)g;
    }
    return 1;
}

static int
ui_handle_input_warn_db_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    double v = 0.0;
    if (c->n >= (int)sizeof(double)) {
        DSD_MEMCPY(&v, c->data, sizeof v);
    }
    if (v < -200.0) {
        v = -200.0;
    }
    if (v > 0.0) {
        v = 0.0;
    }
    opts->input_warn_db = v;
    return 1;
}

/* Turning the -8 source monitor on opens the sink it writes (the local analog stream, or UDP port + 2): a session
   started without it opened none. One that cannot open leaves the monitor off and says so. */
static int
ui_handle_input_monitor_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->monitor_input_audio) {
        opts->monitor_input_audio = 0;
        return 1;
    }
    opts->monitor_input_audio = 1;
    if (dsd_audio_ensure_analog_output(opts) != 0) {
        opts->monitor_input_audio = 0;
        if (state) {
            DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "%s", "Input Monitor: output open failed");
            dsd_state_toast_publish(state, dsd_realtime_time() + 3);
        }
    }
    return 1;
}

static int
ui_handle_cosine_filter_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    opts->use_cosine_filter = opts->use_cosine_filter ? 0 : 1;
    return 1;
}

static int
ui_handle_input_vol_cycle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        if (opts->rtl_volume_multiplier == 1 || opts->rtl_volume_multiplier == 2) {
            opts->rtl_volume_multiplier++;
        } else {
            opts->rtl_volume_multiplier = 1;
        }
        if (state) {
            DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "RTL Monitor Gain: %dX", opts->rtl_volume_multiplier);
            dsd_state_toast_publish(state, dsd_realtime_time() + 2);
        }
    } else {
        if (opts->input_volume_multiplier == 1 || opts->input_volume_multiplier == 2) {
            opts->input_volume_multiplier++;
        } else {
            opts->input_volume_multiplier = 1;
        }
        if (state) {
            DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "Input Volume: %dX", opts->input_volume_multiplier);
            dsd_state_toast_publish(state, dsd_realtime_time() + 2);
        }
    }
    return 1;
}

static int
ui_handle_input_vol_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    if (opts && c->n >= (int)sizeof(int32_t)) {
        int32_t v = 1;
        DSD_MEMCPY(&v, c->data, sizeof v);
        if (v < 1) {
            v = 1;
        }
        if (v > 16) {
            v = 16;
        }
        opts->input_volume_multiplier = v;
    }
    return 1;
}

// Public registry
const struct dsd_app_command_reg dsd_app_actions_audio[] = {
    {DSD_APP_CMD_TOGGLE_MUTE, ui_handle_toggle_mute},
    {DSD_APP_CMD_GAIN_DELTA, ui_handle_gain_delta},
    {DSD_APP_CMD_AGAIN_DELTA, ui_handle_again_delta},
    {DSD_APP_CMD_GAIN_SET, ui_handle_gain_set},
    {DSD_APP_CMD_AGAIN_SET, ui_handle_again_set},
    {DSD_APP_CMD_INPUT_WARN_DB_SET, ui_handle_input_warn_db_set},
    {DSD_APP_CMD_INPUT_MONITOR_TOGGLE, ui_handle_input_monitor_toggle},
    {DSD_APP_CMD_COSINE_FILTER_TOGGLE, ui_handle_cosine_filter_toggle},
    {DSD_APP_CMD_INPUT_VOL_CYCLE, ui_handle_input_vol_cycle},
    {DSD_APP_CMD_INPUT_VOL_SET, ui_handle_input_vol_set},
    {0, NULL},
};
