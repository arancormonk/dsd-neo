// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/cli.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static int
test_config_without_path_does_not_consume_next_arg(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int new_argc = dsd_cli_compact_args(3, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_config_with_path_consumes_only_path(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[] = "config.ini";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_config_equals_form_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--config=config.ini";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int new_argc = dsd_cli_compact_args(3, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_frame_log_consumes_path_and_leaves_short_opts(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--frame-log";
    char arg2[] = "frames.log";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_p25_sm_log_consumes_path_and_leaves_short_opts(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--p25-sm-log";
    char arg2[] = "p25-sm.log";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_vendor_privacy_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-baofeng-pc5";
    char arg2[] = "0123456789ABCDEFFEDCBA9876543210";
    char arg3[] = "--dmr-csi-ee72=112233445566778899";
    char arg4[] = "--dmr-vertex-ks-csv";
    char arg5[] = "vertex_map.csv";
    char arg6[] = "--dmr-force-algid=24";
    char arg7[] = "--dmr-force-algid";
    char arg8[] = "25";
    char arg9[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, NULL};

    int new_argc = dsd_cli_compact_args(10, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_dmr_debug_burst_long_option_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-debug-burst";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int new_argc = dsd_cli_compact_args(3, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_show_keys_long_option_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--show-keys";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int new_argc = dsd_cli_compact_args(3, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_option_terminator_preserves_later_show_keys_argument(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--";
    char arg2[] = "--show-keys";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 4) {
        DSD_FPRINTF(stderr, "expected new_argc=4, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "--") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"--\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    if (argv[2] == NULL || strcmp(argv[2], "--show-keys") != 0) {
        DSD_FPRINTF(stderr, "expected argv[2] to be \"--show-keys\", got \"%s\"\n", argv[2] ? argv[2] : "(null)");
        return 1;
    }
    if (argv[3] == NULL || strcmp(argv[3], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[3] to be \"-fi\", got \"%s\"\n", argv[3] ? argv[3] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_rtl_udp_control_consumes_port_and_leaves_short_opts(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--rtl-udp-control";
    char arg2[] = "9911";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_rtl_udp_control_missing_port_does_not_consume_next_option(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--rtl-udp-control";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int new_argc = dsd_cli_compact_args(3, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_rtl_udp_control_bind_consumes_address_and_leaves_short_opts(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--rtl-udp-control-bind";
    char arg2[] = "0.0.0.0";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_iq_capture_and_replay_long_options_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--iq-capture";
    char arg2[] = "capture.iq";
    char arg3[] = "--iq-capture-format=cu8";
    char arg4[] = "--iq-capture-max-mb";
    char arg5[] = "16";
    char arg6[] = "--iq-replay";
    char arg7[] = "capture.iq.json";
    char arg8[] = "--iq-replay-rate=realtime";
    char arg9[] = "--iq-loop";
    char arg10[] = "--iq-info";
    char arg11[] = "capture.iq.json";
    char arg12[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12, NULL};

    int new_argc = dsd_cli_compact_args(13, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_iq_capture_equals_form_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--iq-capture=cap.iq";
    char arg2[] = "--iq-info=cap.iq.json";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_iq_capture_format_and_replay_rate_paired_forms_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--iq-capture-format";
    char arg2[] = "cu8";
    char arg3[] = "--iq-replay-rate";
    char arg4[] = "realtime";
    char arg5[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, NULL};

    int new_argc = dsd_cli_compact_args(6, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

static int
test_iq_paired_option_missing_value_at_end_is_removed(const char* option_name) {
    char arg0[] = "dsd-neo";
    char arg1[64];
    DSD_SNPRINTF(arg1, sizeof arg1, "%s", option_name ? option_name : "");
    char* argv[] = {arg0, arg1, NULL};

    int new_argc = dsd_cli_compact_args(2, argv);
    if (new_argc != 1) {
        DSD_FPRINTF(stderr, "expected new_argc=1 for missing value option %s, got %d\n",
                    option_name ? option_name : "(null)", new_argc);
        return 1;
    }
    if (argv[1] != NULL) {
        DSD_FPRINTF(stderr, "expected argv[1] to be NULL for missing value option %s\n",
                    option_name ? option_name : "(null)");
        return 1;
    }
    return 0;
}

static int
test_iq_missing_value_forms_are_removed_safely(void) {
    int rc = 0;
    rc |= test_iq_paired_option_missing_value_at_end_is_removed("--iq-capture");
    rc |= test_iq_paired_option_missing_value_at_end_is_removed("--iq-capture-format");
    rc |= test_iq_paired_option_missing_value_at_end_is_removed("--iq-capture-max-mb");
    rc |= test_iq_paired_option_missing_value_at_end_is_removed("--iq-replay");
    rc |= test_iq_paired_option_missing_value_at_end_is_removed("--iq-replay-rate");
    rc |= test_iq_paired_option_missing_value_at_end_is_removed("--iq-info");
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_config_without_path_does_not_consume_next_arg();
    rc |= test_config_with_path_consumes_only_path();
    rc |= test_config_equals_form_is_removed();
    rc |= test_frame_log_consumes_path_and_leaves_short_opts();
    rc |= test_p25_sm_log_consumes_path_and_leaves_short_opts();
    rc |= test_vendor_privacy_long_opts_are_removed();
    rc |= test_dmr_debug_burst_long_option_is_removed();
    rc |= test_show_keys_long_option_is_removed();
    rc |= test_option_terminator_preserves_later_show_keys_argument();
    rc |= test_rtl_udp_control_consumes_port_and_leaves_short_opts();
    rc |= test_rtl_udp_control_missing_port_does_not_consume_next_option();
    rc |= test_rtl_udp_control_bind_consumes_address_and_leaves_short_opts();
    rc |= test_iq_capture_and_replay_long_options_are_removed();
    rc |= test_iq_capture_equals_form_is_removed();
    rc |= test_iq_capture_format_and_replay_rate_paired_forms_are_removed();
    rc |= test_iq_missing_value_forms_are_removed_safely();
    return rc;
}
