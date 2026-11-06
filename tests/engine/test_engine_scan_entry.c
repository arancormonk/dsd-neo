// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The row a frequency typed by hand picks (issue #644 D4, D5): dsd_engine_scan_entry_match() over a -Y list and over a
 * real trunk-scan coordinator, with every reason a row on the frequency is not eligible. Only the tuner is simulated:
 * the trunk tuning hooks answer for the backend. */
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_entry.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "scan_landing.h"
#include "test_env.h"
#include "test_support.h"

enum {
    k_fire_hz = 154430000,
    k_ops_hz = 155475000,
    k_p25_hz = 851012500,
};

static int
expect(int condition, const char* message) {
    if (!condition) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

/* What a match should answer. */
typedef struct {
    dsd_scan_entry_scanner scanner;
    int row;
    unsigned ordinal;
    unsigned count;
    int on_air;
    int trunked;
    dsd_scan_entry_reason reason;
    const char* label;
} want_match;

static int
expect_match(const char* tag, const dsd_opts* opts, const dsd_state* state, uint32_t hz, want_match want) {
    dsd_scan_entry_match_t got;
    DSD_MEMSET(&got, 0x5A, sizeof(got));
    const int rc = dsd_engine_scan_entry_match(opts, state, hz, &got);
    const int ok = rc == (want.scanner != DSD_SCAN_ENTRY_NONE) && got.scanner == want.scanner && got.row == want.row
                   && got.ordinal == want.ordinal && got.count == want.count && got.on_air == want.on_air
                   && got.trunked == want.trunked && got.reason == want.reason && strcmp(got.label, want.label) == 0;
    if (!ok) {
        DSD_FPRINTF(stderr,
                    "FAIL: %s: rc=%d scanner=%d row=%d %u of %u on_air=%d trunked=%d reason=%d label=\"%.63s\"; want "
                    "scanner=%d row=%d %u of %u on_air=%d trunked=%d reason=%d label=\"%s\"\n",
                    tag, rc, (int)got.scanner, got.row, got.ordinal, got.count, got.on_air, got.trunked,
                    (int)got.reason, got.label, (int)want.scanner, want.row, want.ordinal, want.count, want.on_air,
                    want.trunked, (int)want.reason, want.label);
        return 1;
    }
    return 0;
}

static want_match
channel_row(int row, unsigned ordinal, unsigned count, const char* label) {
    return (want_match){DSD_SCAN_ENTRY_CHANNEL, row, ordinal, count, 0, 0, DSD_SCAN_ENTRY_OK, label};
}

static want_match
trunk_row(int row, unsigned ordinal, unsigned count, int trunked, const char* label) {
    return (want_match){DSD_SCAN_ENTRY_TRUNK, row, ordinal, count, 0, trunked, DSD_SCAN_ENTRY_OK, label};
}

static want_match
not_eligible(dsd_scan_entry_reason reason, int row, int trunked, const char* label) {
    return (want_match){DSD_SCAN_ENTRY_NONE, row, 0U, 0U, 0, trunked, reason, label};
}

static const want_match k_no_row = {DSD_SCAN_ENTRY_NONE, -1, 0U, 0U, 0, 0, DSD_SCAN_ENTRY_NO_ROW, ""};

static int
session_new(dsd_opts** opts, dsd_state** state) {
    *opts = (dsd_opts*)calloc(1, sizeof(**opts));
    *state = (dsd_state*)calloc(1, sizeof(**state));
    if (!*opts || !*state) {
        free(*opts);
        free(*state);
        *opts = NULL;
        *state = NULL;
        return 1;
    }
    initOpts(*opts);
    initState(*state);
    (*opts)->audio_in_type = AUDIO_IN_NULL;
    return 0;
}

static void
session_free(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        free(opts);
        free(state);
        return;
    }
    dsd_engine_trunk_scan_shutdown(opts, state);
    freeState(state);
    free(opts);
    free(state);
}

