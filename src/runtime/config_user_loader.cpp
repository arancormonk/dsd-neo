// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * INI loading and profile overlay support for user configuration.
 */

#include <ctype.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_user_internal.h"

static void
trim_whitespace(char* s) {
    char* p = s;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static void
strip_inline_comment(char* s) {
    int in_quote = 0;
    for (char* p = s; *p; ++p) {
        if (*p == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && (*p == '#' || *p == ';')) {
            *p = '\0';
            break;
        }
    }
}

static void
unquote(char* s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int
parse_bool(const char* v, int* out) {
    if (!v || !*v || !out) {
        return -1;
    }
    if (dsd_strcasecmp(v, "1") == 0 || dsd_strcasecmp(v, "true") == 0 || dsd_strcasecmp(v, "yes") == 0
        || dsd_strcasecmp(v, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (dsd_strcasecmp(v, "0") == 0 || dsd_strcasecmp(v, "false") == 0 || dsd_strcasecmp(v, "no") == 0
        || dsd_strcasecmp(v, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static long
parse_int(const char* v, long defv) {
    if (!v || !*v) {
        return defv;
    }
    char* end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v) {
        return defv;
    }
    return x;
}

/**
 * @brief Copy a path value with shell-like expansion (~, $VAR, ${VAR}).
 *
 * Expands the source path and copies it to the destination buffer.
 * Falls back to verbatim copy if expansion fails.
 */
static void
copy_path_expanded(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src || !*src) {
        dst[0] = '\0';
        return;
    }

    char expanded[1024];
    if (dsd_config_expand_path(src, expanded, sizeof(expanded)) == 0) {
        snprintf(dst, dst_size, "%s", expanded);
    } else {
        /* Expansion failed (truncation or error), use verbatim */
        snprintf(dst, dst_size, "%s", src);
    }
    dst[dst_size - 1] = '\0';
}

typedef enum { USER_CFG_PARSE_MODE_BASE = 0, USER_CFG_PARSE_MODE_PROFILE = 1 } user_cfg_parse_mode_t;

static void
copy_text_value(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
    dst[dst_size - 1] = '\0';
}

static long
parse_int_for_mode(const char* v, long base_default, user_cfg_parse_mode_t mode) {
    return parse_int(v, mode == USER_CFG_PARSE_MODE_PROFILE ? 0 : base_default);
}

static int
parse_input_source_value(const char* val, dsdneoUserInputSource* out_source) {
    if (!val || !*val || !out_source) {
        return -1;
    }
    if (dsd_strcasecmp(val, "pulse") == 0) {
        *out_source = DSDCFG_INPUT_PULSE;
        return 0;
    }
    if (dsd_strcasecmp(val, "rtl") == 0) {
        *out_source = DSDCFG_INPUT_RTL;
        return 0;
    }
    if (dsd_strcasecmp(val, "rtltcp") == 0) {
        *out_source = DSDCFG_INPUT_RTLTCP;
        return 0;
    }
    if (dsd_strcasecmp(val, "soapy") == 0) {
        *out_source = DSDCFG_INPUT_SOAPY;
        return 0;
    }
    if (dsd_strcasecmp(val, "file") == 0) {
        *out_source = DSDCFG_INPUT_FILE;
        return 0;
    }
    if (dsd_strcasecmp(val, "tcp") == 0) {
        *out_source = DSDCFG_INPUT_TCP;
        return 0;
    }
    if (dsd_strcasecmp(val, "udp") == 0) {
        *out_source = DSDCFG_INPUT_UDP;
        return 0;
    }
    return -1;
}

static int
parse_output_backend_value(const char* val, dsdneoUserOutputBackend* out_backend) {
    if (!val || !*val || !out_backend) {
        return -1;
    }
    if (dsd_strcasecmp(val, "pulse") == 0) {
        *out_backend = DSDCFG_OUTPUT_PULSE;
        return 0;
    }
    if (dsd_strcasecmp(val, "null") == 0) {
        *out_backend = DSDCFG_OUTPUT_NULL;
        return 0;
    }
    return -1;
}

static int
parse_demod_path_value(const char* val, dsdneoUserDemodPath* out_path) {
    if (!val || !*val || !out_path) {
        return -1;
    }
    if (dsd_strcasecmp(val, "auto") == 0) {
        *out_path = DSDCFG_DEMOD_AUTO;
        return 0;
    }
    if (dsd_strcasecmp(val, "c4fm") == 0) {
        *out_path = DSDCFG_DEMOD_C4FM;
        return 0;
    }
    if (dsd_strcasecmp(val, "gfsk") == 0) {
        *out_path = DSDCFG_DEMOD_GFSK;
        return 0;
    }
    if (dsd_strcasecmp(val, "qpsk") == 0) {
        *out_path = DSDCFG_DEMOD_QPSK;
        return 0;
    }
    return -1;
}

static void
apply_input_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val, user_cfg_parse_mode_t mode) {
    if (strcmp(key_lc, "source") == 0) {
        dsdneoUserInputSource source = DSDCFG_INPUT_UNSET;
        if (parse_input_source_value(val, &source) == 0) {
            cfg->input_source = source;
        }
    } else if (strcmp(key_lc, "pulse_source") == 0 || strcmp(key_lc, "pulse_input") == 0) {
        copy_text_value(cfg->pulse_input, sizeof cfg->pulse_input, val);
    } else if (strcmp(key_lc, "rtl_device") == 0) {
        cfg->rtl_device = (int)parse_int_for_mode(val, 0, mode);
    } else if (strcmp(key_lc, "rtl_freq") == 0) {
        copy_text_value(cfg->rtl_freq, sizeof cfg->rtl_freq, val);
    } else if (strcmp(key_lc, "rtl_gain") == 0) {
        cfg->rtl_gain = (int)parse_int_for_mode(val, 22, mode);
    } else if (strcmp(key_lc, "rtl_ppm") == 0) {
        cfg->rtl_ppm = (int)parse_int_for_mode(val, 0, mode);
    } else if (strcmp(key_lc, "rtl_bw_khz") == 0) {
        cfg->rtl_bw_khz = (int)parse_int_for_mode(val, 12, mode);
    } else if (strcmp(key_lc, "rtl_sql") == 0) {
        cfg->rtl_sql = (int)parse_int_for_mode(val, 0, mode);
    } else if (strcmp(key_lc, "rtl_volume") == 0) {
        cfg->rtl_volume = (int)parse_int_for_mode(val, 1, mode);
    } else if (strcmp(key_lc, "auto_ppm") == 0 || strcmp(key_lc, "rtl_auto_ppm") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->rtl_auto_ppm = b;
        }
    } else if (strcmp(key_lc, "rtltcp_host") == 0) {
        copy_text_value(cfg->rtltcp_host, sizeof cfg->rtltcp_host, val);
    } else if (strcmp(key_lc, "rtltcp_port") == 0) {
        cfg->rtltcp_port = (int)parse_int_for_mode(val, 1234, mode);
    } else if (strcmp(key_lc, "soapy_args") == 0) {
        copy_text_value(cfg->soapy_args, sizeof cfg->soapy_args, val);
    } else if (strcmp(key_lc, "file_path") == 0) {
        copy_path_expanded(cfg->file_path, sizeof cfg->file_path, val);
    } else if (strcmp(key_lc, "file_sample_rate") == 0) {
        cfg->file_sample_rate = (int)parse_int_for_mode(val, 48000, mode);
    } else if (strcmp(key_lc, "tcp_host") == 0) {
        copy_text_value(cfg->tcp_host, sizeof cfg->tcp_host, val);
    } else if (strcmp(key_lc, "tcp_port") == 0) {
        cfg->tcp_port = (int)parse_int_for_mode(val, 7355, mode);
    } else if (strcmp(key_lc, "udp_addr") == 0) {
        copy_text_value(cfg->udp_addr, sizeof cfg->udp_addr, val);
    } else if (strcmp(key_lc, "udp_port") == 0) {
        cfg->udp_port = (int)parse_int_for_mode(val, 7355, mode);
    }
}

static void
apply_output_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "backend") == 0) {
        dsdneoUserOutputBackend backend = DSDCFG_OUTPUT_UNSET;
        if (parse_output_backend_value(val, &backend) == 0) {
            cfg->output_backend = backend;
        }
    } else if (strcmp(key_lc, "pulse_sink") == 0 || strcmp(key_lc, "pulse_output") == 0) {
        copy_text_value(cfg->pulse_output, sizeof cfg->pulse_output, val);
    } else if (strcmp(key_lc, "ncurses_ui") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->ncurses_ui = b;
        }
    }
}

