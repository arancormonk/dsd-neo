// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validation and diagnostics for INI-based user configuration.
 */

#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/config_schema.h>
#include <dsd-neo/runtime/path_policy.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "config_user_internal.h"
#include "dsd-neo/core/safe_api.h"

static int
validate_bool_value(const char* val) {
    int parsed = 0;
    return user_config_parse_bool_value(val, &parsed);
}

namespace {
static const int DSD_CONFIG_VALIDATE_MAX_PROFILES = 64;
static const size_t DSD_CONFIG_VALIDATE_PROFILE_BUF_SIZE = 4096U;
} // namespace

static int
validate_enum_value(const char* val, const char* allowed) {
    if (!val || !allowed) {
        return -1;
    }

    char buf[256];
    DSD_SNPRINTF(buf, sizeof buf, "%s", allowed);
    buf[sizeof buf - 1] = '\0';

    char* save = NULL;
    char* tok = dsd_strtok_r(buf, "|", &save);
    while (tok) {
        if (dsd_strcasecmp(val, tok) == 0) {
            return 0;
        }
        tok = dsd_strtok_r(NULL, "|", &save);
    }
    return -1;
}

static int
validate_enum_value_with_diagnostic(const char* val, const char* allowed, dsdcfg_diagnostics_t* diags, int line_num,
                                    const char* diag_section, const char* diag_key) {
    if (!val) {
        return -1;
    }
    if (allowed && validate_enum_value(val, allowed) != 0) {
        char msg[256];
        DSD_SNPRINTF(msg, sizeof msg, "Invalid value '%s' (allowed: %s)", val, allowed);
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, diag_section ? diag_section : "", diag_key ? diag_key : "",
                         msg);
        return -1;
    }

    return 0;
}

static void
validate_entry_value(const dsdcfg_schema_entry_t* entry, const char* val, dsdcfg_diagnostics_t* diags, int line_num,
                     const char* diag_section, const char* diag_key) {
    if (!entry || !val || !diags) {
        return;
    }

    switch (entry->type) {
        case DSDCFG_TYPE_BOOL:
            if (validate_bool_value(val) != 0) {
                char msg[128];
                DSD_SNPRINTF(msg, sizeof msg, "Invalid boolean value '%s' (use true/false/yes/no/1/0)", val);
                dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, diag_section, diag_key, msg);
            }
            break;

        case DSDCFG_TYPE_INT: {
            int int_val = 0;
            if (user_config_parse_int_value(val, &int_val) != 0) {
                char msg[128];
                DSD_SNPRINTF(msg, sizeof msg, "Invalid integer value '%s'", val);
                dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, diag_section, diag_key, msg);
            } else if ((entry->min_val != 0 || entry->max_val != 0)
                       && (int_val < entry->min_val || int_val > entry->max_val)) {
                char msg[128];
                DSD_SNPRINTF(msg, sizeof msg, "Value %d is out of range [%d, %d]", int_val, entry->min_val,
                             entry->max_val);
                dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, diag_section, diag_key, msg);
            }
            break;
        }

        case DSDCFG_TYPE_ENUM:
            (void)validate_enum_value_with_diagnostic(val, entry->allowed, diags, line_num, diag_section, diag_key);
            break;

        default: break;
    }
}

static int
is_known_config_section(const char* section) {
    if (!section || !*section) {
        return 0;
    }

    int count = dsdcfg_schema_count();
    for (int i = 0; i < count; i++) {
        const dsdcfg_schema_entry_t* entry = dsdcfg_schema_get(i);
        if (entry && dsd_strcasecmp(entry->section, section) == 0) {
            return 1;
        }
    }

    return 0;
}

static void
handle_section_header(char* p, int line_num, char* current_section, size_t current_section_size,
                      int* is_profile_section, dsdcfg_diagnostics_t* diags) {
    if (!p || !current_section || current_section_size == 0 || !is_profile_section || !diags) {
        return;
    }

    char* end = strchr(p, ']');
    if (!end) {
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, "", "", "Malformed section header");
        return;
    }
    *end = '\0';
    DSD_SNPRINTF(current_section, current_section_size, "%s", p + 1);
    user_config_lowercase_ascii(current_section);

    *is_profile_section = (strncmp(current_section, "profile.", 8) == 0);
    if (!(*is_profile_section) && !is_known_config_section(current_section)) {
        char msg[128];
        DSD_SNPRINTF(msg, sizeof msg, "Unknown section [%s]", current_section);
        dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, current_section, "", msg);
    }
}

