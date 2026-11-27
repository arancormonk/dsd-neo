// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/cli.h>
#include <stdio.h>
#include <stdlib.h>
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

/* --rtl-list takes no value: the argument after it is kept. */
static int
test_rtl_list_long_opt_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--rtl-list";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int new_argc = dsd_cli_compact_args(3, argv);
    if (new_argc != 2 || argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "--rtl-list: expected [dsd-neo -fi], got argc %d\n", new_argc);
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
    char arg6[] = "--dmr-tg-key-csv";
    char arg7[] = "tg_key_map.csv";
    char arg8[] = "--dmr-tg-key-csv=tg_key_map.csv";
    char arg9[] = "--dmr-force-algid=24";
    char arg10[] = "--dmr-force-algid";
    char arg11[] = "25";
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
test_src_csv_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--src-csv";
    char arg2[] = "plan.csv";
    char arg3[] = "--src-csv=plan.csv";
    char arg7[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg7, NULL};

    int new_argc = dsd_cli_compact_args(5, argv);
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
test_dmr_chan_overrides_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-chan-overrides";
    char arg2[] = "mine.csv";
    char arg3[] = "--dmr-chan-overrides=mine.csv";
    char arg4[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int new_argc = dsd_cli_compact_args(5, argv);
    if (new_argc != 2 || argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected only \"-fi\" left, got %d (%s)\n", new_argc, argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

/* Issue #697: both list options, in both spellings, leave getopt nothing; "-fi" after them stays. */
static int
test_trunk_scan_list_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--trunk-scan";
    char arg2[] = "a.csv";
    char arg3[] = "--trunk-scan-off";
    char arg4[] = "b.csv";
    char arg5[] = "--trunk-scan=c.csv";
    char arg6[] = "--trunk-scan-off=d.csv";
    char arg7[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, NULL};

    int new_argc = dsd_cli_compact_args(8, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected argv[1] to be \"-fi\", got \"%s\"\n", argv[1] ? argv[1] : "(null)");
        return 1;
    }
    if (dsd_cli_long_option_arity("--trunk-scan-off") != DSD_CLI_LONG_NEXT
        || dsd_cli_long_option_arity("--trunk-scan-off=d.csv") != DSD_CLI_LONG_ALONE) {
        DSD_FPRINTF(stderr, "--trunk-scan-off arity is not the parser's\n");
        return 1;
    }
    return 0;
}

/* Issue #644: --trunk-scan-hold, in both spellings, leaves getopt nothing, a target id that reads like an option
   included; "-fi" after it stays. */
static int
test_trunk_scan_hold_long_opt_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--trunk-scan-hold";
    char arg2[] = "-county";
    char arg3[] = "--trunk-scan-hold=fire";
    char arg4[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int new_argc = dsd_cli_compact_args(5, argv);
    if (new_argc != 2 || argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "--trunk-scan-hold: new_argc=%d argv[1]=\"%s\", want 2 and \"-fi\"\n", new_argc,
                    argv[1] ? argv[1] : "(null)");
        return 1;
    }
    if (dsd_cli_long_option_arity("--trunk-scan-hold") != DSD_CLI_LONG_NEXT
        || dsd_cli_long_option_arity("--trunk-scan-hold=fire") != DSD_CLI_LONG_ALONE) {
        DSD_FPRINTF(stderr, "--trunk-scan-hold arity is not the parser's\n");
        return 1;
    }
    return 0;
}

static int
test_p25_bandplan_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--p25-bandplan";
    char arg2[] = "plan.csv";
    char arg3[] = "--p25-bandplan=plan.csv";
    char arg4[] = "--p25-bandplan-export";
    char arg5[] = "learned.csv";
    char arg6[] = "--p25-bandplan-export=learned.csv";
    char arg7[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, NULL};

    int new_argc = dsd_cli_compact_args(8, argv);
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
test_scan_voice_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--scan-voice-only";
    char arg2[] = "--scan-voice-qualify-ms";
    char arg3[] = "1500";
    char arg4[] = "--scan-voice-qualify-ms=1500";
    char arg5[] = "--scan-voice-hold-ms";
    char arg6[] = "2500";
    char arg7[] = "--scan-voice-hold-ms=2500";
    char arg8[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, NULL};

    int new_argc = dsd_cli_compact_args(9, argv);
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

