// SPDX-License-Identifier: GPL-3.0-or-later
/* Private-source inclusion exposes the handler's completion status; stream and
 * output wrappers exercise config apply without hardware or an audio server. */
#include <assert.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/runtime/cli.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/app_control/app_command_queue.c" // NOLINT(bugprone-suspicious-include)
#include "channel_gate_fake.h"
#include "command_dispatch.h"
#include "dsd-neo/app_control/commands.h"
#include "dsd-neo/core/airspy_config.h"
#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/power.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/engine/channel_scan.h"
#include "dsd-neo/engine/manual_tune.h"
#include "dsd-neo/io/rtl_stream_c.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "dsd-neo/runtime/analog_channel.h"
#include "dsd-neo/runtime/config.h"
#include "dsd-neo/runtime/decode_mode.h"
#include "dsd-neo/runtime/rtl_stream_metrics_hooks.h"
#include "dsd-neo/runtime/squelch.h"
#include "manual_tune_pending.h"
#include "services.h"

/* The drain's own dispatch of a config apply; what a scan list toggle would retain does not apply to one. */
static int
apply_config_cmd(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* cmd) {
    int32_t scan_list_code = 0;
    return apply_cmd(opts, state, cmd, &scan_list_code);
}

static int test_context;
static int test_creates;
static int test_fail_create;
static int test_fail_controls;
static int test_retunes;
static int test_tune_result;
static int test_outputs;
static int test_squelches;
static float test_sql;
/* Issue #899: the retunes marked a new channel and the target the last was queued for, the whole settings the receiver
   was handed (counted), and when each happened against the retunes (test_events). */
static int test_events;
static int test_gates;
static uint32_t test_gate_hz;
static int test_gate_event;
static int test_retune_event;
static int test_squelch_event;
static int test_settings;
/* The receiver's gate (issue #899): the shared stand-in (channel_gate_fake.h) is the stream here. The gates it comes to
   run (test_rx_runs, the last test_rx_gate, at test_rx_event; test_rx_closed for a landing it started closed), whether
   the last retune is still marked a new channel on its profile (test_gate_named), and whether the receiver took that
   retune (landed or in flight: test_gate_taken). The squelch setter wraps count only gates handed over outside the
   closed-start rules, which nothing does any more. */
static dsd_rtl_stream_metrics_hooks test_rx_hooks;
static int test_rx_runs;
static int test_rx_closed;
static dsd_squelch_setting test_rx_gate;
static int test_rx_event;
static int test_gate_named;
static int test_gate_taken;
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
void __wrap_rtl_stream_set_channel_squelch_setting(const dsd_squelch_setting* setting);
void __wrap_rtl_stream_prepare_retune_new_channel_for_target(uint32_t target_freq_hz, int new_channel);
void __wrap_rtl_stream_write_squelch_gate(const dsd_squelch_setting* gate, const dsd_squelch_setting* digital);
int __wrap_rtl_stream_gate_closed_start(void);
int __wrap_io_control_set_freq_tagged(dsd_opts* opts, const dsd_state* state, uint32_t hz, uint64_t* out_pending_id,
                                      dsd_input_cut* rigctl_mark);
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
    test_squelch_event = ++test_events;
}

void
__wrap_rtl_stream_set_channel_squelch_setting(const dsd_squelch_setting* setting) {
    (void)setting;
    test_settings++;
    test_squelch_event = ++test_events;
}

/* A retune is marked a new channel; once it returned without the receiver taking it, its profile is marked no more. */
void
__wrap_rtl_stream_prepare_retune_new_channel_for_target(uint32_t target_freq_hz, int new_channel) {
    if (!new_channel) {
        if (target_freq_hz == test_gate_hz && !test_gate_taken) {
            test_gate_named = 0;
        }
        return;
    }
    test_gates++;
    test_gate_hz = target_freq_hz;
    test_gate_event = ++test_events;
    test_gate_named = 1;
    test_gate_taken = 0;
}

void
__wrap_rtl_stream_write_squelch_gate(const dsd_squelch_setting* gate, const dsd_squelch_setting* digital) {
    test_rx_hooks.write_squelch_gate(gate, digital);
}

int
__wrap_rtl_stream_gate_closed_start(void) {
    return test_rx_hooks.gate_closed_start();
}