static void
apply_mode_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "decode") == 0) {
        dsdneoUserDecodeMode mode = DSDCFG_MODE_UNSET;
        if (user_config_parse_decode_mode_value(val, &mode, NULL) == 0) {
            cfg->decode_mode = mode;
        }
    } else if (strcmp(key_lc, "demod") == 0) {
        dsdneoUserDemodPath path = DSDCFG_DEMOD_UNSET;
        cfg->has_demod = 1;
        if (parse_demod_path_value(val, &path) == 0) {
            cfg->demod_path = path;
        }
    }
}

static void
apply_trunking_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "enabled") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->trunk_enabled = b;
        }
    } else if (strcmp(key_lc, "chan_csv") == 0) {
        copy_path_expanded(cfg->trunk_chan_csv, sizeof cfg->trunk_chan_csv, val);
    } else if (strcmp(key_lc, "group_csv") == 0) {
        copy_path_expanded(cfg->trunk_group_csv, sizeof cfg->trunk_group_csv, val);
    } else if (strcmp(key_lc, "allow_list") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->trunk_use_allow_list = b;
        }
    } else if (strcmp(key_lc, "tune_group_calls") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->trunk_tune_group_calls = b;
        }
    } else if (strcmp(key_lc, "tune_private_calls") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->trunk_tune_private_calls = b;
        }
    } else if (strcmp(key_lc, "tune_data_calls") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->trunk_tune_data_calls = b;
        }
    } else if (strcmp(key_lc, "tune_enc_calls") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->trunk_tune_enc_calls = b;
        }
    }
}

