// SPDX-License-Identifier: GPL-3.0-or-later
/* Private-source inclusion exposes the handler's completion status; stream and
 * output wrappers exercise config apply without hardware or an audio server. */
#include <assert.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/runtime/cli.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/app_control/app_command_queue.c" // NOLINT(bugprone-suspicious-include)
#include "command_dispatch.h"
#include "dsd-neo/app_control/commands.h"
#include "dsd-neo/core/airspy_config.h"
#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/power.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_c.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "dsd-neo/runtime/analog_channel.h"
#include "dsd-neo/runtime/config.h"
#include "dsd-neo/runtime/decode_mode.h"

static int test_context;
static int test_creates;
static int test_fail_create;
static int test_fail_controls;
static int test_retunes;
static int test_tune_result;
static int test_outputs;
static int test_squelches;
static float test_sql;
static uint32_t test_freq;
static uint32_t test_open_freq;
static int test_open_bw;
static int test_open_volume;
static uint32_t test_open_rate;
/* Issue #578: the analog profile the last create opened with, and the DSP rate a start refuses a width at (0: none).
   A start refuses a held width that rate cannot filter, as the stream's analog channel check does at the rate the
   device delivered, and records the refusal (rtl_stream_start_analog_refusal()); every create forgets it. */
static int test_open_analog;
static int test_open_kind;
static int test_open_width;
static int test_refuse_rate;
static int test_refused;

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** ctx);
int __wrap_rtl_stream_start(RtlSdrContext* ctx);
int __wrap_rtl_stream_start_analog_refusal(int* out_kind, int* out_width_hz, int* out_rate_hz);
int __wrap_rtl_stream_stop(RtlSdrContext* ctx);
int __wrap_rtl_stream_destroy(RtlSdrContext* ctx);
uint32_t __wrap_rtl_stream_output_rate(const RtlSdrContext* ctx);
int __wrap_rtl_stream_airspy_controls(const dsd_airspy_config* config);
int __wrap_rtl_stream_airspy_info(dsd_airspy_info* info);
void __wrap_rtl_stream_set_channel_squelch(float level);
int __wrap_io_control_set_freq(dsd_opts* opts, const dsd_state* state, long int hz);
int __wrap_dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts);

int
__wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** ctx) {
    test_creates++;
    test_open_freq = opts->rtlsdr_center_freq;
    test_open_bw = opts->rtl_dsp_bw_khz;
    test_open_volume = opts->rtl_volume_multiplier;
    test_open_rate = opts->airspy.sample_rate;
    test_open_analog = dsd_opts_is_analog_family(opts);
    test_open_kind = opts->analog_demod;
    test_open_width = dsd_opts_analog_width_hz(opts);
    test_refused = 0;
    if (test_fail_create) {
        test_fail_create--;
        *ctx = NULL;
        return -1;
    }
    *ctx = (RtlSdrContext*)&test_context;
    return 0;
}

int
__wrap_rtl_stream_start(RtlSdrContext* ctx) {
    (void)ctx;
    const int held = (test_open_kind == DSD_ANALOG_DEMOD_AM && test_open_width == 0)
                         ? dsd_analog_width_default_hz(DSD_ANALOG_DEMOD_AM)
                         : test_open_width;
    if (test_refuse_rate > 0 && test_open_analog && held > 0 && !dsd_analog_width_realizable(held, test_refuse_rate)) {
        test_refused = 1;
        return -1;
    }
    return 0;
}

int
__wrap_rtl_stream_start_analog_refusal(int* out_kind, int* out_width_hz, int* out_rate_hz) {
    if (!test_refused) {
        return 0;
    }
    if (out_kind) {
        *out_kind = test_open_kind;
    }
    if (out_width_hz) {
        *out_width_hz = test_open_width;
    }
    if (out_rate_hz) {
        *out_rate_hz = test_refuse_rate;
    }
    return 1;
}

int
__wrap_rtl_stream_stop(RtlSdrContext* ctx) {
    (void)ctx;
    return 0;
}

int
__wrap_rtl_stream_destroy(RtlSdrContext* ctx) {
    (void)ctx;
    return 0;
}

uint32_t
__wrap_rtl_stream_output_rate(const RtlSdrContext* ctx) {
    (void)ctx;
    return 48000;
}

int
__wrap_rtl_stream_airspy_controls(const dsd_airspy_config* config) {
    (void)config;
    return test_fail_controls ? -1 : 0;
}

int
__wrap_rtl_stream_airspy_info(dsd_airspy_info* info) {
    (void)info;
    return 0;
}

void
__wrap_rtl_stream_set_channel_squelch(float level) {
    test_squelches++;
    test_sql = level;
}

int
__wrap_io_control_set_freq(dsd_opts* opts, const dsd_state* state, long int hz) {
    (void)state;
    test_retunes++;
    test_freq = (uint32_t)hz;
    if (test_tune_result == 0) {
        opts->rtlsdr_center_freq = (uint32_t)hz;
    }
    return test_tune_result;
}