int
__wrap_io_control_set_freq_tagged(dsd_opts* opts, const dsd_state* state, uint32_t hz, uint64_t* out_pending_id,
                                  dsd_input_cut* rigctl_mark) {
    if (rigctl_mark) {
        dsd_input_backlog_mark(rigctl_mark); /* as the real rigctl leg marks, immediately before the frequency */
    }
    (void)state;
    if (out_pending_id) {
        /* A timed-out tune is followed to its completion under this id (dsd_engine_manual_tune_poll()). */
        *out_pending_id = test_tune_result == RTL_STREAM_TUNE_TIMEOUT ? 899U : 0U;
    }
    test_retunes++;
    test_freq = hz;
    test_retune_event = ++test_events;
    if (test_tune_result == 0) {
        opts->rtlsdr_center_freq = hz;
    }
    /* A receiver retune lands its new channel (none under rigctl), on the family the session runs: with a tune that
       completed, or later with one accepted and pending (RTL_STREAM_TUNE_TIMEOUT). */
    if (hz == test_gate_hz && test_gate_named) {
        if (test_tune_result == 0) {
            test_gate_taken = 1;
            channel_gate_fake_land(1, opts->analog_only == 1);
        } else if (test_tune_result == RTL_STREAM_TUNE_TIMEOUT) {
            test_gate_taken = 1;
        }
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
test_rx_run(const dsd_squelch_setting* gate) {
    if (channel_gate_fake_closed()) {
        test_rx_closed++;
        return;
    }
    test_rx_runs++;
    test_rx_gate = *gate;
    test_rx_event = ++test_events;
}

/* The running Airspy's receiver, opened on the family @p opts runs and written by the decoder with the squelch @p opts
   holds, its gate installed (the engine reaches it through the stream hooks), nothing counted yet. */
static void
test_rx_open(const dsd_opts* opts) {
    channel_gate_fake_install(&test_rx_hooks);
    static dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    channel_gate_fake_install(&hooks);
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    const dsd_squelch_setting opened = dsd_squelch_setting_of_opts(opts);
    const dsd_squelch_setting digital = dsd_squelch_setting_configured(opts);
    channel_gate_fake_open(dsd_opts_is_analog_family(opts), &digital, test_rx_run);
    test_rx_hooks.write_squelch_gate(&opened, &digital);
    test_rx_runs = 0;
    test_rx_closed = 0;
    test_rx_event = 0;
    test_gate_named = 0;
    test_gate_taken = 0;
}

/* The receiver runs @p want now. */
static int
test_rx_runs_gate(const dsd_squelch_setting* want) {
    const dsd_squelch_setting now = channel_gate_fake_gate();
    return dsd_squelch_setting_equal(&now, want);
}

/* Issue #575: the codes the carrier on air decoded, which an apply that moves the Airspy to another centre forgets. */
static void
seed_carrier_codes(dsd_state* state) {
    state->dmr_color_code = 5U;
    state->nxdn_last_ran = 9U;
    state->nxdn_last_ran_stand_in = 1U;
    state->dpmr_color_code = 33;
    state->p2_cc = 0x293U;
    state->p2_cc_verified = 1U;
}

static int
carrier_codes_kept(const dsd_state* state) {
    return state->dmr_color_code == 5U && state->nxdn_last_ran == 9U && state->nxdn_last_ran_stand_in == 1U
           && state->dpmr_color_code == 33 && state->p2_cc_verified == 1U && state->p2_cc == 0x293U;
}

static int
carrier_codes_forgotten(const dsd_state* state) {
    return state->dmr_color_code == 16U && state->nxdn_last_ran == (unsigned int)-1
           && state->nxdn_last_ran_stand_in == 0U && state->dpmr_color_code == -1 && state->p2_cc_verified == 0U
           && state->p2_cc == 0x293U;
}

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
    test_rx_open(opts);
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
        seed_carrier_codes(state);
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_FAILED);
        assert(carrier_codes_kept(state)); /* the reopen failed: the old centre runs again */
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
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_FAILED);
        assert(test_outputs == 2);
        assert(test_retunes == 0 && opts->rtlsdr_center_freq == 851000000);
        assert(carrier_codes_kept(state));
    } else {
        DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "852M");
        cfg->rtl_sql = -55;
        seed_carrier_codes(state);
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_retunes == 1 && test_freq == 852000000 && test_creates == 0);
        assert(carrier_codes_forgotten(state)); /* retuned live to another carrier */
        /* The retune is marked a new channel before it goes, and the decoder writes the config's -55 dB once it has
           landed (issue #899): nothing handed to the old channel. */
        assert(test_gates == 1 && test_gate_hz == 852000000U && test_gate_event < test_retune_event);
        assert(test_squelches == 0 && test_settings == 0);
        const dsd_squelch_setting minus55 = dsd_squelch_setting_of_level(dsd_squelch_level_from_sql(-55));
        assert(test_rx_runs == 1 && test_rx_event > test_retune_event && test_rx_runs_gate(&minus55));
        cfg->airspy.sample_rate = 2500000;
        DSD_SNPRINTF(cfg->airspy.serial, sizeof cfg->airspy.serial, "0123456789abcdef");
        cfg->rtl_bw_khz = 24;
        cfg->rtl_volume = 3;
        DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "853M");
        seed_carrier_codes(state);
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_creates == 1 && test_retunes == 1);
        assert(carrier_codes_forgotten(state)); /* reopened on another carrier */
        assert(test_open_freq == 853000000 && test_open_bw == 24 && test_open_volume == 3);
        assert(test_open_rate == 2500000);
        cfg->rtl_volume = 1;
        seed_carrier_codes(state);
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_creates == 2 && test_open_volume == 1);
        assert(carrier_codes_kept(state)); /* reopened on the same carrier */
        cfg->rtl_sql = 0;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        assert(test_creates == 2 && test_retunes == 1 && test_sql == 0.0f);
        assert(carrier_codes_kept(state));
        const int tune_results[] = {RTL_STREAM_TUNE_TIMEOUT, RTL_STREAM_TUNE_DEFERRED, RTL_STREAM_TUNE_FAILED};
        for (size_t i = 0; i < sizeof tune_results / sizeof tune_results[0]; ++i) {
            uint32_t old_frequency = opts->rtlsdr_center_freq;
            DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "%u", old_frequency + 1000000);
            test_tune_result = tune_results[i];
            seed_carrier_codes(state);
            DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
            int status = apply_config_cmd(opts, state, cmd);
            if (test_tune_result != RTL_STREAM_TUNE_TIMEOUT) {
                /* DEFERRED was never queued, so it must not be reported as applied. */
                assert(status == UI_CMD_APPLY_FAILED && opts->rtlsdr_center_freq == old_frequency);
                assert(carrier_codes_kept(state));
            } else {
                assert(status == UI_CMD_APPLY_COMPLETED && opts->rtlsdr_center_freq == old_frequency + 1000000);
                assert(carrier_codes_forgotten(state));
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
 * Issue #578, #806: a live Airspy runs at its DSP bandwidth (the live rate step lands its capture rate on it), so a
 * config that reopens it for a new sample rate at a 12 kHz bandwidth is held to 12 kHz before it commits, as an
 * RTL-SDR's reopen is: a 25 kHz width is refused up front, with the bandwidths that fit, and nothing reopens or changes
 * (before #806 the Airspy ran 19,531 Hz there, only the rules every rate shares were asked, and the reopened start
 * refused it). A start that refuses a width the bandwidth filters anyway (the fake device here delivers 24 kHz for a
 * 48 kHz request) still puts back what the start ran on (the Airspy settings, the tuning, the decode mode and the
 * widths), restarts the Airspy it had, says why and fails: the session keeps a stream, instead of none and the refused
 * width.
 */
static void
test_config_refused_width(void) {
    for (int case_index = 0; case_index < 4; ++case_index) {
        const int onto_analog = case_index & 1;
        const int up_front = case_index < 2;
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
        opts->rtl_dsp_bw_khz = up_front ? 12 : 24;
        opts->rtl_volume_multiplier = 2;
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "airspy");
        state->rtl_ctx = (RtlSdrContext*)&test_context;
        cfg->has_input = 1;
        cfg->input_source = DSDCFG_INPUT_AIRSPY;
        cfg->airspy = opts->airspy;
        cfg->airspy.sample_rate = 2500000;
        cfg->rtl_bw_khz = up_front ? 12 : 48;
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
        test_open_bw = -1;
        test_refuse_rate = up_front ? 0 : 24000;
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_FAILED);
        assert(state->rtl_ctx == (RtlSdrContext*)&test_context);
        assert(strcmp(opts->audio_in_dev, "airspy") == 0 && opts->airspy.sample_rate == 0);
        if (up_front) {
            assert(test_creates == 0); /* refused before anything reopened */
            assert(opts->rtl_dsp_bw_khz == 12);
            assert(strcmp(state->ui_msg,
                          "Config not applied: NFM 25 kHz does not fit the 12 kHz DSP rate (max 9.6 kHz); "
                          "use a 48 or 72 kHz DSP bandwidth")
                   == 0);
        } else {
            assert(test_creates == 2); /* the refused reopen, then the Airspy it had */
            assert(opts->rtl_started == 1);
            assert(test_open_rate == 0 && test_open_bw == 24 && test_open_freq == 851000000);
            assert(strncmp(state->ui_msg, "Config not applied: NFM 25 kHz does not fit the 24 kHz DSP rate",
                           strlen("Config not applied: NFM 25 kHz does not fit the 24 kHz DSP rate"))
                   == 0);
        }
        if (onto_analog) {
            assert(up_front || test_open_analog == 0);
            assert(dsd_infer_decode_mode_preset(opts) == DSDCFG_MODE_DMR && opts->analog_only == 0);
            assert(opts->analog_nfm_bandwidth_hz == 25000);
        } else {
            assert(up_front || (test_open_analog == 1 && test_open_width == 12500));
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

/*
 * Issue #806 BUG-1: a config with rtl_bw_khz = 72 applied over a running Airspy at 48 kHz reopens it at 72 kHz, as
 * RTL_SET_BW 72 does now. Both run the 72 kHz the live rate step lands the Airspy's capture rate on. Before, the config
 * reopened it at 72 kHz while RTL_SET_BW refused 72, and the stream ran the 48 kHz chain under a 72 kHz label.
 */
static void
test_config_apply_72(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    struct dsd_app_command* cmd = calloc(1, sizeof(*cmd));
    dsdneoUserConfig* cfg = calloc(1, sizeof(*cfg));
    assert(opts && state && cmd && cfg);
    initOpts(opts);
    initState(state);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 851000000;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_volume_multiplier = 2;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "airspy");
    state->rtl_ctx = (RtlSdrContext*)&test_context;
    cfg->has_input = 1;
    cfg->input_source = DSDCFG_INPUT_AIRSPY;
    cfg->airspy = opts->airspy;
    cfg->rtl_bw_khz = 72;
    cfg->rtl_volume = 2;
    cfg->has_output = 1;
    cfg->output_backend = DSDCFG_OUTPUT_NULL;
    cmd->id = DSD_APP_CMD_CONFIG_APPLY;
    cmd->n = sizeof(*cfg);
    DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
    test_creates = 0;
    test_open_bw = -1;
    assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
    assert(test_creates == 1 && test_open_bw == 72);
    assert(opts->rtl_dsp_bw_khz == 72 && strcmp(opts->audio_in_dev, "airspy") == 0);
    assert(strncmp(state->ui_msg, "Config not applied", strlen("Config not applied")) != 0);
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

/*
 * Issue #899: a config apply that fails on a live Airspy puts the analog default back where it stood. An NFM session
 * runs --squelch-analog-default noise+10 over its own Off. A config with no squelch keys whose Airspy controls fail
 * (in place) or whose reopen fails (rolled back to the Airspy that ran) leaves the default in force over that Off, as
 * before the apply: before the fix the in-place failure put noise back in the squelch fields with the default marked
 * out, so the next sync took noise as the session's own and a digital decode kept it. A config whose squelch keys
 * ended the default keeps it ended after either failure, over the Off the session had beneath it, never the default's
 * noise left as a squelch someone set, and an Airspy that keeps running after the in-place failure runs that Off.
 */
static void
test_config_failure_keeps_the_analog_default(void) {
    for (int case_index = 0; case_index < 4; ++case_index) {
        const int keys = case_index & 1;
        const int reopen = (case_index >> 1) & 1;
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
        opts->analog_only = 1;
        opts->analog_demod = DSD_ANALOG_DEMOD_FM;
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "airspy");
        state->rtl_ctx = (RtlSdrContext*)&test_context;
        assert(dsd_squelch_analog_default_parse(opts, "noise+10", NULL, 0U) == 0);
        assert(dsd_squelch_analog_default_sync(opts, state) == 1);
        test_rx_open(opts);
        assert(opts->squelch_default_state == DSD_SQUELCH_DEFAULT_APPLIED);
        cfg->has_input = 1;
        cfg->input_source = DSDCFG_INPUT_AIRSPY;
        cfg->airspy = opts->airspy;
        cfg->rtl_bw_khz = 12;
        cfg->rtl_volume = 2;
        cfg->has_output = 1;
        cfg->output_backend = DSDCFG_OUTPUT_NULL;
        if (keys) {
            cfg->rtl_sql = -60;
            cfg->rtl_sql_is_set = 1;
        }
        if (reopen) {
            cfg->airspy.sample_rate = 2500000;
            test_fail_create = 1;
        } else {
            test_fail_controls = 1;
        }
        cmd->id = DSD_APP_CMD_CONFIG_APPLY;
        cmd->n = sizeof(*cfg);
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        test_creates = 0;
        test_squelches = 0;
        test_sql = -1.0f;
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_FAILED);
        /* The Airspy that keeps running is told the Off the ended default leaves (review round 3): before the fix it
           was never told, and ran the default's noise behind an Off readout. */
        const dsd_squelch_setting rx_off = dsd_squelch_setting_of_level(0.0);
        if (keys && !reopen && (test_rx_runs < 1 || !test_rx_runs_gate(&rx_off))) {
            DSD_FPRINTF(stderr,
                        "failed in-place config with squelch keys: the receiver came to run %d gates, last "
                        "mode %d level %g; want Off\n",
                        test_rx_runs, test_rx_gate.mode, test_rx_gate.level);
            assert(0);
        }
        const int want_state = keys ? DSD_SQUELCH_DEFAULT_ENDED : DSD_SQUELCH_DEFAULT_APPLIED;
        const int want_mode = keys ? DSD_SQUELCH_MODE_LEVEL : DSD_SQUELCH_MODE_NOISE;
        if (opts->squelch_default_state != want_state || opts->rtl_squelch_mode != want_mode
            || (keys && opts->rtl_squelch_level > 0.0)
            || (!keys
                && (opts->squelch_default_base_mode != DSD_SQUELCH_MODE_LEVEL
                    || opts->squelch_default_base_level > 0.0))) {
            DSD_FPRINTF(
                stderr,
                "failed %s config %s squelch keys: default state %d, fields mode %d level %g, base mode %d level "
                "%g; want state %d, mode %d over an Off\n",
                reopen ? "reopen" : "in-place", keys ? "with" : "without", opts->squelch_default_state,
                opts->rtl_squelch_mode, opts->rtl_squelch_level, opts->squelch_default_base_mode,
                opts->squelch_default_base_level, want_state, want_mode);
            assert(0);
        }
        /* A switch to a digital decode then runs the session's own Off, never the default's noise. */
        opts->analog_only = 0;
        opts->frame_dmr = 1;
        (void)dsd_squelch_analog_default_sync(opts, state);
        assert(opts->rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL && !(opts->rtl_squelch_level > 0.0));
        test_fail_controls = 0;
        test_fail_create = 0;
        state->rtl_ctx = NULL;
        freeState(state);
        free(cfg);
        free(cmd);
        free(state);
        free(opts);
    }
}

