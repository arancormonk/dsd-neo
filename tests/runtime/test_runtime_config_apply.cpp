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

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

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

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float_ne(const char* label, float lhs, float rhs) {
    if (fabsf(lhs - rhs) <= 1e-6f) {
        fprintf(stderr, "FAIL: %s (both %.6f)\n", label, lhs);
        return 1;
    }
    return 0;
}

static void
init_test_runtime(dsd_opts* opts, dsd_state* state) {
    initOpts(opts);
    initState(state);
}

typedef struct test_runtime {
    dsd_opts* opts;
    dsd_state* state;
} test_runtime;

static int
alloc_test_runtime(test_runtime* runtime) {
    memset(runtime, 0, sizeof(*runtime));

    // dsd_state is multi-megabyte; keep it off the function stack.
    runtime->opts = (dsd_opts*)calloc(1, sizeof(*runtime->opts));
    runtime->state = (dsd_state*)calloc(1, sizeof(*runtime->state));
    if (runtime->opts == NULL || runtime->state == NULL) {
        fprintf(stderr, "FAIL: alloc test runtime\n");
        free(runtime->opts);
        free(runtime->state);
        runtime->opts = NULL;
        runtime->state = NULL;
        return 1;
    }

    init_test_runtime(runtime->opts, runtime->state);
    return 0;
}

static void
free_test_runtime(test_runtime* runtime) {
    if (runtime->opts != NULL) {
        if (runtime->opts->audio_in_file != NULL) {
            sf_close(runtime->opts->audio_in_file);
            runtime->opts->audio_in_file = NULL;
        }
        free(runtime->opts->audio_in_file_info);
        runtime->opts->audio_in_file_info = NULL;
    }
    if (runtime->state != NULL) {
        freeState(runtime->state);
    }
    free(runtime->state);
    free(runtime->opts);
    runtime->state = NULL;
    runtime->opts = NULL;
}

