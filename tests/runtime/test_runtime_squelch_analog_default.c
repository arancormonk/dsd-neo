// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The analog default squelch (--squelch-analog-default, issue #899): in force on an analog channel of a radio input
 * while nothing set a squelch explicitly, out everywhere else with the session's own setting back exactly as it was.
 * Every source of an explicit squelch ends it for the session; a scan row's own squelch overrides it for that row only.
 * On a standalone AM monitor a NOISE default runs as AUTO, and the notice says so at each start of it.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/input_spec.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/squelch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_support.h"

static int g_pushes;
static dsd_squelch_setting g_pushed;

static void
record_push(const dsd_squelch_setting* setting) {
    g_pushes++;
    g_pushed = *setting;
}

static int
level_is(double got, double want) {
    return fabs(got - want) <= 1e-12 + 1e-9 * fabs(want);
}

/* A radio session on the -fA monitor whose own squelch is @p base_db (0: off), with --squelch-analog-default
   noise+10. */
static void
session_init(dsd_opts** out_opts, dsd_state** out_state, int base_db) {
    dsd_opts* o = (dsd_opts*)calloc(1, sizeof(*o));
    dsd_state* s = (dsd_state*)calloc(1, sizeof(*s));
    assert(o && s);
    o->wav_sample_rate = 48000;
    o->audio_in_type = AUDIO_IN_RTL;
    o->rtl_dsp_bw_khz = 48;
    o->analog_only = 1;
    o->analog_demod = DSD_ANALOG_DEMOD_FM;
    o->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    o->rtl_squelch_level = dsd_squelch_level_from_sql((double)base_db);
    o->rtl_squelch_margin_db = DSD_SQUELCH_MARGIN_DEFAULT_DB;
    assert(dsd_squelch_analog_default_parse(o, "noise+10", NULL, 0U) == 0);
    *out_opts = o;
    *out_state = s;
}

static void
session_free(dsd_opts* o, dsd_state* s) {
    dsd_state_ext_free_all(s);
    free(s);
    free(o);
}

static int
runs_default(const dsd_opts* o) {
    return o->rtl_squelch_mode == DSD_SQUELCH_MODE_NOISE && o->rtl_squelch_margin_db == 10
           && o->squelch_default_state == DSD_SQUELCH_DEFAULT_APPLIED;
}

static int
runs_level(const dsd_opts* o, int db) {
    return o->rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL
           && level_is(o->rtl_squelch_level, dsd_squelch_level_from_sql((double)db))
           && o->squelch_default_state != DSD_SQUELCH_DEFAULT_APPLIED;
}

static void
test_parse(void) {
    dsd_opts* o = (dsd_opts*)calloc(1, sizeof(*o));
    assert(o);
    char err[128];
    assert(dsd_squelch_analog_default_parse(o, "auto", err, sizeof err) == 0);
    assert(o->squelch_default_mode == DSD_SQUELCH_MODE_AUTO && o->squelch_default_margin_db == 10);
    assert(dsd_squelch_analog_default_parse(o, "NOISE+17", err, sizeof err) == 0);
    assert(o->squelch_default_mode == DSD_SQUELCH_MODE_NOISE && o->squelch_default_margin_db == 17);
    assert(dsd_squelch_analog_default_pending(o));
    /* Refused, the options untouched: off (no default is the option left out), a level, a margin out of range, text
       that is no setting. */
    assert(dsd_squelch_analog_default_parse(o, "auto+12", err, sizeof err) == 0);
    static const char* const refused[] = {"off", "OFF", "0", "-60", "0.5", "auto+2", "noise+31", "loud", ""};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        err[0] = '\0';
        assert(dsd_squelch_analog_default_parse(o, refused[i], err, sizeof err) == -1 && err[0] != '\0');
        /* The reason never repeats the text (an off names "off" itself, which is the grammar's word). */
        assert(refused[i][0] == '\0' || i < 3U || strstr(err, refused[i]) == NULL);
        assert(o->squelch_default_mode == DSD_SQUELCH_MODE_AUTO && o->squelch_default_margin_db == 12);
    }
    err[0] = '\0';
    assert(dsd_squelch_analog_default_parse(o, "-60", err, sizeof err) == -1 && strstr(err, "not a level"));
    assert(dsd_squelch_analog_default_parse(o, "off", err, sizeof err) == -1
           && strstr(err, "not off: leave the option out for no default"));
    assert(dsd_squelch_analog_default_parse(o, "loud", err, sizeof err) == -1
           && strstr(err, "expects auto[+N] or noise[+N] (N from 3 to 30)"));
    assert(dsd_squelch_analog_default_parse(o, "auto+2", err, sizeof err) == -1 && strstr(err, "3 to 30"));
    free(o);
}

