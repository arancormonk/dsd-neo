// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/config_files.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <stddef.h>
#include <stdlib.h>

/* A loaded file's apply: the autosave it brings (@p autosave_enabled, @p path) is adopted by the decoder only once the
   config stays applied, so a refused load leaves the autosave where it was. Heap: the config is tens of KB. */
static dsd_app_config_load_payload*
config_load_new(int autosave_enabled, const char* path, const char* profile) {
    dsd_app_config_load_payload* load = (dsd_app_config_load_payload*)calloc(1, sizeof *load);
    if (!load) {
        return NULL;
    }
    load->autosave.autosave_enabled = autosave_enabled ? 1 : 0;
    DSD_SNPRINTF(load->autosave.path, sizeof load->autosave.path, "%s", path);
    DSD_SNPRINTF(load->profile, sizeof load->profile, "%s", profile ? profile : "");
    return load;
}

int
dsd_app_config_load_submit(const char* path) {
    if (!path || !*path) {
        return -1;
    }
    dsd_app_config_load_payload* load = config_load_new(1, path, NULL);
    if (!load) {
        return -1;
    }
    if (dsd_user_config_load(path, &load->config) != 0) {
        free(load);
        return -1;
    }
    /* The decoder says whether it loaded: "Config loaded from ..." or "Config not applied: ...". */
    const int submitted = dsd_app_command_load_config(load);
    free(load);
    return submitted > 0 ? 0 : -1;
}

int
dsd_app_config_load_profile_submit(const char* path, const char* profile) {
    if (!path || !*path || !profile || !*profile) {
        return -1;
    }
    dsd_app_config_load_payload* load = config_load_new(0, path, profile);
    if (!load) {
        return -1;
    }
    if (dsd_user_config_load_profile(path, profile, &load->config) != 0) {
        free(load);
        return -1;
    }
    /* The decoder says whether it loaded: "Profile loaded: ..." or "Config not applied: ...". */
    const int submitted = dsd_app_command_load_config(load);
    free(load);
    return submitted > 0 ? 0 : -1;
}

int
dsd_app_config_list_profiles(const char* path, const char** names, char* names_buf, size_t names_buf_size,
                             int max_names) {
    if (!path || !*path) {
        return -1;
    }
    return dsd_user_config_list_profiles(path, names, names_buf, names_buf_size, max_names);
}

int
dsd_app_config_default_missing(char* path, size_t path_size) {
    if (!path || path_size == 0U) {
        return 0;
    }
    path[0] = '\0';
    const char* def = dsd_user_config_default_path();
    if (!def || !*def) {
        return 0;
    }
    dsd_stat_t st;
    if (dsd_stat_path(def, &st) == 0) {
        return 0;
    }
    const int n = DSD_SNPRINTF(path, path_size, "%s", def);
    if (n < 0 || (size_t)n >= path_size) {
        path[0] = '\0';
        return 0;
    }
    return 1;
}

int
dsd_app_config_save(const char* path, const dsd_opts* opts, const dsd_state* state, int adopt_autosave) {
    if (!path || !*path || !opts || !state) {
        return -1;
    }
    dsdneoUserConfig* cfg = (dsdneoUserConfig*)calloc(1, sizeof *cfg);
    if (!cfg) {
        return -1;
    }
    dsd_snapshot_opts_to_user_config(opts, state, cfg);
    const int rc = dsd_user_config_save_atomic(path, cfg);
    free(cfg);
    if (rc != 0) {
        return -1;
    }
    if (adopt_autosave) {
        dsd_app_config_metadata_payload payload;
        DSD_MEMSET(&payload, 0, sizeof payload);
        payload.autosave_enabled = 1;
        DSD_SNPRINTF(payload.path, sizeof payload.path, "%s", path);
        (void)dsd_app_command_set_config_metadata(&payload);
    }
    return 0;
}
