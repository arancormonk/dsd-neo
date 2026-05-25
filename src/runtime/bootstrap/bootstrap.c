// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/input_spec.h>
#include <dsd-neo/runtime/log.h>
#include <mbelib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/runtime/config_schema.h"

static void
bootstrap_set_exit_rc(int* out_exit_rc, int rc) {
    if (out_exit_rc) {
        *out_exit_rc = rc;
    }
}

#define DSD_BOOTSTRAP_LOCAL_PATH_MAX 1024

static int
bootstrap_open_local_config_path(const char* cfg_path, char* resolved, size_t resolved_size, int* out_exit_rc,
                                 FILE** out_stream) {
    if (!out_stream) {
        bootstrap_set_exit_rc(out_exit_rc, 1);
        return 0;
    }
    *out_stream = dsd_fopen_existing_local_file(cfg_path, resolved, resolved_size);
    if (*out_stream) {
        return 1;
    }
    LOG_ERROR("Config path must name an existing local file without path separators or '..'\n");
    bootstrap_set_exit_rc(out_exit_rc, 1);
    return 0;
}

static int
bootstrap_open_local_config_path_for_stderr(const char* cfg_path, char* resolved, size_t resolved_size,
                                            int* out_exit_rc, FILE** out_stream) {
    if (!out_stream) {
        bootstrap_set_exit_rc(out_exit_rc, 1);
        return 0;
    }
    *out_stream = dsd_fopen_existing_local_file(cfg_path, resolved, resolved_size);
    if (*out_stream) {
        return 1;
    }
    DSD_FPRINTF(stderr, "Config path must name an existing local file without path separators or '..'\n");
    bootstrap_set_exit_rc(out_exit_rc, 1);
    return 0;
}

static int
bootstrap_is_ini_path(const char* path) {
    if (!path) {
        return 0;
    }
    size_t n = strlen(path);
    if (n < 4) {
        return 0;
    }
    const char* ext = path + (n - 4);
    return ext[0] == '.' && tolower((unsigned char)ext[1]) == 'i' && tolower((unsigned char)ext[2]) == 'n'
           && tolower((unsigned char)ext[3]) == 'i';
}

typedef struct {
    int enable_config_cli;
    int force_bootstrap_cli;
    int print_config_cli;
    int dump_template_cli;
    int validate_config_cli;
    int strict_config_cli;
    int list_profiles_cli;
    int cli_file_rate_override;
    const char* config_path_cli;
    const char* profile_cli;
    const char* validate_path_cli;
    int cfg_path_positional_ini;
} bootstrap_cli_args;

static void
bootstrap_disable_autosave(dsd_state* state) {
    state->config_autosave_enabled = 0;
    state->config_autosave_path[0] = '\0';
}

static void
bootstrap_enable_autosave_for_path(dsd_state* state, const char* cfg_path) {
    state->config_autosave_enabled = 1;
    DSD_SNPRINTF(state->config_autosave_path, sizeof state->config_autosave_path, "%s", cfg_path);
    state->config_autosave_path[sizeof state->config_autosave_path - 1] = '\0';
}

static int
bootstrap_cli_try_flag(const char* arg, const char* flag, int* out_flag) {
    if (strcmp(arg, flag) != 0) {
        return 0;
    }
    *out_flag = 1;
    return 1;
}

static int
bootstrap_cli_try_config_option(int argc, char** argv, int* i, bootstrap_cli_args* args) {
    if (strcmp(argv[*i], "--config") != 0) {
        return 0;
    }
    args->enable_config_cli = 1;
    if (*i + 1 < argc && argv[*i + 1][0] != '-') {
        args->config_path_cli = argv[++(*i)];
    }
    return 1;
}

static int
bootstrap_cli_try_validate_option(int argc, char** argv, int* i, bootstrap_cli_args* args) {
    if (strcmp(argv[*i], "--validate-config") != 0) {
        return 0;
    }
    args->validate_config_cli = 1;
    if (*i + 1 < argc && argv[*i + 1][0] != '-') {
        args->validate_path_cli = argv[++(*i)];
    }
    return 1;
}

