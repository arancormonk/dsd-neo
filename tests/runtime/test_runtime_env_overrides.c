// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Options, config and menus override variables inside the process and never write its environment (#782).
 *
 * `no-process-writes` (RUNTIME_ENV_NO_PROCESS_WRITES): every path that used to setenv() runs -- a config apply's
 * [dsp], a bootstrap with every option that sets a variable plus an INI with [dsp] and an RTL [input], the
 * --calc-lcn one-shot, and, with the radio pipeline, the Auto PPM and rtl_tcp autotune services and the two I/Q DSP
 * toggles. The process environment must stay exactly as it was: the same `environ` array, the same entries and the same
 * bytes. On Android adding an entry can free the array another thread's getenv() or localtime_r() is walking. Each value
 * must still reach the runtime config.
 *
 * `session-reset` (RUNTIME_SESSION_OVERRIDES_RESET): an embedding host bootstraps one session after another in the
 * same process (the Android service). A later session without an option must not inherit the earlier session's value
 * of it, while a variable the user exported still applies.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/config.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#include "../../src/app_control/services.h"
#endif

#if DSD_PLATFORM_WIN_NATIVE
#define TEST_ENVIRON _environ
#else
extern char** environ;
#define TEST_ENVIRON environ
#endif

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_near(const char* tag, double got, double want) {
    if (fabs(got - want) > 1e-6) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %f, want %f\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_value(const char* tag, const char* name, const char* want) {
    const char* got = dsd_neo_env_get(name);
    if (got == NULL || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: %s reads %s, want %s\n", tag, name, got ? got : "(unset)", want);
        return 1;
    }
    return 0;
}

/* ---- The process environment, entry by entry ---- */

typedef struct {
    char** base;
    size_t count;
    char** entries; /* the pointers environ held */
    char** copies;  /* their bytes */
} env_snapshot;

static void
snapshot_free(env_snapshot* s) {
    for (size_t i = 0; s->copies && i < s->count; i++) {
        free(s->copies[i]);
    }
    free((void*)s->entries);
    free((void*)s->copies);
    DSD_MEMSET(s, 0, sizeof *s);
}

static int
snapshot_take(env_snapshot* s) {
    DSD_MEMSET(s, 0, sizeof *s);
    char** env = TEST_ENVIRON;
    s->base = env;
    while (env && env[s->count]) {
        s->count++;
    }
    s->entries = (char**)calloc(s->count + 1U, sizeof *s->entries);
    s->copies = (char**)calloc(s->count + 1U, sizeof *s->copies);
    if (!s->entries || !s->copies) {
        return -1;
    }
    for (size_t i = 0; i < s->count; i++) {
        s->entries[i] = env[i];
        s->copies[i] = dsd_strdup(env[i]);
        if (!s->copies[i]) {
            return -1;
        }
    }
    return 0;
}

/* Nonzero, with the first difference on stderr, when the environment is not what the snapshot recorded. */
static int
snapshot_differs(const char* stage, const env_snapshot* s) {
    char** env = TEST_ENVIRON;
    size_t count = 0;
    while (env && env[count]) {
        count++;
    }
    if (count != s->count) {
        DSD_FPRINTF(stderr, "FAIL: %s: the environment went from %zu to %zu entries\n", stage, s->count, count);
        return 1;
    }
#if !DSD_PLATFORM_WIN_NATIVE
    /* The CRT may rebuild Windows' _environ on its own; elsewhere a new array is a write. */
    if (env != s->base) {
        DSD_FPRINTF(stderr, "FAIL: %s: environ moved\n", stage);
        return 1;
    }
#endif
    for (size_t i = 0; i < count; i++) {
#if !DSD_PLATFORM_WIN_NATIVE
        if (env[i] != s->entries[i]) {
            DSD_FPRINTF(stderr, "FAIL: %s: entry %zu was replaced (%s)\n", stage, i, env[i]);
            return 1;
        }
#endif
        if (strcmp(env[i], s->copies[i]) != 0) {
            DSD_FPRINTF(stderr, "FAIL: %s: entry %zu changed from %s to %s\n", stage, i, s->copies[i], env[i]);
            return 1;
        }
    }
    return 0;
}

/* ---- Sessions ---- */

static int
new_session(dsd_opts** opts_out, dsd_state** state_out) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return -1;
    }
    initOpts(opts);
    initState(state);
    *opts_out = opts;
    *state_out = state;
    return 0;
}

static void
free_session(dsd_opts* opts, dsd_state* state) {
    dsd_engine_release_outputs(opts, state);
    freeState(state);
    free(opts);
    free(state);
}

