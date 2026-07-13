// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * INI loading and profile overlay support for user configuration.
 */

#if defined(_WIN32)
#include <algorithm>
#endif
#include <ctype.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/path_policy.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "config_user_internal.h"
#include "dsd-neo/core/frontend_types.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/runtime/call_alert.h"

static int
seek_config_stream_to_start(FILE* stream) {
    if (!stream) {
        return -1;
    }
    return fseek(stream, 0L, SEEK_SET);
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
        DSD_SNPRINTF(dst, dst_size, "%s", expanded);
    } else {
        /* Expansion failed (truncation or error), use verbatim */
        DSD_SNPRINTF(dst, dst_size, "%s", src);
    }
    dst[dst_size - 1] = '\0';
}

namespace {

enum user_cfg_parse_mode_t : unsigned char { USER_CFG_PARSE_MODE_BASE = 0, USER_CFG_PARSE_MODE_PROFILE = 1 };

constexpr int USER_CFG_INVALID_PERSISTED_VERSION = -2;

} // namespace

static void
copy_text_value(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    DSD_SNPRINTF(dst, dst_size, "%s", src ? src : "");
    dst[dst_size - 1] = '\0';
}

static int
apply_integer_setting(const char* value, int base_default, user_cfg_parse_mode_t mode, int* setting) {
    int parsed = 0;
    if (user_config_parse_int_value(value, &parsed) == 0) {
        *setting = parsed;
        return 1;
    }
    if (mode == USER_CFG_PARSE_MODE_BASE) {
        *setting = base_default;
        return 1;
    }
    return 0;
}

static int
parse_section_header_line(char* line, char* out_section, size_t out_section_size) {
    if (!line || !out_section || out_section_size == 0) {
        return 0;
    }
    if (line[0] != '[') {
        return 0;
    }
    char* end = strchr(line, ']');
    if (!end) {
        return -1;
    }
    *end = '\0';
    size_t section_len = strnlen(line + 1, out_section_size - 1U);
    DSD_MEMCPY(out_section, line + 1, section_len);
    out_section[section_len] = '\0';
    user_config_trim_ascii_whitespace(out_section);
    user_config_lowercase_ascii(out_section);
    return 1;
}

static int
parse_key_value_fields(char* line, char** out_key, char** out_val) {
    if (!line || !out_key || !out_val) {
        return 0;
    }
    char* eq = strchr(line, '=');
    if (!eq) {
        return 0;
    }
    *eq = '\0';
    char* key = line;
    char* val = eq + 1;
    user_config_trim_ascii_whitespace(key);
    user_config_trim_ascii_whitespace(val);
    user_config_strip_wrapping_quotes(val);
    *out_key = key;
    *out_val = val;
    return 1;
}

static void
normalize_key_to_lower(const char* key, char* key_lc, size_t key_lc_size) {
    if (!key_lc || key_lc_size == 0) {
        return;
    }
    DSD_SNPRINTF(key_lc, key_lc_size, "%.*s", (int)(key_lc_size - 1), key ? key : "");
    key_lc[key_lc_size - 1] = '\0';
    user_config_lowercase_ascii(key_lc);
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
parse_frontend_kind_value(const char* val, dsd_frontend_kind* out_frontend) {
    if (!val || !*val || !out_frontend) {
        return -1;
    }
    if (dsd_strcasecmp(val, "none") == 0) {
        *out_frontend = DSD_FRONTEND_NONE;
        return 0;
    }
    if (dsd_strcasecmp(val, "terminal") == 0) {
        *out_frontend = DSD_FRONTEND_TERMINAL;
        return 0;
    }
    return -1;
}

static void
set_config_frontend_kind(dsdneoUserConfig* cfg, dsd_frontend_kind frontend) {
    cfg->frontend_kind = frontend;
    cfg->frontend_kind_is_set = 1;
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
apply_input_source_keys(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "source") == 0) {
        dsdneoUserInputSource source = DSDCFG_INPUT_UNSET;
        if (parse_input_source_value(val, &source) == 0) {
            cfg->input_source = source;
        }
    } else if (strcmp(key_lc, "pulse_source") == 0) {
        copy_text_value(cfg->pulse_input, sizeof cfg->pulse_input, val);
    }
}