/* A -Y list of the user's own, running, with @p count rows from @p hz (0 is a placeholder). */
static void
y_list(dsd_opts* opts, dsd_state* state, const long* hz, int count) {
    opts->scanner_mode = 1;
    DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", "channels.csv");
    for (int i = 0; i < count; i++) {
        state->trunk_lcn_freq[i] = hz[i];
    }
    state->lcn_freq_count = count;
}

static int
set_row_width(dsd_state* state, int row, int width_hz) {
    dsd_scan_row_profile* profile = (dsd_scan_row_profile*)calloc(1, sizeof(*profile));
    if (!profile) {
        return 1;
    }
    profile->values.present = DSD_SCAN_OPT_BANDWIDTH;
    profile->values.channel_bw_hz = width_hz;
    if (dsd_channel_profile_set(state, (size_t)row, profile) != 0) {
        free(profile);
        return 1;
    }
    return 0;
}

/* No list in force, or no list of the user's own: nothing matches, whatever rows the state holds. */
static int
test_nothing_matches_without_a_list_in_force(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        return 1;
    }
    int rc = expect_match("no scan", opts, state, k_fire_hz, k_no_row);
    rc |= expect(dsd_engine_scan_entry_match(NULL, state, k_fire_hz, NULL) == 0, "NULL arguments match nothing");
    const long rows[] = {k_fire_hz, k_ops_hz};
    y_list(opts, state, rows, 2);
    rc |= expect_match("a -Y row", opts, state, k_fire_hz, channel_row(0, 1U, 1U, "row 1"));
    rc |= expect_match("0 Hz", opts, state, 0U, k_no_row);
    rc |= expect_match("a frequency on no row", opts, state, k_fire_hz + 1U, k_no_row);
    /* A map the stopped scan left is no list a typed frequency picks a row of. */
    opts->scanner_mode = 0;
    rc |= expect_match("a stopped scan's map", opts, state, k_fire_hz, k_no_row);
    /* Under -T the same map is loaded as a list of the user's own; what excludes it is that no -Y scan runs. */
    opts->trunk_enable = 1;
    rc |= expect(dsd_state_trunk_lcn_user_list_present(opts, state) == 1, "a -T map is the user's own list");
    rc |= expect_match("a -T channel map", opts, state, k_fire_hz, k_no_row);
    opts->trunk_enable = 0;
    /* Channels learned over the air are no list of the user's own. */
    opts->scanner_mode = 1;
    opts->chan_in_file[0] = '\0';
    rc |= expect_match("learned channels", opts, state, k_fire_hz, k_no_row);
    session_free(opts, state);
    return rc;
}

/* A paused -Y scan, and one whose resume is still pending, are the scan in force. */
static int
test_paused_y_scan_matches(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        return 1;
    }
    const long rows[] = {k_fire_hz, k_ops_hz};
    y_list(opts, state, rows, 2);
    opts->scanner_mode = 0;
    state->lcn_scan_paused = 1U;
    int rc = expect_match("paused", opts, state, k_ops_hz, channel_row(1, 1U, 1U, "row 2"));
    state->lcn_scan_resume_pending = 1U;
    rc |= expect_match("resume pending", opts, state, k_ops_hz, channel_row(1, 1U, 1U, "row 2"));
    session_free(opts, state);
    return rc;
}

/* Exact Hz only, the row's label else "row N", placeholders never, and the frequency typed compared whole, before
 * anything narrows it to a long. */
static int
test_y_row_exact_frequency_and_name(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        return 1;
    }
    const long rows[] = {k_fire_hz, 0L, k_ops_hz};
    y_list(opts, state, rows, 3);
    int rc = expect(dsd_state_trunk_lcn_name_set(state, 0U, "fire-dispatch") == 0, "row 0 named");
    rc |= expect_match("a named row", opts, state, k_fire_hz, channel_row(0, 1U, 1U, "fire-dispatch"));
    rc |= expect_match("an unnamed row", opts, state, k_ops_hz, channel_row(2, 1U, 1U, "row 3"));
    rc |= expect_match("1 Hz off", opts, state, k_ops_hz - 1U, k_no_row);
