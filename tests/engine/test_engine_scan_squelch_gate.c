// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* Per-target squelch overrides reach a real gate. The trunk scan coordinator and the frame-sync
 * power gate are both production code; only the tuner is replaced. With the channel power held
 * fixed, the gate's verdict has to flip as the scan moves between a target that opens it, one
 * that closes it, one that switches the squelch off and one that inherits the configured
 * default, and come back to the default when the scan ends (issue #521). */
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "dsd-neo/platform/platform.h"
#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "frame_sync_internal.h"
#include "test_support.h"
#include "trunk_scan_internal.h"
#include "trunk_scan_test_support.h"

static dsd_opts g_opts;
static dsd_state g_state;
static char g_dir[DSD_TEST_PATH_MAX];
static char g_targets[DSD_TEST_PATH_MAX];
static long g_frequency;
static int g_pushes;
static double g_pushed = -1.0;

static int
expect(int condition, const char* message) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

static void
record_push(double mean_power) {
    g_pushes++;
    g_pushed = mean_power;
}

static long
current_frequency(const dsd_opts* opts) {
    (void)opts;
    return g_frequency;
}

static dsd_trunk_tune_result
tune(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t request_id) {
    (void)sps;
    (void)request_id;
    g_frequency = freq;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    state->trunk_cc_freq = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static int
write_file(const char* path, const char* content) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return 1;
    }
    const size_t len = strlen(content);
    const int failed = fwrite(content, 1, len, fp) != len;
    return fclose(fp) != 0 || failed;
}

/* The level in dsd_opts and the one the demod was last handed both match `db` (0 = off), and the
 * production frame-sync gate, fed the fixed channel power, skips (closed) or passes (open). */
static int
expect_gate(const char* stage, int db, int closed) {
    const double want = dsd_squelch_level_from_sql((double)db);
    const double tol = 1e-9 * fmax(fabs(want), fabs(g_opts.rtl_squelch_level));
    const int level_ok = fabs(g_opts.rtl_squelch_level - want) <= tol;
    const int pushed_ok = fabs(g_pushed - want) <= 1e-9 * fmax(fabs(want), fabs(g_pushed));
    const int skip = frame_sync_should_skip_snr_or_power_gate(&g_opts, &g_state);
    if (!level_ok || !pushed_ok || skip != closed) {
        DSD_FPRINTF(stderr, "FAIL: %s: level %.3g pushed %.3g gate %s, want %d dB gate %s\n", stage,
                    g_opts.rtl_squelch_level, g_pushed, skip ? "closed" : "open", db, closed ? "closed" : "open");
        return 1;
    }
    return 0;
}

static int
setup(void) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    initOpts(&g_opts);
    initState(&g_state);
    g_opts.verbose = 0;
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.audio_out_type = 9;
    g_opts.use_rigctl = 1;
    g_opts.trunk_scan_enabled = 1;
    /* A DMR-only baseline, so the configured default gates frame sync (GFSK) before and after
     * the scan as well as on the targets. */
    g_opts.frame_p25p1 = g_opts.frame_p25p2 = g_opts.frame_dstar = g_opts.frame_x2tdma = 0;
    g_opts.frame_nxdn48 = g_opts.frame_nxdn96 = g_opts.frame_dpmr = g_opts.frame_provoice = 0;
    g_opts.frame_ysf = g_opts.frame_m17 = 0;
    g_opts.frame_dmr = 1;
    g_opts.rtl_squelch_level = dsd_squelch_level_from_sql(-65.0);
    /* The channel power every gate below sees: between the target thresholds. */
    g_opts.rtl_pwr = dsd_squelch_level_from_sql(-70.0);
    g_pushes = 0;
    g_pushed = -1.0;
    dsd_trunk_tuning_requests_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune, .tune_to_cc_request = tune, .return_to_cc_request = return_to_cc});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){.get_current_freq_hz = current_frequency});
    const dsd_rtl_stream_metrics_hooks hooks = {.set_channel_squelch = record_push};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    if (!dsd_test_mkdtemp(g_dir, sizeof(g_dir), "dsdneo_sql_gate")
        || dsd_test_path_join(g_targets, sizeof(g_targets), g_dir, "targets.csv") != 0
        || write_file(g_targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                                 "open,dmr-trunk,451000000,,250,250,,--squelch-db -80\n"
                                 "thr,dmr-trunk,452000000,,250,250,,--squelch-db -60\n"
                                 "off,nxdn48-trunk,453000000,,250,250,,--squelch-db 0\n"
                                 "inh,nxdn48-trunk,454000000,,250,250,\n")) {
        return 1;
    }
    DSD_SNPRINTF(g_opts.trunk_scan_targets_csv, sizeof(g_opts.trunk_scan_targets_csv), "%s", g_targets);
    return 0;
}

static void
cleanup(void) {
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_trunk_tuning_requests_reset();
    trunk_scan_test_clear_now();
    freeState(&g_state);
    (void)remove(g_targets);
#if DSD_PLATFORM_WIN_NATIVE
    (void)_rmdir(g_dir);
#else
    (void)rmdir(g_dir);
#endif
}

static int
test_target_squelch_drives_frame_sync_gate(void) {
    if (setup()) {
        cleanup();
        return 1;
    }
    /* Before the scan the configured -65 dB closes the gate on the -70 dB channel. */
    int rc = expect(frame_sync_should_skip_snr_or_power_gate(&g_opts, &g_state) == 1, "default closes before the scan");
    trunk_scan_test_set_now(0.0);
    char error[256] = {0};
    if (dsd_engine_trunk_scan_init(&g_opts, &g_state, error, sizeof(error)) != 0) {
        DSD_FPRINTF(stderr, "scan init: %s\n", error);
        cleanup();
        return 1;
    }
    rc |= expect(dsd_engine_trunk_scan_active_index(&g_state) == 0, "scan starts on the first target");
    rc |= expect_gate("target that opens", -80, 0);

    static const struct {
        const char* stage;
        int db;
        int closed;
    } visits[] = {
        {"target that closes", -60, 1},
        {"target that switches the squelch off", 0, 0},
        {"target that inherits the default", -65, 1},
        {"rotation back to the target that opens", -80, 0},
    };

    for (size_t i = 0; i < sizeof visits / sizeof visits[0]; i++) {
        trunk_scan_test_set_now(0.26 * (double)(i + 1));
        dsd_engine_trunk_scan_tick(&g_opts, &g_state);
        rc |= expect(dsd_engine_trunk_scan_active_index(&g_state) == (i + 1) % 4, visits[i].stage);
        rc |= expect_gate(visits[i].stage, visits[i].db, visits[i].closed);
    }

    const int pushes_before_shutdown = g_pushes;
    dsd_engine_trunk_scan_shutdown(&g_opts, &g_state);
    rc |= expect(g_pushes == pushes_before_shutdown + 1, "shutdown hands the demod the default once");
    rc |= expect_gate("shutdown", -65, 1);
    cleanup();
    return rc;
}

int
main(void) {
    dsd_neo_config_init();
    return test_target_squelch_drives_frame_sync_gate();
}