static int
apply_input_rtl_keys(dsdneoUserConfig* cfg, const char* key_lc, const char* val, user_cfg_parse_mode_t mode) {
    if (strcmp(key_lc, "rtl_device") == 0) {
        (void)apply_integer_setting(val, 0, mode, &cfg->rtl_device);
    } else if (strcmp(key_lc, "rtl_freq") == 0) {
        copy_text_value(cfg->rtl_freq, sizeof cfg->rtl_freq, val);
    } else if (strcmp(key_lc, "rtl_gain") == 0) {
        (void)apply_integer_setting(val, 22, mode, &cfg->rtl_gain);
    } else if (strcmp(key_lc, "rtl_ppm") == 0) {
        if (apply_integer_setting(val, 0, mode, &cfg->rtl_ppm)) {
            cfg->rtl_ppm_is_set = 1;
        }
    } else if (strcmp(key_lc, "rtl_bw_khz") == 0) {
        (void)apply_integer_setting(val, 12, mode, &cfg->rtl_bw_khz);
    } else if (strcmp(key_lc, "rtl_sql") == 0) {
        (void)apply_integer_setting(val, 0, mode, &cfg->rtl_sql);
    } else if (strcmp(key_lc, "rtl_volume") == 0) {
        (void)apply_integer_setting(val, 1, mode, &cfg->rtl_volume);
    } else if (strcmp(key_lc, "auto_ppm") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->rtl_auto_ppm = b;
        }
    } else {
        return 0;
    }
    return 1;
}

static int
apply_input_rtltcp_soapy_keys(dsdneoUserConfig* cfg, const char* key_lc, const char* val, user_cfg_parse_mode_t mode) {
    if (strcmp(key_lc, "rtltcp_host") == 0) {
        copy_text_value(cfg->rtltcp_host, sizeof cfg->rtltcp_host, val);
    } else if (strcmp(key_lc, "rtltcp_port") == 0) {
        (void)apply_integer_setting(val, 1234, mode, &cfg->rtltcp_port);
    } else if (strcmp(key_lc, "soapy_args") == 0) {
        copy_text_value(cfg->soapy_args, sizeof cfg->soapy_args, val);
    } else if (strcmp(key_lc, "soapy_profile") == 0) {
        copy_text_value(cfg->soapy_profile, sizeof cfg->soapy_profile, val);
    } else if (strcmp(key_lc, "soapy_stream_format") == 0) {
        copy_text_value(cfg->soapy_stream_format, sizeof cfg->soapy_stream_format, val);
    } else if (strcmp(key_lc, "soapy_antenna") == 0) {
        copy_text_value(cfg->soapy_antenna, sizeof cfg->soapy_antenna, val);
    } else if (strcmp(key_lc, "soapy_clock") == 0) {
        copy_text_value(cfg->soapy_clock, sizeof cfg->soapy_clock, val);
    } else if (strcmp(key_lc, "soapy_settings") == 0) {
        copy_text_value(cfg->soapy_settings, sizeof cfg->soapy_settings, val);
    } else if (strcmp(key_lc, "soapy_gains") == 0) {
        copy_text_value(cfg->soapy_gains, sizeof cfg->soapy_gains, val);
    } else if (strcmp(key_lc, "soapy_bandwidth_hz") == 0) {
        if (apply_integer_setting(val, -1, mode, &cfg->soapy_bandwidth_hz)) {
            cfg->soapy_bandwidth_hz_is_set = 1;
        }
    } else {
        return 0;
    }
    return 1;
}

static int
apply_input_file_network_keys(dsdneoUserConfig* cfg, const char* key_lc, const char* val, user_cfg_parse_mode_t mode) {
    if (strcmp(key_lc, "file_path") == 0) {
        copy_path_expanded(cfg->file_path, sizeof cfg->file_path, val);
    } else if (strcmp(key_lc, "file_sample_rate") == 0) {
        (void)apply_integer_setting(val, 48000, mode, &cfg->file_sample_rate);
    } else if (strcmp(key_lc, "tcp_host") == 0) {
        copy_text_value(cfg->tcp_host, sizeof cfg->tcp_host, val);
    } else if (strcmp(key_lc, "tcp_port") == 0) {
        (void)apply_integer_setting(val, 7355, mode, &cfg->tcp_port);
    } else if (strcmp(key_lc, "udp_addr") == 0) {
        copy_text_value(cfg->udp_addr, sizeof cfg->udp_addr, val);
    } else if (strcmp(key_lc, "udp_port") == 0) {
        (void)apply_integer_setting(val, 7355, mode, &cfg->udp_port);
    } else {
        return 0;
    }
    return 1;
}

