// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/cli.h>

#include <stdio.h>
#include <string.h>

static int
test_config_without_path_does_not_consume_next_arg(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int new_argc = dsd_cli_compact_args(3, argv);
    if (new_argc != 2) {
        fprintf(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        fprintf(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
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
        fprintf(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        fprintf(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
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
        fprintf(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        fprintf(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
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
        fprintf(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        fprintf(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
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
    char arg4[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int new_argc = dsd_cli_compact_args(5, argv);
    if (new_argc != 2) {
        fprintf(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        fprintf(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
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
        fprintf(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        fprintf(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
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
        fprintf(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        fprintf(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_config_without_path_does_not_consume_next_arg();
    rc |= test_config_with_path_consumes_only_path();
    rc |= test_config_equals_form_is_removed();
    rc |= test_frame_log_consumes_path_and_leaves_short_opts();
    rc |= test_vendor_privacy_long_opts_are_removed();
    rc |= test_rtl_udp_control_consumes_port_and_leaves_short_opts();
    rc |= test_rtl_udp_control_missing_port_does_not_consume_next_option();
    return rc;
}