static void
apply_logging_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "event_log") == 0 || strcmp(key_lc, "event_log_file") == 0) {
        copy_path_expanded(cfg->event_log, sizeof cfg->event_log, val);
    } else if (strcmp(key_lc, "frame_log") == 0) {
        copy_path_expanded(cfg->frame_log, sizeof cfg->frame_log, val);
    }
}

static void
apply_recording_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "per_call_wav") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->per_call_wav = b;
        }
    } else if (strcmp(key_lc, "per_call_wav_dir") == 0) {
        copy_path_expanded(cfg->per_call_wav_dir, sizeof cfg->per_call_wav_dir, val);
    } else if (strcmp(key_lc, "static_wav") == 0) {
        copy_path_expanded(cfg->static_wav_path, sizeof cfg->static_wav_path, val);
    } else if (strcmp(key_lc, "raw_wav") == 0) {
        copy_path_expanded(cfg->raw_wav_path, sizeof cfg->raw_wav_path, val);
    } else if (strcmp(key_lc, "rdio_mode") == 0) {
        int mode = DSD_RDIO_MODE_OFF;
        if (dsd_rdio_mode_from_string(val, &mode) == 0) {
            cfg->rdio_mode = mode;
        }
    } else if (strcmp(key_lc, "rdio_system_id") == 0) {
        long v = parse_int(val, 0);
        if (v < 0) {
            v = 0;
        }
        if (v > 65535) {
            v = 65535;
        }
        cfg->rdio_system_id = (int)v;
    } else if (strcmp(key_lc, "rdio_api_url") == 0) {
        copy_text_value(cfg->rdio_api_url, sizeof cfg->rdio_api_url, val);
    } else if (strcmp(key_lc, "rdio_api_key") == 0) {
        copy_text_value(cfg->rdio_api_key, sizeof cfg->rdio_api_key, val);
    } else if (strcmp(key_lc, "rdio_upload_timeout_ms") == 0) {
        long v = parse_int(val, cfg->rdio_upload_timeout_ms);
        if (v < 100) {
            v = 100;
        }
        if (v > 120000) {
            v = 120000;
        }
        cfg->rdio_upload_timeout_ms = (int)v;
    } else if (strcmp(key_lc, "rdio_upload_retries") == 0) {
        long v = parse_int(val, cfg->rdio_upload_retries);
        if (v < 0) {
            v = 0;
        }
        if (v > 10) {
            v = 10;
        }
        cfg->rdio_upload_retries = (int)v;
    }
}