static void
apply_input_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val, user_cfg_parse_mode_t mode) {
    apply_input_source_keys(cfg, key_lc, val);
    if (apply_input_rtl_keys(cfg, key_lc, val, mode)) {
        return;
    }
    if (apply_input_rtltcp_soapy_keys(cfg, key_lc, val, mode)) {
        return;
    }
    (void)apply_input_file_network_keys(cfg, key_lc, val, mode);
}

static void
apply_output_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "backend") == 0) {
        dsdneoUserOutputBackend backend = DSDCFG_OUTPUT_UNSET;
        if (parse_output_backend_value(val, &backend) == 0) {
            cfg->output_backend = backend;
        }
    } else if (strcmp(key_lc, "pulse_sink") == 0) {
        copy_text_value(cfg->pulse_output, sizeof cfg->pulse_output, val);
    } else if (strcmp(key_lc, "frontend") == 0) {
        dsd_frontend_kind frontend = DSD_FRONTEND_NONE;
        if (parse_frontend_kind_value(val, &frontend) == 0) {
            set_config_frontend_kind(cfg, frontend);
        }
    }
}

static void
apply_mode_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "decode") == 0) {
        dsdneoUserDecodeMode mode = DSDCFG_MODE_UNSET;
        if (user_config_parse_decode_mode_value(val, &mode) == 0) {
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
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->trunk_enabled = b;
        }
    } else if (strcmp(key_lc, "chan_csv") == 0) {
        copy_path_expanded(cfg->trunk_chan_csv, sizeof cfg->trunk_chan_csv, val);
    } else if (strcmp(key_lc, "group_csv") == 0) {
        copy_path_expanded(cfg->trunk_group_csv, sizeof cfg->trunk_group_csv, val);
    } else if (strcmp(key_lc, "allow_list") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->trunk_use_allow_list = b;
        }
    } else if (strcmp(key_lc, "tune_group_calls") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->trunk_tune_group_calls = b;
        }
    } else if (strcmp(key_lc, "tune_private_calls") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->trunk_tune_private_calls = b;
        }
    } else if (strcmp(key_lc, "tune_data_calls") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->trunk_tune_data_calls = b;
        }
    } else if (strcmp(key_lc, "tune_enc_calls") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->trunk_tune_enc_calls = b;
        }
    }
}

static void
apply_trunk_scan_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "enabled") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->trunk_scan_enabled = b;
        }
    } else if (strcmp(key_lc, "targets_csv") == 0) {
        copy_path_expanded(cfg->trunk_scan_targets_csv, sizeof cfg->trunk_scan_targets_csv, val);
    } else if (strcmp(key_lc, "idle_dwell_ms") == 0) {
        int parsed = 0;
        if (user_config_parse_int_value(val, &parsed) == 0) {
            cfg->trunk_scan_idle_dwell_ms = parsed;
        }
    } else if (strcmp(key_lc, "activity_hold_ms") == 0) {
        int parsed = 0;
        if (user_config_parse_int_value(val, &parsed) == 0) {
            cfg->trunk_scan_activity_hold_ms = parsed;
        }
    }
}

static void
apply_logging_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "event_log") == 0) {
        copy_path_expanded(cfg->event_log, sizeof cfg->event_log, val);
    } else if (strcmp(key_lc, "frame_log") == 0) {
        copy_path_expanded(cfg->frame_log, sizeof cfg->frame_log, val);
    } else if (strcmp(key_lc, "p25_sm_log") == 0) {
        copy_path_expanded(cfg->p25_sm_log, sizeof cfg->p25_sm_log, val);
    }
}

static void
set_alert_event(dsdneoUserConfig* cfg, int event, int enabled) {
    if (enabled) {
        cfg->call_alert_events |= event;
    } else {
        cfg->call_alert_events &= ~event;
    }
}