static int
bootstrap(int argc, char** argv, int want) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (new_session(&opts, &state) != 0) {
        return -1;
    }
    int exit_rc = 0;
    const int rc = dsd_runtime_bootstrap(argc, argv, opts, state, NULL, &exit_rc);
    free_session(opts, state);
    return rc == want ? 0 : -1;
}

static int
write_text_file(const char* path, const char* text) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    const size_t len = strlen(text);
    const int ok = fwrite(text, 1, len, fp) == len;
    return (fclose(fp) == 0 && ok) ? 0 : -1;
}

/* ---- RUNTIME_ENV_NO_PROCESS_WRITES ---- */

/* A config apply's [dsp] on the decoder thread: the INI's value applies when nothing else set the variable. */
static int
config_apply_writes_nothing(const env_snapshot* before) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (new_session(&opts, &state) != 0) {
        return expect_true("alloc", 0);
    }
    dsd_app_frontend_runtime_start(opts, state);
    dsdneoUserConfig cfg;
    dsd_user_config_init(&cfg);
    cfg.has_dsp = 1;
    cfg.iq_balance = 1;
    cfg.iq_dc_block = 1;
    int rc = expect_true("config apply submits", dsd_app_command_submit(DSD_APP_CMD_CONFIG_APPLY, &cfg, sizeof cfg)
                                                     == DSD_APP_COMMAND_SUBMIT_QUEUED);
    (void)dsd_app_drain_cmds(opts, state);
    dsd_app_frontend_runtime_stop();
    free_session(opts, state);
    rc |= snapshot_differs("config apply [dsp]", before);
    rc |= expect_value("config apply [dsp]", "DSD_NEO_IQ_BALANCE", "1");
    rc |= expect_value("config apply [dsp]", "DSD_NEO_IQ_DC_BLOCK", "1");
    return rc;
}

/* Every option that sets a variable, and an INI whose [dsp] and RTL [input] set three more. */
static int
bootstrap_writes_nothing(const env_snapshot* before, const char* ini) {
    char* argv[] = {"dsd-neo",
                    "--config",
                    (char*)ini,
                    "--auto-ppm",
                    "--auto-ppm-snr",
                    "8",
                    "--rtltcp-autotune",
                    "--p25-vc-grace",
                    "1.5",
                    "--p25-min-follow-dwell",
                    "2",
                    "--p25-grant-voice-timeout",
                    "3",
                    "--p25-mac-hold",
                    "1",
                    "--p25-cc-grace",
                    "4",
                    "--p25-force-release-extra",
                    "0.5",
                    "--p25-force-release-margin",
                    "0.75",
                    "--p25-p1-err-hold-pct",
                    "20",
                    "--p25-p1-err-hold-sec",
                    "6",
                    "--input-volume",
                    "3",
                    "--input-level-warn-db",
                    "-30",
                    NULL};
    const int argc = (int)(sizeof argv / sizeof argv[0]) - 1;
    int rc = expect_true("bootstrap with every writing option", bootstrap(argc, argv, DSD_BOOTSTRAP_CONTINUE) == 0);
    rc |= snapshot_differs("bootstrap", before);

    /* The values still reach the runtime config (the bootstrap re-reads it after the options). */
    const dsdneoRuntimeConfig* c = dsd_neo_get_config();
    if (!c) {
        return rc | expect_true("runtime config", 0);
    }
    rc |= expect_true("auto ppm", c->auto_ppm_is_set && c->auto_ppm_enable == 1);
    rc |= expect_near("auto ppm snr", c->auto_ppm_snr_db, 8.0);
    rc |= expect_true("tcp autotune", c->tcp_autotune_is_set && c->tcp_autotune_enable == 1);
    rc |= expect_near("p25 vc grace", c->p25_vc_grace_s, 1.5);
    rc |= expect_near("p25 min follow dwell", c->p25_min_follow_dwell_s, 2.0);
    rc |= expect_near("p25 grant voice timeout", c->p25_grant_voice_to_s, 3.0);
    rc |= expect_near("p25 mac hold", c->p25_mac_hold_s, 1.0);
    rc |= expect_near("p25 cc grace", c->p25_cc_grace_s, 4.0);
    rc |= expect_near("p25 force release extra", c->p25_force_release_extra_s, 0.5);
    rc |= expect_near("p25 force release margin", c->p25_force_release_margin_s, 0.75);
    rc |= expect_near("p25p1 err hold pct", c->p25p1_err_hold_pct, 20.0);
    rc |= expect_near("p25p1 err hold s", c->p25p1_err_hold_s, 6.0);
    rc |= expect_true("input volume", c->input_volume_is_set && c->input_volume_multiplier == 3);
    rc |= expect_near("input warn db", c->input_warn_db, -30.0);
    rc |= expect_true("iq dc block (INI)", c->iq_dc_block_is_set && c->iq_dc_block_enable == 1);
    rc |= expect_value("iq balance (INI)", "DSD_NEO_IQ_BALANCE", "1");
    return rc;
}

