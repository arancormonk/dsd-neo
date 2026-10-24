// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The shared squelch readout (issue #521): effective value first, "(row; default X)" while a
 * scan row overrides it, and a notice that says so when an edit of the default is shadowed. */

#include <assert.h>
#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/squelch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
level_is(double level, double db) {
    const double want = dsd_squelch_level_from_sql(db);
    return fabs(level - want) <= 1e-9 * fmax(fabs(level), fabs(want));
}

static void
expect_text(const dsd_app_squelch_view* view, const char* readout, const char* notice) {
    char out[96];
    assert(dsd_app_squelch_view_format(view, out, sizeof out) == 0);
    assert(strcmp(out, readout) == 0);
    assert(dsd_app_squelch_view_edit_notice(view, out, sizeof out) == 0);
    assert(strcmp(out, notice) == 0);
}

/* The dynamic squelch's status on its own, as a frontend that lays it out on a line of its own reads it. */
static void
expect_auto_status(const dsd_app_squelch_view* view, const char* status) {
    char out[40];
    assert(dsd_app_squelch_view_dynamic_status(view, out, sizeof out) == 0);
    assert(strcmp(out, status) == 0);
}

static void
expect_fixed(const dsd_app_squelch_view* view, const char* readout, const char* status) {
    char out[96];
    assert(dsd_app_squelch_view_format_ex(view, DSD_APP_SQUELCH_VIEW_FIXED, out, sizeof out) == 0);
    if (strcmp(out, readout) != 0) {
        DSD_FPRINTF(stderr, "fixed readout '%s', want '%s'\n", out, readout);
        assert(0);
    }
    assert(dsd_app_squelch_view_dynamic_status_ex(view, DSD_APP_SQUELCH_VIEW_FIXED, out, sizeof out) == 0);
    if (strcmp(out, status) != 0) {
        DSD_FPRINTF(stderr, "fixed status '%s', want '%s'\n", out, status);
        assert(0);
    }
}

/* Issue #669: readings that round to zero print without a sign, and the fixed-width text keeps one length while the
   reading changes, so a terminal field after it never moves. */