static void
apply_alerts_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "enabled") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->call_alert_enabled = b;
        }
    } else if (strcmp(key_lc, "voice_start") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            set_alert_event(cfg, DSD_CALL_ALERT_EVENT_VOICE_START, b);
        }
    } else if (strcmp(key_lc, "voice_end") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            set_alert_event(cfg, DSD_CALL_ALERT_EVENT_VOICE_END, b);
        }
    } else if (strcmp(key_lc, "data") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            set_alert_event(cfg, DSD_CALL_ALERT_EVENT_DATA, b);
        }
    }
}

static void
apply_recording_rdio_range_value(const char* val, int default_value, int min_value, int max_value, int* out_value) {
    if (!out_value) {
        return;
    }
    int v = 0;
    if (user_config_parse_int_value(val, &v) != 0) {
        v = default_value;
    }
    if (v < min_value) {
        v = min_value;
    }
    if (v > max_value) {
        v = max_value;
    }
    *out_value = v;
}

static int
apply_recording_basic_keys(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "per_call_wav") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->per_call_wav = b;
        }
    } else if (strcmp(key_lc, "per_call_wav_dir") == 0) {
        copy_path_expanded(cfg->per_call_wav_dir, sizeof cfg->per_call_wav_dir, val);
    } else if (strcmp(key_lc, "static_wav") == 0) {
        copy_path_expanded(cfg->static_wav_path, sizeof cfg->static_wav_path, val);
    } else if (strcmp(key_lc, "raw_wav") == 0) {
        copy_path_expanded(cfg->raw_wav_path, sizeof cfg->raw_wav_path, val);
    } else {
        return 0;
    }
    return 1;
}

static int
apply_recording_rdio_keys(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "rdio_mode") == 0) {
        int mode = DSD_RDIO_MODE_OFF;
        if (dsd_rdio_mode_from_string(val, &mode) == 0) {
            cfg->rdio_mode = mode;
        }
    } else if (strcmp(key_lc, "rdio_system_id") == 0) {
        apply_recording_rdio_range_value(val, 0, 0, 65535, &cfg->rdio_system_id);
    } else if (strcmp(key_lc, "rdio_api_url") == 0) {
        copy_text_value(cfg->rdio_api_url, sizeof cfg->rdio_api_url, val);
    } else if (strcmp(key_lc, "rdio_api_key") == 0) {
        copy_text_value(cfg->rdio_api_key, sizeof cfg->rdio_api_key, val);
    } else if (strcmp(key_lc, "rdio_upload_timeout_ms") == 0) {
        apply_recording_rdio_range_value(val, cfg->rdio_upload_timeout_ms, 100, 120000, &cfg->rdio_upload_timeout_ms);
    } else if (strcmp(key_lc, "rdio_upload_retries") == 0) {
        apply_recording_rdio_range_value(val, cfg->rdio_upload_retries, 0, 10, &cfg->rdio_upload_retries);
    } else if (strcmp(key_lc, "rdio_api_delete_after_upload") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->rdio_api_delete_after_upload = b;
        }
    } else {
        return 0;
    }
    return 1;
}

static void
apply_recording_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (apply_recording_basic_keys(cfg, key_lc, val)) {
        return;
    }
    (void)apply_recording_rdio_keys(cfg, key_lc, val);
}

