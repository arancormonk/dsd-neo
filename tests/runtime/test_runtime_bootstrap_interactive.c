// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifndef DSD_RUNTIME_HAS_TERMINAL_UI
#define DSD_RUNTIME_HAS_TERMINAL_UI 1
#endif

#if DSD_RUNTIME_HAS_TERMINAL_UI
#define EXPECTED_DEFAULT_FRONTEND DSD_FRONTEND_TERMINAL
#else
#define EXPECTED_DEFAULT_FRONTEND DSD_FRONTEND_NONE
#endif

static int g_isatty = 1;
static int g_config_available = 1;
static dsdneoRuntimeConfig g_config;
static int g_config_init_calls;
static dsdneoUserDecodeMode g_last_decode_mode = DSDCFG_MODE_UNSET;
static dsdDecodePresetProfile g_last_decode_profile = DSD_DECODE_PRESET_PROFILE_CONFIG;
static int g_audio_input_calls;
static int g_audio_output_calls;
static int g_chan_import_calls;
static int g_group_import_calls;
static int g_chan_import_rc;
static int g_group_import_rc;

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

int
dsd_isatty(int fd) {
    (void)fd;
    return g_isatty;
}

void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts;
    ++g_config_init_calls;
    g_config_available = 1;
    DSD_MEMSET(&g_config, 0, sizeof g_config);
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return g_config_available ? &g_config : NULL;
}

int
dsd_apply_decode_mode_preset(dsdneoUserDecodeMode mode, dsdDecodePresetProfile profile, dsd_opts* opts,
                             dsd_state* state) {
    (void)opts;
    (void)state;
    g_last_decode_mode = mode;
    g_last_decode_profile = profile;
    return 0;
}

void
dsd_bootstrap_choose_audio_input(dsd_opts* opts) {
    ++g_audio_input_calls;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse:stub-input");
}

void
dsd_bootstrap_choose_audio_output(dsd_opts* opts) {
    ++g_audio_output_calls;
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "pulse:stub-output");
}

int
dsd_stat_path(const char* path, dsd_stat_t* st) {
    if (!path || !st) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(path, "chan.csv") != 0 && strcmp(path, "group.csv") != 0) {
        errno = ENOENT;
        return -1;
    }
    DSD_MEMSET(st, 0, sizeof *st);
    st->st_mode = S_IFREG;
    return 0;
}

int
dsd_stat_is_regular(const dsd_stat_t* st) {
    return st && S_ISREG(st->st_mode);
}

int
csvChanImport(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    ++g_chan_import_calls;
    return g_chan_import_rc;
}

int
csvGroupImport(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    ++g_group_import_calls;
    return g_group_import_rc;
}