static void
test_negative_zero_and_fixed_width(void) {
    dsd_app_squelch_view view;
    DSD_MEMSET(&view, 0, sizeof view);
    view.effective_noise = 1;
    view.configured_noise = 1;
    view.effective_margin_db = 12;
    view.configured_margin_db = 12;
    view.auto_resolution = DSD_SQUELCH_RESOLVED_AS_SET;
    view.noise_running = 1;
    view.noise_measured = 1;
    /* -0.01 dB and -0.49 dB both round to zero. */
    view.noise_quieting_db = -0.01;
    expect_text(&view, "noise +12 dB (quieting 0 dB)", "Applied: squelch -> noise +12 dB");
    view.noise_quieting_db = -0.49;
    expect_text(&view, "noise +12 dB (quieting 0 dB)", "Applied: squelch -> noise +12 dB");
    view.noise_quieting_db = -0.51;
    expect_text(&view, "noise +12 dB (quieting -1 dB)", "Applied: squelch -> noise +12 dB");

    /* Fixed width: the reading right-aligned in four places, the readout padded to the longest text NOISE can show
       ("as auto: off: no channel plan", 29), so learning and measuring take the same room. */
    const char* pad13 = "             ";
    char want[96];
    view.noise_quieting_db = 23.49;
    DSD_SNPRINTF(want, sizeof want, "noise +12 dB (quieting   23 dB)%s", pad13);
    expect_fixed(&view, want, "quieting   23 dB             ");
    view.noise_quieting_db = -0.49;
    DSD_SNPRINTF(want, sizeof want, "noise +12 dB (quieting    0 dB)%s", pad13);
    expect_fixed(&view, want, "quieting    0 dB             ");
    view.noise_quieting_db = 200.0;
    DSD_SNPRINTF(want, sizeof want, "noise +12 dB (quieting  200 dB)%s", pad13);
    expect_fixed(&view, want, "quieting  200 dB             ");
    view.noise_quieting_db = -200.0;
    DSD_SNPRINTF(want, sizeof want, "noise +12 dB (quieting -200 dB)%s", pad13);
    expect_fixed(&view, want, "quieting -200 dB             ");
    view.noise_measured = 0;
    expect_fixed(&view, "noise +12 dB (starting)                     ", "starting                     ");
    view.noise_state = DSD_SQUELCH_NOISE_STATE_LEARNING;
    expect_fixed(&view, "noise +12 dB (learning)                     ", "learning                     ");
    view.noise_state = DSD_SQUELCH_NOISE_STATE_NONE;
    view.noise_as_auto = 1;
    view.auto_running = 1;
    view.auto_plan_valid = 0;
    expect_fixed(&view, "noise +12 dB (as auto: off: no channel plan)", "as auto: off: no channel plan");

    /* AUTO: the floor in six places, padded to "off: no channel plan" (20). */
    DSD_MEMSET(&view, 0, sizeof view);
    view.effective_auto = 1;
    view.configured_auto = 1;
    view.effective_margin_db = 10;
    view.configured_margin_db = 10;
    view.auto_resolution = DSD_SQUELCH_RESOLVED_AS_SET;
    view.auto_running = 1;
    view.auto_plan_valid = 1;
    view.auto_floor_db = -0.04;
    expect_text(&view, "auto +10 dB (floor 0.0 dB)", "Applied: squelch -> auto +10 dB");
    expect_fixed(&view, "auto +10 dB (floor    0.0 dB)     ", "floor    0.0 dB     ");
    view.auto_floor_db = -140.0;
    expect_fixed(&view, "auto +10 dB (floor -140.0 dB)     ", "floor -140.0 dB     ");
    view.auto_floor_db = -1400.0;
    expect_fixed(&view, "auto +10 dB (floor <-99.9 dB)     ", "floor <-99.9 dB     ");
    view.auto_learning = 1;
    expect_fixed(&view, "auto +10 dB (learning)            ", "learning            ");
    view.auto_resolution = DSD_SQUELCH_RESOLVED_NO_RADIO;
    expect_fixed(&view, "auto +10 dB (off: no radio input) ", "off: no radio input ");

    /* A level has no reading, so nothing to pad. */
    DSD_MEMSET(&view, 0, sizeof view);
    view.effective_level = dsd_squelch_level_from_sql(-60.0);
    view.configured_level = view.effective_level;
    expect_fixed(&view, "-60.0 dB", "");
    char out[8];
    assert(dsd_app_squelch_view_format_ex(NULL, DSD_APP_SQUELCH_VIEW_FIXED, out, sizeof out) == -1 && out[0] == '\0');
    assert(dsd_app_squelch_view_dynamic_status_ex(&view, DSD_APP_SQUELCH_VIEW_FIXED, NULL, 4) == -1);
}

/* The editor text parses back to the same enabled level (B5): accepting the prompt unchanged must not turn the
   squelch off or move it. */
static void
expect_round_trip(double level) {
    dsd_app_squelch_view view;
    DSD_MEMSET(&view, 0, sizeof view);
    view.configured_level = level;
    view.effective_level = level;
    char text[32];
    assert(dsd_app_squelch_view_configured_text(&view, text, sizeof text) == 0);
    dsd_squelch_setting back;
    if (dsd_squelch_setting_parse(text, &back, NULL, 0U) != 0 || back.mode != DSD_SQUELCH_MODE_LEVEL
        || dsd_squelch_is_off(back.level)) {
        DSD_FPRINTF(stderr, "level %.17g wrote '%s', which does not read back as an enabled level\n", level, text);
        assert(0);
    }
    /* Within the one-place dB rounding: 0.05 dB is a ratio of about 1.16 %; compare by ratio, not absolutely. */
    const double ratio = back.level / level;
    if (!(fabs(10.0 * log10(ratio)) <= 0.0501)) {
        DSD_FPRINTF(stderr, "level %.17g wrote '%s', read back %.17g\n", level, text, back.level);
        assert(0);
    }
}