/* A running Airspy NFM monitor on 851 MHz with --squelch-analog-default noise+10 over its own Off, its receiver told the
   default, and a config whose [input] the case fills in. */
static void
squelch_default_airspy_start(dsd_opts* opts, dsd_state* state, struct dsd_app_command* cmd) {
    initOpts(opts);
    initState(state);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 851000000;
    opts->rtl_dsp_bw_khz = 12;
    opts->rtl_volume_multiplier = 2;
    opts->rtl_squelch_level = 0.0;
    opts->analog_only = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "airspy");
    state->rtl_ctx = (RtlSdrContext*)&test_context;
    assert(dsd_squelch_analog_default_parse(opts, "noise+10", NULL, 0U) == 0);
    assert(dsd_squelch_analog_default_sync(opts, state) == 1);
    test_rx_open(opts);
    cmd->id = DSD_APP_CMD_CONFIG_APPLY;
    cmd->n = sizeof(dsdneoUserConfig);
    test_events = test_gates = test_retunes = test_squelches = test_settings = 0;
    test_gate_event = test_retune_event = test_squelch_event = 0;
    test_creates = 0;
    test_sql = -1.0f;
    test_tune_result = 0;
}

/*
 * Issue #899 (review rounds 4 and 6): a same-device Airspy config that changes the frequency and the squelch keeps the
 * stream running and retunes it. Starting on the inherited noise +10 and applying 852 MHz with rtl_sql = -1, the retune
 * names a channel of its own, queued before the retune is submitted; the receiver runs noise +10 on 851 MHz, starts the
 * 852 MHz channel closed where it lands, and opens it on the -1 dB the apply leaves, which the decoder writes for that
 * channel after the landing. Before round 4 the retune went first and the -1 dB setter followed once it returned, so
 * 852 MHz samples could queue under the old default.
 */