static void
reset_harness(void) {
    g_isatty = 1;
    g_config_available = 1;
    DSD_MEMSET(&g_config, 0, sizeof g_config);
    g_config_init_calls = 0;
    g_last_decode_mode = DSDCFG_MODE_UNSET;
    g_last_decode_profile = DSD_DECODE_PRESET_PROFILE_CONFIG;
    g_audio_input_calls = 0;
    g_audio_output_calls = 0;
    g_chan_import_calls = 0;
    g_group_import_calls = 0;
    g_chan_import_rc = 0;
    g_group_import_rc = 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

#if !defined(_WIN32)
typedef void (*bootstrap_interactive_fn)(dsd_opts* opts, dsd_state* state);

static int
with_stdin_text(const char* text, bootstrap_interactive_fn fn, dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    FILE* f = tmpfile();
    if (!f) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    if (fputs(text, f) < 0) {
        DSD_FPRINTF(stderr, "fputs failed\n");
        fclose(f);
        return 1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek(input) failed: %s\n", strerror(errno));
        fclose(f);
        return 1;
    }
    clearerr(f);

    int stdin_fd = fileno(stdin);
    if (stdin_fd < 0) {
        DSD_FPRINTF(stderr, "fileno(stdin) failed: %s\n", strerror(errno));
        fclose(f);
        return 1;
    }
    int input_fd = fileno(f);
    if (input_fd < 0) {
        DSD_FPRINTF(stderr, "fileno(input) failed: %s\n", strerror(errno));
        fclose(f);
        return 1;
    }

    int saved = dup(stdin_fd);
    if (saved < 0) {
        DSD_FPRINTF(stderr, "dup(stdin) failed: %s\n", strerror(errno));
        fclose(f);
        return 1;
    }
    if (fflush(stdin) != 0) {
        DSD_FPRINTF(stderr, "fflush(stdin) failed: %s\n", strerror(errno));
        close(saved);
        fclose(f);
        return 1;
    }
    if (dup2(input_fd, stdin_fd) < 0) {
        DSD_FPRINTF(stderr, "dup2(input, stdin) failed: %s\n", strerror(errno));
        close(saved);
        fclose(f);
        return 1;
    }
    clearerr(stdin);

    fn(opts, state);

    if (dup2(saved, stdin_fd) < 0) {
        DSD_FPRINTF(stderr, "dup2(saved, stdin) failed: %s\n", strerror(errno));
        rc = 1;
    }
    clearerr(stdin);
    close(saved);
    fclose(f);
    return rc;
}

static int
test_pulse_defaults_apply_decode_and_ncurses(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    reset_harness();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    rc |= with_stdin_text("\n4\n\n", dsd_bootstrap_interactive, &opts, &state);
    rc |= expect_int("pulse-audio-input-calls", g_audio_input_calls, 1);
    rc |= expect_int("pulse-audio-output-calls", g_audio_output_calls, 1);
    rc |= expect_str("pulse-audio-in", opts.audio_in_dev, "pulse:stub-input");
    rc |= expect_str("pulse-audio-out", opts.audio_out_dev, "pulse:stub-output");
    rc |= expect_int("pulse-decode-mode", g_last_decode_mode, DSDCFG_MODE_DMR);
    rc |= expect_int("pulse-decode-profile", g_last_decode_profile, DSD_DECODE_PRESET_PROFILE_INTERACTIVE);
    rc |= expect_int("pulse-ncurses-default", opts.frontend_kind, EXPECTED_DEFAULT_FRONTEND);
    return rc;
}

static int
test_rtltcp_trunking_imports_group_allow_list_and_null_output(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* input = "3\n"
                        "host.example\n"
                        "2345\n"
                        "851.375M\n"
                        "30\n"
                        "-5\n"
                        "12\n"
                        "7\n"
                        "2\n"
                        "2\n"
                        "y\n"
                        "chan.csv\n"
                        "group.csv\n"
                        "y\n"
                        "n\n"
                        "y\n"
                        "n\n";

    reset_harness();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    rc |= with_stdin_text(input, dsd_bootstrap_interactive, &opts, &state);
    rc |= expect_str("rtltcp-audio-in", opts.audio_in_dev, "rtltcp:host.example:2345:851.375M:30:-5:12:7:2");
    rc |= expect_int("rtltcp-decode-mode", g_last_decode_mode, DSDCFG_MODE_P25P1);
    rc |= expect_int("rtltcp-trunk-p25", opts.p25_trunk, 1);
    rc |= expect_int("rtltcp-trunk-enable", opts.trunk_enable, 1);
    rc |= expect_str("rtltcp-channel-file", opts.chan_in_file, "chan.csv");
    rc |= expect_str("rtltcp-group-file", opts.group_in_file, "group.csv");
    rc |= expect_int("rtltcp-channel-import", g_chan_import_calls, 1);
    rc |= expect_int("rtltcp-group-import", g_group_import_calls, 1);
    rc |= expect_int("rtltcp-allow-list", opts.trunk_use_allow_list, 1);
    rc |= expect_str("rtltcp-null-output", opts.audio_out_dev, "null");
    rc |= expect_int("rtltcp-audio-output-chooser", g_audio_output_calls, 0);
    rc |= expect_int("rtltcp-ncurses-disabled", opts.frontend_kind, DSD_FRONTEND_NONE);
    return rc;
}

static int
test_tcp_trunking_enables_default_rigctl_and_skips_missing_csv(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* input = "5\n"
                        "\n"
                        "9999\n"
                        "13\n"
                        "y\n"
                        "missing-chan.csv\n"
                        "missing-group.csv\n"
                        "n\n"
                        "n\n"
                        "\n";

    reset_harness();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    rc |= with_stdin_text(input, dsd_bootstrap_interactive, &opts, &state);
    rc |= expect_str("tcp-audio-in", opts.audio_in_dev, "tcp:127.0.0.1:9999");
    rc |= expect_int("tcp-decode-mode", g_last_decode_mode, DSDCFG_MODE_TDMA);
    rc |= expect_int("tcp-trunk-enable", opts.trunk_enable, 1);
    rc |= expect_int("tcp-use-rigctl", opts.use_rigctl, 1);
    rc |= expect_int("tcp-rigctl-port", opts.rigctlportno, 4532);
    rc |= expect_int("tcp-channel-import-skipped", g_chan_import_calls, 0);
    rc |= expect_int("tcp-group-import-skipped", g_group_import_calls, 0);
    rc |= expect_str("tcp-no-null-output", opts.audio_out_dev, "");
    rc |= expect_int("tcp-ncurses-default", opts.frontend_kind, EXPECTED_DEFAULT_FRONTEND);
    return rc;
}

static int
test_udp_eof_uses_socket_defaults_and_default_pulse_output(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    reset_harness();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    rc |= with_stdin_text("6\n", dsd_bootstrap_interactive, &opts, &state);
    rc |= expect_str("udp-default-audio-in", opts.audio_in_dev, "udp:127.0.0.1:7355");
    rc |= expect_int("udp-default-decode-mode", g_last_decode_mode, DSDCFG_MODE_AUTO);
    rc |= expect_int("udp-default-audio-output", g_audio_output_calls, 1);
    rc |= expect_str("udp-default-audio-out", opts.audio_out_dev, "pulse:stub-output");
    rc |= expect_int("udp-default-ncurses", opts.frontend_kind, EXPECTED_DEFAULT_FRONTEND);
    return rc;
}

static int
test_file_input_applies_clamped_low_sample_rate(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* input = "4\n"
                        "capture.raw\n"
                        "1\n"
                        "14\n"
                        "n\n"
                        "n\n"
                        "n\n";

    reset_harness();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;

    rc |= with_stdin_text(input, dsd_bootstrap_interactive, &opts, &state);
    rc |= expect_str("file-audio-in", opts.audio_in_dev, "capture.raw");
    rc |= expect_int("file-clamped-sample-rate", opts.wav_sample_rate, 8000);
    rc |= expect_int("file-effective-sample-rate", dsd_opts_effective_input_rate(&opts), 48000);
    rc |= expect_int("file-samples-per-symbol", state.samplesPerSymbol, 10);
    rc |= expect_int("file-symbol-center", state.symbolCenter, 4);
    rc |= expect_int("file-decode-mode", g_last_decode_mode, DSDCFG_MODE_ANALOG);
    rc |= expect_str("file-output-left-empty", opts.audio_out_dev, "");
    rc |= expect_int("file-ncurses-disabled", opts.frontend_kind, DSD_FRONTEND_NONE);
    return rc;
}

static int
test_empty_file_path_falls_back_to_pulse_devices(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* input = "4\n"
                        "\n"
                        "4\n"
                        "n\n";

    reset_harness();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    rc |= with_stdin_text(input, dsd_bootstrap_interactive, &opts, &state);
    rc |= expect_int("empty-file-audio-input-calls", g_audio_input_calls, 1);
    rc |= expect_int("empty-file-audio-output-calls", g_audio_output_calls, 1);
    rc |= expect_str("empty-file-pulse-input", opts.audio_in_dev, "pulse:stub-input");
    rc |= expect_str("empty-file-pulse-output", opts.audio_out_dev, "pulse:stub-output");
    rc |= expect_int("empty-file-decode-mode", g_last_decode_mode, DSDCFG_MODE_DMR);
    rc |= expect_int("empty-file-ncurses-disabled", opts.frontend_kind, DSD_FRONTEND_NONE);
    return rc;
}

static int
test_rtl_input_formats_clamped_radio_options(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* input = "2\n"
                        "851.375M\n"
                        "999\n"
                        "-1\n"
                        "999\n"
                        "3\n"
                        "-1001\n"
                        "9\n"
                        "14\n"
                        "n\n"
                        "n\n"
                        "n\n";

    reset_harness();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    rc |= with_stdin_text(input, dsd_bootstrap_interactive, &opts, &state);
    rc |= expect_str("rtl-clamped-audio-in", opts.audio_in_dev, "rtl:255:851.375M:0:200:4:-1000:3");
    rc |= expect_int("rtl-decode-mode", g_last_decode_mode, DSDCFG_MODE_ANALOG);
    rc |= expect_str("rtl-output-left-empty", opts.audio_out_dev, "");
    rc |= expect_int("rtl-ncurses-disabled", opts.frontend_kind, DSD_FRONTEND_NONE);
    return rc;
}

static int
test_invalid_prompt_values_use_documented_defaults(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* input = "bogus\n"
                        "bogus\n"
                        "maybe\n";

    reset_harness();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    rc |= with_stdin_text(input, dsd_bootstrap_interactive, &opts, &state);
    rc |= expect_str("invalid-default-pulse-input", opts.audio_in_dev, "pulse:stub-input");
    rc |= expect_str("invalid-default-pulse-output", opts.audio_out_dev, "pulse:stub-output");
    rc |= expect_int("invalid-default-decode-mode", g_last_decode_mode, DSDCFG_MODE_AUTO);
    rc |= expect_int("invalid-default-ncurses", opts.frontend_kind, EXPECTED_DEFAULT_FRONTEND);
    return rc;
}

static int
test_rtl_empty_frequency_falls_back_to_pulse_devices(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* input = "2\n"
                        "\n"
                        "14\n"
                        "n\n";

    reset_harness();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    rc |= with_stdin_text(input, dsd_bootstrap_interactive, &opts, &state);
    rc |= expect_int("rtl-empty-frequency-input-calls", g_audio_input_calls, 1);
    rc |= expect_int("rtl-empty-frequency-output-calls", g_audio_output_calls, 1);
    rc |= expect_str("rtl-empty-frequency-pulse-input", opts.audio_in_dev, "pulse:stub-input");
    rc |= expect_str("rtl-empty-frequency-pulse-output", opts.audio_out_dev, "pulse:stub-output");
    rc |= expect_int("rtl-empty-frequency-decode-mode", g_last_decode_mode, DSDCFG_MODE_ANALOG);
    rc |= expect_int("rtl-empty-frequency-ncurses-disabled", opts.frontend_kind, DSD_FRONTEND_NONE);
    return rc;
}
#endif

static int
test_non_tty_keeps_defaults(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    reset_harness();
    g_isatty = 0;
    g_config_available = 0;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "sentinel-in");
    DSD_SNPRINTF(opts.audio_out_dev, sizeof opts.audio_out_dev, "%s", "sentinel-out");

    dsd_bootstrap_interactive(&opts, &state);
    rc |= expect_str("non-tty-audio-in", opts.audio_in_dev, "sentinel-in");
    rc |= expect_str("non-tty-audio-out", opts.audio_out_dev, "sentinel-out");
    rc |= expect_int("non-tty-config-init", g_config_init_calls, 0);
    rc |= expect_int("non-tty-decode-mode", g_last_decode_mode, DSDCFG_MODE_UNSET);
    return rc;
}