int
__wrap_dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) {
    test_outputs++;
    assert(strcmp(opts->audio_out_dev, "null") == 0);
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

static void
test_config_apply(int failure) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    struct dsd_app_command* cmd = calloc(1, sizeof(*cmd));
    dsdneoUserConfig* cfg = calloc(1, sizeof(*cfg));
    assert(opts && state && cmd && cfg);
    initOpts(opts);
    initState(state);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 851000000;
    opts->rtl_dsp_bw_khz = 12;
    opts->rtl_volume_multiplier = 2;
    opts->rtl_squelch_level = 0.0;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "airspy");
    state->rtl_ctx = (RtlSdrContext*)&test_context;
    cfg->has_input = 1;
    cfg->input_source = DSDCFG_INPUT_AIRSPY;
    cfg->airspy = opts->airspy;
    cfg->rtl_bw_khz = 12;
    cfg->rtl_volume = 2;
    cfg->has_output = 1;
    cfg->output_backend = DSDCFG_OUTPUT_NULL;
    cmd->id = DSD_APP_CMD_CONFIG_APPLY;
    cmd->n = sizeof(*cfg);
    if (failure) {
        cfg->airspy.sample_rate = 2500000;
        cfg->rtl_bw_khz = 24;
        cfg->rtl_volume = 3;
        DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "852M");
        test_fail_create = 1;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_cmd(opts, state, cmd) == UI_CMD_APPLY_FAILED);
        assert(test_outputs == 1);
        assert(test_creates == 2); /* failed candidate, then rollback */
        assert(opts->airspy.sample_rate == 0 && opts->rtl_dsp_bw_khz == 12);
        assert(opts->rtlsdr_center_freq == 851000000 && opts->rtl_volume_multiplier == 2);
        assert(test_open_freq == 851000000 && test_open_bw == 12 && test_open_volume == 2);
        test_fail_controls = 1;
        cfg->airspy = opts->airspy;
        cfg->rtl_bw_khz = 12;
        cfg->rtl_volume = 2;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_cmd(opts, state, cmd) == UI_CMD_APPLY_FAILED);
        assert(test_outputs == 2);
        assert(test_retunes == 0 && opts->rtlsdr_center_freq == 851000000);
    } else {
        DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "852M");
        cfg->rtl_sql = -55;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_retunes == 1 && test_freq == 852000000 && test_creates == 0);
        assert(test_squelches == 1 && fabsf(test_sql - (float)dsd_squelch_level_from_sql(-55)) < 1e-8f);
        cfg->airspy.sample_rate = 2500000;
        DSD_SNPRINTF(cfg->airspy.serial, sizeof cfg->airspy.serial, "0123456789abcdef");
        cfg->rtl_bw_khz = 24;
        cfg->rtl_volume = 3;
        DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "853M");
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_creates == 1 && test_retunes == 1);
        assert(test_open_freq == 853000000 && test_open_bw == 24 && test_open_volume == 3);
        assert(test_open_rate == 2500000);
        cfg->rtl_volume = 1;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_creates == 2 && test_open_volume == 1);
        cfg->rtl_sql = 0;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_creates == 2 && test_retunes == 1 && test_sql == 0.0f);
        const int tune_results[] = {RTL_STREAM_TUNE_TIMEOUT, RTL_STREAM_TUNE_DEFERRED, RTL_STREAM_TUNE_FAILED};
        for (size_t i = 0; i < sizeof tune_results / sizeof tune_results[0]; ++i) {
            uint32_t old_frequency = opts->rtlsdr_center_freq;
            DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "%u", old_frequency + 1000000);
            test_tune_result = tune_results[i];
            DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
            int status = apply_cmd(opts, state, cmd);
            if (test_tune_result != RTL_STREAM_TUNE_TIMEOUT) {
                /* DEFERRED was never queued, so it must not be reported as applied. */
                assert(status == UI_CMD_APPLY_FAILED && opts->rtlsdr_center_freq == old_frequency);
            } else {
                assert(status == UI_CMD_APPLY_COMPLETED && opts->rtlsdr_center_freq == old_frequency + 1000000);
            }
            assert(test_creates == 2);
        }
    }
    state->rtl_ctx = NULL;
    freeState(state);
    free(cfg);
    free(cmd);
    free(state);
    free(opts);
}

/*
 * Issue #578: a live Airspy at a 12 kHz DSP bandwidth delivers 19,531 Hz, which filters at most 16.377 kHz. A config
 * that reopens it for a new sample rate is held only to the rules every rate shares before it commits, so the reopened
 * stream's start refuses a 25 kHz width there. The apply then puts back what the start ran on (the Airspy settings, the
 * tuning, the decode mode and the widths), restarts the Airspy it had, says why and fails: the session keeps a stream,
 * instead of none and the refused width.
 */
