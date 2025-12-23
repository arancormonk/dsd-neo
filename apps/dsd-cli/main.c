// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#define DSD_NEO_MAIN

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/log.h>
#include <stdlib.h>

/* exitflag is defined in src/runtime/exitflag.c and declared in dsd.h */

int
pretty_colors() {
    fprintf(stderr, "%sred\n", KRED);
    fprintf(stderr, "%sgreen\n", KGRN);
    fprintf(stderr, "%syellow\n", KYEL);
    fprintf(stderr, "%sblue\n", KBLU);
    fprintf(stderr, "%smagenta\n", KMAG);
    fprintf(stderr, "%scyan\n", KCYN);
    fprintf(stderr, "%swhite\n", KWHT);
    fprintf(stderr, "%snormal\n", KNRM);

    return 0;
}

void
cleanupAndExit(dsd_opts* opts, dsd_state* state) {
    exitflag = 1;
    dsd_engine_cleanup(opts, state);
    exit(0);
}

int
main(int argc, char** argv) {
    extern char* optarg;
    extern int optind;
    dsd_opts* opts = calloc(1, sizeof(dsd_opts));
    dsd_state* state = calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        fprintf(stderr, "Failed to allocate memory for opts/state\n");
        free(opts);
        free(state);
        return 1;
    }
    int argc_effective = argc; // effective argc after runtime compaction
    const char* versionstr = mbe_versionString();

    initOpts(opts);
    initState(state);

    // Optional: user configuration file (INI) -----------------------------
    int enable_config_cli = 0;
    int force_bootstrap_cli = 0;
    int print_config_cli = 0;
    int dump_template_cli = 0;
    int validate_config_cli = 0;
    int strict_config_cli = 0;
    int list_profiles_cli = 0;
    const char* config_path_cli = NULL;
    const char* profile_cli = NULL;
    const char* validate_path_cli = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            enable_config_cli = 1;
            // Optional path argument (if next arg doesn't start with '-')
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config_path_cli = argv[++i];
            }
        } else if (strcmp(argv[i], "--interactive-setup") == 0) {
            force_bootstrap_cli = 1;
        } else if (strcmp(argv[i], "--print-config") == 0) {
            print_config_cli = 1;
        } else if (strcmp(argv[i], "--dump-config-template") == 0) {
            dump_template_cli = 1;
        } else if (strcmp(argv[i], "--validate-config") == 0) {
            validate_config_cli = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                validate_path_cli = argv[++i];
            }
        } else if (strcmp(argv[i], "--strict-config") == 0) {
            strict_config_cli = 1;
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile_cli = argv[++i];
        } else if (strcmp(argv[i], "--list-profiles") == 0) {
            list_profiles_cli = 1;
        }
    }

    const char* config_env = getenv("DSD_NEO_CONFIG");

    int user_cfg_loaded = 0;
    dsdneoUserConfig user_cfg;
    user_cfg.version = 0;

    // Default to no autosave unless a config is actually in play for this run.
    state->config_autosave_enabled = 0;
    state->config_autosave_path[0] = '\0';

    /* Config loading is opt-in: only load if --config is passed (with or
     * without a path) or if DSD_NEO_CONFIG env var is set. CLI takes
     * precedence: --config without a path uses the default, ignoring env. */
    if (enable_config_cli || (config_env && *config_env)) {
        const char* cfg_path = NULL;
        if (config_path_cli && *config_path_cli) {
            cfg_path = config_path_cli;
        } else if (enable_config_cli) {
            cfg_path = dsd_user_config_default_path();
        } else if (config_env && *config_env) {
            cfg_path = config_env;
        }

        if (cfg_path && *cfg_path) {
            // Remember the path so we can autosave the effective config later.
            state->config_autosave_enabled = 1;
            snprintf(state->config_autosave_path, sizeof state->config_autosave_path, "%s", cfg_path);
            state->config_autosave_path[sizeof state->config_autosave_path - 1] = '\0';

            int load_rc;
            if (profile_cli && *profile_cli) {
                load_rc = dsd_user_config_load_profile(cfg_path, profile_cli, &user_cfg);
            } else {
                load_rc = dsd_user_config_load(cfg_path, &user_cfg);
            }

            if (load_rc == 0) {
                dsd_apply_user_config_to_opts(&user_cfg, opts, state);
                user_cfg_loaded = 1;
                if (profile_cli && *profile_cli) {
                    LOG_NOTICE("Loaded user config from %s (profile: %s)\n", cfg_path, profile_cli);
                } else {
                    LOG_NOTICE("Loaded user config from %s\n", cfg_path);
                }
            } else if (profile_cli && *profile_cli) {
                // Missing profile is fatal when --profile is specified
                LOG_ERROR("Profile '%s' not found in config file %s\n", profile_cli, cfg_path);
                return 1;
            } else if (config_path_cli || config_env || enable_config_cli) {
                LOG_WARNING("Failed to load config file from %s; proceeding without config.\n", cfg_path);
            }
        }
    } else {
        // Config loading was not requested; do not autosave either.
        state->config_autosave_enabled = 0;
        state->config_autosave_path[0] = '\0';
    }

    // Phase 1: long-option and env parsing moved into runtime CLI helper
    {
        int oneshot_rc = 0;
        int early_rc = dsd_parse_args(argc, argv, opts, state, &argc_effective, &oneshot_rc);
        if (early_rc == DSD_PARSE_ONE_SHOT) {
            return oneshot_rc;
        } else if (early_rc != DSD_PARSE_CONTINUE) {
            return early_rc;
        }
        // Keep original argc for UI bootstrap heuristics; use argc_effective
        // only when iterating argv for file playback (-r).
    }
    state->cli_argc_effective = argc_effective;
    state->cli_argv = argv;

    // If a user config enabled trunking but this process was started with
    // any CLI arguments and none of them explicitly enabled/disabled trunk
    // (via -T / -Y), fall back to the built-in default of trunking disabled
    // for this run. This keeps CLI-driven sessions from inheriting trunk
    // enable solely from the config file.
    if (argc > 1 && user_cfg_loaded && !opts->trunk_cli_seen) {
        opts->p25_trunk = 0;
        opts->trunk_enable = 0;
    }

    // If a user config specified a non-48kHz file/RAW input and the CLI did
    // not override its sample rate, apply the corresponding symbol timing
    // scaling after all CLI/env parsing so that mode presets are adjusted
    // correctly. This mirrors legacy "-s" behavior without requiring users
    // to manage option ordering manually when using the config file.
    if (user_cfg_loaded && user_cfg.has_input && user_cfg.input_source == DSDCFG_INPUT_FILE
        && user_cfg.file_sample_rate > 0 && user_cfg.file_sample_rate != 48000 && opts->wav_decimator != 0
        && user_cfg.file_path[0] != '\0' && strcmp(opts->audio_in_dev, user_cfg.file_path) == 0
        && opts->wav_sample_rate == user_cfg.file_sample_rate) {
        opts->wav_interpolator = opts->wav_sample_rate / opts->wav_decimator;
        state->samplesPerSymbol = state->samplesPerSymbol * opts->wav_interpolator;
        state->symbolCenter = state->symbolCenter * opts->wav_interpolator;
    }

    if (print_config_cli) {
        dsdneoUserConfig eff;
        dsd_snapshot_opts_to_user_config(opts, state, &eff);
        dsd_user_config_render_ini(&eff, stdout);
        return 0;
    }

    // --dump-config-template: print commented template and exit
    if (dump_template_cli) {
        dsd_user_config_render_template(stdout);
        return 0;
    }

    // --validate-config: validate config file and exit
    if (validate_config_cli) {
        const char* vpath = validate_path_cli;
        if (!vpath || !*vpath) {
            // Use default or explicit config path
            if (config_path_cli && *config_path_cli) {
                vpath = config_path_cli;
            } else if (config_env && *config_env) {
                vpath = config_env;
            } else {
                vpath = dsd_user_config_default_path();
            }
        }
        if (!vpath || !*vpath) {
            fprintf(stderr, "No config file path specified or found.\n");
            return 1;
        }

        dsdcfg_diagnostics_t diags;
        int rc = dsd_user_config_validate(vpath, &diags);

        if (diags.count > 0) {
            dsdcfg_diags_print(&diags, stderr, vpath);
        } else {
            fprintf(stderr, "%s: OK\n", vpath);
        }

        int exit_code = 0;
        if (rc != 0 || diags.error_count > 0) {
            exit_code = 1;
        } else if (strict_config_cli && diags.warning_count > 0) {
            exit_code = 2;
        }

        dsd_user_config_diags_free(&diags);
        return exit_code;
    }

    // --list-profiles: list available profiles and exit
    if (list_profiles_cli) {
        const char* lpath = config_path_cli;
        if (!lpath || !*lpath) {
            if (config_env && *config_env) {
                lpath = config_env;
            } else {
                lpath = dsd_user_config_default_path();
            }
        }
        if (!lpath || !*lpath) {
            fprintf(stderr, "No config file path specified or found.\n");
            return 1;
        }

        const char* names[32];
        char names_buf[1024];
        int count = dsd_user_config_list_profiles(lpath, names, names_buf, sizeof names_buf, 32);

        if (count < 0) {
            fprintf(stderr, "Failed to read config file: %s\n", lpath);
            return 1;
        }

        if (count == 0) {
            printf("No profiles found in %s\n", lpath);
        } else {
            printf("Profiles in %s:\n", lpath);
            for (int i = 0; i < count; i++) {
                printf("  %s\n", names[i]);
            }
        }
        return 0;
    }

    // Print banner only if not a one-shot action
    LOG_NOTICE("------------------------------------------------------------------------------\n");
    LOG_NOTICE("| Digital Speech Decoder: DSD-neo %s (%s) \n", GIT_TAG, GIT_HASH);
    LOG_NOTICE("------------------------------------------------------------------------------\n");

    LOG_NOTICE("MBElib-neo Version: %s\n", versionstr);

#ifdef USE_CODEC2
    LOG_NOTICE("CODEC2 Support Enabled\n");
#endif

    // All long-option parsing, environment mapping, and the DMR TIII LCN
    // calculator one-shot flow are now handled inside dsd_parse_args().

    // If user requested it explicitly, or if there are no CLI args and no
    // user config, offer interactive bootstrap. The CLI flag overrides
    // any env-based skip (DSD_NEO_NO_BOOTSTRAP).
    if (force_bootstrap_cli || (argc <= 1 && !user_cfg_loaded)) {
        if (force_bootstrap_cli) {
            dsd_unsetenv("DSD_NEO_NO_BOOTSTRAP");
        }
        dsd_bootstrap_interactive(opts, state);
    }
    return dsd_engine_run(opts, state);
}
