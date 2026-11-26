// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Basic tests for INI-based user configuration.
 *
 * Exercises load/apply/snapshot behavior for a representative config
 * without touching CLI or environment precedence.
 */

#include <cmath>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/core/udp_comp_ports.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/input_spec.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/squelch.h>
#include <errno.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/types.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/runtime/call_alert.h"
#include "dsd-neo/runtime/config_schema.h"
#include "test_env.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#define DSD_TEST_RMDIR _rmdir
#else
#include <unistd.h>
#define DSD_TEST_RMDIR rmdir
#endif

/* Removes an empty temp directory with DSD_TEST_RMDIR, since remove() does not remove a directory on native Windows.
 * Returns 1 and says why when the directory stays, so a test that leaves a file in it fails. */
static int
remove_test_dir(const char* dir) {
    if (DSD_TEST_RMDIR(dir) != 0) {
        DSD_FPRINTF(stderr, "could not remove temp directory %s: %s\n", dir, strerror(errno));
        return 1;
    }
    return 0;
}

static int
write_temp_config(const char* contents, char* out_path, size_t out_sz) {
    char tmpl[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(tmpl, sizeof(tmpl), "dsdneo_config_user");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    size_t len = strlen(contents);
    ssize_t wr = dsd_write(fd, contents, len);
    if (wr < 0 || (size_t)wr != len) {
        DSD_FPRINTF(stderr, "write failed: %s\n", strerror(errno));
        (void)dsd_close(fd);
        (void)remove(tmpl);
        return 1;
    }
    (void)dsd_close(fd);
    DSD_SNPRINTF(out_path, out_sz, "%s", tmpl);
    out_path[out_sz - 1] = '\0';
    return 0;
}

static void
reset_opts_and_state(dsd_opts& opts, dsd_state& state) {
    static const dsd_opts k_zero_opts = {};
    static const dsd_state k_zero_state = {};
    opts = k_zero_opts;
    state = k_zero_state;
}

static char g_log_text[4096];

static void
capture_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    (void)ctx;
    size_t used = strlen(g_log_text);
    if (text && used < sizeof g_log_text - 1U) {
        DSD_SNPRINTF(g_log_text + used, sizeof g_log_text - used, "%s", text);
    }
}

static int
render_config_to_buffer(const dsdneoUserConfig* cfg, char* out, size_t out_sz) {
    if (!cfg || !out || out_sz == 0) {
        return 1;
    }
    FILE* tmp = tmpfile();
    if (!tmp) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    dsd_user_config_render_ini(cfg, tmp);
    if (fseek(tmp, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek failed\n");
        fclose(tmp);
        return 1;
    }
    size_t n = fread(out, 1, out_sz - 1, tmp);
    if (n == 0 && ferror(tmp)) {
        DSD_FPRINTF(stderr, "fread failed\n");
        fclose(tmp);
        return 1;
    }
    out[n] = '\0';
    fclose(tmp);
    return 0;
}

static int
expect_contains(const char* label, const char* haystack, const char* needle) {
    if (!haystack || !needle || !strstr(haystack, needle)) {
        DSD_FPRINTF(stderr, "FAIL: %s missing \"%s\" in:\n%s\n", label, needle ? needle : "(null)",
                    haystack ? haystack : "(null)");
        return 1;
    }
    return 0;
}

/*
 * Like expect_contains(), but never echoes the rendered buffer: this INI carries
 * RadioReference credentials and a failure message must not leak them.
 */
static int
expect_contains_quiet(const char* label, const char* haystack, const char* needle) {
    if (!haystack || !needle || !strstr(haystack, needle)) {
        DSD_FPRINTF(stderr, "FAIL: %s did not render the expected line\n", label ? label : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_absent_quiet(const char* label, const char* haystack, const char* needle) {
    if (!haystack || !needle || strstr(haystack, needle)) {
        DSD_FPRINTF(stderr, "FAIL: %s rendered a line that should be absent\n", label ? label : "(null)");
        return 1;
    }
    return 0;
}

/*
 * Discussion #717: [trunk_scan] activity_hold_ms is the hold of every conventional digital target (DMR, P25, NXDN, and
 * since issue #890 the simplex families) and an analog target's carrier tail (trunk_scan_conventional_stay_reason(),
 * --help, docs/trunk-scan.md). The schema text, which --dump-config-template prints, said DMR/NXDN only.
 */
static int
test_trunk_scan_activity_hold_description(void) {
    const dsdcfg_schema_entry_t* e = dsdcfg_schema_find("trunk_scan", "activity_hold_ms");
    static const char* const families[] = {"DMR", "P25", "NXDN", "YSF", "D-STAR", "dPMR", "M17", "TETRA DMO", "analog"};
    int named = e && e->description;
    for (size_t i = 0; named && i < sizeof families / sizeof families[0]; i++) {
        named = strstr(e->description, families[i]) != nullptr;
    }
    if (!named) {
        DSD_FPRINTF(stderr,
                    "FAIL: [trunk_scan] activity_hold_ms description does not name every family and analog: %s\n",
                    (e && e->description) ? e->description : "(missing)");
        return 1;
    }
    return 0;
}

static int
test_radioreference_schema_rows(void) {
    int rc = 0;
    int n = 0;
    const int count = dsdcfg_schema_count();
    for (int i = 0; i < count; i++) {
        const dsdcfg_schema_entry_t* e = dsdcfg_schema_get(i);
        if (e && e->section && strcmp(e->section, "radioreference") == 0) {
            n++;
        }
    }
    if (n != 2) {
        DSD_FPRINTF(stderr, "FAIL: expected 2 radioreference schema rows, found %d\n", n);
        rc |= 1;
    }
    if (dsdcfg_schema_find("radioreference", "username") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: radioreference.username is not in the schema\n");
        rc |= 1;
    }
    if (dsdcfg_schema_find("radioreference", "app_key") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: radioreference.app_key is not in the schema\n");
        rc |= 1;
    }
    if (dsdcfg_schema_find("radioreference", "password") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: radioreference.password must never be a persisted setting\n");
        rc |= 1;
    }
    return rc;
}

static int
test_radioreference_config_roundtrip(void) {
    static const char* ini = "[radioreference]\n"
                             "username = \"alice\"\n"
                             "app_key = \"K-1\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    int rc = 0;
    if (!cfg.has_radioreference) {
        DSD_FPRINTF(stderr, "FAIL: [radioreference] did not set has_radioreference\n");
        rc |= 1;
    }
    if (strcmp(cfg.rr_username, "alice") != 0 || strcmp(cfg.rr_app_key, "K-1") != 0) {
        DSD_FPRINTF(stderr, "FAIL: [radioreference] keys did not load into dsdneoUserConfig\n");
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (strcmp(opts.rr_username, "alice") != 0 || strcmp(opts.rr_app_key, "K-1") != 0) {
        DSD_FPRINTF(stderr, "FAIL: [radioreference] keys did not reach dsd_opts\n");
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.has_radioreference || strcmp(snap.rr_username, "alice") != 0 || strcmp(snap.rr_app_key, "K-1") != 0) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped the radioreference credentials\n");
        rc |= 1;
    }

    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("radioreference section header", rendered, "[radioreference]\n");
    rc |= expect_contains_quiet("radioreference username", rendered, "username = \"alice\"\n");
    rc |= expect_contains_quiet("radioreference app key", rendered, "app_key = \"K-1\"\n");

    /* A session with no RadioReference credentials must not emit the section at all. */
    reset_opts_and_state(opts, state);
    dsdneoUserConfig empty_snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &empty_snap);
    if (empty_snap.has_radioreference) {
        DSD_FPRINTF(stderr, "FAIL: snapshot set has_radioreference with no credentials\n");
        rc |= 1;
    }
    if (render_config_to_buffer(&empty_snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_absent_quiet("empty radioreference section", rendered, "[radioreference]");

    return rc;
}

/* Issue #708: [recording] unknown_talkgroups_only loads, reaches opts->record_unknown_only, and saves back; a
   [recording] without it records every talkgroup again. */
static int
test_recording_unknown_talkgroups_only_roundtrip(void) {
    static const char* ini = "[recording]\n"
                             "unknown_talkgroups_only = true\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    int rc = 0;
    if (!cfg.has_recording || cfg.unknown_talkgroups_only != 1) {
        DSD_FPRINTF(stderr, "FAIL: unknown_talkgroups_only did not load (%d)\n", cfg.unknown_talkgroups_only);
        rc |= 1;
    }
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.record_unknown_only != 1) {
        DSD_FPRINTF(stderr, "FAIL: unknown_talkgroups_only did not reach dsd_opts\n");
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.unknown_talkgroups_only != 1) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped unknown_talkgroups_only\n");
        rc |= 1;
    }
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("unknown_talkgroups_only saved", rendered, "unknown_talkgroups_only = true\n");
    if (write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig reloaded;
    const int reload_rc = dsd_user_config_load(path, &reloaded);
    (void)remove(path);
    if (reload_rc != 0 || reloaded.unknown_talkgroups_only != 1) {
        DSD_FPRINTF(stderr, "FAIL: the saved config did not load unknown_talkgroups_only back\n");
        rc |= 1;
    }

    static const char* without = "[recording]\n"
                                 "per_call_wav = true\n";
    if (write_temp_config(without, path, sizeof path) != 0) {
        return 1;
    }
    const int without_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (without_rc != 0 || cfg.unknown_talkgroups_only != 0) {
        DSD_FPRINTF(stderr, "FAIL: a [recording] without the key did not default it off\n");
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.record_unknown_only != 0) {
        DSD_FPRINTF(stderr, "FAIL: a [recording] without the key left the rule on\n");
        rc |= 1;
    }
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("unknown_talkgroups_only saved off", rendered, "unknown_talkgroups_only = false\n");
    return rc;
}

/* [input] rtl_bw_khz = 72 (TETRA's DSP bandwidth) loads, reaches the RTL spec the config builds, runs once that spec is
   read (as the input opens), and saves back; a value that is no selectable bandwidth runs, and saves, as the 48 kHz
   default. */
static int
test_rtl_bw_khz_72_round_trip(void) {
    static const struct {
        int configured_khz;
        int want_khz;
    } cases[] = {{72, 72}, {24, 24}, {60, 48}};

    int rc = 0;
    for (const auto& c : cases) {
        char ini[128];
        DSD_SNPRINTF(ini, sizeof ini, "[input]\nsource = \"rtl\"\nrtl_freq = \"420.1M\"\nrtl_bw_khz = %d\n",
                     c.configured_khz);
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(ini, path, sizeof path) != 0) {
            return 1;
        }
        static dsdneoUserConfig cfg;
        if (dsd_user_config_load(path, &cfg) != 0) {
            DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
            (void)remove(path);
            return 1;
        }
        (void)remove(path);
        if (cfg.rtl_bw_khz != c.configured_khz) {
            DSD_FPRINTF(stderr, "FAIL: rtl_bw_khz = %d loaded as %d\n", c.configured_khz, cfg.rtl_bw_khz);
            rc |= 1;
        }

        static dsd_opts opts;
        static dsd_state state;
        reset_opts_and_state(opts, state);
        opts.rtl_dsp_bw_khz = 48;
        dsd_apply_user_config_to_opts(&cfg, &opts, &state);
        char want_spec[64];
        DSD_SNPRINTF(want_spec, sizeof want_spec, ":420.1M:0:0:%d:", c.configured_khz);
        if (strstr(opts.audio_in_dev, want_spec) == NULL || dsd_rtl_input_spec_apply(&opts) != 1
            || opts.rtl_dsp_bw_khz != c.want_khz) {
            DSD_FPRINTF(stderr, "FAIL: rtl_bw_khz = %d applied as %d (spec %s)\n", c.configured_khz,
                        opts.rtl_dsp_bw_khz, opts.audio_in_dev);
            rc |= 1;
        }

        static dsdneoUserConfig snap;
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        char rendered[8192];
        if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
            return 1;
        }
        char want_line[32];
        DSD_SNPRINTF(want_line, sizeof want_line, "rtl_bw_khz = %d\n", c.want_khz);
        rc |= expect_contains("rtl_bw_khz saved", rendered, want_line);
    }
    return rc;
}

static int
test_radioreference_save_atomic_roundtrip(void) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_radioreference = 1;
    DSD_SNPRINTF(cfg.rr_username, sizeof cfg.rr_username, "%s", "alice");
    DSD_SNPRINTF(cfg.rr_app_key, sizeof cfg.rr_app_key, "%s", "K-1");

    char save_path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(save_path, sizeof save_path, "dsdneo_config_rr_save");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for radioreference save path\n");
        return 1;
    }
    (void)dsd_close(fd);

    int rc = 0;
    if (dsd_user_config_save_atomic(save_path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "save_atomic failed for the radioreference config\n");
        rc |= 1;
    } else {
        dsdneoUserConfig loaded;
        if (dsd_user_config_load(save_path, &loaded) != 0) {
            DSD_FPRINTF(stderr, "reload of the saved radioreference config failed\n");
            rc |= 1;
        } else if (!loaded.has_radioreference || strcmp(loaded.rr_username, "alice") != 0
                   || strcmp(loaded.rr_app_key, "K-1") != 0) {
            DSD_FPRINTF(stderr, "FAIL: saved radioreference credentials did not survive a reload\n");
            rc |= 1;
        }
    }

    (void)remove(save_path);
    return rc;
}

static int
test_input_warn_db_load_snapshot_render(void) {
    /* Canonical load: the INI value parses and flags as explicitly set. */
    static const char* ini = "[input]\n"
                             "source = \"rtl\"\n"
                             "input_warn_db = -60.5\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    int rc = 0;
    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_RTL) {
        DSD_FPRINTF(stderr, "input section not parsed as RTL\n");
        rc |= 1;
    }
    if (!cfg.input_warn_db_is_set || cfg.input_warn_db != -60.5) {
        DSD_FPRINTF(stderr, "FAIL: input_warn_db not parsed as -60.5 (got %f, is_set=%d)\n", cfg.input_warn_db,
                    cfg.input_warn_db_is_set);
        rc |= 1;
    }
    (void)remove(path);

    /* Trailing junk is rejected on load: the key stays unset and the default applies. */
    static const char* junk_ini = "[input]\n"
                                  "source = \"rtl\"\n"
                                  "input_warn_db = -20junk\n";
    if (write_temp_config(junk_ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig junk_cfg;
    if (dsd_user_config_load(path, &junk_cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    if (junk_cfg.input_warn_db_is_set || junk_cfg.input_warn_db != -40.0) {
        DSD_FPRINTF(stderr, "FAIL: invalid input_warn_db must fall back to the -40.0 default (got %f, is_set=%d)\n",
                    junk_cfg.input_warn_db, junk_cfg.input_warn_db_is_set);
        rc |= 1;
    }
    (void)remove(path);

    /* Hex-float spellings are rejected on load: the key stays unset and the default applies. */
    static const char* hex_ini = "[input]\n"
                                 "source = \"rtl\"\n"
                                 "input_warn_db = 0x10\n";
    if (write_temp_config(hex_ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig hex_cfg;
    if (dsd_user_config_load(path, &hex_cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    if (hex_cfg.input_warn_db_is_set || hex_cfg.input_warn_db != -40.0) {
        DSD_FPRINTF(stderr, "FAIL: hex input_warn_db must fall back to the -40.0 default (got %f, is_set=%d)\n",
                    hex_cfg.input_warn_db, hex_cfg.input_warn_db_is_set);
        rc |= 1;
    }
    (void)remove(path);

    /* Exit-autosave direction: the live opts threshold snapshots back into the config
       so menu-made changes persist on exit. */
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.input_warn_db = -55.5;
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.input_warn_db_is_set || snap.input_warn_db != -55.5) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped the live input_warn_db (got %f, is_set=%d)\n", snap.input_warn_db,
                    snap.input_warn_db_is_set);
        rc |= 1;
    }

    /* The rendered INI always carries the live advisory threshold. */
    dsdneoUserConfig render_cfg;
    DSD_MEMSET(&render_cfg, 0, sizeof render_cfg);
    render_cfg.has_input = 1;
    render_cfg.input_source = DSDCFG_INPUT_RTL;
    render_cfg.input_warn_db = -57.5;
    render_cfg.input_warn_db_is_set = 1;
    char rendered[8192];
    if (render_config_to_buffer(&render_cfg, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("render input warn db", rendered, "input_warn_db = -57.5\n");

    return rc;
}

static int
test_apply_file_input_rescales_symbol_timing(void) {
    dsdneoUserConfig cfg = {};
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_FILE;
    cfg.file_sample_rate = 44100;
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", "/tmp/input.wav");
    cfg.file_path[sizeof cfg.file_path - 1] = '\0';

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.wav_decimator = 48000;
    opts.wav_sample_rate = 48000;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.jitter = 3;

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    int rc = 0;
    if (strcmp(opts.audio_in_dev, "/tmp/input.wav") != 0) {
        DSD_FPRINTF(stderr, "file input path not applied correctly: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }
    if (opts.wav_sample_rate != 44100) {
        DSD_FPRINTF(stderr, "file input sample rate not applied correctly: %d\n", opts.wav_sample_rate);
        rc |= 1;
    }
    if (opts.staged_file_sample_rate != 44100) {
        DSD_FPRINTF(stderr, "staged file sample rate not applied correctly: %d\n", opts.staged_file_sample_rate);
        rc |= 1;
    }
    if (dsd_opts_effective_input_rate(&opts) != 44100) {
        DSD_FPRINTF(stderr, "effective input rate mismatch after file config: %d\n",
                    dsd_opts_effective_input_rate(&opts));
        rc |= 1;
    }
    if (state.samplesPerSymbol != 9 || state.symbolCenter != 4 || state.jitter != -1) {
        DSD_FPRINTF(stderr, "file input timing rescale mismatch: sps=%d center=%d jitter=%d\n", state.samplesPerSymbol,
                    state.symbolCenter, state.jitter);
        rc |= 1;
    }

    return rc;
}

/* A [mode] without a decode key runs no preset, so a file input at another rate rescales the session's own symbol
   timing (here 8 samples a symbol at 48 kHz, P25 Phase 2's) rather than a preset's. */
static int
test_apply_file_input_without_decode_keeps_session_timing(void) {
    dsdneoUserConfig cfg = {};
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_UNSET;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_FILE;
    cfg.file_sample_rate = 44100;
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", "/tmp/input.wav");
    cfg.file_path[sizeof cfg.file_path - 1] = '\0';

    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    opts.wav_decimator = 48000;
    opts.wav_sample_rate = 48000;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    int rc = 0;
    if (dsd_opts_effective_input_rate(&opts) != 44100 || state.samplesPerSymbol != 7) {
        DSD_FPRINTF(stderr, "FAIL: a [mode] without a decode key retimed the session: rate=%d sps=%d center=%d\n",
                    dsd_opts_effective_input_rate(&opts), state.samplesPerSymbol, state.symbolCenter);
        rc |= 1;
    }
    return rc;
}

static int
test_decode_mode_and_load_guards(void) {
    static const char* ini = "[mode]\n"
                             "decode = \"edacs_pv\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for canonical decode config %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_mode || cfg.decode_mode != DSDCFG_MODE_EDACS_PV) {
        DSD_FPRINTF(stderr, "decode mode edacs_pv not parsed as EDACS/PV, mode=%d\n", (int)cfg.decode_mode);
        rc |= 1;
    }

    dsdneoUserConfig bad_cfg;
    if (dsd_user_config_load(NULL, &bad_cfg) == 0) {
        DSD_FPRINTF(stderr, "load NULL path should fail\n");
        rc |= 1;
    }
    if (dsd_user_config_load(path, NULL) == 0) {
        DSD_FPRINTF(stderr, "load NULL config should fail\n");
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_persisted_v1_load_boundary(void) {
    struct persisted_config_case {
        const char* label;
        const char* contents;
        int should_load;
    } cases[] = {
        {"persisted version 1", "version = 1\n\n[mode]\ndecode = \"dmr\"\n", 1},
        {"unsupported version", "version = 2\n\n[mode]\ndecode = \"dmr\"\n", 0},
        {"non-integer version", "version = old\n\n[mode]\ndecode = \"dmr\"\n", 0},
    };

    int result = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(cases[i].contents, path, sizeof path) != 0) {
            return 1;
        }

        dsdneoUserConfig cfg;
        int load_rc = dsd_user_config_load(path, &cfg);
        if (cases[i].should_load) {
            if (load_rc != 0 || !cfg.has_mode || cfg.decode_mode != DSDCFG_MODE_DMR) {
                DSD_FPRINTF(stderr, "%s should load through the persisted-config boundary (rc=%d)\n", cases[i].label,
                            load_rc);
                result = 1;
            }
        } else if (load_rc == 0) {
            DSD_FPRINTF(stderr, "%s should be rejected\n", cases[i].label);
            result = 1;
        }
        (void)remove(path);
    }
    return result;
}

static int
test_integer_overflow_is_rejected_consistently(void) {
    static const char* ini = "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_device = 999999999999999999999999\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof diags);
    int rc = 0;
    if (dsd_user_config_validate(path, &diags) == 0 || diags.error_count == 0) {
        DSD_FPRINTF(stderr, "out-of-range integer should fail validation\n");
        rc = 1;
    }
    dsdcfg_diags_free(&diags);

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0 || cfg.rtl_device != 0) {
        DSD_FPRINTF(stderr, "out-of-range integer should leave the base default, got %d\n", cfg.rtl_device);
        rc = 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_unknown_section_warnings_do_not_mutate_loaded_config(void) {
    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[unexpected]\n"
                             "source = \"rtl\"\n"
                             "rtl_device = 9\n"
                             "rtl_freq = \"851.0125M\"\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"nxdn48\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));
    int validate_rc = dsd_user_config_validate(path, &diags);

    int rc = 0;
    if (validate_rc != 0 || diags.error_count != 0 || diags.warning_count == 0) {
        DSD_FPRINTF(stderr, "expected unknown section to validate with warning only, rc=%d errors=%d warnings=%d\n",
                    validate_rc, diags.error_count, diags.warning_count);
        rc |= 1;
    }
    int found_unknown_section = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].level == DSDCFG_DIAG_WARNING && strcmp(diags.items[i].section, "unexpected") == 0
            && strstr(diags.items[i].message, "Unknown section")) {
            found_unknown_section = 1;
            break;
        }
    }
    if (!found_unknown_section) {
        DSD_FPRINTF(stderr, "missing unknown-section warning diagnostic\n");
        rc |= 1;
    }
    dsdcfg_diags_free(&diags);

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for warning-only config %s\n", path);
        (void)remove(path);
        return 1;
    }
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_PULSE) {
        DSD_FPRINTF(stderr, "unknown section mutated input source=%d has_input=%d\n", (int)cfg.input_source,
                    cfg.has_input);
        rc |= 1;
    }
    if (cfg.rtl_device == 9 || cfg.rtl_freq[0] != '\0') {
        DSD_FPRINTF(stderr, "unknown section leaked RTL fields device=%d freq=%s\n", cfg.rtl_device, cfg.rtl_freq);
        rc |= 1;
    }
    if (!cfg.has_mode || cfg.decode_mode != DSDCFG_MODE_NXDN48) {
        DSD_FPRINTF(stderr, "known mode section after unknown section did not load, mode=%d has=%d\n",
                    (int)cfg.decode_mode, cfg.has_mode);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.audio_in_type == AUDIO_IN_RTL || strncmp(opts.audio_in_dev, "rtl:", 4) == 0) {
        DSD_FPRINTF(stderr, "unknown section applied RTL input to live opts: type=%d dev=%s\n", opts.audio_in_type,
                    opts.audio_in_dev);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_render_input_variants_and_save_atomic(void) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.has_output = 1;
    cfg.output_backend = DSDCFG_OUTPUT_PULSE;
    DSD_SNPRINTF(cfg.pulse_output, sizeof cfg.pulse_output, "%s", "speaker");
    cfg.frontend_kind = DSD_FRONTEND_TERMINAL;
    cfg.frontend_kind_is_set = 1;
    cfg.terminal_mouse_wheel = 1;
    cfg.terminal_mouse_wheel_is_set = 1;
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_M17;
    cfg.has_demod = 1;
    cfg.demod_path = DSDCFG_DEMOD_AUTO;
    cfg.has_logging = 1;
    DSD_SNPRINTF(cfg.event_log, sizeof cfg.event_log, "%s", "/tmp/events.log");
    DSD_SNPRINTF(cfg.frame_log, sizeof cfg.frame_log, "%s", "/tmp/frames.log");
    DSD_SNPRINTF(cfg.p25_sm_log, sizeof cfg.p25_sm_log, "%s", "/tmp/p25-sm.log");
    cfg.has_recording = 1;
    cfg.per_call_wav = 0;
    DSD_SNPRINTF(cfg.per_call_wav_dir, sizeof cfg.per_call_wav_dir, "%s", "/tmp/wav");
    DSD_SNPRINTF(cfg.static_wav_path, sizeof cfg.static_wav_path, "%s", "/tmp/static.wav");
    DSD_SNPRINTF(cfg.raw_wav_path, sizeof cfg.raw_wav_path, "%s", "/tmp/raw.wav");
    cfg.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    cfg.rdio_system_id = 12;
    DSD_SNPRINTF(cfg.rdio_api_url, sizeof cfg.rdio_api_url, "%s", "http://rdio.local");
    DSD_SNPRINTF(cfg.rdio_api_key, sizeof cfg.rdio_api_key, "%s", "secret");
    cfg.rdio_upload_timeout_ms = 6000;
    cfg.rdio_upload_retries = 2;
    cfg.rdio_api_delete_after_upload = 1;
    cfg.has_dsp = 1;
    cfg.iq_balance = 1;
    cfg.iq_dc_block = 1;

    char rendered[8192];
    int rc = 0;

    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 3;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "851.375M");
    cfg.rtl_gain = 29;
    cfg.rtl_ppm = 0;
    cfg.rtl_ppm_is_set = 1;
    cfg.rtl_bw_khz = 16;
    cfg.rtl_sql = -45;
    cfg.rtl_volume = 4;
    cfg.rtl_auto_ppm = 1;
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "version =") != NULL) {
        DSD_FPRINTF(stderr, "rendered config must not emit the persisted version marker:\n%s\n", rendered);
        rc |= 1;
    }
    rc |= expect_contains("render rtl source", rendered, "source = \"rtl\"\n");
    rc |= expect_contains("render rtl device", rendered, "rtl_device = 3\n");
    rc |= expect_contains("render rtl explicit zero ppm", rendered, "rtl_ppm = 0\n");
    rc |= expect_contains("render auto demod", rendered, "demod = \"auto\"\n");
    rc |= expect_contains("render pulse sink", rendered, "pulse_sink = \"speaker\"\n");
    rc |= expect_contains("render mouse wheel", rendered, "terminal_mouse_wheel = true\n");
    rc |= expect_contains("render event log", rendered, "event_log = \"/tmp/events.log\"\n");
    rc |= expect_contains("render static wav", rendered, "static_wav = \"/tmp/static.wav\"\n");
    rc |= expect_contains("render raw wav", rendered, "raw_wav = \"/tmp/raw.wav\"\n");
    rc |= expect_contains("render rdio api key", rendered, "rdio_api_key = \"secret\"\n");
    rc |= expect_contains("render dsp balance", rendered, "iq_balance = true\n");

    cfg.input_source = DSDCFG_INPUT_RTLTCP;
    DSD_SNPRINTF(cfg.rtltcp_host, sizeof cfg.rtltcp_host, "%s", "127.0.0.1");
    cfg.rtltcp_port = 1234;
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("render rtltcp source", rendered, "source = \"rtltcp\"\n");
    rc |= expect_contains("render rtltcp host", rendered, "rtltcp_host = \"127.0.0.1\"\n");
    rc |= expect_contains("render rtltcp port", rendered, "rtltcp_port = 1234\n");

    cfg.input_source = DSDCFG_INPUT_FILE;
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", "/tmp/input.wav");
    cfg.file_sample_rate = 96000;
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("render file source", rendered, "source = \"file\"\n");
    rc |= expect_contains("render file path", rendered, "file_path = \"/tmp/input.wav\"\n");
    rc |= expect_contains("render file sample rate", rendered, "file_sample_rate = 96000\n");

    cfg.input_source = DSDCFG_INPUT_TCP;
    DSD_SNPRINTF(cfg.tcp_host, sizeof cfg.tcp_host, "%s", "localhost");
    cfg.tcp_port = 7355;
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("render tcp source", rendered, "source = \"tcp\"\n");
    rc |= expect_contains("render tcp host", rendered, "tcp_host = \"localhost\"\n");
    rc |= expect_contains("render tcp port", rendered, "tcp_port = 7355\n");

    char base_path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(base_path, sizeof base_path, "dsdneo_config_save");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for save path\n");
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(base_path);

    char save_dir[DSD_TEST_PATH_MAX];
    char save_subdir[DSD_TEST_PATH_MAX];
    char save_path[DSD_TEST_PATH_MAX];
    DSD_SNPRINTF(save_dir, sizeof save_dir, "%s.d", base_path);
    DSD_SNPRINTF(save_subdir, sizeof save_subdir, "%s/sub", save_dir);
    DSD_SNPRINTF(save_path, sizeof save_path, "%s/config.ini", save_subdir);

    if (dsd_user_config_save_atomic(NULL, &cfg) == 0 || dsd_user_config_save_atomic("", &cfg) == 0
        || dsd_user_config_save_atomic(save_path, NULL) == 0) {
        DSD_FPRINTF(stderr, "save_atomic guard should reject NULL/empty inputs\n");
        rc |= 1;
    }
    if (dsd_user_config_save_atomic(save_path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "save_atomic failed for %s: %s\n", save_path, strerror(errno));
        rc |= 1;
    } else {
        dsdneoUserConfig loaded;
        if (dsd_user_config_load(save_path, &loaded) != 0) {
            DSD_FPRINTF(stderr, "load saved config failed for %s\n", save_path);
            rc |= 1;
        } else {
            rc |= (loaded.input_source == DSDCFG_INPUT_TCP) ? 0 : 1;
            if (loaded.input_source != DSDCFG_INPUT_TCP || strcmp(loaded.tcp_host, "localhost") != 0
                || loaded.tcp_port != 7355 || loaded.decode_mode != DSDCFG_MODE_M17
                || loaded.demod_path != DSDCFG_DEMOD_AUTO || !loaded.has_dsp || !loaded.iq_balance
                || !loaded.iq_dc_block) {
                DSD_FPRINTF(stderr,
                            "saved config reload mismatch source=%d host=%s port=%d mode=%d demod=%d dsp=%d/%d\n",
                            (int)loaded.input_source, loaded.tcp_host, loaded.tcp_port, (int)loaded.decode_mode,
                            (int)loaded.demod_path, loaded.iq_balance, loaded.iq_dc_block);
                rc |= 1;
            }
        }
    }

    (void)remove(save_path);
    rc |= remove_test_dir(save_subdir);
    rc |= remove_test_dir(save_dir);
    return rc;
}

static int
test_load_and_apply_basic(void) {
    /*
     * Load one representative user config that touches every public section.
     * The first half verifies parsed config fields; the second half applies the
     * snapshot to opts/state and checks the runtime-facing values.
     */
    static const char* ini = "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_device = 1\n"
                             "rtl_freq = \"851.375M\"\n"
                             "rtl_gain = 30\n"
                             "rtl_ppm = 5\n"
                             "rtl_bw_khz = 16\n"
                             "rtl_sql = -50\n"
                             "rtl_volume = 2\n"
                             "\n"
                             "[output]\n"
                             "backend = \"null\"\n"
                             "frontend = \"terminal\"\n"
                             "terminal_mouse_wheel = true\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"dmr\"\n"
                             "\n"
                             "[trunking]\n"
                             "enabled = true\n"
                             "chan_csv = \"/tmp/chan.csv\"\n"
                             "group_csv = \"/tmp/group.csv\"\n"
                             "allow_list = true\n"
                             "\n"
                             "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"/tmp/targets.csv\"\n"
                             "idle_dwell_ms = 5000\n"
                             "activity_hold_ms = 2000\n"
                             "\n"
                             "[logging]\n"
                             "event_log = \"/tmp/events.log\"\n"
                             "frame_log = \"/tmp/frames.log\"\n"
                             "p25_sm_log = \"/tmp/p25-sm.log\"\n"
                             "\n"
                             "[alerts]\n"
                             "enabled = true\n"
                             "voice_start = true\n"
                             "voice_end = false\n"
                             "data = true\n"
                             "\n"
                             "[recording]\n"
                             "per_call_wav = true\n"
                             "per_call_wav_dir = \"/tmp/wav\"\n"
                             "rdio_mode = \"both\"\n"
                             "rdio_system_id = 77\n"
                             "rdio_api_url = \"http://127.0.0.1:3000\"\n"
                             "rdio_api_key = \"apikey\"\n"
                             "rdio_upload_timeout_ms = 2500\n"
                             "rdio_upload_retries = 3\n"
                             "rdio_api_delete_after_upload = true\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_RTL) {
        DSD_FPRINTF(stderr, "input section not parsed as RTL\n");
        rc |= 1;
    }
    if (!cfg.has_output || cfg.output_backend != DSDCFG_OUTPUT_NULL || cfg.frontend_kind != DSD_FRONTEND_TERMINAL
        || !cfg.frontend_kind_is_set) {
        DSD_FPRINTF(stderr, "output section not parsed correctly\n");
        rc |= 1;
    }
    if (!cfg.has_mode || cfg.decode_mode != DSDCFG_MODE_DMR) {
        DSD_FPRINTF(stderr, "mode section not parsed as DMR\n");
        rc |= 1;
    }
    if (!cfg.has_trunking || !cfg.trunk_enabled || !cfg.trunk_use_allow_list) {
        DSD_FPRINTF(stderr, "trunking section not parsed correctly\n");
        rc |= 1;
    }
    if (!cfg.has_trunk_scan || !cfg.trunk_scan_enabled || strcmp(cfg.trunk_scan_lists[0].path, "/tmp/targets.csv") != 0
        || cfg.trunk_scan_idle_dwell_ms != 5000 || cfg.trunk_scan_activity_hold_ms != 2000) {
        DSD_FPRINTF(stderr, "trunk_scan section not parsed correctly\n");
        rc |= 1;
    }
    if (!cfg.has_alerts || !cfg.call_alert_enabled
        || cfg.call_alert_events != (DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_DATA)) {
        DSD_FPRINTF(stderr, "alerts section not parsed correctly enabled=%d events=%d\n", cfg.call_alert_enabled,
                    cfg.call_alert_events);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    // Input, output, and mode fields are applied before trunking and logging.
    if (strcmp(opts.audio_in_dev, "rtl:1:851.375M:30:5:16:-50:2") != 0) {
        DSD_FPRINTF(stderr, "audio_in_dev mismatch: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }
    if (strcmp(opts.audio_out_dev, "null") != 0) {
        DSD_FPRINTF(stderr, "audio_out_dev mismatch: \"%s\"\n", opts.audio_out_dev);
        rc |= 1;
    }
    if (opts.frontend_kind != DSD_FRONTEND_TERMINAL) {
        DSD_FPRINTF(stderr, "terminal frontend not enabled\n");
        rc |= 1;
    }
    if (!cfg.terminal_mouse_wheel_is_set || !cfg.terminal_mouse_wheel
        || opts.frontend_terminal_display.mouse_wheel != 1) {
        DSD_FPRINTF(stderr, "terminal_mouse_wheel = true not loaded or applied\n");
        rc |= 1;
    }
    if (!(opts.frame_dmr == 1 && opts.frame_p25p1 == 0 && opts.frame_p25p2 == 0 && opts.frame_ysf == 0)) {
        DSD_FPRINTF(stderr, "DMR mode flags not applied as expected\n");
        rc |= 1;
    }
    if (opts.trunk_enable != 1) {
        DSD_FPRINTF(stderr, "trunking not enabled in opts\n");
        rc |= 1;
    }
    if (opts.call_alert != 1 || opts.call_alert_events != (DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_DATA)
        || dsd_call_alert_event_enabled(opts.call_alert, opts.call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_END)) {
        DSD_FPRINTF(stderr, "call alert config not applied enabled=%d events=%u\n", opts.call_alert,
                    (unsigned)opts.call_alert_events);
        rc |= 1;
    }
    if (strcmp(opts.chan_in_file, "/tmp/chan.csv") != 0 || strcmp(opts.group_in_file, "/tmp/group.csv") != 0) {
        DSD_FPRINTF(stderr, "trunk CSV paths not applied correctly\n");
        rc |= 1;
    }
    if (opts.trunk_use_allow_list != 1) {
        DSD_FPRINTF(stderr, "trunk_use_allow_list not set\n");
        rc |= 1;
    }
    if (opts.trunk_scan_enabled != 1 || strcmp(opts.trunk_scan_lists[0].path, "/tmp/targets.csv") != 0
        || opts.trunk_scan_idle_dwell_ms != 5000 || opts.trunk_scan_activity_hold_ms != 2000) {
        DSD_FPRINTF(stderr, "trunk scan config not applied correctly enabled=%d targets=%s dwell=%d hold=%d\n",
                    opts.trunk_scan_enabled, opts.trunk_scan_lists[0].path, opts.trunk_scan_idle_dwell_ms,
                    opts.trunk_scan_activity_hold_ms);
        rc |= 1;
    }
    if (strcmp(opts.event_out_file, "/tmp/events.log") != 0 || strcmp(opts.frame_log_file, "/tmp/frames.log") != 0
        || strcmp(opts.p25_sm_log_file, "/tmp/p25-sm.log") != 0) {
        DSD_FPRINTF(stderr, "logging paths not applied correctly event=%s frame=%s p25_sm=%s\n", opts.event_out_file,
                    opts.frame_log_file, opts.p25_sm_log_file);
        rc |= 1;
    }
    if (opts.dmr_stereo_wav != 1 || strcmp(opts.wav_out_dir, "/tmp/wav") != 0) {
        DSD_FPRINTF(stderr, "recording per-call WAV settings not applied\n");
        rc |= 1;
    }
    if (opts.rdio_mode != DSD_RDIO_MODE_BOTH || opts.rdio_system_id != 77) {
        DSD_FPRINTF(stderr, "rdio mode/system_id not applied (%d/%d)\n", opts.rdio_mode, opts.rdio_system_id);
        rc |= 1;
    }
    if (strcmp(opts.rdio_api_url, "http://127.0.0.1:3000") != 0 || strcmp(opts.rdio_api_key, "apikey") != 0) {
        DSD_FPRINTF(stderr, "rdio API settings not applied\n");
        rc |= 1;
    }
    if (opts.rdio_upload_timeout_ms != 2500 || opts.rdio_upload_retries != 3) {
        DSD_FPRINTF(stderr, "rdio upload timeout/retries not applied (%d/%d)\n", opts.rdio_upload_timeout_ms,
                    opts.rdio_upload_retries);
        rc |= 1;
    }
    if (opts.rdio_api_delete_after_upload != 1) {
        DSD_FPRINTF(stderr, "rdio API delete-after-upload not applied (%d)\n", opts.rdio_api_delete_after_upload);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_load_and_apply_alerts_empty_event_mask(void) {
    static const char* ini = "[alerts]\n"
                             "enabled = true\n"
                             "voice_start = false\n"
                             "voice_end = false\n"
                             "data = false\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_alerts || !cfg.call_alert_enabled || cfg.call_alert_events != 0) {
        DSD_FPRINTF(stderr, "empty alert mask not parsed correctly enabled=%d events=%d\n", cfg.call_alert_enabled,
                    cfg.call_alert_events);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (opts.call_alert != 0 || opts.call_alert_events != 0) {
        DSD_FPRINTF(stderr, "empty alert mask should disable runtime alerts enabled=%d events=%u\n", opts.call_alert,
                    (unsigned)opts.call_alert_events);
        rc |= 1;
    }
    if (dsd_call_alert_event_enabled(opts.call_alert, opts.call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_START)
        || dsd_call_alert_event_enabled(opts.call_alert, opts.call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_END)
        || dsd_call_alert_event_enabled(opts.call_alert, opts.call_alert_events, DSD_CALL_ALERT_EVENT_DATA)) {
        DSD_FPRINTF(stderr, "empty alert mask should suppress every event\n");
        rc |= 1;
    }

    char rendered[1024];
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        rc |= 1;
    } else if (!strstr(rendered, "enabled = true\n") || !strstr(rendered, "voice_start = false\n")
               || !strstr(rendered, "voice_end = false\n") || !strstr(rendered, "data = false\n")) {
        DSD_FPRINTF(stderr, "empty alert mask not preserved in rendered config:\n%s\n", rendered);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

/* Issue #670 B3: an rtl_freq too long for the config's 64-byte field is refused at load, with a warning, instead of
   cut: the 64-character "146520000.000...M" lost its M and loaded as 146520000 Hz. The key keeps what it had. */
static int
test_rtl_freq_too_long_is_refused(void) {
    std::string long_freq = "146520000.";
    long_freq.append(53, '0');
    long_freq += 'M';
    if (long_freq.size() != 64U) {
        DSD_FPRINTF(stderr, "test value is %zu characters, want 64\n", long_freq.size());
        return 1;
    }
    int rc = 0;
    for (int with_prior = 0; with_prior <= 1; with_prior++) {
        char ini[512];
        DSD_SNPRINTF(ini, sizeof ini, "[input]\nsource = \"rtl\"\n%srtl_freq = \"%s\"\n",
                     with_prior ? "rtl_freq = \"155.340M\"\n" : "", long_freq.c_str());
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(ini, path, sizeof path) != 0) {
            return 1;
        }
        g_log_text[0] = '\0';
        dsdneoUserConfig cfg;
        if (dsd_user_config_load(path, &cfg) != 0) {
            DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
            (void)remove(path);
            return 1;
        }
        (void)remove(path);
        const char* want = with_prior ? "155.340M" : "";
        if (strcmp(cfg.rtl_freq, want) != 0) {
            DSD_FPRINTF(stderr, "long rtl_freq loaded as '%s', want '%s'\n", cfg.rtl_freq, want);
            rc |= 1;
        }
        if (!strstr(g_log_text, "rtl_freq")) {
            DSD_FPRINTF(stderr, "long rtl_freq loaded without a warning (log: '%s')\n", g_log_text);
            rc |= 1;
        }
        static dsd_opts opts;
        static dsd_state state;
        reset_opts_and_state(opts, state);
        opts.rtlsdr_center_freq = 162475000U;
        dsd_apply_user_config_to_opts(&cfg, &opts, &state);
        (void)dsd_rtl_input_spec_apply(&opts);
        const uint32_t want_hz = with_prior ? 155340000U : 162475000U;
        if (opts.rtlsdr_center_freq != want_hz) {
            DSD_FPRINTF(stderr, "long rtl_freq tuned %u, want %u\n", opts.rtlsdr_center_freq, want_hz);
            rc |= 1;
        }
    }
    return rc;
}

/* Issue #669 B10: an INI rtl_volume outside 0..3 reads as the default 1 on every radio source, as the local RTL setup
   and the menu always read it: 10 would multiply the monitor audio tenfold, and the menu cycle could never return to
   it. */
static int
test_rtl_volume_out_of_range_reads_as_default(void) {
    static const char* const sources[] = {"rtl", "soapy", "rtltcp"};
    int rc = 0;
    for (size_t i = 0; i < sizeof sources / sizeof sources[0]; i++) {
        for (int vol = -1; vol <= 10; vol++) {
            char ini[256];
            DSD_SNPRINTF(ini, sizeof ini,
                         "[input]\nsource = \"%s\"\nrtl_freq = \"155.340M\"\nrtltcp_host = \"127.0.0.1\"\n"
                         "rtltcp_port = 1234\nrtl_volume = %d\n",
                         sources[i], vol);
            char path[DSD_TEST_PATH_MAX];
            if (write_temp_config(ini, path, sizeof path) != 0) {
                return 1;
            }
            dsdneoUserConfig cfg;
            if (dsd_user_config_load(path, &cfg) != 0) {
                (void)remove(path);
                return 1;
            }
            (void)remove(path);
            static dsd_opts opts;
            static dsd_state state;
            reset_opts_and_state(opts, state);
            opts.rtl_volume_multiplier = 2;
            dsd_apply_user_config_to_opts(&cfg, &opts, &state);
            (void)dsd_rtl_input_spec_apply(&opts);
            /* rtl_volume = 0 is the key's unset value, which keeps the volume the session had. */
            const int want = (vol == 0) ? 2 : ((vol < 0 || vol > 3) ? 1 : vol);
            if (opts.rtl_volume_multiplier != want) {
                DSD_FPRINTF(stderr, "%s rtl_volume = %d gave %d, want %d\n", sources[i], vol,
                            opts.rtl_volume_multiplier, want);
                rc |= 1;
            }
        }
    }
    return rc;
}

static int
test_load_and_apply_soapy_input_no_args(void) {
    static const char* ini = "[input]\n"
                             "source = \"soapy\"\n"
                             "rtl_freq = \"155.340M\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_SOAPY) {
        DSD_FPRINTF(stderr, "input section not parsed as Soapy source\n");
        rc |= 1;
    }
    if (cfg.soapy_args[0] != '\0') {
        DSD_FPRINTF(stderr, "soapy_args expected empty for plain soapy source, got \"%s\"\n", cfg.soapy_args);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.rtl_gain_value = 77;
    opts.rtl_dsp_bw_khz = 16;
    opts.rtl_volume_multiplier = 3;

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (strcmp(opts.audio_in_dev, "soapy") != 0) {
        DSD_FPRINTF(stderr, "audio_in_dev mismatch for plain soapy: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }
    if (opts.rtlsdr_center_freq != 155340000U) {
        DSD_FPRINTF(stderr, "rtlsdr_center_freq mismatch for soapy source: %u\n", opts.rtlsdr_center_freq);
        rc |= 1;
    }
    if (opts.rtl_gain_value != 77 || opts.rtl_dsp_bw_khz != 16 || opts.rtl_volume_multiplier != 3) {
        DSD_FPRINTF(stderr, "soapy defaults should preserve existing rtl tuning values gain=%d bw=%d vol=%d\n",
                    opts.rtl_gain_value, opts.rtl_dsp_bw_khz, opts.rtl_volume_multiplier);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_load_and_apply_soapy_input_with_args(void) {
    static const char* ini = "[input]\n"
                             "source = \"soapy\"\n"
                             "soapy_args = \"driver=airspy,serial=ABC123\"\n"
                             "soapy_profile = \"airspy\"\n"
                             "soapy_stream_format = \"cf32\"\n"
                             "soapy_antenna = \"RX\"\n"
                             "soapy_clock = \"external\"\n"
                             "soapy_settings = \"rfnotch_ctrl=true,rx:agc_setpoint=-30\"\n"
                             "soapy_gains = \"LNA:12,MIX:8\"\n"
                             "soapy_bandwidth_hz = 250000\n"
                             "rtl_freq = \"851.375M\"\n"
                             "rtl_gain = 30\n"
                             "rtl_ppm = 5\n"
                             "rtl_bw_khz = 16\n"
                             "rtl_sql = -50\n"
                             "rtl_volume = 2\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_SOAPY) {
        DSD_FPRINTF(stderr, "input section not parsed as Soapy source\n");
        rc |= 1;
    }
    if (strcmp(cfg.soapy_args, "driver=airspy,serial=ABC123") != 0) {
        DSD_FPRINTF(stderr, "soapy_args parse mismatch: \"%s\"\n", cfg.soapy_args);
        rc |= 1;
    }
    if (strcmp(cfg.soapy_profile, "airspy") != 0 || strcmp(cfg.soapy_stream_format, "cf32") != 0
        || strcmp(cfg.soapy_antenna, "RX") != 0 || strcmp(cfg.soapy_clock, "external") != 0
        || strcmp(cfg.soapy_settings, "rfnotch_ctrl=true,rx:agc_setpoint=-30") != 0
        || strcmp(cfg.soapy_gains, "LNA:12,MIX:8") != 0 || cfg.soapy_bandwidth_hz != 250000
        || !cfg.soapy_bandwidth_hz_is_set) {
        DSD_FPRINTF(stderr, "soapy extended fields not parsed correctly\n");
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (strcmp(opts.audio_in_dev, "soapy:driver=airspy,serial=ABC123") != 0) {
        DSD_FPRINTF(stderr, "audio_in_dev mismatch for soapy args: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }
    if (opts.rtlsdr_center_freq != 851375000U) {
        DSD_FPRINTF(stderr, "rtlsdr_center_freq mismatch for soapy args: %u\n", opts.rtlsdr_center_freq);
        rc |= 1;
    }
    if (opts.rtl_gain_value != 30 || opts.rtlsdr_ppm_error != 5 || opts.rtl_dsp_bw_khz != 16
        || opts.rtl_volume_multiplier != 2) {
        DSD_FPRINTF(stderr, "shared rtl tuning values not applied for soapy gain=%d ppm=%d bw=%d vol=%d\n",
                    opts.rtl_gain_value, opts.rtlsdr_ppm_error, opts.rtl_dsp_bw_khz, opts.rtl_volume_multiplier);
        rc |= 1;
    }
    if (strcmp(opts.soapy_profile, "airspy") != 0 || strcmp(opts.soapy_stream_format, "cf32") != 0
        || strcmp(opts.soapy_antenna, "RX") != 0 || strcmp(opts.soapy_clock, "external") != 0
        || strcmp(opts.soapy_settings, "rfnotch_ctrl=true,rx:agc_setpoint=-30") != 0
        || strcmp(opts.soapy_gains, "LNA:12,MIX:8") != 0 || opts.soapy_bandwidth_hz != 250000) {
        DSD_FPRINTF(stderr, "soapy extended fields not applied correctly\n");
        rc |= 1;
    }
    if (!(opts.rtl_squelch_level > 0.0 && opts.rtl_squelch_level < 1.0)) {
        DSD_FPRINTF(stderr, "rtl_squelch_level should be mapped from dB for soapy input, got %.9f\n",
                    opts.rtl_squelch_level);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_load_and_apply_sddc_digital_resample(void) {
    /* RX-888 shape: SDDC profile, VHF tuner port, and an explicit digital resample mode. */
    static const char* ini = "[input]\n"
                             "source = \"soapy\"\n"
                             "soapy_args = \"driver=SDDC\"\n"
                             "soapy_profile = \"sddc\"\n"
                             "soapy_antenna = \"VHF\"\n"
                             "soapy_gains = \"RF:0,IF:24\"\n"
                             "soapy_settings = \"adc_frequency=98304000\"\n"
                             "soapy_bandwidth_hz = 0\n"
                             "digital_resample = \"off\"\n"
                             "rtl_freq = \"851.375M\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (strcmp(cfg.soapy_profile, "sddc") != 0 || strcmp(cfg.soapy_antenna, "VHF") != 0
        || strcmp(cfg.soapy_settings, "adc_frequency=98304000") != 0 || cfg.soapy_bandwidth_hz != 0
        || !cfg.soapy_bandwidth_hz_is_set) {
        DSD_FPRINTF(stderr, "sddc soapy fields not parsed correctly\n");
        rc |= 1;
    }
    if (strcmp(cfg.digital_resample, "off") != 0) {
        DSD_FPRINTF(stderr, "digital_resample parse mismatch: \"%s\"\n", cfg.digital_resample);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (opts.digital_resample_mode != 2) {
        DSD_FPRINTF(stderr, "digital_resample \"off\" applied as mode %d, want 2\n", opts.digital_resample_mode);
        rc |= 1;
    }
    if (strcmp(opts.soapy_profile, "sddc") != 0 || strcmp(opts.soapy_antenna, "VHF") != 0
        || opts.soapy_bandwidth_hz != 0) {
        DSD_FPRINTF(stderr, "sddc soapy fields not applied correctly\n");
        rc |= 1;
    }

    /* Auto is the default and stays implicit so configs are not littered with it. */
    static const char* ini_auto = "[input]\n"
                                  "source = \"soapy\"\n"
                                  "soapy_args = \"driver=SDDC\"\n";
    char auto_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini_auto, auto_path, sizeof auto_path) == 0) {
        dsdneoUserConfig auto_cfg;
        if (dsd_user_config_load(auto_path, &auto_cfg) == 0) {
            static dsd_opts auto_opts;
            static dsd_state auto_state;
            reset_opts_and_state(auto_opts, auto_state);
            dsd_apply_user_config_to_opts(&auto_cfg, &auto_opts, &auto_state);
            if (auto_opts.digital_resample_mode != 0) {
                DSD_FPRINTF(stderr, "omitted digital_resample applied as mode %d, want 0\n",
                            auto_opts.digital_resample_mode);
                rc |= 1;
            }
        } else {
            DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", auto_path);
            rc |= 1;
        }
        (void)remove(auto_path);
    } else {
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_load_and_apply_rtl_digital_resample(void) {
    /* digital_resample is not Soapy-specific: it governs the whole rtl-family demod chain
       (RTL USB, RTL-TCP, IQ replay), so an RTL-shaped config must honor it too. */
    static const char* ini = "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_freq = \"851.375M\"\n"
                             "digital_resample = \"off\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.digital_resample_mode != 2) {
        DSD_FPRINTF(stderr, "rtl source digital_resample \"off\" applied as mode %d, want 2\n",
                    opts.digital_resample_mode);
        rc |= 1;
    }

    /* Snapshot of an RTL input must record the mode so autosave round-trips it. */
    opts.digital_resample_mode = 1; /* on */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851375000");
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (strcmp(snap.digital_resample, "on") != 0) {
        DSD_FPRINTF(stderr, "rtl snapshot digital_resample mismatch: \"%s\", want \"on\"\n", snap.digital_resample);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

/* Issue #634: the RTL-SDR row names its device "rtl" when no radio spec is to hand; an input switch names UDP, TCP and
   Pulse inputs as -i and a saved config spell them. Each saves as the input it is. */
static int
test_snapshot_names_switched_inputs(void) {
    static const struct {
        const char* dev;
        int source;
    } cases[] = {
        {"rtl", DSDCFG_INPUT_RTL},
        {"rtltcp", DSDCFG_INPUT_RTLTCP},
        {"udp:127.0.0.1:7360", DSDCFG_INPUT_UDP},
        {"tcp:audio.example:7355", DSDCFG_INPUT_TCP},
        {"pulse:mic", DSDCFG_INPUT_PULSE},
        {"pulse", DSDCFG_INPUT_PULSE},
    };

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_opts_and_state(opts, state);
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", cases[i].dev);
        dsdneoUserConfig snap;
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        if (!snap.has_input || snap.input_source != cases[i].source) {
            DSD_FPRINTF(stderr, "snapshot of input %s: source %d, want %d\n", cases[i].dev, snap.input_source,
                        cases[i].source);
            rc |= 1;
        }
    }
    return rc;
}

/* Issue #743: a bare "tcp" or "udp" (-i tcp, -i udp) saves as the tcp or udp source it opens, the session's endpoint or
   the default one, not as a file named "tcp"; a saved and reloaded one opens that same endpoint. "-" (stdin) and
   "m17udp:" have no [input] source of their own and stay file inputs that reload as themselves. */
static int
test_snapshot_bare_network_inputs_save_as_their_source(void) {
    static const struct {
        const char* dev;
        const char* tcp_host;
        int tcp_port;
        const char* udp_addr;
        int udp_port;
        int source;
        const char* reopens;
    } cases[] = {
        {"tcp", "", 0, "", 0, DSDCFG_INPUT_TCP, "tcp:localhost:7355"},
        {"tcp", "audio.example", 7400, "", 0, DSDCFG_INPUT_TCP, "tcp:audio.example:7400"},
        {"udp", "", 0, "", 0, DSDCFG_INPUT_UDP, "udp:127.0.0.1:7355"},
        {"udp", "", 0, "0.0.0.0", 7360, DSDCFG_INPUT_UDP, "udp:0.0.0.0:7360"},
        {"-", "", 0, "", 0, DSDCFG_INPUT_FILE, "-"},
        {"m17udp:127.0.0.1:17000", "", 0, "", 0, DSDCFG_INPUT_FILE, "m17udp:127.0.0.1:17000"},
    };

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_opts_and_state(opts, state);
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", cases[i].dev);
        DSD_SNPRINTF(opts.tcp_hostname, sizeof opts.tcp_hostname, "%s", cases[i].tcp_host);
        opts.tcp_portno = cases[i].tcp_port;
        DSD_SNPRINTF(opts.udp_in_bindaddr, sizeof opts.udp_in_bindaddr, "%s", cases[i].udp_addr);
        opts.udp_in_portno = cases[i].udp_port;
        static dsdneoUserConfig snap;
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        if (!snap.has_input || snap.input_source != cases[i].source) {
            DSD_FPRINTF(stderr, "FAIL: bare input %s saved as source %d, want %d\n", cases[i].dev, snap.input_source,
                        cases[i].source);
            rc |= 1;
            continue;
        }
        char path[DSD_TEST_PATH_MAX];
        static dsdneoUserConfig loaded;
        char rendered[8192];
        if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0
            || write_temp_config(rendered, path, sizeof path) != 0) {
            return 1;
        }
        const int load_rc = dsd_user_config_load(path, &loaded);
        (void)remove(path);
        reset_opts_and_state(opts, state);
        if (load_rc == 0) {
            dsd_apply_user_config_to_opts(&loaded, &opts, &state);
        }
        if (load_rc != 0 || strcmp(opts.audio_in_dev, cases[i].reopens) != 0) {
            DSD_FPRINTF(stderr, "FAIL: bare input %s reloads as \"%s\", want \"%s\"\n", cases[i].dev, opts.audio_in_dev,
                        cases[i].reopens);
            rc |= 1;
        }
    }
    return rc;
}

/* The session of an I/Q replay of @p replay_dev, which runs its capture's own centre and gain. */
static void
set_iq_replay_session(dsd_opts& opts, const char* replay_dev) {
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.iq_replay_requested = 1;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", replay_dev);
    opts.rtlsdr_center_freq = 460125000U;
    opts.iq_replay_center_freq = 460125000U;
    opts.rtl_gain_value = 44;
    opts.input_warn_db = -12.0;
}

/*
 * Issue #743 (B1): an I/Q replay has no INI form, so a replay session saves the [input] of the config it loaded as it
 * loaded it (none of the replay's centre, gain or input threshold), or no [input] when it loaded none. Before, it saved
 * source = "file" with file_path = "iqreplay:...", which the next start refuses.
 */
static int
test_replay_snapshot_saves_the_loaded_input(void) {
    static const char* ini = "[input]\n"
                             "source = \"rtltcp\"\n"
                             "rtltcp_host = \"radio.example\"\n"
                             "rtltcp_port = 1235\n"
                             "rtl_freq = \"851.0125M\"\n"
                             "rtl_gain = 12\n"
                             "rtl_bw_khz = 24\n"
                             "input_warn_db = -30.0\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    static dsdneoUserConfig cfg;
    const int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: replay [input] config did not load\n");
        return 1;
    }

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    set_iq_replay_session(opts, "iqreplay:/captures/site.iq.json");
    rc |= dsd_user_config_retain_input(&state, &cfg) == 0 ? 0 : 1;
    static dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        dsd_state_ext_free_all(&state);
        return 1;
    }
    rc |= expect_contains("replay save: the loaded source", rendered, "[input]\nsource = \"rtltcp\"\n");
    rc |= expect_contains("replay save: the loaded host", rendered, "rtltcp_host = \"radio.example\"\n");
    rc |= expect_contains("replay save: the loaded port", rendered, "rtltcp_port = 1235\n");
    rc |= expect_contains("replay save: the loaded frequency, as written", rendered, "rtl_freq = \"851.0125M\"\n");
    rc |= expect_contains("replay save: the loaded gain, not the replay's", rendered, "rtl_gain = 12\n");
    rc |= expect_contains("replay save: the loaded bandwidth", rendered, "rtl_bw_khz = 24\n");
    rc |= expect_contains("replay save: the loaded input threshold", rendered, "input_warn_db = -30.0\n");
    rc |= expect_absent_quiet("replay save: no replay spec", rendered, "iqreplay");
    rc |= expect_absent_quiet("replay save: no file input", rendered, "source = \"file\"");

    /* A session that loaded no config (or one without an [input]) saves no [input]. */
    dsd_user_config_forget_retained_input(&state);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.has_input) {
        DSD_FPRINTF(stderr, "FAIL: replay save with no loaded [input] wrote one (source %d)\n", snap.input_source);
        rc |= 1;
    }
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        dsd_state_ext_free_all(&state);
        return 1;
    }
    rc |= expect_absent_quiet("replay save without a config: no [input]", rendered, "[input]");
    rc |= expect_absent_quiet("replay save without a config: no replay spec", rendered, "iqreplay");

    /* A session that is no replay saves its own input, whatever was retained. */
    reset_opts_and_state(opts, state);
    rc |= dsd_user_config_retain_input(&state, &cfg) == 0 ? 0 : 1;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_PULSE) {
        DSD_FPRINTF(stderr, "FAIL: a live session saved source %d, want its own pulse input\n", snap.input_source);
        rc |= 1;
    }
    /* A file whose name only starts with "pulse" is a file, as startup opens it: it was saved as a Pulse source. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse.wav");
    DSD_MEMSET(&snap, 0, sizeof snap);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.input_source != DSDCFG_INPUT_FILE || strcmp(snap.file_path, "pulse.wav") != 0) {
        DSD_FPRINTF(stderr, "FAIL: pulse.wav saved as source %d path \"%s\", want the file\n", snap.input_source,
                    snap.file_path);
        rc |= 1;
    }
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse:monitor0");
    DSD_MEMSET(&snap, 0, sizeof snap);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.input_source != DSDCFG_INPUT_PULSE || strcmp(snap.pulse_input, "monitor0") != 0) {
        DSD_FPRINTF(stderr, "FAIL: pulse:monitor0 saved as source %d device \"%s\"\n", snap.input_source,
                    snap.pulse_input);
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/* Whether @p snap, saved and read back, passes --validate-config: no error, an I/Q stream rule included. */
static int
expect_snapshot_validates(const char* label, const dsdneoUserConfig* snap) {
    static char rendered[32768];
    char path[DSD_TEST_PATH_MAX];
    if (render_config_to_buffer(snap, rendered, sizeof rendered) != 0
        || write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }
    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof diags);
    const int validate_rc = dsd_user_config_validate(path, &diags);
    (void)remove(path);
    int rc = 0;
    if (validate_rc != 0 || diags.error_count != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: the save does not validate (rc=%d, %d error(s))\n", label, validate_rc,
                    diags.error_count);
        for (int d = 0; d < diags.count; d++) {
            DSD_FPRINTF(stderr, "  [%s] %s: %s\n", diags.items[d].section, diags.items[d].key, diags.items[d].message);
        }
        rc = 1;
    }
    dsdcfg_diags_free(&diags);
    return rc;
}

/*
 * Issue #743 (D8 over B1): a replay session that loaded an I/Q stream [input] saves that [input], and the next start
 * refuses a stream beside trunking, the -Y scanner or a trunk scan. A replay runs them, so the session's save writes each
 * of them off: autosave, Save and --print-config leave a config the next start runs. Before, the save wrote the stream
 * [input] with the replay's trunking on, and the next start of that config exited 1. A live stream session saves the
 * same way; any other retained [input] keeps the session's trunking.
 */
static int
test_replay_snapshot_turns_off_tuning_beside_a_stream_input(void) {
    static const char* stream_ini = "[input]\n"
                                    "source = \"iqtcp\"\n"
                                    "iq_host = \"127.0.0.1\"\n"
                                    "iq_port = 1234\n"
                                    "iq_rate = \"240k\"\n"
                                    "iq_format = \"cs16\"\n"
                                    "[trunking]\n"
                                    "enabled = false\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(stream_ini, path, sizeof path) != 0) {
        return 1;
    }
    static dsdneoUserConfig stream_cfg;
    const int load_rc = dsd_user_config_load(path, &stream_cfg);
    (void)remove(path);
    if (load_rc != 0 || stream_cfg.input_source != DSDCFG_INPUT_IQTCP) {
        DSD_FPRINTF(stderr, "FAIL: stream [input] config did not load\n");
        return 1;
    }

    static const struct {
        const char* label;
        int replay;
        int trunk;
        int scanner;
        int trunk_scan;
        int next_start_trunk_scan;
    } cases[] = {
        {"replay with trunking", 1, 1, 0, 0, 0},
        {"replay with the -Y scanner", 1, 0, 1, 0, 0},
        {"replay with trunking and the scanner it outranks", 1, 1, 1, 0, 0},
        {"replay with a trunk scan", 1, 0, 0, 1, 0},
        {"replay with a trunk scan waiting for the next start", 1, 0, 0, 0, 1},
        {"replay with all of them", 1, 1, 1, 1, 1},
        {"live stream with trunking", 0, 1, 1, 1, 0},
    };

    static dsd_opts opts;
    static dsd_state state;
    static dsdneoUserConfig snap;
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_opts_and_state(opts, state);
        dsd_apply_user_config_to_opts(&stream_cfg, &opts, &state);
        if (cases[i].replay) {
            set_iq_replay_session(opts, "iqreplay:/captures/site.iq.json");
            rc |= dsd_user_config_retain_input(&state, &stream_cfg) == 0 ? 0 : 1;
        }
        opts.trunk_enable = cases[i].trunk;
        opts.scanner_mode = cases[i].scanner;
        opts.trunk_scan_enabled = cases[i].trunk_scan;
        if (cases[i].next_start_trunk_scan) {
            opts.next_start.pending |= DSD_NEXT_START_TRUNK_SCAN;
            opts.next_start.trunk_scan_enabled = 1;
        }
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        if (!snap.has_input || snap.input_source != DSDCFG_INPUT_IQTCP || snap.trunk_enabled != 0
            || snap.trunk_scanner != 0 || snap.trunk_scan_enabled != 0) {
            DSD_FPRINTF(
                stderr, "FAIL: %s: saved source %d, trunking %d, scanner %d, trunk scan %d; want iqtcp, all off\n",
                cases[i].label, snap.input_source, snap.trunk_enabled, snap.trunk_scanner, snap.trunk_scan_enabled);
            rc |= 1;
        }
        char why[256];
        if (dsd_user_config_check_trunk_scan_startable(&snap, nullptr, why, sizeof why) != 0) {
            DSD_FPRINTF(stderr, "FAIL: %s: the save breaks a next-start rule: %s\n", cases[i].label, why);
            rc |= 1;
        }
        rc |= expect_snapshot_validates(cases[i].label, &snap);
        dsd_state_ext_free_all(&state);
    }

    /* Any other retained [input] keeps the session's trunking. */
    static const char* rtltcp_ini = "[input]\n"
                                    "source = \"rtltcp\"\n"
                                    "rtltcp_host = \"radio.example\"\n"
                                    "rtltcp_port = 1235\n";
    if (write_temp_config(rtltcp_ini, path, sizeof path) != 0) {
        return 1;
    }
    static dsdneoUserConfig rtltcp_cfg;
    const int rtltcp_rc = dsd_user_config_load(path, &rtltcp_cfg);
    (void)remove(path);
    if (rtltcp_rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: rtltcp [input] config did not load\n");
        return 1;
    }
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&rtltcp_cfg, &opts, &state);
    set_iq_replay_session(opts, "iqreplay:/captures/site.iq.json");
    rc |= dsd_user_config_retain_input(&state, &rtltcp_cfg) == 0 ? 0 : 1;
    opts.trunk_enable = 1;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.input_source != DSDCFG_INPUT_RTLTCP || snap.trunk_enabled != 1) {
        DSD_FPRINTF(stderr, "FAIL: replay beside an rtltcp [input] saved source %d, trunking %d; want rtltcp, on\n",
                    snap.input_source, snap.trunk_enabled);
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/* The retained [input] as a config, for the merge rows below. */
static const dsdneoUserConfig*
retained_of(const dsd_state* state) {
    static dsdneoUserConfig out;
    dsd_user_config_init(&out);
    (void)dsd_user_config_retained_input(state, &out);
    return &out;
}

/* Issue #899 B15: the rounding note of a retained [input] travels with its rtl_sql. A loaded config's rtl_sql merged
   over a snapshot that rounded -65.5 dB to -66 holds a whole dB, so a save during the replay says nothing about the
   -65.5 dB it no longer writes. */
static int
test_retained_input_merge_clears_the_rounding_note(void) {
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_state& state = *state_storage;
    static dsdneoUserConfig snap;
    static dsdneoUserConfig patch;
    dsd_user_config_init(&snap);
    snap.has_input = 1;
    snap.input_source = DSDCFG_INPUT_RTL;
    snap.rtl_sql = -66;
    snap.rtl_sql_is_set = 1;
    snap.rtl_sql_rounded = 1;
    snap.rtl_sql_unrounded_db = -65.5;
    int rc = dsd_user_config_retain_input(&state, &snap) == 0 ? 0 : 1;
    dsd_user_config_init(&patch);
    patch.has_input = 1;
    patch.rtl_sql = -40;
    patch.rtl_sql_is_set = 1;
    rc |= dsd_user_config_merge_retained_input(&state, &patch) == 0 ? 0 : 1;
    const dsdneoUserConfig* kept = retained_of(&state);
    if (kept->rtl_sql != -40 || kept->rtl_sql_rounded != 0) {
        DSD_FPRINTF(stderr, "FAIL: merged rtl_sql %d keeps the rounding note %d (%.2f dB)\n", kept->rtl_sql,
                    kept->rtl_sql_rounded, kept->rtl_sql_unrounded_db);
        rc |= 1;
    }
    dsd_user_config_forget_retained_input(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * Issue #743 (G4): a config patch applied during a replay (the control API's config_apply, built from
 * dsd_user_config_init() and the keys given) merges the [input] keys it sets into the retained [input]: one naming no
 * source keeps the source and endpoint; a flagged key merges by its flag, explicit 0 included; a patch naming a source
 * changes it. An [input] no config can start (a file input of an iqreplay: spec, as the B1 bug saved) is never
 * retained, whole or merged: the copy held stays, and bootstrap retains none.
 */
static int
test_retained_input_merges_a_patch(void) {
    static dsd_state state;
    static dsdneoUserConfig loaded;
    static dsdneoUserConfig patch;
    static dsd_state empty_state;
    int rc = 0;
    dsd_user_config_init(&loaded);
    loaded.has_input = 1;
    loaded.input_source = DSDCFG_INPUT_RTLTCP;
    DSD_SNPRINTF(loaded.rtltcp_host, sizeof loaded.rtltcp_host, "%s", "radio.example");
    loaded.rtltcp_port = 1235;
    DSD_SNPRINTF(loaded.rtl_freq, sizeof loaded.rtl_freq, "%s", "851.0125M");
    loaded.rtl_gain = 12;
    loaded.rtl_gain_is_set = 1;
    rc |= dsd_user_config_retain_input(&state, &loaded) == 0 ? 0 : 1;

    dsd_user_config_init(&patch);
    patch.has_input = 1;
    patch.input_warn_db = -20.0;
    patch.input_warn_db_is_set = 1;
    patch.rtl_gain = 0; /* explicit AGC */
    patch.rtl_gain_is_set = 1;
    rc |= dsd_user_config_merge_retained_input(&state, &patch) == 0 ? 0 : 1;
    const dsdneoUserConfig* kept = retained_of(&state);
    if (kept->input_source != DSDCFG_INPUT_RTLTCP || strcmp(kept->rtltcp_host, "radio.example") != 0
        || kept->rtltcp_port != 1235 || strcmp(kept->rtl_freq, "851.0125M") != 0 || !kept->input_warn_db_is_set
        || fabs(kept->input_warn_db - (-20.0)) > 1e-9 || !kept->rtl_gain_is_set || kept->rtl_gain != 0) {
        DSD_FPRINTF(stderr, "FAIL: sourceless patch: source %d host \"%s\" freq \"%s\" warn %.1f/%d gain %d/%d\n",
                    kept->input_source, kept->rtltcp_host, kept->rtl_freq, kept->input_warn_db,
                    kept->input_warn_db_is_set, kept->rtl_gain, kept->rtl_gain_is_set);
        rc |= 1;
    }

    dsd_user_config_init(&patch);
    patch.has_input = 1;
    patch.input_source = DSDCFG_INPUT_RTL;
    patch.rtl_bw_khz = 24;
    rc |= dsd_user_config_merge_retained_input(&state, &patch) == 0 ? 0 : 1;
    kept = retained_of(&state);
    if (kept->input_source != DSDCFG_INPUT_RTL || kept->rtl_bw_khz != 24 || strcmp(kept->rtl_freq, "851.0125M") != 0) {
        DSD_FPRINTF(stderr, "FAIL: source patch: source %d bw %d freq \"%s\"\n", kept->input_source, kept->rtl_bw_khz,
                    kept->rtl_freq);
        rc |= 1;
    }

    /* A broken B1 [input] is retained neither merged nor whole; with none held, nothing is retained. */
    dsd_user_config_init(&patch);
    patch.has_input = 1;
    patch.input_source = DSDCFG_INPUT_FILE;
    DSD_SNPRINTF(patch.file_path, sizeof patch.file_path, "%s", "iqreplay:/captures/site.iq.json");
    rc |= dsd_user_config_merge_retained_input(&state, &patch) == 0 ? 0 : 1;
    rc |= dsd_user_config_retain_input(&state, &patch) == 0 ? 0 : 1;
    kept = retained_of(&state);
    if (kept->input_source != DSDCFG_INPUT_RTL || kept->file_path[0] != '\0') {
        DSD_FPRINTF(stderr, "FAIL: broken file [input] retained: source %d file \"%s\"\n", kept->input_source,
                    kept->file_path);
        rc |= 1;
    }
    rc |= dsd_user_config_retain_input(&empty_state, &patch) == 0 ? 0 : 1;
    if (dsd_user_config_retained_input(&empty_state, &patch) != 0) {
        DSD_FPRINTF(stderr, "FAIL: a broken file [input] was retained with none held\n");
        rc |= 1;
    }
    if (dsd_user_config_file_path_is_input_spec("iqreplay:/x.iq.json") != 1
        || dsd_user_config_file_path_is_input_spec("/captures/iqreplay.wav") != 0
        || dsd_user_config_file_path_is_input_spec(NULL) != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_user_config_file_path_is_input_spec()\n");
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    dsd_state_ext_free_all(&empty_state);
    return rc;
}

/* Load @p ini into @p cfg; 1 on failure. The log the load wrote is in g_log_text. */
static int
load_rtl_ini_text(const char* ini, dsdneoUserConfig* cfg) {
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    g_log_text[0] = '\0';
    const int rc = dsd_user_config_load(path, cfg) != 0 ? 1 : 0;
    (void)remove(path);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for:\n%s\n", ini);
    }
    return rc;
}

/* What a start would open for @p cfg: the spec it builds and the selector the spec parser reads from it (-1 refused). */
static int
rtl_start_selector(const dsdneoUserConfig* cfg, dsd_opts& opts, dsd_state& state) {
    reset_opts_and_state(opts, state);
    opts.rtl_dev_index = 7;
    DSD_SNPRINTF(opts.rtl_dev_serial, sizeof opts.rtl_dev_serial, "%s", "STALE");
    dsd_apply_user_config_to_opts(cfg, &opts, &state);
    return dsd_rtl_input_spec_apply(&opts);
}

/*
 * [input] rtl_serial (RTL-SDR by USB serial): it wins over rtl_device in the spec it builds (`rtl:serial=<sn>:...`, the
 * explicit form, so a serial of digits stays a serial), and reads back whole through a render and a reload: spaces
 * inside, '#', ';' and '=' survive the comment stripper inside the quotes the render writes. rtl_device alone builds
 * `rtl:<index>:...` and clears a serial the session held.
 */
static int
test_rtl_serial_loads_renders_and_builds_the_spec(void) {
    int rc = 0;
    static dsdneoUserConfig cfg;
    static dsdneoUserConfig again;
    /* Heap fixtures for this case alone (AGENTS.md: dsd_opts and dsd_state live on the heap). */
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    dsd_opts& opts = *opts_storage;
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_state& state = *state_storage;

    static const struct {
        const char* keys;
        const char* serial;
        int device;
        const char* spec_prefix;
    } k_cases[] = {
        {"rtl_serial = \"A B#;=x\"\n", "A B#;=x", 0, "rtl:serial=A B#;=x:851.375M:"},
        {"rtl_device = 2\nrtl_serial = \"00000001\"\n", "00000001", 2, "rtl:serial=00000001:851.375M:"},
        {"rtl_serial = 1090\n", "1090", 0, "rtl:serial=1090:851.375M:"},
        {"rtl_serial = \"\"\nrtl_device = 3\n", "", 3, "rtl:3:851.375M:"},
        {"rtl_device = 0\n", "", 0, "rtl:0:851.375M:"},
    };

    for (const auto& k : k_cases) {
        char ini[512];
        DSD_SNPRINTF(ini, sizeof ini, "[input]\nsource = \"rtl\"\nrtl_freq = \"851.375M\"\n%s", k.keys);
        if (load_rtl_ini_text(ini, &cfg) != 0) {
            return 1;
        }
        if (strcmp(cfg.rtl_serial, k.serial) != 0 || cfg.rtl_device != k.device || cfg.rtl_serial_invalid) {
            DSD_FPRINTF(stderr, "FAIL: %s loaded serial '%s' device %d invalid %d\n", k.keys, cfg.rtl_serial,
                        cfg.rtl_device, cfg.rtl_serial_invalid);
            rc |= 1;
        }
        if (rtl_start_selector(&cfg, opts, state) != 1
            || strncmp(opts.audio_in_dev, k.spec_prefix, strlen(k.spec_prefix)) != 0
            || strcmp(opts.rtl_dev_serial, k.serial) != 0 || (k.serial[0] == '\0' && opts.rtl_dev_index != k.device)) {
            DSD_FPRINTF(stderr, "FAIL: %s built '%s' -> serial '%s' index %d\n", k.keys, opts.audio_in_dev,
                        opts.rtl_dev_serial, opts.rtl_dev_index);
            rc |= 1;
        }
        char rendered[16384];
        if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0 || load_rtl_ini_text(rendered, &again) != 0) {
            return 1;
        }
        const int names_device = strstr(rendered, "rtl_device = ") != NULL;
        if (strcmp(again.rtl_serial, k.serial) != 0 || again.rtl_serial_invalid || again.rtl_device != k.device
            || names_device != (k.serial[0] == '\0' || k.device != 0)) {
            DSD_FPRINTF(stderr, "FAIL: %s rendered and reloaded as serial '%s' device %d:\n%s\n", k.keys,
                        again.rtl_serial, again.rtl_device, rendered);
            rc |= 1;
        }
    }
    return rc;
}

/*
 * An rtl_serial the rule refuses (':' or '"' in it, a space at either end, longer than DSD_RTL_SERIAL_MAX) is never cut
 * or trimmed into another serial, and never falls through to rtl_device's index: the loader warns and marks it, the
 * spec it builds carries a device field the parser refuses (the start refuses the input), and a render writes a value
 * the loader refuses again. A valid rtl_serial later in the file clears the mark.
 */
static int
test_rtl_serial_invalid_refuses_the_input(void) {
    int rc = 0;
    static dsdneoUserConfig cfg;
    static dsdneoUserConfig again;
    /* Heap fixtures for this case alone (AGENTS.md: dsd_opts and dsd_state live on the heap). */
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    dsd_opts& opts = *opts_storage;
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_state& state = *state_storage;
    const std::string too_long(DSD_RTL_SERIAL_MAX + 1, 'A');
    const std::string values[] = {"\"A:B\"", "\" A\"", "\"A \"", "\"A\"B\"", "\"" + too_long + "\""};
    for (const std::string& value : values) {
        const std::string ini =
            "[input]\nsource = \"rtl\"\nrtl_freq = \"851.375M\"\nrtl_device = 1\nrtl_serial = " + value + "\n";
        if (load_rtl_ini_text(ini.c_str(), &cfg) != 0) {
            return 1;
        }
        if (!cfg.rtl_serial_invalid || cfg.rtl_serial[0] != '\0' || !strstr(g_log_text, "invalid rtl_serial")) {
            DSD_FPRINTF(stderr, "FAIL: rtl_serial = %s loaded serial '%s' invalid %d (log '%s')\n", value.c_str(),
                        cfg.rtl_serial, cfg.rtl_serial_invalid, g_log_text);
            rc |= 1;
        }
        if (rtl_start_selector(&cfg, opts, state) != -1) {
            DSD_FPRINTF(stderr, "FAIL: rtl_serial = %s started '%s' (index %d serial '%s')\n", value.c_str(),
                        opts.audio_in_dev, opts.rtl_dev_index, opts.rtl_dev_serial);
            rc |= 1;
        }
        char rendered[16384];
        if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0 || load_rtl_ini_text(rendered, &again) != 0) {
            return 1;
        }
        if (!again.rtl_serial_invalid) {
            DSD_FPRINTF(stderr, "FAIL: rtl_serial = %s saved as a config that opens a dongle:\n%s\n", value.c_str(),
                        rendered);
            rc |= 1;
        }
    }
    if (load_rtl_ini_text("[input]\nrtl_serial = \"A:B\"\nrtl_serial = \"00000002\"\n", &cfg) != 0) {
        return 1;
    }
    if (cfg.rtl_serial_invalid || strcmp(cfg.rtl_serial, "00000002") != 0) {
        DSD_FPRINTF(stderr, "FAIL: a valid rtl_serial after a refused one: '%s' invalid %d\n", cfg.rtl_serial,
                    cfg.rtl_serial_invalid);
        rc |= 1;
    }
    return rc;
}

/*
 * A save names the dongle the running spec names (snapshot_rtl_device_field()): its serial or its index, the other
 * cleared; a bare `rtl` or an empty device field the one the session opens (its serial when it has one); a device
 * field the parser refuses stays refused. A serial of digits saves as a serial and builds `serial=1090` again.
 */
static int
test_rtl_serial_snapshot_names_the_running_dongle(void) {
    int rc = 0;
    /* Heap fixtures for this case alone (AGENTS.md: dsd_opts and dsd_state live on the heap). */
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    dsd_opts& opts = *opts_storage;
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_state& state = *state_storage;
    static dsdneoUserConfig snap;

    static const struct {
        const char* spec;
        int index;
        const char* session_serial;
        const char* want_serial;
        int want_device;
        int want_invalid;
    } k_cases[] = {
        /* A serial names no index: the one it opened at is not saved beside it. */
        {"rtl:serial=00000001:851.375M", 4, "", "00000001", 0, 0},
        {"rtl:serial=1090:851.375M", 0, "", "1090", 0, 0},
        {"rtl:A1B2:851.375M", 0, "", "A1B2", 0, 0},
        {"rtl:2:851.375M", 0, "OLD", "", 2, 0},
        /* The spec alone names the dongle: a bare `rtl` or an empty field is index 0 with no serial, whatever the
           options last held. */
        {"rtl", 3, "SESSION", "", 0, 0},
        {"rtl::851.375M", 5, "", "", 0, 0},
        {"rtl::851.375M", 5, "SESSION SN", "", 0, 0},
        {"rtl:serial=:851.375M", 1, "", "", 0, 1},
    };

    for (const auto& k : k_cases) {
        reset_opts_and_state(opts, state);
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.rtl_dev_index = k.index;
        DSD_SNPRINTF(opts.rtl_dev_serial, sizeof opts.rtl_dev_serial, "%s", k.session_serial);
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", k.spec);
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        if (snap.input_source != DSDCFG_INPUT_RTL || strcmp(snap.rtl_serial, k.want_serial) != 0
            || snap.rtl_device != k.want_device || snap.rtl_serial_invalid != k.want_invalid) {
            DSD_FPRINTF(stderr, "FAIL: snapshot of '%s' (index %d serial '%s'): serial '%s' device %d invalid %d\n",
                        k.spec, k.index, k.session_serial, snap.rtl_serial, snap.rtl_device, snap.rtl_serial_invalid);
            rc |= 1;
        }
    }
    return rc;
}

/*
 * The RTL-SDR selector of a patch merges into a retained [input] (an I/Q replay session's) as one choice, by the keys
 * the patch names: rtl_device alone selects that index and drops the retained serial (index 0 included, the default
 * value); an explicit empty rtl_serial drops the serial and keeps the retained index; both keys take the serial; a
 * tuning-only patch keeps the serial; a refused serial stays refused.
 */
static int
test_retained_input_merges_the_rtl_selector(void) {
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_state& state = *state_storage;
    static dsdneoUserConfig loaded;
    static dsdneoUserConfig patch;
    int rc = 0;

    struct step {
        const char* serial;
        const char* want_serial;
        int names_device;
        int device;
        int names_serial;
        int invalid;
        int want_device;
        int want_invalid;
    };

    static const step k_steps[] = {
        {"", "A", 0, 0, 0, 0, 2, 0},  /* tuning only: the serial stays */
        {"", "", 1, 0, 0, 0, 0, 0},   /* rtl_device = 0 drops it */
        {"B", "B", 0, 0, 1, 0, 0, 0}, /* a serial again */
        {"", "", 0, 0, 1, 0, 0, 0},   /* explicit empty serial: by the retained index */
        {"C", "C", 1, 6, 1, 0, 6, 0}, /* both: the serial */
        {"", "", 0, 0, 1, 1, 6, 1},   /* a refused serial stays refused */
        {"", "", 0, 0, 0, 0, 6, 1},   /* and a tuning patch keeps it refused */
        /* A config a C caller filled in names its selector by value, without the loader's flags. */
        {"", "", 0, 2, 0, 0, 2, 0},   /* index 2 drops the refused serial */
        {"Z", "Z", 0, 0, 0, 0, 2, 0}, /* a serial */
        {"", "Z", 0, 0, 0, 0, 2, 0},  /* and a tuning patch keeps it */
    };
    dsd_user_config_init(&loaded);
    loaded.has_input = 1;
    loaded.input_source = DSDCFG_INPUT_RTL;
    loaded.rtl_device = 2;
    loaded.rtl_device_is_set = 1;
    DSD_SNPRINTF(loaded.rtl_serial, sizeof loaded.rtl_serial, "%s", "A");
    loaded.rtl_serial_is_set = 1;
    DSD_SNPRINTF(loaded.rtl_freq, sizeof loaded.rtl_freq, "%s", "851.375M");
    rc |= dsd_user_config_retain_input(&state, &loaded) == 0 ? 0 : 1;
    int n = 0;
    for (const step& k : k_steps) {
        dsd_user_config_init(&patch);
        patch.has_input = 1;
        patch.rtl_gain = 20;
        patch.rtl_gain_is_set = 1;
        patch.rtl_device = k.device;
        patch.rtl_device_is_set = k.names_device;
        DSD_SNPRINTF(patch.rtl_serial, sizeof patch.rtl_serial, "%s", k.serial);
        patch.rtl_serial_is_set = k.names_serial;
        patch.rtl_serial_invalid = k.invalid;
        rc |= dsd_user_config_merge_retained_input(&state, &patch) == 0 ? 0 : 1;
        const dsdneoUserConfig* kept = retained_of(&state);
        if (strcmp(kept->rtl_serial, k.want_serial) != 0 || kept->rtl_device != k.want_device
            || kept->rtl_serial_invalid != k.want_invalid || strcmp(kept->rtl_freq, "851.375M") != 0) {
            DSD_FPRINTF(stderr, "FAIL: selector merge step %d: serial '%s' device %d invalid %d freq '%s'\n", n,
                        kept->rtl_serial, kept->rtl_device, kept->rtl_serial_invalid, kept->rtl_freq);
            rc |= 1;
        }
        n++;
    }
    dsd_user_config_forget_retained_input(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * A retained [input] naming both an index and a serial (rtl_device = 2, rtl_serial = A) keeps the index through a save:
 * the render writes it beside the serial, so after the reload a patch clearing the serial falls back on index 2, as it
 * does before the save. The render dropped it, and the same patch then selected index 0. A serial-only config renders
 * no index.
 */
static int
test_retained_index_beside_a_serial_survives_a_save(void) {
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_state& state = *state_storage;
    auto loaded = std::unique_ptr<dsdneoUserConfig>(new dsdneoUserConfig{});
    auto reloaded = std::unique_ptr<dsdneoUserConfig>(new dsdneoUserConfig{});
    auto patch = std::unique_ptr<dsdneoUserConfig>(new dsdneoUserConfig{});
    int rc = 0;
    if (load_rtl_ini_text("[input]\nsource = \"rtl\"\nrtl_freq = \"851.375M\"\nrtl_device = 2\nrtl_serial = \"A\"\n",
                          loaded.get())
        != 0) {
        return 1;
    }
    char rendered[16384];
    if (render_config_to_buffer(loaded.get(), rendered, sizeof rendered) != 0
        || load_rtl_ini_text(rendered, reloaded.get()) != 0) {
        return 1;
    }
    rc |= dsd_user_config_retain_input(&state, reloaded.get()) == 0 ? 0 : 1;
    dsd_user_config_init(patch.get());
    patch->has_input = 1;
    patch->rtl_serial_is_set = 1; /* rtl_serial = "" */
    rc |= dsd_user_config_merge_retained_input(&state, patch.get()) == 0 ? 0 : 1;
    const dsdneoUserConfig* kept = retained_of(&state);
    if (kept->rtl_serial[0] != '\0' || kept->rtl_device != 2) {
        DSD_FPRINTF(stderr, "FAIL: render, reload, clear the serial: serial '%s' device %d; rendered:\n%s\n",
                    kept->rtl_serial, kept->rtl_device, rendered);
        rc |= 1;
    }
    if (load_rtl_ini_text("[input]\nsource = \"rtl\"\nrtl_freq = \"851.375M\"\nrtl_serial = \"A\"\n", loaded.get()) != 0
        || render_config_to_buffer(loaded.get(), rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "rtl_device") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: a serial-only config rendered an index:\n%s\n", rendered);
        rc |= 1;
    }
    dsd_user_config_forget_retained_input(&state);
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * The spec an RTL-SDR [input] builds is whole or none (dsd_user_config_radio_input_spec()): a buffer the complete spec
 * does not fit (64 bytes for a 127-byte serial) gets -1 and an empty string, never a spec cut inside the serial, which
 * would name another dongle (`rtl:serial=<shorter serial>`).
 */
static int
test_rtl_input_spec_is_whole_or_none(void) {
    auto cfg = std::unique_ptr<dsdneoUserConfig>(new dsdneoUserConfig{});
    auto opts = std::unique_ptr<dsd_opts>(new dsd_opts{});
    dsd_user_config_init(cfg.get());
    cfg->has_input = 1;
    cfg->input_source = DSDCFG_INPUT_RTL;
    DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "%s", "851.375M");
    DSD_MEMSET(cfg->rtl_serial, 'S', DSD_RTL_SERIAL_MAX);
    cfg->rtl_serial[DSD_RTL_SERIAL_MAX] = '\0';
    char small[64];
    DSD_MEMSET(small, 'x', sizeof small);
    int rc = 0;
    if (dsd_user_config_radio_input_spec(cfg.get(), opts.get(), small, sizeof small) != -1 || small[0] != '\0') {
        DSD_FPRINTF(stderr, "FAIL: a 127-byte serial in 64 bytes gave '%.63s'\n", small);
        rc |= 1;
    }
    char whole[512];
    if (dsd_user_config_radio_input_spec(cfg.get(), opts.get(), whole, sizeof whole) != 0
        || strncmp(whole, "rtl:serial=", 11) != 0 || strlen(whole) < 11U + DSD_RTL_SERIAL_MAX) {
        DSD_FPRINTF(stderr, "FAIL: the whole spec: '%s'\n", whole);
        rc |= 1;
    }
    return rc;
}

/* Issue #743 (B1): --validate-config refuses a file_path that is an I/Q replay spec, which no config can start; a file
   input named like a network input (the "tcp" an older save wrote) still opens it, and validates. */
static int
test_validate_refuses_a_replay_file_path(void) {
    static const struct {
        const char* ini;
        const char* section;
        const char* key;
        int errors;
    } cases[] = {
        {"[input]\nsource = \"file\"\nfile_path = \"iqreplay:/captures/site.iq.json\"\n", "input", "file_path", 1},
        {"[profile.replay]\ninput.source = \"file\"\ninput.file_path = \"iqreplay:/captures/site.iq.json\"\n",
         "profile.replay", "input.file_path", 1},
        {"[input]\nsource = \"file\"\nfile_path = \"tcp\"\n", "", "", 0},
        {"[input]\nsource = \"file\"\nfile_path = \"/captures/iqreplay.wav\"\n", "", "", 0},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(cases[i].ini, path, sizeof path) != 0) {
            return 1;
        }
        dsdcfg_diagnostics_t diags;
        DSD_MEMSET(&diags, 0, sizeof diags);
        const int validate_rc = dsd_user_config_validate(path, &diags);
        (void)remove(path);
        int named = cases[i].errors == 0;
        for (int d = 0; d < diags.count && !named; d++) {
            named = diags.items[d].level == DSDCFG_DIAG_ERROR && strcmp(diags.items[d].section, cases[i].section) == 0
                    && strcmp(diags.items[d].key, cases[i].key) == 0
                    && strstr(diags.items[d].message, "--iq-replay <path>") != NULL;
        }
        if ((validate_rc != 0) != (cases[i].errors != 0) || diags.error_count != cases[i].errors || !named) {
            DSD_FPRINTF(stderr, "FAIL: validate case %zu: rc=%d errors=%d (want %d), named=%d\n", i, validate_rc,
                        diags.error_count, cases[i].errors, named);
            rc |= 1;
        }
        dsdcfg_diags_free(&diags);
    }
    return rc;
}

/* Issue #743: an [input] source = "iqtcp" | "iqudp" loads its iq_* keys, applies the canonical spec they build, its
   rtl_bw_khz as the DSP bandwidth request, the squelch and the monitor volume, and none of the tuner keys of the inputs
   DSD-neo tunes (gain, PPM, auto-PPM, rtl_freq). A save of the session writes the stream back as its own source, with
   no tuner key, which loads to the same input; keys that build no spec leave the input as it was. */
static int
test_iq_stream_input_load_apply_snapshot(void) {
    static const char* ini = "[input]\n"
                             "source = \"iqudp\"\n"
                             "iq_host = \"0.0.0.0\"\n"
                             "iq_port = 7356\n"
                             "iq_rate = \"1.152M\"\n"
                             "iq_format = \"CF32\"\n"
                             "iq_freq = \"851.0125M\"\n"
                             "rtl_bw_khz = 24\n"
                             "rtl_sql = -50\n"
                             "rtl_volume = 3\n"
                             "rtl_gain = 30\n"
                             "rtl_ppm = 5\n"
                             "auto_ppm = true\n"
                             "rtl_freq = \"460.5M\"\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    static dsdneoUserConfig cfg;
    const int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    int rc = 0;
    if (load_rc != 0 || !cfg.has_input || cfg.input_source != DSDCFG_INPUT_IQUDP || strcmp(cfg.iq_host, "0.0.0.0") != 0
        || cfg.iq_port != 7356 || strcmp(cfg.iq_rate, "1.152M") != 0 || strcmp(cfg.iq_format, "CF32") != 0
        || strcmp(cfg.iq_freq, "851.0125M") != 0) {
        DSD_FPRINTF(stderr, "FAIL: iqudp load: rc=%d source %d host \"%s\" port %d rate \"%s\"\n", load_rc,
                    cfg.input_source, cfg.iq_host, cfg.iq_port, cfg.iq_rate);
        rc |= 1;
    }
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.rtl_gain_value = 7;
    opts.rtlsdr_ppm_error = 1;
    opts.rtlsdr_center_freq = 850000000U;
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (strcmp(opts.audio_in_dev, "iqudp:0.0.0.0:7356:1152000:cf32:851012500") != 0 || opts.rtl_dsp_bw_khz != 24
        || opts.rtl_dsp_bw_source != DSD_RTL_DSP_BW_SOURCE_EXPLICIT || opts.rtl_volume_multiplier != 3
        || opts.rtl_squelch_mode != DSD_SQUELCH_MODE_LEVEL) {
        DSD_FPRINTF(stderr, "FAIL: iqudp apply: dev \"%s\" bw %d (%d) vol %d\n", opts.audio_in_dev, opts.rtl_dsp_bw_khz,
                    opts.rtl_dsp_bw_source, opts.rtl_volume_multiplier);
        rc |= 1;
    }
    if (opts.rtl_gain_value != 7 || opts.rtlsdr_ppm_error != 1 || opts.rtl_auto_ppm != 0
        || opts.rtlsdr_center_freq != 850000000U) {
        DSD_FPRINTF(stderr, "FAIL: iqudp apply set a tuner key: gain %d ppm %d auto %d centre %u\n",
                    opts.rtl_gain_value, opts.rtlsdr_ppm_error, opts.rtl_auto_ppm, opts.rtlsdr_center_freq);
        rc |= 1;
    }
    char spec[256];
    if (dsd_user_config_radio_input_spec(&cfg, &opts, spec, sizeof spec) != 0
        || strcmp(spec, "iqudp:0.0.0.0:7356:1152000:cf32:851012500") != 0
        || dsd_user_config_input_sets_device(&cfg) != 1) {
        DSD_FPRINTF(stderr, "FAIL: iqudp radio input spec \"%s\"\n", spec);
        rc |= 1;
    }

    /* The save: the stream as its own source, canonical text, no tuner key; and it loads to the same input. */
    opts.audio_in_type = AUDIO_IN_RTL;
    static dsdneoUserConfig saved;
    dsd_snapshot_opts_to_user_config(&opts, &state, &saved);
    static char text[16384];
    rc |= render_config_to_buffer(&saved, text, sizeof text);
    rc |= expect_contains("stream save source", text, "source = \"iqudp\"\n");
    rc |= expect_contains("stream save host", text, "iq_host = \"0.0.0.0\"\n");
    rc |= expect_contains("stream save port", text, "iq_port = 7356\n");
    rc |= expect_contains("stream save rate", text, "iq_rate = \"1152000\"\n");
    rc |= expect_contains("stream save format", text, "iq_format = \"cf32\"\n");
    rc |= expect_contains("stream save freq", text, "iq_freq = \"851012500\"\n");
    rc |= expect_contains("stream save bw", text, "rtl_bw_khz = 24\n");
    rc |= expect_contains("stream save volume", text, "rtl_volume = 3\n");
    rc |= expect_contains("stream save squelch", text, "rtl_sql = -50\n");
    for (const char* tuner : {"rtl_gain", "rtl_ppm", "auto_ppm", "rtl_freq", "rtl_device", "file_path"}) {
        rc |= expect_absent_quiet("stream save tuner key", text, tuner);
    }
    if (write_temp_config(text, path, sizeof path) != 0) {
        return 1;
    }
    static dsdneoUserConfig reloaded;
    const int reload_rc = dsd_user_config_load(path, &reloaded);
    (void)remove(path);
    if (reload_rc != 0 || dsd_user_config_radio_input_spec(&reloaded, &opts, spec, sizeof spec) != 0
        || strcmp(spec, opts.audio_in_dev) != 0) {
        DSD_FPRINTF(stderr, "FAIL: stream save reloads as \"%s\"\n", spec);
        rc |= 1;
    }

    /* Keys that build no spec leave the input as it was. */
    cfg.iq_port = 0;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (strcmp(opts.audio_in_dev, "pulse") != 0 || dsd_user_config_input_sets_device(&cfg) != 0) {
        DSD_FPRINTF(stderr, "FAIL: a stream with no port replaced the input: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }
    return rc;
}

/* Issue #743: --validate-config judges a raw I/Q stream [input] by the spec its keys build, an error on the key whose
   field the parser refuses, and on iq_rate for a rate no plan takes to the DSP target the config runs (TETRA's under
   decode = tetra); the tuner keys of the inputs DSD-neo tunes are warnings; a file_path naming a stream is an error. */
static int
test_validate_iq_stream_input(void) {
    static const char base[] = "[input]\nsource = \"iqtcp\"\niq_host = \"127.0.0.1\"\niq_port = 1234\n"
                               "iq_rate = \"240k\"\niq_format = \"cs16\"\n";

    static const struct {
        const char* extra; /* after base's keys, in [input] unless it opens a section */
        const char* base;  /* NULL: base */
        int level;         /* DSDCFG_DIAG_ERROR or _WARNING; -1 for no diagnostic */
        const char* section;
        const char* key;
        const char* text;
    } cases[] = {
        {"", NULL, -1, "", "", ""},
        {"", "[input]\nsource = \"iqtcp\"\niq_port = 1234\niq_rate = \"240k\"\niq_format = \"cs16\"\n",
         DSDCFG_DIAG_ERROR, "input", "iq_host", "HOST is empty"},
        {"", "[input]\nsource = \"iqudp\"\niq_host = \"0.0.0.0\"\niq_rate = \"240k\"\niq_format = \"cs16\"\n",
         DSDCFG_DIAG_ERROR, "input", "iq_port", "PORT"},
        {"",
         "[input]\nsource = \"iqudp\"\niq_host = \"0.0.0.0\"\niq_port = 1\niq_rate = \"7k\"\niq_format = \"cs16\"\n",
         DSDCFG_DIAG_ERROR, "input", "iq_rate", "RATE '7k' is outside"},
        {"", "[input]\nsource = \"iqudp\"\niq_host = \"0.0.0.0\"\niq_port = 1\niq_rate = \"8k\"\niq_format = \"s16\"\n",
         DSDCFG_DIAG_ERROR, "input", "iq_format", "Invalid value (allowed: cu8|cs8|cs16|cs32|cf32)"},
        {"iq_freq = \"5G\"\n", NULL, DSDCFG_DIAG_ERROR, "input", "iq_freq", "FREQ '5G' is above"},
        {"iq_rate = \"49152000\"\nrtl_bw_khz = 24\n", NULL, DSDCFG_DIAG_ERROR, "input", "iq_rate",
         "the stream's rate (49152000 Hz) is above 3200000 Hz; send one channel (the sending program's VFO mode)"},
        {"iq_rate = \"3200001\"\n", NULL, DSDCFG_DIAG_ERROR, "input", "iq_rate",
         "the stream's rate (3200001 Hz) is above 3200000 Hz"},
        {"iq_rate = \"3.2M\"\n[mode]\ndecode = \"tetra\"\n", NULL, -1, "", "", ""},
        {"iq_rate = \"8000\"\n[mode]\ndecode = \"tetra\"\n", NULL, -1, "", "", ""},
        {"rtl_bw_khz = 16\n[mode]\ndecode = \"analog\"\n[analog]\nnfm_bandwidth_hz = 25000\n", NULL, DSDCFG_DIAG_ERROR,
         "analog", "nfm_bandwidth_hz", "does not fit the 16 kHz DSP rate"},
        {"rtl_gain = 30\n", NULL, DSDCFG_DIAG_WARNING, "input", "rtl_gain", "rtl_gain is not used by an I/Q stream"},
        {"rtl_ppm = 3\n", NULL, DSDCFG_DIAG_WARNING, "input", "rtl_ppm", "rtl_ppm is not used by an I/Q stream"},
        {"auto_ppm = true\n", NULL, DSDCFG_DIAG_WARNING, "input", "auto_ppm", "auto_ppm is not used"},
        {"rtl_freq = \"851M\"\n", NULL, DSDCFG_DIAG_WARNING, "input", "rtl_freq",
         "give its receiver frequency as iq_freq"},
        {"[trunking]\nenabled = true\n", NULL, DSDCFG_DIAG_ERROR, "trunking", "enabled",
         "trunking.enabled cannot be used with an I/Q stream input"},
        {"[trunking]\nscanner = true\n", NULL, DSDCFG_DIAG_ERROR, "trunking", "scanner",
         "trunking.scanner cannot be used with an I/Q stream input"},
        {"[trunk_scan]\nenabled = true\ntargets_csv = \"t.csv\"\n", NULL, DSDCFG_DIAG_ERROR, "trunk_scan", "enabled",
         "trunk_scan.enabled cannot be used with an I/Q stream input"},
        {"[profile.net]\ninput.rtl_gain = 30\ntrunking.enabled = true\n", NULL, DSDCFG_DIAG_ERROR, "profile.net",
         "trunking.enabled", "cannot be used with an I/Q stream input"},
        {"[profile.net]\ninput.rtl_gain = 30\n", NULL, DSDCFG_DIAG_WARNING, "profile.net", "input.rtl_gain",
         "rtl_gain is not used"},
        {"", "[input]\nsource = \"file\"\nfile_path = \"iqtcp:127.0.0.1:1234:240k:cs16\"\n", DSDCFG_DIAG_ERROR, "input",
         "file_path", "use source = \"iqtcp\""},
        {"", "[input]\nsource = \"file\"\nfile_path = \"iqudp:0.0.0.0:1234:240k:cs16\"\n", DSDCFG_DIAG_ERROR, "input",
         "file_path", "use source = \"iqudp\""},
        {"", "[input]\nsource = \"rtl\"\nrtl_freq = \"851M\"\n[trunking]\nenabled = true\n", -1, "", "", ""},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char ini[1024];
        DSD_SNPRINTF(ini, sizeof ini, "%s%s", cases[i].base ? cases[i].base : base, cases[i].extra);
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(ini, path, sizeof path) != 0) {
            return 1;
        }
        dsdcfg_diagnostics_t diags;
        DSD_MEMSET(&diags, 0, sizeof diags);
        const int validate_rc = dsd_user_config_validate(path, &diags);
        (void)remove(path);
        int found = cases[i].level < 0 ? (diags.count == 0) : 0;
        for (int d = 0; d < diags.count && !found; d++) {
            found = (int)diags.items[d].level == cases[i].level && strcmp(diags.items[d].section, cases[i].section) == 0
                    && strcmp(diags.items[d].key, cases[i].key) == 0
                    && strstr(diags.items[d].message, cases[i].text) != NULL;
        }
        const int want_rc = cases[i].level == DSDCFG_DIAG_ERROR ? -1 : 0;
        if (!found || validate_rc != want_rc) {
            DSD_FPRINTF(stderr, "FAIL: stream validate case %zu: rc=%d, %d diagnostic(s)%s%s\n", i, validate_rc,
                        diags.count, diags.count ? ", first: " : "", diags.count ? diags.items[0].message : "");
            rc |= 1;
        }
        dsdcfg_diags_free(&diags);
    }
    return rc;
}

/* Issue #743 (D8): the next-start rules hold a raw I/Q stream input to trunking, the -Y scanner and a trunk scan being
   off; a save that newly breaks one is refused, one the session's own save broke already is not, and a save that
   leaves the stream while trunking turns on starts. */
static int
test_check_iq_stream_startable(void) {
    static dsdneoUserConfig before;
    static dsdneoUserConfig after;
    char why[256];
    int rc = 0;
    DSD_MEMSET(&before, 0, sizeof before);
    before.has_input = 1;
    before.input_source = DSDCFG_INPUT_IQTCP;
    before.has_trunking = 1;
    after = before;
    after.trunk_enabled = 1;
    if (dsd_user_config_check_trunk_scan_startable(&after, &before, why, sizeof why) == 0
        || strncmp(why, "trunking.enabled cannot be used with an I/Q stream input", 56) != 0) {
        DSD_FPRINTF(stderr, "FAIL: stream startable, trunking: \"%s\"\n", why);
        rc |= 1;
    }
    after = before;
    after.trunk_scanner = 1;
    if (dsd_user_config_check_trunk_scan_startable(&after, &before, why, sizeof why) == 0
        || strncmp(why, "trunking.scanner cannot be used with an I/Q stream input", 56) != 0) {
        DSD_FPRINTF(stderr, "FAIL: stream startable, scanner: \"%s\"\n", why);
        rc |= 1;
    }
    after = before;
    after.trunk_scan_enabled = 1;
    after.trunk_scan_list_count = 1;
    DSD_SNPRINTF(after.trunk_scan_lists[0].path, sizeof after.trunk_scan_lists[0].path, "%s", "t.csv");
    if (dsd_user_config_check_trunk_scan_startable(&after, &before, why, sizeof why) == 0
        || strncmp(why, "trunk_scan.enabled cannot be used with an I/Q stream input", 58) != 0) {
        DSD_FPRINTF(stderr, "FAIL: stream startable, trunk scan: \"%s\"\n", why);
        rc |= 1;
    }
    /* Broken already: the session's. */
    after = before;
    after.trunk_enabled = 1;
    if (dsd_user_config_check_trunk_scan_startable(&after, &after, why, sizeof why) != 0) {
        DSD_FPRINTF(stderr, "FAIL: a rule the save broke already refused: \"%s\"\n", why);
        rc |= 1;
    }
    /* Leaving the stream while trunking turns on. */
    after.input_source = DSDCFG_INPUT_RTL;
    if (dsd_user_config_check_trunk_scan_startable(&after, &before, why, sizeof why) != 0) {
        DSD_FPRINTF(stderr, "FAIL: an RTL input with trunking refused: \"%s\"\n", why);
        rc |= 1;
    }
    return rc;
}

/* Issue #671: under --trunk-scan the parked target's type sets the live trunking flag, so a save writes the configured
   setting the coordinator publishes; with no target published (no scan, or before the first target), the live flag. */
static int
test_snapshot_trunking_reads_the_trunk_scan_configured_value(void) {
    static const struct {
        int scan_enabled;
        uint16_t targets;
        int live;
        uint8_t configured;
        int want;
    } cases[] = {
        {1, 2, 1, 0, 0},
        {1, 2, 0, 1, 1},
        {1, 0, 1, 0, 1},
        {0, 2, 1, 0, 1},
    };

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_opts_and_state(opts, state);
        opts.trunk_scan_enabled = cases[i].scan_enabled;
        opts.trunk_enable = cases[i].live;
        state.trunk_scan_target_count = cases[i].targets;
        state.trunk_scan_configured_trunk_enable = cases[i].configured;
        dsdneoUserConfig snap;
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        if (snap.trunk_enabled != cases[i].want) {
            DSD_FPRINTF(stderr, "trunking save case %zu: wrote %d, want %d\n", i, snap.trunk_enabled, cases[i].want);
            rc |= 1;
        }
    }
    return rc;
}

/* Issue #671 (D7): a -Y scan the operator paused is still the session's scan, so a save writes scanner = true and the
   next launch runs it; a resume still waiting on its row is paused too. A running scan saves true, a stopped one
   false. A paused trunk scan saves as configured: trunk scan on, the configured trunking. */
static int
test_snapshot_scanner_saves_a_paused_scan(void) {
    static const struct {
        const char* what;
        int scanner_mode;
        uint8_t paused;
        uint8_t resume_pending;
        int want;
    } cases[] = {
        {"running", 1, 0U, 0U, 1},
        {"paused", 0, 1U, 0U, 1},
        {"resume pending", 0, 1U, 1U, 1},
        {"stopped", 0, 0U, 0U, 0},
    };

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_opts_and_state(opts, state);
        opts.scanner_mode = cases[i].scanner_mode;
        state.lcn_scan_paused = cases[i].paused;
        state.lcn_scan_resume_pending = cases[i].resume_pending;
        state.lcn_freq_count = 3;
        dsdneoUserConfig snap;
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        if (snap.trunk_scanner != cases[i].want) {
            DSD_FPRINTF(stderr, "scanner save, %s -Y: wrote %d, want %d\n", cases[i].what, snap.trunk_scanner,
                        cases[i].want);
            rc |= 1;
        }
    }

    reset_opts_and_state(opts, state);
    opts.trunk_scan_enabled = 1;
    opts.trunk_enable = 0;
    state.trunk_scan_paused = 1U;
    state.trunk_scan_target_count = 2U;
    state.trunk_scan_configured_trunk_enable = 1U;
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.trunk_scan_enabled == 0 || snap.trunk_enabled != 1 || snap.trunk_scanner != 0) {
        DSD_FPRINTF(stderr, "paused trunk scan save: scan=%d trunking=%d scanner=%d, want 1/1/0\n",
                    snap.trunk_scan_enabled, snap.trunk_enabled, snap.trunk_scanner);
        rc |= 1;
    }
    return rc;
}

/* Issue #644: a -Y scan held on a row, an entry hold or not, saves exactly as the same scan unheld: the hold and
   its entry mark are the session's, never the configuration's. */
static int
test_snapshot_never_saves_a_scan_hold(void) {
    static dsd_opts opts;
    static dsd_state state;
    static char unheld[16384];
    static char held[16384];
    reset_opts_and_state(opts, state);
    opts.scanner_mode = 1;
    state.lcn_freq_count = 3;
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, unheld, sizeof unheld) != 0) {
        return 1;
    }
    state.lcn_scan_hold = 1U;
    state.lcn_scan_entry_hold = 1U;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, held, sizeof held) != 0) {
        return 1;
    }
    if (strcmp(unheld, held) != 0) {
        DSD_FPRINTF(stderr, "FAIL: an entry-held -Y scan saves differently from an unheld one\n");
        return 1;
    }
    /* The trunk scan's hold and entry mark neither: the config save, the exit autosave and the shutdown a hangup runs
       all write this snapshot. */
    reset_opts_and_state(opts, state);
    opts.trunk_scan_enabled = 1;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, unheld, sizeof unheld) != 0) {
        return 1;
    }
    state.trunk_scan_hold = 1U;
    state.trunk_scan_entry_hold = 1U;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, held, sizeof held) != 0) {
        return 1;
    }
    if (strcmp(unheld, held) != 0) {
        DSD_FPRINTF(stderr, "FAIL: an entry-held trunk scan saves differently from an unheld one\n");
        return 1;
    }
    return 0;
}

/* Issue #671 (N1): what a config apply left for the next start is what a save writes, field by field; with nothing
   waiting the running values are. */
static int
test_snapshot_writes_the_next_start(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.frontend_kind = DSD_FRONTEND_NONE;
    DSD_SNPRINTF(opts.chan_in_file, sizeof opts.chan_in_file, "%s", "running.csv");
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    int rc = 0;
    if (snap.frontend_kind != DSD_FRONTEND_NONE || strcmp(snap.trunk_chan_csv, "running.csv") != 0) {
        DSD_FPRINTF(stderr, "next start: nothing waiting, but the save wrote frontend %d map %s\n",
                    (int)snap.frontend_kind, snap.trunk_chan_csv);
        rc |= 1;
    }
    dsd_opts_next_start* next = &opts.next_start;
    next->pending = DSD_NEXT_START_FRONTEND | DSD_NEXT_START_TRUNK_SCAN | DSD_NEXT_START_TRUNK_SCAN_TARGETS
                    | DSD_NEXT_START_TRUNK_SCAN_IDLE_DWELL | DSD_NEXT_START_TRUNK_SCAN_ACTIVITY_HOLD
                    | DSD_NEXT_START_CHANNEL_MAP | DSD_NEXT_START_BAND_PLAN;
    next->frontend_kind = DSD_FRONTEND_TERMINAL;
    next->trunk_scan_enabled = 1;
    next->trunk_scan_idle_dwell_ms = 4321;
    next->trunk_scan_activity_hold_ms = 2500;
    DSD_SNPRINTF(next->trunk_scan_lists[0].path, sizeof next->trunk_scan_lists[0].path, "%s", "next-targets.csv");
    next->trunk_scan_list_count = 1;
    DSD_SNPRINTF(next->chan_in_file, sizeof next->chan_in_file, "%s", "next.csv");
    DSD_SNPRINTF(next->p25_bandplan_in_file, sizeof next->p25_bandplan_in_file, "%s", "next-plan.csv");
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.frontend_kind != DSD_FRONTEND_TERMINAL || snap.frontend_kind_is_set == 0 || snap.trunk_scan_enabled == 0
        || snap.trunk_scan_idle_dwell_ms != 4321 || snap.trunk_scan_activity_hold_ms != 2500
        || strcmp(snap.trunk_scan_lists[0].path, "next-targets.csv") != 0
        || strcmp(snap.trunk_chan_csv, "next.csv") != 0 || strcmp(snap.trunk_p25_bandplan_csv, "next-plan.csv") != 0) {
        DSD_FPRINTF(stderr, "next start: the save did not write every waiting setting\n");
        rc |= 1;
    }
    return rc;
}

/* Issue #671 (B1, round 4): the trunk-scan rules startup refuses to start without, on a composed config, with the
   reason; with a config to compare against, only the rules newly broken. */
static int
test_check_trunk_scan_startable(void) {
    static dsdneoUserConfig cfg;
    static dsdneoUserConfig before;
    char why[256];
    int rc = 0;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    if (dsd_user_config_check_trunk_scan_startable(&cfg, nullptr, why, sizeof why) != 0 || why[0] != '\0') {
        DSD_FPRINTF(stderr, "startable: no trunk scan refused (%s)\n", why);
        rc |= 1;
    }
    cfg.trunk_scan_enabled = 1;
    if (dsd_user_config_check_trunk_scan_startable(&cfg, nullptr, why, sizeof why) == 0
        || strcmp(why, "targets_csv is required when trunk_scan.enabled is true") != 0) {
        DSD_FPRINTF(stderr, "startable: no list: %s\n", why);
        rc |= 1;
    }
    DSD_SNPRINTF(cfg.trunk_scan_lists[0].path, sizeof cfg.trunk_scan_lists[0].path, "%s", "targets.csv");
    cfg.trunk_scan_list_count = 1;
    if (dsd_user_config_check_trunk_scan_startable(&cfg, nullptr, why, sizeof why) != 0) {
        DSD_FPRINTF(stderr, "startable: a trunk scan with its list refused (%s)\n", why);
        rc |= 1;
    }
    DSD_SNPRINTF(cfg.trunk_p25_bandplan_csv, sizeof cfg.trunk_p25_bandplan_csv, "%s", "plan.csv");
    if (dsd_user_config_check_trunk_scan_startable(&cfg, nullptr, why, sizeof why) == 0
        || strncmp(why, "trunking.p25_bandplan_csv cannot be combined with trunk_scan.enabled", 68) != 0) {
        DSD_FPRINTF(stderr, "startable: a band plan: %s\n", why);
        rc |= 1;
    }
    cfg.trunk_p25_bandplan_csv[0] = '\0';
    DSD_SNPRINTF(cfg.trunk_chan_csv, sizeof cfg.trunk_chan_csv, "%s", "chan.csv");
    if (dsd_user_config_check_trunk_scan_startable(&cfg, nullptr, why, sizeof why) == 0
        || strncmp(why, "trunking.chan_csv cannot be combined with trunk_scan.enabled", 60) != 0) {
        DSD_FPRINTF(stderr, "startable: a channel map: %s\n", why);
        rc |= 1;
    }
    cfg.trunk_chan_csv[0] = '\0';
    cfg.trunk_scanner = 1;
    if (dsd_user_config_check_trunk_scan_startable(&cfg, nullptr, why, sizeof why) == 0
        || strncmp(why, "trunking.scanner cannot be combined with trunk_scan.enabled", 59) != 0) {
        DSD_FPRINTF(stderr, "startable: the scanner: %s\n", why);
        rc |= 1;
    }
    cfg.trunk_enabled = 1; /* trunking wins over the scanner */
    if (dsd_user_config_check_trunk_scan_startable(&cfg, nullptr, why, sizeof why) != 0) {
        DSD_FPRINTF(stderr, "startable: trunking and the scanner refused (%s)\n", why);
        rc |= 1;
    }

    /* Against a config that already breaks the map rule: that rule kept, or fixed, passes; another one refuses. */
    DSD_MEMSET(&before, 0, sizeof before);
    before.trunk_scan_enabled = 1;
    DSD_SNPRINTF(before.trunk_scan_lists[0].path, sizeof before.trunk_scan_lists[0].path, "%s", "targets.csv");
    before.trunk_scan_list_count = 1;
    DSD_SNPRINTF(before.trunk_chan_csv, sizeof before.trunk_chan_csv, "%s", "chan.csv");
    cfg = before;
    DSD_SNPRINTF(cfg.trunk_chan_csv, sizeof cfg.trunk_chan_csv, "%s", "other.csv");
    if (dsd_user_config_check_trunk_scan_startable(&cfg, &before, why, sizeof why) != 0 || why[0] != '\0') {
        DSD_FPRINTF(stderr, "startable: the broken rule kept refused (%s)\n", why);
        rc |= 1;
    }
    cfg.trunk_scan_enabled = 0;
    if (dsd_user_config_check_trunk_scan_startable(&cfg, &before, why, sizeof why) != 0) {
        DSD_FPRINTF(stderr, "startable: the broken rule fixed refused (%s)\n", why);
        rc |= 1;
    }
    cfg = before;
    DSD_SNPRINTF(cfg.trunk_p25_bandplan_csv, sizeof cfg.trunk_p25_bandplan_csv, "%s", "plan.csv");
    if (dsd_user_config_check_trunk_scan_startable(&cfg, &before, why, sizeof why) == 0
        || strncmp(why, "trunking.p25_bandplan_csv cannot be combined with trunk_scan.enabled", 68) != 0) {
        DSD_FPRINTF(stderr, "startable: another rule broken: %s\n", why);
        rc |= 1;
    }
    if (dsd_user_config_check_trunk_scan_startable(nullptr, nullptr, why, sizeof why) == 0) {
        rc |= 1;
    }
    return rc;
}

static int
test_snapshot_digital_resample_mode(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=SDDC");
    opts.digital_resample_mode = 2; /* off */

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    int rc = 0;
    if (strcmp(snap.digital_resample, "off") != 0) {
        DSD_FPRINTF(stderr, "snapshot digital_resample mismatch: \"%s\", want \"off\"\n", snap.digital_resample);
        rc |= 1;
    }

    /* A mode returned to auto must clear any previously snapshotted value so autosave
       does not freeze the old explicit setting. */
    opts.digital_resample_mode = 0;
    DSD_SNPRINTF(snap.digital_resample, sizeof snap.digital_resample, "%s", "off");
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.digital_resample[0] != '\0') {
        DSD_FPRINTF(stderr, "auto mode left stale digital_resample: \"%s\"\n", snap.digital_resample);
        rc |= 1;
    }
    return rc;
}

static int
test_snapshot_roundtrip_soapy_args(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    /*
     * Snapshot a configured Soapy source, render it to INI, reload it, and apply
     * it back to opts. This protects both shared RTL tuning fields and the
     * Soapy-only extended fields.
     */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=sdrplay,serial=RSP1A");
    opts.audio_in_dev[sizeof opts.audio_in_dev - 1] = '\0';
    opts.rtlsdr_center_freq = 935012500U;
    opts.rtl_gain_value = 44;
    opts.rtlsdr_ppm_error = -3;
    opts.rtl_dsp_bw_khz = 24;
    opts.rtl_squelch_level = 1.0;
    opts.rtl_volume_multiplier = 5;
    DSD_SNPRINTF(opts.soapy_profile, sizeof opts.soapy_profile, "%s", "sdrplay");
    DSD_SNPRINTF(opts.soapy_stream_format, sizeof opts.soapy_stream_format, "%s", "cs16");
    DSD_SNPRINTF(opts.soapy_antenna, sizeof opts.soapy_antenna, "%s", "A");
    DSD_SNPRINTF(opts.soapy_clock, sizeof opts.soapy_clock, "%s", "internal");
    DSD_SNPRINTF(opts.soapy_settings, sizeof opts.soapy_settings, "%s",
                 "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4");
    DSD_SNPRINTF(opts.soapy_gains, sizeof opts.soapy_gains, "%s", "IFGR:35");
    opts.soapy_bandwidth_hz = 200000;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    // Snapshot fields should preserve the Soapy args without the input prefix.
    int rc = 0;
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_SOAPY) {
        DSD_FPRINTF(stderr, "snapshot input_source mismatch for soapy\n");
        rc |= 1;
    }
    if (strcmp(snap.soapy_args, "driver=sdrplay,serial=RSP1A") != 0) {
        DSD_FPRINTF(stderr, "snapshot soapy_args mismatch: \"%s\"\n", snap.soapy_args);
        rc |= 1;
    }
    if (strcmp(snap.rtl_freq, "935012500") != 0 || snap.rtl_gain != 44 || snap.rtl_ppm != -3 || snap.rtl_bw_khz != 24
        || snap.rtl_volume != 5) {
        DSD_FPRINTF(stderr, "snapshot shared tuning mismatch freq=%s gain=%d ppm=%d bw=%d vol=%d\n", snap.rtl_freq,
                    snap.rtl_gain, snap.rtl_ppm, snap.rtl_bw_khz, snap.rtl_volume);
        rc |= 1;
    }
    /* A unit power is 0 dB, an active level: it saves as -1, never as 0 (off; issue #899 B15). */
    if (snap.rtl_sql != -1) {
        DSD_FPRINTF(stderr, "snapshot rtl_sql expected -1 from unit squelch power, got %d\n", snap.rtl_sql);
        rc |= 1;
    }
    if (strcmp(snap.soapy_profile, "sdrplay") != 0 || strcmp(snap.soapy_stream_format, "cs16") != 0
        || strcmp(snap.soapy_antenna, "A") != 0 || strcmp(snap.soapy_clock, "internal") != 0
        || strcmp(snap.soapy_settings,
                  "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4")
               != 0
        || strcmp(snap.soapy_gains, "IFGR:35") != 0 || snap.soapy_bandwidth_hz != 200000
        || !snap.soapy_bandwidth_hz_is_set) {
        DSD_FPRINTF(stderr, "snapshot soapy extended fields mismatch\n");
        rc |= 1;
    }

    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (!strstr(rendered, "source = \"soapy\"") || !strstr(rendered, "soapy_args = \"driver=sdrplay,serial=RSP1A\"")) {
        DSD_FPRINTF(stderr, "rendered Soapy config missing source/args:\n%s\n", rendered);
        rc |= 1;
    }
    if (!strstr(rendered, "soapy_profile = \"sdrplay\"") || !strstr(rendered, "soapy_stream_format = \"cs16\"")
        || !strstr(rendered,
                   "soapy_settings = \"rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,"
                   "rfgain_sel=4\"")
        || !strstr(rendered, "soapy_bandwidth_hz = 200000")) {
        DSD_FPRINTF(stderr, "rendered Soapy config missing extended fields:\n%s\n", rendered);
        rc |= 1;
    }

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg_reload;
    if (dsd_user_config_load(path, &cfg_reload) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for rendered soapy config %s\n", path);
        (void)remove(path);
        return 1;
    }

    if (cfg_reload.input_source != DSDCFG_INPUT_SOAPY
        || strcmp(cfg_reload.soapy_args, "driver=sdrplay,serial=RSP1A") != 0) {
        DSD_FPRINTF(stderr, "reloaded soapy config mismatch source=%d args=%s\n", cfg_reload.input_source,
                    cfg_reload.soapy_args);
        rc |= 1;
    }
    if (strcmp(cfg_reload.soapy_profile, "sdrplay") != 0 || strcmp(cfg_reload.soapy_stream_format, "cs16") != 0
        || strcmp(cfg_reload.soapy_antenna, "A") != 0 || strcmp(cfg_reload.soapy_clock, "internal") != 0
        || strcmp(cfg_reload.soapy_settings,
                  "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4")
               != 0
        || strcmp(cfg_reload.soapy_gains, "IFGR:35") != 0 || cfg_reload.soapy_bandwidth_hz != 200000
        || !cfg_reload.soapy_bandwidth_hz_is_set) {
        DSD_FPRINTF(stderr, "reloaded soapy extended fields mismatch\n");
        rc |= 1;
    }

    static dsd_opts opts_reload;
    static dsd_state state_reload;
    reset_opts_and_state(opts_reload, state_reload);
    dsd_apply_user_config_to_opts(&cfg_reload, &opts_reload, &state_reload);
    if (strcmp(opts_reload.audio_in_dev, "soapy:driver=sdrplay,serial=RSP1A") != 0) {
        DSD_FPRINTF(stderr, "reloaded audio_in_dev mismatch: \"%s\"\n", opts_reload.audio_in_dev);
        rc |= 1;
    }
    if (strcmp(opts_reload.soapy_profile, "sdrplay") != 0 || strcmp(opts_reload.soapy_stream_format, "cs16") != 0
        || strcmp(opts_reload.soapy_antenna, "A") != 0 || strcmp(opts_reload.soapy_clock, "internal") != 0
        || strcmp(opts_reload.soapy_settings,
                  "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4")
               != 0
        || strcmp(opts_reload.soapy_gains, "IFGR:35") != 0 || opts_reload.soapy_bandwidth_hz != 200000) {
        DSD_FPRINTF(stderr, "reloaded opts soapy extended fields mismatch\n");
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_snapshot_roundtrip_zero_rtl_ppm(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=airspy");
    opts.audio_in_dev[sizeof opts.audio_in_dev - 1] = '\0';
    opts.rtlsdr_center_freq = 155340000U;
    opts.rtl_gain_value = 22;
    opts.rtlsdr_ppm_error = 0;
    opts.rtl_dsp_bw_khz = 12;
    opts.rtl_squelch_level = 1.0;
    opts.rtl_volume_multiplier = 3;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    int rc = 0;
    if (!snap.rtl_ppm_is_set || snap.rtl_ppm != 0) {
        DSD_FPRINTF(stderr, "snapshot zero rtl_ppm should be explicit, got is_set=%d ppm=%d\n", snap.rtl_ppm_is_set,
                    snap.rtl_ppm);
        rc |= 1;
    }

    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (!strstr(rendered, "rtl_ppm = 0\n")) {
        DSD_FPRINTF(stderr, "rendered config should keep explicit zero rtl_ppm:\n%s\n", rendered);
        rc |= 1;
    }

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg_reload;
    if (dsd_user_config_load(path, &cfg_reload) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for rendered zero-ppm config %s\n", path);
        (void)remove(path);
        return 1;
    }

    if (!cfg_reload.rtl_ppm_is_set || cfg_reload.rtl_ppm != 0) {
        DSD_FPRINTF(stderr, "reloaded zero rtl_ppm should stay explicit, got is_set=%d ppm=%d\n",
                    cfg_reload.rtl_ppm_is_set, cfg_reload.rtl_ppm);
        rc |= 1;
    }

    static dsd_opts opts_reload;
    static dsd_state state_reload;
    reset_opts_and_state(opts_reload, state_reload);
    opts_reload.rtlsdr_ppm_error = 17;
    dsd_apply_user_config_to_opts(&cfg_reload, &opts_reload, &state_reload);
    if (opts_reload.rtlsdr_ppm_error != 0) {
        DSD_FPRINTF(stderr, "reloaded zero rtl_ppm should override existing ppm, got %d\n",
                    opts_reload.rtlsdr_ppm_error);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

/* Issue #518 follow-up: AGC is gain 0, which a save used to leave out and a load could not tell from an omitted key, so
 * a saved AGC session reloaded on whatever gain was running. 0 is now written and read back as a setting; an omitted
 * key still keeps the running gain. */
static int
test_snapshot_roundtrip_agc_rtl_gain(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=rtlsdr");
    opts.rtlsdr_center_freq = 155340000U;
    opts.rtl_gain_value = 0;
    opts.rtl_dsp_bw_khz = 12;
    opts.rtl_volume_multiplier = 2;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    int rc = 0;
    if (!snap.rtl_gain_is_set || snap.rtl_gain != 0) {
        DSD_FPRINTF(stderr, "snapshot AGC gain should be explicit, got is_set=%d gain=%d\n", snap.rtl_gain_is_set,
                    snap.rtl_gain);
        rc |= 1;
    }
    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("rendered AGC gain", rendered, "rtl_gain = 0\n");

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg_reload;
    if (dsd_user_config_load(path, &cfg_reload) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for rendered AGC config %s\n", path);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);
    if (!cfg_reload.rtl_gain_is_set || cfg_reload.rtl_gain != 0) {
        DSD_FPRINTF(stderr, "reloaded AGC gain should stay explicit, got is_set=%d gain=%d\n",
                    cfg_reload.rtl_gain_is_set, cfg_reload.rtl_gain);
        rc |= 1;
    }
    static dsd_opts opts_reload;
    static dsd_state state_reload;
    reset_opts_and_state(opts_reload, state_reload);
    opts_reload.rtl_gain_value = 30;
    dsd_apply_user_config_to_opts(&cfg_reload, &opts_reload, &state_reload);
    if (opts_reload.rtl_gain_value != 0) {
        DSD_FPRINTF(stderr, "reloaded AGC gain should replace the running gain, got %d\n", opts_reload.rtl_gain_value);
        rc |= 1;
    }

    /* An RTL-SDR [input] puts the gain in the spec it builds. */
    dsdneoUserConfig rtl_cfg = cfg_reload;
    rtl_cfg.input_source = DSDCFG_INPUT_RTL;
    rtl_cfg.rtl_device = 0;
    DSD_SNPRINTF(rtl_cfg.rtl_freq, sizeof rtl_cfg.rtl_freq, "%s", "155.34M");
    char spec[256];
    opts_reload.rtl_gain_value = 30;
    if (dsd_user_config_radio_input_spec(&rtl_cfg, &opts_reload, spec, sizeof spec) != 0
        || strncmp(spec, "rtl:0:155.34M:0:", strlen("rtl:0:155.34M:0:")) != 0) {
        DSD_FPRINTF(stderr, "AGC RTL spec should carry gain 0, got %s\n", spec);
        rc |= 1;
    }
    /* Omitted, the running gain stays. */
    rtl_cfg.rtl_gain_is_set = 0;
    rtl_cfg.rtl_gain = 0;
    if (dsd_user_config_radio_input_spec(&rtl_cfg, &opts_reload, spec, sizeof spec) != 0
        || strncmp(spec, "rtl:0:155.34M:30:", strlen("rtl:0:155.34M:30:")) != 0) {
        DSD_FPRINTF(stderr, "omitted gain should keep the running gain, got %s\n", spec);
        rc |= 1;
    }
    return rc;
}

static int
test_snapshot_rtl_and_rtltcp_device_specs(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsdneoUserConfig snap;
    int rc = 0;

    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:7:451.125M:19:-2:12:-55:6");
    opts.rtlsdr_center_freq = 451125000U;
    opts.rtl_gain_value = 21;
    opts.rtlsdr_ppm_error = -4;
    opts.rtl_dsp_bw_khz = 12;
    opts.rtl_squelch_level = 2.0;
    opts.rtl_volume_multiplier = 6;
    opts.rtl_auto_ppm = 1;

    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_RTL || snap.rtl_device != 7) {
        DSD_FPRINTF(stderr, "snapshot RTL source/device mismatch source=%d device=%d\n", (int)snap.input_source,
                    snap.rtl_device);
        rc |= 1;
    }
    if (strcmp(snap.rtl_freq, "451125000") != 0 || snap.rtl_gain != 21 || snap.rtl_ppm != -4 || snap.rtl_bw_khz != 12
        || snap.rtl_sql != -1 || snap.rtl_volume != 6 || !snap.rtl_ppm_is_set || snap.rtl_auto_ppm != 1) {
        DSD_FPRINTF(stderr, "snapshot RTL tuning mismatch freq=%s gain=%d ppm=%d bw=%d sql=%d vol=%d auto=%d\n",
                    snap.rtl_freq, snap.rtl_gain, snap.rtl_ppm, snap.rtl_bw_khz, snap.rtl_sql, snap.rtl_volume,
                    snap.rtl_auto_ppm);
        rc |= 1;
    }

    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:radio.local:1234:769.00625M:28:3:24:-47:5");
    opts.rtlsdr_center_freq = 769006250U;
    opts.rtl_gain_value = 28;
    opts.rtlsdr_ppm_error = 3;
    opts.rtl_dsp_bw_khz = 24;
    opts.rtl_squelch_level = 1e-30; /* under pwr_to_dB()'s floor: saved at -120 dB, the key's lowest */
    opts.rtl_volume_multiplier = 5;

    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_RTLTCP || strcmp(snap.rtltcp_host, "radio.local") != 0
        || snap.rtltcp_port != 1234) {
        DSD_FPRINTF(stderr, "snapshot RTLTCP source mismatch source=%d host=%s port=%d\n", (int)snap.input_source,
                    snap.rtltcp_host, snap.rtltcp_port);
        rc |= 1;
    }
    if (strcmp(snap.rtl_freq, "769006250") != 0 || snap.rtl_gain != 28 || snap.rtl_ppm != 3 || snap.rtl_bw_khz != 24
        || snap.rtl_sql != -120 || snap.rtl_volume != 5 || !snap.rtl_ppm_is_set) {
        DSD_FPRINTF(stderr, "snapshot RTLTCP tuning mismatch freq=%s gain=%d ppm=%d bw=%d sql=%d vol=%d\n",
                    snap.rtl_freq, snap.rtl_gain, snap.rtl_ppm, snap.rtl_bw_khz, snap.rtl_sql, snap.rtl_volume);
        rc |= 1;
    }

    return rc;
}

/*
 * A squelch that is off has to survive being saved and loaded again. Serialized
 * through pwr_to_dB() it came back as rtl_sql = -120, which the loader then
 * applied as a real threshold: the session that had no squelch acquired one, and
 * the startup banner reported a gate that had never been asked for. -120 is also
 * outside the schema's own -100..0 range for the key.
 */
/*
 * A squelch that is off has to survive being saved and loaded again. Serialized
 * through pwr_to_dB() it came back as rtl_sql = -120, which then reached the
 * decoder as a real threshold: the session that had no squelch acquired one, and
 * the startup banner reported a gate nobody asked for. -120 is also outside the
 * schema's own -100..0 range for the key.
 *
 * The two input families store the setting differently. RTL and RTL-TCP render a
 * device string that the engine parses, so the assertion there is on the `sql`
 * field of that string; SoapySDR applies the tuning to opts directly.
 */
static int
test_snapshot_disabled_squelch_roundtrips_as_off(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsdneoUserConfig snap;
    int rc = 0;

    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:22:-2:24:0:2");
    opts.rtlsdr_center_freq = 851375000U;
    opts.rtl_gain_value = 22;
    opts.rtl_dsp_bw_khz = 24;
    opts.rtl_squelch_level = 0.0;
    opts.rtl_volume_multiplier = 2;

    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.rtl_sql != 0) {
        DSD_FPRINTF(stderr, "snapshot of a disabled squelch expected rtl_sql 0, got %d\n", snap.rtl_sql);
        rc |= 1;
    }

    dsd_apply_user_config_to_opts(&snap, &opts, &state);
    if (strcmp(opts.audio_in_dev, "rtl:0:851375000:22:0:24:0:2") != 0) {
        DSD_FPRINTF(stderr, "reloaded disabled squelch expected sql field 0, got %s\n", opts.audio_in_dev);
        rc |= 1;
    }

    /* A real threshold still round-trips through decibels. */
    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:22:-2:24:-50:2");
    opts.rtlsdr_center_freq = 851375000U;
    opts.rtl_gain_value = 22;
    opts.rtl_dsp_bw_khz = 24;
    opts.rtl_squelch_level = pow(10.0, -5.0);
    opts.rtl_volume_multiplier = 2;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.rtl_sql != -50) {
        DSD_FPRINTF(stderr, "snapshot of a -50 dB threshold expected rtl_sql -50, got %d\n", snap.rtl_sql);
        rc |= 1;
    }

    /* SoapySDR applies the shared tuning straight to opts, so the stored
     * threshold is assertable there without going through the engine. */
    reset_opts_and_state(opts, state);
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_SOAPY;
    DSD_SNPRINTF(cfg.soapy_args, sizeof cfg.soapy_args, "%s", "driver=test");
    cfg.rtl_sql = 0;
    opts.rtl_squelch_level = 12.0; /* clobber, so the apply has to write it */
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.rtl_squelch_level != 0.0) {
        DSD_FPRINTF(stderr, "soapy apply of rtl_sql 0 expected 0.0, got %g\n", opts.rtl_squelch_level);
        rc |= 1;
    }

    cfg.rtl_sql = -50;
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (fabs(opts.rtl_squelch_level - pow(10.0, -5.0)) > 1e-12) {
        DSD_FPRINTF(stderr, "soapy apply of rtl_sql -50 expected 1e-5, got %g\n", opts.rtl_squelch_level);
        rc |= 1;
    }

    return rc;
}

/* Issue #899 B15: an export writes a level as the whole dB the rtl_sql key holds, the nearest one, and says so when
 * that rounds it. An active level never saves as 0 (off): the export truncated, so -0.25 dB and a linear 1 (0 dB)
 * saved as rtl_sql = 0 and reloaded with the squelch off. A whole dB saves as itself with no word, and off as 0. */
static int
test_snapshot_squelch_level_whole_db(void) {
    /* What sets each case's level, tagged rather than told by a NaN sentinel, which fast math folds away. */
    enum level_kind { LEVEL_DB, LEVEL_LINEAR_ONE, LEVEL_OFF };

    static const struct {
        double db;          /* LEVEL_DB: the level in dB */
        const char* warned; /* NULL: no warning */
        level_kind kind;
        int sql;
    } cases[] = {
        {-0.25, "-0.25 dB", LEVEL_DB, -1},      {0.0, "0.00 dB", LEVEL_LINEAR_ONE, -1},
        {-65.5, "-65.50 dB", LEVEL_DB, -66},    {-4.49, "-4.49 dB", LEVEL_DB, -4},
        {-60.0, NULL, LEVEL_DB, -60},           {0.0, NULL, LEVEL_OFF, 0},
        {-160.0, "-160.00 dB", LEVEL_DB, -120},
    };

    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    auto back_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto back_state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    dsd_opts& back = *back_storage;
    dsd_state& back_state = *back_state_storage;
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_opts_and_state(opts, state);
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:22:0:24:-50:2");
        opts.rtlsdr_center_freq = 851375000U;
        opts.rtl_gain_value = 22;
        opts.rtl_dsp_bw_khz = 24;
        opts.rtl_volume_multiplier = 2;
        if (cases[i].kind == LEVEL_LINEAR_ONE) {
            opts.rtl_squelch_level = 1.0;
        } else if (cases[i].kind == LEVEL_OFF) {
            opts.rtl_squelch_level = 0.0;
        } else {
            opts.rtl_squelch_level = dsd_squelch_level_from_sql(cases[i].db);
        }
        dsdneoUserConfig snap;
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        g_log_text[0] = '\0';
        char text[32768];
        if (render_config_to_buffer(&snap, text, sizeof text) != 0) {
            return 1;
        }
        char want[32];
        DSD_SNPRINTF(want, sizeof want, "rtl_sql = %d\n", cases[i].sql);
        rc |= expect_contains("whole-dB export", text, want);
        if (cases[i].warned) {
            rc |= expect_contains("rounded export warning", g_log_text, "rounded");
            rc |= expect_contains("rounded export names the level", g_log_text, cases[i].warned);
        } else if (strstr(g_log_text, "rounded")) {
            DSD_FPRINTF(stderr, "FAIL: case %zu warned about a level it did not round: %s\n", i, g_log_text);
            rc |= 1;
        }

        /* Reloaded, the squelch stays on (or off) and gates at the whole dB the file holds. */
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(text, path, sizeof path) != 0) {
            return 1;
        }
        dsdneoUserConfig reloaded;
        const int loaded = dsd_user_config_load(path, &reloaded);
        (void)remove(path);
        if (loaded != 0 || reloaded.rtl_sql != cases[i].sql) {
            DSD_FPRINTF(stderr, "FAIL: case %zu reloaded rtl_sql %d, want %d\n", i, reloaded.rtl_sql, cases[i].sql);
            rc |= 1;
            continue;
        }
        reset_opts_and_state(back, back_state);
        dsd_apply_user_config_to_opts(&reloaded, &back, &back_state);
        if (dsd_rtl_input_spec_apply(&back) != 1) {
            DSD_FPRINTF(stderr, "FAIL: case %zu reloaded spec %s refused\n", i, back.audio_in_dev);
            rc |= 1;
            continue;
        }
        const double want_level = cases[i].sql == 0 ? 0.0 : dsd_squelch_level_from_sql((double)cases[i].sql);
        if (fabs(back.rtl_squelch_level - want_level) > 1e-9 * want_level
            || (cases[i].sql != 0) != (back.rtl_squelch_level > 0.0)) {
            DSD_FPRINTF(stderr, "FAIL: case %zu reloaded level %g, want %g\n", i, back.rtl_squelch_level, want_level);
            rc |= 1;
        }
    }
    return rc;
}

/* [input] rtl_sql_mode / rtl_sql_margin_db (issue #518 follow-up): loaded, carried into an rtl: spec as auto+N (the
 * level stays in rtl_sql), applied straight to opts on SoapySDR, saved back from the session, and rendered. */
static int
test_auto_squelch_keys_roundtrip(void) {
    static const char* ini = "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_freq = \"162.475M\"\n"
                             "rtl_sql = -50\n"
                             "rtl_sql_mode = \"auto\"\n"
                             "rtl_sql_margin_db = 6\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int rc = 0;
    if (dsd_user_config_load(path, &cfg) != 0 || cfg.rtl_sql_mode != DSD_SQUELCH_MODE_AUTO || cfg.rtl_sql_margin_db != 6
        || cfg.rtl_sql != -50) {
        DSD_FPRINTF(stderr, "auto squelch keys not loaded (mode %d margin %d sql %d)\n", cfg.rtl_sql_mode,
                    cfg.rtl_sql_margin_db, cfg.rtl_sql);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (strstr(opts.audio_in_dev, ":auto+6:") == NULL) {
        DSD_FPRINTF(stderr, "rtl spec without the auto squelch: %s\n", opts.audio_in_dev);
        rc |= 1;
    }
    /* The spec's auto+6 carries no level: rtl_sql is the level beneath it, through the engine's reading of the spec and
       back out in a save. */
    (void)dsd_rtl_input_spec_apply(&opts);
    if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_AUTO || opts.rtl_squelch_margin_db != 6
        || fabs(opts.rtl_squelch_level - pow(10.0, -5.0)) > 1e-12) {
        DSD_FPRINTF(stderr, "rtl load of auto+6 over -50: mode %d margin %d level %g\n", opts.rtl_squelch_mode,
                    opts.rtl_squelch_margin_db, opts.rtl_squelch_level);
        rc |= 1;
    }
    {
        dsdneoUserConfig saved;
        dsd_snapshot_opts_to_user_config(&opts, &state, &saved);
        if (saved.rtl_sql != -50 || saved.rtl_sql_mode != DSD_SQUELCH_MODE_AUTO) {
            DSD_FPRINTF(stderr, "rtl load then save: sql %d mode %d\n", saved.rtl_sql, saved.rtl_sql_mode);
            rc |= 1;
        }
    }
    /* --squelch keeps the level it set. */
    reset_opts_and_state(opts, state);
    opts.rtl_squelch_cli_set = 1;
    opts.rtl_squelch_level = pow(10.0, -7.0);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (fabs(opts.rtl_squelch_level - pow(10.0, -7.0)) > 1e-15) {
        DSD_FPRINTF(stderr, "a config's rtl_sql replaced the --squelch level: %g\n", opts.rtl_squelch_level);
        rc |= 1;
    }

    /* SoapySDR: straight to opts. The level stays for a switch back. */
    reset_opts_and_state(opts, state);
    dsdneoUserConfig soapy = cfg;
    soapy.input_source = DSDCFG_INPUT_SOAPY;
    DSD_SNPRINTF(soapy.soapy_args, sizeof soapy.soapy_args, "%s", "driver=test");
    dsd_apply_user_config_to_opts(&soapy, &opts, &state);
    if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_AUTO || opts.rtl_squelch_margin_db != 6
        || fabs(opts.rtl_squelch_level - pow(10.0, -5.0)) > 1e-12) {
        DSD_FPRINTF(stderr, "soapy apply of auto+6: mode %d margin %d level %g\n", opts.rtl_squelch_mode,
                    opts.rtl_squelch_margin_db, opts.rtl_squelch_level);
        rc |= 1;
    }
    /* level, and an unset margin, read as the defaults. */
    soapy.rtl_sql_mode = DSD_SQUELCH_MODE_LEVEL;
    dsd_apply_user_config_to_opts(&soapy, &opts, &state);
    if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_LEVEL) {
        DSD_FPRINTF(stderr, "soapy apply of level kept auto\n");
        rc |= 1;
    }
    soapy.rtl_sql_mode = DSD_SQUELCH_MODE_AUTO;
    soapy.rtl_sql_margin_db = 0;
    dsd_apply_user_config_to_opts(&soapy, &opts, &state);
    if (opts.rtl_squelch_margin_db != DSD_SQUELCH_MARGIN_DEFAULT_DB) {
        DSD_FPRINTF(stderr, "an unset margin read %d\n", opts.rtl_squelch_margin_db);
        rc |= 1;
    }

    /* The save takes the session's setting and renders both keys; a level setting renders neither. */
    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:162.475M:22:-2:24:auto+6:2");
    opts.rtlsdr_center_freq = 162475000U;
    opts.rtl_squelch_level = pow(10.0, -5.0);
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts.rtl_squelch_margin_db = 6;
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char text[8192];
    if (snap.rtl_sql_mode != DSD_SQUELCH_MODE_AUTO || snap.rtl_sql_margin_db != 6 || snap.rtl_sql != -50
        || render_config_to_buffer(&snap, text, sizeof text) != 0) {
        DSD_FPRINTF(stderr, "snapshot of auto+6: mode %d margin %d sql %d\n", snap.rtl_sql_mode, snap.rtl_sql_margin_db,
                    snap.rtl_sql);
        return 1;
    }
    rc |= expect_contains("auto render", text, "rtl_sql_mode = \"auto\"");
    rc |= expect_contains("auto render", text, "rtl_sql_margin_db = 6");
    rc |= expect_contains("auto render", text, "rtl_sql = -50");
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, text, sizeof text) != 0 || strstr(text, "rtl_sql_mode") != NULL
        || strstr(text, "rtl_sql_margin_db") != NULL) {
        DSD_FPRINTF(stderr, "a level setting rendered the auto keys:\n%s\n", text);
        rc |= 1;
    }
    return rc;
}

/* rtl_sql_mode = noise (issue #518 follow-up): loaded, carried into an rtl: spec as noise+N over the level, applied
 * straight to opts on SoapySDR, saved back from the session, and rendered. */
static int
test_noise_squelch_keys_roundtrip(void) {
    static const char* ini = "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_freq = \"162.475M\"\n"
                             "rtl_sql = -50\n"
                             "rtl_sql_mode = \"noise\"\n"
                             "rtl_sql_margin_db = 14\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int rc = 0;
    if (dsd_user_config_load(path, &cfg) != 0 || cfg.rtl_sql_mode != DSD_SQUELCH_MODE_NOISE
        || cfg.rtl_sql_margin_db != 14) {
        DSD_FPRINTF(stderr, "noise squelch keys not loaded (mode %d margin %d)\n", cfg.rtl_sql_mode,
                    cfg.rtl_sql_margin_db);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (strstr(opts.audio_in_dev, ":noise+14:") == NULL) {
        DSD_FPRINTF(stderr, "rtl spec without the noise squelch: %s\n", opts.audio_in_dev);
        rc |= 1;
    }
    (void)dsd_rtl_input_spec_apply(&opts);
    if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_NOISE || opts.rtl_squelch_margin_db != 14
        || fabs(opts.rtl_squelch_level - pow(10.0, -5.0)) > 1e-12) {
        DSD_FPRINTF(stderr, "rtl load of noise+14 over -50: mode %d margin %d level %g\n", opts.rtl_squelch_mode,
                    opts.rtl_squelch_margin_db, opts.rtl_squelch_level);
        rc |= 1;
    }

    reset_opts_and_state(opts, state);
    dsdneoUserConfig soapy = cfg;
    soapy.input_source = DSDCFG_INPUT_SOAPY;
    DSD_SNPRINTF(soapy.soapy_args, sizeof soapy.soapy_args, "%s", "driver=test");
    dsd_apply_user_config_to_opts(&soapy, &opts, &state);
    if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_NOISE || opts.rtl_squelch_margin_db != 14) {
        DSD_FPRINTF(stderr, "soapy apply of noise+14: mode %d margin %d\n", opts.rtl_squelch_mode,
                    opts.rtl_squelch_margin_db);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char text[8192];
    if (snap.rtl_sql_mode != DSD_SQUELCH_MODE_NOISE || snap.rtl_sql_margin_db != 14
        || render_config_to_buffer(&snap, text, sizeof text) != 0) {
        DSD_FPRINTF(stderr, "snapshot of noise+14: mode %d margin %d\n", snap.rtl_sql_mode, snap.rtl_sql_margin_db);
        return 1;
    }
    rc |= expect_contains("noise render", text, "rtl_sql_mode = \"noise\"");
    rc |= expect_contains("noise render", text, "rtl_sql_margin_db = 14");
    return rc;
}

/* Issue #899: an [input] that names no source (an I/Q replay's retained input) applies its squelch
   keys, so it renders them too, with no source invented for them: a save and reload keeps rtl_sql = -50, and an auto
   squelch its mode and margin. Before the fix the render wrote only input_warn_db for that [input]. */
static int
test_sourceless_squelch_keys_roundtrip(void) {
    static const char* ini = "[input]\n"
                             "rtl_sql = -50\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    const int loaded = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (loaded != 0 || cfg.has_input == 0 || cfg.input_source != DSDCFG_INPUT_UNSET || cfg.rtl_sql_is_set == 0) {
        DSD_FPRINTF(stderr, "a source-less [input]'s rtl_sql did not load (rc %d source %d set %d)\n", loaded,
                    static_cast<int>(cfg.input_source), cfg.rtl_sql_is_set);
        return 1;
    }
    char text[8192];
    if (render_config_to_buffer(&cfg, text, sizeof text) != 0) {
        return 1;
    }
    int rc = expect_contains("source-less level render", text, "[input]\nrtl_sql = -50\n");
    const char* input = strstr(text, "[input]\n");
    const char* next = input != nullptr ? strstr(input + 1, "\n[") : nullptr;
    const char* source = input != nullptr ? strstr(input, "source = ") : nullptr;
    if (source != nullptr && (next == nullptr || source < next)) {
        DSD_FPRINTF(stderr, "FAIL: a source-less [input] rendered a source:\n%s\n", text);
        rc |= 1;
    }
    if (write_temp_config(text, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig back;
    const int reloaded = dsd_user_config_load(path, &back);
    (void)remove(path);
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    if (reloaded != 0 || back.rtl_sql_is_set == 0 || back.rtl_sql != -50) {
        DSD_FPRINTF(stderr, "the saved source-less squelch did not load back (rc %d set %d sql %d)\n", reloaded,
                    back.rtl_sql_is_set, back.rtl_sql);
        rc |= 1;
    } else {
        dsd_apply_user_config_to_opts(&back, &opts, &state);
        if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_LEVEL || fabs(opts.rtl_squelch_level - pow(10.0, -5.0)) > 1e-15) {
            DSD_FPRINTF(stderr, "the reloaded source-less squelch applied mode %d level %g\n", opts.rtl_squelch_mode,
                        opts.rtl_squelch_level);
            rc |= 1;
        }
    }
    dsd_state_ext_free_all(&state);

    cfg.rtl_sql_mode = DSD_SQUELCH_MODE_AUTO;
    cfg.rtl_sql_margin_db = 9;
    if (render_config_to_buffer(&cfg, text, sizeof text) != 0) {
        return 1;
    }
    rc |= expect_contains("source-less auto render", text, "rtl_sql_mode = \"auto\"\nrtl_sql_margin_db = 9\n");
    /* A source-less [input] without the keys renders none. */
    cfg.rtl_sql_is_set = 0;
    if (render_config_to_buffer(&cfg, text, sizeof text) != 0) {
        return 1;
    }
    if (strstr(text, "rtl_sql") != nullptr) {
        DSD_FPRINTF(stderr, "FAIL: a source-less [input] without squelch keys rendered some:\n%s\n", text);
        rc |= 1;
    }
    return rc;
}

/* The [input] squelch keys on audio input (issue #628): a PCM source's rtl_sql / rtl_sql_mode / rtl_sql_margin_db apply
 * to the session when they are present (the noise squelch runs there now, and the level always did), a PCM config
 * without them (one saved before audio input had a squelch) leaves the session's squelch alone (a runtime load must not
 * switch a live one off), --squelch wins, and a PCM session's save always writes the keys, off and the unset level
 * included, as an RTL session's does. */
static int
test_pcm_squelch_keys_roundtrip(void) {
    static const char* ini = "[input]\n"
                             "source = \"tcp\"\n"
                             "tcp_host = \"127.0.0.1\"\n"
                             "tcp_port = 7355\n"
                             "rtl_sql = -40\n"
                             "rtl_sql_mode = \"noise\"\n"
                             "rtl_sql_margin_db = 8\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int rc = 0;
    if (dsd_user_config_load(path, &cfg) != 0 || !cfg.rtl_sql_is_set || cfg.rtl_sql_mode != DSD_SQUELCH_MODE_NOISE) {
        DSD_FPRINTF(stderr, "tcp squelch keys not loaded (set %d mode %d)\n", cfg.rtl_sql_is_set, cfg.rtl_sql_mode);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_NOISE || opts.rtl_squelch_margin_db != 8
        || fabs(opts.rtl_squelch_level - pow(10.0, -4.0)) > 1e-12) {
        DSD_FPRINTF(stderr, "tcp apply of noise+8 over -40: mode %d margin %d level %g\n", opts.rtl_squelch_mode,
                    opts.rtl_squelch_margin_db, opts.rtl_squelch_level);
        rc |= 1;
    }

    /* A PCM config without the keys: the session's squelch stays. */
    dsdneoUserConfig bare = cfg;
    bare.rtl_sql_is_set = 0;
    bare.rtl_sql = 0;
    bare.rtl_sql_mode = DSD_SQUELCH_MODE_LEVEL;
    const dsd_squelch_setting noise12 = dsd_squelch_setting_noise(12);
    dsd_squelch_setting_store(&opts, &noise12);
    dsd_apply_user_config_to_opts(&bare, &opts, &state);
    if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_NOISE || opts.rtl_squelch_margin_db != 12) {
        DSD_FPRINTF(stderr, "a PCM config without squelch keys changed the squelch: mode %d margin %d\n",
                    opts.rtl_squelch_mode, opts.rtl_squelch_margin_db);
        rc |= 1;
    }

    /* --squelch wins. */
    reset_opts_and_state(opts, state);
    opts.rtl_squelch_cli_set = 1;
    opts.rtl_squelch_level = pow(10.0, -7.0);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_LEVEL || fabs(opts.rtl_squelch_level - pow(10.0, -7.0)) > 1e-15) {
        DSD_FPRINTF(stderr, "a PCM config replaced the --squelch setting: mode %d\n", opts.rtl_squelch_mode);
        rc |= 1;
    }

    /* The save: a tcp session under noise+8 writes the keys, and one with the squelch off writes it off. */
    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "tcp:127.0.0.1:7355");
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.rtl_squelch_level = pow(10.0, -4.0);
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    opts.rtl_squelch_margin_db = 8;
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char text[8192];
    if (!snap.rtl_sql_is_set || render_config_to_buffer(&snap, text, sizeof text) != 0) {
        DSD_FPRINTF(stderr, "snapshot of a tcp session's noise+8 not set\n");
        return 1;
    }
    rc |= expect_contains("tcp noise render", text, "rtl_sql_mode = \"noise\"");
    rc |= expect_contains("tcp noise render", text, "rtl_sql_margin_db = 8");
    rc |= expect_contains("tcp noise render", text, "rtl_sql = -40");
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts.rtl_squelch_level = 0.0;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, text, sizeof text) != 0) {
        return 1;
    }
    rc |= expect_contains("tcp off render", text, "rtl_sql = 0");
    /* Loaded back over a session running noise, that save switches the squelch off again. */
    if (write_temp_config(text, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig off;
    const int loaded = dsd_user_config_load(path, &off);
    (void)remove(path);
    const dsd_squelch_setting noise10 = dsd_squelch_setting_noise(10);
    dsd_squelch_setting_store(&opts, &noise10);
    if (loaded != 0 || !off.rtl_sql_is_set) {
        DSD_FPRINTF(stderr, "the saved off squelch did not load (rc %d set %d)\n", loaded, off.rtl_sql_is_set);
        rc |= 1;
    } else {
        dsd_apply_user_config_to_opts(&off, &opts, &state);
        if (opts.rtl_squelch_mode != DSD_SQUELCH_MODE_LEVEL || !dsd_squelch_is_off(opts.rtl_squelch_level)) {
            DSD_FPRINTF(stderr, "reloading a saved off squelch left mode %d level %g\n", opts.rtl_squelch_mode,
                        opts.rtl_squelch_level);
            rc |= 1;
        }
    }
    /* The unset level (-110 dB) saves as -110, inside the key's -120..0 range: a default Pulse session's save validates
       without a range warning, and loads back the same threshold, so a level gate decides as it did before. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts.rtl_squelch_level = pow(10.0, -11.0);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, text, sizeof text) != 0) {
        return 1;
    }
    rc |= expect_contains("pulse unset level render", text, "rtl_sql = -110");
    if (write_temp_config(text, path, sizeof path) != 0) {
        return 1;
    }
    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));
    const int validated = dsd_user_config_validate(path, &diags);
    (void)remove(path);
    for (int i = 0; i < diags.count; i++) {
        if (strstr(diags.items[i].key, "rtl_sql") != NULL) {
            DSD_FPRINTF(stderr, "a default Pulse save's %s does not validate: %s\n", diags.items[i].key,
                        diags.items[i].message);
            rc |= 1;
        }
    }
    if (validated != 0 || diags.error_count != 0) {
        DSD_FPRINTF(stderr, "a default Pulse save does not validate (rc %d, %d errors)\n", validated,
                    diags.error_count);
        rc |= 1;
    }
    dsdcfg_diags_free(&diags);
    if (write_temp_config(text, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig unset;
    const int unset_loaded = dsd_user_config_load(path, &unset);
    (void)remove(path);
    opts.rtl_squelch_level = pow(10.0, -6.0);
    if (unset_loaded != 0) {
        DSD_FPRINTF(stderr, "the saved unset level did not load (rc %d)\n", unset_loaded);
        rc |= 1;
    } else {
        dsd_apply_user_config_to_opts(&unset, &opts, &state);
        const double want = pow(10.0, -11.0);
        if (fabs(opts.rtl_squelch_level - want) > want * 1e-9) {
            DSD_FPRINTF(stderr, "the saved unset level reloaded as %g, not %g\n", opts.rtl_squelch_level, want);
            rc |= 1;
        }
    }
    /* A WAV session's level is saved too. */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "capture.wav");
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.rtl_squelch_level = pow(10.0, -6.0);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, text, sizeof text) != 0) {
        return 1;
    }
    rc |= expect_contains("wav level render", text, "rtl_sql = -60");
    return rc;
}

static int
test_load_and_apply_rtltcp_regression(void) {
    static const char* ini = "[input]\n"
                             "source = \"rtltcp\"\n"
                             "rtltcp_host = \"127.0.0.1\"\n"
                             "rtltcp_port = 1234\n"
                             "rtl_freq = \"851.375M\"\n"
                             "rtl_gain = 30\n"
                             "rtl_ppm = 5\n"
                             "rtl_bw_khz = 16\n"
                             "rtl_sql = -50\n"
                             "rtl_volume = 2\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_RTLTCP) {
        DSD_FPRINTF(stderr, "input section not parsed as RTLTCP source\n");
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (strcmp(opts.audio_in_dev, "rtltcp:127.0.0.1:1234:851.375M:30:5:16:-50:2") != 0) {
        DSD_FPRINTF(stderr, "rtltcp audio_in_dev regression: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_snapshot_roundtrip(void) {
    static const char* ini = "[input]\n"
                             "source = \"udp\"\n"
                             "udp_addr = \"127.0.0.1\"\n"
                             "udp_port = 9000\n"
                             "\n"
                             "[output]\n"
                             "backend = \"pulse\"\n"
                             "frontend = \"none\"\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"analog\"\n"
                             "\n"
                             "[trunking]\n"
                             "enabled = false\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    /* A live On: a config without terminal_mouse_wheel must leave it alone. */
    opts.frontend_terminal_display.mouse_wheel = 1;

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    opts.call_alert = 1;
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;
    int rc = 0;
    if (opts.frontend_terminal_display.mouse_wheel != 1) {
        DSD_FPRINTF(stderr, "an absent terminal_mouse_wheel must leave the live wheel setting alone\n");
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    if (!snap.terminal_mouse_wheel_is_set || snap.terminal_mouse_wheel != 1) {
        DSD_FPRINTF(stderr, "snapshot must carry the live wheel setting\n");
        rc |= 1;
    }
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_UDP) {
        DSD_FPRINTF(stderr, "snapshot input_source mismatch\n");
        rc |= 1;
    }
    if (strcmp(snap.udp_addr, "127.0.0.1") != 0 || snap.udp_port != 9000) {
        DSD_FPRINTF(stderr, "snapshot udp_addr/udp_port mismatch: %s:%d\n", snap.udp_addr, snap.udp_port);
        rc |= 1;
    }
    if (!snap.has_output || snap.output_backend != DSDCFG_OUTPUT_PULSE) {
        DSD_FPRINTF(stderr, "snapshot output_backend mismatch\n");
        rc |= 1;
    }
    if (!snap.has_mode || snap.decode_mode != DSDCFG_MODE_ANALOG) {
        DSD_FPRINTF(stderr, "snapshot decode_mode mismatch\n");
        rc |= 1;
    }
    if (!snap.has_trunking) {
        DSD_FPRINTF(stderr, "snapshot missing trunking section\n");
        rc |= 1;
    }
    if (snap.trunk_enabled != 0) {
        DSD_FPRINTF(stderr, "snapshot trunk_enabled should be false for this config\n");
        rc |= 1;
    }
    if (!snap.has_alerts || !snap.call_alert_enabled || snap.call_alert_events != DSD_CALL_ALERT_EVENT_VOICE_END) {
        DSD_FPRINTF(stderr, "snapshot alerts mismatch enabled=%d events=%d\n", snap.call_alert_enabled,
                    snap.call_alert_events);
        rc |= 1;
    }

    // Render the snapshot as Save config does; the live wheel setting must be written.
    static char saved[16384];
    if (render_config_to_buffer(&snap, saved, sizeof saved) != 0) {
        (void)remove(path);
        return 1;
    }
    rc |= expect_contains("save writes the wheel setting", saved, "terminal_mouse_wheel = true\n");

    (void)remove(path);
    return rc;
}

static int
test_apply_demod_lock(void) {
    static const char* ini = "[mode]\n"
                             "decode = \"auto\"\n"
                             "demod = \"qpsk\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    int rc = 0;
    if (!(opts.mod_cli_lock == 1 && opts.mod_qpsk == 1 && opts.mod_c4fm == 0 && opts.mod_gfsk == 0)) {
        DSD_FPRINTF(stderr, "demod lock not applied correctly (c4fm=%d qpsk=%d gfsk=%d lock=%d)\n", opts.mod_c4fm,
                    opts.mod_qpsk, opts.mod_gfsk, opts.mod_cli_lock);
        rc |= 1;
    }
    if (state.rf_mod != 1) {
        DSD_FPRINTF(stderr, "rf_mod should be 1 for QPSK lock, got %d\n", state.rf_mod);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_snapshot_persists_demod_lock(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "pulse");
    DSD_SNPRINTF(opts.audio_out_dev, sizeof opts.audio_out_dev, "null");
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    opts.mod_c4fm = 0;
    opts.mod_gfsk = 0;
    state.rf_mod = 1;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    int rc = 0;
    if (!snap.has_demod || snap.demod_path != DSDCFG_DEMOD_QPSK) {
        DSD_FPRINTF(stderr, "snapshot missing demod lock (has_demod=%d demod_path=%d)\n", snap.has_demod,
                    snap.demod_path);
        rc |= 1;
    }

    FILE* tmp = tmpfile();
    if (!tmp) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    dsd_user_config_render_ini(&snap, tmp);
    char buf[512];
    if (fseek(tmp, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek failed\n");
        fclose(tmp);
        return 1;
    }
    size_t n = fread(buf, 1, sizeof buf - 1, tmp);
    if (n == 0 && ferror(tmp)) {
        DSD_FPRINTF(stderr, "fread failed\n");
        fclose(tmp);
        return 1;
    }
    buf[n] = '\0';
    fclose(tmp);

    if (!strstr(buf, "demod = \"qpsk\"")) {
        DSD_FPRINTF(stderr, "rendered INI missing demod line:\n%s\n", buf);
        rc |= 1;
    }

    return rc;
}

static int
test_apply_logging_retargets_frame_log_file(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    FILE* first_handle = tmpfile();
    if (!first_handle) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.frame_log_f = first_handle;
    opts.frame_log_open_error_reported = 1;
    opts.frame_log_write_error_reported = 1;
    DSD_SNPRINTF(opts.frame_log_file, sizeof opts.frame_log_file, "%s", "/tmp/frames-old.log");
    opts.frame_log_file[sizeof opts.frame_log_file - 1] = '\0';
    FILE* first_p25_handle = tmpfile();
    if (!first_p25_handle) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.p25_sm_log_f = first_p25_handle;
    opts.p25_sm_log_open_error_reported = 1;
    opts.p25_sm_log_write_error_reported = 1;
    DSD_SNPRINTF(opts.p25_sm_log_file, sizeof opts.p25_sm_log_file, "%s", "/tmp/p25-sm-old.log");
    opts.p25_sm_log_file[sizeof opts.p25_sm_log_file - 1] = '\0';

    dsdneoUserConfig cfg = {};
    cfg.has_logging = 1;
    DSD_SNPRINTF(cfg.frame_log, sizeof cfg.frame_log, "%s", "/tmp/frames-new.log");
    cfg.frame_log[sizeof cfg.frame_log - 1] = '\0';
    DSD_SNPRINTF(cfg.p25_sm_log, sizeof cfg.p25_sm_log, "%s", "/tmp/p25-sm-new.log");
    cfg.p25_sm_log[sizeof cfg.p25_sm_log - 1] = '\0';

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    /*
     * Retargeting to new frame-log paths must close existing handles, replace the
     * remembered path strings, and clear sticky open/write error state.
     */
    int rc = 0;
    if (opts.frame_log_f != NULL) {
        DSD_FPRINTF(stderr, "frame log handle should be closed after retarget\n");
        rc |= 1;
    }
    if (strcmp(opts.frame_log_file, "/tmp/frames-new.log") != 0) {
        DSD_FPRINTF(stderr, "frame log path not updated after retarget: %s\n", opts.frame_log_file);
        rc |= 1;
    }
    if (opts.frame_log_open_error_reported != 0) {
        DSD_FPRINTF(stderr, "frame log open error state should reset after retarget\n");
        rc |= 1;
    }
    if (opts.frame_log_write_error_reported != 0) {
        DSD_FPRINTF(stderr, "frame log write error state should reset after retarget\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_f != NULL) {
        DSD_FPRINTF(stderr, "P25 SM log handle should be closed after retarget\n");
        rc |= 1;
    }
    if (strcmp(opts.p25_sm_log_file, "/tmp/p25-sm-new.log") != 0) {
        DSD_FPRINTF(stderr, "P25 SM log path not updated after retarget: %s\n", opts.p25_sm_log_file);
        rc |= 1;
    }
    if (opts.p25_sm_log_open_error_reported != 0) {
        DSD_FPRINTF(stderr, "P25 SM log open error state should reset after retarget\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_write_error_reported != 0) {
        DSD_FPRINTF(stderr, "P25 SM log write error state should reset after retarget\n");
        rc |= 1;
    }

    FILE* second_handle = tmpfile();
    if (!second_handle) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.frame_log_f = second_handle;
    opts.frame_log_open_error_reported = 1;
    opts.frame_log_write_error_reported = 1;
    FILE* second_p25_handle = tmpfile();
    if (!second_p25_handle) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.p25_sm_log_f = second_p25_handle;
    opts.p25_sm_log_open_error_reported = 1;
    opts.p25_sm_log_write_error_reported = 1;
    cfg.frame_log[0] = '\0';
    cfg.p25_sm_log[0] = '\0';

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    /*
     * Clearing configured paths is the disable case: both handles close, paths
     * become empty, and the same sticky error flags reset for future attempts.
     */
    if (opts.frame_log_f != NULL) {
        DSD_FPRINTF(stderr, "frame log handle should be closed when disabling logging path\n");
        rc |= 1;
    }
    if (opts.frame_log_file[0] != '\0') {
        DSD_FPRINTF(stderr, "frame log path should be cleared when disabling logging path\n");
        rc |= 1;
    }
    if (opts.frame_log_open_error_reported != 0) {
        DSD_FPRINTF(stderr, "frame log open error state should reset when disabling logging path\n");
        rc |= 1;
    }
    if (opts.frame_log_write_error_reported != 0) {
        DSD_FPRINTF(stderr, "frame log write error state should reset when disabling logging path\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_f != NULL) {
        DSD_FPRINTF(stderr, "P25 SM log handle should be closed when disabling logging path\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_file[0] != '\0') {
        DSD_FPRINTF(stderr, "P25 SM log path should be cleared when disabling logging path\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_open_error_reported != 0) {
        DSD_FPRINTF(stderr, "P25 SM log open error state should reset when disabling logging path\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_write_error_reported != 0) {
        DSD_FPRINTF(stderr, "P25 SM log write error state should reset when disabling logging path\n");
        rc |= 1;
    }

    return rc;
}

static int
test_apply_mode_ysf_uses_config_profile_behavior(void) {
    dsdneoUserConfig cfg = {};
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_YSF;

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    int rc = 0;
    if (!(opts.frame_ysf == 1 && opts.frame_dstar == 0 && opts.frame_dmr == 0)) {
        DSD_FPRINTF(stderr, "YSF config mode flags not applied as expected\n");
        rc |= 1;
    }
    if (opts.pulse_digi_out_channels != 2 || opts.dmr_stereo != 1) {
        DSD_FPRINTF(stderr, "YSF config profile audio mismatch channels=%d stereo=%d\n", opts.pulse_digi_out_channels,
                    opts.dmr_stereo);
        rc |= 1;
    }
    if (strcmp(opts.output_name, "YSF") != 0) {
        DSD_FPRINTF(stderr, "YSF output_name mismatch: %s\n", opts.output_name);
        rc |= 1;
    }
    return rc;
}

static int
test_snapshot_staged_file_rate_uses_requested_rate(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "/tmp/staged-input.wav");
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.wav_sample_rate = 48000;
    opts.staged_file_sample_rate = 96000;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    int rc = 0;
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_FILE) {
        DSD_FPRINTF(stderr, "staged file snapshot input_source mismatch\n");
        rc |= 1;
    }
    if (snap.file_sample_rate != 96000) {
        DSD_FPRINTF(stderr, "staged file snapshot sample rate mismatch: %d\n", snap.file_sample_rate);
        rc |= 1;
    }
    return rc;
}

static int
test_snapshot_mode_inference_tdma_and_auto(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset_opts_and_state(opts, state);
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_dstar = 0;
    opts.frame_ysf = 0;
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 0;
    opts.frame_provoice = 0;
    opts.frame_m17 = 0;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    int rc = 0;
    if (snap.decode_mode != DSDCFG_MODE_TDMA) {
        DSD_FPRINTF(stderr, "expected TDMA mode inference, got %d\n", (int)snap.decode_mode);
        rc |= 1;
    }

    /* A decoder set no preset reproduces must not be persisted as a preset: saving it as "auto"
       would reload as every decoder enabled. The snapshot leaves the mode unset so the renderer
       omits the key and the reload keeps whatever the fresh session established. */
    reset_opts_and_state(opts, state);
    opts.frame_dmr = 1;
    opts.frame_ysf = 1;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.decode_mode != DSDCFG_MODE_UNSET) {
        DSD_FPRINTF(stderr, "expected unset mode inference for an unmatched set, got %d\n", (int)snap.decode_mode);
        rc |= 1;
    }
    char unmatched[4096];
    if (render_config_to_buffer(&snap, unmatched, sizeof unmatched) != 0) {
        return 1;
    }
    if (strstr(unmatched, "decode =") != nullptr) {
        DSD_FPRINTF(stderr, "unmatched decoder set rendered a decode key:\n%s\n", unmatched);
        rc |= 1;
    }

    /* The AUTO set itself still round-trips as "auto". */
    reset_opts_and_state(opts, state);
    opts.frame_dstar = 1;
    opts.frame_x2tdma = 1;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    opts.frame_dmr = 1;
    opts.frame_dpmr = 1;
    opts.frame_provoice = 1;
    opts.frame_ysf = 1;
    opts.frame_m17 = 1;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.decode_mode != DSDCFG_MODE_AUTO) {
        DSD_FPRINTF(stderr, "expected AUTO inference for the full decoder set, got %d\n", (int)snap.decode_mode);
        rc |= 1;
    }
    return rc;
}

/*
 * A -fa session must come back as a -fa session. The decoder set is persisted as `decode =
 * "auto"` and reloading it re-enables every decoder; a set no preset reproduces persists no
 * decode key at all and reloads with the session's own decoder set untouched.
 */
static int
test_snapshot_roundtrip_preserves_decoder_set(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    reset_opts_and_state(opts, state);
    (void)dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state);

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "decode = \"auto\"") == nullptr) {
        DSD_FPRINTF(stderr, "-fa session did not persist decode = \"auto\":\n%s\n", rendered);
        return 1;
    }

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig loaded;
    int load_rc = dsd_user_config_load(path, &loaded);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "reloading the -fa snapshot failed\n");
        return 1;
    }

    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&loaded, &opts, &state);
    if (!(opts.frame_dstar == 1 && opts.frame_x2tdma == 1 && opts.frame_p25p1 == 1 && opts.frame_p25p2 == 1
          && opts.frame_nxdn48 == 1 && opts.frame_nxdn96 == 1 && opts.frame_dmr == 1 && opts.frame_dpmr == 1
          && opts.frame_provoice == 1 && opts.frame_ysf == 1 && opts.frame_m17 == 1)) {
        DSD_FPRINTF(stderr, "reloaded auto config lost decoders\n");
        rc |= 1;
    }

    /* A narrowed set no preset reproduces (here the pre-Auto default, without NXDN, dPMR, ProVoice or M17) persists no
       decode key, so a reload cannot widen it. The default set itself is now Auto and saves as `auto`
       (RUNTIME_CONFIG_APPLY). */
    reset_opts_and_state(opts, state);
    opts.frame_dstar = 1;
    opts.frame_x2tdma = 1;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_ysf = 1;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "decode =") != nullptr) {
        DSD_FPRINTF(stderr, "narrowed decoder set rendered a decode key:\n%s\n", rendered);
        return 1;
    }
    if (write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }
    load_rc = dsd_user_config_load(path, &loaded);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "reloading the narrowed snapshot failed\n");
        return 1;
    }

    reset_opts_and_state(opts, state);
    opts.frame_dstar = 1;
    opts.frame_x2tdma = 1;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_ysf = 1;
    dsd_apply_user_config_to_opts(&loaded, &opts, &state);
    if (opts.frame_nxdn48 != 0 || opts.frame_nxdn96 != 0 || opts.frame_dpmr != 0 || opts.frame_provoice != 0
        || opts.frame_m17 != 0) {
        DSD_FPRINTF(stderr, "reloading a narrowed-set config widened the decoder set\n");
        rc |= 1;
    }
    if (opts.frame_dmr != 1 || opts.frame_p25p1 != 1) {
        DSD_FPRINTF(stderr, "reloading a narrowed-set config dropped decoders\n");
        rc |= 1;
    }
    return rc;
}

static int
test_snapshot_roundtrip_dmr_mono_override(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    struct {
        const char* label;
        dsdneoUserDecodeMode expected_mode;
        int frame_dstar;
        int frame_p25p1;
        int frame_p25p2;
        int frame_dmr;
        int all_digital;
        int expected_stereo;
    } cases[] = {
        {"dmr-only", DSDCFG_MODE_DMR_MONO, 0, 0, 0, 1, 0, 0},
        /* Matches no preset, so it persists without a decode key; the override must survive anyway. */
        {"unmatched-set", DSDCFG_MODE_UNSET, 1, 1, 1, 1, 0, 1},
        {"auto", DSDCFG_MODE_AUTO, 0, 0, 0, 0, 1, 1},
        {"tdma", DSDCFG_MODE_TDMA, 0, 1, 1, 1, 0, 1},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_opts_and_state(opts, state);
        opts.frame_dstar = cases[i].frame_dstar;
        opts.frame_p25p1 = cases[i].frame_p25p1;
        opts.frame_p25p2 = cases[i].frame_p25p2;
        opts.frame_dmr = cases[i].frame_dmr;
        if (cases[i].all_digital) {
            opts.frame_dstar = 1;
            opts.frame_x2tdma = 1;
            opts.frame_p25p1 = 1;
            opts.frame_p25p2 = 1;
            opts.frame_nxdn48 = 1;
            opts.frame_nxdn96 = 1;
            opts.frame_dmr = 1;
            opts.frame_dpmr = 1;
            opts.frame_provoice = 1;
            opts.frame_ysf = 1;
            opts.frame_m17 = 1;
        }
        opts.dmr_mono = 1;
        opts.dmr_stereo = cases[i].expected_stereo;
        state.dmr_stereo = cases[i].expected_stereo;

        dsdneoUserConfig snap;
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        if (!snap.has_mode || snap.decode_mode != cases[i].expected_mode || !snap.has_dmr_mono || snap.dmr_mono != 1) {
            DSD_FPRINTF(stderr, "%s mono snapshot mismatch: mode=%d has_override=%d override=%d\n", cases[i].label,
                        (int)snap.decode_mode, snap.has_dmr_mono, snap.dmr_mono);
            rc |= 1;
        }

        char rendered[4096];
        if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
            return 1;
        }
        if (!strstr(rendered, "dmr_mono = true")) {
            DSD_FPRINTF(stderr, "%s mono snapshot did not render the independent override:\n%s\n", cases[i].label,
                        rendered);
            rc |= 1;
        }

        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(rendered, path, sizeof path) != 0) {
            return 1;
        }

        dsdneoUserConfig loaded;
        if (dsd_user_config_load(path, &loaded) != 0 || loaded.decode_mode != cases[i].expected_mode
            || !loaded.has_dmr_mono || loaded.dmr_mono != 1) {
            DSD_FPRINTF(stderr, "%s mono saved config did not reload the override\n", cases[i].label);
            (void)remove(path);
            return 1;
        }

        reset_opts_and_state(opts, state);
        opts.dmr_stereo = 1;
        state.dmr_stereo = 1;
        dsd_apply_user_config_to_opts(&loaded, &opts, &state);
        if (opts.dmr_mono != 1 || opts.dmr_stereo != cases[i].expected_stereo
            || state.dmr_stereo != cases[i].expected_stereo) {
            DSD_FPRINTF(stderr, "%s mono reload mismatch: mono=%d stereo=%d state_stereo=%d\n", cases[i].label,
                        opts.dmr_mono, opts.dmr_stereo, state.dmr_stereo);
            rc |= 1;
        }

        (void)remove(path);
    }
    return rc;
}

static int
test_dmr_mono_preset_precedes_false_override(void) {
    static const char* ini = "version = 1\n\n"
                             "[mode]\n"
                             "decode = \"dmr_mono\"\n"
                             "dmr_mono = false\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "dmr_mono preset conflict config failed to load\n");
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.dmr_stereo = 1;
    state.dmr_stereo = 1;
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (opts.frame_dmr != 1 || opts.dmr_mono != 1 || opts.dmr_stereo != 0 || state.dmr_stereo != 0) {
        DSD_FPRINTF(stderr, "dmr_mono preset lost to false override: frame=%d mono=%d stereo=%d state_stereo=%d\n",
                    opts.frame_dmr, opts.dmr_mono, opts.dmr_stereo, state.dmr_stereo);
        return 1;
    }
    return 0;
}

/*
 * The three settings a RadioReference import applies that used to have no INI
 * key at all: conventional scanning (-Y), the P25 learned-candidate preference
 * (-^), and the EDACS EA/ESK variant. Before these keys existed, a Config->Save
 * of an imported multi-repeater conventional system or an ESK EDACS system came
 * back decoding differently on the next launch.
 */
static int
test_persistence_gap_schema_rows(void) {
    int rc = 0;

    static const struct {
        const char* section;
        const char* key;
    } expected[] = {
        {"trunking", "scanner"},
        {"trunking", "p25_prefer_candidates"},
        {"mode", "edacs_addressing"},
        {"mode", "edacs_ea"},
        {"mode", "edacs_esk"},
        {"mode", "dmr_lrrp_ports"},
        {"mode", "dmr_udp_comp_ports"},
        {"trunking", "p25_bandplan_csv"},
        {"trunking", "src_csv"},
        {"trunking", "scan_voice_only"},
        {"trunking", "scan_skip_silent"},
        {"trunking", "scan_skip_silent_ms"},
        {"trunking", "scan_voice_qualify_ms"},
        {"trunking", "scan_voice_hold_ms"},
        {"trunking", "scan_max_visit_ms"},
        {"analog", "nfm_bandwidth_hz"},
        {"analog", "am_bandwidth_hz"},
        {"analog", "tone_filter"},
        {"analog", "tone_list"},
    };

    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        if (dsdcfg_schema_find(expected[i].section, expected[i].key) == NULL) {
            DSD_FPRINTF(stderr, "FAIL: %s.%s is not in the schema\n", expected[i].section, expected[i].key);
            rc |= 1;
        }
    }
    return rc;
}

static int
load_am_config_text(const char* ini, dsdneoUserConfig* cfg) {
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    const int load_rc = dsd_user_config_load(path, cfg);
    (void)remove(path);
    return load_rc;
}

/*
 * Issue #524: [mode] decode = am and [analog] am_bandwidth_hz ride INI -> cfg -> opts -> snapshot -> render. 0 in the
 * options is the AM default, not a request, so a save writes the key only for an explicit width (6000 included); the
 * section itself is always written, so a config saved at the default loads back as the default. A width outside
 * 5000..20000, 0 or not whole Hz, is refused by the loader, which keeps the width it had loaded (the default, for a
 * base file).
 */
static int
test_am_mode_and_bandwidth_roundtrip(void) {
    int rc = 0;
    dsdneoUserConfig cfg;
    if (load_am_config_text("[mode]\ndecode = am\n[analog]\nam_bandwidth_hz = 8000\n", &cfg) != 0) {
        DSD_FPRINTF(stderr, "AM config failed to load\n");
        return 1;
    }
    if (!cfg.has_mode || cfg.decode_mode != DSDCFG_MODE_AM || !cfg.has_analog || cfg.analog_am_bandwidth_hz != 8000) {
        DSD_FPRINTF(stderr, "FAIL: AM config did not load: mode=%d has_analog=%d width=%d\n", (int)cfg.decode_mode,
                    cfg.has_analog, cfg.analog_am_bandwidth_hz);
        rc |= 1;
    }
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_am_bandwidth_hz != 8000 || opts.analog_demod != DSD_ANALOG_DEMOD_AM || opts.analog_only != 1
        || dsd_infer_decode_mode_preset(&opts) != DSDCFG_MODE_AM) {
        DSD_FPRINTF(stderr, "FAIL: AM config did not reach dsd_opts: width=%d kind=%d analog_only=%d\n",
                    opts.analog_am_bandwidth_hz, opts.analog_demod, opts.analog_only);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("AM decode mode", rendered, "decode = \"am\"\n");
    rc |= expect_contains("explicit AM width", rendered, "[analog]\nam_bandwidth_hz = 8000\n");

    opts.analog_am_bandwidth_hz = DSD_ANALOG_AM_WIDTH_DEFAULT_HZ;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("explicit default-valued AM width", rendered, "am_bandwidth_hz = 6000\n");

    opts.analog_am_bandwidth_hz = 0;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "am_bandwidth_hz") != NULL || !strstr(rendered, "[analog]\n")) {
        DSD_FPRINTF(stderr, "FAIL: the default AM width was saved, or its section left out:\n%s\n", rendered);
        rc |= 1;
    }
    if (load_am_config_text(rendered, &cfg) != 0 || !cfg.has_analog || cfg.analog_am_bandwidth_hz != 0) {
        DSD_FPRINTF(stderr, "FAIL: a saved AM default did not load back as the default\n");
        rc |= 1;
    }
    opts.analog_am_bandwidth_hz = 12000;
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_am_bandwidth_hz != 0) {
        DSD_FPRINTF(stderr, "FAIL: loading a config saved at the AM default kept width %d\n",
                    opts.analog_am_bandwidth_hz);
        rc |= 1;
    }

    /* Each invalid value follows a valid width in the same section: the loader refuses it and keeps that width, so a
       value it took instead (0 as the default included) reads as a change. */
    const char* invalid[] = {"25000", "6k", "4999", "0"};
    for (const char* value : invalid) {
        char ini[128];
        DSD_SNPRINTF(ini, sizeof ini, "[analog]\nam_bandwidth_hz = 10000\nam_bandwidth_hz = %s\n", value);
        if (load_am_config_text(ini, &cfg) != 0 || cfg.analog_am_bandwidth_hz != 10000) {
            DSD_FPRINTF(stderr, "FAIL: invalid AM width %s was not refused (width %d)\n", value,
                        cfg.analog_am_bandwidth_hz);
            rc |= 1;
        }
    }
    /* A config without [analog] leaves the configured width alone. */
    reset_opts_and_state(opts, state);
    opts.analog_am_bandwidth_hz = 15000;
    if (load_am_config_text("[mode]\ndecode = \"am\"\n", &cfg) != 0) {
        return 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_am_bandwidth_hz != 15000 || opts.analog_demod != DSD_ANALOG_DEMOD_AM) {
        DSD_FPRINTF(stderr, "FAIL: a config without [analog] changed the AM width to %d\n",
                    opts.analog_am_bandwidth_hz);
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/* [mode] decode = tetra rides INI -> cfg -> opts -> snapshot -> render and loads back: the TETRA preset (-fT) has an
 * INI name like every other preset, so a TETRA session's save does not drop its mode. */
static int
test_tetra_mode_roundtrip(void) {
    int rc = 0;
    static dsdneoUserConfig cfg;
    if (load_am_config_text("[mode]\ndecode = \"TETRA\"\n", &cfg) != 0 || !cfg.has_mode
        || cfg.decode_mode != DSDCFG_MODE_TETRA) {
        DSD_FPRINTF(stderr, "FAIL: decode = TETRA did not load as the TETRA preset (mode %d)\n", (int)cfg.decode_mode);
        return 1;
    }
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.frame_tetra != 1 || opts.frame_dmr != 0 || dsd_infer_decode_mode_preset(&opts) != DSDCFG_MODE_TETRA) {
        DSD_FPRINTF(stderr, "FAIL: decode = tetra did not reach dsd_opts (frame_tetra %d)\n", opts.frame_tetra);
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        dsd_state_ext_free_all(&state);
        return 1;
    }
    rc |= expect_contains("TETRA decode mode", rendered, "decode = \"tetra\"\n");
    if (load_am_config_text(rendered, &cfg) != 0 || cfg.decode_mode != DSDCFG_MODE_TETRA) {
        DSD_FPRINTF(stderr, "FAIL: a saved TETRA session did not load back as TETRA\n");
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

static int g_tetra_ext_cleanups = 0;

static void
count_tetra_ext_cleanup(void* ptr) {
    g_tetra_ext_cleanups++;
    free(ptr);
}

/*
 * A [mode] without a decode key (a demod setting alone) applies no preset, so a TETRA session that applies it keeps its
 * TETRA decoder: the flag, the decoder's state extension and what it published.
 */
static int
test_mode_without_decode_keeps_tetra(void) {
    int rc = 0;
    static dsdneoUserConfig cfg;
    if (load_am_config_text("[mode]\ndemod = \"qpsk\"\n", &cfg) != 0 || !cfg.has_mode
        || cfg.decode_mode != DSDCFG_MODE_UNSET) {
        DSD_FPRINTF(stderr, "FAIL: a [mode] with a demod alone did not load as an unset decode mode (mode %d)\n",
                    (int)cfg.decode_mode);
        return 1;
    }
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_TETRA, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "FAIL: TETRA preset failed\n");
        return 1;
    }
    int* ext = static_cast<int*>(calloc(1U, sizeof(int)));
    if (!ext || dsd_state_ext_set(&state, DSD_STATE_EXT_PROTO_TETRA, ext, count_tetra_ext_cleanup) != 0) {
        free(ext);
        return 1;
    }
    g_tetra_ext_cleanups = 0;
    state.tetra_pub.mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    state.tetra_pub.generation = 3U;
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.frame_tetra != 1 || dsd_state_ext_get(&state, DSD_STATE_EXT_PROTO_TETRA) != ext
        || g_tetra_ext_cleanups != 0 || state.tetra_pub.mode != DSD_TETRA_MODE_TMO_DOWNLINK
        || state.tetra_pub.generation != 3U || dsd_infer_decode_mode_preset(&opts) != DSDCFG_MODE_TETRA) {
        DSD_FPRINTF(stderr, "FAIL: a [mode] without a decode key left TETRA: frame_tetra %d, cleanups %d\n",
                    opts.frame_tetra, g_tetra_ext_cleanups);
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * Issue #527: [analog] tone_filter and tone_list ride INI -> cfg -> opts -> snapshot -> render. The mode is written
 * when it is on, the list whenever there is one (off keeps it), and both load back. A refused list, or a list policy
 * with no list, applies as off. A scan row's own policy runs over dsd_opts while the row is on air and is never what a
 * save writes: the configured one is.
 */
static int
test_tone_policy_roundtrip(void) {
    int rc = 0;
    dsdneoUserConfig cfg;
    if (load_am_config_text("[analog]\ntone_filter = allow\ntone_list = \"d023/100\"\n", &cfg) != 0) {
        return 1;
    }
    char list[DSD_TONE_LIST_TEXT_MAX + 1] = "";
    (void)dsd_tone_set_format(&cfg.analog_tone_set, list, sizeof list);
    if (!cfg.has_analog || cfg.analog_tone_filter != DSD_TONE_FILTER_ALLOW || strcmp(list, "100.0/D023N") != 0) {
        DSD_FPRINTF(stderr, "FAIL: tone policy did not load: mode=%d list=%s\n", cfg.analog_tone_filter, list);
        rc |= 1;
    }
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_tone_filter != DSD_TONE_FILTER_ALLOW
        || !dsd_tone_set_equal(&opts.analog_tone_set, &cfg.analog_tone_set)) {
        DSD_FPRINTF(stderr, "FAIL: tone policy did not reach dsd_opts\n");
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("tone policy", rendered, "tone_filter = \"allow\"\ntone_list = \"100.0/D023N\"\n");

    /* A row's own policy, on air: the save still writes the configured one. */
    if (dsd_scan_mode_begin(&opts, &state) != 0 || dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) != 0) {
        return 1;
    }
    dsd_scan_option_values row = {};
    row.present = DSD_SCAN_OPT_TONE;
    row.tone_filter = DSD_TONE_FILTER_BLOCK;
    if (dsd_tone_set_parse("67.0", &row.tone_set, NULL, 0) != 0 || dsd_scan_mode_options(&opts, &state, &row) != 0
        || opts.analog_tone_filter != DSD_TONE_FILTER_BLOCK) {
        DSD_FPRINTF(stderr, "FAIL: the row policy did not go on air\n");
        rc |= 1;
    }
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("configured policy under a row", rendered,
                          "tone_filter = \"allow\"\ntone_list = \"100.0/D023N\"\n");
    if (strstr(rendered, "tone_filter = \"block\"") || strstr(rendered, "tone_list = \"67.0\"")) {
        DSD_FPRINTF(stderr, "FAIL: a row's tone policy was saved:\n%s\n", rendered);
        rc |= 1;
    }
    dsd_scan_mode_leave(&opts, &state);

    /* Off keeps the list; a saved off with a list loads back as off with the list. */
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "tone_filter") || !strstr(rendered, "[analog]\ntone_list = \"100.0/D023N\"\n")) {
        DSD_FPRINTF(stderr, "FAIL: off should save the list alone:\n%s\n", rendered);
        rc |= 1;
    }
    if (load_am_config_text(rendered, &cfg) != 0 || cfg.analog_tone_filter != DSD_TONE_FILTER_OFF
        || dsd_tone_set_count(&cfg.analog_tone_set) != 2) {
        DSD_FPRINTF(stderr, "FAIL: a saved off with a list did not load back\n");
        rc |= 1;
    }

    /* A refused list, and a list policy without a list, apply as off. */
    static const char* const off[] = {"[analog]\ntone_filter = allow\ntone_list = 100.0,67.0\n",
                                      "[analog]\ntone_filter = block\n",
                                      "[analog]\ntone_filter = allow\ntone_list = 161.0\n"};
    for (const char* ini : off) {
        opts.analog_tone_filter = DSD_TONE_FILTER_BLOCK;
        if (load_am_config_text(ini, &cfg) != 0) {
            return 1;
        }
        dsd_apply_user_config_to_opts(&cfg, &opts, &state);
        if (opts.analog_tone_filter != DSD_TONE_FILTER_OFF) {
            DSD_FPRINTF(stderr, "FAIL: %s did not apply as off\n", ini);
            rc |= 1;
        }
    }
    /* A refused mode keeps the one already loaded. */
    if (load_am_config_text("[analog]\ntone_filter = block\ntone_filter = deny\n", &cfg) != 0
        || cfg.analog_tone_filter != DSD_TONE_FILTER_BLOCK) {
        DSD_FPRINTF(stderr, "FAIL: a refused tone_filter replaced the one loaded\n");
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * The live tone-filter editor (DSD_APP_CMD_TONE_FILTER_SET) edits the configured policy through
 * dsd_scan_mode_set_configured_tone_policy(). Edited while an nfm row's own policy is on air, the edit is what a save
 * writes and loads back, never the row's; edited outside a row it is in force at once and saved the same way, off with
 * its list kept.
 */
static int
test_tone_policy_edit_roundtrip(void) {
    int rc = 0;
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    opts.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    dsd_tone_set row_set = {};
    dsd_tone_set edited = {};
    if (dsd_tone_set_parse("100.0", &opts.analog_tone_set, NULL, 0) != 0
        || dsd_tone_set_parse("67.0", &row_set, NULL, 0) != 0
        || dsd_tone_set_parse("D023I/71.9", &edited, NULL, 0) != 0) {
        return 1;
    }
    if (dsd_scan_mode_begin(&opts, &state) != 0 || dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) != 0) {
        return 1;
    }
    dsd_scan_option_values row = {};
    row.present = DSD_SCAN_OPT_TONE;
    row.tone_filter = DSD_TONE_FILTER_BLOCK;
    row.tone_set = row_set;
    if (dsd_scan_mode_options(&opts, &state, &row) != 0) {
        return 1;
    }
    if (dsd_scan_mode_set_configured_tone_policy(&opts, &state, DSD_TONE_FILTER_BLOCK, &edited) != 0
        || opts.analog_tone_filter != DSD_TONE_FILTER_BLOCK
        || dsd_tone_set_equal(&opts.analog_tone_set, &row_set) == 0) {
        DSD_FPRINTF(stderr, "FAIL: the row's own policy should shadow the edit\n");
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |=
        expect_contains("edited policy under a row", rendered, "tone_filter = \"block\"\ntone_list = \"71.9/D023I\"\n");
    if (strstr(rendered, "tone_list = \"67.0\"") != nullptr || strstr(rendered, "tone_list = \"100.0\"") != nullptr) {
        DSD_FPRINTF(stderr, "FAIL: the row's or the old policy was saved:\n%s\n", rendered);
        rc |= 1;
    }
    dsdneoUserConfig loaded;
    if (load_am_config_text(rendered, &loaded) != 0 || loaded.analog_tone_filter != DSD_TONE_FILTER_BLOCK
        || dsd_tone_set_equal(&loaded.analog_tone_set, &edited) == 0) {
        DSD_FPRINTF(stderr, "FAIL: the edited policy did not load back\n");
        rc |= 1;
    }
    dsd_scan_mode_leave(&opts, &state);
    if (opts.analog_tone_filter != DSD_TONE_FILTER_BLOCK || dsd_tone_set_equal(&opts.analog_tone_set, &edited) == 0) {
        DSD_FPRINTF(stderr, "FAIL: the row's departure did not restore the edited policy\n");
        rc |= 1;
    }

    /* No row: the edit is in force at once; off keeps its list, and saves and loads back as off with the list. */
    if (dsd_scan_mode_set_configured_tone_policy(&opts, &state, DSD_TONE_FILTER_OFF, &edited) != 1
        || opts.analog_tone_filter != DSD_TONE_FILTER_OFF) {
        DSD_FPRINTF(stderr, "FAIL: the edit outside a row is not in force\n");
        rc |= 1;
    }
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "tone_filter") != nullptr || strstr(rendered, "tone_list = \"71.9/D023I\"\n") == nullptr
        || load_am_config_text(rendered, &loaded) != 0 || loaded.analog_tone_filter != DSD_TONE_FILTER_OFF
        || dsd_tone_set_equal(&loaded.analog_tone_set, &edited) == 0) {
        DSD_FPRINTF(stderr, "FAIL: an edited off did not save its list alone:\n%s\n", rendered);
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_scanner_and_candidates_roundtrip(void) {
    static const char* ini = "[trunking]\n"
                             "scanner = true\n"
                             "p25_prefer_candidates = true\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "scanner/candidates config failed to load\n");
        return 1;
    }

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.scanner_mode != 1) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] scanner did not reach opts.scanner_mode\n");
        rc |= 1;
    }
    if (opts.p25_prefer_candidates != 1) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] p25_prefer_candidates did not reach dsd_opts\n");
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.trunk_scanner || !snap.trunk_p25_prefer_candidates) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped the scanner / candidate-preference answers\n");
        rc |= 1;
    }

    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("trunking scanner", rendered, "scanner = true\n");
    rc |= expect_contains_quiet("trunking candidate preference", rendered, "p25_prefer_candidates = true\n");

    /* Off must round-trip as explicitly off, not as an omitted key: these are
       tuner-owner answers, and a missing key would silently inherit whatever
       the previous session left in opts. */
    reset_opts_and_state(opts, state);
    dsdneoUserConfig off_snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &off_snap);
    if (render_config_to_buffer(&off_snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("trunking scanner off", rendered, "scanner = false\n");
    rc |= expect_contains_quiet("trunking candidates off", rendered, "p25_prefer_candidates = false\n");
    return rc;
}

static int
test_scan_voice_gate_roundtrip(void) {
    static const char* ini = "[trunking]\n"
                             "scan_voice_only = true\n"
                             "scan_voice_qualify_ms = 1500\n"
                             "scan_voice_hold_ms = 2500\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "scan voice gate config failed to load\n");
        return 1;
    }

    int rc = 0;
    if (!cfg.trunk_scan_voice_only || cfg.trunk_scan_voice_qualify_ms != 1500 || cfg.trunk_scan_voice_hold_ms != 2500) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] scan voice gate keys did not load, got only=%d qualify=%d hold=%d\n",
                    cfg.trunk_scan_voice_only, cfg.trunk_scan_voice_qualify_ms, cfg.trunk_scan_voice_hold_ms);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.scan_voice_only != 1 || opts.scan_voice_qualify_ms != 1500 || opts.scan_voice_hold_ms != 2500) {
        DSD_FPRINTF(stderr,
                    "FAIL: [trunking] scan voice gate keys did not reach dsd_opts, got only=%d qualify=%d "
                    "hold=%d\n",
                    opts.scan_voice_only, opts.scan_voice_qualify_ms, opts.scan_voice_hold_ms);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.trunk_scan_voice_only || snap.trunk_scan_voice_qualify_ms != 1500
        || snap.trunk_scan_voice_hold_ms != 2500) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped the scan voice gate answers\n");
        rc |= 1;
    }

    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("trunking scan voice only", rendered, "scan_voice_only = true\n");
    rc |= expect_contains_quiet("trunking voice qualify", rendered, "scan_voice_qualify_ms = 1500\n");
    rc |= expect_contains_quiet("trunking voice hold", rendered, "scan_voice_hold_ms = 2500\n");

    /* Off must round-trip as explicitly off, not as an omitted key: a missing
       key would silently inherit whatever the previous session left in opts. */
    reset_opts_and_state(opts, state);
    dsdneoUserConfig off_snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &off_snap);
    if (render_config_to_buffer(&off_snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("trunking scan voice off", rendered, "scan_voice_only = false\n");
    return rc;
}

/* The session-wide skip-silent setting (#834): INI -> cfg -> opts -> snapshot -> render, off written explicitly, and an
 * omitted key resetting a live value to off when the config is applied. */
static int
test_scan_skip_silent_roundtrip(void) {
    int rc = 0;
    char path[DSD_TEST_PATH_MAX];
    static const char* const on_ini = "[trunking]\nscan_skip_silent = true\n";
    static const char* const omitted_ini = "[trunking]\nscan_voice_only = false\n";
    static dsd_opts opts;
    static dsd_state state;
    dsdneoUserConfig cfg;

    if (write_temp_config(on_ini, path, sizeof path) != 0) {
        return 1;
    }
    int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0 || !cfg.trunk_scan_skip_silent) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] scan_skip_silent did not load\n");
        return 1;
    }
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.scan_skip_silent != 1) {
        DSD_FPRINTF(stderr, "FAIL: scan_skip_silent did not reach dsd_opts\n");
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (!snap.trunk_scan_skip_silent || render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped scan_skip_silent\n");
        return 1;
    }
    rc |= expect_contains_quiet("trunking scan skip silent", rendered, "scan_skip_silent = true\n");

    /* An omitted key applies as off, even over a live "on". */
    if (write_temp_config(omitted_ini, path, sizeof path) != 0) {
        return 1;
    }
    load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    opts.scan_skip_silent = 1;
    if (load_rc != 0) {
        return 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.scan_skip_silent != 0) {
        DSD_FPRINTF(stderr, "FAIL: an omitted scan_skip_silent did not reset to off\n");
        rc |= 1;
    }

    /* Off round-trips as an explicit off. */
    dsdneoUserConfig off_snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &off_snap);
    if (render_config_to_buffer(&off_snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("trunking scan skip silent off", rendered, "scan_skip_silent = false\n");
    return rc;
}

/*
 * The global per-visit cap (issue #507) rides the same rails as the voice gate: INI -> cfg ->
 * opts -> snapshot -> render. Disabled renders as an explicit 0 so a saved config pins "off"
 * instead of inheriting whatever the previous session left in opts.
 */
static int
test_scan_max_visit_roundtrip(void) {
    static const char* ini = "[trunking]\n"
                             "scan_max_visit_ms = 20000\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "scan max visit config failed to load\n");
        return 1;
    }

    int rc = 0;
    if (cfg.trunk_scan_max_visit_ms != 20000) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] scan_max_visit_ms did not load, got %d\n", cfg.trunk_scan_max_visit_ms);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.scan_max_visit_ms != 20000) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] scan_max_visit_ms did not reach dsd_opts, got %d\n",
                    opts.scan_max_visit_ms);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.trunk_scan_max_visit_ms != 20000) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped scan_max_visit_ms, got %d\n", snap.trunk_scan_max_visit_ms);
        rc |= 1;
    }

    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("trunking scan max visit", rendered, "scan_max_visit_ms = 20000\n");

    /* Disabled must round-trip as an explicit 0, not as an omitted key. */
    reset_opts_and_state(opts, state);
    dsdneoUserConfig off_snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &off_snap);
    if (render_config_to_buffer(&off_snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("trunking scan max visit off", rendered, "scan_max_visit_ms = 0\n");
    return rc;
}

/* The skip-silent reading length (#834): [trunking] scan_skip_silent_ms loads 0 (the calibrated reading) and 10..500
 * as given, reaches dsd_opts, and saves back, always written; a [trunking] without it is 0, also over a live value. A
 * value of 1..9, past 500, negative or not a whole number is refused with a warning naming the key, and the value
 * already loaded stays: never clamped. */
static int
test_scan_skip_silent_ms_roundtrip(void) {
    int rc = 0;
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    for (int want : {0, 10, 85, 90, 500}) {
        char ini[96];
        DSD_SNPRINTF(ini, sizeof ini, "[trunking]\nscan_skip_silent = true\nscan_skip_silent_ms = %d\n", want);
        dsdneoUserConfig cfg{};
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(ini, path, sizeof path) != 0) {
            return 1;
        }
        const int load_rc = dsd_user_config_load(path, &cfg);
        (void)remove(path);
        if (load_rc != 0 || cfg.trunk_scan_skip_silent_ms != want) {
            DSD_FPRINTF(stderr, "FAIL: scan_skip_silent_ms = %d loaded as %d\n", want, cfg.trunk_scan_skip_silent_ms);
            rc |= 1;
            continue;
        }
        reset_opts_and_state(opts, state);
        opts.scan_skip_silent_ms = 70;
        dsd_apply_user_config_to_opts(&cfg, &opts, &state);
        if (opts.scan_skip_silent_ms != want) {
            DSD_FPRINTF(stderr, "FAIL: scan_skip_silent_ms = %d reached dsd_opts as %d\n", want,
                        opts.scan_skip_silent_ms);
            rc |= 1;
        }
        dsdneoUserConfig snap;
        dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
        char rendered[8192];
        if (snap.trunk_scan_skip_silent_ms != want || render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
            DSD_FPRINTF(stderr, "FAIL: the snapshot dropped scan_skip_silent_ms = %d\n", want);
            rc |= 1;
            continue;
        }
        char line[64];
        DSD_SNPRINTF(line, sizeof line, "scan_skip_silent_ms = %d\n", want);
        rc |= expect_contains_quiet("trunking scan skip silent ms", rendered, line);
        dsdneoUserConfig reloaded{};
        if (write_temp_config(rendered, path, sizeof path) != 0) {
            return 1;
        }
        const int reload_rc = dsd_user_config_load(path, &reloaded);
        (void)remove(path);
        if (reload_rc != 0 || reloaded.trunk_scan_skip_silent_ms != want) {
            DSD_FPRINTF(stderr, "FAIL: the saved config did not load scan_skip_silent_ms = %d back\n", want);
            rc |= 1;
        }
    }

    /* Left out of a present [trunking]: 0, even over a live value. */
    {
        dsdneoUserConfig cfg{};
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config("[trunking]\nscan_skip_silent = true\n", path, sizeof path) != 0) {
            return 1;
        }
        const int load_rc = dsd_user_config_load(path, &cfg);
        (void)remove(path);
        reset_opts_and_state(opts, state);
        opts.scan_skip_silent_ms = 120;
        if (load_rc != 0 || cfg.trunk_scan_skip_silent_ms != 0) {
            DSD_FPRINTF(stderr, "FAIL: an omitted scan_skip_silent_ms loaded as %d\n", cfg.trunk_scan_skip_silent_ms);
            rc |= 1;
        }
        dsd_apply_user_config_to_opts(&cfg, &opts, &state);
        if (opts.scan_skip_silent_ms != 0) {
            DSD_FPRINTF(stderr, "FAIL: an omitted scan_skip_silent_ms kept %d\n", opts.scan_skip_silent_ms);
            rc |= 1;
        }
    }

    /* Each refused value follows a valid one in the same section, which stays, with a warning naming the key. */
    for (const char* value : {"1", "9", "501", "-10", "abc", "5e1", "50ms", "99999999999"}) {
        char ini[128];
        DSD_SNPRINTF(ini, sizeof ini, "[trunking]\nscan_skip_silent_ms = 50\nscan_skip_silent_ms = %s\n", value);
        dsdneoUserConfig cfg{};
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(ini, path, sizeof path) != 0) {
            return 1;
        }
        g_log_text[0] = '\0';
        const int load_rc = dsd_user_config_load(path, &cfg);
        (void)remove(path);
        if (load_rc != 0 || cfg.trunk_scan_skip_silent_ms != 50) {
            DSD_FPRINTF(stderr, "FAIL: scan_skip_silent_ms = %s was not refused (%d)\n", value,
                        cfg.trunk_scan_skip_silent_ms);
            rc |= 1;
        }
        if (!strstr(g_log_text, "scan_skip_silent_ms")) {
            DSD_FPRINTF(stderr, "FAIL: scan_skip_silent_ms = %s refused without a warning (log: '%s')\n", value,
                        g_log_text);
            rc |= 1;
        }
    }
    return rc;
}

static int
load_config_text(const char* ini, dsdneoUserConfig* cfg) {
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    const int load_rc = dsd_user_config_load(path, cfg);
    (void)remove(path);
    return load_rc;
}

/* Issue #756: [recording] analog_max_gap_ms loads, reaches opts->analog_record_max_gap_ms, and saves back, always
   written; a [recording] without it is the 200 ms default (off). A value outside 0..10000, or not a whole number, is
   refused with a warning and the value already loaded stays: never clamped. */
static int
test_recording_analog_max_gap_roundtrip(void) {
    int rc = 0;
    dsdneoUserConfig cfg{};
    if (load_config_text("[recording]\nanalog_max_gap_ms = 2000\n", &cfg) != 0 || !cfg.has_recording
        || cfg.analog_max_gap_ms != 2000) {
        DSD_FPRINTF(stderr, "FAIL: analog_max_gap_ms did not load (%d)\n", cfg.analog_max_gap_ms);
        return 1;
    }
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_record_max_gap_ms != 2000) {
        DSD_FPRINTF(stderr, "FAIL: analog_max_gap_ms did not reach dsd_opts (%d)\n", opts.analog_record_max_gap_ms);
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("analog_max_gap_ms saved", rendered, "analog_max_gap_ms = 2000\n");
    dsdneoUserConfig reloaded{};
    if (load_config_text(rendered, &reloaded) != 0 || reloaded.analog_max_gap_ms != 2000) {
        DSD_FPRINTF(stderr, "FAIL: the saved config did not load analog_max_gap_ms back\n");
        rc |= 1;
    }

    /* Left out of a present [recording]: the default, saved as itself. */
    if (load_config_text("[recording]\nper_call_wav = true\n", &cfg) != 0 || cfg.analog_max_gap_ms != 200) {
        DSD_FPRINTF(stderr, "FAIL: a [recording] without the key did not default it (%d)\n", cfg.analog_max_gap_ms);
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_record_max_gap_ms != 200) {
        DSD_FPRINTF(stderr, "FAIL: a [recording] without the key kept %d\n", opts.analog_record_max_gap_ms);
        rc |= 1;
    }
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("analog_max_gap_ms saved at the default", rendered, "analog_max_gap_ms = 200\n");

    /* Both ends of the range load as given. */
    for (int want : {0, 10000}) {
        char ini[96];
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\nanalog_max_gap_ms = %d\n", want);
        if (load_config_text(ini, &cfg) != 0 || cfg.analog_max_gap_ms != want) {
            DSD_FPRINTF(stderr, "FAIL: analog_max_gap_ms = %d loaded as %d\n", want, cfg.analog_max_gap_ms);
            rc |= 1;
        }
    }

    /* Each refused value follows a valid one in the same section, which stays, with a warning naming the key. */
    for (const char* value : {"-1", "10001", "2s", "1e3", "99999999999"}) {
        char ini[128];
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\nanalog_max_gap_ms = 1500\nanalog_max_gap_ms = %s\n", value);
        g_log_text[0] = '\0';
        if (load_config_text(ini, &cfg) != 0 || cfg.analog_max_gap_ms != 1500) {
            DSD_FPRINTF(stderr, "FAIL: analog_max_gap_ms = %s was not refused (%d)\n", value, cfg.analog_max_gap_ms);
            rc |= 1;
        }
        if (!strstr(g_log_text, "analog_max_gap_ms")) {
            DSD_FPRINTF(stderr, "FAIL: analog_max_gap_ms = %s refused without a warning (log: '%s')\n", value,
                        g_log_text);
            rc |= 1;
        }
    }
    return rc;
}

/* Issue #668: [recording] analog_preroll_ms loads, reaches opts->analog_record_preroll_ms, and saves back, always
   written; a [recording] without it is the default. A value outside 0..1000, or not a whole number, is refused with a
   warning and the value already loaded stays: never clamped. */
namespace {
int
test_recording_analog_preroll_roundtrip(void) {
    int rc = 0;
    dsdneoUserConfig cfg{};
    if (load_config_text("[recording]\nanalog_preroll_ms = 300\n", &cfg) != 0 || cfg.has_recording == 0
        || cfg.analog_preroll_ms != 300) {
        DSD_FPRINTF(stderr, "FAIL: analog_preroll_ms did not load (%d)\n", cfg.analog_preroll_ms);
        return 1;
    }
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_record_preroll_ms != 300) {
        DSD_FPRINTF(stderr, "FAIL: analog_preroll_ms did not reach dsd_opts (%d)\n", opts.analog_record_preroll_ms);
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("analog_preroll_ms saved", rendered, "analog_preroll_ms = 300\n");
    dsdneoUserConfig reloaded{};
    if (load_config_text(rendered, &reloaded) != 0 || reloaded.analog_preroll_ms != 300) {
        DSD_FPRINTF(stderr, "FAIL: the saved config did not load analog_preroll_ms back\n");
        rc |= 1;
    }

    /* Left out of a present [recording]: the default, saved as itself. */
    if (load_config_text("[recording]\nper_call_wav = true\n", &cfg) != 0
        || cfg.analog_preroll_ms != DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS) {
        DSD_FPRINTF(stderr, "FAIL: a [recording] without the key did not default it (%d)\n", cfg.analog_preroll_ms);
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_record_preroll_ms != DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS) {
        DSD_FPRINTF(stderr, "FAIL: a [recording] without the key kept %d\n", opts.analog_record_preroll_ms);
        rc |= 1;
    }
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    char want_default[64];
    DSD_SNPRINTF(want_default, sizeof want_default, "analog_preroll_ms = %d\n", DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS);
    rc |= expect_contains_quiet("analog_preroll_ms saved at the default", rendered, want_default);

    /* Both ends of the range load as given. */
    for (const int want : {0, 1000}) {
        char ini[96];
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\nanalog_preroll_ms = %d\n", want);
        if (load_config_text(ini, &cfg) != 0 || cfg.analog_preroll_ms != want) {
            DSD_FPRINTF(stderr, "FAIL: analog_preroll_ms = %d loaded as %d\n", want, cfg.analog_preroll_ms);
            rc |= 1;
        }
    }

    /* Each refused value follows a valid one in the same section, which stays, with a warning naming the key. */
    for (const char* value : {"-1", "1001", "2s", "1e3", "99999999999"}) {
        char ini[128];
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\nanalog_preroll_ms = 150\nanalog_preroll_ms = %s\n", value);
        g_log_text[0] = '\0';
        if (load_config_text(ini, &cfg) != 0 || cfg.analog_preroll_ms != 150) {
            DSD_FPRINTF(stderr, "FAIL: analog_preroll_ms = %s was not refused (%d)\n", value, cfg.analog_preroll_ms);
            rc |= 1;
        }
        if (strstr(g_log_text, "analog_preroll_ms") == nullptr) {
            DSD_FPRINTF(stderr, "FAIL: analog_preroll_ms = %s refused without a warning (log: '%s')\n", value,
                        g_log_text);
            rc |= 1;
        }
    }
    return rc;
}

/* Issue #668: [recording] rdio_analog_system_id and rdio_analog_talkgroup load, reach dsd_opts, and save back; 0 (the
   default) is not written, and loads as 0. A value outside the range, or not a whole number, is refused with a warning
   and the value already loaded stays: never clamped. */
int
test_recording_rdio_analog_roundtrip(void) {
    int rc = 0;
    dsdneoUserConfig cfg{};
    if (load_config_text("[recording]\nrdio_analog_system_id = 9\nrdio_analog_talkgroup = 2147483647\n", &cfg) != 0
        || cfg.rdio_analog_system_id != 9 || cfg.rdio_analog_talkgroup != 2147483647) {
        DSD_FPRINTF(stderr, "FAIL: rdio_analog_* did not load (%d/%d)\n", cfg.rdio_analog_system_id,
                    cfg.rdio_analog_talkgroup);
        return 1;
    }
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.rdio_analog_system_id != 9 || opts.rdio_analog_talkgroup != 2147483647) {
        DSD_FPRINTF(stderr, "FAIL: rdio_analog_* did not reach dsd_opts (%d/%d)\n", opts.rdio_analog_system_id,
                    opts.rdio_analog_talkgroup);
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("rdio_analog_system_id saved", rendered, "rdio_analog_system_id = 9\n");
    rc |= expect_contains_quiet("rdio_analog_talkgroup saved", rendered, "rdio_analog_talkgroup = 2147483647\n");
    dsdneoUserConfig reloaded{};
    if (load_config_text(rendered, &reloaded) != 0 || reloaded.rdio_analog_system_id != 9
        || reloaded.rdio_analog_talkgroup != 2147483647) {
        DSD_FPRINTF(stderr, "FAIL: the saved config did not load rdio_analog_* back\n");
        rc |= 1;
    }

    /* 0 resets either one, and a config at the defaults neither writes them nor loads anything but 0. */
    if (load_config_text("[recording]\nrdio_analog_system_id = 9\nrdio_analog_system_id = 0\n"
                         "rdio_analog_talkgroup = 4242\nrdio_analog_talkgroup = 0\n",
                         &cfg)
            != 0
        || cfg.rdio_analog_system_id != 0 || cfg.rdio_analog_talkgroup != 0) {
        DSD_FPRINTF(stderr, "FAIL: 0 did not reset rdio_analog_* (%d/%d)\n", cfg.rdio_analog_system_id,
                    cfg.rdio_analog_talkgroup);
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (opts.rdio_analog_system_id != 0 || opts.rdio_analog_talkgroup != 0
        || strstr(rendered, "rdio_analog_") != nullptr) {
        DSD_FPRINTF(stderr, "FAIL: rdio_analog_* at 0 reached dsd_opts or the saved config\n");
        rc |= 1;
    }

    /* Each refused value follows a valid one in the same section, which stays, with a warning naming the key. */
    static const struct {
        const char* key;
        const char* value;
    } refused[] = {
        {"rdio_analog_system_id", "-1"},         {"rdio_analog_system_id", "2147483648"},
        {"rdio_analog_system_id", "9x"},         {"rdio_analog_talkgroup", "-1"},
        {"rdio_analog_talkgroup", "2147483648"}, {"rdio_analog_talkgroup", "99999999999"},
        {"rdio_analog_talkgroup", "1e3"},
    };

    for (const auto& c : refused) {
        char ini[160];
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\n%s = 77\n%s = %s\n", c.key, c.key, c.value);
        g_log_text[0] = '\0';
        const int loaded = load_config_text(ini, &cfg);
        const int kept =
            strcmp(c.key, "rdio_analog_system_id") == 0 ? cfg.rdio_analog_system_id : cfg.rdio_analog_talkgroup;
        if (loaded != 0 || kept != 77 || strstr(g_log_text, c.key) == nullptr) {
            DSD_FPRINTF(stderr, "FAIL: %s = %s was not refused with a warning (%d; log: '%s')\n", c.key, c.value, kept,
                        g_log_text);
            rc |= 1;
        }
    }
    return rc;
}

static int
load_profile_text(const char* ini, const char* profile, dsdneoUserConfig* cfg) {
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    const int load_rc = dsd_user_config_load_profile(path, profile, cfg);
    (void)remove(path);
    return load_rc;
}

static int*
rdio_system_field(dsdneoUserConfig* cfg, const char* key) {
    return strcmp(key, "rdio_system_id") == 0 ? &cfg->rdio_system_id : &cfg->rdio_analog_system_id;
}

/* One rdio-scanner system ID key (issue #931): a value up to 2147483647 (154437, a kHz-sized one) loads, reaches
   dsd_opts and survives a save and a reload; 0 is not written. */
static int
rdio_system_id_key_roundtrip(const char* key) {
    int rc = 0;
    char ini[160];
    DSD_SNPRINTF(ini, sizeof ini, "[recording]\n%s = 154437\n", key);
    dsdneoUserConfig cfg{};
    if (load_config_text(ini, &cfg) != 0 || *rdio_system_field(&cfg, key) != 154437) {
        DSD_FPRINTF(stderr, "FAIL: %s = 154437 did not load (%d)\n", key, *rdio_system_field(&cfg, key));
        return 1;
    }
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    const int applied = strcmp(key, "rdio_system_id") == 0 ? opts.rdio_system_id : opts.rdio_analog_system_id;
    if (applied != 154437) {
        DSD_FPRINTF(stderr, "FAIL: %s did not reach dsd_opts (%d)\n", key, applied);
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    char want[64];
    DSD_SNPRINTF(want, sizeof want, "\n%s = 154437\n", key);
    rc |= expect_contains_quiet("rdio system ID saved", rendered, want);
    dsdneoUserConfig reloaded{};
    if (load_config_text(rendered, &reloaded) != 0 || *rdio_system_field(&reloaded, key) != 154437) {
        DSD_FPRINTF(stderr, "FAIL: the saved config did not load %s back\n", key);
        rc |= 1;
    }

    DSD_SNPRINTF(ini, sizeof ini, "[recording]\n%s = 154437\n%s = 0\n", key, key);
    if (load_config_text(ini, &cfg) != 0 || *rdio_system_field(&cfg, key) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s = 0 did not reset it\n", key);
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    DSD_SNPRINTF(want, sizeof want, "\n%s =", key);
    if (strstr(rendered, want) != nullptr) {
        DSD_FPRINTF(stderr, "FAIL: %s = 0 was written to the saved config\n", key);
        rc |= 1;
    }
    return rc;
}

/* Issue #931 (B3): [recording] rdio_system_id and rdio_analog_system_id take 0..2147483647. A value outside it, or not
   a whole number, is refused with a warning naming the key, and the value already loaded stays: rdio_system_id used to
   clamp to 65535 and turn text into 0, silently, so a bad profile value wiped the base file's. */
static int
test_recording_rdio_system_id_range(void) {
    int rc = rdio_system_id_key_roundtrip("rdio_system_id") | rdio_system_id_key_roundtrip("rdio_analog_system_id");
    for (const char* key : {"rdio_system_id", "rdio_analog_system_id"}) {
        for (const char* value : {"48x", "-1", "2147483648", "99999999999", "1e3", "\"\""}) {
            char ini[160];
            DSD_SNPRINTF(ini, sizeof ini, "[recording]\n%s = 77\n%s = %s\n", key, key, value);
            dsdneoUserConfig cfg{};
            g_log_text[0] = '\0';
            if (load_config_text(ini, &cfg) != 0 || *rdio_system_field(&cfg, key) != 77
                || strstr(g_log_text, key) == nullptr) {
                DSD_FPRINTF(stderr, "FAIL: %s = %s was not refused with a warning (%d; log: '%s')\n", key, value,
                            *rdio_system_field(&cfg, key), g_log_text);
                rc |= 1;
            }
            /* A profile's bad value over a nonzero base value keeps the base value. */
            DSD_SNPRINTF(ini, sizeof ini, "[recording]\n%s = 12\n\n[profile.p]\nrecording.%s = %s\n", key, key, value);
            dsdneoUserConfig prof{};
            if (load_profile_text(ini, "p", &prof) != 0 || *rdio_system_field(&prof, key) != 12) {
                DSD_FPRINTF(stderr, "FAIL: profile %s = %s over base 12 left %d\n", key, value,
                            *rdio_system_field(&prof, key));
                rc |= 1;
            }
        }
        char ini[160];
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\n%s = 12\n\n[profile.p]\nrecording.%s = 2147483647\n", key, key);
        dsdneoUserConfig prof{};
        if (load_profile_text(ini, "p", &prof) != 0 || *rdio_system_field(&prof, key) != 2147483647) {
            DSD_FPRINTF(stderr, "FAIL: profile %s = 2147483647 did not load (%d)\n", key,
                        *rdio_system_field(&prof, key));
            rc |= 1;
        }
    }
    return rc;
}

/* Issue #931 (B7): [recording] rdio_mode takes off|dirwatch|api|both in any case. An invalid value, or an empty one
   (which the parser reads as off), is refused with a warning and the mode already loaded stays, in a base file and in a
   profile over a valid base alike; --validate-config already reports both as errors. */
static int
test_recording_rdio_mode_refused(void) {
    int rc = 0;
    for (const char* value : {"apii", "dirwach", "\"\"", "\"api \""}) {
        char ini[160];
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\nrdio_mode = api\nrdio_mode = %s\n", value);
        dsdneoUserConfig cfg{};
        g_log_text[0] = '\0';
        if (load_config_text(ini, &cfg) != 0 || cfg.rdio_mode != DSD_RDIO_MODE_API
            || strstr(g_log_text, "rdio_mode") == nullptr) {
            DSD_FPRINTF(stderr, "FAIL: rdio_mode = %s was not refused with a warning (%d; log: '%s')\n", value,
                        cfg.rdio_mode, g_log_text);
            rc |= 1;
        }
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\nrdio_mode = api\n\n[profile.p]\nrecording.rdio_mode = %s\n", value);
        dsdneoUserConfig prof{};
        if (load_profile_text(ini, "p", &prof) != 0 || prof.rdio_mode != DSD_RDIO_MODE_API) {
            DSD_FPRINTF(stderr, "FAIL: profile rdio_mode = %s over base api left %d\n", value, prof.rdio_mode);
            rc |= 1;
        }
    }
    dsdneoUserConfig prof{};
    if (load_profile_text("[recording]\nrdio_mode = api\n\n[profile.p]\nrecording.rdio_mode = DirWatch\n", "p", &prof)
            != 0
        || prof.rdio_mode != DSD_RDIO_MODE_DIRWATCH) {
        DSD_FPRINTF(stderr, "FAIL: profile rdio_mode = DirWatch did not load (%d)\n", prof.rdio_mode);
        rc |= 1;
    }
    return rc;
}

/* Issue #931 (gate r1): a physical line longer than the reader's 8191 bytes is dropped whole, with a warning that
   quotes none of it. Its tail used to be read as a line of its own, so "rdio_system_id=91" past the cut of an
   oversized key line took effect. A line that just fits (8191 bytes, its newline after) still loads, and the lines
   after an oversized one do. */
static int
test_config_line_too_long_dropped_whole(void) {
    int rc = 0;
    const std::string prefix = "rdio_api_key = \"";
    const std::string oversized =
        prefix + "SECRETMARK" + std::string(8191U - prefix.size() - 10U, 'k') + "rdio_system_id=91\n";
    const std::string ini =
        std::string("[recording]\nrdio_system_id = 7\nrdio_api_key = \"kept\"\n") + oversized + "rdio_mode = \"api\"\n";
    dsdneoUserConfig cfg{};
    g_log_text[0] = '\0';
    if (load_config_text(ini.c_str(), &cfg) != 0 || cfg.rdio_system_id != 7 || strcmp(cfg.rdio_api_key, "kept") != 0
        || cfg.rdio_mode != DSD_RDIO_MODE_API || strstr(g_log_text, "longer than 8191 bytes") == nullptr
        || strstr(g_log_text, "SECRETMARK") != nullptr || strstr(g_log_text, "kkkk") != nullptr) {
        DSD_FPRINTF(stderr, "FAIL: oversized line: system=%d key=%.16s mode=%d (log: '%.200s')\n", cfg.rdio_system_id,
                    cfg.rdio_api_key, cfg.rdio_mode, g_log_text);
        rc |= 1;
    }
    /* 8191 bytes with the newline after them fit: a comment that long is a comment, and the next line loads. */
    const std::string fits = std::string("[recording]\n#") + std::string(8190U, 'c') + "\nrdio_system_id = 9\n";
    g_log_text[0] = '\0';
    if (load_config_text(fits.c_str(), &cfg) != 0 || cfg.rdio_system_id != 9
        || strstr(g_log_text, "longer") != nullptr) {
        DSD_FPRINTF(stderr, "FAIL: an 8191-byte line was not read as one (system=%d, log: '%.200s')\n",
                    cfg.rdio_system_id, g_log_text);
        rc |= 1;
    }
    /* A profile load reads the same way: the tail of an oversized profile line sets nothing. */
    const std::string profile = std::string("[recording]\nrdio_system_id = 7\n\n[profile.p]\n") + "recording." + prefix
                                + std::string(8191U - prefix.size() - 10U, 'k') + "recording.rdio_system_id=91\n";
    dsdneoUserConfig prof{};
    if (load_profile_text(profile.c_str(), "p", &prof) != 0 || prof.rdio_system_id != 7) {
        DSD_FPRINTF(stderr, "FAIL: an oversized profile line's tail took effect (system=%d)\n", prof.rdio_system_id);
        rc |= 1;
    }
    return rc;
}

/* @p ini (any bytes, NULs included) written to a temporary config and loaded. */
static int
load_config_bytes(const std::string& ini, dsdneoUserConfig* cfg) {
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_config_user_bytes");
    if (fd < 0) {
        return 1;
    }
    const ssize_t wr = dsd_write(fd, ini.data(), ini.size());
    (void)dsd_close(fd);
    const int load_rc = (wr >= 0 && (size_t)wr == ini.size()) ? dsd_user_config_load(path, cfg) : 1;
    (void)remove(path);
    return load_rc;
}

/* Issue #931 (gate r2): the reader counts the bytes it reads. A line holding a NUL is one invalid line, consumed whole:
   neither the part before the NUL nor, in an oversized line, the bytes past the buffer are read as a setting, and the
   warning quotes none of it. A line of exactly 8191 bytes loads the same with LF and with CRLF after it. */
static int
test_config_line_nul_and_crlf(void) {
    int rc = 0;
    const std::string base = "[recording]\nrdio_system_id = 7\nrdio_api_key = \"kept\"\n";
    std::string nul_long = "rdio_api_key = \"SECRETMARK";
    nul_long.push_back('\0');
    nul_long += std::string(9000U, 'k') + "rdio_system_id=91\n";
    std::string nul_short = "rdio_system_id = 5";
    nul_short.push_back('\0');
    nul_short += "SECRETMARK\n";
    for (const std::string& bad : {nul_long, nul_short}) {
        dsdneoUserConfig cfg{};
        g_log_text[0] = '\0';
        if (load_config_bytes(base + bad + "rdio_mode = \"api\"\n", &cfg) != 0 || cfg.rdio_system_id != 7
            || strcmp(cfg.rdio_api_key, "kept") != 0 || cfg.rdio_mode != DSD_RDIO_MODE_API
            || strstr(g_log_text, "NUL byte") == nullptr || strstr(g_log_text, "SECRETMARK") != nullptr) {
            DSD_FPRINTF(stderr, "FAIL: a line with a NUL (%zu bytes): system=%d key=%.16s mode=%d (log: '%.200s')\n",
                        bad.size(), cfg.rdio_system_id, cfg.rdio_api_key, cfg.rdio_mode, g_log_text);
            rc |= 1;
        }
    }
    std::string max_line = "rdio_system_id = 9 #";
    max_line += std::string(8191U - max_line.size(), 'c');
    for (const char* eol : {"\n", "\r\n"}) {
        dsdneoUserConfig cfg{};
        g_log_text[0] = '\0';
        const std::string ini = std::string("[recording]") + eol + max_line + eol + "rdio_mode = \"api\"" + eol;
        if (load_config_bytes(ini, &cfg) != 0 || cfg.rdio_system_id != 9 || cfg.rdio_mode != DSD_RDIO_MODE_API
            || g_log_text[0] != '\0') {
            DSD_FPRINTF(stderr, "FAIL: an 8191-byte line ending %s: system=%d mode=%d (log: '%.200s')\n",
                        eol[0] == '\r' ? "CRLF" : "LF", cfg.rdio_system_id, cfg.rdio_mode, g_log_text);
            rc |= 1;
        }
    }
    return rc;
}

#if defined(DSD_TEST_FAULT_READS)
// IWYU pragma: no_include <bits/types/cookie_io_functions_t.h>

/* Issue #931 (gate r3): a stream that serves g_fault_content and then, from the g_fault_from_open-th open on (1-based),
   fails with EIO instead of ending; earlier opens end cleanly after it. With g_fault_path set, only an open of a path
   holding it is faulted and every other open is the real one. Off unless g_fault_reads. */
static int g_fault_reads;
static std::string g_fault_path;
static int g_fault_from_open;
static int g_fault_opens;
static int g_fault_hits;
static std::string g_fault_content;

namespace {
struct fault_stream {
    size_t offset;
    int faults;
};
} // namespace

static ssize_t
fault_read(void* cookie, char* buf, size_t size) {
    fault_stream* fs = static_cast<fault_stream*>(cookie);
    if (fs->offset >= g_fault_content.size()) {
        if (!fs->faults) {
            return 0;
        }
        g_fault_hits++;
        errno = EIO;
        return -1;
    }
    size_t n = g_fault_content.size() - fs->offset;
    if (n > size) {
        n = size;
    }
    DSD_MEMCPY(buf, g_fault_content.data() + fs->offset, n);
    fs->offset += n;
    return (ssize_t)n;
}

/* The validator rewinds the stream before it reads: only a rewind is needed. */
static int
fault_seek(void* cookie, off64_t* pos, int whence) {
    if (whence != SEEK_SET || *pos < 0 || (size_t)*pos > g_fault_content.size()) {
        errno = EINVAL;
        return -1;
    }
    static_cast<fault_stream*>(cookie)->offset = (size_t)*pos;
    return 0;
}

static int
fault_close(void* cookie) {
    delete static_cast<fault_stream*>(cookie);
    return 0;
}

// GNU ld --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
extern "C" FILE* __real_dsd_path_fopen_user_read_file(const char* requested, char* out, size_t out_size);
extern "C" FILE* __wrap_dsd_path_fopen_user_read_file(const char* requested, char* out, size_t out_size);

extern "C" FILE*
__wrap_dsd_path_fopen_user_read_file(const char* requested, char* out, size_t out_size) {
    if (!g_fault_reads || (!g_fault_path.empty() && (!requested || !strstr(requested, g_fault_path.c_str())))) {
        return __real_dsd_path_fopen_user_read_file(requested, out, out_size);
    }
    if (out && out_size > 0) {
        DSD_SNPRINTF(out, out_size, "%s", requested ? requested : "");
    }
    fault_stream* fs = new fault_stream{0U, ++g_fault_opens >= g_fault_from_open ? 1 : 0};
    cookie_io_functions_t io{};
    io.read = fault_read;
    io.seek = fault_seek;
    io.close = fault_close;
    return fopencookie(fs, "r", io);
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

static void
fault_arm(const char* content, int from_open) {
    g_fault_path.clear();
    g_fault_content = content;
    g_fault_from_open = from_open;
    g_fault_opens = 0;
    g_fault_hits = 0;
    g_fault_reads = 1;
}

/* Issue #931 (gate r3, r3fix): one rule for read errors. A read error in any file of a load (the root, an include, a
   profile overlay) fails the whole load with DSD_USER_CONFIG_READ_ERROR: the caller's config is zeroed (config.h),
   holding nothing of the files, not even the complete lines before the error, and the failing file is named once. A
   partial line ("rdio_system_id = 154" then EIO) is never applied. The profile list fails on a read error even after
   its output is full, and --validate-config reports a read error, a failed profile listing included. */
static int
test_config_read_error_mid_line(void) {
    int rc = 0;
    static const char k_partial[] = "[recording]\nrdio_system_id = 154";
    dsdneoUserConfig cfg{};
    fault_arm(k_partial, 99);
    int load_rc = dsd_user_config_load("faulted.ini", &cfg);
    g_fault_reads = 0;
    if (load_rc != 0 || cfg.rdio_system_id != 154) {
        DSD_FPRINTF(stderr, "FAIL: a clean end after the bytes: rc=%d system=%d\n", load_rc, cfg.rdio_system_id);
        rc |= 1;
    }
    /* A complete line, then EIO, on the include pass (open 1) or the main pass (open 2): nothing of it is kept. */
    for (int from_open : {1, 2}) {
        g_log_text[0] = '\0';
        cfg.rdio_system_id = 5;
        fault_arm("[recording]\nrdio_system_id = 77\n", from_open);
        load_rc = dsd_user_config_load("faulted.ini", &cfg);
        g_fault_reads = 0;
        if (load_rc != DSD_USER_CONFIG_READ_ERROR || cfg.rdio_system_id != 0 || g_fault_hits == 0
            || strstr(g_log_text, "a read error in faulted.ini") == nullptr) {
            DSD_FPRINTF(stderr, "FAIL: a read error from open %d: rc=%d system=%d hits=%d (log: '%s')\n", from_open,
                        load_rc, cfg.rdio_system_id, g_fault_hits, g_log_text);
            rc |= 1;
        }
    }
    fault_arm(k_partial, 2);
    load_rc = dsd_user_config_load("faulted.ini", &cfg);
    g_fault_reads = 0;
    if (load_rc != DSD_USER_CONFIG_READ_ERROR || cfg.rdio_system_id == 154) {
        DSD_FPRINTF(stderr, "FAIL: a partial line before EIO: rc=%d system=%d\n", load_rc, cfg.rdio_system_id);
        rc |= 1;
    }
    /* The profile overlay is the third open (include pass, base pass, overlay): the profile load fails as a read
       error, not a missing profile, and keeps none of the base's values. */
    g_log_text[0] = '\0';
    fault_arm("[recording]\nrdio_system_id = 12\n\n[profile.p]\nrecording.rdio_system_id = 154", 3);
    load_rc = dsd_user_config_load_profile("faulted.ini", "p", &cfg);
    g_fault_reads = 0;
    if (load_rc != DSD_USER_CONFIG_READ_ERROR || cfg.rdio_system_id != 0 || g_fault_hits == 0) {
        DSD_FPRINTF(stderr, "FAIL: a read error in the profile overlay: rc=%d system=%d\n", load_rc,
                    cfg.rdio_system_id);
        rc |= 1;
    }
    /* An include whose read fails fails the whole load, naming the include. */
    char child[DSD_TEST_PATH_MAX];
    char root[DSD_TEST_PATH_MAX];
    if (write_temp_config("[recording]\nrdio_unknown_talkgroup = 3\n", child, sizeof child) != 0) {
        return 1;
    }
    const std::string root_ini = std::string("include = \"") + child + "\"\n[recording]\nrdio_system_id = 7\n";
    if (write_temp_config(root_ini.c_str(), root, sizeof root) != 0) {
        (void)remove(child);
        return 1;
    }
    g_log_text[0] = '\0';
    fault_arm("[recording]\nrdio_unknown_talkgroup = 3\n", 1);
    g_fault_path = child;
    load_rc = dsd_user_config_load(root, &cfg);
    g_fault_reads = 0;
    g_fault_path.clear();
    const std::string want_child = std::string("a read error in ") + child;
    if (load_rc != DSD_USER_CONFIG_READ_ERROR || cfg.rdio_system_id != 0 || cfg.rdio_unknown_talkgroup != 0
        || strstr(g_log_text, want_child.c_str()) == nullptr) {
        DSD_FPRINTF(stderr, "FAIL: an include's read error: rc=%d system=%d (log: '%s')\n", load_rc, cfg.rdio_system_id,
                    g_log_text);
        rc |= 1;
    }
    (void)remove(root);
    (void)remove(child);
    /* The list fails on a read error even when one name fills it first. */
    const char* names[4] = {nullptr};
    char names_buf[256];
    fault_arm("[profile.a]\nx = 1\n[profile.b]\n", 1);
    const int listed = dsd_user_config_list_profiles("faulted.ini", names, names_buf, sizeof names_buf, 1);
    g_fault_reads = 0;
    if (listed != -1) {
        DSD_FPRINTF(stderr, "FAIL: a read error after a full profile list listed %d\n", listed);
        rc |= 1;
    }

    /* --validate-config: a read error in the file it checks, and one in the profile listing of the composed checks
       (opens: syntax pass 1, composed include pass 2, base 3, the list 4). */
    static const struct {
        const char* content;
        int from_open;
        const char* want;
    } validations[] = {
        {k_partial, 1, "Read error; the file cannot be loaded"},
        {"[recording]\nrdio_system_id = 7\n[profile.a]\n", 4, "Read error; the profiles could not be listed"},
    };

    for (const auto& v : validations) {
        dsdcfg_diagnostics_t diags;
        DSD_MEMSET(&diags, 0, sizeof(diags));
        fault_arm(v.content, v.from_open);
        const int validated = dsd_user_config_validate("faulted.ini", &diags);
        g_fault_reads = 0;
        int read_errors = 0;
        for (int i = 0; i < diags.count; i++) {
            read_errors += diags.items[i].level == DSDCFG_DIAG_ERROR && strstr(diags.items[i].message, v.want);
        }
        if (validated == 0 || read_errors != 1) {
            DSD_FPRINTF(stderr, "FAIL: --validate-config, %s: rc=%d errors=%d\n", v.want, validated, read_errors);
            for (int i = 0; i < diags.count; i++) {
                DSD_FPRINTF(stderr, "  %s\n", diags.items[i].message);
            }
            rc |= 1;
        }
        dsdcfg_diags_free(&diags);
    }
    return rc;
}
#endif

/* Issue #931 (gate r3): a refused [recording] number is never quoted. A credential put in rdio_system_id by mistake
   (a generated config) is warned about by key, range and the value kept, and is printed nowhere. */
static int
test_recording_refused_number_not_quoted(void) {
    int rc = 0;

    static const struct {
        const char* key;
        const char* want;
    } cases[] = {
        {"rdio_system_id", "Config: invalid rdio_system_id (expected 0 to 2147483647); keeping 77\n"},
        {"rdio_analog_system_id", "Config: invalid rdio_analog_system_id (expected 0 to 2147483647); keeping 77\n"},
        {"rdio_analog_talkgroup", "Config: invalid rdio_analog_talkgroup (expected 0 to 2147483647); keeping 77\n"},
        {"rdio_unknown_talkgroup", "Config: invalid rdio_unknown_talkgroup (expected 0 to 2147483647); keeping 77\n"},
        {"analog_preroll_ms", "Config: invalid analog_preroll_ms (expected 0 to 1000); keeping 77\n"},
        {"analog_max_gap_ms", "Config: invalid analog_max_gap_ms (expected 0 to 10000); keeping 77\n"},
    };

    for (const auto& c : cases) {
        char ini[256];
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\n%s = 77\n%s = \"https://user:SECRETMARK@host.invalid/\"\n", c.key,
                     c.key);
        dsdneoUserConfig cfg{};
        g_log_text[0] = '\0';
        if (load_config_text(ini, &cfg) != 0 || strstr(g_log_text, c.want) == nullptr
            || strstr(g_log_text, "SECRETMARK") != nullptr) {
            DSD_FPRINTF(stderr, "FAIL: %s refused a credential as: '%s'\n", c.key, g_log_text);
            rc |= 1;
        }
    }
    return rc;
}

/* Issue #931 (B11): [recording] rdio_api_url (1023 bytes at most) and rdio_api_key (255) load the largest value that
   fits whole; one byte more is refused with a warning naming the key, never the value, and the value already loaded
   stays, instead of a value cut to another endpoint or key. */
static int
test_recording_rdio_credentials_too_long(void) {
    static const struct {
        const char* key;
        size_t capacity;
    } fields[] = {
        {"rdio_api_url", sizeof(dsdneoUserConfig::rdio_api_url)},
        {"rdio_api_key", sizeof(dsdneoUserConfig::rdio_api_key)},
    };

    int rc = 0;
    for (const auto& field : fields) {
        for (size_t len = field.capacity - 1U; len <= field.capacity; len++) {
            std::string value(len, 'q');
            value.replace(0, 10, "SECRETMARK");
            const std::string ini =
                std::string("[recording]\n") + field.key + " = \"kept\"\n" + field.key + " = \"" + value + "\"\n";
            dsdneoUserConfig cfg{};
            g_log_text[0] = '\0';
            const int loaded = load_config_text(ini.c_str(), &cfg);
            const char* got = strcmp(field.key, "rdio_api_url") == 0 ? cfg.rdio_api_url : cfg.rdio_api_key;
            const bool fits = len < field.capacity;
            const bool ok =
                loaded == 0
                && (fits ? value == got : strcmp(got, "kept") == 0 && strstr(g_log_text, field.key) != nullptr);
            if (!ok || strstr(g_log_text, "SECRETMARK") != nullptr) {
                DSD_FPRINTF(stderr, "FAIL: %s of %zu bytes: loaded %zu bytes (log: '%s')\n", field.key, len,
                            strlen(got), g_log_text);
                rc |= 1;
            }
        }
    }
    return rc;
}

/* Issue #888: [recording] rdio_unknown_talkgroup loads, reaches dsd_opts, and saves back; 0 (the default) is not
   written, and loads as 0. A value outside 0..2147483647, or not a whole number, is refused with a warning and the
   value already loaded stays: never clamped. */
int
test_recording_rdio_unknown_talkgroup_roundtrip(void) {
    int rc = 0;
    dsdneoUserConfig cfg{};
    if (load_config_text("[recording]\nrdio_unknown_talkgroup = 2147483647\n", &cfg) != 0
        || cfg.rdio_unknown_talkgroup != 2147483647) {
        DSD_FPRINTF(stderr, "FAIL: rdio_unknown_talkgroup did not load (%d)\n", cfg.rdio_unknown_talkgroup);
        return 1;
    }
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    if (opts.rdio_unknown_talkgroup != 0) {
        DSD_FPRINTF(stderr, "FAIL: rdio_unknown_talkgroup defaults to %d, want 0\n", opts.rdio_unknown_talkgroup);
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.rdio_unknown_talkgroup != 2147483647) {
        DSD_FPRINTF(stderr, "FAIL: rdio_unknown_talkgroup did not reach dsd_opts (%d)\n", opts.rdio_unknown_talkgroup);
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("rdio_unknown_talkgroup saved", rendered, "rdio_unknown_talkgroup = 2147483647\n");
    dsdneoUserConfig reloaded{};
    if (load_config_text(rendered, &reloaded) != 0 || reloaded.rdio_unknown_talkgroup != 2147483647) {
        DSD_FPRINTF(stderr, "FAIL: the saved config did not load rdio_unknown_talkgroup back\n");
        rc |= 1;
    }

    /* 0 resets it, and a config at the default neither writes it nor loads anything but 0. */
    if (load_config_text("[recording]\nrdio_unknown_talkgroup = 4242\nrdio_unknown_talkgroup = 0\n", &cfg) != 0
        || cfg.rdio_unknown_talkgroup != 0) {
        DSD_FPRINTF(stderr, "FAIL: 0 did not reset rdio_unknown_talkgroup (%d)\n", cfg.rdio_unknown_talkgroup);
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (opts.rdio_unknown_talkgroup != 0 || strstr(rendered, "rdio_unknown_talkgroup") != nullptr) {
        DSD_FPRINTF(stderr, "FAIL: rdio_unknown_talkgroup at 0 reached dsd_opts or the saved config\n");
        rc |= 1;
    }

    /* Each refused value follows a valid one in the same section, which stays, with a warning naming the key. */
    static const char* const refused[] = {"-1", "2147483648", "99999999999", "1e3", "9x"};
    for (const char* value : refused) {
        char ini[160];
        DSD_SNPRINTF(ini, sizeof ini, "[recording]\nrdio_unknown_talkgroup = 77\nrdio_unknown_talkgroup = %s\n", value);
        g_log_text[0] = '\0';
        const int loaded = load_config_text(ini, &cfg);
        if (loaded != 0 || cfg.rdio_unknown_talkgroup != 77
            || strstr(g_log_text, "rdio_unknown_talkgroup") == nullptr) {
            DSD_FPRINTF(stderr, "FAIL: rdio_unknown_talkgroup = %s was not refused with a warning (%d; log: '%s')\n",
                        value, cfg.rdio_unknown_talkgroup, g_log_text);
            rc |= 1;
        }
    }
    return rc;
}

/* Issue #931: [recording] rdio_system_by_frequency loads as a bool, reaches dsd_opts, saves back as true, and loads
   again; false (the default) is not written. A value that is not a bool keeps the one already loaded. */
int
test_recording_rdio_system_by_frequency_roundtrip(void) {
    int rc = 0;
    dsdneoUserConfig cfg{};
    if (load_config_text("[recording]\nrdio_system_by_frequency = true\n", &cfg) != 0
        || cfg.rdio_system_by_frequency != 1) {
        DSD_FPRINTF(stderr, "FAIL: rdio_system_by_frequency did not load (%d)\n", cfg.rdio_system_by_frequency);
        return 1;
    }
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    if (opts.rdio_system_by_frequency != 0) {
        DSD_FPRINTF(stderr, "FAIL: rdio_system_by_frequency defaults to %d, want 0\n", opts.rdio_system_by_frequency);
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.rdio_system_by_frequency != 1) {
        DSD_FPRINTF(stderr, "FAIL: rdio_system_by_frequency did not reach dsd_opts (%d)\n",
                    opts.rdio_system_by_frequency);
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("rdio_system_by_frequency saved", rendered, "rdio_system_by_frequency = true\n");
    dsdneoUserConfig reloaded{};
    if (load_config_text(rendered, &reloaded) != 0 || reloaded.rdio_system_by_frequency != 1) {
        DSD_FPRINTF(stderr, "FAIL: the saved config did not load rdio_system_by_frequency back\n");
        rc |= 1;
    }

    /* false turns it off, and a config at the default neither writes it nor loads anything but off. */
    if (load_config_text("[recording]\nrdio_system_by_frequency = true\nrdio_system_by_frequency = false\n", &cfg) != 0
        || cfg.rdio_system_by_frequency != 0) {
        DSD_FPRINTF(stderr, "FAIL: false did not turn rdio_system_by_frequency off (%d)\n",
                    cfg.rdio_system_by_frequency);
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (opts.rdio_system_by_frequency != 0 || strstr(rendered, "rdio_system_by_frequency") != nullptr) {
        DSD_FPRINTF(stderr, "FAIL: rdio_system_by_frequency off reached dsd_opts or the saved config\n");
        rc |= 1;
    }

    /* Not a bool: the value already loaded stays. */
    if (load_config_text("[recording]\nrdio_system_by_frequency = true\nrdio_system_by_frequency = maybe\n", &cfg) != 0
        || cfg.rdio_system_by_frequency != 1) {
        DSD_FPRINTF(stderr, "FAIL: rdio_system_by_frequency = maybe changed the value (%d)\n",
                    cfg.rdio_system_by_frequency);
        rc |= 1;
    }
    return rc;
}
} // namespace

/*
 * Issue #525: [analog] nfm_bandwidth_hz rides INI -> cfg -> opts -> snapshot -> render. 0 in the
 * options is the default, not a request, so a save writes the key only for an explicit width -- an
 * explicit 16000 included, since it forces the channel filter on where the default would not. The
 * section itself is always saved, so a config saved at the default loads back as the default over a
 * session's explicit width. An invalid value is refused by the loader, which keeps the default.
 */
static int
test_analog_nfm_bandwidth_roundtrip(void) {
    int rc = 0;
    dsdneoUserConfig cfg;
    if (load_config_text("[analog]\nnfm_bandwidth_hz = 12500\n", &cfg) != 0) {
        DSD_FPRINTF(stderr, "analog config failed to load\n");
        return 1;
    }
    if (!cfg.has_analog || cfg.analog_nfm_bandwidth_hz != 12500) {
        DSD_FPRINTF(stderr, "FAIL: [analog] nfm_bandwidth_hz did not load, got has=%d width=%d\n", cfg.has_analog,
                    cfg.analog_nfm_bandwidth_hz);
        rc |= 1;
    }

    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_nfm_bandwidth_hz != 12500) {
        DSD_FPRINTF(stderr, "FAIL: nfm_bandwidth_hz did not reach dsd_opts, got %d\n", opts.analog_nfm_bandwidth_hz);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("explicit NFM width", rendered, "[analog]\nnfm_bandwidth_hz = 12500\n");

    /* The explicit width equal to the default is still a request, and is saved as one. */
    opts.analog_nfm_bandwidth_hz = 16000;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("explicit default-valued NFM width", rendered, "nfm_bandwidth_hz = 16000\n");

    /* The default's key is not saved, so a later default change reaches this config, but its section is: loading the
       file into a session with an explicit width puts the default back rather than keeping that width. */
    reset_opts_and_state(opts, state);
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "nfm_bandwidth_hz") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: the default NFM width was saved:\n%s\n", rendered);
        rc |= 1;
    }
    rc |= expect_contains("the section of a default width", rendered, "[analog]\n\n");
    if (load_config_text(rendered, &cfg) != 0 || !cfg.has_analog || cfg.analog_nfm_bandwidth_hz != 0) {
        DSD_FPRINTF(stderr, "FAIL: a saved default did not load back as a present [analog] (has=%d width=%d)\n",
                    cfg.has_analog, cfg.analog_nfm_bandwidth_hz);
        rc |= 1;
    }
    opts.analog_nfm_bandwidth_hz = 12500;
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_nfm_bandwidth_hz != 0) {
        DSD_FPRINTF(stderr, "FAIL: loading a config saved at the default kept width %d\n",
                    opts.analog_nfm_bandwidth_hz);
        rc |= 1;
    }

    /* Refused, not clamped: the loader keeps the default. */
    const char* invalid[] = {"[analog]\nnfm_bandwidth_hz = 30000\n", "[analog]\nnfm_bandwidth_hz = 12.5k\n",
                             "[analog]\nnfm_bandwidth_hz = 0\n"};
    for (const char* ini : invalid) {
        if (load_config_text(ini, &cfg) != 0 || cfg.analog_nfm_bandwidth_hz != 0) {
            DSD_FPRINTF(stderr, "FAIL: invalid width was not refused (width %d) for:\n%s", cfg.analog_nfm_bandwidth_hz,
                        ini);
            rc |= 1;
        }
    }

    /* A config without [analog] leaves the configured width alone; one whose width the loader refused sets the
       default, as a config naming no width in that section would. */
    reset_opts_and_state(opts, state);
    opts.analog_nfm_bandwidth_hz = 20000;
    if (load_config_text("[mode]\ndecode = \"analog\"\n", &cfg) != 0) {
        return 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_nfm_bandwidth_hz != 20000) {
        DSD_FPRINTF(stderr, "FAIL: a config without [analog] changed the width to %d\n", opts.analog_nfm_bandwidth_hz);
        rc |= 1;
    }
    if (load_config_text("[analog]\nnfm_bandwidth_hz = 30000\n", &cfg) != 0) {
        return 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.analog_nfm_bandwidth_hz != 0) {
        DSD_FPRINTF(stderr, "FAIL: an [analog] section with a refused width left width %d\n",
                    opts.analog_nfm_bandwidth_hz);
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/* One dsd_user_config_radio_input_spec() case: @p want NULL expects -1 and @p out left for the caller to ignore. */
static int
expect_radio_input_spec(const char* label, const dsdneoUserConfig* cfg, const dsd_opts* opts, const char* want) {
    char spec[256];
    spec[0] = '\0';
    const int got_rc = dsd_user_config_radio_input_spec(cfg, opts, spec, sizeof spec);
    if (!want) {
        if (got_rc != -1) {
            DSD_FPRINTF(stderr, "FAIL: %s: expected no radio input spec, got rc=%d \"%s\"\n", label, got_rc, spec);
            return 1;
        }
        return 0;
    }
    if (got_rc != 0 || strcmp(spec, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: got rc=%d \"%s\", want \"%s\"\n", label, got_rc, spec, want);
        return 1;
    }
    return 0;
}

/*
 * Issue #525: the radio input spec a config's [input] builds, without applying it, which a runtime config apply
 * compares with the running input to know whether it reopens the device (and what then sets the DSP rate). It is the
 * spec the apply writes: an rtl source needs rtl_freq, rtl_tcp needs a host and adds the tuning only with rtl_freq, the
 * values the config leaves out come from the options, a SoapySDR source carries its args, an Airspy source is
 * "airspy", and a non-radio source builds none.
 */
static int
test_radio_input_spec(void) {
    int rc = 0;
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    dsd_opts& opts = *opts_storage;
    opts.rtl_gain_value = 30;
    opts.rtlsdr_ppm_error = 4;
    opts.rtl_dsp_bw_khz = 24;
    opts.rtl_volume_multiplier = 1;

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 1;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "146.52M");
    cfg.rtl_gain = 22;
    cfg.rtl_ppm_is_set = 1;
    cfg.rtl_ppm = -3;
    cfg.rtl_bw_khz = 16;
    cfg.rtl_sql = -50;
    cfg.rtl_volume = 2;
    rc |= expect_radio_input_spec("no [input]", &cfg, &opts, NULL);
    cfg.has_input = 1;
    rc |= expect_radio_input_spec("rtl with rtl_freq", &cfg, &opts, "rtl:1:146.52M:22:-3:16:-50:2");
    /* What the config leaves out is the session's, as the apply takes it. */
    cfg.rtl_gain = 0;
    cfg.rtl_ppm_is_set = 0;
    cfg.rtl_bw_khz = 0;
    cfg.rtl_volume = 0;
    rc |= expect_radio_input_spec("rtl from the session's tuning", &cfg, &opts, "rtl:1:146.52M:30:4:24:-50:1");
    /* No rtl_freq: the apply leaves the input as it is. */
    cfg.rtl_freq[0] = '\0';
    rc |= expect_radio_input_spec("rtl without rtl_freq", &cfg, &opts, NULL);

    cfg.input_source = DSDCFG_INPUT_RTLTCP;
    DSD_SNPRINTF(cfg.rtltcp_host, sizeof cfg.rtltcp_host, "%s", "127.0.0.1");
    rc |= expect_radio_input_spec("rtltcp without rtl_freq", &cfg, &opts, "rtltcp:127.0.0.1:1234");
    cfg.rtltcp_port = 1235;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "851.375M");
    cfg.rtl_bw_khz = 48;
    rc |= expect_radio_input_spec("rtltcp with rtl_freq", &cfg, &opts, "rtltcp:127.0.0.1:1235:851.375M:30:4:48:-50:1");
    cfg.rtltcp_host[0] = '\0';
    rc |= expect_radio_input_spec("rtltcp without a host", &cfg, &opts, NULL);

    /* The spec is what the apply writes to the input, so a caller compares it with the running one. */
    cfg.input_source = DSDCFG_INPUT_SOAPY;
    rc |= expect_radio_input_spec("soapy without args", &cfg, &opts, "soapy");
    DSD_SNPRINTF(cfg.soapy_args, sizeof cfg.soapy_args, "%s", "driver=airspy");
    rc |= expect_radio_input_spec("soapy with args", &cfg, &opts, "soapy:driver=airspy");
    {
        static dsd_opts applied;
        static dsd_state state;
        reset_opts_and_state(applied, state);
        dsd_apply_user_config_to_opts(&cfg, &applied, &state);
        rc |= expect_radio_input_spec("soapy spec is the applied input", &cfg, &opts, applied.audio_in_dev);
        dsd_state_ext_free_all(&state);
    }
    cfg.input_source = DSDCFG_INPUT_AIRSPY;
    rc |= expect_radio_input_spec("airspy", &cfg, &opts, "airspy");

    static const dsdneoUserInputSource k_other_sources[] = {
        DSDCFG_INPUT_UNSET, DSDCFG_INPUT_PULSE, DSDCFG_INPUT_FILE, DSDCFG_INPUT_TCP, DSDCFG_INPUT_UDP,
    };
    for (dsdneoUserInputSource source : k_other_sources) {
        cfg.input_source = source;
        char label[48];
        DSD_SNPRINTF(label, sizeof label, "source %d builds no radio spec", static_cast<int>(source));
        rc |= expect_radio_input_spec(label, &cfg, &opts, NULL);
    }

    cfg.input_source = DSDCFG_INPUT_RTL;
    char spec[64];
    rc |= (dsd_user_config_radio_input_spec(NULL, &opts, spec, sizeof spec) == -1) ? 0 : 1;
    rc |= (dsd_user_config_radio_input_spec(&cfg, NULL, spec, sizeof spec) == -1) ? 0 : 1;
    rc |= (dsd_user_config_radio_input_spec(&cfg, &opts, NULL, sizeof spec) == -1) ? 0 : 1;
    rc |= (dsd_user_config_radio_input_spec(&cfg, &opts, spec, 0U) == -1) ? 0 : 1;
    if (rc) {
        DSD_FPRINTF(stderr, "FAIL: dsd_user_config_radio_input_spec cases\n");
    }
    return rc;
}

/*
 * Issue #521: a row's --squelch-db is effective state, not a user default. Config->Save taken
 * while the row is on air writes the configured rtl_sql -- for every radio input family that
 * saves the shared tuning -- and an explicit per-row "off" never becomes the saved default.
 */
static int
test_squelch_snapshot_uses_configured_not_row_override(void) {
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    const char* inputs[] = {"rtl:0:851.375M:22:-2:24:-80:2", "rtltcp:127.0.0.1:1234:851.375M:22:-2:24:-80:2",
                            "soapy:driver=test", "airspy"};
    int rc = 0;
    for (const char* input : inputs) {
        static const int row_dbs[] = {-60, 0};
        for (int row_db : row_dbs) {
            reset_opts_and_state(opts, state);
            DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", input);
            opts.rtlsdr_center_freq = 851375000U;
            opts.rtl_dsp_bw_khz = 24;
            opts.rtl_squelch_level = dsd_squelch_level_from_sql(-80.0); /* the configured default */
            if (dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) != 0) {
                DSD_FPRINTF(stderr, "FAIL: could not open a scan scope\n");
                return 1;
            }
            dsd_scan_option_values row;
            DSD_MEMSET(&row, 0, sizeof row);
            row.present = DSD_SCAN_OPT_SQUELCH;
            row.squelch_db = row_db;
            (void)dsd_scan_mode_options(&opts, &state, &row);
            const double row_level = dsd_squelch_level_from_sql((double)row_db);
            if (fabs(opts.rtl_squelch_level - row_level) > 1e-9 * fmax(row_level, 1e-12)) {
                DSD_FPRINTF(stderr, "FAIL: %s row squelch %d dB did not reach dsd_opts\n", input, row_db);
                rc |= 1;
            }
            dsdneoUserConfig snap;
            dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
            if (snap.rtl_sql != -80) {
                DSD_FPRINTF(stderr, "FAIL: %s save during a %d dB row wrote rtl_sql %d, want the default -80\n", input,
                            row_db, snap.rtl_sql);
                rc |= 1;
            }
            dsd_scan_mode_leave(&opts, &state);
            if (fabs(opts.rtl_squelch_level - dsd_squelch_level_from_sql(-80.0)) > 1e-18) {
                DSD_FPRINTF(stderr, "FAIL: %s leaving the row did not restore the default\n", input);
                rc |= 1;
            }
            dsd_state_ext_free_all(&state);
        }
    }
    return rc;
}

/*
 * Issue #526: an nfm row's analog class and its --nfm-bandwidth-hz are effective state, not user defaults. Config->Save
 * taken while such a row is on air keeps the configured decode mode and writes the row's width nowhere, and leaving
 * the row puts the configured decoder and width back. The saved [analog] nfm_bandwidth_hz is the configured one, from
 * the configured view, never the row's.
 */
static int
test_nfm_row_snapshot_keeps_configured_mode_and_width(void) {
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:851.375M:22:-2:24:-80:2");
    opts.rtlsdr_center_freq = 851375000U;
    opts.rtl_dsp_bw_khz = 24;
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != 0
        || dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not open an nfm scan scope\n");
        return 1;
    }
    int rc = 0;
    dsd_scan_option_values row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 12500;
    (void)dsd_scan_mode_options(&opts, &state, &row);
    if (opts.analog_only != 1 || opts.analog_nfm_bandwidth_hz != 12500) {
        DSD_FPRINTF(stderr, "FAIL: nfm row did not reach dsd_opts (analog_only=%d width=%d)\n", opts.analog_only,
                    opts.analog_nfm_bandwidth_hz);
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.decode_mode != DSDCFG_MODE_DMR) {
        DSD_FPRINTF(stderr, "FAIL: save during an nfm row wrote decode mode %d, want the configured DMR\n",
                    (int)snap.decode_mode);
        rc |= 1;
    }
    char rendered[16384];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        rc |= 1;
    } else if (strstr(rendered, "12500") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: save during an nfm row wrote the row width:\n%s\n", rendered);
        rc |= 1;
    }
    dsd_scan_mode_leave(&opts, &state);
    if (opts.analog_only != 0 || opts.analog_nfm_bandwidth_hz != 0 || opts.frame_dmr != 1) {
        DSD_FPRINTF(stderr, "FAIL: leaving the nfm row left analog_only=%d width=%d frame_dmr=%d\n", opts.analog_only,
                    opts.analog_nfm_bandwidth_hz, opts.frame_dmr);
        rc |= 1;
    }

    /* An explicit configured width is the one saved, not the row's that runs over it, and an edit made while the row
       shadows it (the width command's path) is saved too. */
    opts.analog_nfm_bandwidth_hz = 20000;
    if (dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) != 0 || dsd_scan_mode_options(&opts, &state, &row) != 0
        || opts.analog_nfm_bandwidth_hz != 12500) {
        DSD_FPRINTF(stderr, "FAIL: nfm row width did not run over a configured 20 kHz (width=%d)\n",
                    opts.analog_nfm_bandwidth_hz);
        rc |= 1;
    }
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.has_analog || snap.analog_nfm_bandwidth_hz != 20000) {
        DSD_FPRINTF(stderr, "FAIL: save during an nfm row wrote nfm_bandwidth_hz %d, want the configured 20000\n",
                    snap.analog_nfm_bandwidth_hz);
        rc |= 1;
    }
    if (dsd_scan_mode_set_configured_nfm_bandwidth(&opts, &state, 11250) != 0) {
        DSD_FPRINTF(stderr, "FAIL: the row width did not shadow a configured width edit\n");
        rc |= 1;
    }
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        rc |= 1;
    } else if (strstr(rendered, "nfm_bandwidth_hz = 11250") == NULL || strstr(rendered, "12500") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: save after a shadowed width edit did not write the edited default:\n%s\n", rendered);
        rc |= 1;
    }
    dsd_scan_mode_leave(&opts, &state);
    if (opts.analog_nfm_bandwidth_hz != 11250) {
        DSD_FPRINTF(stderr, "FAIL: leaving the nfm row put back width %d, want the edited 11250\n",
                    opts.analog_nfm_bandwidth_hz);
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * Issue #524: the AM width follows the same configured-view rule as the NFM width. Config->Save taken while an nfm row
 * runs over an -fM session keeps decode = am and writes the configured am_bandwidth_hz from the configured view, never
 * the options in force; an AM width edit made under the row (the width command's path) is saved and survives the leave.
 */
static int
test_am_session_row_snapshot_keeps_configured_am_width(void) {
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:118.1M:22:-2:24:-80:2");
    opts.rtlsdr_center_freq = 118100000U;
    opts.rtl_dsp_bw_khz = 24;
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_AM, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not apply the AM preset\n");
        return 1;
    }
    opts.analog_am_bandwidth_hz = 8000;
    int rc = 0;
    dsd_scan_option_values row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 12500;
    if (dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) != 0 || dsd_scan_mode_options(&opts, &state, &row) != 0
        || opts.analog_demod != DSD_ANALOG_DEMOD_FM || opts.analog_nfm_bandwidth_hz != 12500) {
        DSD_FPRINTF(stderr, "FAIL: the nfm row did not run over the AM session (demod=%d width=%d)\n",
                    opts.analog_demod, opts.analog_nfm_bandwidth_hz);
        dsd_state_ext_free_all(&state);
        return 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.decode_mode != DSDCFG_MODE_AM || !snap.has_analog || snap.analog_am_bandwidth_hz != 8000
        || snap.analog_nfm_bandwidth_hz != 0) {
        DSD_FPRINTF(stderr, "FAIL: save during an nfm row on AM wrote mode %d am %d nfm %d, want am/8000/0\n",
                    (int)snap.decode_mode, snap.analog_am_bandwidth_hz, snap.analog_nfm_bandwidth_hz);
        rc |= 1;
    }
    /* The AM width command's path under the row: no row width shadows it, and the configured view takes it. */
    if (dsd_scan_mode_set_configured_analog_width(&opts, &state, DSD_ANALOG_DEMOD_AM, 10000) != 1) {
        DSD_FPRINTF(stderr, "FAIL: an AM width edit under the nfm row was shadowed\n");
        rc |= 1;
    }
    /* The save reads the configured view, whatever dsd_opts holds meanwhile. */
    opts.analog_am_bandwidth_hz = 5000;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[16384];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        rc |= 1;
    } else if (strstr(rendered, "am_bandwidth_hz = 10000") == NULL || strstr(rendered, "12500") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: save after an AM width edit under the row did not write the configured width:\n%s\n",
                    rendered);
        rc |= 1;
    }
    dsd_scan_mode_leave(&opts, &state);
    if (dsd_infer_decode_mode_preset(&opts) != DSDCFG_MODE_AM || opts.analog_am_bandwidth_hz != 10000
        || opts.analog_nfm_bandwidth_hz != 0) {
        DSD_FPRINTF(stderr, "FAIL: leaving the nfm row left mode %d am %d nfm %d, want AM/10000/0\n",
                    (int)dsd_infer_decode_mode_preset(&opts), opts.analog_am_bandwidth_hz,
                    opts.analog_nfm_bandwidth_hz);
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * Issue #526: an am row's class and its --am-bandwidth-hz are effective state, not user defaults. Config->Save taken
 * while such a row is on air over a DMR session keeps decode = dmr and writes the configured am_bandwidth_hz (none
 * here), never the row's; an AM width edit made while the row's own width shadows it (the width command's path) is the
 * one saved, and the leave puts the configured decoder and the edited width back.
 */
static int
test_am_row_snapshot_keeps_configured_mode_and_width(void) {
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:118.3M:22:-2:24:-80:2");
    opts.rtlsdr_center_freq = 118300000U;
    opts.rtl_dsp_bw_khz = 24;
    if (dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) != 0
        || dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_AM) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not open an am scan scope\n");
        return 1;
    }
    int rc = 0;
    dsd_scan_option_values row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 8333;
    row.channel_bw_kind = DSD_ANALOG_DEMOD_AM;
    (void)dsd_scan_mode_options(&opts, &state, &row);
    if (opts.analog_only != 1 || opts.analog_demod != DSD_ANALOG_DEMOD_AM || opts.analog_am_bandwidth_hz != 8333) {
        DSD_FPRINTF(stderr, "FAIL: am row did not reach dsd_opts (analog_only=%d demod=%d width=%d)\n",
                    opts.analog_only, opts.analog_demod, opts.analog_am_bandwidth_hz);
        rc |= 1;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[16384];
    if (snap.decode_mode != DSDCFG_MODE_DMR || snap.analog_am_bandwidth_hz != 0) {
        DSD_FPRINTF(stderr, "FAIL: save during an am row wrote decode mode %d am width %d, want DMR and none\n",
                    (int)snap.decode_mode, snap.analog_am_bandwidth_hz);
        rc |= 1;
    }
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        rc |= 1;
    } else if (strstr(rendered, "8333") != NULL || strstr(rendered, "decode = \"dmr\"\n") == NULL
               || strstr(rendered, "decode = \"am\"") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: save during an am row wrote the row's class or width:\n%s\n", rendered);
        rc |= 1;
    }
    /* The width command's path under the row: the row's own width shadows the edit (0), which the baseline keeps. */
    if (dsd_scan_mode_set_configured_analog_width(&opts, &state, DSD_ANALOG_DEMOD_AM, 10000) != 0
        || opts.analog_am_bandwidth_hz != 8333) {
        DSD_FPRINTF(stderr, "FAIL: the am row width did not shadow a configured AM width edit (width=%d)\n",
                    opts.analog_am_bandwidth_hz);
        rc |= 1;
    }
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.decode_mode != DSDCFG_MODE_DMR || snap.analog_am_bandwidth_hz != 10000) {
        DSD_FPRINTF(stderr,
                    "FAIL: save after a shadowed AM width edit wrote decode mode %d am width %d, want DMR and "
                    "10000\n",
                    (int)snap.decode_mode, snap.analog_am_bandwidth_hz);
        rc |= 1;
    }
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        rc |= 1;
    } else if (strstr(rendered, "am_bandwidth_hz = 10000") == NULL || strstr(rendered, "8333") != NULL
               || strstr(rendered, "decode = \"dmr\"\n") == NULL || strstr(rendered, "decode = \"am\"") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: save after a shadowed AM width edit did not write the edited default:\n%s\n",
                    rendered);
        rc |= 1;
    }
    dsd_scan_mode_leave(&opts, &state);
    if (opts.analog_only != 0 || opts.frame_dmr != 1 || opts.analog_demod != DSD_ANALOG_DEMOD_FM
        || opts.analog_am_bandwidth_hz != 10000) {
        DSD_FPRINTF(stderr, "FAIL: leaving the am row left analog_only=%d frame_dmr=%d demod=%d am width=%d\n",
                    opts.analog_only, opts.frame_dmr, opts.analog_demod, opts.analog_am_bandwidth_hz);
        rc |= 1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

/*
 * A parked row's own --scan-max-visit-ms is effective state, not a user default. The save path
 * has to read the configured baseline through dsd_scan_mode_configured_view(), or a
 * Config->Save taken while a row override is active would pin the row's value for every
 * future session.
 */
static int
test_scan_max_visit_snapshot_uses_configured_not_row_override(void) {
    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    opts.scan_max_visit_ms = 20000; /* the configured global */

    if (dsd_scan_mode_begin(&opts, &state) != 0 || dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_INHERIT) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not open a scan scope\n");
        return 1;
    }

    int rc = 0;
    dsd_scan_option_values row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.present = DSD_SCAN_OPT_MAX_VISIT;
    row.max_visit_ms = 5000;
    if (dsd_scan_mode_options(&opts, &state, &row) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not install the row cap override\n");
        dsd_scan_mode_leave(&opts, &state);
        return 1;
    }
    if (opts.scan_max_visit_ms != 5000) {
        DSD_FPRINTF(stderr, "FAIL: row cap override did not reach dsd_opts, got %d\n", opts.scan_max_visit_ms);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.trunk_scan_max_visit_ms != 20000) {
        DSD_FPRINTF(stderr, "FAIL: snapshot saved the row cap instead of the configured one, got %d\n",
                    snap.trunk_scan_max_visit_ms);
        rc |= 1;
    }

    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        dsd_scan_mode_leave(&opts, &state);
        return 1;
    }
    rc |= expect_contains_quiet("trunking configured scan max visit", rendered, "scan_max_visit_ms = 20000\n");
    rc |= expect_absent_quiet("trunking row scan max visit", rendered, "scan_max_visit_ms = 5000\n");

    dsd_scan_mode_leave(&opts, &state);
    if (opts.scan_max_visit_ms != 20000) {
        DSD_FPRINTF(stderr, "FAIL: leaving the scope did not restore the configured cap, got %d\n",
                    opts.scan_max_visit_ms);
        rc |= 1;
    }
    return rc;
}

/*
 * [trunking] p25_bandplan_csv rides the same rails as chan_csv: INI -> cfg ->
 * opts->p25_bandplan_in_file -> snapshot -> render, and an empty path renders
 * no key at all so a saved config does not pin an empty string.
 */
static int
test_src_csv_roundtrip(void) {
    static const char* ini = "[trunking]\n"
                             "src_csv = \"/tmp/source.csv\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "src_csv config failed to load\n");
        return 1;
    }

    int rc = 0;
    if (strcmp(cfg.trunk_src_csv, "/tmp/source.csv") != 0) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] src_csv did not load, got \"%s\"\n", cfg.trunk_src_csv);
        rc |= 1;
    }

    auto opts_storage = std::unique_ptr<dsd_opts>(new dsd_opts{});
    auto state_storage = std::unique_ptr<dsd_state>(new dsd_state{});
    dsd_opts& opts = *opts_storage;
    dsd_state& state = *state_storage;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (strcmp(opts.src_in_file, "/tmp/source.csv") != 0) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] src_csv did not reach opts.src_in_file, got \"%s\"\n", opts.src_in_file);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (strcmp(snap.trunk_src_csv, "/tmp/source.csv") != 0) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped src_csv, got \"%s\"\n", snap.trunk_src_csv);
        rc |= 1;
    }

    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("trunking src_csv", rendered, "src_csv = \"/tmp/source.csv\"\n");

    reset_opts_and_state(opts, state);
    dsdneoUserConfig empty_snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &empty_snap);
    if (render_config_to_buffer(&empty_snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "src_csv") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: empty src_csv must not be rendered\n");
        rc |= 1;
    }
    return rc;
}

static int
test_p25_bandplan_csv_roundtrip(void) {
    static const char* ini = "[trunking]\n"
                             "p25_bandplan_csv = \"/tmp/plan.csv\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "p25_bandplan_csv config failed to load\n");
        return 1;
    }

    int rc = 0;
    if (strcmp(cfg.trunk_p25_bandplan_csv, "/tmp/plan.csv") != 0) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] p25_bandplan_csv did not load, got \"%s\"\n", cfg.trunk_p25_bandplan_csv);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (strcmp(opts.p25_bandplan_in_file, "/tmp/plan.csv") != 0) {
        DSD_FPRINTF(stderr, "FAIL: [trunking] p25_bandplan_csv did not reach opts.p25_bandplan_in_file, got \"%s\"\n",
                    opts.p25_bandplan_in_file);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (strcmp(snap.trunk_p25_bandplan_csv, "/tmp/plan.csv") != 0) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped p25_bandplan_csv, got \"%s\"\n", snap.trunk_p25_bandplan_csv);
        rc |= 1;
    }

    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains_quiet("trunking p25_bandplan_csv", rendered, "p25_bandplan_csv = \"/tmp/plan.csv\"\n");

    reset_opts_and_state(opts, state);
    dsdneoUserConfig empty_snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &empty_snap);
    if (render_config_to_buffer(&empty_snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "p25_bandplan_csv") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: empty p25_bandplan_csv must not be rendered\n");
        rc |= 1;
    }
    return rc;
}

/*
 * The ordering that makes this work at all: dsd_apply_decode_mode_preset() for
 * edacs_pv runs decode_mode_apply_edacs_pv(), which resets state->ea_mode and
 * state->esk_mask to 0 unconditionally. Both keys must therefore be applied
 * AFTER the preset, exactly like dmr_mono. Loading them before it would leave a
 * config that reads correctly and decodes wrongly.
 */
static int
test_edacs_variant_roundtrip(void) {
    static const char* ini = "version = 1\n\n"
                             "[mode]\n"
                             "decode = \"edacs_pv\"\n"
                             "edacs_ea = true\n"
                             "edacs_esk = true\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "edacs variant config failed to load\n");
        return 1;
    }

    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (state.ea_mode != 1) {
        DSD_FPRINTF(stderr, "FAIL: edacs_ea lost to the decode preset (ea_mode=%d)\n", state.ea_mode);
        rc |= 1;
    }
    /* 0xA0 is the only non-zero mask any CLI variant or the RadioReference apply
       handler ever sets; the key is a boolean over exactly that value. */
    if (state.esk_mask != 0xA0) {
        DSD_FPRINTF(stderr, "FAIL: edacs_esk did not set the 0xA0 mask (esk_mask=0x%X)\n", (unsigned)state.esk_mask);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.edacs_ea || !snap.edacs_esk) {
        DSD_FPRINTF(stderr, "FAIL: snapshot dropped the EDACS EA/ESK variant\n");
        rc |= 1;
    }

    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    /* Saved as the tri-state key only: an older file's edacs_ea is written back as its edacs_addressing. */
    rc |= expect_contains_quiet("edacs addressing", rendered, "edacs_addressing = \"extended\"\n");
    rc |= expect_absent_quiet("no edacs_ea written", rendered, "edacs_ea");
    rc |= expect_contains_quiet("edacs esk", rendered, "edacs_esk = true\n");

    /* The other half of the 2x2 the four CLI variants expose: EA on, ESK off. */
    static const char* ini_ea_only = "version = 1\n\n"
                                     "[mode]\n"
                                     "decode = \"edacs_pv\"\n"
                                     "edacs_ea = true\n"
                                     "edacs_esk = false\n";
    if (write_temp_config(ini_ea_only, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg2;
    load_rc = dsd_user_config_load(path, &cfg2);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "edacs ea-only config failed to load\n");
        return 1;
    }
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg2, &opts, &state);
    if (state.ea_mode != 1 || state.esk_mask != 0) {
        DSD_FPRINTF(stderr, "FAIL: ea-only variant wrong (ea_mode=%d esk_mask=0x%X)\n", state.ea_mode,
                    (unsigned)state.esk_mask);
        rc |= 1;
    }
    return rc;
}

/*
 * Discussion #717: [mode] edacs_addressing is the tri-state form of edacs_ea, so a
 * ProVoice-alone (-fp) session can say "no addressing mode" past the edacs_pv
 * preset's standard addressing. It wins over edacs_ea wherever the two sit; a value
 * it does not name is ignored, leaving edacs_ea to answer; a file with only
 * edacs_ea loads as before.
 */
static int
test_edacs_addressing_key_precedence(void) {
    static const struct {
        const char* label;
        const char* mode_body;
        int want_ea_mode;
        unsigned want_esk;
        const char* want_rendered;
    } cases[] = {
        {"old edacs_ea=false", "decode = \"edacs_pv\"\nedacs_ea = false\n", 0, 0U, "edacs_addressing = \"standard\"\n"},
        {"old edacs_ea=true without decode", "edacs_ea = true\n", 1, 0U, "edacs_addressing = \"extended\"\n"},
        {"none under edacs_pv", "decode = \"edacs_pv\"\nedacs_addressing = \"none\"\n", -1, 0U,
         "edacs_addressing = \"none\"\n"},
        {"none before edacs_ea=true", "edacs_addressing = \"none\"\nedacs_ea = true\n", -1, 0U,
         "edacs_addressing = \"none\"\n"},
        {"extended after edacs_ea=false", "edacs_ea = false\nedacs_addressing = \"Extended\"\nedacs_esk = true\n", 1,
         0xA0U, "edacs_addressing = \"extended\"\n"},
        {"standard over edacs_ea=true", "edacs_ea = true\nedacs_addressing = \"standard\"\n", 0, 0U,
         "edacs_addressing = \"standard\"\n"},
        {"unknown value leaves edacs_ea", "edacs_addressing = \"bogus\"\nedacs_ea = true\n", 1, 0U,
         "edacs_addressing = \"extended\"\n"},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char ini[256];
        DSD_SNPRINTF(ini, sizeof ini, "version = 1\n\n[mode]\n%s", cases[i].mode_body);
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(ini, path, sizeof path) != 0) {
            return 1;
        }
        dsdneoUserConfig cfg;
        const int load_rc = dsd_user_config_load(path, &cfg);
        (void)remove(path);
        if (load_rc != 0) {
            DSD_FPRINTF(stderr, "FAIL: %s: config failed to load\n", cases[i].label);
            rc |= 1;
            continue;
        }
        static dsd_opts opts;
        static dsd_state state;
        reset_opts_and_state(opts, state);
        state.ea_mode = 7; /* a value no key produces, so the key is what decided */
        state.esk_mask = 0x55;
        dsd_apply_user_config_to_opts(&cfg, &opts, &state);
        if (state.ea_mode != cases[i].want_ea_mode || state.esk_mask != cases[i].want_esk) {
            DSD_FPRINTF(stderr, "FAIL: %s: ea_mode=%d esk_mask=0x%X (want %d, 0x%X)\n", cases[i].label, state.ea_mode,
                        (unsigned)state.esk_mask, cases[i].want_ea_mode, cases[i].want_esk);
            rc |= 1;
        }
        char rendered[8192];
        if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
            return 1;
        }
        rc |= expect_contains_quiet(cases[i].label, rendered, cases[i].want_rendered);
        rc |= expect_absent_quiet(cases[i].label, rendered, "edacs_ea");
    }
    return rc;
}

/*
 * state->ea_mode is TRI-state: dsd_init.c seeds it at -1 ("no EDACS addressing
 * mode chosen yet"), which edacs-fme.c, provoice.c, dsd_events.c and the ncurses
 * EDACS rows all distinguish from 0 (standard). A snapshot that folded -1 through
 * a truthiness test would save it as "extended addressing", and the next load
 * would pin every session to EA with no way back - the in-session toggle only
 * ever produces 0 or 1. So the keys must be OMITTED until the answer exists, and
 * a config without them must leave -1 alone.
 */
static int
test_edacs_variant_unanswered_is_not_persisted(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    state.ea_mode = -1; /* what dsd_init.c installs */
    state.esk_mask = 0;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.has_edacs_variant) {
        DSD_FPRINTF(stderr, "FAIL: an unanswered EDACS mode was recorded (edacs_ea=%d)\n", snap.edacs_ea);
        rc |= 1;
    }

    char rendered[8192];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "edacs_ea") != NULL || strstr(rendered, "edacs_esk") != NULL
        || strstr(rendered, "edacs_addressing") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: an unanswered EDACS mode reached the INI\n");
        rc |= 1;
    }

    /* And the other direction: an unrelated config must not decide the question.
       A [mode] decode preset is excluded on purpose - dsd_apply_decode_mode_preset()
       runs decode_mode_apply_edacs_pv(), which answers "standard" by design, the
       same as the CLI's -fh. */
    static const char* ini = "version = 1\n\n"
                             "[trunking]\n"
                             "enabled = true\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    const int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "EDACS-less config failed to load\n");
        return 1;
    }
    reset_opts_and_state(opts, state);
    state.ea_mode = -1;
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (state.ea_mode != -1) {
        DSD_FPRINTF(stderr, "FAIL: a config with no EDACS keys answered the question (ea_mode=%d)\n", state.ea_mode);
        rc |= 1;
    }
    return rc;
}

/*
 * Exactly one automatic tuner owner. actions_trunk.c and rr_apply_tuner_owner
 * both enforce it; a config apply that set scanner_mode on its own would leave
 * the trunking SM following a control channel while the scanner retunes off it
 * on every hangtime expiry.
 */
static int
test_scanner_and_trunking_are_mutually_exclusive(void) {
    int rc = 0;
    static const char* ini = "version = 1\n\n"
                             "[trunking]\n"
                             "enabled = true\n"
                             "scanner = true\n";
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load(path, &cfg);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "trunking+scanner config failed to load\n");
        return 1;
    }
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.trunk_enable != 1 || opts.scanner_mode != 0) {
        DSD_FPRINTF(stderr, "FAIL: both tuner owners live (trunk_enable=%d scanner_mode=%d)\n", opts.trunk_enable,
                    opts.scanner_mode);
        rc |= 1;
    }

    /* Scanner alone still wins the tuner, and clears a trunking flag the session
       was carrying from somewhere else. */
    static const char* ini_scan = "version = 1\n\n"
                                  "[trunking]\n"
                                  "enabled = false\n"
                                  "scanner = true\n";
    if (write_temp_config(ini_scan, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg2;
    load_rc = dsd_user_config_load(path, &cfg2);
    (void)remove(path);
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "scanner-only config failed to load\n");
        return 1;
    }
    reset_opts_and_state(opts, state);
    opts.trunk_enable = 1;
    dsd_apply_user_config_to_opts(&cfg2, &opts, &state);
    if (opts.scanner_mode != 1 || opts.trunk_enable != 0) {
        DSD_FPRINTF(stderr, "FAIL: scanner did not take the tuner (trunk_enable=%d scanner_mode=%d)\n",
                    opts.trunk_enable, opts.scanner_mode);
        rc |= 1;
    }
    return rc;
}

/*
 * MUST be the first case main() runs. dsd_user_config_default_path() latches its
 * answer in `static char buf[1024]; static int inited` (src/runtime/config_user.cpp,
 * identifier dsd_user_config_default_path), so the environment override only takes
 * effect if nothing in this binary has asked for a config path yet. No other case
 * in this file touches a config-path helper today; keep it that way.
 */
static int
test_imports_dir_follows_config_dir(void) {
    char scratch[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(scratch, sizeof scratch, "dsdneo_imports_dir") == NULL) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

#if defined(_WIN32)
    const char* cfg_env = "APPDATA";
#else
    const char* cfg_env = "XDG_CONFIG_HOME";
#endif
    if (dsd_test_setenv(cfg_env, scratch, 1) != 0) {
        DSD_FPRINTF(stderr, "dsd_test_setenv(%s) failed\n", cfg_env);
        (void)DSD_TEST_RMDIR(scratch);
        return 1;
    }

    /* Not named *_dir: readability-suspicious-call-argument reads a "<x>_dir"
       argument in dsd_test_path_join's `out` slot as swapped with its `dir`. */
    char cfg_root[DSD_TEST_PATH_MAX] = {};
    char expected[DSD_TEST_PATH_MAX] = {};
    int rc = dsd_test_path_join(cfg_root, sizeof cfg_root, scratch, "dsd-neo") != 0 ? 1 : 0;
    rc |= dsd_test_path_join(expected, sizeof expected, cfg_root, "imports") != 0 ? 1 : 0;

    const char* dir = dsd_user_imports_dir();
    if (!dir) {
        DSD_FPRINTF(stderr, "dsd_user_imports_dir returned NULL\n");
        (void)DSD_TEST_RMDIR(scratch);
        return 1;
    }
    rc |= strcmp(dir, expected) == 0 ? 0 : 1;

    rc |= dsd_user_imports_dir_create() == 0 ? 0 : 1;
    dsd_stat_t st;
    rc |= dsd_stat_path(expected, &st) == 0 && !dsd_stat_is_regular(&st) ? 0 : 1;

    /* Idempotent, and the answer is recomputed rather than latched. */
    rc |= dsd_user_imports_dir_create() == 0 ? 0 : 1;
    rc |= strcmp(dsd_user_imports_dir(), expected) == 0 ? 0 : 1;

    rc |= remove_test_dir(expected);
    rc |= remove_test_dir(cfg_root);
    rc |= remove_test_dir(scratch);
    return rc;
}

static int
test_dmr_lrrp_ports_load_apply_snapshot_roundtrip(void) {
    // Blanks and a repeated entry are tolerated on the way in; the table holds each port once.
    static const char* ini = "[mode]\n"
                             "dmr_lrrp_ports = \"5000, 5001,5000\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    int rc = 0;
    if (strcmp(cfg.dmr_lrrp_ports, "5000, 5001,5000") != 0) {
        DSD_FPRINTF(stderr, "dmr_lrrp_ports parse mismatch: \"%s\"\n", cfg.dmr_lrrp_ports);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.lrrp_extra_port_count != 2 || opts.lrrp_extra_ports[0] != 5000U || opts.lrrp_extra_ports[1] != 5001U) {
        DSD_FPRINTF(stderr, "dmr_lrrp_ports apply mismatch: count=%d ports={%u,%u}\n", opts.lrrp_extra_port_count,
                    (unsigned)opts.lrrp_extra_ports[0], (unsigned)opts.lrrp_extra_ports[1]);
        rc |= 1;
    }

    // Applying the same config again replaces the table rather than growing it.
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.lrrp_extra_port_count != 2) {
        DSD_FPRINTF(stderr, "dmr_lrrp_ports re-apply accumulated: count=%d\n", opts.lrrp_extra_port_count);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (strcmp(snap.dmr_lrrp_ports, "5000,5001") != 0) {
        DSD_FPRINTF(stderr, "snapshot dmr_lrrp_ports mismatch: \"%s\"\n", snap.dmr_lrrp_ports);
        rc |= 1;
    }

    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (!strstr(rendered, "dmr_lrrp_ports = \"5000,5001\"")) {
        DSD_FPRINTF(stderr, "rendered config missing dmr_lrrp_ports:\n%s\n", rendered);
        rc |= 1;
    }

    if (write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg_reload;
    if (dsd_user_config_load(path, &cfg_reload) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for rendered config %s\n", path);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    static dsd_opts opts_reload;
    static dsd_state state_reload;
    reset_opts_and_state(opts_reload, state_reload);
    dsd_apply_user_config_to_opts(&cfg_reload, &opts_reload, &state_reload);
    if (opts_reload.lrrp_extra_port_count != 2 || opts_reload.lrrp_extra_ports[0] != 5000U
        || opts_reload.lrrp_extra_ports[1] != 5001U) {
        DSD_FPRINTF(stderr, "reloaded dmr_lrrp_ports mismatch: count=%d\n", opts_reload.lrrp_extra_port_count);
        rc |= 1;
    }
    return rc;
}

static int
test_dmr_lrrp_ports_absent_key_keeps_existing_list(void) {
    // A config that never mentions the key must not wipe a list the CLI already supplied.
    static const char* ini = "[mode]\n"
                             "dmr_mono = false\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.lrrp_extra_ports[0] = 6000U;
    opts.lrrp_extra_port_count = 1;
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    int rc = 0;
    if (opts.lrrp_extra_port_count != 1 || opts.lrrp_extra_ports[0] != 6000U) {
        DSD_FPRINTF(stderr, "absent dmr_lrrp_ports changed the list: count=%d port0=%u\n", opts.lrrp_extra_port_count,
                    (unsigned)opts.lrrp_extra_ports[0]);
        rc |= 1;
    }

    // And a snapshot of a session with no ports renders no key at all.
    reset_opts_and_state(opts, state);
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "dmr_lrrp_ports")) {
        DSD_FPRINTF(stderr, "empty port list rendered a dmr_lrrp_ports key:\n%s\n", rendered);
        rc |= 1;
    }
    return rc;
}

static int
test_dmr_lrrp_ports_bad_entries_are_skipped(void) {
    // Loading is lenient: the validator warns, apply keeps the entries that parse.
    static const char* ini = "[mode]\n"
                             "dmr_lrrp_ports = \"5000,abc,70000,5001\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.lrrp_extra_port_count != 2 || opts.lrrp_extra_ports[0] != 5000U || opts.lrrp_extra_ports[1] != 5001U) {
        DSD_FPRINTF(stderr, "bad dmr_lrrp_ports entries not skipped: count=%d\n", opts.lrrp_extra_port_count);
        return 1;
    }
    return 0;
}

/* Load @p ini from a temp file into @p cfg; returns 0 on success. */
static int
load_ini_text(const char* ini, dsdneoUserConfig* cfg) {
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }
    if (dsd_user_config_load(path, cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }
    (void)remove(path);
    return 0;
}

static int
test_dmr_udp_comp_ports_load_apply_snapshot_roundtrip(void) {
    // Blanks, out-of-order entries and a repeated pair are tolerated on the way in; the map holds each link once
    // and renders in index order.
    static const char* ini = "[mode]\n"
                             "dmr_udp_comp_ports = \"98 = 4007, 97=4005,97=4005\"\n";
    dsdneoUserConfig cfg;
    if (load_ini_text(ini, &cfg) != 0) {
        return 1;
    }

    int rc = 0;
    if (strcmp(cfg.dmr_udp_comp_ports, "98 = 4007, 97=4005,97=4005") != 0) {
        DSD_FPRINTF(stderr, "dmr_udp_comp_ports parse mismatch: \"%s\"\n", cfg.dmr_udp_comp_ports);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 97U) != 4005U
        || dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 98U) != 4007U) {
        DSD_FPRINTF(stderr, "dmr_udp_comp_ports apply mismatch: 97=%u 98=%u\n",
                    (unsigned)dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 97U),
                    (unsigned)dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 98U));
        rc |= 1;
    }

    // Applying another config replaces the map rather than merging into it.
    dsdneoUserConfig other;
    if (load_ini_text("[mode]\ndmr_udp_comp_ports = \"100=5016\"\n", &other) != 0) {
        return 1;
    }
    dsd_apply_user_config_to_opts(&other, &opts, &state);
    if (dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 97U) != 0U
        || dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 100U) != 5016U) {
        DSD_FPRINTF(stderr, "dmr_udp_comp_ports re-apply merged: 97=%u 100=%u\n",
                    (unsigned)dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 97U),
                    (unsigned)dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 100U));
        rc |= 1;
    }
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (strcmp(snap.dmr_udp_comp_ports, "97=4005,98=4007") != 0) {
        DSD_FPRINTF(stderr, "snapshot dmr_udp_comp_ports mismatch: \"%s\"\n", snap.dmr_udp_comp_ports);
        rc |= 1;
    }

    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (!strstr(rendered, "dmr_udp_comp_ports = \"97=4005,98=4007\"")) {
        DSD_FPRINTF(stderr, "rendered config missing dmr_udp_comp_ports:\n%s\n", rendered);
        rc |= 1;
    }

    dsdneoUserConfig cfg_reload;
    if (load_ini_text(rendered, &cfg_reload) != 0) {
        return 1;
    }
    static dsd_opts opts_reload;
    static dsd_state state_reload;
    reset_opts_and_state(opts_reload, state_reload);
    dsd_apply_user_config_to_opts(&cfg_reload, &opts_reload, &state_reload);
    if (memcmp(opts_reload.dmr_udp_comp_ports, opts.dmr_udp_comp_ports, sizeof opts.dmr_udp_comp_ports) != 0) {
        DSD_FPRINTF(stderr, "reloaded dmr_udp_comp_ports mismatch\n");
        rc |= 1;
    }
    return rc;
}

static int
test_dmr_udp_comp_ports_full_map_roundtrip(void) {
    // Every index 95-127 linked to the widest port still fits the key and survives snapshot, render and reload.
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    for (unsigned i = 0; i < DSD_UDP_COMP_PORT_MAP_ENTRIES; i++) {
        opts.dmr_udp_comp_ports[i] = 65535U;
    }
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    uint16_t parsed[DSD_UDP_COMP_PORT_MAP_ENTRIES];
    int rc = 0;
    if (dsd_udp_comp_port_list_parse(snap.dmr_udp_comp_ports, parsed) != 0
        || memcmp(parsed, opts.dmr_udp_comp_ports, sizeof parsed) != 0) {
        DSD_FPRINTF(stderr, "full dmr_udp_comp_ports snapshot lost entries: \"%s\"\n", snap.dmr_udp_comp_ports);
        rc |= 1;
    }
    static char rendered[16384];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    dsdneoUserConfig cfg_reload;
    if (load_ini_text(rendered, &cfg_reload) != 0) {
        return 1;
    }
    static dsd_opts opts_reload;
    static dsd_state state_reload;
    reset_opts_and_state(opts_reload, state_reload);
    dsd_apply_user_config_to_opts(&cfg_reload, &opts_reload, &state_reload);
    if (memcmp(opts_reload.dmr_udp_comp_ports, opts.dmr_udp_comp_ports, sizeof opts.dmr_udp_comp_ports) != 0) {
        DSD_FPRINTF(stderr, "full dmr_udp_comp_ports reload mismatch: \"%s\"\n", cfg_reload.dmr_udp_comp_ports);
        rc |= 1;
    }
    return rc;
}

static int
test_dmr_udp_comp_ports_absent_key_keeps_existing_map(void) {
    // A config that never mentions the key must not wipe a map the CLI already supplied.
    dsdneoUserConfig cfg;
    if (load_ini_text("[mode]\ndmr_mono = false\n", &cfg) != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.dmr_udp_comp_ports[97U - DSD_UDP_COMP_PORT_IDX_FIRST] = 4005U;
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    int rc = 0;
    if (dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 97U) != 4005U) {
        DSD_FPRINTF(stderr, "absent dmr_udp_comp_ports changed the map: 97=%u\n",
                    (unsigned)dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 97U));
        rc |= 1;
    }

    // And a snapshot of a session with no links renders no key at all.
    reset_opts_and_state(opts, state);
    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (strstr(rendered, "dmr_udp_comp_ports")) {
        DSD_FPRINTF(stderr, "empty port map rendered a dmr_udp_comp_ports key:\n%s\n", rendered);
        rc |= 1;
    }
    return rc;
}

static int
test_dmr_udp_comp_ports_bad_entries_are_skipped(void) {
    // Loading is lenient: the validator warns, apply keeps the entries that parse. Rejected here: a reserved index,
    // an ETSI-defined one, one past 127, port 0, a port past 65535, a malformed entry and a second port for 97.
    static const char* ini =
        "[mode]\n"
        "dmr_udp_comp_ports = \"97=4005,50=4005,1=4005,128=4005,99=0,99=70000,abc,97=4007,98=4007\"\n";
    dsdneoUserConfig cfg;
    if (load_ini_text(ini, &cfg) != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    uint16_t want[DSD_UDP_COMP_PORT_MAP_ENTRIES];
    DSD_MEMSET(want, 0, sizeof want);
    want[97U - DSD_UDP_COMP_PORT_IDX_FIRST] = 4005U;
    want[98U - DSD_UDP_COMP_PORT_IDX_FIRST] = 4007U;
    if (memcmp(want, opts.dmr_udp_comp_ports, sizeof want) != 0) {
        DSD_FPRINTF(stderr, "bad dmr_udp_comp_ports entries not skipped: 97=%u 98=%u 99=%u\n",
                    (unsigned)dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 97U),
                    (unsigned)dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 98U),
                    (unsigned)dsd_udp_comp_port_lookup(opts.dmr_udp_comp_ports, 99U));
        return 1;
    }
    return 0;
}

static int
test_tg_lockout_persistence_roundtrip(void) {
    int rc = 0;
    const char* configurations[] = {"[trunking]\nenabled = true\n", "[trunking]\npersist_tg_lockouts = false\n",
                                    "[trunking]\npersist_tg_lockouts = true\n"};
    for (int i = 0; i < 3; ++i) {
        char path[DSD_TEST_PATH_MAX];
        if (write_temp_config(configurations[i], path, sizeof path) != 0) {
            return 1;
        }
        dsdneoUserConfig cfg;
        const int loaded = dsd_user_config_load(path, &cfg);
        remove(path);
        if (loaded != 0 || cfg.trunk_persist_tg_lockouts != (i != 1)) {
            DSD_FPRINTF(stderr, "lockout config/default did not load\n");
            return 1;
        }
        static dsd_opts opts;
        static dsd_state state;
        reset_opts_and_state(opts, state);
        dsd_apply_user_config_to_opts(&cfg, &opts, &state);
        if (opts.persist_tg_lockouts != (i != 1)) {
            return 1;
        }
        dsdneoUserConfig snapshot;
        dsd_snapshot_opts_to_user_config(&opts, &state, &snapshot);
        char rendered[8192];
        if (snapshot.trunk_persist_tg_lockouts != (i != 1)
            || render_config_to_buffer(&snapshot, rendered, sizeof rendered) != 0) {
            return 1;
        }
        rc |= expect_contains_quiet("persist lockouts roundtrip", rendered,
                                    i == 1 ? "persist_tg_lockouts = false\n" : "persist_tg_lockouts = true\n");
    }
    return rc;
}

static void
receive_check(bool condition, int line) {
    if (!condition) {
        DSD_FPRINTF(stderr, "receive configuration check failed at line %d\n", line);
        abort();
    }
}

#define RECEIVE_CHECK(condition) receive_check((condition), __LINE__)

static int
test_receive_defaults_and_row_inheritance(void) {
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    auto cfg = std::make_unique<dsdneoUserConfig>();
    char path[DSD_TEST_PATH_MAX];
    RECEIVE_CHECK(
        write_temp_config("[logging]\nevent_log = /home/o'neil/events.log  # main log\n"
                          "[receive]\nslot_enabled = true\nslot_allow = 1\ndcc_enabled = True\ndcc_allow = 0/7\n"
                          "dcc_block = 0\nnac_enabled = ON\nnac_allow = 0x293/FFE\n"
                          "names = --dcc-name \"7=Hospital's #1\" --tone-name 'D023I=Dispatch' ; comment\n",
                          path, sizeof path)
        == 0);
    RECEIVE_CHECK(dsd_user_config_load(path, cfg.get()) == 0 && cfg->has_receive && !cfg->receive_refused);
    /* An apostrophe is ordinary text: the comment after it is still a comment (only double quotes protect one). */
    RECEIVE_CHECK(!strcmp(cfg->event_log, "/home/o'neil/events.log"));
    /* [receive] booleans are spelled as every other INI boolean: case does not matter. */
    RECEIVE_CHECK(cfg->receive.filters[DSD_RECEIVE_DCC].enabled && cfg->receive.filters[DSD_RECEIVE_NAC].enabled);
    RECEIVE_CHECK(cfg->receive.names.count == 2);
    dsd_apply_user_config_to_opts(cfg.get(), opts.get(), state.get());
    RECEIVE_CHECK(opts->receive.filters[DSD_RECEIVE_DCC].enabled);
    RECEIVE_CHECK(dsd_receive_set_contains(&opts->receive.filters[DSD_RECEIVE_NAC].allow, 0xFFE));
    RECEIVE_CHECK(!strcmp(opts->receive.names.rules[0].name, "Hospital's #1"));
    const dsd_receive_policy defaults = opts->receive;
    dsd_scan_options row = {};
    char error[160];
    RECEIVE_CHECK(dsd_scan_options_parse("--no-dcc-filter --slot-block 2 --dcc-name '0=On call'", DSD_SCAN_MODE_DMR, 1,
                                         &row, error, sizeof error)
                  == 0);
    RECEIVE_CHECK(dsd_scan_mode_enter(opts.get(), state.get(), DSD_SCAN_MODE_DMR) == 0);
    RECEIVE_CHECK(dsd_scan_mode_options(opts.get(), state.get(), &row.values) == 0);
    RECEIVE_CHECK(!opts->receive.filters[DSD_RECEIVE_DCC].enabled);
    RECEIVE_CHECK(!dsd_receive_set_contains(&opts->receive.filters[DSD_RECEIVE_SLOT].allow, 1));
    RECEIVE_CHECK(dsd_receive_set_contains(&opts->receive.filters[DSD_RECEIVE_SLOT].block, 2));
    RECEIVE_CHECK(opts->receive.names.count == 1 && !strcmp(opts->receive.names.rules[0].name, "On call"));
    dsd_snapshot_opts_to_user_config(opts.get(), state.get(), cfg.get());
    RECEIVE_CHECK(dsd_receive_policy_equal(&cfg->receive, &defaults));
    RECEIVE_CHECK(dsd_user_config_save_atomic(path, cfg.get()) == 0);
    dsd_user_config_init(cfg.get());
    RECEIVE_CHECK(dsd_user_config_load(path, cfg.get()) == 0 && !cfg->receive_refused);
    RECEIVE_CHECK(dsd_receive_policy_equal(&cfg->receive, &defaults));
    RECEIVE_CHECK(dsd_scan_mode_options(opts.get(), state.get(), nullptr) == 0);
    RECEIVE_CHECK(dsd_receive_policy_equal(&opts->receive, &defaults));
    const dsd_scan_options previous = row;
    RECEIVE_CHECK(dsd_scan_options_parse("--ran-allow 1", DSD_SCAN_MODE_DMR, 1, &row, error, sizeof error) < 0);
    RECEIVE_CHECK(previous.values.present == row.values.present
                  && !dsd_scan_option_receive_differs(&previous.values, &row.values));
    dsd_user_config_init(cfg.get());
    RECEIVE_CHECK(dsd_user_config_apply_key(cfg.get(), "receive", "dcc_allow", "3") == 1);
    RECEIVE_CHECK(dsd_user_config_apply_key(cfg.get(), "receive", "nac_allow", "FFF") == 1);
    RECEIVE_CHECK(cfg->receive_refused);
    dsd_apply_user_config_to_opts(cfg.get(), opts.get(), state.get());
    RECEIVE_CHECK(dsd_receive_policy_equal(&opts->receive, &defaults));
    dsd_state_ext_free_all(state.get());
    RECEIVE_CHECK(remove(path) == 0);
    return 0;
}

static int g_receive_boundaries = 0;

static void
count_receive_boundary(dsd_opts*, dsd_state*, int) {
    g_receive_boundaries++;
}

/* Issue #637: a configuration applied while the scan scope is suspended judges no call against the passing global
   policy: once the row's own options are back, the boundary runs only if the policy in force really changed. */
static int
test_receive_boundary_waits_for_resume(void) {
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    dsd_frame_sync_hooks hooks = {};
    hooks.receive_policy_boundary = count_receive_boundary;
    dsd_frame_sync_hooks_set(&hooks);
    dsd_scan_options row = {};
    char error[160];
    RECEIVE_CHECK(dsd_scan_options_parse("--dcc-allow 7", DSD_SCAN_MODE_DMR, 1, &row, error, sizeof error) == 0);
    RECEIVE_CHECK(dsd_scan_mode_enter(opts.get(), state.get(), DSD_SCAN_MODE_DMR) == 0);
    RECEIVE_CHECK(dsd_scan_mode_options(opts.get(), state.get(), &row.values) == 0);
    g_receive_boundaries = 0;
    RECEIVE_CHECK(dsd_scan_mode_suspend(opts.get(), state.get()) == 1);
    dsd_receive_policy global = {};
    RECEIVE_CHECK(dsd_receive_edit_parse(&global, DSD_RECEIVE_DCC, 1, "", "7", "", error, sizeof error) == 0);
    RECEIVE_CHECK(dsd_scan_mode_set_configured_receive(opts.get(), state.get(), &global) == 0);
    RECEIVE_CHECK(g_receive_boundaries == 0);
    (void)dsd_scan_mode_resume(opts.get(), state.get());
    RECEIVE_CHECK(g_receive_boundaries == 0);
    RECEIVE_CHECK(dsd_receive_set_contains(&opts->receive.filters[DSD_RECEIVE_DCC].allow, 7));
    dsd_frame_sync_hooks_set(nullptr);
    dsd_state_ext_free_all(state.get());
    return 0;
}

#undef RECEIVE_CHECK

int
main(void) {
    int rc = 0;
    dsd_neo_log_set_tap(capture_log, NULL);
    rc |= test_imports_dir_follows_config_dir();
    rc |= test_rtl_freq_too_long_is_refused();
    rc |= test_rtl_volume_out_of_range_reads_as_default();
    rc |= test_receive_defaults_and_row_inheritance();
    rc |= test_receive_boundary_waits_for_resume();
    rc |= test_rtl_bw_khz_72_round_trip();
    rc |= test_recording_unknown_talkgroups_only_roundtrip();
    rc |= test_recording_analog_max_gap_roundtrip();
    rc |= test_recording_analog_preroll_roundtrip();
    rc |= test_recording_rdio_analog_roundtrip();
    rc |= test_recording_rdio_system_id_range();
    rc |= test_recording_rdio_mode_refused();
    rc |= test_recording_rdio_credentials_too_long();
    rc |= test_config_line_too_long_dropped_whole();
    rc |= test_config_line_nul_and_crlf();
    rc |= test_recording_refused_number_not_quoted();
#if defined(DSD_TEST_FAULT_READS)
    rc |= test_config_read_error_mid_line();
#endif
    rc |= test_recording_rdio_unknown_talkgroup_roundtrip();
    rc |= test_recording_rdio_system_by_frequency_roundtrip();
    rc |= test_apply_file_input_rescales_symbol_timing();
    rc |= test_apply_file_input_without_decode_keeps_session_timing();
    rc |= test_decode_mode_and_load_guards();
    rc |= test_persisted_v1_load_boundary();
    rc |= test_integer_overflow_is_rejected_consistently();
    rc |= test_unknown_section_warnings_do_not_mutate_loaded_config();
    rc |= test_render_input_variants_and_save_atomic();
    rc |= test_load_and_apply_basic();
    rc |= test_load_and_apply_alerts_empty_event_mask();
    rc |= test_load_and_apply_soapy_input_no_args();
    rc |= test_load_and_apply_soapy_input_with_args();
    rc |= test_load_and_apply_sddc_digital_resample();
    rc |= test_load_and_apply_rtl_digital_resample();
    rc |= test_snapshot_digital_resample_mode();
    rc |= test_snapshot_names_switched_inputs();
    rc |= test_snapshot_bare_network_inputs_save_as_their_source();
    rc |= test_replay_snapshot_saves_the_loaded_input();
    rc |= test_replay_snapshot_turns_off_tuning_beside_a_stream_input();
    rc |= test_retained_input_merges_a_patch();
    rc |= test_retained_input_merge_clears_the_rounding_note();
    rc |= test_rtl_serial_loads_renders_and_builds_the_spec();
    rc |= test_rtl_serial_invalid_refuses_the_input();
    rc |= test_rtl_serial_snapshot_names_the_running_dongle();
    rc |= test_retained_input_merges_the_rtl_selector();
    rc |= test_retained_index_beside_a_serial_survives_a_save();
    rc |= test_rtl_input_spec_is_whole_or_none();
    rc |= test_validate_refuses_a_replay_file_path();
    rc |= test_iq_stream_input_load_apply_snapshot();
    rc |= test_validate_iq_stream_input();
    rc |= test_check_iq_stream_startable();
    rc |= test_snapshot_trunking_reads_the_trunk_scan_configured_value();
    rc |= test_snapshot_scanner_saves_a_paused_scan();
    rc |= test_snapshot_never_saves_a_scan_hold();
    rc |= test_snapshot_writes_the_next_start();
    rc |= test_check_trunk_scan_startable();
    rc |= test_snapshot_roundtrip_soapy_args();
    rc |= test_snapshot_roundtrip_zero_rtl_ppm();
    rc |= test_snapshot_roundtrip_agc_rtl_gain();
    rc |= test_snapshot_rtl_and_rtltcp_device_specs();
    rc |= test_snapshot_disabled_squelch_roundtrips_as_off();
    rc |= test_snapshot_squelch_level_whole_db();
    rc |= test_auto_squelch_keys_roundtrip();
    rc |= test_noise_squelch_keys_roundtrip();
    rc |= test_pcm_squelch_keys_roundtrip();
    rc |= test_sourceless_squelch_keys_roundtrip();
    rc |= test_load_and_apply_rtltcp_regression();
    rc |= test_snapshot_roundtrip();
    rc |= test_apply_demod_lock();
    rc |= test_snapshot_persists_demod_lock();
    rc |= test_apply_logging_retargets_frame_log_file();
    rc |= test_apply_mode_ysf_uses_config_profile_behavior();
    rc |= test_snapshot_staged_file_rate_uses_requested_rate();
    rc |= test_snapshot_mode_inference_tdma_and_auto();
    rc |= test_snapshot_roundtrip_preserves_decoder_set();
    rc |= test_snapshot_roundtrip_dmr_mono_override();
    rc |= test_radioreference_schema_rows();
    rc |= test_trunk_scan_activity_hold_description();
    rc |= test_radioreference_config_roundtrip();
    rc |= test_radioreference_save_atomic_roundtrip();
    rc |= test_input_warn_db_load_snapshot_render();
    rc |= test_dmr_mono_preset_precedes_false_override();
    rc |= test_persistence_gap_schema_rows();
    rc |= test_dmr_lrrp_ports_load_apply_snapshot_roundtrip();
    rc |= test_dmr_lrrp_ports_absent_key_keeps_existing_list();
    rc |= test_dmr_lrrp_ports_bad_entries_are_skipped();
    rc |= test_dmr_udp_comp_ports_load_apply_snapshot_roundtrip();
    rc |= test_dmr_udp_comp_ports_full_map_roundtrip();
    rc |= test_dmr_udp_comp_ports_absent_key_keeps_existing_map();
    rc |= test_dmr_udp_comp_ports_bad_entries_are_skipped();
    rc |= test_scanner_and_candidates_roundtrip();
    rc |= test_scan_voice_gate_roundtrip();
    rc |= test_scan_skip_silent_roundtrip();
    rc |= test_scan_max_visit_roundtrip();
    rc |= test_scan_skip_silent_ms_roundtrip();
    rc |= test_analog_nfm_bandwidth_roundtrip();
    rc |= test_radio_input_spec();
    rc |= test_am_mode_and_bandwidth_roundtrip();
    rc |= test_tetra_mode_roundtrip();
    rc |= test_mode_without_decode_keeps_tetra();
    rc |= test_tone_policy_roundtrip();
    rc |= test_tone_policy_edit_roundtrip();
    rc |= test_tg_lockout_persistence_roundtrip();
    rc |= test_scan_max_visit_snapshot_uses_configured_not_row_override();
    rc |= test_squelch_snapshot_uses_configured_not_row_override();
    rc |= test_nfm_row_snapshot_keeps_configured_mode_and_width();
    rc |= test_am_row_snapshot_keeps_configured_mode_and_width();
    rc |= test_am_session_row_snapshot_keeps_configured_am_width();
    rc |= test_src_csv_roundtrip();
    rc |= test_p25_bandplan_csv_roundtrip();
    rc |= test_edacs_variant_roundtrip();
    rc |= test_edacs_addressing_key_precedence();
    rc |= test_edacs_variant_unanswered_is_not_persisted();
    rc |= test_scanner_and_trunking_are_mutually_exclusive();
    return rc;
}