static void
apply_dsp_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "iq_balance") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->iq_balance = b;
        }
    } else if (strcmp(key_lc, "iq_dc_block") == 0) {
        int b = 0;
        if (parse_bool(val, &b) == 0) {
            cfg->iq_dc_block = b;
        }
    }
}

static void
apply_section_key(dsdneoUserConfig* cfg, const char* section, const char* key_lc, const char* val,
                  user_cfg_parse_mode_t mode) {
    if (strcmp(section, "input") == 0) {
        cfg->has_input = 1;
        apply_input_section_key(cfg, key_lc, val, mode);
    } else if (strcmp(section, "output") == 0) {
        cfg->has_output = 1;
        apply_output_section_key(cfg, key_lc, val);
    } else if (strcmp(section, "mode") == 0) {
        cfg->has_mode = 1;
        apply_mode_section_key(cfg, key_lc, val);
    } else if (strcmp(section, "trunking") == 0) {
        cfg->has_trunking = 1;
        apply_trunking_section_key(cfg, key_lc, val);
    } else if (strcmp(section, "logging") == 0) {
        cfg->has_logging = 1;
        apply_logging_section_key(cfg, key_lc, val);
    } else if (strcmp(section, "recording") == 0) {
        cfg->has_recording = 1;
        apply_recording_section_key(cfg, key_lc, val);
    } else if (strcmp(section, "dsp") == 0) {
        cfg->has_dsp = 1;
        apply_dsp_section_key(cfg, key_lc, val);
    }
}

// INI loader ------------------------------------------------------------------

/* Forward declarations for include processing */
static int process_includes(const char* path, dsdneoUserConfig* cfg, int depth, const char** include_stack,
                            int include_stack_size);

/* Internal loader that does NOT reset the config struct.
 * Used for accumulating values from multiple files (includes). */
static int
user_config_load_no_reset(const char* path, dsdneoUserConfig* cfg) {
    if (!cfg || !path || !*path) {
        return -1;
    }

    FILE* fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char line[1024];
    char current_section[64];
    current_section[0] = '\0';

    while (fgets(line, sizeof line, fp)) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (!end) {
                continue;
            }
            *end = '\0';
            snprintf(current_section, sizeof current_section, "%s", line + 1);
            trim_whitespace(current_section);
            for (char* p = current_section; *p; ++p) {
                *p = (char)tolower((unsigned char)*p);
            }
            continue;
        }

        strip_inline_comment(line);
        if (line[0] == '\0') {
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char* key = line;
        char* val = eq + 1;
        trim_whitespace(key);
        trim_whitespace(val);
        unquote(val);

        char key_lc[64];
        snprintf(key_lc, sizeof key_lc, "%.*s", (int)(sizeof key_lc - 1), key);
        key_lc[sizeof key_lc - 1] = '\0';
        for (char* p = key_lc; *p; ++p) {
            *p = (char)tolower((unsigned char)*p);
        }

        if (current_section[0] == '\0') {
            // Top-level keys
            if (strcmp(key_lc, "version") == 0) {
                cfg->version = (int)parse_int(val, 1);
            }
            continue;
        }

        apply_section_key(cfg, current_section, key_lc, val, USER_CFG_PARSE_MODE_BASE);
    }

    fclose(fp);
    return 0;
}

int
dsd_user_config_load(const char* path, dsdneoUserConfig* cfg) {
    if (!cfg) {
        return -1;
    }

    user_cfg_reset(cfg);

    /* Process includes first (they provide base values that can be overridden) */
    const char* stack[1] = {path};
    process_includes(path, cfg, 0, stack, 1);

    /* Now load the main config (which overrides included values) */
    return user_config_load_no_reset(path, cfg);
}

// Profile support -------------------------------------------------------------