static void
apply_dsp_section_key(dsdneoUserConfig* cfg, const char* key_lc, const char* val) {
    if (strcmp(key_lc, "iq_balance") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
            cfg->iq_balance = b;
        }
    } else if (strcmp(key_lc, "iq_dc_block") == 0) {
        int b = 0;
        if (user_config_parse_bool_value(val, &b) == 0) {
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
    } else if (strcmp(section, "trunk_scan") == 0) {
        cfg->has_trunk_scan = 1;
        apply_trunk_scan_section_key(cfg, key_lc, val);
    } else if (strcmp(section, "logging") == 0) {
        cfg->has_logging = 1;
        apply_logging_section_key(cfg, key_lc, val);
    } else if (strcmp(section, "alerts") == 0) {
        cfg->has_alerts = 1;
        apply_alerts_section_key(cfg, key_lc, val);
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
static int process_includes_stream(FILE* fp, dsdneoUserConfig* cfg, int depth, const char** include_stack,
                                   int include_stack_size);

/* Internal loader that does NOT reset the config struct.
 * Used for accumulating values from multiple files (includes). */
static int
user_config_load_no_reset_stream(FILE* fp, dsdneoUserConfig* cfg) {
    if (!cfg || !fp) {
        return -1;
    }

    char line[1024];
    char current_section[64];
    current_section[0] = '\0';

    while (fgets(line, sizeof line, fp)) {
        user_config_trim_ascii_whitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }

        int section_result = parse_section_header_line(line, current_section, sizeof current_section);
        if (section_result > 0) {
            continue;
        }

        user_config_strip_inline_comment(line);
        user_config_trim_ascii_whitespace(line);
        if (line[0] == '\0') {
            continue;
        }

        char* key = NULL;
        char* val = NULL;
        if (!parse_key_value_fields(line, &key, &val)) {
            continue;
        }

        char key_lc[64];
        normalize_key_to_lower(key, key_lc, sizeof key_lc);

        if (current_section[0] == '\0') {
            // Top-level keys
            if (strcmp(key_lc, "version") == 0) {
                int version = 0;
                if (user_config_parse_int_value(val, &version) != 0 || version != 1) {
                    return USER_CFG_INVALID_PERSISTED_VERSION;
                }
            }
            continue;
        }

        apply_section_key(cfg, current_section, key_lc, val, USER_CFG_PARSE_MODE_BASE);
    }

    return 0;
}

static int
user_config_load_no_reset(const char* path, dsdneoUserConfig* cfg) {
    if (!cfg || !path || !*path) {
        return -1;
    }

    char opened_path[2048];
    FILE* fp = dsd_path_fopen_user_read_file(path, opened_path, sizeof opened_path);
    if (!fp) {
        return -1;
    }

    int rc = user_config_load_no_reset_stream(fp, cfg);
    fclose(fp);
    return rc;
}

int
dsd_user_config_load(const char* path, dsdneoUserConfig* cfg) {
    if (!cfg) {
        return -1;
    }

    user_cfg_reset(cfg);
    char root_path[2048];
    if (dsd_path_expand_user(path, root_path, sizeof root_path) != 0) {
        return -1;
    }

    /* Process includes first (they provide base values that can be overridden) */
    const char* stack[1] = {root_path};
    int include_rc = process_includes(root_path, cfg, 0, stack, 1);
    if (include_rc != 0) {
        return include_rc;
    }

    /* Now load the main config (which overrides included values) */
    return user_config_load_no_reset(root_path, cfg);
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
    DSD_SNPRINTF(buf, sizeof buf, "%s", dotted_key);
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
skip_ascii_spaces(char* p, char** out_next) {
    if (!out_next) {
        return -1;
    }
    char* cur = p;
    while (cur && *cur && isspace((unsigned char)*cur)) {
        cur++;
    }
    *out_next = cur;
    return 0;
}

static int
copy_include_path_value(char* p, char* out_inc_path, size_t out_inc_path_size) {
    if (!p || !out_inc_path || out_inc_path_size == 0) {
        return -1;
    }
    size_t n = strlen(p);
    if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
        DSD_SNPRINTF(out_inc_path, out_inc_path_size, "%.*s", (int)(n - 2), p + 1);
    } else {
        DSD_SNPRINTF(out_inc_path, out_inc_path_size, "%s", p);
    }
    out_inc_path[out_inc_path_size - 1] = '\0';
    return 0;
}

int
user_config_parse_include_directive_line(char* line, char* out_include_path, size_t out_include_path_size) {
    if (!line || !out_include_path || out_include_path_size == 0) {
        return 0;
    }
    out_include_path[0] = '\0';
    user_config_trim_ascii_whitespace(line);
    if (line[0] == '[') {
        return -1;
    }
    if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
        return 0;
    }
    user_config_strip_inline_comment(line);
    user_config_trim_ascii_whitespace(line);
    if (line[0] == '\0') {
        return 0;
    }
    if (dsd_strncasecmp(line, "include", 7) != 0) {
        return 0;
    }
    char* p = NULL;
    (void)skip_ascii_spaces(line + 7, &p);
    if (*p != '=') {
        return 0;
    }
    (void)skip_ascii_spaces(p + 1, &p);
    if (copy_include_path_value(p, out_include_path, out_include_path_size) != 0) {
        return 0;
    }
    return 1;
}

static bool
is_include_path_separator(char c) {
    return c == '/' || c == '\\';
}