static int
bootstrap_cli_try_profile_option(int argc, char** argv, int* i, bootstrap_cli_args* args) {
    if (strcmp(argv[*i], "--profile") != 0) {
        return 0;
    }
    if (*i + 1 < argc) {
        args->profile_cli = argv[++(*i)];
    }
    return 1;
}

static int
bootstrap_cli_try_file_rate_override(int argc, char** argv, int* i, bootstrap_cli_args* args) {
    if (strcmp(argv[*i], "-s") == 0) {
        args->cli_file_rate_override = 1;
        if (*i + 1 < argc) {
            ++(*i);
        }
        return 1;
    }
    if (strncmp(argv[*i], "-s", 2) == 0 && argv[*i][2] != '\0') {
        args->cli_file_rate_override = 1;
        return 1;
    }
    return 0;
}

static void
bootstrap_parse_cli_args(int argc, char** argv, bootstrap_cli_args* args) {
    if (!args) {
        return;
    }
    DSD_MEMSET(args, 0, sizeof(*args));
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (bootstrap_cli_try_config_option(argc, argv, &i, args)) {
            continue;
        }
        if (bootstrap_cli_try_flag(arg, "--interactive-setup", &args->force_bootstrap_cli)) {
            continue;
        }
        if (bootstrap_cli_try_flag(arg, "--print-config", &args->print_config_cli)) {
            continue;
        }
        if (bootstrap_cli_try_flag(arg, "--dump-config-template", &args->dump_template_cli)) {
            continue;
        }
        if (bootstrap_cli_try_validate_option(argc, argv, &i, args)) {
            continue;
        }
        if (bootstrap_cli_try_flag(arg, "--strict-config", &args->strict_config_cli)) {
            continue;
        }
        if (bootstrap_cli_try_profile_option(argc, argv, &i, args)) {
            continue;
        }
        if (bootstrap_cli_try_flag(arg, "--list-profiles", &args->list_profiles_cli)) {
            continue;
        }
        (void)bootstrap_cli_try_file_rate_override(argc, argv, &i, args);
    }
}

static void
bootstrap_apply_positional_ini_shortcut(int argc, char** argv, bootstrap_cli_args* args) {
    if (!args || args->enable_config_cli) {
        return;
    }
    if (argc == 2 && argv[1] != NULL && argv[1][0] != '-' && bootstrap_is_ini_path(argv[1])) {
        args->enable_config_cli = 1;
        args->config_path_cli = argv[1];
        args->cfg_path_positional_ini = 1;
    }
}

static int
bootstrap_config_load_requested(const bootstrap_cli_args* args, const char* config_env) {
    return args->enable_config_cli || (config_env && *config_env);
}

static const char*
bootstrap_explicit_config_path_for_load(const bootstrap_cli_args* args, const char* config_env) {
    if (args->config_path_cli && *args->config_path_cli) {
        return args->config_path_cli;
    }
    if (config_env && *config_env) {
        return config_env;
    }
    return NULL;
}

static int
bootstrap_load_user_config_from_path(const bootstrap_cli_args* args, const char* cfg_path, dsdneoUserConfig* user_cfg) {
    if (args->profile_cli && *args->profile_cli) {
        return dsd_user_config_load_profile(cfg_path, args->profile_cli, user_cfg);
    }
    return dsd_user_config_load(cfg_path, user_cfg);
}

static int
bootstrap_load_user_config_from_stream(const bootstrap_cli_args* args, FILE* cfg_stream, const char* cfg_path,
                                       dsdneoUserConfig* user_cfg) {
    if (args->profile_cli && *args->profile_cli) {
        return dsd_user_config_load_profile_stream(cfg_stream, cfg_path, args->profile_cli, user_cfg);
    }
    return dsd_user_config_load_stream(cfg_stream, cfg_path, user_cfg);
}

static void
bootstrap_handle_loaded_user_config(dsd_opts* opts, dsd_state* state, const bootstrap_cli_args* args,
                                    int explicit_profile_selected, const dsdneoUserConfig* user_cfg,
                                    int* user_cfg_loaded, const char* cfg_path) {
    dsd_apply_user_config_to_opts_pre_cli(user_cfg, opts, state);
    *user_cfg_loaded = 1;
    if (explicit_profile_selected) {
        state->config_autosave_enabled = 0;
        LOG_NOTICE("Autosave disabled for profiled config %s to avoid overwriting profile sections.\n", cfg_path);
        LOG_NOTICE("Loaded user config from %s (profile: %s)\n", cfg_path, args->profile_cli);
        return;
    }
    LOG_NOTICE("Loaded user config from %s\n", cfg_path);
}