/* The session-wide skip-silent switches (#834) take no value and must leave this pre-getopt pass, or getopt rejects
 * them. */
static int
test_scan_skip_silent_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--scan-skip-silent";
    char arg2[] = "--no-scan-skip-silent";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2 || argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected only -fi to survive, got argc=%d\n", new_argc);
        return 1;
    }
    return 0;
}

/* The skip-silent reading length (#834) takes a value in both spellings: the flag and its value leave this pre-getopt
 * pass together, so a bare "50" never reads as a positional argument, and the boolean --scan-skip-silent beside it is
 * still its own switch. */
static int
test_scan_skip_silent_ms_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--scan-skip-silent-ms";
    char arg2[] = "50";
    char arg3[] = "--scan-skip-silent-ms=120";
    char arg4[] = "--scan-skip-silent";
    char arg5[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, NULL};

    int new_argc = dsd_cli_compact_args(6, argv);
    if (new_argc != 2 || argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected only -fi to survive the skip-silent length, got argc=%d argv[1]=\"%s\"\n",
                    new_argc, argv[1] ? argv[1] : "(null)");
        return 1;
    }
    return 0;
}

/* The per-visit cap (issue #507) must leave this pre-getopt pass in BOTH spellings. A
 * surviving bare value token looks like a positional argument to
 * bootstrap_compacted_arg_disables_inherited_trunk_scan(), which would switch off a
 * config-inherited trunk scan. */
static int
test_scan_max_visit_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--scan-max-visit-ms";
    char arg2[] = "20000";
    char arg3[] = "--scan-max-visit-ms=25000";
    char arg4[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int new_argc = dsd_cli_compact_args(5, argv);
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

/* Issue #524: --am-bandwidth-hz takes its value as the next token or after '='. Both spellings leave the short-option
   pass, so a bare "8000" is never read as a positional argument (which would switch a config-inherited trunk scan
   off). */
static int
test_am_bandwidth_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--am-bandwidth-hz";
    char arg2[] = "8000";
    char arg3[] = "--am-bandwidth-hz=6000";
    char arg4[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int new_argc = dsd_cli_compact_args(5, argv);
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

/* Issue #756: the NFM recording max gap leaves the pre-getopt pass in both spellings, so its bare value is never read
   as a positional argument. */
static int
test_analog_record_max_gap_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--analog-record-max-gap-ms";
    char arg2[] = "2000";
    char arg3[] = "--analog-record-max-gap-ms=500";
    char arg4[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int new_argc = dsd_cli_compact_args(5, argv);
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

/* Issue #668: the NFM recording pre-roll leaves the pre-getopt pass in both spellings, so its bare value is never read
   as a positional argument. */
static int
test_analog_record_preroll_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--analog-record-preroll-ms";
    char arg2[] = "200";
    char arg3[] = "--analog-record-preroll-ms=500";
    char arg4[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int new_argc = dsd_cli_compact_args(5, argv);
    if (new_argc != 2 || argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected only -fi to remain, got %d args\n", new_argc);
        return 1;
    }
    return 0;
}

/* Issue #668: the NFM rdio-scanner options leave the pre-getopt pass with their values, so a bare value is never read
   as a positional argument. */
static int
test_rdio_analog_long_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--rdio-analog-system-id";
    char arg2[] = "9";
    char arg3[] = "--rdio-analog-talkgroup";
    char arg4[] = "4242";
    char arg5[] = "-fA";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, NULL};

    int new_argc = dsd_cli_compact_args(6, argv);
    if (new_argc != 2 || argv[1] == NULL || strcmp(argv[1], "-fA") != 0) {
        DSD_FPRINTF(stderr, "expected only -fA to remain, got %d args\n", new_argc);
        return 1;
    }
    return 0;
}

/* Issue #888: --rdio-unknown-talkgroup leaves the pre-getopt pass with its value, so a bare value is never read as a
   positional argument. */
static int
test_rdio_unknown_talkgroup_long_opt_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--rdio-unknown-talkgroup";
    char arg2[] = "4242";
    char arg3[] = "-fs";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2 || argv[1] == NULL || strcmp(argv[1], "-fs") != 0) {
        DSD_FPRINTF(stderr, "expected only -fs to remain, got %d args\n", new_argc);
        return 1;
    }
    return 0;
}