static int
create_temp_wav_file(const char* prefix, int sample_rate, int channels, char* out_path, size_t out_path_sz) {
    if (!prefix || !out_path || out_path_sz == 0 || sample_rate <= 0 || channels <= 0 || channels > 2) {
        fprintf(stderr, "FAIL: invalid temp wav request\n");
        return 1;
    }

    char base_path[DSD_TEST_PATH_MAX] = {0};
    int fd = dsd_test_mkstemp(base_path, sizeof base_path, prefix);
    if (fd < 0) {
        fprintf(stderr, "FAIL: dsd_test_mkstemp failed for %s\n", prefix);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(base_path);

    if (snprintf(out_path, out_path_sz, "%s.wav", base_path) >= (int)out_path_sz) {
        fprintf(stderr, "FAIL: temp wav path too long for %s\n", prefix);
        return 1;
    }
    (void)remove(out_path);

    SF_INFO info = {0};
    info.samplerate = sample_rate;
    info.channels = channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* sf = sf_open(out_path, SFM_WRITE, &info);
    if (sf == NULL) {
        fprintf(stderr, "FAIL: sf_open write failed for %s: %s\n", out_path, sf_strerror(NULL));
        (void)remove(out_path);
        return 1;
    }

    short samples[16] = {0};
    sf_count_t sample_count = (sf_count_t)(channels * 8);
    if (sf_write_short(sf, samples, sample_count) != sample_count) {
        fprintf(stderr, "FAIL: sf_write_short failed for %s\n", out_path);
        sf_close(sf);
        (void)remove(out_path);
        return 1;
    }
    if (sf_close(sf) != 0) {
        fprintf(stderr, "FAIL: sf_close failed for %s\n", out_path);
        (void)remove(out_path);
        return 1;
    }
    return 0;
}

static int
create_temp_raw_pcm_wav_suffix(const char* prefix, const short* samples, size_t sample_count, char* out_path,
                               size_t out_path_sz) {
    if (!prefix || !samples || sample_count == 0 || !out_path || out_path_sz == 0) {
        fprintf(stderr, "FAIL: invalid raw temp file request\n");
        return 1;
    }

    char base_path[DSD_TEST_PATH_MAX] = {0};
    int fd = dsd_test_mkstemp(base_path, sizeof base_path, prefix);
    if (fd < 0) {
        fprintf(stderr, "FAIL: dsd_test_mkstemp failed for %s\n", prefix);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(base_path);

    if (snprintf(out_path, out_path_sz, "%s.wav", base_path) >= (int)out_path_sz) {
        fprintf(stderr, "FAIL: temp raw wav path too long for %s\n", prefix);
        return 1;
    }

    FILE* fp = fopen(out_path, "wb");
    if (!fp) {
        fprintf(stderr, "FAIL: fopen write failed for %s\n", out_path);
        return 1;
    }

    size_t nwritten = fwrite(samples, sizeof(samples[0]), sample_count, fp);
    fclose(fp);
    if (nwritten != sample_count) {
        fprintf(stderr, "FAIL: fwrite failed for %s\n", out_path);
        (void)remove(out_path);
        return 1;
    }

    return 0;
}

static int
test_basic_pulse_config_apply(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    // Start from a known input/output so that config apply has something to
    // mutate. Use Pulse I/O to avoid depending on RTL or network resources.
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    opts->audio_in_type = AUDIO_IN_PULSE;
    snprintf(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "pulse");
    opts->audio_out_type = 0;

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
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_true("ncurses flag enabled", opts->use_ncurses_terminal);
    rc |= expect_true("pulse input preserved", strncmp(opts->audio_in_dev, "pulse", 5) == 0);
    rc |= expect_true("pulse output preserved", strncmp(opts->audio_out_dev, "pulse", 5) == 0);
    free_test_runtime(&runtime);
    return rc;
}

static int
test_stereo_file_hot_swap_rolls_back_to_live_input(void) {
    char mono_path[DSD_TEST_PATH_MAX] = {0};
    char stereo_path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_wav_file("dsdneo_cfg_apply_mono", 48000, 1, mono_path, sizeof mono_path) != 0
        || create_temp_wav_file("dsdneo_cfg_apply_stereo", 48000, 2, stereo_path, sizeof stereo_path) != 0) {
        (void)remove(mono_path);
        (void)remove(stereo_path);
        return 1;
    }

    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        (void)remove(mono_path);
        (void)remove(stereo_path);
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    dsd_opts_apply_input_sample_rate(opts, 48000);
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", mono_path);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts->audio_in_file_info));
    if (opts->audio_in_file_info == NULL) {
        fprintf(stderr, "FAIL: alloc mono SF_INFO\n");
        free_test_runtime(&runtime);
        (void)remove(mono_path);
        (void)remove(stereo_path);
        return 1;
    }
    opts->audio_in_file = sf_open(mono_path, SFM_READ, opts->audio_in_file_info);
    if (opts->audio_in_file == NULL) {
        fprintf(stderr, "FAIL: sf_open read failed for %s: %s\n", mono_path, sf_strerror(NULL));
        free_test_runtime(&runtime);
        (void)remove(mono_path);
        (void)remove(stereo_path);
        return 1;
    }
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    state->jitter = 5;

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_FILE;
    cfg.file_sample_rate = 48000;
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_P25P2;
    snprintf(cfg.file_path, sizeof cfg.file_path, "%s", stereo_path);

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(opts, state);

    short sample = 0;
    sf_count_t read_count = sf_read_short(opts->audio_in_file, &sample, 1);

    int rc = 0;
    rc |= expect_true("stereo hot swap keeps WAV input active", opts->audio_in_type == AUDIO_IN_WAV);
    rc |= expect_true("stereo hot swap restores previous path", strcmp(opts->audio_in_dev, mono_path) == 0);
    rc |= expect_true("stereo hot swap keeps previous handle", opts->audio_in_file != NULL);
    rc |= expect_true("stereo hot swap keeps previous metadata",
                      opts->audio_in_file_info != NULL && opts->audio_in_file_info->channels == 1);
    rc |= expect_true("stereo hot swap preserves file sample rate", opts->wav_sample_rate == 48000);
    rc |= expect_true("stereo hot swap keeps previous file readable", read_count == 1);
    rc |= expect_true("stereo hot swap preserves restored file timing", state->samplesPerSymbol == 10);
    rc |= expect_true("stereo hot swap preserves restored file center", state->symbolCenter == 4);
    rc |= expect_true("stereo hot swap preserves restored file jitter", state->jitter == 5);

    free_test_runtime(&runtime);
    (void)remove(mono_path);
    (void)remove(stereo_path);
    return rc;
}

static int
test_return_cc_uses_pulse_rate_not_stale_file_rate(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->pulse_digi_rate_in = 48000;
    opts->wav_sample_rate = 96000;
    opts->p25_trunk = 1;
    state->trunk_cc_freq = 851012500;
    state->p25_cc_is_tdma = 0;
    state->samplesPerSymbol = 20;
    state->symbolCenter = 9;

    ui_post_cmd(UI_CMD_RETURN_CC, NULL, 0);
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_true("return CC recomputes FDMA timing from pulse input rate", state->samplesPerSymbol == 10);
    rc |= expect_true("return CC recomputes pulse symbol center", state->symbolCenter == 4);
    free_test_runtime(&runtime);
    return rc;
}

static int
test_file_config_apply_keeps_live_pulse_timing(void) {
    char wav_path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_wav_file("dsdneo_cfg_apply_cross_backend", 48000, 1, wav_path, sizeof wav_path) != 0) {
        return 1;
    }

    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        (void)remove(wav_path);
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->pulse_digi_rate_in = 48000;
    opts->wav_sample_rate = 96000;
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    state->jitter = 6;

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_FILE;
    cfg.file_sample_rate = 48000;
    snprintf(cfg.file_path, sizeof cfg.file_path, "%s", wav_path);

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_true("cross-backend apply keeps pulse input live", opts->audio_in_type == AUDIO_IN_PULSE);
    rc |= expect_true("cross-backend apply stages requested file path", strcmp(opts->audio_in_dev, wav_path) == 0);
    rc |= expect_int_eq("cross-backend apply keeps live pulse timing rate", dsd_opts_current_input_timing_rate(opts),
                        48000);
    rc |= expect_true("cross-backend apply keeps pulse timing with stale file rate", state->samplesPerSymbol == 10);
    rc |= expect_true("cross-backend apply keeps pulse symbol center with stale file rate", state->symbolCenter == 4);
    rc |= expect_true("cross-backend apply preserves live jitter snapshot", state->jitter == 6);

    free_test_runtime(&runtime);
    (void)remove(wav_path);
    return rc;
}