static void
test_live_airspy_retune_carries_the_config_gate(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    struct dsd_app_command* cmd = calloc(1, sizeof(*cmd));
    dsdneoUserConfig* cfg = calloc(1, sizeof(*cfg));
    assert(opts && state && cmd && cfg);
    squelch_default_airspy_start(opts, state, cmd);
    cfg->has_input = 1;
    cfg->input_source = DSDCFG_INPUT_AIRSPY;
    cfg->airspy = opts->airspy;
    cfg->rtl_bw_khz = 12;
    cfg->rtl_volume = 2;
    cfg->rtl_sql = -1;
    cfg->rtl_sql_is_set = 1;
    DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "852M");
    cfg->has_output = 1;
    cfg->output_backend = DSDCFG_OUTPUT_NULL;
    DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
    assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
    const double want = dsd_squelch_level_from_sql(-1.0);
    if (test_retunes != 1 || test_creates != 0 || test_gates != 1 || test_gate_hz != 852000000U
        || !(test_gate_event < test_retune_event) || test_squelches != 0 || test_settings != 0 || test_rx_closed != 1
        || test_rx_runs != 1 || !(test_rx_event > test_retune_event)
        || !(fabs(test_rx_gate.level - want) <= 1e-9 * want)) {
        DSD_FPRINTF(stderr,
                    "live Airspy retune with rtl_sql = -1: %d retunes (event %d), %d creates, %d channels named (event "
                    "%d, %u Hz), %d level and %d setting pushes (last event %d, level %g), the receiver started %d "
                    "closed and came to run %d gates (last at event %d, level %g); want one channel named for 852 MHz "
                    "before the one retune, closed where it lands, then -1 dB, and no push\n",
                    test_retunes, test_retune_event, test_creates, test_gates, test_gate_event, test_gate_hz,
                    test_squelches, test_settings, test_squelch_event, (double)test_sql, test_rx_closed, test_rx_runs,
                    test_rx_event, test_rx_gate.level);
        assert(0);
    }
    assert(opts->squelch_default_state == DSD_SQUELCH_DEFAULT_ENDED && opts->rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL
           && fabs(opts->rtl_squelch_level - want) <= 1e-9 * want && opts->rtlsdr_center_freq == 852000000U);
    state->rtl_ctx = NULL;
    freeState(state);
    free(cfg);
    free(cmd);
    free(state);
    free(opts);
}