static void
validate_top_level_key_value(const char* key, const char* val, dsdcfg_diagnostics_t* diags, int line_num) {
    if (!key || !val || !diags) {
        return;
    }

    if (dsd_strcasecmp(key, "version") == 0) {
        int ver = 0;
        if (user_config_parse_int_value(val, &ver) != 0) {
            dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, "", key, "version must be an integer");
        } else if (ver != 1) {
            dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, "", key,
                             "unsupported persisted config version (only version 1 is accepted)");
        }
        return;
    }

    if (dsd_strcasecmp(key, "include") == 0) {
        if (!val[0]) {
            dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, "", key, "include path is empty");
        }
        return;
    }

    std::string msg = std::string("Unknown top-level key '") + key + "'";
    dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, "", key, msg.c_str());
}

static void
validate_profile_key_value(char* key, const char* val, dsdcfg_diagnostics_t* diags, int line_num,
                           const char* current_section) {
    if (!key || !val || !diags || !current_section) {
        return;
    }

    char* dot = strchr(key, '.');
    if (!dot) {
        std::string msg = std::string("Profile key '") + key + "' should use section.key format";
        dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, current_section, key, msg.c_str());
        return;
    }

    *dot = '\0';
    const char* target_sec = key;
    const char* target_key = dot + 1;
    const dsdcfg_schema_entry_t* entry = dsdcfg_schema_find(target_sec, target_key);
    if (!entry) {
        std::string msg = std::string("Unknown key '") + target_sec + "." + target_key + "' in profile";
        dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, current_section, key, msg.c_str());
    } else {
        validate_entry_value(entry, val, diags, line_num, current_section, key);
    }
    *dot = '.';
}

static void
validate_section_key_value(char* key, const char* val, dsdcfg_diagnostics_t* diags, int line_num,
                           const char* current_section) {
    if (!key || !val || !diags || !current_section) {
        return;
    }

    std::string key_lc = key;
    if (!key_lc.empty()) {
        user_config_lowercase_ascii(&key_lc[0]);
    }

    const dsdcfg_schema_entry_t* entry = dsdcfg_schema_find(current_section, key_lc.c_str());
    if (!entry) {
        std::string msg = std::string("Unknown key '") + key + "' in section [" + current_section + "]";
        dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, current_section, key, msg.c_str());
        return;
    }

    validate_entry_value(entry, val, diags, line_num, current_section, key);
}

static void
validate_config_line(char* p, int line_num, const char* current_section, int is_profile_section,
                     dsdcfg_diagnostics_t* diags) {
    if (!p || !current_section || !diags) {
        return;
    }

    char* eq = strchr(p, '=');
    if (!eq) {
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, current_section, "",
                         "Line is not a comment, section, or key=value");
        return;
    }

    *eq = '\0';
    char* key = user_config_trim_ascii_whitespace(p);
    char* val = user_config_trim_ascii_whitespace(eq + 1);
    user_config_strip_wrapping_quotes(val);

    if (current_section[0] == '\0') {
        validate_top_level_key_value(key, val, diags, line_num);
        return;
    }
    if (is_profile_section) {
        validate_profile_key_value(key, val, diags, line_num, current_section);
        return;
    }
    validate_section_key_value(key, val, diags, line_num, current_section);
}

static void
validate_composed_trunk_scan_requirements(const dsdneoUserConfig* cfg, const char* section, const char* key,
                                          dsdcfg_diagnostics_t* diags) {
    if (!cfg || !diags || !cfg->trunk_scan_enabled || cfg->trunk_scan_targets_csv[0] != '\0') {
        return;
    }
    dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, section ? section : "trunk_scan", key ? key : "targets_csv",
                     "targets_csv is required when trunk_scan.enabled is true");
}

static void
validate_composed_trunk_scan_channel_map_conflict(const dsdneoUserConfig* cfg, const char* section, const char* key,
                                                  dsdcfg_diagnostics_t* diags) {
    if (!cfg || !diags || !cfg->trunk_scan_enabled || cfg->trunk_chan_csv[0] == '\0') {
        return;
    }
    dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, section ? section : "trunking", key ? key : "chan_csv",
                     "trunking.chan_csv cannot be combined with trunk_scan.enabled; use per-target chan_csv values");
}

static void
validate_composed_config_base(const dsdneoUserConfig* cfg, dsdcfg_diagnostics_t* diags) {
    validate_composed_trunk_scan_requirements(cfg, "trunk_scan", "targets_csv", diags);
    validate_composed_trunk_scan_channel_map_conflict(cfg, "trunking", "chan_csv", diags);
}

