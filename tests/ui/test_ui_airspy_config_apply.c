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
#include "dsd-neo/runtime/config.h"

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

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** ctx);
int __wrap_rtl_stream_start(RtlSdrContext* ctx);
int __wrap_rtl_stream_stop(RtlSdrContext* ctx);
int __wrap_rtl_stream_destroy(RtlSdrContext* ctx);
uint32_t __wrap_rtl_stream_output_rate(const RtlSdrContext* ctx);
int __wrap_rtl_stream_airspy_controls(const dsd_airspy_config* config);
int __wrap_rtl_stream_airspy_info(dsd_airspy_info* info);
void __wrap_rtl_stream_set_channel_squelch(float level);
int __wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int hz);
int __wrap_dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts);

int
__wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** ctx) {
    test_creates++;
    test_open_freq = opts->rtlsdr_center_freq;
    test_open_bw = opts->rtl_dsp_bw_khz;
    test_open_volume = opts->rtl_volume_multiplier;
    test_open_rate = opts->airspy.sample_rate;
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
    return 0;
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
__wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int hz) {
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
        assert(ui_cmd_handle_config_apply(opts, state, cmd) == UI_CMD_APPLY_FAILED);
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
        assert(ui_cmd_handle_config_apply(opts, state, cmd) == UI_CMD_APPLY_FAILED);
        assert(test_outputs == 2);
        assert(test_retunes == 0 && opts->rtlsdr_center_freq == 851000000);
    } else {
        DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "852M");
        cfg->rtl_sql = -55;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(ui_cmd_handle_config_apply(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_retunes == 1 && test_freq == 852000000 && test_creates == 0);
        assert(test_squelches == 1 && fabsf(test_sql - (float)dsd_squelch_level_from_sql(-55)) < 1e-8f);
        cfg->airspy.sample_rate = 2500000;
        DSD_SNPRINTF(cfg->airspy.serial, sizeof cfg->airspy.serial, "0123456789abcdef");
        cfg->rtl_bw_khz = 24;
        cfg->rtl_volume = 3;
        DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "853M");
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(ui_cmd_handle_config_apply(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_creates == 1 && test_retunes == 1);
        assert(test_open_freq == 853000000 && test_open_bw == 24 && test_open_volume == 3);
        assert(test_open_rate == 2500000);
        cfg->rtl_volume = 1;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(ui_cmd_handle_config_apply(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_creates == 2 && test_open_volume == 1);
        cfg->rtl_sql = 0;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(ui_cmd_handle_config_apply(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_creates == 2 && test_retunes == 1 && test_sql == 0.0f);
        const int tune_results[] = {RTL_STREAM_TUNE_TIMEOUT, RTL_STREAM_TUNE_DEFERRED, RTL_STREAM_TUNE_FAILED};
        for (size_t i = 0; i < sizeof tune_results / sizeof tune_results[0]; ++i) {
            uint32_t old_frequency = opts->rtlsdr_center_freq;
            DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "%u", old_frequency + 1000000);
            test_tune_result = tune_results[i];
            DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
            int status = ui_cmd_handle_config_apply(opts, state, cmd);
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
    } else {
        test_config_apply(strcmp(argv[1], "M7") == 0);
    }
    return 0;
}