static void
test_configured_text_round_trip(void) {
    expect_round_trip(1.0);
    expect_round_trip(0.99);
    expect_round_trip(0.9886);
    expect_round_trip(dsd_squelch_level_from_sql(-0.06));
    expect_round_trip(dsd_squelch_level_from_sql(-0.04));
    expect_round_trip(dsd_squelch_level_from_sql(-60.0));
    expect_round_trip(dsd_squelch_level_from_sql(-120.0));
    expect_round_trip(dsd_squelch_level_from_sql(-200.0));
    expect_round_trip(pow(10.0, -200.04 / 10.0));
    expect_round_trip(pow(10.0, -250.0 / 10.0));
    expect_round_trip(1e-30);
    expect_round_trip(4.0);
    dsd_app_squelch_view view;
    DSD_MEMSET(&view, 0, sizeof view);
    view.configured_level = dsd_squelch_level_from_sql(-80.0);
    char text[32];
    assert(dsd_app_squelch_view_configured_text(&view, text, sizeof text) == 0 && strcmp(text, "-80.0") == 0);
    view.configured_level = 1e-30;
    assert(dsd_app_squelch_view_configured_text(&view, text, sizeof text) == 0 && strcmp(text, "1e-30") == 0);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->wav_sample_rate = 48000;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->frame_dmr = 1;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);

    dsd_app_squelch_view view;
    /* No scan scope: one value, no annotation. */
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(!view.row_override && level_is(view.effective_level, -80.0) && level_is(view.configured_level, -80.0));
    expect_text(&view, "-80.0 dB", "Applied: squelch -> -80.0 dB");

    /* A row that inherits is not an override. */
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(!view.row_override);
    expect_text(&view, "-80.0 dB", "Applied: squelch -> -80.0 dB");

    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_SQUELCH;
    row.squelch_db = -60;
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.row_override && level_is(view.effective_level, -60.0) && level_is(view.configured_level, -80.0));
    expect_text(&view, "-60.0 dB (row; default -80.0 dB)",
                "Default squelch -80.0 dB; this channel overrides it (-60.0 dB)");

    /* Suspended for a command, dsd_opts holds the default being edited; the row still shows. */
    assert(dsd_scan_mode_suspend(opts, state));
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-75.0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.row_override && level_is(view.effective_level, -60.0) && level_is(view.configured_level, -75.0));
    expect_text(&view, "-60.0 dB (row; default -75.0 dB)",
                "Default squelch -75.0 dB; this channel overrides it (-60.0 dB)");
    assert(dsd_scan_mode_resume(opts, state) == 0);

    /* Off reads as off on either side of the annotation. */
    row.squelch_db = 0;
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    expect_text(&view, "off (row; default -75.0 dB)", "Default squelch -75.0 dB; this channel overrides it (off)");
    assert(view.effective_off && !view.configured_off);
    assert(fabs(dsd_app_squelch_db_or_off(view.effective_level)) < 1e-12);
    assert(fabs(dsd_app_squelch_db_or_off(view.configured_level) - (-75.0)) < 1e-9);
    dsd_scan_mode_leave(opts, state);
    opts->rtl_squelch_level = 0.0;
    row.squelch_db = -45;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    expect_text(&view, "-45.0 dB (row; default off)", "Default squelch off; this channel overrides it (-45.0 dB)");
    assert(!view.effective_off && view.configured_off);

    /* A frontend snapshot pair reads the same as the live state. */
    dsd_state* copy = (dsd_state*)calloc(1, sizeof(*copy));
    assert(copy);
    dsd_scan_mode_copy_snapshot(copy, state);
    dsd_app_squelch_view snap;
    assert(dsd_app_squelch_view_get(opts, copy, &snap) == 0);
    assert(snap.row_override && level_is(snap.effective_level, -45.0) && dsd_squelch_is_off(snap.configured_level));
    dsd_scan_mode_leave(opts, state);

    /* A legacy linear default at full scale gates everything: it is not off, although its dB
     * reading is 0 like off's. The flags carry the difference to numeric frontends. */
    opts->rtl_squelch_level = 1.0;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(!view.configured_off && !view.effective_off);
    assert(fabs(dsd_app_squelch_db_or_off(view.configured_level)) < 1e-12);
    expect_text(&view, "0.0 dB", "Applied: squelch -> 0.0 dB");

    /* The auto squelch (issue #518 follow-up): its margin, what it shows in force, and why it is off where it is. */
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts->rtl_squelch_margin_db = 10;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.effective_auto && view.configured_auto && view.effective_off);
    expect_text(&view, "auto +10 dB (off: no radio input)", "Applied: squelch -> auto +10 dB");
    expect_auto_status(&view, "off: no radio input");
    opts->audio_in_type = AUDIO_IN_RTL;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    expect_text(&view, "auto +10 dB (off on digital)", "Applied: squelch -> auto +10 dB");
    expect_auto_status(&view, "off on digital");
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->frame_dmr = 0;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(!view.effective_off && !view.auto_running);
    expect_text(&view, "auto +10 dB (learning)", "Applied: squelch -> auto +10 dB");
    expect_auto_status(&view, "learning");
    state->squelch_auto_active = 1;
    state->squelch_auto_plan_valid = 1;
    state->squelch_auto_state = 1;
    state->squelch_auto_floor_cdb = -7830;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.auto_running && !view.auto_learning && fabs(view.auto_floor_db - (-78.3)) < 1e-9);
    expect_text(&view, "auto +10 dB (floor -78.3 dB)", "Applied: squelch -> auto +10 dB");
    expect_auto_status(&view, "floor -78.3 dB");
    state->squelch_auto_plan_valid = 0;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.effective_off);
    expect_text(&view, "auto +10 dB (off: no channel plan)", "Applied: squelch -> auto +10 dB");
    expect_auto_status(&view, "off: no channel plan");
    state->squelch_auto_plan_valid = 1;
    char text[24];
    assert(dsd_app_squelch_view_configured_text(&view, text, sizeof text) == 0 && strcmp(text, "auto+10") == 0);

    /* The noise squelch: "starting" until its first window, its quieting while it runs, "as auto" where the tracker
       stands in for it (an FM channel with no band, an AM channel), and its configured text. */
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    opts->rtl_squelch_margin_db = 12;
    state->squelch_auto_active = 0;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.effective_noise && view.configured_noise && !view.effective_auto && !view.configured_auto);
    assert(!view.effective_off && !view.noise_running && !view.noise_as_auto);
    expect_text(&view, "noise +12 dB (starting)", "Applied: squelch -> noise +12 dB");
    state->squelch_auto_active = 1;
    state->squelch_noise_active = 1;
    state->squelch_auto_gate_open = 1;
    /* Running, but no window measured yet: still starting. */
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.noise_running && !view.noise_measured && !view.noise_as_auto);
    expect_auto_status(&view, "starting");
    state->squelch_noise_measured = 1;
    state->squelch_noise_quieting_cdb = 2349;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.noise_running && view.noise_measured && view.noise_gate_open && !view.auto_running
           && !view.auto_gate_open);
    assert(fabs(view.noise_quieting_db - 23.49) < 1e-9);
    expect_text(&view, "noise +12 dB (quieting 23 dB)", "Applied: squelch -> noise +12 dB");
    expect_auto_status(&view, "quieting 23 dB");
    state->squelch_noise_active = 0;
    state->squelch_noise_measured = 0;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.noise_as_auto && view.auto_running && view.auto_gate_open && !view.noise_gate_open);
    expect_text(&view, "noise +12 dB (as auto: floor -78.3 dB)", "Applied: squelch -> noise +12 dB");
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    state->squelch_auto_active = 0;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.auto_resolution == DSD_SQUELCH_RESOLVED_AM_AUTO && view.noise_as_auto && !view.effective_off);
    expect_auto_status(&view, "as auto: learning");
    assert(dsd_app_squelch_view_configured_text(&view, text, sizeof text) == 0 && strcmp(text, "noise+12") == 0);
    /* On AM audio input the noise squelch is off: no discriminator to measure, and no auto to stand in. */
    opts->audio_in_type = AUDIO_IN_WAV;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.effective_off && view.auto_resolution == DSD_SQUELCH_RESOLVED_AUDIO_AM && !view.noise_as_auto);
    expect_auto_status(&view, "off on AM audio");

    /* On audio input's FM monitor (issue #628) the PCM noise squelch runs it: "starting" before it published,
       "learning" until it holds a reference and has read a window, its quieting after, and off with no band or no
       room above voice. */
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    state->squelch_auto_active = 0;
    state->squelch_noise_active = 0;
    state->squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NONE;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.auto_resolution == DSD_SQUELCH_RESOLVED_AS_SET && !view.effective_off && !view.noise_as_auto);
    expect_auto_status(&view, "starting");
    state->squelch_auto_active = 1;
    state->squelch_noise_active = 1;
    state->squelch_auto_gate_open = 0;
    state->squelch_noise_state = DSD_SQUELCH_NOISE_STATE_LEARNING;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.noise_running && !view.noise_as_auto && !view.effective_off);
    expect_text(&view, "noise +12 dB (learning)", "Applied: squelch -> noise +12 dB");
    state->squelch_noise_state = DSD_SQUELCH_NOISE_STATE_KNOWN;
    state->squelch_noise_measured = 1;
    state->squelch_noise_quieting_cdb = 1810;
    state->squelch_auto_gate_open = 1;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.noise_gate_open && !view.effective_off);
    expect_text(&view, "noise +12 dB (quieting 18 dB)", "Applied: squelch -> noise +12 dB");
    state->squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NO_BAND;
    state->squelch_noise_measured = 0;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.effective_off);
    expect_text(&view, "noise +12 dB (off: no band above voice)", "Applied: squelch -> noise +12 dB");
    state->squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NO_ROOM;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.effective_off);
    expect_auto_status(&view, "off: no room above voice");
    /* AUTO on audio input stays off. */
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.effective_off);
    expect_auto_status(&view, "off: no radio input");
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    state->squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NONE;
    state->squelch_noise_active = 0;
    state->squelch_auto_active = 0;
    state->squelch_auto_gate_open = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts->rtl_squelch_margin_db = 10;
    state->squelch_auto_active = 1;
    state->squelch_auto_gate_open = 0;
    state->squelch_noise_quieting_cdb = 0;

    /* A row's own auto under a level default, and a row's level under an auto default. */
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM) == 0);
    row.squelch_db = 0;
    row.squelch_mode = DSD_SQUELCH_MODE_AUTO;
    row.squelch_margin_db = 6;
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    state->squelch_auto_floor_cdb = -8100;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(view.row_override && view.effective_auto && !view.configured_auto);
    expect_text(&view, "auto +6 dB (floor -81.0 dB; row; default -80.0 dB)",
                "Default squelch -80.0 dB; this channel overrides it (auto +6 dB)");
    expect_auto_status(&view, "floor -81.0 dB");
    assert(dsd_app_squelch_view_configured_text(&view, text, sizeof text) == 0 && strcmp(text, "-80.0") == 0);
    dsd_scan_mode_leave(opts, state);
    const dsd_squelch_setting auto8 = dsd_squelch_setting_auto(8);
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM) == 0);
    row.squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    row.squelch_db = -60;
    assert(dsd_scan_mode_options(opts, state, &row) == 0);
    assert(dsd_scan_mode_set_configured_squelch_setting(opts, state, &auto8) == 0);
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    expect_text(&view, "-60.0 dB (row; default auto +8 dB)",
                "Default squelch auto +8 dB; this channel overrides it (-60.0 dB)");
    /* A level in force shows no auto status, whatever the default beneath it. */
    expect_auto_status(&view, "");
    dsd_scan_mode_leave(opts, state);
    opts->rtl_squelch_level = 0.0;
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    assert(dsd_app_squelch_view_get(opts, state, &view) == 0);
    assert(dsd_app_squelch_view_configured_text(&view, text, sizeof text) == 0 && strcmp(text, "off") == 0);
    assert(dsd_app_squelch_view_configured_text(NULL, text, sizeof text) == -1 && text[0] == '\0');
    text[0] = 'x';
    assert(dsd_app_squelch_view_dynamic_status(NULL, text, sizeof text) == -1 && text[0] == '\0');
    assert(dsd_app_squelch_view_dynamic_status(&view, NULL, sizeof text) == -1);
    assert(dsd_app_squelch_view_dynamic_status(&view, text, 0) == -1);

    /* Issue #625: the M17 encoder's monitor and EDACS analog voice are analog channels for the dynamic squelch. */
    dsd_opts* enc = (dsd_opts*)calloc(1, sizeof(*enc));
    dsd_state* est = (dsd_state*)calloc(1, sizeof(*est));
    assert(enc && est);
    enc->audio_in_type = AUDIO_IN_RTL;
    enc->m17encoder = 1;
    enc->frame_dmr = 1; /* -fZ leaves the default digital frame flags set */
    enc->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    enc->rtl_squelch_margin_db = 10;
    est->squelch_auto_active = 1;
    est->squelch_auto_plan_valid = 1;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    expect_text(&view, "auto +10 dB (learning)", "Applied: squelch -> auto +10 dB");
    est->squelch_auto_state = 1;
    est->squelch_auto_floor_cdb = -7830;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    expect_auto_status(&view, "floor -78.3 dB");
    enc->rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    est->squelch_noise_active = 1;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    expect_auto_status(&view, "starting");
    est->squelch_noise_active = 0;
    /* On audio input neither setting runs for the encoder (the PCM noise squelch serves the FM monitor). */
    enc->audio_in_type = AUDIO_IN_UDP;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    expect_auto_status(&view, "off: no radio input");
    enc->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    expect_auto_status(&view, "off: no radio input");

    /* EDACS analog voice: what the squelch is for between calls, the tracker during one, NOISE as AUTO. */
    DSD_MEMSET(enc, 0, sizeof(*enc));
    DSD_MEMSET(est, 0, sizeof(*est));
    enc->audio_in_type = AUDIO_IN_RTL;
    enc->frame_provoice = 1;
    enc->trunk_enable = 1;
    enc->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    enc->rtl_squelch_margin_db = 10;
    est->squelch_auto_active = 1;
    est->squelch_auto_plan_valid = 1;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    assert(view.edacs_voice && !view.edacs_call && !view.effective_off);
    expect_text(&view, "auto +10 dB (EDACS analog calls)", "Applied: squelch -> auto +10 dB");
    est->squelch_edacs_call = 1;
    est->squelch_auto_state = 1;
    est->squelch_auto_floor_cdb = -8100;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    expect_auto_status(&view, "floor -81.0 dB");
    enc->rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    assert(view.noise_as_auto);
    expect_auto_status(&view, "as auto: floor -81.0 dB");
    est->squelch_edacs_call = 0;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    expect_auto_status(&view, "EDACS analog calls, as auto");
    enc->audio_in_type = AUDIO_IN_TCP;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    expect_auto_status(&view, "off: no radio input");
    /* ProVoice without trunking follows no analog call: a digital channel. */
    enc->audio_in_type = AUDIO_IN_RTL;
    enc->trunk_enable = 0;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    expect_auto_status(&view, "off on digital");
    /* Auto trunking enables ProVoice too, but with no EDACS addressing selected it is no EDACS session: the dynamic
       squelch is off on its digital channels, and selecting EDACS (standard addressing, as -fh -fa) makes it one. */
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, enc, est) == 0);
    enc->trunk_enable = 1;
    est->ea_mode = -1;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    assert(!view.edacs_voice);
    expect_auto_status(&view, "off on digital");
    est->ea_mode = 0;
    assert(dsd_app_squelch_view_get(enc, est, &view) == 0);
    assert(view.edacs_voice && !view.edacs_call);
    expect_auto_status(&view, "EDACS analog calls, as auto");
    dsd_state_ext_free_all(est);
    free(est);
    free(enc);

    test_negative_zero_and_fixed_width();
    test_configured_text_round_trip();

    char out[8];
    assert(dsd_app_squelch_view_get(NULL, state, &view) == -1 && !view.row_override);
    assert(dsd_app_squelch_view_get(opts, state, NULL) == -1);
    assert(dsd_app_squelch_view_format(&view, NULL, 0) == -1);
    assert(dsd_app_squelch_view_format(NULL, out, sizeof out) == -1 && out[0] == '\0');
    assert(dsd_app_squelch_view_edit_notice(NULL, out, sizeof out) == -1 && out[0] == '\0');

    dsd_state_ext_free_all(copy);
    dsd_state_ext_free_all(state);
    free(copy);
    free(state);
    free(opts);
    return 0;
}