static void
validate_composed_profile_config(const char* profile_name, const dsdneoUserConfig* cfg, dsdcfg_diagnostics_t* diags) {
    char section[64];
    DSD_SNPRINTF(section, sizeof section, "profile.%s", profile_name ? profile_name : "");
    section[sizeof section - 1] = '\0';
    validate_composed_trunk_scan_requirements(cfg, section, "trunk_scan.targets_csv", diags);
    validate_composed_trunk_scan_channel_map_conflict(cfg, section, "trunking.chan_csv", diags);
}

static void
validate_composed_config_path(const char* path, dsdcfg_diagnostics_t* diags) {
    if (!path || !diags) {
        return;
    }

    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load_profile(path, NULL, &cfg);
    if (load_rc != 0) {
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, "", "", "composed config or included file could not be loaded");
        return;
    }
    validate_composed_config_base(&cfg, diags);

    const char* names[DSD_CONFIG_VALIDATE_MAX_PROFILES];
    char names_buf[DSD_CONFIG_VALIDATE_PROFILE_BUF_SIZE];
    int count =
        dsd_user_config_list_profiles(path, names, names_buf, sizeof names_buf, DSD_CONFIG_VALIDATE_MAX_PROFILES);
    if (count <= 0) {
        return;
    }

    for (int i = 0; i < count; i++) {
        load_rc = dsd_user_config_load_profile(path, names[i], &cfg);
        if (load_rc != 0) {
            dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, "", "",
                             "composed profile or included file could not be loaded");
            return;
        }
        validate_composed_profile_config(names[i], &cfg, diags);
    }
}

static int validate_config_stream_syntax_append(FILE* stream, const char* source_path, int depth,
                                                const char** include_stack, int include_stack_size,
                                                dsdcfg_diagnostics_t* diags);

static void
set_new_diagnostic_sources(dsdcfg_diagnostics_t* diags, int first_index, const char* source_path) {
    if (!diags || !source_path || source_path[0] == '\0') {
        return;
    }
    for (int i = first_index; i < diags->count; i++) {
        if (diags->items[i].source_path[0] == '\0') {
            DSD_SNPRINTF(diags->items[i].source_path, sizeof diags->items[i].source_path, "%s", source_path);
            diags->items[i].source_path[sizeof diags->items[i].source_path - 1] = '\0';
        }
    }
}

static int
validate_included_config_path(const char* path, int directive_line, int depth, const char** include_stack,
                              int include_stack_size, dsdcfg_diagnostics_t* diags) {
    char opened_path[2048];
    FILE* fp = dsd_path_fopen_user_read_file(path, opened_path, sizeof opened_path);
    if (!fp) {
        char msg[256];
        DSD_SNPRINTF(msg, sizeof msg, "Cannot open included config '%s': %s", path, strerror(errno));
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, directive_line, "", "include", msg);
        return -1;
    }

    int rc = validate_config_stream_syntax_append(fp, opened_path, depth, include_stack, include_stack_size, diags);
    fclose(fp);
    return rc;
}

static void
validate_include_directive(const char* source_path, const char* requested_path, int directive_line, int depth,
                           const char** include_stack, int include_stack_size, dsdcfg_diagnostics_t* diags) {
    if (!requested_path || requested_path[0] == '\0') {
        return;
    }

    char resolved_path[2048];
    if (dsd_path_resolve_relative_to_file(source_path, requested_path, resolved_path, sizeof resolved_path) != 0) {
        char msg[256];
        DSD_SNPRINTF(msg, sizeof msg, "Cannot resolve include '%s': %s", requested_path, strerror(errno));
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, directive_line, "", "include", msg);
        return;
    }
    if (user_config_include_stack_contains_path(include_stack, include_stack_size, resolved_path)) {
        return;
    }
    if (depth >= DSD_USER_CONFIG_MAX_INCLUDE_DEPTH) {
        char msg[128];
        DSD_SNPRINTF(msg, sizeof msg, "include nesting exceeds maximum depth %d", DSD_USER_CONFIG_MAX_INCLUDE_DEPTH);
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, directive_line, "", "include", msg);
        return;
    }

    const char* nested_stack[DSD_USER_CONFIG_MAX_INCLUDE_DEPTH + 1];
    int nested_stack_size = 0;
    for (int i = 0; i < include_stack_size && i < DSD_USER_CONFIG_MAX_INCLUDE_DEPTH; i++) {
        nested_stack[nested_stack_size++] = include_stack[i];
    }
    nested_stack[nested_stack_size++] = resolved_path;
    (void)validate_included_config_path(resolved_path, directive_line, depth + 1, nested_stack, nested_stack_size,
                                        diags);
}

