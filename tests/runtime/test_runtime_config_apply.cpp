// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Minimal smoke test for UI_CMD_CONFIG_APPLY runtime behavior.
 *
 * This does not spawn the full ncurses UI; it exercises the config apply
 * command handler with a fake dsd_opts/dsd_state to ensure that applying a
 * config that changes basic fields does not crash and updates core fields as
 * expected. Backend-specific restarts (RTL/RTLTCP/TCP/UDP/Pulse) are covered
 * indirectly by existing integration paths and are intentionally not mocked
 * here to keep this test simple and portable.
 */

#include <dsd-neo/runtime/config.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_cmd.h>

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
#define DSD_NEO_MAIN
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#undef DSD_NEO_MAIN
#ifdef __cplusplus
}
#endif

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static void
init_test_runtime(dsd_opts* opts, dsd_state* state) {
    initOpts(opts);
    initState(state);
}

static int
test_basic_pulse_config_apply(void) {
    dsd_opts opts;
    dsd_state state;
    init_test_runtime(&opts, &state);

    // Start from a known input/output so that config apply has something to
    // mutate. Use Pulse I/O to avoid depending on RTL or network resources.
    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.audio_in_type = AUDIO_IN_PULSE;
    snprintf(opts.audio_out_dev, sizeof opts.audio_out_dev, "%s", "pulse");
    opts.audio_out_type = 0;

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_PULSE;
    snprintf(cfg.pulse_input, sizeof cfg.pulse_input, "%s", "test-source");
    cfg.has_output = 1;
    cfg.output_backend = DSDCFG_OUTPUT_PULSE;
    snprintf(cfg.pulse_output, sizeof cfg.pulse_output, "%s", "test-sink");
    cfg.ncurses_ui = 1;

    // Public API: ui_post_cmd() enqueues; ui_drain_cmds() is called from the
    // demod loop to apply pending commands. For the purposes of this test we
    // call both directly.
    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(&opts, &state);

    int rc = 0;
    rc |= expect_true("ncurses flag enabled", opts.use_ncurses_terminal);
    rc |= expect_true("pulse input preserved", strncmp(opts.audio_in_dev, "pulse", 5) == 0);
    rc |= expect_true("pulse output preserved", strncmp(opts.audio_out_dev, "pulse", 5) == 0);
    return rc;
}

#ifdef USE_RADIO
static int
test_same_value_rtl_ppm_retry_is_republished(void) {
    dsd_opts opts;
    dsd_state state;
    init_test_runtime(&opts, &state);

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_gain_value = 10;
    opts.rtl_dsp_bw_khz = 48;
    opts.rtl_volume_multiplier = 2;
    opts.rtlsdr_ppm_error = 0;
    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:1000000:10:5:48:0:2");

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    snprintf(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "1000000");
    cfg.rtl_gain = 10;
    cfg.rtl_ppm = 5;
    cfg.rtl_ppm_is_set = 1;
    cfg.rtl_bw_khz = 48;
    cfg.rtl_volume = 2;

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(&opts, &state);

    int rc = 0;
    rc |= expect_true("same-value config apply restores live requested ppm", opts.rtlsdr_ppm_error == 5);
    rc |= expect_true("same-value config apply keeps device string stable",
                      strncmp(opts.audio_in_dev, "rtl:0:1000000:10:5:48:0:2", sizeof opts.audio_in_dev) == 0);
    return rc;
}

static int
test_zero_rtl_ppm_apply_updates_live_request(void) {
    dsd_opts opts;
    dsd_state state;
    init_test_runtime(&opts, &state);

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_gain_value = 10;
    opts.rtl_dsp_bw_khz = 48;
    opts.rtl_volume_multiplier = 2;
    opts.rtlsdr_ppm_error = 9;
    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:1000000:10:0:48:0:2");

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    snprintf(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "1000000");
    cfg.rtl_gain = 10;
    cfg.rtl_ppm = 0;
    cfg.rtl_ppm_is_set = 1;
    cfg.rtl_bw_khz = 48;
    cfg.rtl_volume = 2;

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(&opts, &state);

    return expect_true("zero ppm config apply updates live requested ppm", opts.rtlsdr_ppm_error == 0);
}

static int
test_omitted_rtl_ppm_apply_preserves_live_request(void) {
    dsd_opts opts;
    dsd_state state;
    init_test_runtime(&opts, &state);

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_gain_value = 10;
    opts.rtl_dsp_bw_khz = 48;
    opts.rtl_volume_multiplier = 2;
    opts.rtlsdr_ppm_error = 9;
    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:1000000:10:9:48:0:2");

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    snprintf(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "1000000");
    cfg.rtl_gain = 10;
    cfg.rtl_bw_khz = 48;
    cfg.rtl_volume = 2;

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(&opts, &state);

    int rc = 0;
    rc |= expect_true("omitted ppm preserves live requested ppm", opts.rtlsdr_ppm_error == 9);
    rc |= expect_true("omitted ppm keeps existing device string ppm",
                      strncmp(opts.audio_in_dev, "rtl:0:1000000:10:9:48:0:2", sizeof opts.audio_in_dev) == 0);
    return rc;
}
#endif

int
main(void) {
    int rc = 0;
    rc |= test_basic_pulse_config_apply();
#ifdef USE_RADIO
    rc |= test_same_value_rtl_ppm_retry_is_republished();
    rc |= test_zero_rtl_ppm_apply_updates_live_request();
    rc |= test_omitted_rtl_ppm_apply_preserves_live_request();
#endif
    return rc ? 1 : 0;
}