/*
 * Issue #899 (review round 4, P2): an [input] that names no source and carries only rtl_sql (DSDCFG_INPUT_UNSET) sets the
 * running input's squelch from its keys and ends the analog default, and the Airspy that keeps running hears it at once.
 * On the inherited noise +10 over Off, rtl_sql = 0 leaves Off in force and hands the receiver Off; rtl_sql = -50 hands
 * it -50 dB. Before the fix the keys ended the default (the options read Off) but the receiver kept noise +10, and an
 * rtl_sql other than the session's Off never reached the options at all.
 */
static void
test_sourceless_squelch_keys_reach_the_receiver(void) {
    static const int sql[] = {0, -50};
    for (size_t i = 0; i < sizeof sql / sizeof sql[0]; i++) {
        dsd_opts* opts = calloc(1, sizeof(*opts));
        dsd_state* state = calloc(1, sizeof(*state));
        struct dsd_app_command* cmd = calloc(1, sizeof(*cmd));
        dsdneoUserConfig* cfg = calloc(1, sizeof(*cfg));
        assert(opts && state && cmd && cfg);
        squelch_default_airspy_start(opts, state, cmd);
        cfg->has_input = 1;
        cfg->input_source = DSDCFG_INPUT_UNSET;
        cfg->rtl_sql = sql[i];
        cfg->rtl_sql_is_set = 1;
        cfg->has_output = 1;
        cfg->output_backend = DSDCFG_OUTPUT_NULL;
        DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        const double want = dsd_squelch_level_from_sql((double)sql[i]);
        const dsd_squelch_setting want_gate = dsd_squelch_setting_of_level(sql[i] == 0 ? 0.0 : want);
        const int pushed = test_rx_runs >= 1 && test_rx_runs_gate(&want_gate);
        if (!pushed || opts->squelch_default_state != DSD_SQUELCH_DEFAULT_ENDED
            || opts->rtl_squelch_mode != DSD_SQUELCH_MODE_LEVEL
            || (sql[i] == 0 ? !dsd_squelch_is_off(opts->rtl_squelch_level)
                            : !(fabs(opts->rtl_squelch_level - want) <= 1e-9 * want))) {
            DSD_FPRINTF(
                stderr,
                "[input] rtl_sql = %d alone: the receiver came to run %d gates (last %g), options mode %d level "
                "%g default %d; want the receiver and the options on it, the default ended\n",
                sql[i], test_rx_runs, test_rx_gate.level, opts->rtl_squelch_mode, opts->rtl_squelch_level,
                opts->squelch_default_state);
            assert(0);
        }
        assert(test_creates == 0 && test_retunes == 0);
        state->rtl_ctx = NULL;
        freeState(state);
        free(cfg);
        free(cmd);
        free(state);
        free(opts);
    }
}