static int
bootstrap_handle_failed_user_config_load(const bootstrap_cli_args* args, int explicit_profile_selected,
                                         const char* config_env, const char* cfg_path, int* out_exit_rc) {
    if (explicit_profile_selected) {
        LOG_ERROR("Profile '%s' not found in config file %s\n", args->profile_cli, cfg_path);
        bootstrap_set_exit_rc(out_exit_rc, 1);
        return DSD_BOOTSTRAP_ERROR;
    }
    if (args->config_path_cli || config_env || args->enable_config_cli) {
        LOG_WARNING("Failed to load config file from %s; proceeding without config.\n", cfg_path);
    }
    return DSD_BOOTSTRAP_CONTINUE;
}

static int
bootstrap_load_user_config_if_requested(dsd_opts* opts, dsd_state* state, const bootstrap_cli_args* args,
                                        const char* config_env, int explicit_profile_selected,
                                        dsdneoUserConfig* user_cfg, int* user_cfg_loaded, int* out_exit_rc) {
    if (!opts || !state || !args || !user_cfg || !user_cfg_loaded) {
        return DSD_BOOTSTRAP_ERROR;
    }
    if (!bootstrap_config_load_requested(args, config_env)) {
        bootstrap_disable_autosave(state);
        return DSD_BOOTSTRAP_CONTINUE;
    }

    char resolved_cfg_path[DSD_BOOTSTRAP_LOCAL_PATH_MAX];
    FILE* cfg_stream = NULL;
    int load_rc = -1;
    const char* explicit_cfg_path = bootstrap_explicit_config_path_for_load(args, config_env);
    const char* cfg_path = NULL;
    if (explicit_cfg_path) {
        if (!bootstrap_open_local_config_path(explicit_cfg_path, resolved_cfg_path, sizeof resolved_cfg_path,
                                              out_exit_rc, &cfg_stream)) {
            return DSD_BOOTSTRAP_ERROR;
        }
        cfg_path = resolved_cfg_path;
        bootstrap_enable_autosave_for_path(state, cfg_path);
        load_rc = bootstrap_load_user_config_from_stream(args, cfg_stream, cfg_path, user_cfg);
        fclose(cfg_stream);
    } else {
        const char* default_cfg_path = dsd_user_config_default_path();
        if (!default_cfg_path || !*default_cfg_path) {
            return DSD_BOOTSTRAP_CONTINUE;
        }
        cfg_path = default_cfg_path;
        bootstrap_enable_autosave_for_path(state, cfg_path);
        load_rc = bootstrap_load_user_config_from_path(args, default_cfg_path, user_cfg);
    }

    if (load_rc == 0) {
        bootstrap_handle_loaded_user_config(opts, state, args, explicit_profile_selected, user_cfg, user_cfg_loaded,
                                            cfg_path);
        return DSD_BOOTSTRAP_CONTINUE;
    }

    return bootstrap_handle_failed_user_config_load(args, explicit_profile_selected, config_env, cfg_path, out_exit_rc);
}

static int
bootstrap_parse_runtime_args(dsd_opts* opts, dsd_state* state, int argc, char** argv, int cfg_path_positional_ini,
                             const dsdneoUserConfig* user_cfg, int user_cfg_loaded, int cli_file_rate_override,
                             int* argc_effective, int* out_exit_rc) {
    int parse_exit_rc = 1;
    const int parse_argc = cfg_path_positional_ini ? 1 : argc;
    int parse_rc = dsd_parse_args(parse_argc, argv, opts, state, argc_effective, &parse_exit_rc);
    if (parse_rc == DSD_PARSE_ONE_SHOT) {
        bootstrap_set_exit_rc(out_exit_rc, parse_exit_rc);
        return DSD_BOOTSTRAP_EXIT;
    }
    if (parse_rc == DSD_PARSE_ERROR) {
        bootstrap_set_exit_rc(out_exit_rc, parse_exit_rc);
        return DSD_BOOTSTRAP_ERROR;
    }
    if (parse_rc != DSD_PARSE_CONTINUE) {
        bootstrap_set_exit_rc(out_exit_rc, 1);
        return DSD_BOOTSTRAP_ERROR;
    }
    if (user_cfg_loaded && !cli_file_rate_override && user_cfg) {
        dsd_finalize_user_config_file_input_after_cli(user_cfg, opts, state);
    }
    return DSD_BOOTSTRAP_CONTINUE;
}