/* Issue #931: --rdio-system-by-frequency and --no-rdio-system-by-frequency take no value: the pre-getopt pass removes
   them alone and keeps the option after each. */
static int
test_rdio_system_by_frequency_opts_are_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--rdio-system-by-frequency";
    char arg2[] = "-fA";
    char arg3[] = "--no-rdio-system-by-frequency";
    char arg4[] = "-P";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int new_argc = dsd_cli_compact_args(5, argv);
    if (new_argc != 3 || argv[1] == NULL || strcmp(argv[1], "-fA") != 0 || argv[2] == NULL
        || strcmp(argv[2], "-P") != 0) {
        DSD_FPRINTF(stderr, "expected -fA and -P to remain, got %d args\n", new_argc);
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
test_dmr_debug_unsynced_long_option_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-debug-unsynced";
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
test_lrrp_extra_port_consumes_port_and_leaves_short_opts(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--lrrp-extra-port";
    char arg2[] = "5000";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected -fi to survive compaction\n");
        return 1;
    }
    return 0;
}

static int
test_lrrp_extra_port_equals_form_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--lrrp-extra-port=5000";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int new_argc = dsd_cli_compact_args(3, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected -fi to survive compaction\n");
        return 1;
    }
    return 0;
}

static int
test_dmr_udp_comp_port_consumes_value_and_leaves_short_opts(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-udp-comp-port";
    char arg2[] = "97=4005";
    char arg3[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int new_argc = dsd_cli_compact_args(4, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected -fi to survive compaction\n");
        return 1;
    }
    return 0;
}

static int
test_dmr_udp_comp_port_equals_form_is_removed(void) {
    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-udp-comp-port=97=4005";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int new_argc = dsd_cli_compact_args(3, argv);
    if (new_argc != 2) {
        DSD_FPRINTF(stderr, "expected new_argc=2, got %d\n", new_argc);
        return 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "-fi") != 0) {
        DSD_FPRINTF(stderr, "expected -fi to survive compaction\n");
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

/* Issue #931: the parser takes the next argument as the port whatever it starts with (and refuses "-T" as one), so
   compaction drops it too: getopt never reads a value the long-option pass already took ("--api -T --api 0" turns no
   trunking on). */
static int
test_rtl_udp_control_takes_next_argument_whatever_it_is(void) {
    static const char* const options[] = {
        "--lrrp-extra-port", "--dmr-udp-comp-port", "--rtl-udp-control", "--rtl-udp-control-bind", "--api",
        "--api-bind",        "--tetra-net",         "--tetra-key"};
    int rc = 0;
    for (size_t i = 0; i < sizeof options / sizeof options[0]; i++) {
        char arg0[] = "dsd-neo";
        char arg1[32];
        DSD_SNPRINTF(arg1, sizeof arg1, "%s", options[i]);
        char arg2[] = "-T";
        char arg3[] = "-fi";
        char* argv[] = {arg0, arg1, arg2, arg3, NULL};
        const int new_argc = dsd_cli_compact_args(4, argv);
        if (new_argc != 2 || argv[1] == NULL || strcmp(argv[1], "-fi") != 0
            || dsd_cli_long_option_arity(options[i]) != DSD_CLI_LONG_NEXT) {
            DSD_FPRINTF(stderr, "%s -T -fi: new_argc=%d argv[1]=\"%s\"\n", options[i], new_argc,
                        argv[1] ? argv[1] : "(null)");
            rc = 1;
        }
    }
    return rc;
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

/**
 * @brief Compaction shifts survivors down and leaves stale aliases in the tail.
 *
 * A host that heap-allocates argv (the Android JNI layer does) must free through
 * its own record of the allocations, never through the compacted array: the tail
 * slots still point at strings that also live at lower indices, so freeing all
 * argc slots double-frees them. This pins the aliasing so the hazard cannot be
 * quietly designed away, and — because the strings are heap-allocated — a wrong
 * free here is a real double free under the ASan preset.
 */
static int
test_compaction_leaves_aliases_in_the_tail(void) {
    const char* const words[] = {"dsd-neo", "--frontend", "none", "-f1", "-o", "pulse"};
    const int argc = (int)(sizeof(words) / sizeof(words[0]));
    char* owned[sizeof(words) / sizeof(words[0])];
    char* argv[(sizeof(words) / sizeof(words[0])) + 1U];
    int rc = 0;
    int aliases = 0;

    for (int i = 0; i < argc; i++) {
        const size_t len = strlen(words[i]) + 1U;
        owned[i] = (char*)malloc(len);
        if (owned[i] == NULL) {
            DSD_FPRINTF(stderr, "allocation failed\n");
            for (int j = 0; j < i; j++) {
                free(owned[j]);
            }
            return 1;
        }
        DSD_MEMCPY(owned[i], words[i], len);
        argv[i] = owned[i];
    }
    argv[argc] = NULL;

    const int new_argc = dsd_cli_compact_args(argc, argv);
    if (new_argc != 4) {
        DSD_FPRINTF(stderr, "expected new_argc=4, got %d\n", new_argc);
        rc = 1;
    }

    for (int i = new_argc; i < argc; i++) {
        for (int j = 0; j < new_argc; j++) {
            if (argv[i] != NULL && argv[i] == argv[j]) {
                aliases++;
            }
        }
    }
    if (aliases == 0) {
        DSD_FPRINTF(stderr, "expected the compacted tail to alias a surviving entry\n");
        rc = 1;
    }

    /* Free through the ownership record, which compaction never touched. */
    for (int i = 0; i < argc; i++) {
        free(owned[i]);
    }
    return rc;
}

/* Issue #708: a frontend walks options a user typed by the parser's own arity, the tables compaction steps by. */
static int
test_option_arity_matches_the_parser(void) {
    int rc = 0;

    struct {
        const char* arg;
        dsd_cli_long_arity arity;
    } const longs[] = {
        {"--record-unknown-only", DSD_CLI_LONG_ALONE},
        {"--rdio-api-delete-after-upload", DSD_CLI_LONG_ALONE},
        {"--frontend=none", DSD_CLI_LONG_ALONE},
        {"--wav-journal", DSD_CLI_LONG_NEXT},
        {"--input-level-warn-db", DSD_CLI_LONG_NEXT},
        {"--analog-record-max-gap-ms", DSD_CLI_LONG_NEXT},
        {"--analog-record-max-gap-ms=2000", DSD_CLI_LONG_ALONE},
        {"--analog-record-preroll-ms", DSD_CLI_LONG_NEXT},
        {"--analog-record-preroll-ms=200", DSD_CLI_LONG_ALONE},
        {"--rdio-analog-system-id", DSD_CLI_LONG_NEXT},
        {"--rdio-analog-talkgroup", DSD_CLI_LONG_NEXT},
        {"--rdio-unknown-talkgroup", DSD_CLI_LONG_NEXT},
        {"--rdio-system-by-frequency", DSD_CLI_LONG_ALONE},
        {"--no-rdio-system-by-frequency", DSD_CLI_LONG_ALONE},
        {"--iq-replay", DSD_CLI_LONG_NEXT},
        {"--api-token", DSD_CLI_LONG_NEXT},
        {"--config", DSD_CLI_LONG_NEXT_NON_OPTION},
        {"--squelch-analog-default", DSD_CLI_LONG_NEXT},
        {"--squelch-analog-default=noise+10", DSD_CLI_LONG_ALONE},
        {"--airspy-list", DSD_CLI_LONG_ALONE},
        {"--rtl-list", DSD_CLI_LONG_ALONE},
        {"--no-such-option", DSD_CLI_LONG_UNKNOWN},
    };

    for (size_t i = 0; i < sizeof longs / sizeof longs[0]; i++) {
        if (dsd_cli_long_option_arity(longs[i].arg) != longs[i].arity) {
            DSD_FPRINTF(stderr, "FAIL: %s reads as arity %d\n", longs[i].arg,
                        (int)dsd_cli_long_option_arity(longs[i].arg));
            rc = 1;
        }
    }
    if (dsd_cli_long_option_arity(NULL) != DSD_CLI_LONG_UNKNOWN) {
        DSD_FPRINTF(stderr, "FAIL: NULL has an arity\n");
        rc = 1;
    }

    /* Issue #899: of the options that take the next argument unless it starts with '-', only --config and
       --validate-config do without one; the parser refuses the others without theirs. */
    static const struct {
        const char* arg;
        int optional;
    } values[] = {
        {"--config", 1},    {"--validate-config", 1},
        {"--api", 0},       {"--api-bind", 0},
        {"--tetra-key", 0}, {"--tetra-net", 0},
        {"--squelch", 0},   {"--config=x.ini", 0},
        {"--api-token", 0}, {"--rtl-udp-control-bind", 0},
        {"--no-such", 0},
    };

    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        if (dsd_cli_long_option_value_optional(values[i].arg) != values[i].optional) {
            DSD_FPRINTF(stderr, "FAIL: %s reads as value-optional %d\n", values[i].arg,
                        dsd_cli_long_option_value_optional(values[i].arg));
            rc = 1;
        }
    }
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        if (values[i].optional && dsd_cli_long_option_arity(values[i].arg) != DSD_CLI_LONG_NEXT_NON_OPTION) {
            DSD_FPRINTF(stderr, "FAIL: %s is value-optional but not next-non-option\n", values[i].arg);
            rc = 1;
        }
    }
    if (dsd_cli_long_option_value_optional(NULL)) {
        DSD_FPRINTF(stderr, "FAIL: NULL takes no value\n");
        rc = 1;
    }
    const char takes[] = "7wPcoiF";
    const int want[] = {1, 1, 0, 1, 1, 1, 0};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        if (dsd_cli_short_option_takes_value(takes[i]) != want[i]) {
            DSD_FPRINTF(stderr, "FAIL: -%c value %d\n", takes[i], dsd_cli_short_option_takes_value(takes[i]));
            rc = 1;
        }
    }
    if (dsd_cli_short_option_takes_value(':') || dsd_cli_short_option_takes_value('\0')
        || dsd_cli_short_options()[0] != ':') {
        DSD_FPRINTF(stderr, "FAIL: the option string's own markers read as options\n");
        rc = 1;
    }
    return rc;
}