/* A same-device Airspy config on the running NFM monitor (noise +10) that moves it to 852 MHz with rtl_sql = -1. */
static void
live_airspy_squelch_config(dsd_opts* opts, dsd_state* state, struct dsd_app_command* cmd, dsdneoUserConfig* cfg) {
    squelch_default_airspy_start(opts, state, cmd);
    cfg->has_input = 1;
    cfg->input_source = DSDCFG_INPUT_AIRSPY;
    cfg->airspy = opts->airspy;
    cfg->rtl_bw_khz = 12;
    cfg->rtl_volume = 2;
    cfg->rtl_sql = -1;
    cfg->rtl_sql_is_set = 1;
    DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "852M");
    cfg->has_output = 1;
    cfg->output_backend = DSDCFG_OUTPUT_NULL;
    DSD_MEMCPY(cmd->data, cfg, sizeof(*cfg));
}

/*
 * Issue #899 (review rounds 5 and 6): the squelch a live Airspy config leaves reaches the receiver whatever tunes the
 * frequency. With rigctl tuning the peer (no receiver retune, so no channel named on one), the -1 dB the config leaves
 * is written for the channel the receiver is on, and it runs it; before round 5 a successful rigctl tune counted as the
 * gate's landing and the receiver kept noise +10. With the native retune accepted and still in flight
 * (RTL_STREAM_TUNE_TIMEOUT), the receiver keeps noise +10 on 851 MHz; the retune's channel starts closed where it lands
 * and the drain after it opens it on -1 dB; and when that retune fails late instead, the engine's poll of the timed-out
 * tune (dsd_engine_manual_tune_poll()) settles it, and the drain writes -1 dB for the channel the receiver stayed on.
 * Before round 5 nothing owed the push, and the receiver kept noise +10 for good. Round 7 (P2): with that timed-out tune
 * A still in flight, a second tune B the receiver refuses at once; A then lands late, closed, and the drain after its
 * outcome writes -1 dB on it: the refusal leaves nothing the decoder waits for that would keep it muted.
 */