static int
test_file_config_apply_keeps_live_socket_timing(void) {
    char wav_path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_wav_file("dsdneo_cfg_apply_socket_stage", 96000, 1, wav_path, sizeof wav_path) != 0) {
        return 1;
    }

    const struct {
        int audio_in_type;
        const char* live_dev;
        const char* label;
    } cases[] = {
        {AUDIO_IN_TCP, "tcp:127.0.0.1:7355", "tcp"},
        {AUDIO_IN_UDP, "udp:127.0.0.1:7355", "udp"},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        test_runtime runtime;
        if (alloc_test_runtime(&runtime) != 0) {
            (void)remove(wav_path);
            return 1;
        }

        dsd_opts* opts = runtime.opts;
        dsd_state* state = runtime.state;

        snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", cases[i].live_dev);
        opts->audio_in_type = cases[i].audio_in_type;
        opts->wav_sample_rate = 48000;
        opts->staged_file_sample_rate = 0;
        state->samplesPerSymbol = 10;
        state->symbolCenter = 4;
        state->jitter = 6;

        dsdneoUserConfig cfg = {0};
        cfg.version = 1;
        cfg.has_input = 1;
        cfg.input_source = DSDCFG_INPUT_FILE;
        cfg.file_sample_rate = 96000;
        snprintf(cfg.file_path, sizeof cfg.file_path, "%s", wav_path);

        ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
        ui_drain_cmds(opts, state);

        dsdneoUserConfig snap = {0};
        dsd_snapshot_opts_to_user_config(opts, state, &snap);

        char label[128];
        snprintf(label, sizeof label, "%s apply keeps live socket input active", cases[i].label);
        rc |= expect_true(label, opts->audio_in_type == cases[i].audio_in_type);
        snprintf(label, sizeof label, "%s apply preserves live socket rate", cases[i].label);
        rc |= expect_int_eq(label, opts->wav_sample_rate, 48000);
        snprintf(label, sizeof label, "%s apply stages requested file rate", cases[i].label);
        rc |= expect_int_eq(label, opts->staged_file_sample_rate, 96000);
        snprintf(label, sizeof label, "%s apply keeps live socket timing", cases[i].label);
        rc |= expect_int_eq(label, dsd_opts_current_input_timing_rate(opts), 48000);
        snprintf(label, sizeof label, "%s apply preserves socket sps", cases[i].label);
        rc |= expect_int_eq(label, state->samplesPerSymbol, 10);
        snprintf(label, sizeof label, "%s apply preserves socket center", cases[i].label);
        rc |= expect_int_eq(label, state->symbolCenter, 4);
        snprintf(label, sizeof label, "%s snapshot keeps staged file source", cases[i].label);
        rc |= expect_true(label, snap.has_input && snap.input_source == DSDCFG_INPUT_FILE);
        snprintf(label, sizeof label, "%s snapshot keeps staged file rate", cases[i].label);
        rc |= expect_int_eq(label, snap.file_sample_rate, 96000);

        free_test_runtime(&runtime);
    }

    (void)remove(wav_path);
    return rc;
}

