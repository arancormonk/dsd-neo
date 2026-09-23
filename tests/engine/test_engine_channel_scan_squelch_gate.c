// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* Per-row squelch overrides reach a real gate on the conventional -Y scanner, the counterpart of
 * ENGINE_SCAN_SQUELCH_GATE for trunk-scan targets. The channel scanner, the scan scope and the
 * frame-sync power gate are production code; only the tune and the engine's no-carrier reset are
 * replaced, since an RTL tune needs a live stream. With the channel power held fixed, the gate's
 * verdict has to flip as the scanner moves between a row that opens it, one that closes it, one
 * that switches the squelch off and one that inherits the configured default, and come back to the
 * default when the scan ends (issue #521).
 *
 * A PCM input has no demodulator and no frame-sync power gate; there the row level only reaches
 * the analog input monitor (-8), and ENGINE_CHANNEL_SCAN checks that dsd_opts carries it. */
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "frame_sync_internal.h"

static int g_pushes;
static double g_pushed = -1.0;
static int g_tunes;

/* The tune always lands: the scanner commits the row as it would after a real retune. */
dsd_trunk_tune_result
dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t* out) {
    (void)sps;
    (void)state;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    g_tunes++;
    const uint64_t request = dsd_trunk_tuning_request_begin();
    *out = request;
    dsd_trunk_tuning_request_complete(request, DSD_TRUNK_TUNE_RESULT_OK);
    return DSD_TRUNK_TUNE_RESULT_OK;
}

void
dsd_engine_reset_no_carrier_state(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
record_push(double mean_power) {
    g_pushes++;
    g_pushed = mean_power;
}

static uint32_t
output_rate(void) {
    return 48000U;
}

static int
expect(int condition, const char* message) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

static int
level_is(double level, double db) {
    const double want = dsd_squelch_level_from_sql(db);
    return fabs(level - want) <= 1e-9 * fmax(fabs(want), fabs(level));
}

/* The level in dsd_opts and the one the demod was last handed both match `db` (0 = off), and the
 * production frame-sync gate, fed the fixed channel power, skips (closed) or passes (open). */
static int
expect_gate(const dsd_opts* opts, const dsd_state* state, const char* stage, int db, int closed) {
    const int skip = frame_sync_should_skip_snr_or_power_gate(opts, state);
    if (!level_is(opts->rtl_squelch_level, db) || !level_is(g_pushed, db) || skip != closed) {
        DSD_FPRINTF(stderr, "FAIL: %s: level %.3g pushed %.3g gate %s, want %d dB gate %s\n", stage,
                    opts->rtl_squelch_level, g_pushed, skip ? "closed" : "open", db, closed ? "closed" : "open");
        return 1;
    }
    return 0;
}

static int
add_row(dsd_state* state, int row, dsd_scan_mode mode, int squelch_set, int squelch_db) {
    *dsd_state_trunk_lcn_slot(state, row) = 451000000L + 1000000L * row;
    if (dsd_channel_mode_set(state, (size_t)row, mode) != 0) {
        return 1;
    }
    if (!squelch_set) {
        return 0;
    }
    dsd_scan_row_profile* profile = (dsd_scan_row_profile*)calloc(1, sizeof(*profile));
    if (!profile) {
        return 1;
    }
    profile->values.present = DSD_SCAN_OPT_SQUELCH;
    profile->values.squelch_db = squelch_db;
    if (dsd_channel_profile_set(state, (size_t)row, profile) != 0) {
        free(profile);
        return 1;
    }
    return 0;
}

static int
test_row_squelch_drives_frame_sync_gate(dsd_opts* opts, dsd_state* state) {
    opts->verbose = 0;
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->audio_out_type = 9;
    /* A DMR-only baseline, so the configured default gates frame sync (GFSK) before and after
     * the scan as well as on the rows. */
    opts->frame_p25p1 = opts->frame_p25p2 = opts->frame_dstar = opts->frame_x2tdma = 0;
    opts->frame_nxdn48 = opts->frame_nxdn96 = opts->frame_dpmr = opts->frame_provoice = 0;
    opts->frame_ysf = opts->frame_m17 = 0;
    opts->frame_dmr = 1;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-65.0);
    /* The channel power every gate below sees: between the row thresholds. */
    opts->rtl_pwr = dsd_squelch_level_from_sql(-70.0);
    state->samplesPerSymbol = 10;
    state->lcn_freq_count = 4;
    if (add_row(state, 0, DSD_SCAN_MODE_DMR, 1, -80) || add_row(state, 1, DSD_SCAN_MODE_DMR, 1, -60)
        || add_row(state, 2, DSD_SCAN_MODE_NXDN48, 1, 0) || add_row(state, 3, DSD_SCAN_MODE_NXDN48, 0, 0)) {
        DSD_FPRINTF(stderr, "FAIL: could not build the channel map\n");
        return 1;
    }
    const dsd_rtl_stream_metrics_hooks hooks = {.output_rate_hz = output_rate, .set_channel_squelch = record_push};
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    /* Before the scan the configured -65 dB closes the gate on the -70 dB channel. */
    int rc = expect(frame_sync_should_skip_snr_or_power_gate(opts, state) == 1, "default closes before the scan");

    static const struct {
        const char* stage;
        int db;
        int closed;
    } visits[] = {
        {"row that opens", -80, 0},
        {"row that closes", -60, 1},
        {"row that switches the squelch off", 0, 0},
        {"row that inherits the default", -65, 1},
        {"rotation back to the row that opens", -80, 0},
    };

    for (size_t i = 0; i < sizeof visits / sizeof visits[0]; i++) {
        rc |= expect(dsd_engine_channel_scan_step(opts, state) == 1, visits[i].stage);
        rc |= expect(state->lcn_freq_roll == (int)(i % 4) + 1, visits[i].stage);
        rc |= expect_gate(opts, state, visits[i].stage, visits[i].db, visits[i].closed);
    }
    rc |= expect(g_tunes == 5, "every row retuned once");

    const int pushes_before_leave = g_pushes;
    dsd_engine_channel_scan_leave(opts, state);
    rc |= expect(g_pushes == pushes_before_leave + 1, "leaving hands the demod the default once");
    rc |= expect_gate(opts, state, "scan teardown", -65, 1);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    return rc;
}

int
main(void) {
    dsd_neo_config_init();
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    const int rc = test_row_squelch_drives_frame_sync_gate(opts, state);
    dsd_trunk_tuning_requests_reset();
    freeState(state);
    free(state);
    free(opts);
    return rc;
}