/* The channel kinds: the analog monitor of a radio input takes it; a digital decoder, audio input, the M17 encoder and
   a session without the option run the session's own, exactly as it was. */
static void
test_channel_kinds(void) {
    dsd_opts* o = NULL;
    dsd_state* s = NULL;
    session_init(&o, &s, -110);
    assert(dsd_squelch_analog_default_wanted(o, s));
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));
    /* The level stays beneath, for a switch back; the configured fields are the session's own. */
    assert(level_is(o->rtl_squelch_level, dsd_squelch_level_from_sql(-110.0)));
    double level = 0.0;
    int mode = -1;
    int margin = 0;
    dsd_squelch_configured_fields(o, &level, &mode, &margin);
    assert(mode == DSD_SQUELCH_MODE_LEVEL && level_is(level, dsd_squelch_level_from_sql(-110.0)));
    assert(dsd_squelch_analog_default_sync(o, s) == 0 && runs_default(o));

    /* A switch to a digital decoder: the session's own comes back. */
    o->analog_only = 0;
    o->frame_dmr = 1;
    assert(!dsd_squelch_analog_default_wanted(o, s));
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_level(o, -110));
    assert(dsd_squelch_analog_default_sync(o, s) == 0);
    /* AM: the configured fields run NOISE (the receiver runs it as auto); see test_am_notice(). */
    o->analog_only = 1;
    o->analog_demod = DSD_ANALOG_DEMOD_AM;
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "squelch_analog_default_kinds") == 0);
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));
    o->analog_demod = DSD_ANALOG_DEMOD_FM;
    assert(dsd_squelch_analog_default_sync(o, s) == 0 && runs_default(o));
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    char log[1024];
    assert(dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0 && strstr(log, "runs auto+10dB."));

    /* Audio input suspends it; back on a radio input it is in force again. */
    o->audio_in_type = AUDIO_IN_WAV;
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_level(o, -110));
    o->audio_in_type = AUDIO_IN_RTL;
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));

    /* The M17 encoder shares the front end but not the analog family. */
    o->m17encoder = 1;
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_level(o, -110));
    o->m17encoder = 0;

    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));
    session_free(o, s);

    /* Never given (an off is refused, so it can never be given): nothing ever changes, and a refused off leaves it
       so. */
    for (int refused_off = 0; refused_off < 2; refused_off++) {
        session_init(&o, &s, -110);
        o->squelch_default_mode = DSD_SQUELCH_MODE_LEVEL;
        o->squelch_default_margin_db = 0;
        if (refused_off) {
            assert(dsd_squelch_analog_default_parse(o, "off", NULL, 0U) == -1);
        }
        assert(o->squelch_default_mode == DSD_SQUELCH_MODE_LEVEL && !dsd_squelch_analog_default_pending(o));
        assert(!dsd_squelch_analog_default_wanted(o, s));
        assert(dsd_squelch_analog_default_sync(o, s) == 0 && runs_level(o, -110));
        session_free(o, s);
    }
}

/* Every live edit of the configured squelch ends it for the session, an Off or a value equal to the default as much as
   another, and from wherever it lands: on the monitor or on a digital decoder. */