static void
test_live_airspy_squelch_reaches_the_receiver_whatever_tunes(void) {
    const dsd_squelch_setting noise10 = dsd_squelch_setting_noise(10);
    const dsd_squelch_setting minus1 = dsd_squelch_setting_of_level(dsd_squelch_level_from_sql(-1.0));
    for (int case_index = 0; case_index < 4; case_index++) {
        dsd_opts* opts = calloc(1, sizeof(*opts));
        dsd_state* state = calloc(1, sizeof(*state));
        struct dsd_app_command* cmd = calloc(1, sizeof(*cmd));
        dsdneoUserConfig* cfg = calloc(1, sizeof(*cfg));
        assert(opts && state && cmd && cfg);
        live_airspy_squelch_config(opts, state, cmd, cfg);
        const char* what = case_index == 0   ? "rigctl tunes"
                           : case_index == 1 ? "native, lands late"
                           : case_index == 2 ? "native, fails late"
                                             : "native, lands late after a refused tune";
        if (case_index == 0) {
            opts->use_rigctl = 1;
        } else {
            test_tune_result = RTL_STREAM_TUNE_TIMEOUT;
        }
        assert(apply_config_cmd(opts, state, cmd) == UI_CMD_APPLY_COMPLETED);
        int ok = test_retunes == 1 && opts->squelch_default_state == DSD_SQUELCH_DEFAULT_ENDED;
        if (case_index == 0) {
            ok = ok && test_gates == 0 && test_rx_runs_gate(&minus1);
        } else {
            /* In flight: the receiver keeps the old channel and its gate, and the decoder writes nothing while the
               timed-out tune is followed (a drain meanwhile). */
            dsd_engine_receiver_gate_settle(opts, state, 0);
            ok = ok && test_gates == 1 && test_gate_taken && test_rx_runs_gate(&noise10) && !channel_gate_fake_closed();
            if (case_index == 3) {
                test_tune_result = RTL_STREAM_TUNE_FAILED;
                uint64_t refused_id = 0U;
                ok = ok && svc_io_control_tune(opts, state, 853000000U, &refused_id) != 0;
                test_tune_result = RTL_STREAM_TUNE_TIMEOUT;
                channel_gate_fake_land(1, 1);
                ok = ok && channel_gate_fake_closed();
                dsd_engine_manual_tune_completion(899U, RTL_STREAM_TUNE_OK);
                (void)dsd_engine_manual_tune_poll(opts, state);
            } else if (case_index == 1) {
                channel_gate_fake_land(1, 1);
                ok = ok && channel_gate_fake_closed();
                dsd_engine_manual_tune_completion(899U, RTL_STREAM_TUNE_OK);
                ok = ok && dsd_engine_manual_tune_poll(opts, state) == DSD_MANUAL_TUNE_LANDED;
            } else {
                dsd_engine_manual_tune_completion(899U, RTL_STREAM_TUNE_FAILED);
                ok = ok && dsd_engine_manual_tune_poll(opts, state) == DSD_MANUAL_TUNE_FAILED;
            }
            /* The drain after the outcome (ui_settle_squelch(), no command). */
            dsd_engine_receiver_gate_settle(opts, state, 0);
            ok = ok && !channel_gate_fake_closed() && test_rx_runs_gate(&minus1);
        }
        if (!ok || test_squelches != 0 || test_settings != 0) {
            const dsd_squelch_setting now = channel_gate_fake_gate();
            DSD_FPRINTF(stderr,
                        "live Airspy squelch, %s: %d retunes, %d gates, %d untagged pushes; the receiver runs mode %d "
                        "level %g (%d gates run); want -1 dB on the channel it produces\n",
                        what, test_retunes, test_gates, test_squelches + test_settings, now.mode, now.level,
                        test_rx_runs);
            assert(0);
        }
        opts->use_rigctl = 0;
        test_tune_result = 0;
        state->rtl_ctx = NULL;
        freeState(state);
        free(cfg);
        free(cmd);
        free(state);
        free(opts);
    }
}

int
main(int argc, char** argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "D899") == 0) {
        test_config_failure_keeps_the_analog_default();
        test_live_airspy_retune_carries_the_config_gate();
        test_sourceless_squelch_keys_reach_the_receiver();
        test_live_airspy_squelch_reaches_the_receiver_whatever_tunes();
        return 0;
    }
    if (strcmp(argv[1], "M2") == 0) {
        test_serial_cli_override();
    } else if (strcmp(argv[1], "M11") == 0) {
        test_config_refused_width();
    } else if (strcmp(argv[1], "BUG1") == 0) {
        test_config_apply_72();
    } else {
        test_config_apply(strcmp(argv[1], "M7") == 0);
    }
    return 0;
}