static int
bootstrap_handle_print_config(const bootstrap_cli_args* args, dsd_opts* opts, const dsd_state* state,
                              int* out_exit_rc) {
    if (!args->print_config_cli) {
        return DSD_BOOTSTRAP_CONTINUE;
    }
    int soapy_tuning_applied = 0;
    (void)dsd_normalize_soapy_input_spec(opts, &soapy_tuning_applied);
    dsdneoUserConfig eff;
    dsd_snapshot_opts_to_user_config(opts, state, &eff);
    dsd_user_config_render_ini(&eff, stdout);
    bootstrap_set_exit_rc(out_exit_rc, 0);
    return DSD_BOOTSTRAP_EXIT;
}

static int
bootstrap_handle_dump_template(const bootstrap_cli_args* args, int* out_exit_rc) {
    if (!args->dump_template_cli) {
        return DSD_BOOTSTRAP_CONTINUE;
    }
    dsd_user_config_render_template(stdout);
    bootstrap_set_exit_rc(out_exit_rc, 0);
    return DSD_BOOTSTRAP_EXIT;
}

static int
bootstrap_handle_validate_config(const bootstrap_cli_args* args, const char* config_env, int* out_exit_rc) {
    if (!args->validate_config_cli) {
        return DSD_BOOTSTRAP_CONTINUE;
    }
    const char* explicit_vpath = args->validate_path_cli && *args->validate_path_cli
                                     ? args->validate_path_cli
                                     : bootstrap_explicit_config_path_for_load(args, config_env);
    char resolved_vpath[DSD_BOOTSTRAP_LOCAL_PATH_MAX];
    const char* vpath = NULL;
    FILE* validate_stream = NULL;
    dsdcfg_diagnostics_t diags;
    int rc = -1;
    if (explicit_vpath) {
        if (!bootstrap_open_local_config_path_for_stderr(explicit_vpath, resolved_vpath, sizeof resolved_vpath,
                                                         out_exit_rc, &validate_stream)) {
            return DSD_BOOTSTRAP_ERROR;
        }
        vpath = resolved_vpath;
        rc = dsd_user_config_validate_stream(validate_stream, &diags);
        fclose(validate_stream);
    } else {
        const char* default_vpath = dsd_user_config_default_path();
        if (!default_vpath || !*default_vpath) {
            DSD_FPRINTF(stderr, "No config file path specified or found.\n");
            bootstrap_set_exit_rc(out_exit_rc, 1);
            return DSD_BOOTSTRAP_ERROR;
        }
        vpath = default_vpath;
        rc = dsd_user_config_validate(default_vpath, &diags);
    }

    if (diags.count > 0) {
        dsdcfg_diags_print(&diags, stderr, vpath);
    } else {
        DSD_FPRINTF(stderr, "%s: OK\n", vpath);
    }

    int exit_code = 0;
    if (rc != 0 || diags.error_count > 0) {
        exit_code = 1;
    } else if (args->strict_config_cli && diags.warning_count > 0) {
        exit_code = 2;
    }

    dsd_user_config_diags_free(&diags);
    bootstrap_set_exit_rc(out_exit_rc, exit_code);
    return DSD_BOOTSTRAP_EXIT;
}