#if LONG_MAX > 0xFFFFFFFFL
    /* A row above 4294.967295 MHz is never the frequency typed whose low 32 bits it shares. */
    state->trunk_lcn_freq[1] = (long)k_fire_hz + 4294967296L;
    rc |= expect_match("a row 2^32 Hz above", opts, state, k_fire_hz, channel_row(0, 1U, 1U, "fire-dispatch"));
    /* A frequency above 2147.483647 MHz matches its row exactly. */
    state->trunk_lcn_freq[1] = 3000000000L;
    rc |= expect_match("above 2^31 Hz", opts, state, 3000000000U, channel_row(1, 1U, 1U, "row 2"));
#endif
    session_free(opts, state);
    return rc;
}

/* An avoided row, and an analog row whose width does not fit (an explicit width with the channel filter turned off on a
 * radio input, an am row on an audio input nothing demodulates AM on), is no pick: the reason names it. */
static int
test_y_ineligible_rows_name_their_reason(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        return 1;
    }
    const long rows[] = {k_fire_hz, k_ops_hz, k_p25_hz};
    y_list(opts, state, rows, 3);
    int rc = expect(dsd_state_trunk_lcn_name_set(state, 0U, "fire-dispatch") == 0, "row 0 named");
    rc |= expect(dsd_channel_mode_set(state, 0U, DSD_SCAN_MODE_NFM) == 0, "row 0 nfm");
    rc |= expect(dsd_channel_mode_set(state, 1U, DSD_SCAN_MODE_AM) == 0, "row 1 am");
    rc |= expect(dsd_channel_mode_set(state, 2U, DSD_SCAN_MODE_P25) == 0, "row 2 p25");
    rc |= expect(set_row_width(state, 0, 12500) == 0, "row 0 width");
    rc |= expect(dsd_state_trunk_lcn_avoid_set(state, 2U, 1) == 0, "row 2 avoided");
    rc |= expect_match("avoided", opts, state, k_p25_hz, not_eligible(DSD_SCAN_ENTRY_AVOIDED, 2, 0, "row 3"));
    rc |= expect_match("am without a demodulator", opts, state, k_ops_hz,
                       not_eligible(DSD_SCAN_ENTRY_WIDTH, 1, 0, "row 2"));
    opts->use_rigctl = 1;
    rc |= expect_match("am through a rigctl peer", opts, state, k_ops_hz, channel_row(1, 1U, 1U, "row 2"));
    opts->audio_in_type = AUDIO_IN_RTL;
    rc |= expect_match("an explicit width on a radio input", opts, state, k_fire_hz,
                       channel_row(0, 1U, 1U, "fire-dispatch"));
    (void)dsd_test_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();
    rc |= expect_match("an explicit width with the channel filter off", opts, state, k_fire_hz,
                       not_eligible(DSD_SCAN_ENTRY_WIDTH, 0, 0, "fire-dispatch"));
    (void)dsd_test_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();
    session_free(opts, state);
    return rc;
}

/* Several typed rows on one frequency (nxdn and nxdn48): the first in file order, named "1 of 2"; one not eligible is
 * passed over and not counted; the row confirmed on air wins over file order, and equal frequency alone confirms
 * nothing. */