static void
test_explicit_edit_ends_it(void) {
    for (int on_monitor = 0; on_monitor < 2; on_monitor++) {
        const dsd_squelch_setting edits[] = {dsd_squelch_setting_of_level(0.0), dsd_squelch_setting_noise(10),
                                             dsd_squelch_setting_of_level(dsd_squelch_level_from_sql(-60.0))};
        for (size_t i = 0; i < sizeof edits / sizeof edits[0]; i++) {
            dsd_opts* o = NULL;
            dsd_state* s = NULL;
            session_init(&o, &s, 0);
            o->analog_only = on_monitor;
            assert(dsd_squelch_analog_default_sync(o, s) == on_monitor);
            assert(dsd_scan_mode_set_configured_squelch_setting(o, s, &edits[i]) == 1);
            assert(o->squelch_default_state == DSD_SQUELCH_DEFAULT_ENDED && !dsd_squelch_analog_default_pending(o));
            const dsd_squelch_setting now = dsd_squelch_setting_of_opts(o);
            assert(dsd_squelch_setting_equal(&now, &edits[i]));
            /* Never back: not on a switch to the monitor, not after a round trip through digital. */
            o->analog_only = 1;
            assert(dsd_squelch_analog_default_sync(o, s) == 0);
            o->analog_only = 0;
            assert(dsd_squelch_analog_default_sync(o, s) == 0);
            o->analog_only = 1;
            assert(dsd_squelch_analog_default_sync(o, s) == 0);
            const dsd_squelch_setting kept = dsd_squelch_setting_of_opts(o);
            assert(dsd_squelch_setting_equal(&kept, &edits[i])
                   && o->squelch_default_state == DSD_SQUELCH_DEFAULT_ENDED);
            session_free(o, s);
        }
    }
}

/* The startup sources: --squelch (dsd_squelch_analog_default_end(), which the CLI calls), a spec's sql field other than
   0 (a frontend's 0 is no squelch), and a config's squelch keys. */
static void
test_startup_sources(void) {
    static const struct {
        const char* field;
        int ends;
    } fields[] = {{"0", 0}, {"off", 1}, {"-60", 1}, {"noise+10", 1}, {"auto+14", 1}};

    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        dsd_opts* o = NULL;
        dsd_state* s = NULL;
        session_init(&o, &s, -110);
        assert(dsd_squelch_spec_field_apply(o, fields[i].field) == 0);
        assert((o->squelch_default_state == DSD_SQUELCH_DEFAULT_ENDED) == (fields[i].ends != 0));
        assert(dsd_squelch_analog_default_sync(o, s) == (fields[i].ends ? 0 : 1));
        if (!fields[i].ends) {
            /* The spec's 0 is the base the default runs over, and comes back off the monitor. */
            assert(runs_default(o));
            o->analog_only = 0;
            assert(dsd_squelch_analog_default_sync(o, s) == 1 && dsd_squelch_is_off(o->rtl_squelch_level));
        }
        session_free(o, s);
    }

    for (int sql_set = 0; sql_set < 2; sql_set++) {
        dsd_opts* o = NULL;
        dsd_state* s = NULL;
        session_init(&o, &s, -110);
        dsdneoUserConfig* cfg = (dsdneoUserConfig*)calloc(1, sizeof(*cfg));
        assert(cfg);
        cfg->has_input = 1;
        cfg->input_source = DSDCFG_INPUT_PULSE;
        cfg->rtl_sql = -60;
        cfg->rtl_sql_is_set = sql_set;
        dsd_apply_user_config_to_opts(cfg, o, s);
        o->audio_in_type = AUDIO_IN_RTL;
        assert((o->squelch_default_state == DSD_SQUELCH_DEFAULT_ENDED) == (sql_set != 0));
        assert(dsd_squelch_analog_default_sync(o, s) == !sql_set);
        free(cfg);
        session_free(o, s);
    }
}

/* -Y and trunk scan alike (scan_mode.c's row commit): an analog row without its own squelch runs the default in the
   same push as its other options; a digital row and a row with its own squelch run the session's own and their own;
   the next analog row runs it again; the leave goes by the decoder left on air. The scope's configured squelch is the
   session's own, even when the scan began on the monitor with the default in force. */