static int
bootstrap_handle_list_profiles(const bootstrap_cli_args* args, const char* config_env, int* out_exit_rc) {
    if (!args->list_profiles_cli) {
        return DSD_BOOTSTRAP_CONTINUE;
    }
    const char* explicit_lpath = bootstrap_explicit_config_path_for_load(args, config_env);
    char resolved_lpath[DSD_BOOTSTRAP_LOCAL_PATH_MAX];
    const char* lpath = NULL;
    const char* names[32];
    char names_buf[1024];
    int count = -1;
    if (explicit_lpath) {
        FILE* list_stream = NULL;
        if (!bootstrap_open_local_config_path_for_stderr(explicit_lpath, resolved_lpath, sizeof resolved_lpath,
                                                         out_exit_rc, &list_stream)) {
            return DSD_BOOTSTRAP_EXIT;
        }
        lpath = resolved_lpath;
        count = dsd_user_config_list_profiles_stream(list_stream, names, names_buf, sizeof names_buf, 32);
        fclose(list_stream);
    } else {
        const char* default_lpath = dsd_user_config_default_path();
        if (!default_lpath || !*default_lpath) {
            DSD_FPRINTF(stderr, "No config file path specified or found.\n");
            bootstrap_set_exit_rc(out_exit_rc, 1);
            return DSD_BOOTSTRAP_EXIT;
        }
        lpath = default_lpath;
        count = dsd_user_config_list_profiles(default_lpath, names, names_buf, sizeof names_buf, 32);
    }

    if (count < 0) {
        DSD_FPRINTF(stderr, "Failed to read config file: %s\n", lpath);
        bootstrap_set_exit_rc(out_exit_rc, 1);
        return DSD_BOOTSTRAP_EXIT;
    }

    if (count == 0) {
        printf("No profiles found in %s\n", lpath);
    } else {
        printf("Profiles in %s:\n", lpath);
        for (int i = 0; i < count; i++) {
            printf("  %s\n", names[i]);
        }
    }
    bootstrap_set_exit_rc(out_exit_rc, 0);
    return DSD_BOOTSTRAP_EXIT;
}

static const char*
bootstrap_get_config_env_path(const dsd_opts* opts) {
    dsd_neo_config_init(opts);
    const dsdneoRuntimeConfig* rcfg = dsd_neo_get_config();
    if (rcfg && rcfg->config_path_is_set) {
        return rcfg->config_path;
    }
    return NULL;
}

static void
bootstrap_free_argv_copy(char** argv_copy, int argc) {
    if (!argv_copy) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        free(argv_copy[i]);
    }
    free((void*)argv_copy);
}

static int
bootstrap_record_effective_cli_args(dsd_state* state, char** argv, int argc_effective, int* out_argc_effective) {
    if (!state || argc_effective < 0) {
        return DSD_BOOTSTRAP_ERROR;
    }
    char** argv_copy = (char**)calloc((size_t)argc_effective + 1U, sizeof(*argv_copy));
    if (!argv_copy) {
        return DSD_BOOTSTRAP_ERROR;
    }
    for (int i = 0; i < argc_effective; i++) {
        argv_copy[i] = dsd_strdup(argv[i] ? argv[i] : "");
        if (!argv_copy[i]) {
            bootstrap_free_argv_copy(argv_copy, i);
            return DSD_BOOTSTRAP_ERROR;
        }
    }

    bootstrap_free_argv_copy(state->cli_argv, state->cli_argc_effective);
    state->cli_argc_effective = argc_effective;
    state->cli_argv = argv_copy;
    if (out_argc_effective) {
        *out_argc_effective = argc_effective;
    }
    return DSD_BOOTSTRAP_CONTINUE;
}

static void
bootstrap_apply_runtime_config_after_cli(dsd_opts* opts, dsd_state* state) {
    /* Re-parse env-derived config after CLI mapping (CLI sets DSD_NEO_* env overrides). */
    dsd_neo_config_init(opts);
    dsd_apply_runtime_config_to_opts(dsd_neo_get_config(), opts, state);
}

static void
bootstrap_apply_trunk_cli_gating(dsd_opts* opts, int argc_effective, int user_cfg_loaded,
                                 int explicit_profile_selected) {
    if (argc_effective > 1 && user_cfg_loaded && !opts->trunk_cli_seen && !explicit_profile_selected) {
        opts->p25_trunk = 0;
        opts->trunk_enable = 0;
    }
}