static int
test_y_rows_sharing_a_frequency(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        return 1;
    }
    const long rows[] = {k_p25_hz, k_p25_hz, k_ops_hz, k_p25_hz};
    y_list(opts, state, rows, 4);
    int rc = expect(dsd_channel_mode_set(state, 0U, DSD_SCAN_MODE_NXDN96) == 0, "row 0 nxdn");
    rc |= expect(dsd_channel_mode_set(state, 1U, DSD_SCAN_MODE_NXDN48) == 0, "row 1 nxdn48");
    rc |= expect(dsd_channel_mode_set(state, 3U, DSD_SCAN_MODE_P25) == 0, "row 3 p25");
    rc |= expect_match("three rows", opts, state, k_p25_hz, channel_row(0, 1U, 3U, "row 1"));
    rc |= expect(dsd_state_trunk_lcn_avoid_set(state, 0U, 1) == 0, "row 0 avoided");
    rc |= expect_match("the first avoided", opts, state, k_p25_hz, channel_row(1, 1U, 2U, "row 2"));
    rc |= expect(dsd_state_trunk_lcn_avoid_set(state, 0U, 0) == 0, "row 0 back");
    rc |= expect_match("none on air", opts, state, k_p25_hz, channel_row(0, 1U, 3U, "row 1"));
    dsd_engine_scan_landing_note_row(state, 3, k_p25_hz);
    want_match on_air = channel_row(3, 3U, 3U, "row 4");
    on_air.on_air = 1;
    rc |= expect_match("the row on air", opts, state, k_p25_hz, on_air);
    /* A landing in another scan session is no proof. */
    state->lcn_scan_session++;
    rc |= expect_match("an earlier session's landing", opts, state, k_p25_hz, channel_row(0, 1U, 3U, "row 1"));
    session_free(opts, state);
    return rc;
}

static want_match
on_air_row(int row, unsigned ordinal, unsigned count, const char* label) {
    want_match want = channel_row(row, ordinal, count, label);
    want.on_air = 1;
    return want;
}

/* An untyped list: a run of adjacent rows on one frequency is one channel; the same frequency further down the list is
 * another channel. The row of the run the scan is confirmed on speaks for the run, avoided neighbours or not, while
 * that row is in the rotation; with none confirmed, the run's first row decides. */
static int
test_untyped_adjacent_rows_are_one_channel(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        return 1;
    }
    const long rows[] = {k_fire_hz, k_fire_hz, k_ops_hz, k_fire_hz};
    y_list(opts, state, rows, 4);
    int rc = expect_match("a run and a later row", opts, state, k_fire_hz, channel_row(0, 1U, 2U, "row 1"));
    /* The timed step visits every row of a run: one landed on its second row. */
    dsd_engine_scan_landing_note_row(state, 1, k_fire_hz);
    rc |= expect_match("the run's second row on air", opts, state, k_fire_hz, on_air_row(1, 1U, 2U, "row 2"));
    rc |= expect(dsd_state_trunk_lcn_avoid_set(state, 0U, 1) == 0, "row 0 avoided");
    rc |= expect_match("on air beside the run's avoided first row", opts, state, k_fire_hz,
                       on_air_row(1, 1U, 2U, "row 2"));
    rc |= expect(dsd_state_trunk_lcn_avoid_set(state, 1U, 1) == 0, "row 1 avoided");
    rc |= expect_match("the row on air avoided too", opts, state, k_fire_hz, channel_row(3, 1U, 1U, "row 4"));
    rc |= expect(dsd_state_trunk_lcn_avoid_set(state, 0U, 0) == 0, "row 0 back");
    rc |= expect_match("the row on air avoided alone", opts, state, k_fire_hz, channel_row(0, 1U, 2U, "row 1"));
    dsd_engine_scan_landing_forget(state);
    rc |= expect_match("a run's later row avoided", opts, state, k_fire_hz, channel_row(0, 1U, 2U, "row 1"));
    rc |= expect(dsd_state_trunk_lcn_avoid_set(state, 0U, 1) == 0, "row 0 avoided again");
    rc |= expect_match("the run's first row avoided", opts, state, k_fire_hz, channel_row(3, 1U, 1U, "row 4"));
    rc |= expect(dsd_state_trunk_lcn_avoid_set(state, 1U, 0) == 0, "row 1 back");
    rc |= expect_match("none on air, the first row avoided", opts, state, k_fire_hz, channel_row(3, 1U, 1U, "row 4"));
    rc |= expect(dsd_state_trunk_lcn_avoid_set(state, 3U, 1) == 0, "row 3 avoided");
    rc |= expect_match("every channel avoided", opts, state, k_fire_hz,
                       not_eligible(DSD_SCAN_ENTRY_AVOIDED, 0, 0, "row 1"));
    session_free(opts, state);
    return rc;
}