/* Issue #931 (B10): how many elements a long option spans as the parser reads it, for the raw-argv passes (bootstrap,
   the one-shot check): every long option's value counts, a secret's or a path's that reads like an option included. */
static int
test_long_option_span(void) {
    char a0[] = "dsd-neo";
    char key[] = "--rdio-api-key";
    char print_config[] = "--print-config";
    char config[] = "--config";
    char path[] = "x.ini";
    char dashes[] = "--";
    char url[] = "--rdio-api-url";
    char frame_log[] = "--frame-log";
    char frontend_eq[] = "--frontend=none";
    char unknown[] = "--no-such-option";
    char short_opt[] = "-o";
    char api[] = "--api";
    char validate[] = "--validate-config";

    struct {
        char* argv[4];
        int argc;
        int i;
        int want;
    } const cases[] = {
        {{a0, key, print_config, NULL}, 3, 1, 2},
        {{a0, key, dashes, NULL}, 3, 1, 2},
        {{a0, url, config, NULL}, 3, 1, 2},
        {{a0, frame_log, print_config, NULL}, 3, 1, 2},
        {{a0, key, NULL, NULL}, 2, 1, 1},
        {{a0, config, path, NULL}, 3, 1, 2},
        {{a0, config, print_config, NULL}, 3, 1, 1},
        {{a0, config, NULL, NULL}, 2, 1, 1},
        {{a0, validate, key, NULL}, 3, 1, 1},
        {{a0, api, key, NULL}, 3, 1, 2},
        {{a0, api, short_opt, NULL}, 3, 1, 2},
        {{a0, print_config, key, NULL}, 3, 1, 1},
        {{a0, frontend_eq, path, NULL}, 3, 1, 1},
        {{a0, unknown, path, NULL}, 3, 1, 0},
        {{a0, short_opt, path, NULL}, 3, 1, 0},
        {{a0, dashes, path, NULL}, 3, 1, 0},
        {{a0, key, path, NULL}, 3, 3, 0},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char* argv[4];
        for (int k = 0; k < 4; k++) {
            argv[k] = cases[i].argv[k];
        }
        const int got = dsd_cli_long_option_span(cases[i].argc, argv, cases[i].i);
        if (got != cases[i].want) {
            DSD_FPRINTF(stderr, "FAIL: case %zu (%s) spans %d, want %d\n", i, argv[cases[i].i] ? argv[cases[i].i] : "-",
                        got, cases[i].want);
            rc = 1;
        }
    }
    if (dsd_cli_long_option_span(2, NULL, 1) != 0) {
        DSD_FPRINTF(stderr, "FAIL: a NULL argv spans something\n");
        rc = 1;
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_compaction_leaves_aliases_in_the_tail();
    rc |= test_config_without_path_does_not_consume_next_arg();
    rc |= test_config_with_path_consumes_only_path();
    rc |= test_config_equals_form_is_removed();
    rc |= test_frame_log_consumes_path_and_leaves_short_opts();
    rc |= test_p25_sm_log_consumes_path_and_leaves_short_opts();
    rc |= test_vendor_privacy_long_opts_are_removed();
    rc |= test_dmr_debug_burst_long_option_is_removed();
    rc |= test_dmr_debug_unsynced_long_option_is_removed();
    rc |= test_show_keys_long_option_is_removed();
    rc |= test_option_terminator_preserves_later_show_keys_argument();
    rc |= test_lrrp_extra_port_consumes_port_and_leaves_short_opts();
    rc |= test_lrrp_extra_port_equals_form_is_removed();
    rc |= test_dmr_udp_comp_port_consumes_value_and_leaves_short_opts();
    rc |= test_dmr_udp_comp_port_equals_form_is_removed();
    rc |= test_rtl_udp_control_consumes_port_and_leaves_short_opts();
    rc |= test_rtl_udp_control_takes_next_argument_whatever_it_is();
    rc |= test_rtl_udp_control_bind_consumes_address_and_leaves_short_opts();
    rc |= test_iq_capture_and_replay_long_options_are_removed();
    rc |= test_iq_capture_equals_form_is_removed();
    rc |= test_iq_capture_format_and_replay_rate_paired_forms_are_removed();
    rc |= test_iq_missing_value_forms_are_removed_safely();
    rc |= test_src_csv_long_opts_are_removed();
    rc |= test_dmr_chan_overrides_long_opts_are_removed();
    rc |= test_p25_bandplan_long_opts_are_removed();
    rc |= test_trunk_scan_list_long_opts_are_removed();
    rc |= test_trunk_scan_hold_long_opt_is_removed();
    rc |= test_scan_voice_long_opts_are_removed();
    rc |= test_scan_skip_silent_long_opts_are_removed();
    rc |= test_scan_max_visit_long_opts_are_removed();
    rc |= test_scan_skip_silent_ms_long_opts_are_removed();
    rc |= test_am_bandwidth_long_opts_are_removed();
    rc |= test_analog_record_max_gap_long_opts_are_removed();
    rc |= test_analog_record_preroll_long_opts_are_removed();
    rc |= test_rdio_analog_long_opts_are_removed();
    rc |= test_rdio_unknown_talkgroup_long_opt_is_removed();
    rc |= test_rdio_system_by_frequency_opts_are_removed();
    rc |= test_option_arity_matches_the_parser();
    rc |= test_long_option_span();
    rc |= test_rtl_list_long_opt_is_removed();
    return rc;
}