static int
bootstrap_handle_post_parse_actions(const bootstrap_cli_args* args, dsd_opts* opts, const dsd_state* state,
                                    const char* config_env, int* out_exit_rc) {
    int boot_rc = bootstrap_handle_print_config(args, opts, state, out_exit_rc);
    if (boot_rc != DSD_BOOTSTRAP_CONTINUE) {
        return boot_rc;
    }
    boot_rc = bootstrap_handle_dump_template(args, out_exit_rc);
    if (boot_rc != DSD_BOOTSTRAP_CONTINUE) {
        return boot_rc;
    }
    boot_rc = bootstrap_handle_validate_config(args, config_env, out_exit_rc);
    if (boot_rc != DSD_BOOTSTRAP_CONTINUE) {
        return boot_rc;
    }
    return bootstrap_handle_list_profiles(args, config_env, out_exit_rc);
}

static void
bootstrap_log_startup_banner(void) {
    LOG_NOTICE("------------------------------------------------------------------------------\n");
    LOG_NOTICE("| Digital Speech Decoder: DSD-neo %s (%s) \n", GIT_TAG, GIT_HASH);
    LOG_NOTICE("------------------------------------------------------------------------------\n");

    const char* versionstr = mbe_versionString();
    LOG_NOTICE("MBElib-neo Version: %s\n", versionstr);

#ifdef USE_CODEC2
    LOG_NOTICE("CODEC2 Support Enabled\n");
#endif
}

static void
bootstrap_maybe_run_interactive(const bootstrap_cli_args* args, int argc, int user_cfg_loaded, dsd_opts* opts,
                                dsd_state* state) {
    if (args->force_bootstrap_cli || (argc <= 1 && !user_cfg_loaded)) {
        if (args->force_bootstrap_cli) {
            dsd_unsetenv("DSD_NEO_NO_BOOTSTRAP");
            dsd_neo_config_init(opts);
        }
        dsd_bootstrap_interactive(opts, state);
    }
}

int
dsd_runtime_bootstrap(int argc, char** argv, dsd_opts* opts, dsd_state* state, int* out_argc_effective,
                      int* out_exit_rc) {
    if (!opts || !state || argc < 0 || !argv) {
        bootstrap_set_exit_rc(out_exit_rc, 1);
        return DSD_BOOTSTRAP_ERROR;
    }

    int argc_effective = argc;
    bootstrap_cli_args args;
    bootstrap_parse_cli_args(argc, argv, &args);
    bootstrap_apply_positional_ini_shortcut(argc, argv, &args);

    const char* config_env = bootstrap_get_config_env_path(opts);

    const int explicit_profile_selected = (args.profile_cli && *args.profile_cli) ? 1 : 0;
    int user_cfg_loaded = 0;
    dsdneoUserConfig user_cfg;
    DSD_MEMSET(&user_cfg, 0, sizeof user_cfg);

    bootstrap_disable_autosave(state);

    int boot_rc = bootstrap_load_user_config_if_requested(opts, state, &args, config_env, explicit_profile_selected,
                                                          &user_cfg, &user_cfg_loaded, out_exit_rc);
    if (boot_rc != DSD_BOOTSTRAP_CONTINUE) {
        return boot_rc;
    }

    boot_rc = bootstrap_parse_runtime_args(opts, state, argc, argv, args.cfg_path_positional_ini, &user_cfg,
                                           user_cfg_loaded, args.cli_file_rate_override, &argc_effective, out_exit_rc);
    if (boot_rc != DSD_BOOTSTRAP_CONTINUE) {
        return boot_rc;
    }

    boot_rc = bootstrap_record_effective_cli_args(state, argv, argc_effective, out_argc_effective);
    if (boot_rc != DSD_BOOTSTRAP_CONTINUE) {
        bootstrap_set_exit_rc(out_exit_rc, 1);
        return boot_rc;
    }
    bootstrap_apply_runtime_config_after_cli(opts, state);
    bootstrap_apply_trunk_cli_gating(opts, argc_effective, user_cfg_loaded, explicit_profile_selected);

    boot_rc = bootstrap_handle_post_parse_actions(&args, opts, state, config_env, out_exit_rc);
    if (boot_rc != DSD_BOOTSTRAP_CONTINUE) {
        return boot_rc;
    }

    bootstrap_log_startup_banner();
    bootstrap_maybe_run_interactive(&args, argc, user_cfg_loaded, opts, state);

    bootstrap_set_exit_rc(out_exit_rc, 0);
    return DSD_BOOTSTRAP_CONTINUE;
}
