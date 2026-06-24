// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

static int g_enum_rc;
static dsd_audio_device g_inputs[16];
static dsd_audio_device g_outputs[16];
static int g_last_max_count;

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

int
dsd_audio_enumerate_devices(dsd_audio_device* inputs, dsd_audio_device* outputs, int max_count) {
    g_last_max_count = max_count;
    if (g_enum_rc < 0) {
        return g_enum_rc;
    }
    if (inputs) {
        DSD_MEMCPY(inputs, g_inputs, sizeof g_inputs);
    }
    if (outputs) {
        DSD_MEMCPY(outputs, g_outputs, sizeof g_outputs);
    }
    return 0;
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

static void
reset_devices(void) {
    g_enum_rc = 0;
    g_last_max_count = 0;
    DSD_MEMSET(g_inputs, 0, sizeof g_inputs);
    DSD_MEMSET(g_outputs, 0, sizeof g_outputs);

    g_inputs[0].initialized = 1;
    DSD_SNPRINTF(g_inputs[0].name, sizeof g_inputs[0].name, "%s", "input-one");
    DSD_SNPRINTF(g_inputs[0].description, sizeof g_inputs[0].description, "%s", "Input One");
    g_inputs[1].initialized = 1;
    DSD_SNPRINTF(g_inputs[1].name, sizeof g_inputs[1].name, "%s", "input-two");
    DSD_SNPRINTF(g_inputs[1].description, sizeof g_inputs[1].description, "%s", "Input Two");

    g_outputs[0].initialized = 1;
    DSD_SNPRINTF(g_outputs[0].name, sizeof g_outputs[0].name, "%s", "output-one");
    DSD_SNPRINTF(g_outputs[0].description, sizeof g_outputs[0].description, "%s", "Output One");
    g_outputs[1].initialized = 1;
    DSD_SNPRINTF(g_outputs[1].name, sizeof g_outputs[1].name, "%s", "output-two");
    DSD_SNPRINTF(g_outputs[1].description, sizeof g_outputs[1].description, "%s", "Output Two");
}

#if !defined(_WIN32)
typedef void (*bootstrap_audio_fn)(dsd_opts* opts);

static int
with_stdin_text(const char* text, bootstrap_audio_fn fn, dsd_opts* opts) {
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

    fn(opts);

    if (dup2(saved, stdin_fd) < 0) {
        DSD_FPRINTF(stderr, "dup2(saved, stdin) failed: %s\n", strerror(errno));
        rc = 1;
    }
    close(saved);
    fclose(f);
    return rc;
}

static int
test_output_selection_defaults_and_clamps(void) {
    int rc = 0;
    static dsd_opts opts;

    reset_devices();
    DSD_MEMSET(&opts, 0, sizeof opts);
    rc |= with_stdin_text("\n", dsd_bootstrap_choose_audio_output, &opts);
    rc |= expect_str("output-empty-default", opts.audio_out_dev, "pulse");
    rc |= expect_int("output-enum-max", g_last_max_count, 16);

    reset_devices();
    DSD_MEMSET(&opts, 0, sizeof opts);
    rc |= with_stdin_text("2\n", dsd_bootstrap_choose_audio_output, &opts);
    rc |= expect_str("output-select-second", opts.audio_out_dev, "pulse:output-two");

    reset_devices();
    DSD_MEMSET(&opts, 0, sizeof opts);
    rc |= with_stdin_text("99\n", dsd_bootstrap_choose_audio_output, &opts);
    rc |= expect_str("output-clamp-high", opts.audio_out_dev, "pulse:output-two");

    reset_devices();
    DSD_MEMSET(&opts, 0, sizeof opts);
    rc |= with_stdin_text("bad\n", dsd_bootstrap_choose_audio_output, &opts);
    rc |= expect_str("output-invalid-default", opts.audio_out_dev, "pulse");

    return rc;
}

static int
test_input_selection_defaults_and_clamps(void) {
    int rc = 0;
    static dsd_opts opts;

    reset_devices();
    DSD_MEMSET(&opts, 0, sizeof opts);
    rc |= with_stdin_text("1\r\n", dsd_bootstrap_choose_audio_input, &opts);
    rc |= expect_str("input-select-first", opts.audio_in_dev, "pulse:input-one");

    reset_devices();
    DSD_MEMSET(&opts, 0, sizeof opts);
    rc |= with_stdin_text("-2\n", dsd_bootstrap_choose_audio_input, &opts);
    rc |= expect_str("input-clamp-low", opts.audio_in_dev, "pulse");

    reset_devices();
    DSD_MEMSET(&opts, 0, sizeof opts);
    rc |= with_stdin_text("99\n", dsd_bootstrap_choose_audio_input, &opts);
    rc |= expect_str("input-clamp-high", opts.audio_in_dev, "pulse:input-two");

    return rc;
}
#endif

static int
test_enumeration_failure_uses_default_devices(void) {
    int rc = 0;
    static dsd_opts opts;

    reset_devices();
    g_enum_rc = -1;
    DSD_MEMSET(&opts, 0, sizeof opts);
    dsd_bootstrap_choose_audio_output(&opts);
    rc |= expect_str("output-enum-fail", opts.audio_out_dev, "pulse");

    reset_devices();
    g_enum_rc = -1;
    DSD_MEMSET(&opts, 0, sizeof opts);
    dsd_bootstrap_choose_audio_input(&opts);
    rc |= expect_str("input-enum-fail", opts.audio_in_dev, "pulse");

    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_enumeration_failure_uses_default_devices();
#if !defined(_WIN32)
    rc |= test_output_selection_defaults_and_clamps();
    rc |= test_input_selection_defaults_and_clamps();
#else
    DSD_FPRINTF(stderr, "stdin redirection coverage skipped on Windows\n");
#endif

    if (rc == 0) {
        printf("RUNTIME_BOOTSTRAP_AUDIO: OK\n");
    }
    return rc;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-unix.Errno)