static size_t
normalize_include_path_prefix(const std::string& input, std::string& prefix) {
    size_t pos = 0;
#if DSD_PLATFORM_WIN_NATIVE
    if (input.size() >= 2 && isalpha((unsigned char)input[0]) && input[1] == ':') {
        prefix.push_back((char)tolower((unsigned char)input[0]));
        prefix.push_back(':');
        pos = 2;
    }
#else
    (void)input;
    (void)prefix;
#endif
    return pos;
}

static size_t
skip_include_path_separators(const std::string& input, size_t pos) {
    while (pos < input.size() && is_include_path_separator(input[pos])) {
        pos++;
    }
    return pos;
}

static void
append_normalized_include_path_component(const std::string& component, bool absolute,
                                         std::vector<std::string>& components) {
#if DSD_PLATFORM_WIN_NATIVE
    std::string folded_component = component;
    std::transform(folded_component.begin(), folded_component.end(), folded_component.begin(),
                   [](char c) { return (char)tolower((unsigned char)c); });
    const std::string& normalized_component = folded_component;
#else
    const std::string& normalized_component = component;
#endif
    if (normalized_component.empty() || normalized_component == ".") {
        return;
    }
    if (normalized_component != "..") {
        components.push_back(normalized_component);
        return;
    }
    if (!components.empty() && components.back() != "..") {
        components.pop_back();
    } else if (!absolute) {
        components.push_back(normalized_component);
    }
}

static std::vector<std::string>
normalize_include_path_components(const std::string& input, size_t pos, bool absolute) {
    std::vector<std::string> components;
    while (pos < input.size()) {
        size_t end = pos;
        while (end < input.size() && !is_include_path_separator(input[end])) {
            end++;
        }
        append_normalized_include_path_component(input.substr(pos, end - pos), absolute, components);
        pos = skip_include_path_separators(input, end);
    }
    return components;
}

static std::string
join_normalized_include_path(const std::string& prefix, bool absolute, const std::vector<std::string>& components) {
    std::string normalized = prefix;
    if (absolute) {
        normalized.push_back('/');
    }
    for (const std::string& component : components) {
        bool append_separator = !normalized.empty() && normalized.back() != '/';
#if DSD_PLATFORM_WIN_NATIVE
        if (!absolute && !prefix.empty() && normalized == prefix) {
            append_separator = false;
        }
#endif
        if (append_separator) {
            normalized.push_back('/');
        }
        normalized += component;
    }
    if (normalized.empty()) {
        return absolute ? "/" : ".";
    }
    return normalized;
}

static std::string
normalize_include_path_identity(const char* path) {
    const std::string input = path ? path : "";
    std::string prefix;
    size_t pos = normalize_include_path_prefix(input, prefix);
    bool absolute = pos < input.size() && is_include_path_separator(input[pos]);
    pos = skip_include_path_separators(input, pos);
    const std::vector<std::string> components = normalize_include_path_components(input, pos, absolute);
    return join_normalized_include_path(prefix, absolute, components);
}

int
user_config_include_stack_contains_path(const char** include_stack, int include_stack_size, const char* path) {
    if (!include_stack || !path || path[0] == '\0') {
        return 0;
    }
    std::string normalized_path = normalize_include_path_identity(path);
    for (int i = 0; i < include_stack_size; i++) {
        if (include_stack[i] && normalize_include_path_identity(include_stack[i]) == normalized_path) {
            return 1;
        }
    }
    return 0;
}

static int
process_nested_includes(const char* inc_path, dsdneoUserConfig* cfg, int depth, const char** include_stack,
                        int include_stack_size) {
    const char* nested_stack[8];
    int nested_stack_size = 0;
    for (int i = 0; i < include_stack_size && i < 7; i++) {
        nested_stack[nested_stack_size++] = include_stack[i];
    }
    nested_stack[nested_stack_size++] = inc_path;
    return process_includes(inc_path, cfg, depth + 1, nested_stack, nested_stack_size);
}