/* The --calc-lcn one-shot hands its options to the calculator through the variables it reads. */
static int
calc_lcn_writes_nothing(const env_snapshot* before, const char* csv) {
    char* argv[] = {"dsd-neo",  "--calc-lcn",    (char*)csv, "--calc-step",      "12500", "--calc-cc-freq",
                    "851.0125", "--calc-cc-lcn", "1",        "--calc-start-lcn", "1",     NULL};
    const int argc = (int)(sizeof argv / sizeof argv[0]) - 1;
    int rc = expect_true("the --calc-lcn one-shot exits", bootstrap(argc, argv, DSD_BOOTSTRAP_EXIT) == 0);
    rc |= snapshot_differs("--calc-lcn", before);
    rc |= expect_value("--calc-lcn step", "DSD_NEO_DMR_T3_STEP_HZ", "12500");
    return rc;
}

#ifdef USE_RADIO
/* The terminal's and the control API's RTL services, and the I/Q DSP toggles, on the decoder thread. */
static int
radio_services_write_nothing(const env_snapshot* before) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (new_session(&opts, &state) != 0) {
        return expect_true("alloc", 0);
    }
    dsd_app_frontend_runtime_start(opts, state);
    int rc = expect_true("auto ppm service", svc_rtl_set_auto_ppm(opts, NULL, 0) == 0);
    rc |= expect_true("autotune service", svc_rtltcp_set_autotune(opts, NULL, 0) == 0);
    rc |= snapshot_differs("RTL services", before);
    rc |= expect_value("auto ppm service", "DSD_NEO_AUTO_PPM", "0");
    rc |= expect_value("autotune service", "DSD_NEO_TCP_AUTOTUNE", "0");

    rtl_stream_toggle_iq_balance(0);
    rtl_stream_set_iq_dc(0, -1);
    dsd_app_dsp_payload dsp;
    DSD_MEMSET(&dsp, 0, sizeof dsp);
    dsp.op = DSD_APP_DSP_OP_TOGGLE_IQBAL;
    (void)dsd_app_command_submit(DSD_APP_CMD_DSP_OP, &dsp, sizeof dsp);
    dsp.op = DSD_APP_DSP_OP_IQ_DC_TOGGLE;
    (void)dsd_app_command_submit(DSD_APP_CMD_DSP_OP, &dsp, sizeof dsp);
    rc |= expect_true("the DSP toggles drain", dsd_app_drain_cmds(opts, state) == 2);
    rc |= snapshot_differs("DSP toggles", before);
    rc |= expect_value("iq balance toggle", "DSD_NEO_IQ_BALANCE", "1");
    rc |= expect_value("iq dc toggle", "DSD_NEO_IQ_DC_BLOCK", "1");
    dsd_app_frontend_runtime_stop();
    free_session(opts, state);
    return rc;
}
#endif

