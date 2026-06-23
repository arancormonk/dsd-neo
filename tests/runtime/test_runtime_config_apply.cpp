// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(performance-type-promotion-in-math-fn)
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Minimal smoke test for UI_CMD_CONFIG_APPLY runtime behavior.
 *
 * This does not spawn the full ncurses UI; it exercises the config apply
 * command handler with a fake dsd_opts/dsd_state to ensure that applying a
 * config that changes basic fields does not crash and updates core fields as
 * expected. Most backend-specific restarts remain covered indirectly by other
 * integration paths; TCP reconnect is stubbed here because config-apply
 * rollback semantics depend on the helper's success/failure result.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_cmd.h>
#include <dsd-neo/ui/ui_dsp_cmd.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/file_compat.h"
#include "dsd-neo/platform/posix_compat.h"
#include "dsd-neo/runtime/call_alert.h"
#include "menu_actions.h"
#include "menu_callbacks.h"
#include "menu_internal.h"
#include "test_support.h"
#include "ui_key_status.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %d want %d)\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64_eq(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %016llX want %016llX)\n", label, (unsigned long long)got,
                    (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_float_ne(const char* label, float lhs, float rhs) {
    if (fabsf(lhs - rhs) <= 1e-6f) {
        DSD_FPRINTF(stderr, "FAIL: %s (both %.6f)\n", label, lhs);
        return 1;
    }
    return 0;
}

static int
expect_float_near(const char* label, float got, float want, float tolerance) {
    if (fabsf(got - want) > tolerance) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %.6f want %.6f)\n", label, got, want);
        return 1;
    }
    return 0;
}

static uint16_t
static_bits_to_u16(const uint8_t bits[882]) {
    uint16_t value = 0U;
    for (int i = 0; i < 16; ++i) {
        value = (uint16_t)((value << 1U) | (uint16_t)(bits[i] & 1U));
    }
    return value;
}

static int g_tcp_connect_audio_calls = 0;
static int g_tcp_connect_audio_result = 0;
static char g_last_tcp_connect_audio_host[256];
static int g_last_tcp_connect_audio_port = 0;

static void
reset_tcp_connect_audio_fake(void) {
    g_tcp_connect_audio_calls = 0;
    g_tcp_connect_audio_result = 0;
    g_last_tcp_connect_audio_host[0] = '\0';
    g_last_tcp_connect_audio_port = 0;
}