static int
test_file_hot_swap_rebuilds_filters_when_header_matches_configured_rate(void) {
    char old_path[DSD_TEST_PATH_MAX] = {0};
    char new_path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_wav_file("dsdneo_cfg_apply_old_rate", 48000, 1, old_path, sizeof old_path) != 0
        || create_temp_wav_file("dsdneo_cfg_apply_new_rate", 72000, 1, new_path, sizeof new_path) != 0) {
        (void)remove(old_path);
        (void)remove(new_path);
        return 1;
    }

    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        (void)remove(old_path);
        (void)remove(new_path);
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    dsd_opts_apply_input_sample_rate(opts, 48000);
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", old_path);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts->audio_in_file_info));
    if (opts->audio_in_file_info == NULL) {
        fprintf(stderr, "FAIL: alloc old-rate SF_INFO\n");
        free_test_runtime(&runtime);
        (void)remove(old_path);
        (void)remove(new_path);
        return 1;
    }
    opts->audio_in_file = sf_open(old_path, SFM_READ, opts->audio_in_file_info);
    if (opts->audio_in_file == NULL) {
        fprintf(stderr, "FAIL: sf_open read failed for %s: %s\n", old_path, sf_strerror(NULL));
        free_test_runtime(&runtime);
        (void)remove(old_path);
        (void)remove(new_path);
        return 1;
    }

    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    dsd_audio_rescale_symbol_timing(state, 48000, 48000);
    float old_rc_coef = state->RCFilter.coef[0];

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_FILE;
    cfg.file_sample_rate = 72000;
    snprintf(cfg.file_path, sizeof cfg.file_path, "%s", new_path);

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_true("file hot swap keeps WAV input active", opts->audio_in_type == AUDIO_IN_WAV);
    rc |= expect_true("file hot swap updates active path", strcmp(opts->audio_in_dev, new_path) == 0);
    rc |= expect_true("file hot swap adopts new WAV header rate", opts->wav_sample_rate == 72000);
    rc |= expect_true("file hot swap rescales symbol timing to new rate", state->samplesPerSymbol == 15);
    rc |= expect_float_ne("file hot swap rebuilds RCFilter coefficients", state->RCFilter.coef[0], old_rc_coef);

    free_test_runtime(&runtime);
    (void)remove(old_path);
    (void)remove(new_path);
    return rc;
}