static void
test_scan_rows(void) {
    dsd_opts* o = NULL;
    dsd_state* s = NULL;
    session_init(&o, &s, 0);
    o->scanner_mode = 1; /* -Y: a row is no standalone monitor */
    const dsd_rtl_stream_metrics_hooks hooks = {.set_channel_squelch_setting = record_push};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    g_pushes = 0;
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));

    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_scan_mode_configured_view(s)->rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL);
    assert(dsd_squelch_is_off(dsd_scan_mode_configured_view(s)->rtl_squelch_level));
    assert(dsd_scan_mode_options(o, s, NULL) == 0);
    assert(runs_level(o, 0) && g_pushes == 1 && dsd_squelch_setting_is_off(&g_pushed));

    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(o, s, NULL) == 0);
    assert(runs_default(o) && g_pushes == 2 && g_pushed.mode == DSD_SQUELCH_MODE_NOISE && g_pushed.margin_db == 10);
    /* The same row again: nothing new for the receiver. */
    assert(dsd_scan_mode_options(o, s, NULL) == 0);
    assert(g_pushes == 2);

    /* A row's own squelch, for that row only. */
    dsd_scan_option_values own = {0};
    own.present = DSD_SCAN_OPT_SQUELCH;
    own.squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    own.squelch_db = -60;
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(o, s, &own) == 0);
    assert(runs_level(o, -60) && g_pushes == 3);
    assert(dsd_squelch_analog_default_sync(o, s) == 0 && runs_level(o, -60));
    assert(dsd_squelch_analog_default_pending(o));
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_AM) == 0);
    assert(dsd_scan_mode_options(o, s, NULL) == 0);
    assert(runs_default(o) && g_pushes == 4);

    /* A command suspends the scope: nothing moves until the resume, which runs the row's default again. */
    assert(dsd_scan_mode_suspend(o, s) == 1);
    assert(runs_level(o, 0));
    assert(dsd_squelch_analog_default_sync(o, s) == 0 && runs_level(o, 0));
    (void)dsd_scan_mode_resume(o, s);
    assert(runs_default(o) && g_pushes == 5);
    assert(dsd_squelch_is_off(dsd_scan_mode_configured_view(s)->rtl_squelch_level));

    /* The leave goes by the decoder left on air: the configured -fA monitor takes the default (already in). */
    dsd_scan_mode_leave(o, s);
    assert(runs_default(o));
    /* A scan configured digital leaves the session's own. */
    o->analog_only = 0;
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(o, s, NULL) == 0);
    assert(runs_default(o));
    const int pushes = g_pushes;
    dsd_scan_mode_leave(o, s);
    assert(runs_level(o, 0) && g_pushes == pushes + 1 && dsd_squelch_setting_is_off(&g_pushed));

    /* A save takes the session's own squelch, never the default. */
    o->analog_only = 1;
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));
    dsdneoUserConfig* cfg = (dsdneoUserConfig*)calloc(1, sizeof(*cfg));
    assert(cfg);
    dsd_snapshot_opts_to_user_config(o, s, cfg);
    assert(cfg->rtl_sql == 0 && !dsd_squelch_mode_is_dynamic(cfg->rtl_sql_mode));
    free(cfg);

    /* An explicit edit while a row inherits it ends it for every row after. */
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(o, s, NULL) == 0);
    assert(runs_default(o));
    const dsd_squelch_setting auto6 = dsd_squelch_setting_auto(6);
    assert(dsd_scan_mode_set_configured_squelch_setting(o, s, &auto6) == 1);
    assert(o->rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO && o->rtl_squelch_margin_db == 6);
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(o, s, NULL) == 0);
    assert(o->rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO && o->rtl_squelch_margin_db == 6);
    dsd_scan_mode_leave(o, s);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    session_free(o, s);
}

/* Saves the session's config to a private temp file and loads it back: the INI text into @p text, the reload into
   @p reloaded. */
