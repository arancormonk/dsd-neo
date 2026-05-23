// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/cli.h>

#include <string.h>

static int
compact_matches_exact(const char* arg, const char* const* options, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(arg, options[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
compact_matches_prefix(const char* arg, const char* const* options, size_t count) {
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(options[i]);
        if (strncmp(arg, options[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
compact_has_next_nonnull(int i, int argc, char** argv) {
    return (i + 1 < argc && argv[i + 1] != NULL);
}

static int
compact_has_next_non_option(int i, int argc, char** argv) {
    return (i + 1 < argc && argv[i + 1] != NULL && argv[i + 1][0] != '-');
}

int
dsd_cli_compact_args(int argc, char** argv) {
    if (argc <= 0 || argv == NULL) {
        return 0;
    }

    static const char* const k_skip_exact_no_arg[] = {
        "--auto-ppm",          "--rtltcp-autotune",      "--iq-loop",       "--rdio-api-delete-after-upload",
        "--enc-lockout",       "--enc-follow",           "--no-config",     "--print-config",
        "--interactive-setup", "--dump-config-template", "--strict-config", "--list-profiles",
    };

    static const char* const k_skip_exact_next_any[] = {
        "--input-volume",
        "--input-level-warn-db",
        "--frame-log",
        "--rdio-mode",
        "--rdio-system-id",
        "--rdio-api-url",
        "--rdio-api-key",
        "--rdio-upload-timeout-ms",
        "--rdio-upload-retries",
        "--dmr-baofeng-pc5",
        "--dmr-csi-ee72",
        "--dmr-vertex-ks-csv",
        "--auto-ppm-snr",
        "--profile",
        "--p25-vc-grace",
        "--p25-min-follow-dwell",
        "--p25-grant-voice-timeout",
        "--p25-retune-backoff",
        "--p25-mac-hold",
        "--p25-ring-hold",
        "--p25-cc-grace",
        "--p25-force-release-extra",
        "--p25-force-release-margin",
        "--p25-p1-err-hold-pct",
        "--p25-p1-err-hold-sec",
        "--calc-lcn",
        "--calc-step",
        "--calc-cc-freq",
        "--calc-cc-lcn",
        "--calc-start-lcn",
    };

    static const char* const k_skip_exact_next_nonnull[] = {
        "--iq-capture", "--iq-capture-format", "--iq-capture-max-mb", "--symbol-capture-format",
        "--iq-replay",  "--iq-replay-rate",    "--iq-info",
    };

    static const char* const k_skip_exact_next_nonopt[] = {
        "--rtl-udp-control",
        "--config",
        "--validate-config",
    };

    static const char* const k_skip_prefix[] = {
        "--rtl-udp-control=",       "--iq-capture=",   "--iq-capture-format=", "--iq-capture-max-mb=",
        "--symbol-capture-format=", "--iq-replay=",    "--iq-replay-rate=",    "--iq-info=",
        "--dmr-baofeng-pc5=",       "--dmr-csi-ee72=", "--dmr-vertex-ks-csv=", "--config=",
    };

    // Remove recognized long options so short-option getopt() sees remaining
    // tokens; keep argv[0] as program name.
    int w = 1;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (arg == NULL) {
            break;
        }
        if (compact_matches_prefix(arg, k_skip_prefix, sizeof(k_skip_prefix) / sizeof(k_skip_prefix[0]))) {
            continue;
        }
        if (compact_matches_exact(arg, k_skip_exact_no_arg,
                                  sizeof(k_skip_exact_no_arg) / sizeof(k_skip_exact_no_arg[0]))) {
            continue;
        }
        if (compact_matches_exact(arg, k_skip_exact_next_nonopt,
                                  sizeof(k_skip_exact_next_nonopt) / sizeof(k_skip_exact_next_nonopt[0]))) {
            if (compact_has_next_non_option(i, argc, argv)) {
                i++;
            }
            continue;
        }
        if (compact_matches_exact(arg, k_skip_exact_next_nonnull,
                                  sizeof(k_skip_exact_next_nonnull) / sizeof(k_skip_exact_next_nonnull[0]))) {
            if (compact_has_next_nonnull(i, argc, argv)) {
                i++;
            }
            continue;
        }
        if (compact_matches_exact(arg, k_skip_exact_next_any,
                                  sizeof(k_skip_exact_next_any) / sizeof(k_skip_exact_next_any[0]))) {
            if (i + 1 < argc) {
                i++;
            }
            continue;
        }
        argv[w++] = argv[i];
    }
    argv[w] = NULL;
    return w;
}