static int
test_config_no_bootstrap_skips_wizard(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    reset_harness();
    g_config.no_bootstrap_enable = 1;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    dsd_bootstrap_interactive(&opts, &state);
    rc |= expect_int("no-bootstrap-audio-input", g_audio_input_calls, 0);
    rc |= expect_int("no-bootstrap-audio-output", g_audio_output_calls, 0);
    rc |= expect_int("no-bootstrap-decode-mode", g_last_decode_mode, DSDCFG_MODE_UNSET);
    rc |= expect_int("no-bootstrap-ncurses", opts.frontend_kind, DSD_FRONTEND_NONE);
    return rc;
}

static int
test_missing_config_initializes_runtime_config(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;

    reset_harness();
    g_config_available = 0;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

#if !defined(_WIN32)
    rc |= with_stdin_text("\n1\nn\n", dsd_bootstrap_interactive, &opts, &state);
#else
    dsd_bootstrap_interactive(&opts, &state);
#endif
    rc |= expect_int("missing-config-init", g_config_init_calls, 1);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_non_tty_keeps_defaults();
    rc |= test_config_no_bootstrap_skips_wizard();
    rc |= test_missing_config_initializes_runtime_config();
#if !defined(_WIN32)
    rc |= test_pulse_defaults_apply_decode_and_ncurses();
    rc |= test_rtltcp_trunking_imports_group_allow_list_and_null_output();
    rc |= test_tcp_trunking_enables_default_rigctl_and_skips_missing_csv();
    rc |= test_udp_eof_uses_socket_defaults_and_default_pulse_output();
    rc |= test_file_input_applies_clamped_low_sample_rate();
    rc |= test_empty_file_path_falls_back_to_pulse_devices();
    rc |= test_rtl_input_formats_clamped_radio_options();
    rc |= test_invalid_prompt_values_use_documented_defaults();
    rc |= test_rtl_empty_frequency_falls_back_to_pulse_devices();
#else
    DSD_FPRINTF(stderr, "stdin redirection coverage skipped on Windows\n");
#endif

    if (rc == 0) {
        printf("RUNTIME_BOOTSTRAP_INTERACTIVE: OK\n");
    }
    return rc;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno)