static void
save_and_reload(const dsd_opts* o, const dsd_state* s, char* text, size_t text_size, dsdneoUserConfig* reloaded) {
    dsdneoUserConfig* cfg = (dsdneoUserConfig*)calloc(1, sizeof(*cfg));
    assert(cfg);
    dsd_snapshot_opts_to_user_config(o, s, cfg);
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_squelch_default_save");
    assert(fd >= 0);
    (void)dsd_close(fd);
    FILE* out = dsd_fopen_private(path, "wb");
    assert(out);
    dsd_user_config_render_ini(cfg, out);
    const int out_rc = fclose(out);
    assert(out_rc == 0);
    FILE* in = dsd_fopen_private(path, "rb");
    assert(in);
    const size_t n = fread(text, 1U, text_size - 1U, in);
    assert(n < text_size - 1U); /* the whole save: a cut one would hide the keys the callers look for */
    text[n] = '\0';
    const int in_rc = fclose(in);
    assert(in_rc == 0);
    assert(dsd_user_config_load(path, reloaded) == 0);
    const int remove_rc = remove(path);
    assert(remove_rc == 0);
    free(cfg);
}

/* The next run of a saved session, given --squelch-analog-default noise+10 again: it loads the save over a fresh
   session on the -fA monitor of a radio input, whose level is the startup one (dsd_init), and applies the RTL-SDR spec
   the load built, as the start does. */
static void
next_run(const dsdneoUserConfig* reloaded, dsd_opts** out_opts, dsd_state** out_state) {
    session_init(out_opts, out_state, DSD_SQUELCH_STARTUP_DB);
    dsd_apply_user_config_to_opts(reloaded, *out_opts, *out_state);
    (*out_opts)->audio_in_type = AUDIO_IN_RTL;
    (*out_opts)->analog_only = 1;
    assert(dsd_rtl_input_spec_apply(*out_opts) >= 0);
}

static int
saves_squelch_keys(const char* text) {
    return strstr(text, "rtl_sql") != NULL;
}

/* Whether the RTL-SDR spec @p dev (rtl:dev:freq:gain:ppm:bw:sql:vol) has @p want in its sql field. */
static int
rtl_spec_sql_is(const char* dev, const char* want) {
    const char* field = dev;
    for (int colons = 0; colons < 6; colons++) {
        field = strchr(field, ':');
        if (!field) {
            return 0;
        }
        field++;
    }
    const size_t len = strlen(want);
    return strncmp(field, want, len) == 0 && (field[len] == ':' || field[len] == '\0');
}

/* The inputs a save is taken on: a live RTL-SDR, an I/Q replay saving the [input] it loaded (source = "rtl", no
   squelch key), audio input, and a digital decoder of a live RTL-SDR. The live RTL-SDR ones run a synthetic frequency,
   so the save has an rtl_freq and the reload builds its RTL-SDR spec. */
enum { SAVE_ON_RTL, SAVE_ON_REPLAY, SAVE_ON_AUDIO, SAVE_ON_DIGITAL, SAVE_ON_COUNT };

static void
session_input(dsd_opts* o, dsd_state* s, int input) {
    const char* dev = input == SAVE_ON_REPLAY  ? "iqreplay:/captures/synthetic.iq.json"
                      : input == SAVE_ON_AUDIO ? "pulse"
                                               : "rtl";
    DSD_SNPRINTF(o->audio_in_dev, sizeof o->audio_in_dev, "%s", dev);
    if (input == SAVE_ON_RTL || input == SAVE_ON_DIGITAL) {
        o->rtlsdr_center_freq = 123456000U;
    }
    if (input == SAVE_ON_REPLAY) {
        dsdneoUserConfig* loaded = (dsdneoUserConfig*)calloc(1, sizeof(*loaded));
        assert(loaded);
        loaded->has_input = 1;
        loaded->input_source = DSDCFG_INPUT_RTL;
        assert(dsd_user_config_retain_input(s, loaded) == 0);
        free(loaded);
    } else if (input == SAVE_ON_AUDIO) {
        o->audio_in_type = AUDIO_IN_PULSE;
    } else if (input == SAVE_ON_DIGITAL) {
        o->analog_only = 0;
        o->frame_dmr = 1;
    }
}