static int
process_includes_stream(FILE* fp, dsdneoUserConfig* cfg, int depth, const char** include_stack,
                        int include_stack_size) {
    if (depth > DSD_USER_CONFIG_MAX_INCLUDE_DEPTH) {
        return -1;
    }
    if (!fp) {
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        char inc_path[1024];
        int parse_rc = user_config_parse_include_directive_line(line, inc_path, sizeof inc_path);
        if (parse_rc < 0) {
            break;
        }
        if (parse_rc == 0) {
            continue;
        }
        char resolved_inc_path[1024];
        const char* including_path = include_stack_size > 0 ? include_stack[include_stack_size - 1] : NULL;
        if (dsd_path_resolve_relative_to_file(including_path, inc_path, resolved_inc_path, sizeof resolved_inc_path)
            != 0) {
            return -1;
        }
        DSD_SNPRINTF(inc_path, sizeof inc_path, "%s", resolved_inc_path);
        inc_path[sizeof inc_path - 1] = '\0';
        if (user_config_include_stack_contains_path(include_stack, include_stack_size, inc_path)) {
            continue; /* skip circular include */
        }
        if (depth >= DSD_USER_CONFIG_MAX_INCLUDE_DEPTH) {
            return -1;
        }

        /* First process any nested includes in the included file */
        int include_rc = process_nested_includes(inc_path, cfg, depth, include_stack, include_stack_size);
        if (include_rc != 0) {
            return include_rc;
        }
        /* Then load the included file's config values */
        int load_rc = user_config_load_no_reset(inc_path, cfg);
        if (load_rc != 0) {
            return load_rc;
        }
    }

    return 0;
}

static int
process_includes(const char* path, dsdneoUserConfig* cfg, int depth, const char** include_stack,
                 int include_stack_size) {
    char opened_path[2048];
    FILE* fp = dsd_path_fopen_user_read_file(path, opened_path, sizeof opened_path);
    if (!fp) {
        return -1;
    }

    int rc = process_includes_stream(fp, cfg, depth, include_stack, include_stack_size);
    fclose(fp);
    return rc;
}

static int
build_target_profile_name(const char* profile_name, char* target_profile, size_t target_profile_size) {
    if (!target_profile || target_profile_size == 0) {
        return 0;
    }
    target_profile[0] = '\0';
    if (!profile_name || profile_name[0] == '\0') {
        return 0;
    }
    DSD_SNPRINTF(target_profile, target_profile_size, "profile.%s", profile_name);
    target_profile[target_profile_size - 1] = '\0';
    user_config_lowercase_ascii(target_profile);
    return 1;
}

static void
update_profile_section_state(const char* current_section, const char* target_profile, int* in_target_profile,
                             int* profile_found) {
    if (!in_target_profile || !profile_found || !current_section || !target_profile) {
        return;
    }
    *in_target_profile = (target_profile[0] != '\0' && strcmp(current_section, target_profile) == 0);
    if (*in_target_profile) {
        *profile_found = 1;
    }
}

static int
should_skip_profile_overlay_line(int in_target_profile, const char* current_section, const char* key) {
    if (in_target_profile) {
        return 0;
    }
    if (strncmp(current_section, "profile.", 8) == 0) {
        return 1;
    }
    if (current_section[0] == '\0' && dsd_strcasecmp(key, "include") == 0) {
        return 1;
    }
    return 1;
}

static int
handle_profile_overlay_key(dsdneoUserConfig* cfg, const char* current_section, int in_target_profile, const char* key,
                           const char* val) {
    if (in_target_profile) {
        apply_profile_key(cfg, key, val);
        return 1;
    }
    return should_skip_profile_overlay_line(in_target_profile, current_section, key);
}

static int
load_config_internal_stream(FILE* fp, const char* profile_name, dsdneoUserConfig* cfg) {
    if (!fp || !cfg) {
        return -1;
    }

    /* Includes are already processed by dsd_user_config_load_profile() before
     * calling this function. This function only extracts profile overlay keys. */

    char line[1024];
    char current_section[64];
    current_section[0] = '\0';
    char target_profile[64];
    (void)build_target_profile_name(profile_name, target_profile, sizeof target_profile);

    int in_target_profile = 0;
    int profile_found = 0;

    while (fgets(line, sizeof line, fp)) {
        user_config_trim_ascii_whitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }

        int section_result = parse_section_header_line(line, current_section, sizeof current_section);
        if (section_result < 0) {
            continue;
        }
        if (section_result > 0) {
            update_profile_section_state(current_section, target_profile, &in_target_profile, &profile_found);
            continue;
        }

        user_config_strip_inline_comment(line);
        user_config_trim_ascii_whitespace(line);
        if (line[0] == '\0') {
            continue;
        }

        char* key = NULL;
        char* val = NULL;
        if (!parse_key_value_fields(line, &key, &val)) {
            continue;
        }
        if (handle_profile_overlay_key(cfg, current_section, in_target_profile, key, val)) {
            continue;
        }
    }

    /* If a profile was requested but not found, return error */
    if (profile_name && *profile_name && !profile_found) {
        return -1;
    }

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

    char opened_path[2048];
    FILE* fp = dsd_path_fopen_user_read_file(path, opened_path, sizeof opened_path);
    if (!fp) {
        return -1;
    }

    int rc = load_config_internal_stream(fp, profile_name, cfg);
    fclose(fp);
    return rc;
}