static void
test_config_refused_width(void) {
    for (int onto_analog = 0; onto_analog < 2; ++onto_analog) {
        dsd_opts* opts = calloc(1, sizeof(*opts));
        dsd_state* state = calloc(1, sizeof(*state));
        struct dsd_app_command* cmd = calloc(1, sizeof(*cmd));
        dsdneoUserConfig* cfg = calloc(1, sizeof(*cfg));
        assert(opts && state && cmd && cfg);
        initOpts(opts);
        initState(state);
        /* Case A: an -fA session on NFM 12.5 kHz, and a config with a 25 kHz width. Case B: a DMR session with a
           stored 25 kHz, and a config whose [mode] moves it onto the analog monitor. */
        const dsdneoUserDecodeMode mode = onto_analog ? DSDCFG_MODE_DMR : DSDCFG_MODE_ANALOG;
        assert(dsd_apply_decode_mode_preset(mode, DSD_DECODE_PRESET_PROFILE_CONFIG, opts, state) == 0);
        opts->analog_nfm_bandwidth_hz = onto_analog ? 25000 : 12500;
        opts->audio_out_type = 9;
        opts->audio_in_type = AUDIO_IN_RTL;
        opts->rtlsdr_center_freq = 851000000;
        opts->rtl_dsp_bw_khz = 12;
        opts->rtl_volume_multiplier = 2;
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "airspy");
        state->rtl_ctx = (RtlSdrContext*)&test_context;
        cfg->has_input = 1;
        cfg->input_source = DSDCFG_INPUT_AIRSPY;
        cfg->airspy = opts->airspy;
        cfg->airspy.sample_rate = 2500000;
        cfg->rtl_bw_khz = 12;
        cfg->rtl_volume = 2;
        cfg->has_output = 1;
        cfg->output_backend = DSDCFG_OUTPUT_NULL;
        if (onto_analog) {
            cfg->has_mode = 1;
            cfg->decode_mode = DSDCFG_MODE_ANALOG;
        } else {
            cfg->has_analog = 1;
            cfg->analog_nfm_bandwidth_hz = 25000;
        }
        cmd->id = DSD_APP_CMD_CONFIG_APPLY;
        cmd->n = sizeof(*cfg);
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        test_creates = 0;
        test_refuse_rate = 19531;
        assert(apply_cmd(opts, state, cmd) == UI_CMD_APPLY_FAILED);
        assert(test_creates == 2); /* the refused reopen, then the Airspy it had */
        assert(state->rtl_ctx == (RtlSdrContext*)&test_context && opts->rtl_started == 1);
        assert(strcmp(opts->audio_in_dev, "airspy") == 0 && opts->airspy.sample_rate == 0);
        assert(test_open_rate == 0 && test_open_bw == 12 && test_open_freq == 851000000);
        assert(strncmp(state->ui_msg, "Config not applied: NFM 25 kHz does not fit the 19.531 kHz DSP rate",
                       strlen("Config not applied: NFM 25 kHz does not fit the 19.531 kHz DSP rate"))
               == 0);
        if (onto_analog) {
            assert(test_open_analog == 0);
            assert(dsd_infer_decode_mode_preset(opts) == DSDCFG_MODE_DMR && opts->analog_only == 0);
            assert(opts->analog_nfm_bandwidth_hz == 25000);
        } else {
            assert(test_open_analog == 1 && test_open_width == 12500);
            assert(opts->analog_nfm_bandwidth_hz == 12500);
        }
        test_refuse_rate = 0;
        state->rtl_ctx = NULL;
        freeState(state);
        free(cfg);
        free(cmd);
        free(state);
        free(opts);
    }
}

static void
test_serial_cli_override(void) {
    for (int serial = 0; serial < 2; ++serial) {
        dsd_opts* opts = calloc(1, sizeof(*opts));
        dsd_state* state = calloc(1, sizeof(*state));
        assert(opts && state);
        initOpts(opts);
        initState(state);
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "airspy");
        opts->airspy_config_error = 1;
        char program[] = "dsd-neo";
        char serial_arg[] = "--airspy-serial=0123456789abcdef";
        char gain_arg[] = "--airspy-vga-gain=5";
        char* argv[] = {program, serial ? serial_arg : gain_arg, NULL};
        int effective = 0, exit_rc = 0;
        int rc = dsd_parse_args(2, argv, opts, state, &effective, &exit_rc);
        assert(serial ? rc == DSD_PARSE_CONTINUE : rc == DSD_PARSE_ERROR);
        assert(opts->airspy_config_error == !serial);
        freeState(state);
        free(state);
        free(opts);
    }
}

int
main(int argc, char** argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "M2") == 0) {
        test_serial_cli_override();
    } else if (strcmp(argv[1], "M11") == 0) {
        test_config_refused_width();
    } else {
        test_config_apply(strcmp(argv[1], "M7") == 0);
    }
    return 0;
}