static int
run_no_process_writes(void) {
    char dir[DSD_TEST_PATH_MAX];
    char ini[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_env_writes") || dsd_test_path_join(ini, sizeof ini, dir, "c.ini")
        || dsd_test_path_join(csv, sizeof csv, dir, "lcn.csv")) {
        return expect_true("temp paths", 0);
    }
    const char* ini_text = "[dsp]\niq_balance = true\niq_dc_block = true\n"
#ifdef USE_RADIO
                           "[input]\nsource = \"rtl\"\nauto_ppm = true\n"
#endif
        ;
    if (write_text_file(ini, ini_text) != 0 || write_text_file(csv, "lcn,freq\n1,851.0125\n2,851.0250\n") != 0) {
        return expect_true("temp files", 0);
    }

    /* Every variable these paths set starts unset, so a write would add an entry. */
    static const char* const kNames[] = {"DSD_NEO_AUTO_PPM",
                                         "DSD_NEO_AUTO_PPM_SNR_DB",
                                         "DSD_NEO_TCP_AUTOTUNE",
                                         "DSD_NEO_P25_VC_GRACE",
                                         "DSD_NEO_P25_MIN_FOLLOW_DWELL",
                                         "DSD_NEO_P25_GRANT_VOICE_TO",
                                         "DSD_NEO_P25_MAC_HOLD",
                                         "DSD_NEO_P25_CC_GRACE",
                                         "DSD_NEO_P25_FORCE_RELEASE_EXTRA",
                                         "DSD_NEO_P25P1_ERR_HOLD_PCT",
                                         "DSD_NEO_P25_FORCE_RELEASE_MARGIN",
                                         "DSD_NEO_P25P1_ERR_HOLD_S",
                                         "DSD_NEO_INPUT_VOLUME",
                                         "DSD_NEO_INPUT_WARN_DB",
                                         "DSD_NEO_IQ_BALANCE",
                                         "DSD_NEO_IQ_DC_BLOCK",
                                         "DSD_NEO_DMR_T3_CALC_CSV",
                                         "DSD_NEO_DMR_T3_STEP_HZ",
                                         "DSD_NEO_DMR_T3_CC_FREQ",
                                         "DSD_NEO_DMR_T3_CC_LCN",
                                         "DSD_NEO_DMR_T3_START_LCN",
                                         "DSD_NEO_CONFIG",
                                         NULL};
    for (size_t i = 0; kNames[i] != NULL; i++) {
        (void)dsd_test_unsetenv(kNames[i]);
    }

    env_snapshot before;
    if (snapshot_take(&before) != 0) {
        snapshot_free(&before);
        return expect_true("snapshot", 0);
    }
    int rc = config_apply_writes_nothing(&before);
    rc |= bootstrap_writes_nothing(&before, ini);
    rc |= calc_lcn_writes_nothing(&before, csv);
#ifdef USE_RADIO
    rc |= radio_services_write_nothing(&before);
#endif
    snapshot_free(&before);

    static const char* const kFiles[] = {"c.ini", "lcn.csv", NULL};
    rc |= expect_true("temp dir removed", dsd_test_remove_temp_dir(dir, kFiles) == 0);
    return rc;
}

/* ---- RUNTIME_SESSION_OVERRIDES_RESET ---- */

static int
run_session_reset(void) {
    (void)dsd_test_unsetenv("DSD_NEO_AUTO_PPM");
    (void)dsd_test_unsetenv("DSD_NEO_P25_VC_GRACE");
    (void)dsd_test_unsetenv("DSD_NEO_CONFIG");
    /* A variable the user exported, which an option overrides for one session only. */
    if (dsd_test_setenv("DSD_NEO_INPUT_VOLUME", "2", 1) != 0) {
        return expect_true("export", 0);
    }

    char* first[] = {"dsd-neo", "--auto-ppm", "--input-volume", "3", "--p25-vc-grace", "1.5", NULL};
    int rc = expect_true("first session", bootstrap(6, first, DSD_BOOTSTRAP_CONTINUE) == 0);
    const dsdneoRuntimeConfig* c = dsd_neo_get_config();
    rc |= expect_true("first session config", c != NULL);
    if (c) {
        rc |= expect_true("first session: auto ppm", c->auto_ppm_is_set && c->auto_ppm_enable == 1);
        rc |= expect_true("first session: input volume", c->input_volume_multiplier == 3);
        rc |= expect_true("first session: vc grace", c->p25_vc_grace_is_set);
    }

    /* The next session, in the same process, without those options. */
    char* second[] = {"dsd-neo", NULL};
    rc |= expect_true("second session", bootstrap(1, second, DSD_BOOTSTRAP_CONTINUE) == 0);
    c = dsd_neo_get_config();
    rc |= expect_true("second session config", c != NULL);
    if (c) {
        rc |= expect_true("second session: no auto ppm", c->auto_ppm_is_set == 0);
        rc |= expect_true("second session: the exported input volume",
                          c->input_volume_is_set && c->input_volume_multiplier == 2);
        rc |= expect_true("second session: no vc grace", c->p25_vc_grace_is_set == 0);
    }
    rc |= expect_true("second session: auto ppm reads unset", dsd_neo_env_get("DSD_NEO_AUTO_PPM") == NULL);
    rc |= expect_value("second session: exported input volume", "DSD_NEO_INPUT_VOLUME", "2");
    (void)dsd_test_unsetenv("DSD_NEO_INPUT_VOLUME");
    return rc;
}

int
main(int argc, char** argv) {
    if (argc != 2) {
        DSD_FPRINTF(stderr, "usage: %s no-process-writes|session-reset\n", argv[0]);
        return 2;
    }
    int rc = 2;
    if (strcmp(argv[1], "no-process-writes") == 0) {
        rc = run_no_process_writes();
    } else if (strcmp(argv[1], "session-reset") == 0) {
        rc = run_session_reset();
    }
    if (rc == 0) {
        printf("RUNTIME_ENV_OVERRIDES %s: OK\n", argv[1]);
    }
    return rc;
}