/* Helper to apply a single dotted key (e.g., "input.source") to config */
static void
apply_profile_key(dsdneoUserConfig* cfg, const char* dotted_key, const char* val) {
    if (!cfg || !dotted_key || !val) {
        return;
    }

    /* Profile loader reads lines into a 1024-byte buffer; keep this scratch buffer at least that large to avoid
     * truncating dotted keys during parsing (and to silence -Wformat-truncation under _FORTIFY_SOURCE builds). */
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", dotted_key);
    buf[sizeof buf - 1] = '\0';

    char* dot = strchr(buf, '.');
    if (!dot) {
        return;
    }
    *dot = '\0';
    char* section = buf;
    char* key = dot + 1;

    /* Lowercase for comparison */
    for (char* c = section; *c; ++c) {
        *c = (char)tolower((unsigned char)*c);
    }
    for (char* c = key; *c; ++c) {
        *c = (char)tolower((unsigned char)*c);
    }

    apply_section_key(cfg, section, key, val, USER_CFG_PARSE_MODE_PROFILE);
}

/* Internal: load config with include and profile support */
static int load_config_internal(const char* path, const char* profile_name, dsdneoUserConfig* cfg, int depth,
                                const char** include_stack, int include_stack_size);

/* Process include directives */
static int
process_includes(const char* path, dsdneoUserConfig* cfg, int depth, const char** include_stack,
                 int include_stack_size) {
    if (depth >= 3) {
        return 0; /* max depth reached */
    }

    FILE* fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        /* Trim */
        char* p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        size_t n = strlen(p);
        while (n > 0 && isspace((unsigned char)p[n - 1])) {
            p[--n] = '\0';
        }

        /* Stop at first section - includes must be before sections */
        if (p[0] == '[') {
            break;
        }

        /* Skip comments and empty */
        if (p[0] == '\0' || p[0] == '#' || p[0] == ';') {
            continue;
        }

        /* Look for include = "path" */
        if (dsd_strncasecmp(p, "include", 7) != 0) {
            continue;
        }
        p += 7;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != '=') {
            continue;
        }
        p++;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        /* Extract path */
        char inc_path[1024];
        n = strlen(p);
        if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
            snprintf(inc_path, sizeof inc_path, "%.*s", (int)(n - 2), p + 1);
        } else {
            snprintf(inc_path, sizeof inc_path, "%s", p);
        }
        inc_path[sizeof inc_path - 1] = '\0';

        /* Expand path */
        char expanded[1024];
        if (dsd_config_expand_path(inc_path, expanded, sizeof expanded) == 0) {
            snprintf(inc_path, sizeof inc_path, "%s", expanded);
        }

        /* Check for circular include */
        int circular = 0;
        for (int i = 0; i < include_stack_size; i++) {
            if (include_stack[i] && strcmp(include_stack[i], inc_path) == 0) {
                circular = 1;
                break;
            }
        }
        if (circular) {
            continue; /* skip circular include */
        }

        /* First process any nested includes in the included file */
        const char* nested_stack[8];
        int nested_stack_size = 0;
        for (int i = 0; i < include_stack_size && i < 7; i++) {
            nested_stack[nested_stack_size++] = include_stack[i];
        }
        nested_stack[nested_stack_size++] = inc_path;
        process_includes(inc_path, cfg, depth + 1, nested_stack, nested_stack_size);

        /* Then load the included file's config values */
        user_config_load_no_reset(inc_path, cfg);
    }

    fclose(fp);
    return 0;
}