/* ---- The trunk scan --------------------------------------------------------------------------------------------- */

static char g_dir[DSD_TEST_PATH_MAX];
static char g_list_a[DSD_TEST_PATH_MAX];
static char g_list_b[DSD_TEST_PATH_MAX];

/* A trunked target's control-channel park: the backend takes it at once. A conventional target's park goes to the
   rigctl peer, which no test session has, so it fails and its target cools down. */
static dsd_trunk_tune_result
cc_tune(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t request_id) {
    (void)opts;
    (void)sps;
    (void)request_id;
    state->trunk_cc_freq = freq;
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

/* The conventional parks a walk fails on purpose print their retune warnings, which name each target's cooldown: they
   are captured rather than printed, from quiet_begin() to quiet_end(), which wants at least one. */
static int
quiet_begin(dsd_test_capture_stderr* cap) {
    return expect(dsd_test_capture_stderr_begin(cap, "scanentry") == 0, "stderr captured");
}

static int
quiet_end(dsd_test_capture_stderr* cap, const char* what) {
    char text[4096] = {0};
    (void)dsd_test_capture_stderr_end(cap);
    (void)dsd_test_capture_stderr_read(cap, text, sizeof text);
    if (!strstr(text, "retune failed; cooling down briefly")) {
        DSD_FPRINTF(stderr, "FAIL: %s failed no park; its walk said:\n%s\n", what, text);
        return 1;
    }
    return 0;
}

/* `b` on the target on air, answering @p want, through a walk that fails conventional parks. */
static int
avoid_active_quietly(dsd_opts* opts, dsd_state* state, int want) {
    dsd_test_capture_stderr cap;
    if (quiet_begin(&cap) != 0) {
        return 1;
    }
    const int got = dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE);
    return quiet_end(&cap, "the avoid") | expect(got == want, "the avoid's answer");
}

static const char k_targets_header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options,label\n";

/* A trunk scan over list A (and list B when @p body_b is given, switched off at start with @p b_off), parked on its
 * first target, held so no dwell rotates it. */
static int
trunk_scan_start(dsd_opts** opts, dsd_state** state, const char* body_a, const char* body_b, int b_off) {
    if (session_new(opts, state) != 0) {
        return 1;
    }
    (*opts)->use_rigctl = 1;
    (*opts)->trunk_scan_enabled = 1;
    dsd_trunk_tuning_requests_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_cc_request = cc_tune});
    char a[2048];
    char b[2048];
    DSD_SNPRINTF(a, sizeof a, "%s%s", k_targets_header, body_a);
    DSD_SNPRINTF(b, sizeof b, "%s%s", k_targets_header, body_b ? body_b : "");
    if (!dsd_test_mkdtemp(g_dir, sizeof(g_dir), "dsdneo_scan_entry")
        || dsd_test_path_join(g_list_a, sizeof(g_list_a), g_dir, "a.csv") != 0
        || dsd_test_path_join(g_list_b, sizeof(g_list_b), g_dir, "b.csv") != 0 || write_file(g_list_a, a)
        || (body_b && write_file(g_list_b, b))) {
        return 1;
    }
    DSD_SNPRINTF((*opts)->trunk_scan_lists[0].path, sizeof(*opts)->trunk_scan_lists[0].path, "%s", g_list_a);
    (*opts)->trunk_scan_list_count = 1;
    if (body_b) {
        DSD_SNPRINTF((*opts)->trunk_scan_lists[1].path, sizeof(*opts)->trunk_scan_lists[1].path, "%s", g_list_b);
        (*opts)->trunk_scan_lists[1].start_off = b_off ? 1U : 0U;
        (*opts)->trunk_scan_list_count = 2;
    }
    char err[256] = {0};
    if (dsd_engine_trunk_scan_init(*opts, *state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "trunk scan init: %s\n", err);
        return 1;
    }
    return dsd_engine_trunk_scan_control(*opts, *state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE) == 1 ? 0 : 1;
}