int
svc_tcp_connect_audio(dsd_opts* opts, const char* host, int port) { // NOLINT(misc-use-internal-linkage)
    g_tcp_connect_audio_calls++;
    DSD_SNPRINTF(g_last_tcp_connect_audio_host, sizeof g_last_tcp_connect_audio_host, "%s", host ? host : "");
    g_last_tcp_connect_audio_port = port;

    if (g_tcp_connect_audio_result == 0 && opts && host && port > 0) {
        DSD_SNPRINTF(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", host);
        opts->tcp_portno = port;
        opts->audio_in_type = AUDIO_IN_TCP;
    }
    return g_tcp_connect_audio_result;
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
    DSD_MEMSET(runtime, 0, sizeof(*runtime));

    // dsd_state is multi-megabyte; keep it off the function stack.
    runtime->opts = (dsd_opts*)calloc(1, sizeof(*runtime->opts));
    runtime->state = (dsd_state*)calloc(1, sizeof(*runtime->state));
    if (runtime->opts == NULL || runtime->state == NULL) {
        DSD_FPRINTF(stderr, "FAIL: alloc test runtime\n");
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
        closeAudioInput(runtime->opts);
        closeAudioOutput(runtime->opts);
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
create_temp_config_file(const char* contents, char* out_path, size_t out_path_sz) {
    if (!contents || !out_path || out_path_sz == 0) {
        DSD_FPRINTF(stderr, "FAIL: invalid temp config request\n");
        return 1;
    }

    char path[DSD_TEST_PATH_MAX] = {0};
    int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_ui_profile_config");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_test_mkstemp failed for temp config\n");
        return 1;
    }
    (void)dsd_close(fd);

    FILE* fp = dsd_fopen_private(path, "w");
    if (!fp) {
        DSD_FPRINTF(stderr, "FAIL: fopen write failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    int write_failed = fputs(contents, fp) < 0;
    int close_failed = fclose(fp) != 0;
    if (write_failed || close_failed) {
        DSD_FPRINTF(stderr, "FAIL: write failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int n = DSD_SNPRINTF(out_path, out_path_sz, "%s", path);
    if (n < 0 || n >= (int)out_path_sz) {
        DSD_FPRINTF(stderr, "FAIL: temp config path too long\n");
        (void)remove(path);
        return 1;
    }
    return 0;
}

static int
create_temp_wav_file(const char* prefix, int sample_rate, int channels, char* out_path, size_t out_path_sz) {
    if (!prefix || !out_path || out_path_sz == 0 || sample_rate <= 0 || channels <= 0 || channels > 2) {
        DSD_FPRINTF(stderr, "FAIL: invalid temp wav request\n");
        return 1;
    }

    char base_path[DSD_TEST_PATH_MAX] = {0};
    int fd = dsd_test_mkstemp(base_path, sizeof base_path, prefix);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_test_mkstemp failed for %s\n", prefix);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(base_path);

    if (DSD_SNPRINTF(out_path, out_path_sz, "%s.wav", base_path) >= (int)out_path_sz) {
        DSD_FPRINTF(stderr, "FAIL: temp wav path too long for %s\n", prefix);
        return 1;
    }
    (void)remove(out_path);

    SF_INFO info = {0};
    info.samplerate = sample_rate;
    info.channels = channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* sf = sf_open(out_path, SFM_WRITE, &info);
    if (sf == NULL) {
        DSD_FPRINTF(stderr, "FAIL: sf_open write failed for %s: %s\n", out_path, sf_strerror(NULL));
        (void)remove(out_path);
        return 1;
    }

    short samples[16] = {0};
    sf_count_t sample_count = ((sf_count_t)channels) * 8;
    if (sf_write_short(sf, samples, sample_count) != sample_count) {
        DSD_FPRINTF(stderr, "FAIL: sf_write_short failed for %s\n", out_path);
        sf_close(sf);
        (void)remove(out_path);
        return 1;
    }
    if (sf_close(sf) != 0) {
        DSD_FPRINTF(stderr, "FAIL: sf_close failed for %s\n", out_path);
        (void)remove(out_path);
        return 1;
    }
    return 0;
}

static int
create_removed_temp_path(const char* prefix, char* out_path, size_t out_path_sz) {
    if (!prefix || !out_path || out_path_sz == 0) {
        DSD_FPRINTF(stderr, "FAIL: invalid temp path request\n");
        return 1;
    }

    char path[DSD_TEST_PATH_MAX] = {0};
    int fd = dsd_test_mkstemp(path, sizeof path, prefix);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_test_mkstemp failed for %s\n", prefix);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(path);

    if (DSD_SNPRINTF(out_path, out_path_sz, "%s", path) >= (int)out_path_sz) {
        DSD_FPRINTF(stderr, "FAIL: temp path too long for %s\n", prefix);
        return 1;
    }
    return 0;
}

static int
create_temp_raw_pcm_wav_suffix(const char* prefix, const short* samples, size_t sample_count, char* out_path,
                               size_t out_path_sz) {
    if (!prefix || !samples || sample_count == 0 || !out_path || out_path_sz == 0) {
        DSD_FPRINTF(stderr, "FAIL: invalid raw temp file request\n");
        return 1;
    }

    char base_path[DSD_TEST_PATH_MAX] = {0};
    int fd = dsd_test_mkstemp(base_path, sizeof base_path, prefix);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_test_mkstemp failed for %s\n", prefix);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(base_path);

    if (DSD_SNPRINTF(out_path, out_path_sz, "%s.wav", base_path) >= (int)out_path_sz) {
        DSD_FPRINTF(stderr, "FAIL: temp raw wav path too long for %s\n", prefix);
        return 1;
    }

    FILE* fp = dsd_fopen_private(out_path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "FAIL: fopen write failed for %s\n", out_path);
        return 1;
    }

    size_t nwritten = fwrite(samples, sizeof(samples[0]), sample_count, fp);
    fclose(fp);
    if (nwritten != sample_count) {
        DSD_FPRINTF(stderr, "FAIL: fwrite failed for %s\n", out_path);
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
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    opts->audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "pulse");
    opts->audio_out_type = 0;

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_PULSE;
    DSD_SNPRINTF(cfg.pulse_input, sizeof cfg.pulse_input, "%s", "test-source");
    cfg.has_output = 1;
    cfg.output_backend = DSDCFG_OUTPUT_PULSE;
    DSD_SNPRINTF(cfg.pulse_output, sizeof cfg.pulse_output, "%s", "test-sink");
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
test_ui_command_queue_applies_fifo(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    int32_t first = 5;
    int32_t second = 11;
    ui_post_cmd(UI_CMD_GAIN_SET, &first, sizeof first);
    ui_post_cmd(UI_CMD_GAIN_SET, &second, sizeof second);

    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("FIFO drain count", applied, 2);
    rc |= expect_int_eq("FIFO final digital gain", (int)opts->audio_gain, second);
    rc |= expect_int_eq("FIFO final left state gain", (int)state->aout_gain, second);
    rc |= expect_int_eq("FIFO final right state gain", (int)state->aout_gainR, second);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_command_queue_overflow_drops_oldest(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    dsd_exitflag_store(0);
    ui_post_cmd(UI_CMD_QUIT, NULL, 0);
    for (int i = 0; i < 127; i++) {
        int32_t gain = i;
        ui_post_cmd(UI_CMD_GAIN_SET, &gain, sizeof gain);
    }

    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("overflow keeps bounded queue depth", applied, 127);
    rc |= expect_int_eq("overflow drops oldest quit command", (int)dsd_exitflag_load(), 0);
    rc |= expect_int_eq("overflow applies newest gain command with clamp", (int)opts->audio_gain, 50);
    rc |= expect_int_eq("overflow updates gain snapshot", (int)state->aout_gain, 50);

    dsd_exitflag_store(0);
    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_command_queue_truncates_oversized_payload_string(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    enum { OVERSIZED_PAYLOAD_LEN = UI_CMD_DATA_MAX + 97 };

    char* oversized = (char*)malloc(OVERSIZED_PAYLOAD_LEN);
    if (!oversized) {
        DSD_FPRINTF(stderr, "FAIL: oversized payload alloc\n");
        free_test_runtime(&runtime);
        return 1;
    }
    DSD_MEMSET(oversized, 'x', OVERSIZED_PAYLOAD_LEN);

    ui_post_cmd(UI_CMD_INPUT_WAV_SET, oversized, OVERSIZED_PAYLOAD_LEN);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("oversized string command drain count", applied, 1);
    rc |= expect_int_eq("oversized string switches input type", opts->audio_in_type, AUDIO_IN_WAV);
    rc |= expect_int_eq("oversized string is field bounded", (int)strlen(opts->audio_in_dev),
                        (int)sizeof(opts->audio_in_dev) - 1);
    rc |= expect_true("oversized string preserves copied prefix",
                      strspn(opts->audio_in_dev, "x") == strlen(opts->audio_in_dev));
    rc |= expect_true("oversized string writes toast", strstr(state->ui_msg, "Applied: WAV input") != NULL);

    free(oversized);
    free_test_runtime(&runtime);
    return rc;
}

static void
free_test_profile_context(ProfileSelCtx* pctx) {
    if (!pctx) {
        return;
    }
    if (pctx->names) {
        for (int i = 0; i < pctx->n; i++) {
            free((void*)pctx->names[i]);
        }
    }
    free((void*)pctx->labels);
    free((void*)pctx->names);
    free(pctx);
}

static ProfileSelCtx*
make_test_profile_context(dsd_state* state, const char* path) {
    ProfileSelCtx* pctx = (ProfileSelCtx*)calloc(1, sizeof(*pctx));
    if (!pctx) {
        return NULL;
    }
    pctx->state = state;
    pctx->n = 2;
    int n = DSD_SNPRINTF(pctx->path, sizeof pctx->path, "%s", path ? path : "");
    if (n < 0 || n >= (int)sizeof pctx->path) {
        free_test_profile_context(pctx);
        return NULL;
    }
    pctx->labels = (const char**)calloc((size_t)pctx->n, sizeof(char*));
    pctx->names = (const char**)calloc((size_t)pctx->n, sizeof(char*));
    if (!pctx->labels || !pctx->names) {
        free_test_profile_context(pctx);
        return NULL;
    }
    pctx->names[0] = dsd_strdup("alpha");
    pctx->names[1] = dsd_strdup("beta");
    if (!pctx->names[0] || !pctx->names[1]) {
        free_test_profile_context(pctx);
        return NULL;
    }
    pctx->labels[0] = pctx->names[0];
    pctx->labels[1] = pctx->names[1];
    return pctx;
}

static int
test_ui_profile_selection_applies_overlay_and_disables_autosave(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[output]\n"
                             "backend = \"null\"\n"
                             "ncurses_ui = false\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"p25p1\"\n"
                             "\n"
                             "[profile.alpha]\n"
                             "mode.decode = \"ysf\"\n"
                             "\n"
                             "[profile.beta]\n"
                             "mode.decode = \"dmr\"\n"
                             "output.ncurses_ui = true\n";

    char path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_config_file(ini, path, sizeof path) != 0) {
        return 1;
    }

    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        (void)remove(path);
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;
    state->config_autosave_enabled = 1;
    DSD_SNPRINTF(state->config_autosave_path, sizeof state->config_autosave_path, "%s", path);

    ProfileSelCtx* pctx = make_test_profile_context(state, path);
    if (!pctx) {
        DSD_FPRINTF(stderr, "FAIL: could not allocate profile test context\n");
        free_test_runtime(&runtime);
        (void)remove(path);
        return 1;
    }

    chooser_done_config_profile(pctx, 1);
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("UI profile load disables autosave", state->config_autosave_enabled, 0);
    rc |= expect_true("UI profile load retains config path", strcmp(state->config_autosave_path, path) == 0);
    rc |= expect_true("UI profile overlay applies DMR mode",
                      opts->frame_dmr == 1 && opts->frame_p25p1 == 0 && opts->frame_p25p2 == 0 && opts->frame_ysf == 0);
    rc |= expect_int_eq("UI profile overlay applies ncurses flag", opts->use_ncurses_terminal, 1);

    free_test_runtime(&runtime);
    (void)remove(path);
    return rc;
}

static int
test_ui_profile_menu_no_profiles_does_not_apply_base_config(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[output]\n"
                             "backend = \"null\"\n"
                             "ncurses_ui = true\n";

    char path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_config_file(ini, path, sizeof path) != 0) {
        return 1;
    }

    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        (void)remove(path);
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;
    UiCtx ctx = {opts, state};

    state->config_autosave_enabled = 1;
    DSD_SNPRINTF(state->config_autosave_path, sizeof state->config_autosave_path, "%s", path);

    act_config_load_profile(&ctx);
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("no-profile UI load leaves autosave enabled", state->config_autosave_enabled, 1);
    rc |= expect_true("no-profile UI load retains config path", strcmp(state->config_autosave_path, path) == 0);
    rc |= expect_int_eq("no-profile UI load does not apply base ncurses flag", opts->use_ncurses_terminal, 0);

    free_test_runtime(&runtime);
    (void)remove(path);
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
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", mono_path);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts->audio_in_file_info));
    if (opts->audio_in_file_info == NULL) {
        DSD_FPRINTF(stderr, "FAIL: alloc mono SF_INFO\n");
        free_test_runtime(&runtime);
        (void)remove(mono_path);
        (void)remove(stereo_path);
        return 1;
    }
    opts->audio_in_file = sf_open(mono_path, SFM_READ, opts->audio_in_file_info);
    if (opts->audio_in_file == NULL) {
        DSD_FPRINTF(stderr, "FAIL: sf_open read failed for %s: %s\n", mono_path, sf_strerror(NULL));
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
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", stereo_path);

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
test_call_alert_off_selection_survives_ui_command_path(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    opts->call_alert = 1;
    opts->call_alert_events = DSD_CALL_ALERT_EVENT_ALL;

    uint8_t events = 0;
    ui_post_cmd(UI_CMD_CALL_ALERT_EVENTS_SET, &events, sizeof events);
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("call alert off selection disables master", opts->call_alert, 0);
    rc |= expect_int_eq("call alert off selection stores empty mask", opts->call_alert_events, 0);
    rc |= expect_int_eq(
        "call alert off selection suppresses data event",
        dsd_call_alert_event_enabled(opts->call_alert, opts->call_alert_events, DSD_CALL_ALERT_EVENT_DATA), 0);

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(opts, state, &snap);
    rc |= expect_true("call alert snapshot includes alerts", snap.has_alerts);
    rc |= expect_int_eq("call alert snapshot preserves disabled master", snap.call_alert_enabled, 0);
    rc |= expect_int_eq("call alert snapshot preserves empty event mask", snap.call_alert_events, 0);

    ui_post_cmd(UI_CMD_CALL_ALERT_TOGGLE, NULL, 0);
    ui_drain_cmds(opts, state);

    rc |= expect_int_eq("call alert toggle keeps empty selection disabled", opts->call_alert, 0);
    rc |= expect_int_eq("call alert toggle preserves empty event mask", opts->call_alert_events, 0);
    rc |= expect_int_eq(
        "call alert toggle with empty selection suppresses data event",
        dsd_call_alert_event_enabled(opts->call_alert, opts->call_alert_events, DSD_CALL_ALERT_EVENT_DATA), 0);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_visibility_toggles_preserve_show_keys(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    const struct VisibilityCommand {
        int cmd_id;
        const char* label;
    } commands[] = {
        {UI_CMD_UI_SHOW_P25_AFFIL_TOGGLE, "P25 affiliations visibility toggle preserves show_keys"},
        {UI_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE, "P25 callsign visibility toggle preserves show_keys"},
        {UI_CMD_P25_GA_TOGGLE, "P25 group affiliation toggle preserves show_keys"},
        {UI_CMD_UI_SHOW_DSP_PANEL_TOGGLE, "DSP panel visibility toggle preserves show_keys"},
        {UI_CMD_UI_SHOW_P25_METRICS_TOGGLE, "P25 metrics visibility toggle preserves show_keys"},
        {UI_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE, "P25 neighbors visibility toggle preserves show_keys"},
        {UI_CMD_UI_SHOW_P25_IDEN_TOGGLE, "P25 IDEN visibility toggle preserves show_keys"},
        {UI_CMD_UI_SHOW_P25_CCC_TOGGLE, "P25 CC candidates visibility toggle preserves show_keys"},
        {UI_CMD_UI_SHOW_CHANNELS_TOGGLE, "channels visibility toggle preserves show_keys"},
    };

    int rc = 0;
    for (size_t i = 0U; i < sizeof commands / sizeof commands[0]; i++) {
        opts->show_keys = 1U;
        ui_post_cmd(commands[i].cmd_id, NULL, 0);
        ui_drain_cmds(opts, state);
        rc |= expect_int_eq(commands[i].label, (int)opts->show_keys, 1);
    }

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_aes_key_command_clears_manual_hytera_fields(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    state->H = 0x000000AABBCCDDEEULL;
    state->K1 = 0x1111111111111111ULL;
    state->K2 = 0x2222222222222222ULL;
    state->K3 = 0x3333333333333333ULL;
    state->K4 = 0x4444444444444444ULL;
    state->keyloader = 1;
    state->payload_keyid = 7;
    state->payload_keyidR = 8;
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;

    struct AesKeyPayload {
        uint64_t K1, K2, K3, K4;
    } p = {
        0x20029736A5D91042ULL,
        0xC923EB0697484433ULL,
        0x005EFC58A1905195ULL,
        0xE28E9C7836AA2DB8ULL,
    };

    ui_post_cmd(UI_CMD_KEY_AES_SET, &p, sizeof p);
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_u64_eq("UI AES A1 slot 0", state->A1[0], p.K1);
    rc |= expect_u64_eq("UI AES A2 slot 0", state->A2[0], p.K2);
    rc |= expect_u64_eq("UI AES A3 slot 0", state->A3[0], p.K3);
    rc |= expect_u64_eq("UI AES A4 slot 0", state->A4[0], p.K4);
    rc |= expect_u64_eq("UI AES A1 slot 1", state->A1[1], p.K1);
    rc |= expect_u64_eq("UI AES A2 slot 1", state->A2[1], p.K2);
    rc |= expect_u64_eq("UI AES A3 slot 1", state->A3[1], p.K3);
    rc |= expect_u64_eq("UI AES A4 slot 1", state->A4[1], p.K4);
    rc |= expect_int_eq("UI AES loaded slot 0", state->aes_key_loaded[0], 1);
    rc |= expect_int_eq("UI AES loaded slot 1", state->aes_key_loaded[1], 1);
    rc |= expect_int_eq("UI AES segment count slot 0", (int)state->aes_key_segments[0], 4);
    rc |= expect_int_eq("UI AES segment count slot 1", (int)state->aes_key_segments[1], 4);
    rc |= expect_u64_eq("UI AES clears H", state->H, 0ULL);
    rc |= expect_u64_eq("UI AES clears K1", state->K1, 0ULL);
    rc |= expect_u64_eq("UI AES clears K2", state->K2, 0ULL);
    rc |= expect_u64_eq("UI AES clears K3", state->K3, 0ULL);
    rc |= expect_u64_eq("UI AES clears K4", state->K4, 0ULL);
    rc |= expect_int_eq("UI AES clears Hytera segment count", (int)state->hytera_key_segments, 0);
    rc |= expect_int_eq("UI AES disables keyloader", state->keyloader, 0);
    rc |= expect_int_eq("UI AES clears slot 0 payload key ID", state->payload_keyid, 0);
    rc |= expect_int_eq("UI AES clears slot 1 payload key ID", state->payload_keyidR, 0);
    rc |= expect_int_eq("UI AES unmutes encrypted left", opts->dmr_mute_encL, 0);
    rc |= expect_int_eq("UI AES unmutes encrypted right", opts->dmr_mute_encR, 0);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_aes_key_command_handles_zero_and_short_payloads(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    state->A1[0] = 0x1111ULL;
    state->aes_key_loaded[0] = 1;
    state->aes_key_segments[0] = 2U;
    state->H = 0x1234ULL;
    state->K1 = 0x5678ULL;
    state->hytera_key_segments = 1U;
    state->keyloader = 1;
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;

    uint64_t short_payload[3] = {0x10ULL, 0x20ULL, 0x30ULL};
    ui_post_cmd(UI_CMD_KEY_AES_SET, short_payload, sizeof short_payload);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("short AES key command is drained", applied, 1);
    rc |= expect_u64_eq("short AES preserves A1", state->A1[0], 0x1111ULL);
    rc |= expect_int_eq("short AES preserves loaded flag", state->aes_key_loaded[0], 1);
    rc |= expect_int_eq("short AES preserves segment count", (int)state->aes_key_segments[0], 2);
    rc |= expect_int_eq("short AES preserves keyloader", state->keyloader, 1);
    rc |= expect_int_eq("short AES preserves encrypted mute", opts->dmr_mute_encL, 1);

    struct AesKeyPayload {
        uint64_t K1, K2, K3, K4;
    } zero = {0ULL, 0ULL, 0ULL, 0ULL};

    ui_post_cmd(UI_CMD_KEY_AES_SET, &zero, sizeof zero);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("zero AES key command is drained", applied, 1);
    rc |= expect_u64_eq("zero AES clears A1 slot 0", state->A1[0], 0ULL);
    rc |= expect_u64_eq("zero AES clears A4 slot 1", state->A4[1], 0ULL);
    rc |= expect_int_eq("zero AES leaves slot 0 unloaded", state->aes_key_loaded[0], 0);
    rc |= expect_int_eq("zero AES leaves slot 1 unloaded", state->aes_key_loaded[1], 0);
    rc |= expect_int_eq("zero AES records full segment width slot 0", (int)state->aes_key_segments[0], 4);
    rc |= expect_int_eq("zero AES records full segment width slot 1", (int)state->aes_key_segments[1], 4);
    rc |= expect_u64_eq("zero AES clears Hytera H", state->H, 0ULL);
    rc |= expect_u64_eq("zero AES clears Hytera K1", state->K1, 0ULL);
    rc |= expect_int_eq("zero AES clears Hytera segment count", (int)state->hytera_key_segments, 0);
    rc |= expect_int_eq("zero AES clears keyloader", state->keyloader, 0);
    rc |= expect_int_eq("zero AES unmutes encrypted left", opts->dmr_mute_encL, 0);
    rc |= expect_int_eq("zero AES unmutes encrypted right", opts->dmr_mute_encR, 0);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_hytera_status_counts_k_fields_only(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_state* state = runtime.state;

    state->H = 0x2345ULL;
    state->aes_key_segments[0] = 4U;
    state->aes_key_segments[1] = 2U;

    int rc = 0;
    rc |=
        expect_int_eq("UI Hytera status ignores H-only stale AES metadata", (int)ui_hytera_key_segment_count(state), 0);

    state->K1 = 0x0123456789ULL;
    rc |= expect_int_eq("UI Hytera BP status ignores stale AES metadata", (int)ui_hytera_key_segment_count(state), 1);

    state->hytera_key_segments = 2U;
    rc |= expect_int_eq("UI Hytera 128-bit status preserves zero K2", (int)ui_hytera_key_segment_count(state), 2);
    state->hytera_key_segments = 0U;

    state->K2 = 0x1122334455667788ULL;
    rc |= expect_int_eq("UI Hytera 128-bit status counts K fields", (int)ui_hytera_key_segment_count(state), 2);

    state->K2 = 0ULL;
    state->K4 = 0x8877665544332211ULL;
    rc |= expect_int_eq("UI Hytera 256-bit status counts upper K fields", (int)ui_hytera_key_segment_count(state), 4);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_hytera_key_command_records_segment_variants(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    struct HyteraKeyPayload {
        uint64_t H, K1, K2, K3, K4;
    } p = {0ULL, 0ULL, 0ULL, 0ULL, 0ULL};

    state->hytera_key_segments = 4U;
    state->keyloader = 1;
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;
    ui_post_cmd(UI_CMD_KEY_HYTERA_SET, &p, sizeof p);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("zero Hytera command is drained", applied, 1);
    rc |= expect_int_eq("zero Hytera clears segment count", (int)state->hytera_key_segments, 0);
    rc |= expect_int_eq("zero Hytera clears keyloader", state->keyloader, 0);
    rc |= expect_int_eq("zero Hytera unmutes encrypted left", opts->dmr_mute_encL, 0);
    rc |= expect_int_eq("zero Hytera unmutes encrypted right", opts->dmr_mute_encR, 0);

    p.H = 0x10ULL;
    p.K1 = 0x11ULL;
    p.K2 = 0x22ULL;
    state->hytera_key_segments = 0U;
    ui_post_cmd(UI_CMD_KEY_HYTERA_SET, &p, sizeof p);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("two-segment Hytera command is drained", applied, 1);
    rc |= expect_int_eq("two-segment Hytera records segment count", (int)state->hytera_key_segments, 2);
    rc |= expect_u64_eq("two-segment Hytera stores K2", state->K2, p.K2);

    p.K3 = 0x33ULL;
    p.K4 = 0ULL;
    ui_post_cmd(UI_CMD_KEY_HYTERA_SET, &p, sizeof p);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("upper-segment Hytera command is drained", applied, 1);
    rc |= expect_int_eq("upper-segment Hytera records four segments", (int)state->hytera_key_segments, 4);
    rc |= expect_u64_eq("upper-segment Hytera stores K3", state->K3, p.K3);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_hytera_key_command_preserves_aes_metadata(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    state->A1[0] = 0x20029736A5D91042ULL;
    state->A2[0] = 0xC923EB0697484433ULL;
    state->A3[0] = 0x005EFC58A1905195ULL;
    state->A4[0] = 0xE28E9C7836AA2DB8ULL;
    state->A1[1] = 0x361097A53A529002ULL;
    state->A2[1] = 0x3344489706EB23C9ULL;
    state->A3[1] = 0x955190A158FC5E00ULL;
    state->A4[1] = 0xB82DAA36789C8EE2ULL;
    state->aes_key_loaded[0] = 1;
    state->aes_key_loaded[1] = 1;
    state->aes_key_segments[0] = 4U;
    state->aes_key_segments[1] = 4U;

    uint8_t expected_aes_key[32];
    for (size_t i = 0U; i < sizeof expected_aes_key; i++) {
        state->aes_key[i] = (uint8_t)(i + 1U);
        expected_aes_key[i] = state->aes_key[i];
    }

    struct HyteraKeyPayload {
        uint64_t H, K1, K2, K3, K4;
    } p = {
        0x0123456789ULL, 0x0123456789ULL, 0ULL, 0ULL, 0ULL,
    };

    ui_post_cmd(UI_CMD_KEY_HYTERA_SET, &p, sizeof p);
    ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_u64_eq("UI Hytera command sets H", state->H, p.H);
    rc |= expect_u64_eq("UI Hytera command sets K1", state->K1, p.K1);
    rc |= expect_u64_eq("UI Hytera command sets K2", state->K2, p.K2);
    rc |= expect_u64_eq("UI Hytera command sets K3", state->K3, p.K3);
    rc |= expect_u64_eq("UI Hytera command sets K4", state->K4, p.K4);
    rc |= expect_int_eq("UI Hytera command records BP segment count", (int)state->hytera_key_segments, 1);
    rc |= expect_int_eq("UI Hytera preserves AES loaded slot 0", state->aes_key_loaded[0], 1);
    rc |= expect_int_eq("UI Hytera preserves AES loaded slot 1", state->aes_key_loaded[1], 1);
    rc |= expect_int_eq("UI Hytera preserves AES segment count slot 0", (int)state->aes_key_segments[0], 4);
    rc |= expect_int_eq("UI Hytera preserves AES segment count slot 1", (int)state->aes_key_segments[1], 4);
    rc |= expect_u64_eq("UI Hytera preserves AES A1 slot 0", state->A1[0], 0x20029736A5D91042ULL);
    rc |= expect_u64_eq("UI Hytera preserves AES A2 slot 0", state->A2[0], 0xC923EB0697484433ULL);
    rc |= expect_u64_eq("UI Hytera preserves AES A3 slot 0", state->A3[0], 0x005EFC58A1905195ULL);
    rc |= expect_u64_eq("UI Hytera preserves AES A4 slot 0", state->A4[0], 0xE28E9C7836AA2DB8ULL);
    rc |= expect_u64_eq("UI Hytera preserves AES A1 slot 1", state->A1[1], 0x361097A53A529002ULL);
    rc |= expect_u64_eq("UI Hytera preserves AES A2 slot 1", state->A2[1], 0x3344489706EB23C9ULL);
    rc |= expect_u64_eq("UI Hytera preserves AES A3 slot 1", state->A3[1], 0x955190A158FC5E00ULL);
    rc |= expect_u64_eq("UI Hytera preserves AES A4 slot 1", state->A4[1], 0xB82DAA36789C8EE2ULL);
    rc |= expect_true("UI Hytera preserves AES key bytes",
                      memcmp(state->aes_key, expected_aes_key, sizeof expected_aes_key) == 0);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_basic_key_commands_reset_payload_mute_state(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    state->K = 0x12345678U;
    state->keyloader = 1;
    state->payload_keyid = 77;
    state->payload_keyidR = 88;
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;

    uint8_t short_key = 0xFEU;
    ui_post_cmd(UI_CMD_KEY_BASIC_SET, &short_key, sizeof short_key);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("short basic key command is drained", applied, 1);
    rc |= expect_int_eq("short basic key preserves K", (int)state->K, 0x12345678);
    rc |= expect_int_eq("short basic key preserves keyloader", state->keyloader, 1);
    rc |= expect_int_eq("short basic key preserves left mute", opts->dmr_mute_encL, 1);
    rc |= expect_int_eq("short basic key preserves right mute", opts->dmr_mute_encR, 1);

    uint32_t basic = 0xA5A55A5AU;
    ui_post_cmd(UI_CMD_KEY_BASIC_SET, &basic, sizeof basic);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("basic key command is drained", applied, 1);
    rc |= expect_int_eq("basic key command sets K", (int)state->K, (int)basic);
    rc |= expect_int_eq("basic key clears keyloader", state->keyloader, 0);
    rc |= expect_int_eq("basic key clears left payload key ID", state->payload_keyid, 0);
    rc |= expect_int_eq("basic key clears right payload key ID", state->payload_keyidR, 0);
    rc |= expect_int_eq("basic key unmutes encrypted left", opts->dmr_mute_encL, 0);
    rc |= expect_int_eq("basic key unmutes encrypted right", opts->dmr_mute_encR, 0);

    state->keyloader = 1;
    state->payload_keyid = 9;
    state->payload_keyidR = 10;
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;

    uint32_t scrambler = 0x0001C0DEU;
    ui_post_cmd(UI_CMD_KEY_SCRAMBLER_SET, &scrambler, sizeof scrambler);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("scrambler key command is drained", applied, 1);
    rc |= expect_int_eq("scrambler key command sets R", (int)state->R, (int)scrambler);
    rc |= expect_int_eq("scrambler key clears keyloader", state->keyloader, 0);
    rc |= expect_int_eq("scrambler key unmutes encrypted left", opts->dmr_mute_encL, 0);
    rc |= expect_int_eq("scrambler key unmutes encrypted right", opts->dmr_mute_encR, 0);

    DSD_MEMSET(state->static_ks_bits, 0, sizeof state->static_ks_bits);
    state->ken_sc = 0;
    ui_post_cmd(UI_CMD_KEY_KEN_SCR_SET, "1", sizeof "1");
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("Kenwood stream key command is drained", applied, 1);
    rc |= expect_int_eq("Kenwood stream key enables forced scrambler", state->ken_sc, 1);
    rc |=
        expect_int_eq("Kenwood stream key seeds slot 0 bits", static_bits_to_u16(state->static_ks_bits[0]) >> 8U, 0x80);
    rc |=
        expect_int_eq("Kenwood stream key seeds slot 1 bits", static_bits_to_u16(state->static_ks_bits[1]) >> 8U, 0x80);

    DSD_MEMSET(state->static_ks_bits, 0, sizeof state->static_ks_bits);
    state->any_bp = 0;
    ui_post_cmd(UI_CMD_KEY_ANYTONE_BP_SET, "2345", sizeof "2345");
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("Anytone BP stream key command is drained", applied, 1);
    rc |= expect_int_eq("Anytone BP stream key enables forced mode", state->any_bp, 1);
    rc |= expect_int_eq("Anytone BP stream key permutes slot 0 bits", static_bits_to_u16(state->static_ks_bits[0]),
                        0xDBBD);
    rc |= expect_int_eq("Anytone BP stream key permutes slot 1 bits", static_bits_to_u16(state->static_ks_bits[1]),
                        0xDBBD);

    state->keyloader = 1;
    state->payload_keyid = 11;
    state->payload_keyidR = 12;
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;

    uint64_t rc4des = 0x0123456789ABCDEFULL;
    ui_post_cmd(UI_CMD_KEY_RC4DES_SET, &rc4des, sizeof rc4des);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("RC4/DES key command is drained", applied, 1);
    rc |= expect_u64_eq("RC4/DES key command sets R", state->R, rc4des);
    rc |= expect_u64_eq("RC4/DES key command sets RR", state->RR, rc4des);
    rc |= expect_int_eq("RC4/DES key clears keyloader", state->keyloader, 0);
    rc |= expect_int_eq("RC4/DES key clears left payload key ID", state->payload_keyid, 0);
    rc |= expect_int_eq("RC4/DES key clears right payload key ID", state->payload_keyidR, 0);
    rc |= expect_int_eq("RC4/DES key unmutes encrypted left", opts->dmr_mute_encL, 0);
    rc |= expect_int_eq("RC4/DES key unmutes encrypted right", opts->dmr_mute_encR, 0);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_m17_user_data_command_truncates_to_state_buffer(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_state* state = runtime.state;

    DSD_SNPRINTF(state->m17dat, sizeof state->m17dat, "%s", "stale");
    char payload[128];
    DSD_MEMSET(payload, 'M', sizeof payload);

    ui_post_cmd(UI_CMD_M17_USER_DATA_SET, payload, sizeof payload);
    int applied = ui_drain_cmds(runtime.opts, state);

    int rc = 0;
    rc |= expect_int_eq("M17 user data command is drained", applied, 1);
    rc |= expect_int_eq("M17 user data is field bounded", (int)strlen(state->m17dat), (int)sizeof(state->m17dat) - 1);
    rc |= expect_true("M17 user data preserves copied payload prefix",
                      strspn(state->m17dat, "M") == strlen(state->m17dat));
    rc |= expect_int_eq("M17 user data is NUL terminated", state->m17dat[sizeof(state->m17dat) - 1], '\0');

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_runtime_toggle_commands_dispatch_through_queue(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    opts->dmr_le = 0;
    opts->unmute_encrypted_p25 = 0;
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 0;
    opts->inverted_x2tdma = 0;
    opts->inverted_dmr = 1;
    opts->inverted_dpmr = 0;
    opts->inverted_m17 = 1;

    ui_post_cmd(UI_CMD_DMR_LE_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_ALL_MUTES_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_INV_X2_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_INV_DMR_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_INV_DPMR_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_INV_M17_TOGGLE, NULL, 0);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("runtime toggle drain count", applied, 6);
    rc |= expect_int_eq("DMR LE toggled through queue", opts->dmr_le, 1);
    rc |= expect_int_eq("P25 encrypted unmute toggled through queue", opts->unmute_encrypted_p25, 1);
    rc |= expect_int_eq("DMR left mute toggled through queue", opts->dmr_mute_encL, 0);
    rc |= expect_int_eq("DMR right mute toggled through queue", opts->dmr_mute_encR, 1);
    rc |= expect_int_eq("X2 inversion toggled through queue", opts->inverted_x2tdma, 1);
    rc |= expect_int_eq("DMR inversion toggled through queue", opts->inverted_dmr, 0);
    rc |= expect_int_eq("dPMR inversion toggled through queue", opts->inverted_dpmr, 1);
    rc |= expect_int_eq("M17 inversion toggled through queue", opts->inverted_m17, 0);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_legacy_toggle_commands_dispatch_through_queue(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    opts->aggressive_framesync = 0;
    opts->p25_lcw_retune = 1;
    opts->p25_prefer_candidates = 0;
    opts->reverse_mute = 1;
    opts->use_lpf = 0;
    opts->use_hpf = 1;
    opts->use_pbf = 0;
    opts->use_hpf_d = 1;
    opts->payload = 0;

    ui_post_cmd(UI_CMD_CRC_RELAX_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_AGGR_SYNC_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_LCW_RETUNE_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_P25_CC_CAND_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_REVERSE_MUTE_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_LPF_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_HPF_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_PBF_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_HPF_D_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_PAYLOAD_TOGGLE, NULL, 0);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("legacy toggle drain count", applied, 10);
    rc |= expect_int_eq("CRC relax then aggr-sync aliases toggle twice", opts->aggressive_framesync, 0);
    rc |= expect_int_eq("LCW retune toggled through queue", opts->p25_lcw_retune, 0);
    rc |= expect_int_eq("P25 CC candidates toggled through queue", opts->p25_prefer_candidates, 1);
    rc |= expect_int_eq("reverse mute toggled through queue", opts->reverse_mute, 0);
    rc |= expect_int_eq("LPF toggled through queue", opts->use_lpf, 1);
    rc |= expect_int_eq("HPF toggled through queue", opts->use_hpf, 0);
    rc |= expect_int_eq("PBF toggled through queue", opts->use_pbf, 1);
    rc |= expect_int_eq("HPF-D toggled through queue", opts->use_hpf_d, 0);
    rc |= expect_int_eq("payload display toggled through queue", opts->payload, 1);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_dsp_op_commands_dispatch_through_queue(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;
    int rc = 0;

    rtl_stream_toggle_cqpsk(0);
    UiDspPayload dsp = {.op = UI_DSP_OP_TOGGLE_CQ};
    ui_post_cmd(UI_CMD_DSP_OP, &dsp, sizeof dsp);
    int applied = ui_drain_cmds(opts, state);
    int cq = 0;
    rtl_stream_get_cqpsk_status(&cq, NULL);
    rc |= expect_int_eq("DSP CQPSK toggle command drains", applied, 1);
    rc |= expect_int_eq("DSP CQPSK toggle enables mode", cq, 1);

    rtl_stream_toggle_iq_balance(0);
    (void)dsd_unsetenv("DSD_NEO_IQ_BALANCE");
    dsp.op = UI_DSP_OP_TOGGLE_IQBAL;
    ui_post_cmd(UI_CMD_DSP_OP, &dsp, sizeof dsp);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("DSP IQ balance command drains", applied, 1);
    rc |= expect_int_eq("DSP IQ balance toggles on", rtl_stream_get_iq_balance(), 1);
    rc |= expect_true("DSP IQ balance publishes env", strcmp(dsd_neo_env_get("DSD_NEO_IQ_BALANCE"), "1") == 0);

    rtl_stream_set_iq_dc(0, 4);
    int dc_shift = 0;
    (void)rtl_stream_get_iq_dc(&dc_shift);
    int dc_shift_before_toggle = dc_shift;
    (void)dsd_unsetenv("DSD_NEO_IQ_DC_BLOCK");
    dsp.op = UI_DSP_OP_IQ_DC_TOGGLE;
    ui_post_cmd(UI_CMD_DSP_OP, &dsp, sizeof dsp);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("DSP IQ DC command drains", applied, 1);
    rc |= expect_int_eq("DSP IQ DC toggles on", rtl_stream_get_iq_dc(&dc_shift), 1);
    rc |= expect_int_eq("DSP IQ DC toggle preserves shift", dc_shift, dc_shift_before_toggle);
    rc |= expect_true("DSP IQ DC publishes env", strcmp(dsd_neo_env_get("DSD_NEO_IQ_DC_BLOCK"), "1") == 0);

    dsp.op = UI_DSP_OP_IQ_DC_K_DELTA;
    dsp.a = 3;
    ui_post_cmd(UI_CMD_DSP_OP, &dsp, sizeof dsp);
    applied = ui_drain_cmds(opts, state);
    (void)rtl_stream_get_iq_dc(&dc_shift);
    rc |= expect_int_eq("DSP IQ DC shift command drains", applied, 1);
    rc |= expect_int_eq("DSP IQ DC shift applies delta", dc_shift, dc_shift_before_toggle + 3);

    rtl_stream_set_ted_gain(0.1f);
    dsp.op = UI_DSP_OP_TED_GAIN_SET;
    dsp.a = 999;
    ui_post_cmd(UI_CMD_DSP_OP, &dsp, sizeof dsp);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("DSP TED gain command drains", applied, 1);
    rc |= expect_float_near("DSP TED gain command clamps high", rtl_stream_get_ted_gain(), 0.5f, 0.0001f);

    rtl_stream_set_tuner_autogain(0);
    dsp.op = UI_DSP_OP_TUNER_AUTOGAIN_TOGGLE;
    ui_post_cmd(UI_CMD_DSP_OP, &dsp, sizeof dsp);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("DSP tuner autogain command drains", applied, 1);
    rc |= expect_int_eq("DSP tuner autogain toggles on", rtl_stream_get_tuner_autogain(), 1);

    rtl_stream_toggle_iq_balance(1);
    uint8_t short_payload[1] = {0U};
    ui_post_cmd(UI_CMD_DSP_OP, short_payload, sizeof short_payload);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("short DSP command still drains", applied, 1);
    rc |= expect_int_eq("short DSP command preserves IQ balance", rtl_stream_get_iq_balance(), 1);

    (void)dsd_unsetenv("DSD_NEO_IQ_BALANCE");
    (void)dsd_unsetenv("DSD_NEO_IQ_DC_BLOCK");
    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_legacy_slot_and_display_commands_update_state(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    for (int i = 0; i < 100; ++i) {
        state->audio_out_float_buf[i] = 1.0f;
        state->audio_out_buf[i] = 123;
        state->audio_out_float_bufR[i] = 2.0f;
        state->audio_out_bufR[i] = 456;
    }
    state->audio_out_idx = 77;
    state->audio_out_idx2 = 88;
    state->audio_out_idxR = 55;
    state->audio_out_idx2R = 66;
    opts->slot1_on = 1;
    opts->slot2_on = 1;
    opts->slot_preference = 0;

    ui_post_cmd(UI_CMD_SLOT1_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_SLOT2_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_SLOT_PREF_CYCLE, NULL, 0);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("slot command drain count", applied, 3);
    rc |= expect_int_eq("slot1 toggled off", opts->slot1_on, 0);
    rc |= expect_int_eq("slot2 toggled off", opts->slot2_on, 0);
    rc |= expect_int_eq("slot preference cycles after slot toggles", opts->slot_preference, 1);
    rc |= expect_true("slot1 float cursor moves to flushed tail",
                      state->audio_out_float_buf_p == state->audio_out_float_buf + 100);
    rc |= expect_true("slot1 pcm cursor moves to flushed tail", state->audio_out_buf_p == state->audio_out_buf + 100);
    rc |= expect_true("slot2 float cursor moves to flushed tail",
                      state->audio_out_float_buf_pR == state->audio_out_float_bufR + 100);
    rc |= expect_true("slot2 pcm cursor moves to flushed tail", state->audio_out_buf_pR == state->audio_out_bufR + 100);
    rc |= expect_int_eq("slot1 float index reset", state->audio_out_idx2, 0);
    rc |= expect_int_eq("slot1 pcm index reset", state->audio_out_idx, 0);
    rc |= expect_int_eq("slot2 float index reset", state->audio_out_idx2R, 0);
    rc |= expect_int_eq("slot2 pcm index reset", state->audio_out_idxR, 0);
    rc |= expect_true("slot1 audio buffer cleared",
                      state->audio_out_float_buf[0] == 0.0f && state->audio_out_buf[0] == 0);
    rc |= expect_true("slot2 audio buffer cleared",
                      state->audio_out_float_bufR[0] == 0.0f && state->audio_out_bufR[0] == 0);

    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->constellation = 0;
    ui_post_cmd(UI_CMD_CONST_TOGGLE, NULL, 0);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("non-RTL constellation command drains", applied, 1);
    rc |= expect_int_eq("non-RTL constellation remains disabled", opts->constellation, 0);

    opts->audio_in_type = AUDIO_IN_RTL;
    opts->mod_qpsk = 1;
    opts->const_gate_qpsk = 0.80f;
    opts->eye_view = 0;
    opts->eye_unicode = 0;
    opts->eye_color = 0;
    opts->fsk_hist_view = 0;
    opts->spectrum_view = 0;
    float delta = 0.25f;
    ui_post_cmd(UI_CMD_CONST_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_CONST_NORM_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_CONST_GATE_DELTA, &delta, sizeof delta);
    ui_post_cmd(UI_CMD_EYE_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_FSK_HIST_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_SPECTRUM_TOGGLE, NULL, 0);
    applied = ui_drain_cmds(opts, state);

    rc |= expect_int_eq("RTL display command drain count", applied, 6);
    rc |= expect_int_eq("RTL constellation toggled on", opts->constellation, 1);
    rc |= expect_int_eq("constellation normalization toggled", opts->const_norm_mode, 1);
    rc |= expect_true("constellation gate clamps high", fabsf(opts->const_gate_qpsk - 0.90f) <= 1e-6f);
    rc |= expect_int_eq("eye view toggled on", opts->eye_view, 1);
    rc |= expect_int_eq("FSK histogram toggled on", opts->fsk_hist_view, 1);
    rc |= expect_int_eq("spectrum view toggled on", opts->spectrum_view, 1);

    (void)rtl_stream_spectrum_set_size(128);
    int32_t spectrum_delta = 70;
    ui_post_cmd(UI_CMD_SPEC_SIZE_DELTA, &spectrum_delta, sizeof spectrum_delta);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("spectrum size command drains", applied, 1);
    rc |= expect_int_eq("spectrum size delta rounds to next power of two", rtl_stream_spectrum_get_size(), 256);

    ui_post_cmd(UI_CMD_EYE_UNICODE_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_EYE_COLOR_TOGGLE, NULL, 0);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("eye format command drain count", applied, 2);
    rc |= expect_int_eq("eye unicode toggled on", opts->eye_unicode, 1);
    rc |= expect_int_eq("eye color toggled on", opts->eye_color, 1);

    (void)rtl_stream_spectrum_set_size(64);
    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_legacy_protocol_reset_and_mode_toggles(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    opts->dmr_dmrla_is_set = 1;
    opts->dmr_dmrla_n = 9;
    state->dmr_rest_channel = 4;
    state->dmr_mfid = 0x68;
    DSD_SNPRINTF(state->dmr_branding, sizeof state->dmr_branding, "%s", "brand");
    DSD_SNPRINTF(state->nxdn_location_category, sizeof state->nxdn_location_category, "%s", "cat");
    state->nxdn_last_ran = 5;
    state->nxdn_ran = 6;
    state->nxdn_rcn = 7;
    state->nxdn_base_freq = 200;
    state->nxdn_step = 25;
    state->nxdn_bw = 12;

    opts->m17encoder = 1;
    state->m17encoder_tx = 1;
    state->m17encoder_eot = 0;
    opts->frame_provoice = 1;
    state->esk_mask = 0;
    state->ea_mode = 0;
    state->edacs_site_id = 3;
    state->edacs_lcn_count = 5;
    state->edacs_cc_lcn = 2;
    state->edacs_vc_lcn = 4;
    state->edacs_tuned_lcn = 6;
    state->edacs_vc_call_type = 1;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 852000000;
    opts->p25_is_tuned = 1;
    state->lasttg = 123;
    state->lastsrc = 456;

    ui_post_cmd(UI_CMD_DMR_RESET, NULL, 0);
    ui_post_cmd(UI_CMD_M17_TX_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_PROVOICE_ESK_TOGGLE, NULL, 0);
    ui_post_cmd(UI_CMD_PROVOICE_MODE_TOGGLE, NULL, 0);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("protocol reset command drain count", applied, 4);
    rc |= expect_int_eq("DMR rest channel reset", state->dmr_rest_channel, -1);
    rc |= expect_int_eq("DMR MFID reset", state->dmr_mfid, -1);
    rc |= expect_true("DMR branding reset", state->dmr_branding[0] == '\0');
    rc |= expect_int_eq("DMR LA state reset", opts->dmr_dmrla_is_set, 0);
    rc |= expect_int_eq("DMR LA count reset", opts->dmr_dmrla_n, 0);
    rc |= expect_true("NXDN category reset", strcmp(state->nxdn_location_category, " ") == 0);
    rc |= expect_int_eq("NXDN last RAN reset", state->nxdn_last_ran, -1);
    rc |= expect_int_eq("NXDN RAN reset", state->nxdn_ran, 0);
    rc |= expect_int_eq("NXDN RCN reset", state->nxdn_rcn, 0);
    rc |= expect_int_eq("NXDN base reset", state->nxdn_base_freq, 0);
    rc |= expect_int_eq("NXDN step reset", state->nxdn_step, 0);
    rc |= expect_int_eq("NXDN bandwidth reset", state->nxdn_bw, 0);
    rc |= expect_int_eq("M17 TX toggled off", state->m17encoder_tx, 0);
    rc |= expect_int_eq("M17 EOT marked", state->m17encoder_eot, 1);
    rc |= expect_int_eq("ProVoice ESK enabled", state->esk_mask, 0xA0);
    rc |= expect_int_eq("ProVoice EA mode toggled", state->ea_mode, 1);
    rc |= expect_int_eq("ProVoice site reset", state->edacs_site_id, 0);
    rc |= expect_int_eq("ProVoice tuned LCN reset", state->edacs_tuned_lcn, -1);
    rc |= expect_int_eq("ProVoice P25 CC reset", (int)state->p25_cc_freq, 0);
    rc |= expect_int_eq("ProVoice trunk CC reset", (int)state->trunk_cc_freq, 0);
    rc |= expect_int_eq("ProVoice tuned flag reset", opts->p25_is_tuned, 0);
    rc |= expect_int_eq("ProVoice last TG reset", state->lasttg, 0);
    rc |= expect_int_eq("ProVoice last source reset", state->lastsrc, 0);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_replay_and_stop_playback_manage_symbol_state(void) {
    char sym_path[DSD_TEST_PATH_MAX] = {0};
    int fd = dsd_test_mkstemp(sym_path, sizeof sym_path, "dsdneo_ui_replay_symbol");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: temp symbol replay file\n");
        return 1;
    }
    const uint8_t bytes[] = {0x01U, 0x02U, 0x03U};
    ssize_t nwritten = write(fd, bytes, sizeof bytes);
    (void)dsd_close(fd);
    if (nwritten != (ssize_t)sizeof bytes) {
        DSD_FPRINTF(stderr, "FAIL: write temp symbol replay file\n");
        (void)remove(sym_path);
        return 1;
    }

    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        (void)remove(sym_path);
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", sym_path);
    opts->audio_in_type = AUDIO_IN_PULSE;
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_SOFT;
    state->symbol_replay_header_checked = 1;
    state->symbol_replay_has_soft = 1;

    ui_post_cmd(UI_CMD_REPLAY_LAST, NULL, 0);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("replay last command drains", applied, 1);
    rc |= expect_int_eq("replay last switches to symbol bin", opts->audio_in_type, AUDIO_IN_SYMBOL_BIN);
    rc |= expect_true("replay last opens symbol file", opts->symbolfile != NULL);
    rc |= expect_int_eq("replay last resets format", state->symbol_replay_format, DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN);
    rc |= expect_int_eq("replay last clears header checked", state->symbol_replay_header_checked, 0);
    rc |= expect_int_eq("replay last clears soft flag", state->symbol_replay_has_soft, 0);

    opts->audio_out_type = 1;
    ui_post_cmd(UI_CMD_STOP_PLAYBACK, NULL, 0);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("stop playback command drains", applied, 1);
    rc |= expect_true("stop playback closes symbol file", opts->symbolfile == NULL);
    rc |= expect_int_eq("stop playback falls back to stdin without Pulse output", opts->audio_in_type, AUDIO_IN_STDIN);

    free_test_runtime(&runtime);
    (void)remove(sym_path);
    return rc;
}

static int
test_ui_io_command_queue_applies_local_input_and_network_payloads(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    struct HostPortPayload {
        char host[256];
        int32_t port;
    };

    struct HostPortPayload tcp = {0};
    DSD_SNPRINTF(tcp.host, sizeof tcp.host, "%s", "example.invalid");
    tcp.port = 7355;
    reset_tcp_connect_audio_fake();
    ui_post_cmd(UI_CMD_TCP_CONNECT_AUDIO_CFG, &tcp, sizeof tcp);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("TCP command drain count", applied, 1);
    rc |= expect_int_eq("TCP command calls connector", g_tcp_connect_audio_calls, 1);
    rc |= expect_true("TCP command passes host", strcmp(g_last_tcp_connect_audio_host, "example.invalid") == 0);
    rc |= expect_int_eq("TCP command passes port", g_last_tcp_connect_audio_port, 7355);
    rc |= expect_int_eq("TCP command switches input type on success", opts->audio_in_type, AUDIO_IN_TCP);
    rc |= expect_true("TCP command stores host on success", strcmp(opts->tcp_hostname, "example.invalid") == 0);
    rc |= expect_true("TCP command writes success toast", strstr(state->ui_msg, "TCP audio connected") != NULL);

    struct HostPortPayload tcp_fail = {0};
    DSD_SNPRINTF(tcp_fail.host, sizeof tcp_fail.host, "%s", "fail.invalid");
    tcp_fail.port = 1234;
    g_tcp_connect_audio_result = -1;
    ui_post_cmd(UI_CMD_TCP_CONNECT_AUDIO_CFG, &tcp_fail, sizeof tcp_fail);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("TCP failure command drain count", applied, 1);
    rc |= expect_int_eq("TCP failure still calls connector", g_tcp_connect_audio_calls, 2);
    rc |=
        expect_true("TCP failure does not replace connected host", strcmp(opts->tcp_hostname, "example.invalid") == 0);
    rc |= expect_true("TCP failure writes failure toast", strstr(state->ui_msg, "TCP audio connect failed") != NULL);

    struct HostPortPayload udp = {0};
    udp.port = 45000;
    ui_post_cmd(UI_CMD_UDP_INPUT_CFG, &udp, sizeof udp);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("UDP input command drain count", applied, 1);
    rc |= expect_int_eq("UDP input switches input type", opts->audio_in_type, AUDIO_IN_UDP);
    rc |= expect_int_eq("UDP input stores port", opts->udp_in_portno, 45000);
    rc |= expect_true("UDP input allows empty bind address", opts->udp_in_bindaddr[0] == '\0');
    rc |= expect_true("UDP input uses logical device name", strcmp(opts->audio_in_dev, "udp") == 0);
    rc |= expect_true("UDP input toast uses loopback fallback", strstr(state->ui_msg, "127.0.0.1:45000") != NULL);

    const char wav_path[] = "/tmp/dsdneo-input.wav";
    ui_post_cmd(UI_CMD_INPUT_WAV_SET, wav_path, sizeof wav_path);
    ui_post_cmd(UI_CMD_INPUT_SYM_STREAM_SET, "capture.sym", sizeof "capture.sym");
    ui_post_cmd(UI_CMD_INPUT_SET_PULSE, NULL, 0);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("local input command drain count", applied, 3);
    rc |= expect_int_eq("Pulse input command wins final type", opts->audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_true("Pulse input command stores logical device", strcmp(opts->audio_in_dev, "pulse") == 0);
    rc |= expect_true("Pulse input command writes toast", strstr(state->ui_msg, "Input switched to Pulse") != NULL);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_file_open_commands_report_service_results(void) {
    char static_path[DSD_TEST_PATH_MAX] = {0};
    char raw_path[DSD_TEST_PATH_MAX] = {0};
    char sym_out_path[DSD_TEST_PATH_MAX] = {0};
    char sym_in_path[DSD_TEST_PATH_MAX] = {0};
    if (create_removed_temp_path("dsdneo_queue_static_wav", static_path, sizeof static_path) != 0
        || create_removed_temp_path("dsdneo_queue_raw_wav", raw_path, sizeof raw_path) != 0
        || create_removed_temp_path("dsdneo_queue_symbol_out", sym_out_path, sizeof sym_out_path) != 0) {
        return 1;
    }

    int fd = dsd_test_mkstemp(sym_in_path, sizeof sym_in_path, "dsdneo_queue_symbol_in");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_test_mkstemp failed for symbol input\n");
        return 1;
    }
    (void)dsd_close(fd);
    FILE* sym_fp = dsd_fopen_private(sym_in_path, "wb");
    if (!sym_fp) {
        DSD_FPRINTF(stderr, "FAIL: fopen write failed for %s\n", sym_in_path);
        (void)remove(sym_in_path);
        return 1;
    }
    (void)fputs("01230123", sym_fp);
    fclose(sym_fp);

    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        (void)remove(sym_in_path);
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;
    int rc = 0;

    ui_post_cmd(UI_CMD_WAV_STATIC_OPEN, static_path, strlen(static_path) + 1);
    int applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("static WAV command drains", applied, 1);
    rc |= expect_true("static WAV opens SNDFILE", opts->wav_out_f != NULL);
    rc |= expect_true("static WAV stores requested path", strcmp(opts->wav_out_file, static_path) == 0);
    rc |= expect_int_eq("static WAV sets mono/static flag", opts->static_wav_file, 1);
    rc |= expect_int_eq("static WAV clears stereo WAV mode", opts->dmr_stereo_wav, 0);
    rc |= expect_true("static WAV writes toast", strstr(state->ui_msg, "Static WAV output") != NULL);
    if (opts->wav_out_f) {
        sf_close(opts->wav_out_f);
        opts->wav_out_f = NULL;
    }

    ui_post_cmd(UI_CMD_WAV_RAW_OPEN, raw_path, strlen(raw_path) + 1);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("raw WAV command drains", applied, 1);
    rc |= expect_true("raw WAV opens SNDFILE", opts->wav_out_raw != NULL);
    rc |= expect_true("raw WAV stores requested path", strcmp(opts->wav_out_file_raw, raw_path) == 0);
    rc |= expect_true("raw WAV writes toast", strstr(state->ui_msg, "Raw WAV output") != NULL);
    if (opts->wav_out_raw) {
        sf_close(opts->wav_out_raw);
        opts->wav_out_raw = NULL;
    }

    ui_post_cmd(UI_CMD_SYMCAP_OPEN, sym_out_path, strlen(sym_out_path) + 1);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("symbol capture command drains", applied, 1);
    rc |= expect_true("symbol capture opens file", opts->symbol_out_f != NULL);
    rc |= expect_true("symbol capture stores requested path", strcmp(opts->symbol_out_file, sym_out_path) == 0);
    rc |= expect_true("symbol capture writes toast", strstr(state->ui_msg, "Symbol capture") != NULL);
    if (opts->symbol_out_f) {
        fclose(opts->symbol_out_f);
        opts->symbol_out_f = NULL;
    }

    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_SOFT;
    state->symbol_replay_header_checked = 1;
    state->symbol_replay_has_soft = 1;
    opts->audio_in_type = AUDIO_IN_PULSE;
    ui_post_cmd(UI_CMD_SYMBOL_IN_OPEN, sym_in_path, strlen(sym_in_path) + 1);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("symbol input command drains", applied, 1);
    rc |= expect_true("symbol input opens file", opts->symbolfile != NULL);
    rc |= expect_int_eq("symbol input switches input type", opts->audio_in_type, AUDIO_IN_SYMBOL_BIN);
    rc |= expect_true("symbol input stores requested path", strcmp(opts->audio_in_dev, sym_in_path) == 0);
    rc |= expect_int_eq("symbol input resets replay format", state->symbol_replay_format,
                        DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN);
    rc |= expect_int_eq("symbol input clears header checked", state->symbol_replay_header_checked, 0);
    rc |= expect_int_eq("symbol input clears soft flag", state->symbol_replay_has_soft, 0);
    rc |= expect_true("symbol input writes toast", strstr(state->ui_msg, "Symbol input") != NULL);
    if (opts->symbolfile) {
        fclose(opts->symbolfile);
        opts->symbolfile = NULL;
    }

    free_test_runtime(&runtime);
    (void)remove(static_path);
    (void)remove(raw_path);
    (void)remove(sym_out_path);
    (void)remove(sym_in_path);
    return rc;
}

static int
test_ui_legacy_file_capture_commands_manage_handles(void) {
    char wav_dir[DSD_TEST_PATH_MAX] = {0};
    char sym_path[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(wav_dir, sizeof wav_dir, "dsdneo_queue_wav_dir")
        || create_removed_temp_path("dsdneo_queue_sym_stop", sym_path, sizeof sym_path) != 0) {
        DSD_FPRINTF(stderr, "FAIL: temp capture path setup failed\n");
        return 1;
    }

    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        (void)remove(wav_dir);
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;
    int rc = 0;

    DSD_SNPRINTF(opts->wav_out_dir, sizeof opts->wav_out_dir, "%s", wav_dir);
    ui_post_cmd(UI_CMD_WAV_START, NULL, 0);
    int applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("legacy WAV start command drains", applied, 1);
    rc |= expect_true("legacy WAV start opens left handle", opts->wav_out_f != NULL);
    rc |= expect_true("legacy WAV start opens right handle", opts->wav_out_fR != NULL);
    rc |= expect_int_eq("legacy WAV start enables stereo WAV", opts->dmr_stereo_wav, 1);
    rc |= expect_true("legacy WAV start stores left temp under dir",
                      strstr(opts->wav_out_file, wav_dir) == opts->wav_out_file);
    rc |= expect_true("legacy WAV start stores right temp under dir",
                      strstr(opts->wav_out_fileR, wav_dir) == opts->wav_out_fileR);

    ui_post_cmd(UI_CMD_WAV_STOP, NULL, 0);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("legacy WAV stop command drains", applied, 1);
    rc |= expect_true("legacy WAV stop closes left handle", opts->wav_out_f == NULL);
    rc |= expect_true("legacy WAV stop closes right handle", opts->wav_out_fR == NULL);
    rc |= expect_true("legacy WAV stop clears left filename", opts->wav_out_file[0] == '\0');
    rc |= expect_true("legacy WAV stop clears right filename", opts->wav_out_fileR[0] == '\0');
    rc |= expect_int_eq("legacy WAV stop disables stereo WAV", opts->dmr_stereo_wav, 0);

    FILE* sym_fp = dsd_fopen_private(sym_path, "wb");
    if (!sym_fp) {
        DSD_FPRINTF(stderr, "FAIL: symbol capture stop setup failed for %s\n", sym_path);
        rc |= 1;
    } else {
        opts->symbol_out_f = sym_fp;
        DSD_SNPRINTF(opts->symbol_out_file, sizeof opts->symbol_out_file, "%s", sym_path);
        opts->symbol_out_file_is_auto = 1;
        ui_post_cmd(UI_CMD_SYMCAP_STOP, NULL, 0);
        applied = ui_drain_cmds(opts, state);
        rc |= expect_int_eq("symbol capture stop command drains", applied, 1);
        rc |= expect_true("symbol capture stop closes handle", opts->symbol_out_f == NULL);
        rc |= expect_true("symbol capture stop stages replay input path", strcmp(opts->audio_in_dev, sym_path) == 0);
        rc |= expect_int_eq("symbol capture stop clears auto flag", opts->symbol_out_file_is_auto, 0);
    }

    free_test_runtime(&runtime);
    (void)remove(sym_path);
    (void)remove(wav_dir);
    return rc;
}

static int
test_ui_import_and_dsp_output_commands_report_service_results(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;
    int rc = 0;

    ui_post_cmd(UI_CMD_DSP_OUT_SET, "queue-dsp.bin", sizeof "queue-dsp.bin");
    int applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("DSP output command drains", applied, 1);
    rc |= expect_int_eq("DSP output enables file sink", opts->use_dsp_output, 1);
    rc |= expect_true("DSP output stores generated path", strstr(opts->dsp_out_file, "DSP/queue-dsp.bin") != NULL);
    rc |= expect_true("DSP output writes success toast", strstr(state->ui_msg, "Applied: DSP output") != NULL);

    const char* missing_chan = "./missing-channel-map.csv";
    ui_post_cmd(UI_CMD_IMPORT_CHANNEL_MAP, missing_chan, strlen(missing_chan) + 1U);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("channel import command drains", applied, 1);
    rc |= expect_true("channel import records requested path", strcmp(opts->chan_in_file, missing_chan) == 0);
    rc |= expect_true("channel import failure toast", strstr(state->ui_msg, "Failed: Channel map import") != NULL);

    const char* missing_group = "./missing-group-list.csv";
    ui_post_cmd(UI_CMD_IMPORT_GROUP_LIST, missing_group, strlen(missing_group) + 1U);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("group import command drains", applied, 1);
    rc |= expect_true("group import records requested path", strcmp(opts->group_in_file, missing_group) == 0);
    rc |= expect_true("group import failure toast", strstr(state->ui_msg, "Failed: Group list reload") != NULL);

    const char* missing_dec = "./missing-keys-dec.csv";
    ui_post_cmd(UI_CMD_IMPORT_KEYS_DEC, missing_dec, strlen(missing_dec) + 1U);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("decimal keys import command drains", applied, 1);
    rc |= expect_true("decimal keys import records requested path", strcmp(opts->key_in_file, missing_dec) == 0);
    rc |= expect_true("decimal keys import failure toast", strstr(state->ui_msg, "Failed: Keys (DEC) import") != NULL);

    const char* missing_hex = "./missing-keys-hex.csv";
    ui_post_cmd(UI_CMD_IMPORT_KEYS_HEX, missing_hex, strlen(missing_hex) + 1U);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("hex keys import command drains", applied, 1);
    rc |= expect_true("hex keys import records requested path", strcmp(opts->key_in_file, missing_hex) == 0);
    rc |= expect_true("hex keys import failure toast", strstr(state->ui_msg, "Failed: Keys (HEX) import") != NULL);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_malformed_payload_commands_drain_without_mutation(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    struct ShortHostPortPayload {
        char host[256];
    } short_tcp = {{0}};

    DSD_SNPRINTF(short_tcp.host, sizeof short_tcp.host, "%s", "short.invalid");

    DSD_SNPRINTF(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", "old.invalid");
    opts->tcp_portno = 31337;
    opts->audio_in_type = AUDIO_IN_PULSE;
    DSD_SNPRINTF(state->ui_msg, sizeof state->ui_msg, "%s", "sentinel");
    reset_tcp_connect_audio_fake();

    ui_post_cmd(UI_CMD_TCP_CONNECT_AUDIO_CFG, &short_tcp, sizeof short_tcp);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("short TCP payload command is drained", applied, 1);
    rc |= expect_int_eq("short TCP payload does not call connector", g_tcp_connect_audio_calls, 0);
    rc |= expect_true("short TCP payload preserves host", strcmp(opts->tcp_hostname, "old.invalid") == 0);
    rc |= expect_int_eq("short TCP payload preserves port", opts->tcp_portno, 31337);
    rc |= expect_int_eq("short TCP payload preserves input type", opts->audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_true("short TCP payload preserves toast", strcmp(state->ui_msg, "sentinel") == 0);

    uint8_t short_scalar = 0x7FU;
    opts->setmod_bw = 12500;
    state->tg_hold = 4242U;
    opts->trunk_hangtime = 7.25f;
    opts->slot_preference = 1;
    opts->slot1_on = 1;
    opts->slot2_on = 0;
    ui_post_cmd(UI_CMD_RIGCTL_SET_MOD_BW, &short_scalar, sizeof short_scalar);
    ui_post_cmd(UI_CMD_TG_HOLD_SET, &short_scalar, sizeof short_scalar);
    ui_post_cmd(UI_CMD_HANGTIME_SET, &short_scalar, sizeof short_scalar);
    ui_post_cmd(UI_CMD_SLOT_PREF_SET, &short_scalar, sizeof short_scalar);
    ui_post_cmd(UI_CMD_SLOTS_ONOFF_SET, &short_scalar, sizeof short_scalar);
    applied = ui_drain_cmds(opts, state);

    rc |= expect_int_eq("short scalar payload commands are drained", applied, 5);
    rc |= expect_int_eq("short rigctl payload preserves bandwidth", opts->setmod_bw, 12500);
    rc |= expect_int_eq("short TG payload preserves hold", (int)state->tg_hold, 4242);
    rc |= expect_true("short hangtime payload preserves value", fabs(opts->trunk_hangtime - 7.25f) <= 1e-6f);
    rc |= expect_int_eq("short slot preference payload preserves value", opts->slot_preference, 1);
    rc |= expect_int_eq("short slot mask payload preserves slot 1", opts->slot1_on, 1);
    rc |= expect_int_eq("short slot mask payload preserves slot 2", opts->slot2_on, 0);

    struct ShortP2Payload {
        uint64_t w;
        uint64_t s;
    } short_p2 = {0xABCDEULL, 0x123ULL};

    state->p2_wacn = 0x11111ULL;
    state->p2_sysid = 0x222ULL;
    state->p2_cc = 0x333ULL;
    state->p2_hardset = 1;
    ui_post_cmd(UI_CMD_P25_P2_PARAMS_SET, &short_p2, sizeof short_p2);
    applied = ui_drain_cmds(opts, state);

    rc |= expect_int_eq("short P25 P2 params command is drained", applied, 1);
    rc |= expect_u64_eq("short P25 P2 params preserves WACN", state->p2_wacn, 0x11111ULL);
    rc |= expect_u64_eq("short P25 P2 params preserves SYSID", state->p2_sysid, 0x222ULL);
    rc |= expect_u64_eq("short P25 P2 params preserves CC", state->p2_cc, 0x333ULL);
    rc |= expect_int_eq("short P25 P2 params preserves hardset", state->p2_hardset, 1);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_runtime_parameter_commands_clamp_and_update_state(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    int32_t mod_bw = 50000;
    uint32_t tg = 123456U;
    double hangtime = -2.5;
    int32_t slot_pref = 9;
    int32_t slot_mask = 2;

    ui_post_cmd(UI_CMD_RIGCTL_SET_MOD_BW, &mod_bw, sizeof mod_bw);
    ui_post_cmd(UI_CMD_TG_HOLD_SET, &tg, sizeof tg);
    ui_post_cmd(UI_CMD_HANGTIME_SET, &hangtime, sizeof hangtime);
    ui_post_cmd(UI_CMD_SLOT_PREF_SET, &slot_pref, sizeof slot_pref);
    ui_post_cmd(UI_CMD_SLOTS_ONOFF_SET, &slot_mask, sizeof slot_mask);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("runtime parameter drain count", applied, 5);
    rc |= expect_int_eq("rigctl bandwidth clamps high", opts->setmod_bw, 25000);
    rc |= expect_int_eq("TG hold updates state", (int)state->tg_hold, (int)tg);
    rc |= expect_true("negative hangtime clamps to zero", fabs(opts->trunk_hangtime) <= 1e-9);
    rc |= expect_int_eq("slot preference clamps high", opts->slot_preference, 1);
    rc |= expect_int_eq("slot mask clears slot 1", opts->slot1_on, 0);
    rc |= expect_int_eq("slot mask enables slot 2", opts->slot2_on, 1);
    rc |= expect_true("last runtime command writes slot mask toast", strstr(state->ui_msg, "Slot mask -> 2") != NULL);

    free_test_runtime(&runtime);
    return rc;
}

static int
test_ui_output_lrrp_and_p25_parameter_commands_dispatch_through_queue(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    ui_post_cmd(UI_CMD_PULSE_OUT_SET, "speaker", sizeof "speaker");
    ui_post_cmd(UI_CMD_PULSE_IN_SET, "microphone", sizeof "microphone");
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("Pulse I/O command drain count", applied, 2);
    rc |= expect_true("Pulse output selects pulse device", strcmp(opts->audio_out_dev, "pulse") == 0);
    rc |= expect_int_eq("Pulse output selects pulse backend", opts->audio_out_type, 0);
    rc |= expect_true("Pulse input selects pulse device", strcmp(opts->audio_in_dev, "pulse") == 0);
    rc |= expect_int_eq("Pulse input selects pulse backend", opts->audio_in_type, AUDIO_IN_PULSE);
    rc |= expect_true("Pulse input writes toast", strstr(state->ui_msg, "Pulse input") != NULL);

    ui_post_cmd(UI_CMD_LRRP_SET_HOME, NULL, 0);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("LRRP home command drain count", applied, 1);
    rc |= expect_int_eq("LRRP home enables output", opts->lrrp_file_output, 1);
    rc |= expect_true("LRRP home stores expanded filename", strstr(opts->lrrp_out_file, "lrrp.txt") != NULL);
    rc |= expect_true("LRRP home writes toast", strstr(state->ui_msg, "LRRP output") != NULL);

    ui_post_cmd(UI_CMD_LRRP_SET_DSDP, NULL, 0);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("LRRP DSDPlus command drain count", applied, 1);
    rc |= expect_int_eq("LRRP DSDPlus keeps output enabled", opts->lrrp_file_output, 1);
    rc |= expect_true("LRRP DSDPlus stores standard filename", strcmp(opts->lrrp_out_file, "DSDPlus.LRRP") == 0);

    const char lrrp_path[] = "custom-lrrp.txt";
    ui_post_cmd(UI_CMD_LRRP_SET_CUSTOM, lrrp_path, sizeof lrrp_path);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("LRRP custom command drain count", applied, 1);
    rc |= expect_int_eq("LRRP custom enables output", opts->lrrp_file_output, 1);
    rc |= expect_true("LRRP custom stores path", strcmp(opts->lrrp_out_file, lrrp_path) == 0);
    rc |= expect_true("LRRP custom writes toast", strstr(state->ui_msg, "LRRP output") != NULL);

    ui_post_cmd(UI_CMD_LRRP_DISABLE, NULL, 0);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("LRRP disable command drain count", applied, 1);
    rc |= expect_int_eq("LRRP disable clears output flag", opts->lrrp_file_output, 0);
    rc |= expect_true("LRRP disable clears path", opts->lrrp_out_file[0] == '\0');

    struct P2Payload {
        uint64_t w;
        uint64_t s;
        uint64_t n;
    } p2 = {0x1FFFFFULL, 0x1FFFULL, 0x1FFFULL};

    ui_post_cmd(UI_CMD_P25_P2_PARAMS_SET, &p2, sizeof p2);
    applied = ui_drain_cmds(opts, state);
    rc |= expect_int_eq("P25 P2 params command drain count", applied, 1);
    rc |= expect_u64_eq("P25 P2 WACN clamps", state->p2_wacn, 0xFFFFFULL);
    rc |= expect_u64_eq("P25 P2 SYSID clamps", state->p2_sysid, 0xFFFULL);
    rc |= expect_u64_eq("P25 P2 CC clamps", state->p2_cc, 0xFFFULL);
    rc |= expect_int_eq("P25 P2 hardset enabled after nonzero params", state->p2_hardset, 1);

    free_test_runtime(&runtime);
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

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
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

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
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
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", wav_path);

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

        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", cases[i].live_dev);
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
        DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", wav_path);

        ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
        ui_drain_cmds(opts, state);

        dsdneoUserConfig snap = {0};
        dsd_snapshot_opts_to_user_config(opts, state, &snap);

        char label[128];
        DSD_SNPRINTF(label, sizeof label, "%s apply keeps live socket input active", cases[i].label);
        rc |= expect_true(label, opts->audio_in_type == cases[i].audio_in_type);
        DSD_SNPRINTF(label, sizeof label, "%s apply preserves live socket rate", cases[i].label);
        rc |= expect_int_eq(label, opts->wav_sample_rate, 48000);
        DSD_SNPRINTF(label, sizeof label, "%s apply stages requested file rate", cases[i].label);
        rc |= expect_int_eq(label, opts->staged_file_sample_rate, 96000);
        DSD_SNPRINTF(label, sizeof label, "%s apply keeps live socket timing", cases[i].label);
        rc |= expect_int_eq(label, dsd_opts_current_input_timing_rate(opts), 48000);
        DSD_SNPRINTF(label, sizeof label, "%s apply preserves socket sps", cases[i].label);
        rc |= expect_int_eq(label, state->samplesPerSymbol, 10);
        DSD_SNPRINTF(label, sizeof label, "%s apply preserves socket center", cases[i].label);
        rc |= expect_int_eq(label, state->symbolCenter, 4);
        DSD_SNPRINTF(label, sizeof label, "%s snapshot keeps staged file source", cases[i].label);
        rc |= expect_true(label, snap.has_input && snap.input_source == DSDCFG_INPUT_FILE);
        DSD_SNPRINTF(label, sizeof label, "%s snapshot keeps staged file rate", cases[i].label);
        rc |= expect_int_eq(label, snap.file_sample_rate, 96000);

        free_test_runtime(&runtime);
    }

    (void)remove(wav_path);
    return rc;
}

static int
test_tcp_hot_restart_failure_rolls_back_requested_spec_and_retries(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    reset_tcp_connect_audio_fake();
    g_tcp_connect_audio_result = -1;

    opts->audio_in_type = AUDIO_IN_TCP;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "tcp:old.example:1200");
    DSD_SNPRINTF(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", "old.example");
    opts->tcp_portno = 1200;

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_TCP;
    DSD_SNPRINTF(cfg.tcp_host, sizeof cfg.tcp_host, "%s", "new.example");
    cfg.tcp_port = 1300;

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(opts, state);

    dsdneoUserConfig snap = {0};
    dsd_snapshot_opts_to_user_config(opts, state, &snap);

    int rc = 0;
    rc |= expect_int_eq("tcp hot restart failure attempts reconnect", g_tcp_connect_audio_calls, 1);
    rc |= expect_true("tcp hot restart failure targets requested host",
                      strcmp(g_last_tcp_connect_audio_host, "new.example") == 0);
    rc |= expect_int_eq("tcp hot restart failure targets requested port", g_last_tcp_connect_audio_port, 1300);
    rc |= expect_true("tcp hot restart failure keeps live device spec",
                      strcmp(opts->audio_in_dev, "tcp:old.example:1200") == 0);
    rc |= expect_true("tcp hot restart failure keeps live host", strcmp(opts->tcp_hostname, "old.example") == 0);
    rc |= expect_int_eq("tcp hot restart failure keeps live port", opts->tcp_portno, 1200);
    rc |= expect_true("tcp hot restart failure snapshot reports live endpoint",
                      snap.has_input && snap.input_source == DSDCFG_INPUT_TCP
                          && strcmp(snap.tcp_host, "old.example") == 0);
    rc |= expect_int_eq("tcp hot restart failure snapshot reports live port", snap.tcp_port, 1200);

    g_tcp_connect_audio_calls = 0;
    g_last_tcp_connect_audio_host[0] = '\0';
    g_last_tcp_connect_audio_port = 0;

    ui_post_cmd(UI_CMD_CONFIG_APPLY, &cfg, sizeof cfg);
    ui_drain_cmds(opts, state);

    rc |= expect_int_eq("tcp hot restart retries after previous failure", g_tcp_connect_audio_calls, 1);
    rc |= expect_true("tcp hot restart retry keeps targeting requested host",
                      strcmp(g_last_tcp_connect_audio_host, "new.example") == 0);
    rc |= expect_int_eq("tcp hot restart retry keeps targeting requested port", g_last_tcp_connect_audio_port, 1300);

    free_test_runtime(&runtime);
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
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", old_path);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts->audio_in_file_info));
    if (opts->audio_in_file_info == NULL) {
        DSD_FPRINTF(stderr, "FAIL: alloc old-rate SF_INFO\n");
        free_test_runtime(&runtime);
        (void)remove(old_path);
        (void)remove(new_path);
        return 1;
    }
    opts->audio_in_file = sf_open(old_path, SFM_READ, opts->audio_in_file_info);
    if (opts->audio_in_file == NULL) {
        DSD_FPRINTF(stderr, "FAIL: sf_open read failed for %s: %s\n", old_path, sf_strerror(NULL));
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
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", new_path);

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
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", path);
    opts->audio_in_type = AUDIO_IN_WAV;

    int active_sample_rate = 0;
    int opened_as_container = 1;
    if (dsd_audio_open_mono_file_input(path, opts->wav_sample_rate, &opts->audio_in_file, &opts->audio_in_file_info,
                                       &active_sample_rate, &opened_as_container)
        != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_audio_open_mono_file_input failed for %s: %s\n", path, sf_strerror(NULL));
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
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", path);

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
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:1000000:10:5:48:0:2");

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "1000000");
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
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:1000000:10:0:48:0:2");

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "1000000");
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
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:1000000:10:9:48:0:2");

    dsdneoUserConfig cfg = {0};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 0;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "1000000");
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

static int
test_ui_rtl_setting_commands_stage_without_live_restart(void) {
    test_runtime runtime;
    if (alloc_test_runtime(&runtime) != 0) {
        return 1;
    }
    dsd_opts* opts = runtime.opts;
    dsd_state* state = runtime.state;

    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->rtl_needs_restart = 0;
    opts->rtl_started = 0;
    opts->rtl_dev_index = 5;
    opts->rtl_gain_value = 3;
    opts->rtl_dsp_bw_khz = 12;
    opts->rtl_volume_multiplier = 2;
    opts->rtl_bias_tee = 0;
    opts->rtltcp_autotune = 0;
    opts->rtl_auto_ppm = 0;

    int32_t dev = -4;
    int32_t gain = 75;
    int32_t ppm = 205;
    int32_t bw = 7;
    double sql_db = -42.0;
    int32_t vol = 8;
    int32_t on = 1;
    ui_post_cmd(UI_CMD_RTL_SET_DEV, &dev, sizeof dev);
    ui_post_cmd(UI_CMD_RTL_SET_GAIN, &gain, sizeof gain);
    ui_post_cmd(UI_CMD_RTL_SET_PPM, &ppm, sizeof ppm);
    ui_post_cmd(UI_CMD_RTL_SET_BW, &bw, sizeof bw);
    ui_post_cmd(UI_CMD_RTL_SET_SQL_DB, &sql_db, sizeof sql_db);
    ui_post_cmd(UI_CMD_RTL_SET_VOL_MULT, &vol, sizeof vol);
    ui_post_cmd(UI_CMD_RTL_SET_BIAS_TEE, &on, sizeof on);
    ui_post_cmd(UI_CMD_RTLTCP_SET_AUTOTUNE, &on, sizeof on);
    ui_post_cmd(UI_CMD_RTL_SET_AUTO_PPM, &on, sizeof on);
    int applied = ui_drain_cmds(opts, state);

    int rc = 0;
    rc |= expect_int_eq("RTL setting commands drain count", applied, 9);
    rc |= expect_int_eq("RTL device command clamps negative index", opts->rtl_dev_index, 0);
    rc |= expect_int_eq("RTL gain command clamps max", opts->rtl_gain_value, 49);
    rc |= expect_int_eq("RTL PPM command clamps max", opts->rtlsdr_ppm_error, 200);
    rc |= expect_int_eq("RTL bandwidth command falls back to supported default", opts->rtl_dsp_bw_khz, 48);
    rc |= expect_true("RTL squelch command stores requested dB",
                      fabs(pwr_to_dB(opts->rtl_squelch_level) - sql_db) < 0.001);
    rc |= expect_int_eq("RTL volume command clamps invalid multiplier", opts->rtl_volume_multiplier, 1);
    rc |= expect_int_eq("RTL bias command stores state without live context", opts->rtl_bias_tee, 1);
    rc |= expect_int_eq("RTL TCP autotune command stores state", opts->rtltcp_autotune, 1);
    rc |= expect_int_eq("RTL auto PPM command stores state", opts->rtl_auto_ppm, 1);
    rc |= expect_int_eq("RTL inactive setting commands do not start stream", opts->rtl_started, 0);
    rc |= expect_true("RTL setting commands leave restart staged", opts->rtl_needs_restart == 1);
    rc |= expect_true("RTL auto PPM command writes final toast", strstr(state->ui_msg, "Auto PPM -> On") != NULL);

    free_test_runtime(&runtime);
    return rc;
}

#endif

int
main(void) {
    int rc = 0;
    rc |= test_basic_pulse_config_apply();
    rc |= test_ui_command_queue_applies_fifo();
    rc |= test_ui_command_queue_overflow_drops_oldest();
    rc |= test_ui_command_queue_truncates_oversized_payload_string();
    rc |= test_ui_profile_selection_applies_overlay_and_disables_autosave();
    rc |= test_ui_profile_menu_no_profiles_does_not_apply_base_config();
    rc |= test_stereo_file_hot_swap_rolls_back_to_live_input();
    rc |= test_call_alert_off_selection_survives_ui_command_path();
    rc |= test_ui_visibility_toggles_preserve_show_keys();
    rc |= test_ui_aes_key_command_clears_manual_hytera_fields();
    rc |= test_ui_aes_key_command_handles_zero_and_short_payloads();
    rc |= test_ui_hytera_status_counts_k_fields_only();
    rc |= test_ui_hytera_key_command_records_segment_variants();
    rc |= test_ui_hytera_key_command_preserves_aes_metadata();
    rc |= test_ui_basic_key_commands_reset_payload_mute_state();
    rc |= test_ui_m17_user_data_command_truncates_to_state_buffer();
    rc |= test_ui_runtime_toggle_commands_dispatch_through_queue();
    rc |= test_ui_legacy_toggle_commands_dispatch_through_queue();
    rc |= test_ui_dsp_op_commands_dispatch_through_queue();
    rc |= test_ui_legacy_slot_and_display_commands_update_state();
    rc |= test_ui_legacy_protocol_reset_and_mode_toggles();
    rc |= test_ui_replay_and_stop_playback_manage_symbol_state();
    rc |= test_ui_io_command_queue_applies_local_input_and_network_payloads();
    rc |= test_ui_file_open_commands_report_service_results();
    rc |= test_ui_legacy_file_capture_commands_manage_handles();
    rc |= test_ui_import_and_dsp_output_commands_report_service_results();
    rc |= test_ui_malformed_payload_commands_drain_without_mutation();
    rc |= test_ui_runtime_parameter_commands_clamp_and_update_state();
    rc |= test_ui_output_lrrp_and_p25_parameter_commands_dispatch_through_queue();
    rc |= test_return_cc_uses_pulse_rate_not_stale_file_rate();
    rc |= test_file_config_apply_keeps_live_pulse_timing();
    rc |= test_file_config_apply_keeps_live_socket_timing();
    rc |= test_tcp_hot_restart_failure_rolls_back_requested_spec_and_retries();
    rc |= test_file_hot_swap_rebuilds_filters_when_header_matches_configured_rate();
    rc |= test_same_path_headerless_wav_reconfig_keeps_requested_raw_rate();
#ifdef USE_RADIO
    rc |= test_same_value_rtl_ppm_retry_is_republished();
    rc |= test_zero_rtl_ppm_apply_updates_live_request();
    rc |= test_omitted_rtl_ppm_apply_preserves_live_request();
    rc |= test_ui_rtl_setting_commands_stage_without_live_restart();
#endif
    return rc ? 1 : 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(performance-type-promotion-in-math-fn)