static int
load_config_internal(const char* path, const char* profile_name, dsdneoUserConfig* cfg, int depth,
                     const char** include_stack, int include_stack_size) {
    (void)depth;
    (void)include_stack;
    (void)include_stack_size;

    if (!path || !cfg) {
        return -1;
    }

    /* Includes are already processed by dsd_user_config_load_profile() before
     * calling this function. This function only extracts profile overlay keys. */

    FILE* fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char line[1024];
    char current_section[64];
    current_section[0] = '\0';
    char target_profile[64];
    target_profile[0] = '\0';
    if (profile_name) {
        snprintf(target_profile, sizeof target_profile, "profile.%s", profile_name);
        for (char* c = target_profile; *c; ++c) {
            *c = (char)tolower((unsigned char)*c);
        }
    }

    int in_target_profile = 0;
    int profile_found = 0;

    while (fgets(line, sizeof line, fp)) {
        /* Trim */
        char* p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        size_t n = strlen(p);
        while (n > 0 && isspace((unsigned char)p[n - 1])) {
            p[--n] = '\0';
        }

        if (p[0] == '\0' || p[0] == '#' || p[0] == ';') {
            continue;
        }

        /* Section header */
        if (p[0] == '[') {
            char* end = strchr(p, ']');
            if (!end) {
                continue;
            }
            *end = '\0';
            snprintf(current_section, sizeof current_section, "%s", p + 1);
            for (char* c = current_section; *c; ++c) {
                *c = (char)tolower((unsigned char)*c);
            }

            /* Check if we entered the target profile section */
            in_target_profile = (target_profile[0] && strcmp(current_section, target_profile) == 0);
            if (in_target_profile) {
                profile_found = 1;
            }
            continue;
        }

        /* Key=value */
        char* eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char* key = p;
        char* val = eq + 1;

        /* Trim */
        while (*key && isspace((unsigned char)*key)) {
            key++;
        }
        n = strlen(key);
        while (n > 0 && isspace((unsigned char)key[n - 1])) {
            key[--n] = '\0';
        }
        while (*val && isspace((unsigned char)*val)) {
            val++;
        }
        n = strlen(val);
        while (n > 0 && isspace((unsigned char)val[n - 1])) {
            val[--n] = '\0';
        }

        /* Unquote */
        size_t val_len = strlen(val);
        if (val_len >= 2 && val[0] == '"' && val[val_len - 1] == '"') {
            memmove(val, val + 1, val_len - 2);
            val[val_len - 2] = '\0';
        }

        /* Handle profile section keys */
        if (in_target_profile) {
            apply_profile_key(cfg, key, val);
            continue;
        }

        /* Skip other profile sections */
        if (strncmp(current_section, "profile.", 8) == 0) {
            continue;
        }

        /* Skip include directives (already processed) */
        if (current_section[0] == '\0' && dsd_strcasecmp(key, "include") == 0) {
            continue;
        }

        /* Regular section keys are handled by dsd_user_config_load() which is called
         * before this function for profile loading. This function only handles
         * profile overlay keys (in_target_profile case above). */
    }

    fclose(fp);

    /* If a profile was requested but not found, return error */
    if (profile_name && *profile_name && !profile_found) {
        return -1;
    }

    return 0;
}

int
dsd_user_config_load_profile(const char* path, const char* profile_name, dsdneoUserConfig* cfg) {
    if (!cfg) {
        return -1;
    }

    /* Reset config once at start */
    user_cfg_reset(cfg);

    /* Process includes first (they provide base values that can be overridden) */
    const char* stack[1] = {path};
    process_includes(path, cfg, 0, stack, 1);

    /* Now load the main config (which overrides included values) */
    int rc = user_config_load_no_reset(path, cfg);
    if (rc != 0) {
        return rc;
    }

    /* If no profile requested, we're done */
    if (!profile_name || !*profile_name) {
        return 0;
    }

    /* Now overlay profile settings */
    return load_config_internal(path, profile_name, cfg, 0, stack, 1);
}

int
dsd_user_config_list_profiles(const char* path, const char** names, char* names_buf, size_t names_buf_size,
                              int max_names) {
    if (!path || !names || !names_buf || names_buf_size == 0 || max_names <= 0) {
        return -1;
    }

    FILE* fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    int count = 0;
    size_t buf_used = 0;
    char line[1024];

    while (fgets(line, sizeof line, fp) && count < max_names) {
        /* Trim */
        char* p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        size_t n = strlen(p);
        while (n > 0 && isspace((unsigned char)p[n - 1])) {
            p[--n] = '\0';
        }

        /* Look for [profile.NAME] */
        if (p[0] != '[') {
            continue;
        }
        char* end = strchr(p, ']');
        if (!end) {
            continue;
        }
        *end = '\0';
        const char* section = p + 1;

        if (dsd_strncasecmp(section, "profile.", 8) != 0) {
            continue;
        }

        const char* profile_name = section + 8;
        size_t name_len = strlen(profile_name);
        if (name_len == 0) {
            continue;
        }

        /* Copy to buffer */
        if (buf_used + name_len + 1 > names_buf_size) {
            break; /* buffer full */
        }

        names[count] = names_buf + buf_used;
        memcpy(names_buf + buf_used, profile_name, name_len + 1);
        buf_used += name_len + 1;
        count++;
    }

    fclose(fp);
    return count;
}