int
dsd_user_config_load_profile(const char* path, const char* profile_name, dsdneoUserConfig* cfg) {
    if (!cfg) {
        return -1;
    }

    /* Reset config once at start */
    user_cfg_reset(cfg);
    char root_path[2048];
    if (dsd_path_expand_user(path, root_path, sizeof root_path) != 0) {
        return -1;
    }

    /* Process includes first (they provide base values that can be overridden) */
    const char* stack[1] = {root_path};
    int include_rc = process_includes(root_path, cfg, 0, stack, 1);
    if (include_rc != 0) {
        return include_rc;
    }

    /* Now load the main config (which overrides included values) */
    int rc = user_config_load_no_reset(root_path, cfg);
    if (rc != 0) {
        return rc;
    }

    /* If no profile requested, we're done */
    if (!profile_name || !*profile_name) {
        return 0;
    }

    /* Now overlay profile settings */
    return load_config_internal(root_path, profile_name, cfg, 0, stack, 1);
}

static int
extract_profile_section_name(char* line, const char** out_profile_name) {
    if (!line || !out_profile_name) {
        return 0;
    }
    user_config_trim_ascii_whitespace(line);
    if (line[0] != '[') {
        return 0;
    }
    char* end = strchr(line, ']');
    if (!end) {
        return 0;
    }
    *end = '\0';
    const char* section = line + 1;
    if (dsd_strncasecmp(section, "profile.", 8) != 0) {
        return 0;
    }
    const char* profile_name = section + 8;
    if (profile_name[0] == '\0') {
        return 0;
    }
    *out_profile_name = profile_name;
    return 1;
}

static int
append_profile_name_to_buffer(const char* profile_name, const char** names, int* count, int max_names, char* names_buf,
                              size_t names_buf_size, size_t* buf_used) {
    if (!profile_name || !names || !count || !names_buf || !buf_used || *count >= max_names) {
        return 0;
    }
    size_t name_len = strlen(profile_name);
    if (*buf_used + name_len + 1 > names_buf_size) {
        return 0;
    }
    names[*count] = names_buf + *buf_used;
    DSD_MEMCPY(names_buf + *buf_used, profile_name, name_len + 1);
    *buf_used += name_len + 1;
    (*count)++;
    return 1;
}

int
dsd_user_config_list_profiles_stream(FILE* stream, const char** names, char* names_buf, size_t names_buf_size,
                                     int max_names) {
    if (!stream || !names || !names_buf || names_buf_size == 0 || max_names <= 0) {
        return -1;
    }

    if (seek_config_stream_to_start(stream) != 0) {
        return -1;
    }

    int count = 0;
    size_t buf_used = 0;
    char line[1024];

    while (fgets(line, sizeof line, stream) && count < max_names) {
        const char* profile_name = NULL;
        if (!extract_profile_section_name(line, &profile_name)) {
            continue;
        }
        if (!append_profile_name_to_buffer(profile_name, names, &count, max_names, names_buf, names_buf_size,
                                           &buf_used)) {
            break; /* buffer full */
        }
    }

    return count;
}

int
dsd_user_config_list_profiles(const char* path, const char** names, char* names_buf, size_t names_buf_size,
                              int max_names) {
    if (!path || !names || !names_buf || names_buf_size == 0 || max_names <= 0) {
        return -1;
    }

    char opened_path[2048];
    FILE* fp = dsd_path_fopen_user_read_file(path, opened_path, sizeof opened_path);
    if (!fp) {
        return -1;
    }

    int count = dsd_user_config_list_profiles_stream(fp, names, names_buf, names_buf_size, max_names);
    fclose(fp);
    return count;
}