/* A save while the analog default stands (no squelch set this session) writes no squelch key, as a config that never
   set one, so the next run keeps the default: on every input of session_input(), the audio input and the digital
   decoder included, where a switch back to the radio's monitor would hand the default again. */
static void
test_save_keeps_default(void) {
    char text[16384];
    dsdneoUserConfig* reloaded = (dsdneoUserConfig*)calloc(1, sizeof(*reloaded));
    assert(reloaded);
    for (int input = 0; input < SAVE_ON_COUNT; input++) {
        dsd_opts* o = NULL;
        dsd_state* s = NULL;
        session_init(&o, &s, 0);
        session_input(o, s, input);
        const int on_monitor = input == SAVE_ON_RTL || input == SAVE_ON_REPLAY;
        assert(dsd_squelch_analog_default_sync(o, s) == on_monitor);
        save_and_reload(o, s, text, sizeof text, reloaded);
        if (saves_squelch_keys(text) || reloaded->rtl_sql_is_set) {
            DSD_FPRINTF(stderr, "input %d: a save with the analog default standing wrote a squelch key:\n%s\n", input,
                        text);
            assert(0);
        }
        session_free(o, s);
        next_run(reloaded, &o, &s);
        assert(dsd_squelch_analog_default_pending(o));
        /* The spec the reload builds from rtl_freq carries the omitted rtl_sql as 0, which ends nothing. */
        const int builds_spec = input == SAVE_ON_RTL || input == SAVE_ON_DIGITAL;
        assert(!builds_spec || rtl_spec_sql_is(o->audio_in_dev, "0"));
        assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));
        session_free(o, s);
    }
    free(reloaded);
}

/* The level beneath a standing default across a save: a session started with no squelch field (the startup level)
   saves no squelch key; the next run, given the option again, starts at the startup level, applies what the reload
   builds as the start does (the RTL-SDR spec's sql field 0; on a SoapySDR rtl_sql's 0, which the load writes itself)
   and still runs the default. Beneath it the level comes back Off, which is no squelch as the startup level is (at or
   below -110 dB no analog call is allowed, dsd_squelch_analog_calls_allowed(), and such a power gate never closes),
   so leaving the analog monitor (here for a digital decoder) runs Off, and the default still stands. */
static void
test_save_base_comes_back_off(void) {
    char text[16384];
    dsdneoUserConfig* reloaded = (dsdneoUserConfig*)calloc(1, sizeof(*reloaded));
    assert(reloaded);
    static const char* const devs[] = {"rtl", "soapy"};
    for (size_t i = 0; i < sizeof devs / sizeof devs[0]; i++) {
        dsd_opts* o = NULL;
        dsd_state* s = NULL;
        session_init(&o, &s, DSD_SQUELCH_STARTUP_DB);
        DSD_SNPRINTF(o->audio_in_dev, sizeof o->audio_in_dev, "%s", devs[i]);
        o->rtlsdr_center_freq = 123456000U;
        assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));
        save_and_reload(o, s, text, sizeof text, reloaded);
        assert(!saves_squelch_keys(text) && !reloaded->rtl_sql_is_set);
        session_free(o, s);
        next_run(reloaded, &o, &s);
        assert(dsd_squelch_analog_default_pending(o));
        assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));
        o->analog_only = 0;
        o->frame_dmr = 1;
        assert(dsd_squelch_analog_default_sync(o, s) == 1);
        if (!runs_level(o, 0) || !dsd_squelch_analog_default_pending(o)) {
            DSD_FPRINTF(stderr,
                        "%s: after a save and reload the level beneath the default is %.3g (mode %d), not Off, or the "
                        "default no longer stands (%d):\n%s\n",
                        devs[i], o->rtl_squelch_level, o->rtl_squelch_mode, dsd_squelch_analog_default_pending(o),
                        text);
            assert(0);
        }
        session_free(o, s);
    }
    free(reloaded);
}