namespace {

struct config_validation_scan_t {
    const char* source_path;
    int depth;
    const char** include_stack;
    int include_stack_size;
    char current_section[64];
    int is_profile_section;
    int scan_includes;
};

} // namespace

static void
set_scan_diagnostic_sources(const config_validation_scan_t* scan, dsdcfg_diagnostics_t* diags, int first_index) {
    if (scan->depth > 0) {
        set_new_diagnostic_sources(diags, first_index, scan->source_path);
    }
}

static int
parse_validation_include(config_validation_scan_t* scan, const char* line, char* include_path,
                         size_t include_path_size) {
    if (!scan->scan_includes) {
        return 0;
    }
    char include_line[1024];
    DSD_SNPRINTF(include_line, sizeof include_line, "%s", line);
    int rc = user_config_parse_include_directive_line(include_line, include_path, include_path_size);
    if (rc < 0) {
        scan->scan_includes = 0;
        return 0;
    }
    return rc;
}

static void
validate_config_stream_line(char* line, int line_num, config_validation_scan_t* scan, dsdcfg_diagnostics_t* diags) {
    char include_path[1024];
    int include_parse_rc = parse_validation_include(scan, line, include_path, sizeof include_path);
    char* p = user_config_trim_ascii_whitespace(line);
    if (p[0] == '\0' || p[0] == '#' || p[0] == ';') {
        return;
    }

    if (p[0] == '[') {
        int first_diag = diags->count;
        handle_section_header(p, line_num, scan->current_section, sizeof scan->current_section,
                              &scan->is_profile_section, diags);
        set_scan_diagnostic_sources(scan, diags, first_diag);
        return;
    }

    user_config_strip_inline_comment(p);
    p = user_config_trim_ascii_whitespace(p);
    if (p[0] == '\0') {
        return;
    }

    int first_diag = diags->count;
    validate_config_line(p, line_num, scan->current_section, scan->is_profile_section, diags);
    set_scan_diagnostic_sources(scan, diags, first_diag);
    if (include_parse_rc <= 0) {
        return;
    }

    first_diag = diags->count;
    validate_include_directive(scan->source_path, include_path, line_num, scan->depth, scan->include_stack,
                               scan->include_stack_size, diags);
    set_scan_diagnostic_sources(scan, diags, first_diag);
}

static int
validate_config_stream_syntax_append(FILE* stream, const char* source_path, int depth, const char** include_stack,
                                     int include_stack_size, dsdcfg_diagnostics_t* diags) {
    if (!diags) {
        return -1;
    }
    int errors_before = diags->error_count;
    if (!stream) {
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, "", "", "No config stream provided");
        return -1;
    }

    if (fseek(stream, 0L, SEEK_SET) != 0) {
        int first_diag = diags->count;
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, "", "", "Cannot seek config stream");
        config_validation_scan_t scan = {};
        scan.source_path = source_path;
        scan.depth = depth;
        set_scan_diagnostic_sources(&scan, diags, first_diag);
        return -1;
    }

    char line[1024];
    config_validation_scan_t scan = {};
    scan.source_path = source_path;
    scan.depth = depth;
    scan.include_stack = include_stack;
    scan.include_stack_size = include_stack_size;
    scan.scan_includes = 1;
    int line_num = 0;

    while (fgets(line, sizeof line, stream)) {
        line_num++;
        validate_config_stream_line(line, line_num, &scan, diags);
    }

    return diags->error_count > errors_before ? -1 : 0;
}

int
dsd_user_config_validate(const char* path, dsdcfg_diagnostics_t* diags) {
    if (!diags) {
        return -1;
    }

    dsdcfg_diags_init(diags);

    if (!path || !*path) {
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, "", "", "No config path provided");
        return -1;
    }

    char opened_path[2048];
    FILE* fp = dsd_path_fopen_user_read_file(path, opened_path, sizeof opened_path);
    if (!fp) {
        char msg[256];
        DSD_SNPRINTF(msg, sizeof msg, "Cannot open file: %s", strerror(errno));
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, "", "", msg);
        return -1;
    }

    const char* include_stack[1] = {opened_path};
    int rc = validate_config_stream_syntax_append(fp, opened_path, 0, include_stack, 1, diags);
    fclose(fp);
    if (rc == 0) {
        validate_composed_config_path(path, diags);
    }
    return diags->error_count > 0 ? -1 : rc;
}