static int
test_same_path_headerless_wav_reconfig_keeps_requested_raw_rate(void) {
    const short samples[] = {0x1234, -0x1234, 0x0456, -0x0456};
    char path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_raw_pcm_wav_suffix("dsdneo_cfg_apply_raw_same_path", samples, sizeof samples / sizeof samples[0],
                                       path, sizeof path)
        != 0) {
        return 1;
    }

    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        (void)remove(path);
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    dsd_opts_apply_input_sample_rate(opts, 72000);
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", path);
    opts->audio_in_type = AUDIO_IN_WAV;

    int active_sample_rate = 0;
    int opened_as_container = 1;
    if (dsd_audio_open_mono_file_input(path, opts->wav_sample_rate, &opts->audio_in_file, &opts->audio_in_file_info,
                                       &active_sample_rate, &opened_as_container)
        != 0) {
        fprintf(stderr, "FAIL: dsd_audio_open_mono_file_input failed for %s: %s\n", path, sf_strerror(NULL));
        free_test_runtime(&runtime);
        (void)remove(path);
        return 1;
    }

    state->samplesPerSymbol = 15;
    state->symbolCenter = 6;

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_FILE;
    cfg.file_sample_rate = 48000;
    snprintf(cfg.file_path, sizeof cfg.file_path, "%s", path);

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("same-path raw wav opens as raw input", opened_as_container, 0);
    rc |= expect_int_eq("same-path raw wav starts at configured raw rate", active_sample_rate, 72000);
    rc |= expect_int_eq("same-path raw wav keeps requested reconfigured rate", opts->wav_sample_rate, 48000);
    rc |= expect_int_eq("same-path raw wav keeps raw format metadata",
                        opts->audio_in_file_info->format & SF_FORMAT_TYPEMASK, SF_FORMAT_RAW);
    rc |= expect_int_eq("same-path raw wav rescales timing to requested rate", state->samplesPerSymbol, 10);
    rc |= expect_int_eq("same-path raw wav rescales symbol center", state->symbolCenter, 4);

    free_test_runtime(&runtime);
    (void)remove(path);
    return rc;
}

#ifdef USE_RADIO
static int
test_same_value_rtl_ppm_retry_is_republished(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_gain_value = 10;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_volume_multiplier = 2;
    opts->rtlsdr_ppm_error = 0;
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:1000000:10:5:48:0:2");

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
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_true("same-value config apply restores live requested ppm", opts->rtlsdr_ppm_error == 5);
    rc |= expect_true("same-value config apply keeps device string stable",
                      strncmp(opts->audio_in_dev, "rtl:0:1000000:10:5:48:0:2", sizeof opts->audio_in_dev) == 0);
    free_test_runtime(&runtime);
    return rc;
}

static int
test_zero_rtl_ppm_apply_updates_live_request(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_gain_value = 10;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_volume_multiplier = 2;
    opts->rtlsdr_ppm_error = 9;
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:1000000:10:0:48:0:2");

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
    ui_drain_cmds(opts, state);

    int rc = expect_true("zero ppm config apply updates live requested ppm", opts->rtlsdr_ppm_error == 0);
    free_test_runtime(&runtime);
    return rc;
}

static int
test_omitted_rtl_ppm_apply_preserves_live_request(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_gain_value = 10;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_volume_multiplier = 2;
    opts->rtlsdr_ppm_error = 9;
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:1000000:10:9:48:0:2");

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
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_true("omitted ppm preserves live requested ppm", opts->rtlsdr_ppm_error == 9);
    rc |= expect_true("omitted ppm keeps existing device string ppm",
                      strncmp(opts->audio_in_dev, "rtl:0:1000000:10:9:48:0:2", sizeof opts->audio_in_dev) == 0);
    free_test_runtime(&runtime);
    return rc;
}
#endif

int
main(void) {
    int rc = 0;
    rc |= test_basic_pulse_config_apply();
    rc |= test_stereo_file_hot_swap_rolls_back_to_live_input();
    rc |= test_return_cc_uses_pulse_rate_not_stale_file_rate();
    rc |= test_file_config_apply_keeps_live_pulse_timing();
    rc |= test_file_config_apply_keeps_live_socket_timing();
    rc |= test_file_hot_swap_rebuilds_filters_when_header_matches_configured_rate();
    rc |= test_same_path_headerless_wav_reconfig_keeps_requested_raw_rate();
#ifdef USE_RADIO
    rc |= test_same_value_rtl_ppm_retry_is_republished();
    rc |= test_zero_rtl_ppm_apply_updates_live_request();
    rc |= test_omitted_rtl_ppm_apply_preserves_live_request();
#endif
    return rc ? 1 : 0;
}