/* Once an explicit squelch ended the default (an Off included), the save writes the session's squelch, which ends the
   default on the next run: an Off as rtl_sql = 0, auto+12 with its mode and margin. */
static void
test_save_after_explicit_squelch(void) {
    char text[16384];
    dsdneoUserConfig* reloaded = (dsdneoUserConfig*)calloc(1, sizeof(*reloaded));
    assert(reloaded);
    const dsd_squelch_setting edits[] = {dsd_squelch_setting_of_level(0.0), dsd_squelch_setting_auto(12)};
    for (size_t i = 0; i < sizeof edits / sizeof edits[0]; i++) {
        dsd_opts* o = NULL;
        dsd_state* s = NULL;
        session_init(&o, &s, 0);
        session_input(o, s, SAVE_ON_RTL);
        assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));
        assert(dsd_scan_mode_set_configured_squelch_setting(o, s, &edits[i]) == 1);
        save_and_reload(o, s, text, sizeof text, reloaded);
        const int is_auto = dsd_squelch_setting_is_dynamic(&edits[i]);
        assert(strstr(text, "rtl_sql = 0\n") != NULL);
        assert(is_auto == (strstr(text, "rtl_sql_mode = \"auto\"\nrtl_sql_margin_db = 12\n") != NULL));
        assert(is_auto || strstr(text, "rtl_sql_mode") == NULL);
        assert(reloaded->rtl_sql_is_set && reloaded->rtl_sql == 0
               && reloaded->rtl_sql_mode == (is_auto ? DSD_SQUELCH_MODE_AUTO : DSD_SQUELCH_MODE_LEVEL)
               && (!is_auto || reloaded->rtl_sql_margin_db == 12));
        session_free(o, s);
        next_run(reloaded, &o, &s);
        assert(o->squelch_default_state == DSD_SQUELCH_DEFAULT_ENDED && !dsd_squelch_analog_default_pending(o));
        /* The RTL-SDR spec the reload builds carries the setting, which the start applied over the startup level:
           Off, or auto+12 with Off beneath it. */
        assert(rtl_spec_sql_is(o->audio_in_dev, is_auto ? "auto+12" : "0"));
        assert(dsd_squelch_analog_default_sync(o, s) == 0);
        assert(is_auto ? (o->rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO && o->rtl_squelch_margin_db == 12
                          && level_is(o->rtl_squelch_level, 0.0))
                       : runs_level(o, 0));
        session_free(o, s);
    }
    free(reloaded);
}

/* Without the option a save writes rtl_sql as it always did, on a live radio input and a replay's loaded [input]. */
static void
test_save_without_default(void) {
    char text[16384];
    dsdneoUserConfig* reloaded = (dsdneoUserConfig*)calloc(1, sizeof(*reloaded));
    assert(reloaded);
    for (int input = SAVE_ON_RTL; input <= SAVE_ON_REPLAY; input++) {
        dsd_opts* o = NULL;
        dsd_state* s = NULL;
        session_init(&o, &s, 0);
        o->squelch_default_mode = DSD_SQUELCH_MODE_LEVEL;
        o->squelch_default_margin_db = 0;
        session_input(o, s, input);
        assert(dsd_squelch_analog_default_sync(o, s) == 0);
        save_and_reload(o, s, text, sizeof text, reloaded);
        assert(strstr(text, "rtl_sql = 0\n") != NULL && strstr(text, "rtl_sql_mode") == NULL);
        assert(reloaded->rtl_sql_is_set);
        session_free(o, s);
    }
    free(reloaded);
}

/* The decoder the session runs decides, however it got there: a config whose [mode] makes a digital session analog
   (Advanced --config) brings the default in at the next sync; a decode change made while a -Y scan's scope is
   suspended (DECODE_MODE_SET under a row) reaches the rows that follow the configured decoder at the resume. */