static void
trunk_scan_end(dsd_opts* opts, dsd_state* state) {
    session_free(opts, state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    (void)remove(g_list_a);
    (void)remove(g_list_b);
#if DSD_PLATFORM_WIN_NATIVE
    (void)_rmdir(g_dir);
#else
    (void)rmdir(g_dir);
#endif
}

/* Copies of one channel in two lists, nxdn and nxdn48 on one frequency, and a conventional and a trunked target on one
 * frequency: every eligible target counts, in list order (list number, then file order), the label else the id names
 * it, the one confirmed on air wins, an avoided one is passed over, and one cooling down after a failed retune counts:
 * the pick is explicit. */
static int
test_trunk_targets_sharing_a_frequency(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = trunk_scan_start(&opts, &state,
                              "county,p25-trunk,851012500,,250,250,,,County P25\n"
                              "nx,nxdn-conventional,155475000,,250,250,,,\n"
                              "nx48,nxdn48-conventional,155475000,,250,250,,,NXDN48 ops\n"
                              "county-conv,p25-conventional,851012500,,250,250,,,\n",
                              "county-copy,p25-trunk,851012500,,250,250,,,County again\n", 0);
    if (rc != 0) {
        trunk_scan_end(opts, state);
        return expect(0, "trunk scan started");
    }
    rc |= expect(dsd_engine_trunk_scan_target_confirmed(state, 0), "the first park confirmed");
    want_match on_air = trunk_row(0, 1U, 3U, 1, "County P25");
    on_air.on_air = 1;
    rc |= expect_match("the trunked target on air", opts, state, k_p25_hz, on_air);
    rc |= expect_match("nxdn and nxdn48", opts, state, k_ops_hz, trunk_row(1, 1U, 2U, 0, "nx"));
    /* `b` on the county system: the conventional targets' parks fail (no tuner takes them) and cool down, and the scan
       parks the copy in list B. */
    rc |= avoid_active_quietly(opts, state, 0);
    rc |= expect(state->trunk_scan_active_ordinal == 5U, "the copy parked");
    on_air = trunk_row(4, 2U, 2U, 1, "County again");
    on_air.on_air = 1;
    rc |= expect_match("avoided, cooling down and on air", opts, state, k_p25_hz, on_air);
    /* Off the air (paused), list order decides. */
    rc |= expect(dsd_engine_trunk_scan_pause_locked(opts, state, DSD_TRUNK_SCAN_PAUSE_IN_PLACE) == 0, "paused");
    rc |= expect_match("paused, list order", opts, state, k_p25_hz, trunk_row(3, 1U, 2U, 0, "county-conv"));
    trunk_scan_end(opts, state);
    return rc;
}

/* A target whose list is off and an avoided one are no pick: with none eligible, the first such in list order names
 * the reason; switching the list on makes its target the pick. */
static int
test_trunk_ineligible_targets_name_their_reason(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = trunk_scan_start(&opts, &state,
                              "county,p25-trunk,851012500,,250,250,,,County P25\n"
                              "ops,nfm-conventional,155475000,,250,250,,--squelch-db -50,\n",
                              "fire-b,nfm-conventional,154445000,,250,250,,--squelch-db -50,Fire B\n"
                              "county-b,p25-trunk,851012500,,250,250,,,\n",
                              1);
    if (rc != 0) {
        trunk_scan_end(opts, state);
        return expect(0, "trunk scan started");
    }
    rc |= expect_match("a list that is off", opts, state, 154445000U,
                       not_eligible(DSD_SCAN_ENTRY_LIST_OFF, 2, 0, "Fire B"));
    want_match on_air = trunk_row(0, 1U, 1U, 1, "County P25");
    on_air.on_air = 1;
    rc |= expect_match("a copy in a list that is off", opts, state, k_p25_hz, on_air);
    /* `b` on the county system: ops's park fails, so the receiver falls back onto county, avoided. */
    rc |= avoid_active_quietly(opts, state, 1);
    rc |= expect_match("avoided, its copy off", opts, state, k_p25_hz,
                       not_eligible(DSD_SCAN_ENTRY_AVOIDED, 0, 1, "County P25"));
    rc |= expect_match("cooling down, named by its id", opts, state, k_ops_hz, trunk_row(1, 1U, 1U, 0, "ops"));
    /* The scan on the avoided target owes a departure, which list B gives it: fire-b's park fails on the way. */
    dsd_test_capture_stderr cap;
    if (quiet_begin(&cap) == 0) {
        const int set = dsd_engine_trunk_scan_list_set(opts, state, state->trunk_scan_session, 2, 1, 1U);
        rc |= quiet_end(&cap, "list B on") | expect(set == DSD_TRUNK_SCAN_LIST_SET_OK, "list B on");
    } else {
        rc = 1;
    }
    on_air = trunk_row(3, 1U, 1U, 1, "county-b");
    on_air.on_air = 1;
    rc |= expect_match("its copy's list on", opts, state, k_p25_hz, on_air);
    trunk_scan_end(opts, state);
    return rc;
}

/* A completed park confirms its target until the receiver may have left it: the operator's tune (its carrier
 * boundary) and a radio stream start (a reopen starts wherever it is told) each take the proof away. */
static int
test_trunk_target_unconfirmed_by_a_tune_or_a_reopen(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = trunk_scan_start(&opts, &state, "county,p25-trunk,851012500,,250,250,,,County P25\n", NULL, 0);
    if (rc != 0) {
        trunk_scan_end(opts, state);
        return expect(0, "trunk scan started");
    }
    rc |= expect(dsd_engine_trunk_scan_target_confirmed(state, 0), "the park confirmed");
    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    rc |= expect(!dsd_engine_trunk_scan_target_confirmed(state, 0), "a manual tune's boundary unconfirms it");
    rc |= expect(dsd_engine_trunk_scan_pause_locked(opts, state, DSD_TRUNK_SCAN_PAUSE_IN_PLACE) == 0
                     && dsd_engine_trunk_scan_resume_locked(opts, state) == 0,
                 "re-parked");
    rc |= expect(dsd_engine_trunk_scan_target_confirmed(state, 0), "the re-park confirmed");
    dsd_engine_note_stream_source(opts, state, 0);
    rc |= expect(!dsd_engine_trunk_scan_target_confirmed(state, 0), "a stream start unconfirms it");
    trunk_scan_end(opts, state);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_nothing_matches_without_a_list_in_force();
    rc |= test_paused_y_scan_matches();
    rc |= test_y_row_exact_frequency_and_name();
    rc |= test_y_ineligible_rows_name_their_reason();
    rc |= test_y_rows_sharing_a_frequency();
    rc |= test_untyped_adjacent_rows_are_one_channel();
    rc |= test_trunk_targets_sharing_a_frequency();
    rc |= test_trunk_ineligible_targets_name_their_reason();
    rc |= test_trunk_target_unconfirmed_by_a_tune_or_a_reopen();
    if (rc == 0) {
        printf("ENGINE_SCAN_ENTRY: OK\n");
    }
    return rc;
}