static void
test_effective_decoder(void) {
    dsd_opts* o = NULL;
    dsd_state* s = NULL;
    session_init(&o, &s, 0);
    o->analog_only = 0;
    o->frame_dmr = 1;
    assert(dsd_squelch_analog_default_sync(o, s) == 0);
    dsdneoUserConfig* cfg = (dsdneoUserConfig*)calloc(1, sizeof(*cfg));
    assert(cfg);
    cfg->has_mode = 1;
    cfg->decode_mode = DSDCFG_MODE_ANALOG;
    dsd_apply_user_config_to_opts(cfg, o, s);
    assert(o->analog_only == 1);
    assert(dsd_squelch_analog_default_sync(o, s) == 1 && runs_default(o));
    free(cfg);
    session_free(o, s);

    session_init(&o, &s, 0);
    o->scanner_mode = 1;
    o->analog_only = 0;
    o->frame_dmr = 1;
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_INHERIT) == 0);
    assert(dsd_scan_mode_options(o, s, NULL) == 0);
    assert(runs_level(o, 0));
    assert(dsd_scan_mode_suspend(o, s) == 1);
    o->analog_only = 1; /* the configured decoder becomes the NFM monitor */
    (void)dsd_scan_mode_resume(o, s);
    assert(o->analog_only == 1 && runs_default(o));
    dsd_scan_mode_leave(o, s);
    assert(runs_default(o));
    session_free(o, s);
}

/* The AM monitor of a radio input runs NOISE as AUTO with the same N: the notice says so at each start of it, whatever
   started it (here the default's own start, a switch from FM, and a scan's leave). */
static int
count_notices(const char* log) {
    int n = 0;
    for (const char* p = log; (p = strstr(p, "this AM channel runs auto+10dB.")) != NULL; p++) {
        n++;
    }
    return n;
}

static void
test_am_notice(void) {
    dsd_opts* o = NULL;
    dsd_state* s = NULL;
    session_init(&o, &s, 0);
    o->analog_demod = DSD_ANALOG_DEMOD_AM;
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "squelch_analog_default_am") == 0);
    (void)dsd_squelch_analog_default_sync(o, s); /* 1: in, on AM */
    (void)dsd_squelch_analog_default_sync(o, s); /* still: no repeat */
    o->analog_demod = DSD_ANALOG_DEMOD_FM;
    (void)dsd_squelch_analog_default_sync(o, s); /* FM runs noise: nothing */
    o->analog_demod = DSD_ANALOG_DEMOD_AM;
    (void)dsd_squelch_analog_default_sync(o, s); /* 2: a switch to AM */
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_NFM) == 0);
    assert(dsd_scan_mode_options(o, s, NULL) == 0); /* a scan row: not the standalone monitor */
    dsd_scan_mode_leave(o, s);                      /* 3: back on the AM monitor */
    /* An explicit noise on a session without the option says it too (every origin). */
    dsd_opts* plain = (dsd_opts*)calloc(1, sizeof(*plain));
    assert(plain);
    *plain = *o;
    plain->squelch_default_mode = DSD_SQUELCH_MODE_LEVEL;
    plain->squelch_default_state = DSD_SQUELCH_DEFAULT_IDLE;
    plain->squelch_am_auto_noted = 0;
    const dsd_squelch_setting noise10 = dsd_squelch_setting_noise(10);
    dsd_squelch_setting_store(plain, &noise10);
    (void)dsd_squelch_analog_default_sync(plain, NULL); /* 4 */
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    char log[4096] = {0};
    assert(dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0);
    if (count_notices(log) != 4) {
        DSD_FPRINTF(stderr, "AM notices: got %d want 4:\n%s\n", count_notices(log), log);
        assert(0);
    }
    free(plain);
    session_free(o, s);
}

int
main(void) {
    test_parse();
    test_channel_kinds();
    test_explicit_edit_ends_it();
    test_startup_sources();
    test_scan_rows();
    test_effective_decoder();
    test_save_keeps_default();
    test_save_base_comes_back_off();
    test_save_after_explicit_squelch();
    test_save_without_default();
    test_am_notice();
    printf("RUNTIME_SQUELCH_ANALOG_DEFAULT: OK\n");
    return 0;
}
