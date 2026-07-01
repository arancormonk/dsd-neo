// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * User-facing INI configuration for DSD-neo.
 *
 * Parses and writes a small, stable subset of options (input/output/mode/
 * trunking) to allow users to persist common preferences without impacting
 * existing CLI/environment workflows.
 */

#include <cmath>
#include <dsd-neo/app_control/frontend_types.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/config_schema.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/path_policy.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_user_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/runtime/call_alert.h"

#if defined(_WIN32)
#include <windows.h>
#endif

// Internal helpers ------------------------------------------------------------

// Local helper to convert linear power to dB (avoids dependency on dsd_misc.c)
static double
local_pwr_to_dB(double mean_power) {
    if (mean_power <= 0) {
        return -120.0;
    }
    double dB = 10.0 * std::log10(mean_power);
    if (dB > 0.0) {
        dB = 0.0;
    }
    if (dB < -120.0) {
        dB = -120.0;
    }
    return dB;
}

// Local helper to convert dB to linear power (avoids dependency on dsd_misc.c)
static double
local_dB_to_pwr(double dB) {
    if (dB >= 0.0) {
        return 1.0;
    }
    if (dB < -200.0) {
        dB = -200.0;
    }
    const double k_ln10_over_10 = 2.302585092994046 / 10.0;
    double pwr = std::exp(dB * k_ln10_over_10);
    if (pwr < 0.0) {
        pwr = 0.0;
    }
    if (pwr > 1.0) {
        pwr = 1.0;
    }
    return pwr;
}

void
user_cfg_reset(dsdneoUserConfig* cfg) {
    if (!cfg) {
        return;
    }
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
    cfg->version = 1;
    // Set trunking tune defaults to match main.c init defaults
    cfg->trunk_tune_group_calls = 1;
    cfg->trunk_tune_private_calls = 1;
    cfg->trunk_tune_data_calls = 0;
    cfg->trunk_tune_enc_calls = 1;
    cfg->trunk_scan_idle_dwell_ms = 3000;
    cfg->trunk_scan_activity_hold_ms = 1200;

    cfg->rtl_auto_ppm = 0;
    cfg->soapy_bandwidth_hz = -1;
    cfg->soapy_bandwidth_hz_is_set = 0;

    cfg->call_alert_enabled = 0;
    cfg->call_alert_events = DSD_CALL_ALERT_EVENT_ALL;

    // Recording defaults (match initOpts)
    cfg->per_call_wav = 0;
    DSD_SNPRINTF(cfg->per_call_wav_dir, sizeof cfg->per_call_wav_dir, "%s", "./WAV");
    cfg->per_call_wav_dir[sizeof cfg->per_call_wav_dir - 1] = '\0';
    cfg->rdio_mode = DSD_RDIO_MODE_OFF;
    cfg->rdio_system_id = 0;
    DSD_SNPRINTF(cfg->rdio_api_url, sizeof cfg->rdio_api_url, "%s", "http://127.0.0.1:3000");
    cfg->rdio_api_url[sizeof cfg->rdio_api_url - 1] = '\0';
    cfg->rdio_api_key[0] = '\0';
    cfg->rdio_upload_timeout_ms = 5000;
    cfg->rdio_upload_retries = 1;
    cfg->rdio_api_delete_after_upload = 0;

    // DSP defaults (match runtime defaults)
    cfg->iq_balance = 0;
    cfg->iq_dc_block = 0;
}

static void
close_frame_log_handle(dsd_opts* opts) {
    if (!opts || opts->frame_log_f == NULL) {
        return;
    }
    fflush(opts->frame_log_f);
    fclose(opts->frame_log_f);
    opts->frame_log_f = NULL;
}

static void
close_p25_sm_log_handle(dsd_opts* opts) {
    if (!opts || opts->p25_sm_log_f == NULL) {
        return;
    }
    fflush(opts->p25_sm_log_f);
    fclose(opts->p25_sm_log_f);
    opts->p25_sm_log_f = NULL;
}

namespace {

struct decode_mode_name_map_t {
    dsdneoUserDecodeMode mode;
    const char* value;
};

struct decode_mode_alias_map_t {
    const char* alias;
    dsdneoUserDecodeMode mode;
};

} // namespace

static const decode_mode_name_map_t k_decode_mode_names[] = {
    {DSDCFG_MODE_AUTO, "auto"},         {DSDCFG_MODE_P25P1, "p25p1"},   {DSDCFG_MODE_P25P2, "p25p2"},
    {DSDCFG_MODE_DMR, "dmr"},           {DSDCFG_MODE_NXDN48, "nxdn48"}, {DSDCFG_MODE_NXDN96, "nxdn96"},
    {DSDCFG_MODE_X2TDMA, "x2tdma"},     {DSDCFG_MODE_YSF, "ysf"},       {DSDCFG_MODE_DSTAR, "dstar"},
    {DSDCFG_MODE_EDACS_PV, "edacs_pv"}, {DSDCFG_MODE_DPMR, "dpmr"},     {DSDCFG_MODE_M17, "m17"},
    {DSDCFG_MODE_TDMA, "tdma"},         {DSDCFG_MODE_ANALOG, "analog"},
};

static const decode_mode_alias_map_t k_decode_mode_aliases[] = {
    {"p25p1_only", DSDCFG_MODE_P25P1},  {"p25p2_only", DSDCFG_MODE_P25P2},      {"edacs", DSDCFG_MODE_EDACS_PV},
    {"provoice", DSDCFG_MODE_EDACS_PV}, {"analog_monitor", DSDCFG_MODE_ANALOG},
};

static const char*
decode_mode_to_ini_name(dsdneoUserDecodeMode mode) {
    for (size_t i = 0; i < sizeof(k_decode_mode_names) / sizeof(k_decode_mode_names[0]); i++) {
        if (k_decode_mode_names[i].mode == mode) {
            return k_decode_mode_names[i].value;
        }
    }
    return NULL;
}

int
user_config_parse_decode_mode_value(const char* val, dsdneoUserDecodeMode* out_mode, int* used_compat_alias) {
    if (!val || !*val || !out_mode) {
        return -1;
    }
    if (used_compat_alias) {
        *used_compat_alias = 0;
    }

    for (size_t i = 0; i < sizeof(k_decode_mode_names) / sizeof(k_decode_mode_names[0]); i++) {
        if (dsd_strcasecmp(val, k_decode_mode_names[i].value) == 0) {
            *out_mode = k_decode_mode_names[i].mode;
            return 0;
        }
    }
    for (size_t i = 0; i < sizeof(k_decode_mode_aliases) / sizeof(k_decode_mode_aliases[0]); i++) {
        if (dsd_strcasecmp(val, k_decode_mode_aliases[i].alias) == 0) {
            *out_mode = k_decode_mode_aliases[i].mode;
            if (used_compat_alias) {
                *used_compat_alias = 1;
            }
            return 0;
        }
    }

    return -1;
}

int
user_config_is_mode_decode_key(const char* section, const char* key) {
    if (!section || !key) {
        return 0;
    }
    return dsd_strcasecmp(section, "mode") == 0 && dsd_strcasecmp(key, "decode") == 0;
}

static void
copy_token_string(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    DSD_SNPRINTF(dst, dst_size, "%s", src);
    dst[dst_size - 1] = '\0';
}

static size_t
split_colon_tokens(char* scratch, size_t scratch_size, const char* in, char** out_tokens, size_t max_tokens) {
    if (!scratch || scratch_size == 0 || !in || !out_tokens || max_tokens == 0) {
        return 0;
    }

    DSD_SNPRINTF(scratch, scratch_size, "%.*s", (int)(scratch_size - 1), in);
    scratch[scratch_size - 1] = '\0';

    size_t count = 0;
    char* save = NULL;
    for (char* tok = dsd_strtok_r(scratch, ":", &save); tok && count < max_tokens;
         tok = dsd_strtok_r(NULL, ":", &save)) {
        out_tokens[count++] = tok;
    }
    return count;
}

static int
parse_int_atoi_compat(const char* text) {
    if (!text || *text == '\0') {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    long v = strtol(text, &end, 10);
    if (end == text || (end && *end != '\0') || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return 0;
    }
    return (int)v;
}

static int
is_valid_rtl_bw_khz(int bw) {
    return (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16 || bw == 24 || bw == 48);
}

static int
resolve_configured_rtl_ppm(const dsdneoUserConfig* cfg, const dsd_opts* opts) {
    if (!cfg) {
        return opts ? opts->rtlsdr_ppm_error : 0;
    }
    if (cfg->rtl_ppm_is_set) {
        return cfg->rtl_ppm;
    }
    return opts ? opts->rtlsdr_ppm_error : 0;
}

static void
apply_shared_radio_tuning_from_config(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg || !opts) {
        return;
    }

    if (cfg->rtl_freq[0]) {
        opts->rtlsdr_center_freq = dsd_parse_freq_hz(cfg->rtl_freq);
    }

    int gain = cfg->rtl_gain ? cfg->rtl_gain : opts->rtl_gain_value;
    int ppm = resolve_configured_rtl_ppm(cfg, opts);
    int bw = cfg->rtl_bw_khz ? cfg->rtl_bw_khz : opts->rtl_dsp_bw_khz;
    int sql = cfg->rtl_sql;
    int vol = cfg->rtl_volume ? cfg->rtl_volume : opts->rtl_volume_multiplier;

    if (!is_valid_rtl_bw_khz(bw)) {
        bw = 48;
    }

    opts->rtl_gain_value = gain;
    opts->rtlsdr_ppm_error = ppm;
    opts->rtl_dsp_bw_khz = bw;
    if (sql < 0) {
        opts->rtl_squelch_level = local_dB_to_pwr((double)sql);
    } else {
        opts->rtl_squelch_level = (double)sql;
    }
    opts->rtl_volume_multiplier = vol;
}

static void
apply_soapy_tuning_from_config(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg || !opts) {
        return;
    }
    if (cfg->soapy_profile[0]) {
        copy_token_string(opts->soapy_profile, sizeof opts->soapy_profile, cfg->soapy_profile);
    }
    if (cfg->soapy_stream_format[0]) {
        copy_token_string(opts->soapy_stream_format, sizeof opts->soapy_stream_format, cfg->soapy_stream_format);
    }
    if (cfg->soapy_antenna[0]) {
        copy_token_string(opts->soapy_antenna, sizeof opts->soapy_antenna, cfg->soapy_antenna);
    }
    if (cfg->soapy_clock[0]) {
        copy_token_string(opts->soapy_clock, sizeof opts->soapy_clock, cfg->soapy_clock);
    }
    if (cfg->soapy_settings[0]) {
        copy_token_string(opts->soapy_settings, sizeof opts->soapy_settings, cfg->soapy_settings);
    }
    if (cfg->soapy_gains[0]) {
        copy_token_string(opts->soapy_gains, sizeof opts->soapy_gains, cfg->soapy_gains);
    }
    if (cfg->soapy_bandwidth_hz_is_set) {
        opts->soapy_bandwidth_hz = cfg->soapy_bandwidth_hz;
    }
}

static void
snapshot_apply_live_rtl_values(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    if (!opts || !cfg) {
        return;
    }

    cfg->rtl_gain = opts->rtl_gain_value;
    cfg->rtl_ppm = opts->rtlsdr_ppm_error;
    cfg->rtl_ppm_is_set = 1;
    cfg->rtl_bw_khz = opts->rtl_dsp_bw_khz;
    cfg->rtl_sql = (int)local_pwr_to_dB(opts->rtl_squelch_level);
    cfg->rtl_volume = opts->rtl_volume_multiplier;
    if (opts->rtlsdr_center_freq > 0) {
        DSD_SNPRINTF(cfg->rtl_freq, sizeof cfg->rtl_freq, "%u", opts->rtlsdr_center_freq);
        cfg->rtl_freq[sizeof cfg->rtl_freq - 1] = '\0';
    }
}

static void
snapshot_apply_live_soapy_values(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    if (!opts || !cfg) {
        return;
    }
    if (opts->soapy_profile[0]) {
        copy_token_string(cfg->soapy_profile, sizeof cfg->soapy_profile, opts->soapy_profile);
    }
    if (opts->soapy_stream_format[0]) {
        copy_token_string(cfg->soapy_stream_format, sizeof cfg->soapy_stream_format, opts->soapy_stream_format);
    }
    if (opts->soapy_antenna[0]) {
        copy_token_string(cfg->soapy_antenna, sizeof cfg->soapy_antenna, opts->soapy_antenna);
    }
    if (opts->soapy_clock[0]) {
        copy_token_string(cfg->soapy_clock, sizeof cfg->soapy_clock, opts->soapy_clock);
    }
    if (opts->soapy_settings[0]) {
        copy_token_string(cfg->soapy_settings, sizeof cfg->soapy_settings, opts->soapy_settings);
    }
    if (opts->soapy_gains[0]) {
        copy_token_string(cfg->soapy_gains, sizeof cfg->soapy_gains, opts->soapy_gains);
    }
    if (opts->soapy_bandwidth_hz >= 0) {
        cfg->soapy_bandwidth_hz = opts->soapy_bandwidth_hz;
        cfg->soapy_bandwidth_hz_is_set = 1;
    }
}

static void
snapshot_parse_rtl_device_spec(const char* audio_in_dev, dsdneoUserConfig* cfg) {
    if (!audio_in_dev || !cfg) {
        return;
    }

    char buf[1024];
    char* tok[9] = {0};
    size_t n = split_colon_tokens(buf, sizeof buf, audio_in_dev, tok, sizeof(tok) / sizeof(tok[0]));
    if (n > 1) {
        cfg->rtl_device = parse_int_atoi_compat(tok[1]);
    }
    if (n > 2) {
        copy_token_string(cfg->rtl_freq, sizeof cfg->rtl_freq, tok[2]);
    }
    if (n > 3) {
        cfg->rtl_gain = parse_int_atoi_compat(tok[3]);
    }
    if (n > 4) {
        cfg->rtl_ppm = parse_int_atoi_compat(tok[4]);
        cfg->rtl_ppm_is_set = 1;
    }
    if (n > 5) {
        cfg->rtl_bw_khz = parse_int_atoi_compat(tok[5]);
    }
    if (n > 6) {
        cfg->rtl_sql = parse_int_atoi_compat(tok[6]);
    }
    if (n > 7) {
        cfg->rtl_volume = parse_int_atoi_compat(tok[7]);
    }
}

static void
snapshot_parse_rtltcp_device_spec(const char* audio_in_dev, dsdneoUserConfig* cfg) {
    if (!audio_in_dev || !cfg) {
        return;
    }

    char buf[1024];
    char* tok[10] = {0};
    size_t n = split_colon_tokens(buf, sizeof buf, audio_in_dev, tok, sizeof(tok) / sizeof(tok[0]));
    if (n > 1) {
        copy_token_string(cfg->rtltcp_host, sizeof cfg->rtltcp_host, tok[1]);
    }
    if (n > 2) {
        cfg->rtltcp_port = parse_int_atoi_compat(tok[2]);
    }
    if (n > 3) {
        copy_token_string(cfg->rtl_freq, sizeof cfg->rtl_freq, tok[3]);
    }
    if (n > 4) {
        cfg->rtl_gain = parse_int_atoi_compat(tok[4]);
    }
    if (n > 5) {
        cfg->rtl_ppm = parse_int_atoi_compat(tok[5]);
        cfg->rtl_ppm_is_set = 1;
    }
    if (n > 6) {
        cfg->rtl_bw_khz = parse_int_atoi_compat(tok[6]);
    }
    if (n > 7) {
        cfg->rtl_sql = parse_int_atoi_compat(tok[7]);
    }
    if (n > 8) {
        cfg->rtl_volume = parse_int_atoi_compat(tok[8]);
    }
}

static void
snapshot_parse_soapy_device_spec(const char* audio_in_dev, dsdneoUserConfig* cfg) {
    if (!audio_in_dev || !cfg) {
        return;
    }

    const char* colon = strchr(audio_in_dev, ':');
    if (colon && *(colon + 1) != '\0') {
        copy_token_string(cfg->soapy_args, sizeof cfg->soapy_args, colon + 1);
    }
}

static void
snapshot_parse_host_port_spec(const char* audio_in_dev, char* host, size_t host_size, int* port) {
    if (!audio_in_dev || !host || host_size == 0 || !port) {
        return;
    }

    char buf[512];
    char* tok[4] = {0};
    size_t n = split_colon_tokens(buf, sizeof buf, audio_in_dev, tok, sizeof(tok) / sizeof(tok[0]));
    if (n > 1) {
        copy_token_string(host, host_size, tok[1]);
    }
    if (n > 2) {
        *port = parse_int_atoi_compat(tok[2]);
    }
}

static void
ensure_dir_exists(const char* path) {
    if (!path || !*path) {
        return;
    }

    char buf[1024];
    DSD_SNPRINTF(buf, sizeof buf, "%s", path);
    buf[sizeof buf - 1] = '\0';

    // strip trailing slash if present
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '/' || buf[n - 1] == '\\')) {
        buf[--n] = '\0';
    }

    if (n == 0) {
        return;
    }

    // Walk components and mkdir progressively.
    char* p = buf;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (strlen(buf) > 0) {
                dsd_mkdir(buf, 0700);
            }
            *p = saved;
        }
        p++;
    }
    dsd_mkdir(buf, 0700);
}

// Default path ---------------------------------------------------------------

const char*
dsd_user_config_default_path(void) {
    static char buf[1024];
    static int inited = 0;

    if (inited) {
        return buf[0] ? buf : NULL;
    }

    buf[0] = '\0';

#if defined(_WIN32)
    const char* appdata = getenv("APPDATA");
    if (appdata && *appdata) {
        DSD_SNPRINTF(buf, sizeof buf, "%s\\dsd-neo\\config.ini", appdata);
        buf[sizeof buf - 1] = '\0';
    }
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        DSD_SNPRINTF(buf, sizeof buf, "%s/dsd-neo/config.ini", xdg);
        buf[sizeof buf - 1] = '\0';
    } else {
        const char* home = getenv("HOME");
        if (home && *home) {
            DSD_SNPRINTF(buf, sizeof buf, "%s/.config/dsd-neo/config.ini", home);
            buf[sizeof buf - 1] = '\0';
        }
    }
#endif

    inited = 1;
    return buf[0] ? buf : NULL;
}

// INI writer ------------------------------------------------------------------

static int
config_parent_dir(const char* save_path, char* dir, size_t dir_size) {
    if (!save_path || !dir || dir_size == 0) {
        return -1;
    }

    int n = DSD_SNPRINTF(dir, dir_size, "%s", save_path);
    if (n < 0 || n >= (int)dir_size) {
        return -1;
    }
    dir[dir_size - 1] = '\0';

    char* last_sep = strrchr(dir, '/');
#if defined(_WIN32)
    char* last_sep2 = strrchr(dir, '\\');
    if (!last_sep || (last_sep2 && last_sep2 > last_sep)) {
        last_sep = last_sep2;
    }
#endif
    if (!last_sep) {
        dir[0] = '\0';
        return 0;
    }

    *last_sep = '\0';
    return 1;
}

static int
config_write_temp_file(FILE* fp, const dsdneoUserConfig* cfg) {
    if (!fp) {
        return -1;
    }

    dsd_user_config_render_ini(cfg, fp);

    if (fflush(fp) != 0) {
        int saved_errno = errno;
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    int fd = dsd_fileno(fp);
    if (fd >= 0) {
        (void)dsd_fchmod(fd, 0600);
        if (dsd_fsync(fd) != 0) {
            int saved_errno = errno;
            fclose(fp);
            errno = saved_errno;
            return -1;
        }
    }

    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

static int
config_replace_temp_file(const char* tmp, const char* save_path) {
    if (dsd_replace_file_with_temp(tmp, save_path) != 0) {
        (void)remove(tmp);
        return -1;
    }
    return 0;
}

int
dsd_user_config_save_atomic(const char* path, const dsdneoUserConfig* cfg) {
    if (!path || !*path || !cfg) {
        return -1;
    }

    char save_path[2048];
    if (dsd_path_expand_user(path, save_path, sizeof save_path) != 0) {
        return -1;
    }

    char dir[2048];
    int has_dir = config_parent_dir(save_path, dir, sizeof dir);
    if (has_dir < 0) {
        return -1;
    }
    if (has_dir) {
        ensure_dir_exists(dir);
    }

    char tmp[2048];
    FILE* tmp_fp = dsd_fopen_private_temp_for_replace(save_path, tmp, sizeof tmp, "w");
    if (!tmp_fp) {
        return -1;
    }

    if (config_write_temp_file(tmp_fp, cfg) != 0) {
        (void)remove(tmp);
        return -1;
    }

    return config_replace_temp_file(tmp, save_path);
}

static const char*
ini_bool(int value) {
    return value ? "true" : "false";
}

static const char*
frontend_kind_to_ini_name(dsd_frontend_kind frontend) {
    switch (frontend) {
        case DSD_FRONTEND_TERMINAL: return "terminal";
        case DSD_FRONTEND_NATIVE: return "native";
        case DSD_FRONTEND_NONE:
        default: return "none";
    }
}

static void
render_input_source(FILE* out, dsdneoUserInputSource source) {
    switch (source) {
        case DSDCFG_INPUT_PULSE: DSD_FPRINTF(out, "source = \"pulse\"\n"); break;
        case DSDCFG_INPUT_RTL: DSD_FPRINTF(out, "source = \"rtl\"\n"); break;
        case DSDCFG_INPUT_RTLTCP: DSD_FPRINTF(out, "source = \"rtltcp\"\n"); break;
        case DSDCFG_INPUT_SOAPY: DSD_FPRINTF(out, "source = \"soapy\"\n"); break;
        case DSDCFG_INPUT_FILE: DSD_FPRINTF(out, "source = \"file\"\n"); break;
        case DSDCFG_INPUT_TCP: DSD_FPRINTF(out, "source = \"tcp\"\n"); break;
        case DSDCFG_INPUT_UDP: DSD_FPRINTF(out, "source = \"udp\"\n"); break;
        default: break;
    }
}

static void
render_input_rtl_common(FILE* out, const dsdneoUserConfig* cfg, int include_auto_ppm) {
    if (!out || !cfg) {
        return;
    }
    if (cfg->rtl_freq[0]) {
        DSD_FPRINTF(out, "rtl_freq = \"%s\"\n", cfg->rtl_freq);
    }
    if (cfg->rtl_gain) {
        DSD_FPRINTF(out, "rtl_gain = %d\n", cfg->rtl_gain);
    }
    if (cfg->rtl_ppm_is_set) {
        DSD_FPRINTF(out, "rtl_ppm = %d\n", cfg->rtl_ppm);
    }
    if (cfg->rtl_bw_khz) {
        DSD_FPRINTF(out, "rtl_bw_khz = %d\n", cfg->rtl_bw_khz);
    }
    DSD_FPRINTF(out, "rtl_sql = %d\n", cfg->rtl_sql);
    if (cfg->rtl_volume) {
        DSD_FPRINTF(out, "rtl_volume = %d\n", cfg->rtl_volume);
    }
    if (include_auto_ppm) {
        DSD_FPRINTF(out, "auto_ppm = %s\n", ini_bool(cfg->rtl_auto_ppm));
    }
}

static void
render_input_pulse(FILE* out, const dsdneoUserConfig* cfg) {
    if (cfg->pulse_input[0]) {
        DSD_FPRINTF(out, "pulse_source = \"%s\"\n", cfg->pulse_input);
    }
}

static void
render_input_rtl(FILE* out, const dsdneoUserConfig* cfg) {
    DSD_FPRINTF(out, "rtl_device = %d\n", cfg->rtl_device);
    render_input_rtl_common(out, cfg, 1);
}

static void
render_input_rtltcp(FILE* out, const dsdneoUserConfig* cfg) {
    if (cfg->rtltcp_host[0]) {
        DSD_FPRINTF(out, "rtltcp_host = \"%s\"\n", cfg->rtltcp_host);
    }
    if (cfg->rtltcp_port) {
        DSD_FPRINTF(out, "rtltcp_port = %d\n", cfg->rtltcp_port);
    }
    render_input_rtl_common(out, cfg, 1);
}

static void
render_input_soapy(FILE* out, const dsdneoUserConfig* cfg) {
    if (cfg->soapy_args[0]) {
        DSD_FPRINTF(out, "soapy_args = \"%s\"\n", cfg->soapy_args);
    }
    if (cfg->soapy_profile[0]) {
        DSD_FPRINTF(out, "soapy_profile = \"%s\"\n", cfg->soapy_profile);
    }
    if (cfg->soapy_stream_format[0]) {
        DSD_FPRINTF(out, "soapy_stream_format = \"%s\"\n", cfg->soapy_stream_format);
    }
    if (cfg->soapy_antenna[0]) {
        DSD_FPRINTF(out, "soapy_antenna = \"%s\"\n", cfg->soapy_antenna);
    }
    if (cfg->soapy_clock[0]) {
        DSD_FPRINTF(out, "soapy_clock = \"%s\"\n", cfg->soapy_clock);
    }
    if (cfg->soapy_settings[0]) {
        DSD_FPRINTF(out, "soapy_settings = \"%s\"\n", cfg->soapy_settings);
    }
    if (cfg->soapy_gains[0]) {
        DSD_FPRINTF(out, "soapy_gains = \"%s\"\n", cfg->soapy_gains);
    }
    if (cfg->soapy_bandwidth_hz_is_set) {
        DSD_FPRINTF(out, "soapy_bandwidth_hz = %d\n", cfg->soapy_bandwidth_hz);
    }
    render_input_rtl_common(out, cfg, 0);
}

static void
render_input_file(FILE* out, const dsdneoUserConfig* cfg) {
    if (cfg->file_path[0]) {
        DSD_FPRINTF(out, "file_path = \"%s\"\n", cfg->file_path);
    }
    if (cfg->file_sample_rate) {
        DSD_FPRINTF(out, "file_sample_rate = %d\n", cfg->file_sample_rate);
    }
}

static void
render_input_tcp(FILE* out, const dsdneoUserConfig* cfg) {
    if (cfg->tcp_host[0]) {
        DSD_FPRINTF(out, "tcp_host = \"%s\"\n", cfg->tcp_host);
    }
    if (cfg->tcp_port) {
        DSD_FPRINTF(out, "tcp_port = %d\n", cfg->tcp_port);
    }
}

static void
render_input_udp(FILE* out, const dsdneoUserConfig* cfg) {
    if (cfg->udp_addr[0]) {
        DSD_FPRINTF(out, "udp_addr = \"%s\"\n", cfg->udp_addr);
    }
    if (cfg->udp_port) {
        DSD_FPRINTF(out, "udp_port = %d\n", cfg->udp_port);
    }
}

static void
render_input_section(FILE* out, const dsdneoUserConfig* cfg) {
    DSD_FPRINTF(out, "[input]\n");
    render_input_source(out, cfg->input_source);
    switch (cfg->input_source) {
        case DSDCFG_INPUT_PULSE: render_input_pulse(out, cfg); break;
        case DSDCFG_INPUT_RTL: render_input_rtl(out, cfg); break;
        case DSDCFG_INPUT_RTLTCP: render_input_rtltcp(out, cfg); break;
        case DSDCFG_INPUT_SOAPY: render_input_soapy(out, cfg); break;
        case DSDCFG_INPUT_FILE: render_input_file(out, cfg); break;
        case DSDCFG_INPUT_TCP: render_input_tcp(out, cfg); break;
        case DSDCFG_INPUT_UDP: render_input_udp(out, cfg); break;
        default: break;
    }
    DSD_FPRINTF(out, "\n");
}

static void
render_output_section(FILE* out, const dsdneoUserConfig* cfg) {
    DSD_FPRINTF(out, "[output]\n");
    switch (cfg->output_backend) {
        case DSDCFG_OUTPUT_PULSE: DSD_FPRINTF(out, "backend = \"pulse\"\n"); break;
        case DSDCFG_OUTPUT_NULL: DSD_FPRINTF(out, "backend = \"null\"\n"); break;
        default: break;
    }
    if (cfg->pulse_output[0]) {
        DSD_FPRINTF(out, "pulse_sink = \"%s\"\n", cfg->pulse_output);
    }
    if (cfg->frontend_kind_is_set) {
        DSD_FPRINTF(out, "frontend = \"%s\"\n", frontend_kind_to_ini_name(cfg->frontend_kind));
    }
    DSD_FPRINTF(out, "\n");
}

static void
render_mode_section(FILE* out, const dsdneoUserConfig* cfg) {
    DSD_FPRINTF(out, "[mode]\n");
    const char* decode_name = decode_mode_to_ini_name(cfg->decode_mode);
    if (decode_name) {
        DSD_FPRINTF(out, "decode = \"%s\"\n", decode_name);
    }
    if (cfg->has_demod) {
        switch (cfg->demod_path) {
            case DSDCFG_DEMOD_AUTO: DSD_FPRINTF(out, "demod = \"auto\"\n"); break;
            case DSDCFG_DEMOD_C4FM: DSD_FPRINTF(out, "demod = \"c4fm\"\n"); break;
            case DSDCFG_DEMOD_GFSK: DSD_FPRINTF(out, "demod = \"gfsk\"\n"); break;
            case DSDCFG_DEMOD_QPSK: DSD_FPRINTF(out, "demod = \"qpsk\"\n"); break;
            default: break;
        }
    }
    DSD_FPRINTF(out, "\n");
}

static void
render_trunking_section(FILE* out, const dsdneoUserConfig* cfg) {
    DSD_FPRINTF(out, "[trunking]\n");
    DSD_FPRINTF(out, "enabled = %s\n", ini_bool(cfg->trunk_enabled));
    if (cfg->trunk_chan_csv[0]) {
        DSD_FPRINTF(out, "chan_csv = \"%s\"\n", cfg->trunk_chan_csv);
    }
    if (cfg->trunk_group_csv[0]) {
        DSD_FPRINTF(out, "group_csv = \"%s\"\n", cfg->trunk_group_csv);
    }
    DSD_FPRINTF(out, "allow_list = %s\n", ini_bool(cfg->trunk_use_allow_list));
    DSD_FPRINTF(out, "tune_group_calls = %s\n", ini_bool(cfg->trunk_tune_group_calls));
    DSD_FPRINTF(out, "tune_private_calls = %s\n", ini_bool(cfg->trunk_tune_private_calls));
    DSD_FPRINTF(out, "tune_data_calls = %s\n", ini_bool(cfg->trunk_tune_data_calls));
    DSD_FPRINTF(out, "tune_enc_calls = %s\n", ini_bool(cfg->trunk_tune_enc_calls));
    DSD_FPRINTF(out, "\n");
}

static void
render_trunk_scan_section(FILE* out, const dsdneoUserConfig* cfg) {
    DSD_FPRINTF(out, "[trunk_scan]\n");
    DSD_FPRINTF(out, "enabled = %s\n", ini_bool(cfg->trunk_scan_enabled));
    if (cfg->trunk_scan_targets_csv[0]) {
        DSD_FPRINTF(out, "targets_csv = \"%s\"\n", cfg->trunk_scan_targets_csv);
    }
    DSD_FPRINTF(out, "idle_dwell_ms = %d\n", cfg->trunk_scan_idle_dwell_ms);
    DSD_FPRINTF(out, "activity_hold_ms = %d\n", cfg->trunk_scan_activity_hold_ms);
    DSD_FPRINTF(out, "\n");
}

static void
render_logging_section(FILE* out, const dsdneoUserConfig* cfg) {
    DSD_FPRINTF(out, "[logging]\n");
    if (cfg->event_log[0]) {
        DSD_FPRINTF(out, "event_log = \"%s\"\n", cfg->event_log);
    }
    if (cfg->frame_log[0]) {
        DSD_FPRINTF(out, "frame_log = \"%s\"\n", cfg->frame_log);
    }
    if (cfg->p25_sm_log[0]) {
        DSD_FPRINTF(out, "p25_sm_log = \"%s\"\n", cfg->p25_sm_log);
    }
    DSD_FPRINTF(out, "\n");
}

static void
render_alerts_section(FILE* out, const dsdneoUserConfig* cfg) {
    uint8_t events = dsd_call_alert_mask_events((uint8_t)cfg->call_alert_events);
    DSD_FPRINTF(out, "[alerts]\n");
    DSD_FPRINTF(out, "enabled = %s\n", ini_bool(cfg->call_alert_enabled));
    DSD_FPRINTF(out, "voice_start = %s\n", (events & DSD_CALL_ALERT_EVENT_VOICE_START) ? "true" : "false");
    DSD_FPRINTF(out, "voice_end = %s\n", (events & DSD_CALL_ALERT_EVENT_VOICE_END) ? "true" : "false");
    DSD_FPRINTF(out, "data = %s\n", (events & DSD_CALL_ALERT_EVENT_DATA) ? "true" : "false");
    DSD_FPRINTF(out, "\n");
}

static void
render_recording_section(FILE* out, const dsdneoUserConfig* cfg) {
    DSD_FPRINTF(out, "[recording]\n");
    DSD_FPRINTF(out, "per_call_wav = %s\n", ini_bool(cfg->per_call_wav));
    if (cfg->per_call_wav_dir[0]) {
        DSD_FPRINTF(out, "per_call_wav_dir = \"%s\"\n", cfg->per_call_wav_dir);
    }
    if (cfg->static_wav_path[0]) {
        DSD_FPRINTF(out, "static_wav = \"%s\"\n", cfg->static_wav_path);
    }
    if (cfg->raw_wav_path[0]) {
        DSD_FPRINTF(out, "raw_wav = \"%s\"\n", cfg->raw_wav_path);
    }
    DSD_FPRINTF(out, "rdio_mode = \"%s\"\n", dsd_rdio_mode_to_string(cfg->rdio_mode));
    if (cfg->rdio_system_id > 0) {
        DSD_FPRINTF(out, "rdio_system_id = %d\n", cfg->rdio_system_id);
    }
    if (cfg->rdio_api_url[0]) {
        DSD_FPRINTF(out, "rdio_api_url = \"%s\"\n", cfg->rdio_api_url);
    }
    if (cfg->rdio_api_key[0]) {
        DSD_FPRINTF(out, "rdio_api_key = \"%s\"\n", cfg->rdio_api_key);
    }
    if (cfg->rdio_upload_timeout_ms > 0) {
        DSD_FPRINTF(out, "rdio_upload_timeout_ms = %d\n", cfg->rdio_upload_timeout_ms);
    }
    if (cfg->rdio_upload_retries >= 0) {
        DSD_FPRINTF(out, "rdio_upload_retries = %d\n", cfg->rdio_upload_retries);
    }
    DSD_FPRINTF(out, "rdio_api_delete_after_upload = %s\n", ini_bool(cfg->rdio_api_delete_after_upload));
    DSD_FPRINTF(out, "\n");
}

static void
render_dsp_section(FILE* out, const dsdneoUserConfig* cfg) {
    DSD_FPRINTF(out, "[dsp]\n");
    DSD_FPRINTF(out, "iq_balance = %s\n", ini_bool(cfg->iq_balance));
    DSD_FPRINTF(out, "iq_dc_block = %s\n", ini_bool(cfg->iq_dc_block));
    DSD_FPRINTF(out, "\n");
}

void
dsd_user_config_render_ini(const dsdneoUserConfig* cfg, FILE* stream) {
    if (!cfg || !stream) {
        return;
    }

    DSD_FPRINTF(stream, "version = %d\n\n", cfg->version > 0 ? cfg->version : 1);
    if (cfg->has_input) {
        render_input_section(stream, cfg);
    }
    if (cfg->has_output) {
        render_output_section(stream, cfg);
    }
    if (cfg->has_mode) {
        render_mode_section(stream, cfg);
    }
    if (cfg->has_trunking) {
        render_trunking_section(stream, cfg);
    }
    if (cfg->has_trunk_scan) {
        render_trunk_scan_section(stream, cfg);
    }
    if (cfg->has_logging) {
        render_logging_section(stream, cfg);
    }
    if (cfg->has_alerts) {
        render_alerts_section(stream, cfg);
    }
    if (cfg->has_recording) {
        render_recording_section(stream, cfg);
    }
    if (cfg->has_dsp) {
        render_dsp_section(stream, cfg);
    }
}

// Mapping helpers -------------------------------------------------------------

static void
resolve_rtl_spec_values(const dsdneoUserConfig* cfg, const dsd_opts* opts, int* gain, int* ppm, int* bw, int* sql,
                        int* vol) {
    if (!cfg || !opts || !gain || !ppm || !bw || !sql || !vol) {
        return;
    }
    *gain = cfg->rtl_gain ? cfg->rtl_gain : opts->rtl_gain_value;
    *ppm = resolve_configured_rtl_ppm(cfg, opts);
    *bw = cfg->rtl_bw_khz ? cfg->rtl_bw_khz : opts->rtl_dsp_bw_khz;
    *sql = cfg->rtl_sql;
    *vol = cfg->rtl_volume ? cfg->rtl_volume : opts->rtl_volume_multiplier;
}

static void
apply_input_source_pulse(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (cfg->pulse_input[0]) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "pulse:%s", cfg->pulse_input);
    } else {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    }
}

static void
apply_input_source_rtl(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg->rtl_freq[0]) {
        return;
    }
    int gain = 0;
    int ppm = 0;
    int bw = 0;
    int sql = 0;
    int vol = 0;
    resolve_rtl_spec_values(cfg, opts, &gain, &ppm, &bw, &sql, &vol);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtl:%d:%s:%d:%d:%d:%d:%d", cfg->rtl_device,
                 cfg->rtl_freq, gain, ppm, bw, sql, vol);
}

static void
apply_input_source_rtltcp(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg->rtltcp_host[0]) {
        return;
    }
    if (!cfg->rtl_freq[0]) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:%s:%d", cfg->rtltcp_host,
                     cfg->rtltcp_port ? cfg->rtltcp_port : 1234);
        return;
    }

    int gain = 0;
    int ppm = 0;
    int bw = 0;
    int sql = 0;
    int vol = 0;
    resolve_rtl_spec_values(cfg, opts, &gain, &ppm, &bw, &sql, &vol);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:%s:%d:%s:%d:%d:%d:%d:%d", cfg->rtltcp_host,
                 cfg->rtltcp_port ? cfg->rtltcp_port : 1234, cfg->rtl_freq, gain, ppm, bw, sql, vol);
}

static void
apply_input_source_soapy(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (cfg->soapy_args[0]) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "soapy:%s", cfg->soapy_args);
    } else {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
    }
    apply_shared_radio_tuning_from_config(cfg, opts);
    apply_soapy_tuning_from_config(cfg, opts);
}

static void
apply_input_source_file(const dsdneoUserConfig* cfg, dsd_opts* opts, int apply_file_input_rate_now) {
    if (!cfg->file_path[0]) {
        return;
    }
    int configured_rate = (cfg->file_sample_rate > 0) ? cfg->file_sample_rate : 48000;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", cfg->file_path);
    opts->staged_file_sample_rate = configured_rate;
    if (apply_file_input_rate_now) {
        dsd_opts_apply_input_sample_rate(opts, configured_rate);
    }
}

static void
apply_input_source_tcp(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (cfg->tcp_host[0]) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "tcp:%s:%d", cfg->tcp_host,
                     cfg->tcp_port ? cfg->tcp_port : 7355);
    }
}

static void
apply_input_source_udp(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (cfg->udp_addr[0]) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "udp:%s:%d", cfg->udp_addr,
                     cfg->udp_port ? cfg->udp_port : 7355);
    }
}

static void
apply_input_rtl_auto_ppm(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg || !opts) {
        return;
    }
    if (cfg->input_source != DSDCFG_INPUT_RTL && cfg->input_source != DSDCFG_INPUT_RTLTCP) {
        return;
    }
    opts->rtl_auto_ppm = cfg->rtl_auto_ppm ? 1 : 0;
    if (getenv("DSD_NEO_AUTO_PPM") == NULL) {
        dsd_setenv("DSD_NEO_AUTO_PPM", opts->rtl_auto_ppm ? "1" : "0", 0);
    }
}

static void
apply_input_config(const dsdneoUserConfig* cfg, dsd_opts* opts, int apply_file_input_rate_now) {
    if (!cfg || !opts || !cfg->has_input) {
        return;
    }

    opts->staged_file_sample_rate = 0;
    switch (cfg->input_source) {
        case DSDCFG_INPUT_PULSE: apply_input_source_pulse(cfg, opts); break;
        case DSDCFG_INPUT_RTL: apply_input_source_rtl(cfg, opts); break;
        case DSDCFG_INPUT_RTLTCP: apply_input_source_rtltcp(cfg, opts); break;
        case DSDCFG_INPUT_SOAPY: apply_input_source_soapy(cfg, opts); break;
        case DSDCFG_INPUT_FILE: apply_input_source_file(cfg, opts, apply_file_input_rate_now); break;
        case DSDCFG_INPUT_TCP: apply_input_source_tcp(cfg, opts); break;
        case DSDCFG_INPUT_UDP: apply_input_source_udp(cfg, opts); break;
        default: break;
    }
    apply_input_rtl_auto_ppm(cfg, opts);
}

static void
apply_output_config(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg || !opts || !cfg->has_output) {
        return;
    }
    switch (cfg->output_backend) {
        case DSDCFG_OUTPUT_PULSE:
            if (cfg->pulse_output[0]) {
                DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "pulse:%s", cfg->pulse_output);
            } else {
                DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "pulse");
            }
            break;
        case DSDCFG_OUTPUT_NULL: DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null"); break;
        default: break;
    }
    if (cfg->frontend_kind_is_set) {
        opts->frontend_kind = cfg->frontend_kind;
    }
}

static void
apply_mode_config(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state) {
    if (cfg->has_mode) {
        (void)dsd_apply_decode_mode_preset(cfg->decode_mode, DSD_DECODE_PRESET_PROFILE_CONFIG, opts, state);
    }
}

static void
apply_demod_mode(dsd_opts* opts, dsd_state* state, int mod_c4fm, int mod_qpsk, int mod_gfsk, int mod_cli_lock,
                 int rf_mod) {
    if (!opts || !state) {
        return;
    }
    opts->mod_c4fm = mod_c4fm;
    opts->mod_qpsk = mod_qpsk;
    opts->mod_gfsk = mod_gfsk;
    opts->mod_cli_lock = mod_cli_lock;
    state->rf_mod = rf_mod;
}

static void
apply_demod_config(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state) {
    if (!cfg || !opts || !state || !cfg->has_demod) {
        return;
    }
    switch (cfg->demod_path) {
        case DSDCFG_DEMOD_AUTO: apply_demod_mode(opts, state, 1, 1, 1, 0, 0); break;
        case DSDCFG_DEMOD_C4FM: apply_demod_mode(opts, state, 1, 0, 0, 1, 0); break;
        case DSDCFG_DEMOD_GFSK: apply_demod_mode(opts, state, 0, 0, 1, 1, 2); break;
        case DSDCFG_DEMOD_QPSK: apply_demod_mode(opts, state, 0, 1, 0, 1, 1); break;
        default: break;
    }
}

static void
apply_trunking_config(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg || !opts || !cfg->has_trunking) {
        return;
    }
    if (cfg->trunk_enabled) {
        opts->p25_trunk = 1;
        opts->trunk_enable = 1;
    }
    if (cfg->trunk_chan_csv[0]) {
        DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", cfg->trunk_chan_csv);
        opts->chan_in_file[sizeof opts->chan_in_file - 1] = '\0';
    }
    if (cfg->trunk_group_csv[0]) {
        DSD_SNPRINTF(opts->group_in_file, sizeof opts->group_in_file, "%s", cfg->trunk_group_csv);
        opts->group_in_file[sizeof opts->group_in_file - 1] = '\0';
    }
    opts->trunk_use_allow_list = cfg->trunk_use_allow_list ? 1 : 0;
    opts->trunk_tune_group_calls = cfg->trunk_tune_group_calls ? 1 : 0;
    opts->trunk_tune_private_calls = cfg->trunk_tune_private_calls ? 1 : 0;
    opts->trunk_tune_data_calls = cfg->trunk_tune_data_calls ? 1 : 0;
    opts->trunk_tune_enc_calls = cfg->trunk_tune_enc_calls ? 1 : 0;
}

static void
apply_trunk_scan_config(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg || !opts || !cfg->has_trunk_scan) {
        return;
    }
    opts->trunk_scan_enabled = cfg->trunk_scan_enabled ? 1 : 0;
    if (cfg->trunk_scan_targets_csv[0]) {
        DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s",
                     cfg->trunk_scan_targets_csv);
        opts->trunk_scan_targets_csv[sizeof opts->trunk_scan_targets_csv - 1] = '\0';
    }
    opts->trunk_scan_idle_dwell_ms = cfg->trunk_scan_idle_dwell_ms;
    opts->trunk_scan_activity_hold_ms = cfg->trunk_scan_activity_hold_ms;
}

static void
apply_logging_config(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg || !opts || !cfg->has_logging) {
        return;
    }

    DSD_SNPRINTF(opts->event_out_file, sizeof opts->event_out_file, "%s", cfg->event_log);
    opts->event_out_file[sizeof opts->event_out_file - 1] = '\0';

    char frame_log_file_next[sizeof opts->frame_log_file];
    DSD_SNPRINTF(frame_log_file_next, sizeof frame_log_file_next, "%s", cfg->frame_log);
    frame_log_file_next[sizeof frame_log_file_next - 1] = '\0';
    if (strcmp(opts->frame_log_file, frame_log_file_next) != 0) {
        close_frame_log_handle(opts);
        opts->frame_log_open_error_reported = 0;
        opts->frame_log_write_error_reported = 0;
    }
    DSD_SNPRINTF(opts->frame_log_file, sizeof opts->frame_log_file, "%s", frame_log_file_next);
    opts->frame_log_file[sizeof opts->frame_log_file - 1] = '\0';

    char p25_sm_log_file_next[sizeof opts->p25_sm_log_file];
    DSD_SNPRINTF(p25_sm_log_file_next, sizeof p25_sm_log_file_next, "%s", cfg->p25_sm_log);
    p25_sm_log_file_next[sizeof p25_sm_log_file_next - 1] = '\0';
    if (strcmp(opts->p25_sm_log_file, p25_sm_log_file_next) != 0) {
        close_p25_sm_log_handle(opts);
        opts->p25_sm_log_open_error_reported = 0;
        opts->p25_sm_log_write_error_reported = 0;
    }
    DSD_SNPRINTF(opts->p25_sm_log_file, sizeof opts->p25_sm_log_file, "%s", p25_sm_log_file_next);
    opts->p25_sm_log_file[sizeof opts->p25_sm_log_file - 1] = '\0';
}

static void
apply_alerts_config(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg || !opts || !cfg->has_alerts) {
        return;
    }
    uint8_t events = dsd_call_alert_mask_events((uint8_t)cfg->call_alert_events);
    opts->call_alert = (cfg->call_alert_enabled && events != 0) ? 1 : 0;
    opts->call_alert_events = events;
}

static void
apply_recording_config(const dsdneoUserConfig* cfg, dsd_opts* opts) {
    if (!cfg || !opts || !cfg->has_recording) {
        return;
    }

    if (cfg->per_call_wav_dir[0]) {
        DSD_SNPRINTF(opts->wav_out_dir, sizeof opts->wav_out_dir, "%s", cfg->per_call_wav_dir);
        opts->wav_out_dir[sizeof opts->wav_out_dir - 1] = '\0';
    }

    if (cfg->per_call_wav) {
        opts->dmr_stereo_wav = 1;
        opts->static_wav_file = 0;
    } else if (cfg->static_wav_path[0]) {
        opts->dmr_stereo_wav = 0;
        opts->static_wav_file = 1;
        DSD_SNPRINTF(opts->wav_out_file, sizeof opts->wav_out_file, "%s", cfg->static_wav_path);
        opts->wav_out_file[sizeof opts->wav_out_file - 1] = '\0';
    } else {
        opts->dmr_stereo_wav = 0;
        opts->static_wav_file = 0;
    }

    if (cfg->raw_wav_path[0]) {
        DSD_SNPRINTF(opts->wav_out_file_raw, sizeof opts->wav_out_file_raw, "%s", cfg->raw_wav_path);
        opts->wav_out_file_raw[sizeof opts->wav_out_file_raw - 1] = '\0';
    } else {
        opts->wav_out_file_raw[0] = '\0';
    }

    opts->rdio_mode = cfg->rdio_mode;
    opts->rdio_system_id = cfg->rdio_system_id;
    opts->rdio_upload_timeout_ms = cfg->rdio_upload_timeout_ms;
    opts->rdio_upload_retries = cfg->rdio_upload_retries;
    opts->rdio_api_delete_after_upload = cfg->rdio_api_delete_after_upload;

    if (cfg->rdio_api_url[0]) {
        DSD_SNPRINTF(opts->rdio_api_url, sizeof opts->rdio_api_url, "%s", cfg->rdio_api_url);
        opts->rdio_api_url[sizeof opts->rdio_api_url - 1] = '\0';
    }
    if (cfg->rdio_api_key[0]) {
        DSD_SNPRINTF(opts->rdio_api_key, sizeof opts->rdio_api_key, "%s", cfg->rdio_api_key);
        opts->rdio_api_key[sizeof opts->rdio_api_key - 1] = '\0';
    }
}

static void
apply_dsp_config(const dsdneoUserConfig* cfg) {
    if (!cfg || !cfg->has_dsp) {
        return;
    }
    if (getenv("DSD_NEO_IQ_BALANCE") == NULL) {
        dsd_setenv("DSD_NEO_IQ_BALANCE", cfg->iq_balance ? "1" : "0", 0);
    }
    if (getenv("DSD_NEO_IQ_DC_BLOCK") == NULL) {
        dsd_setenv("DSD_NEO_IQ_DC_BLOCK", cfg->iq_dc_block ? "1" : "0", 0);
    }
}

static void
apply_file_input_symbol_timing(const dsdneoUserConfig* cfg, const dsd_opts* opts, dsd_state* state,
                               int old_effective_input_rate, int apply_file_input_rate_now) {
    if (!cfg || !opts || !state || !apply_file_input_rate_now || !cfg->has_input
        || cfg->input_source != DSDCFG_INPUT_FILE || cfg->file_path[0] == '\0') {
        return;
    }

    int effective_input_rate = dsd_opts_effective_input_rate(opts);
    if (cfg->has_mode) {
        dsd_apply_decode_mode_symbol_timing(cfg->decode_mode, effective_input_rate, state);
    } else {
        dsd_state_rescale_symbol_timing(state, old_effective_input_rate, effective_input_rate);
    }
}

static void
dsd_apply_user_config_to_opts_impl(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state,
                                   int apply_file_input_rate_now) {
    if (!cfg || !opts || !state) {
        return;
    }

    const int old_effective_input_rate = dsd_opts_effective_input_rate(opts);
    apply_input_config(cfg, opts, apply_file_input_rate_now);
    apply_output_config(cfg, opts);
    apply_mode_config(cfg, opts, state);
    apply_demod_config(cfg, opts, state);
    apply_trunking_config(cfg, opts);
    apply_trunk_scan_config(cfg, opts);
    apply_logging_config(cfg, opts);
    apply_alerts_config(cfg, opts);
    apply_recording_config(cfg, opts);
    apply_dsp_config(cfg);
    apply_file_input_symbol_timing(cfg, opts, state, old_effective_input_rate, apply_file_input_rate_now);
}

void
dsd_apply_user_config_to_opts(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state) {
    dsd_apply_user_config_to_opts_impl(cfg, opts, state, 1);
}

void
dsd_apply_user_config_to_opts_pre_cli(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state) {
    dsd_apply_user_config_to_opts_impl(cfg, opts, state, 0);
}

void
dsd_finalize_user_config_file_input_after_cli(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state) {
    if (!cfg || !opts || !state) {
        return;
    }
    if (!cfg->has_input || cfg->input_source != DSDCFG_INPUT_FILE || cfg->file_path[0] == '\0') {
        return;
    }
    if (strcmp(opts->audio_in_dev, cfg->file_path) != 0) {
        return;
    }

    int configured_rate = (cfg->file_sample_rate > 0) ? cfg->file_sample_rate : 48000;
    opts->staged_file_sample_rate = configured_rate;
    int old_effective_input_rate = dsd_opts_effective_input_rate(opts);
    if (opts->wav_sample_rate != configured_rate) {
        dsd_opts_apply_input_sample_rate(opts, configured_rate);
    }

    int effective_input_rate = dsd_opts_effective_input_rate(opts);
    if (effective_input_rate != old_effective_input_rate) {
        dsd_state_rescale_symbol_timing(state, old_effective_input_rate, effective_input_rate);
    }
}

static void
snapshot_input_config(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    cfg->has_input = 1;
    if (strncmp(opts->audio_in_dev, "rtl:", 4) == 0) {
        cfg->input_source = DSDCFG_INPUT_RTL;
        snapshot_parse_rtl_device_spec(opts->audio_in_dev, cfg);
        snapshot_apply_live_rtl_values(opts, cfg);
    } else if (strncmp(opts->audio_in_dev, "rtltcp:", 7) == 0) {
        cfg->input_source = DSDCFG_INPUT_RTLTCP;
        snapshot_parse_rtltcp_device_spec(opts->audio_in_dev, cfg);
        snapshot_apply_live_rtl_values(opts, cfg);
    } else if ((strcmp(opts->audio_in_dev, "soapy") == 0) || (strncmp(opts->audio_in_dev, "soapy:", 6) == 0)) {
        cfg->input_source = DSDCFG_INPUT_SOAPY;
        snapshot_parse_soapy_device_spec(opts->audio_in_dev, cfg);
        snapshot_apply_live_rtl_values(opts, cfg);
        snapshot_apply_live_soapy_values(opts, cfg);
    } else if (strncmp(opts->audio_in_dev, "tcp:", 4) == 0) {
        cfg->input_source = DSDCFG_INPUT_TCP;
        snapshot_parse_host_port_spec(opts->audio_in_dev, cfg->tcp_host, sizeof cfg->tcp_host, &cfg->tcp_port);
    } else if (strncmp(opts->audio_in_dev, "udp:", 4) == 0) {
        cfg->input_source = DSDCFG_INPUT_UDP;
        snapshot_parse_host_port_spec(opts->audio_in_dev, cfg->udp_addr, sizeof cfg->udp_addr, &cfg->udp_port);
    } else if (strncmp(opts->audio_in_dev, "pulse", 5) == 0) {
        cfg->input_source = DSDCFG_INPUT_PULSE;
        const char* dev = NULL;
        if (opts->audio_in_dev[5] == ':') {
            dev = opts->audio_in_dev + 6;
        }
        if (dev && *dev) {
            DSD_SNPRINTF(cfg->pulse_input, sizeof cfg->pulse_input, "%s", dev);
            cfg->pulse_input[sizeof cfg->pulse_input - 1] = '\0';
        }
    } else {
        cfg->input_source = DSDCFG_INPUT_FILE;
        DSD_SNPRINTF(cfg->file_path, sizeof cfg->file_path, "%.*s", (int)(sizeof cfg->file_path - 1),
                     opts->audio_in_dev);
        cfg->file_path[sizeof cfg->file_path - 1] = '\0';
        cfg->file_sample_rate =
            (opts->audio_in_type == AUDIO_IN_WAV) ? opts->wav_sample_rate : dsd_opts_requested_file_sample_rate(opts);
    }

    if (cfg->input_source == DSDCFG_INPUT_RTL || cfg->input_source == DSDCFG_INPUT_RTLTCP) {
        cfg->rtl_auto_ppm = opts->rtl_auto_ppm ? 1 : 0;
    }
}

static void
snapshot_output_config(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    cfg->has_output = 1;
    if (strncmp(opts->audio_out_dev, "pulse", 5) == 0) {
        cfg->output_backend = DSDCFG_OUTPUT_PULSE;
        const char* dev = NULL;
        if (opts->audio_out_dev[5] == ':') {
            dev = opts->audio_out_dev + 6;
        }
        if (dev && *dev) {
            DSD_SNPRINTF(cfg->pulse_output, sizeof cfg->pulse_output, "%s", dev);
            cfg->pulse_output[sizeof cfg->pulse_output - 1] = '\0';
        }
    } else if (strcmp(opts->audio_out_dev, "null") == 0) {
        cfg->output_backend = DSDCFG_OUTPUT_NULL;
    } else {
        cfg->output_backend = DSDCFG_OUTPUT_UNSET;
    }
    cfg->frontend_kind = opts->frontend_kind;
    cfg->frontend_kind_is_set = 1;
}

static void
snapshot_mode_config(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    cfg->has_mode = 1;
    cfg->decode_mode = dsd_infer_decode_mode_preset(opts);
}

static void
snapshot_demod_config(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    if (!opts->mod_cli_lock) {
        return;
    }
    cfg->has_demod = 1;
    if (opts->mod_gfsk) {
        cfg->demod_path = DSDCFG_DEMOD_GFSK;
    } else if (opts->mod_qpsk) {
        cfg->demod_path = DSDCFG_DEMOD_QPSK;
    } else if (opts->mod_c4fm) {
        cfg->demod_path = DSDCFG_DEMOD_C4FM;
    } else {
        cfg->demod_path = DSDCFG_DEMOD_AUTO;
    }
}

static void
snapshot_trunking_config(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    cfg->has_trunking = 1;
    cfg->trunk_enabled = (opts->p25_trunk || opts->trunk_enable) ? 1 : 0;
    DSD_SNPRINTF(cfg->trunk_chan_csv, sizeof cfg->trunk_chan_csv, "%s", opts->chan_in_file);
    cfg->trunk_chan_csv[sizeof cfg->trunk_chan_csv - 1] = '\0';
    DSD_SNPRINTF(cfg->trunk_group_csv, sizeof cfg->trunk_group_csv, "%s", opts->group_in_file);
    cfg->trunk_group_csv[sizeof cfg->trunk_group_csv - 1] = '\0';
    cfg->trunk_use_allow_list = opts->trunk_use_allow_list ? 1 : 0;
    cfg->trunk_tune_group_calls = opts->trunk_tune_group_calls ? 1 : 0;
    cfg->trunk_tune_private_calls = opts->trunk_tune_private_calls ? 1 : 0;
    cfg->trunk_tune_data_calls = opts->trunk_tune_data_calls ? 1 : 0;
    cfg->trunk_tune_enc_calls = opts->trunk_tune_enc_calls ? 1 : 0;
}

static void
snapshot_trunk_scan_config(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    cfg->has_trunk_scan = 1;
    cfg->trunk_scan_enabled = opts->trunk_scan_enabled ? 1 : 0;
    DSD_SNPRINTF(cfg->trunk_scan_targets_csv, sizeof cfg->trunk_scan_targets_csv, "%s", opts->trunk_scan_targets_csv);
    cfg->trunk_scan_targets_csv[sizeof cfg->trunk_scan_targets_csv - 1] = '\0';
    cfg->trunk_scan_idle_dwell_ms = opts->trunk_scan_idle_dwell_ms;
    cfg->trunk_scan_activity_hold_ms = opts->trunk_scan_activity_hold_ms;
}

static void
snapshot_logging_config(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    cfg->has_logging = 1;
    DSD_SNPRINTF(cfg->event_log, sizeof cfg->event_log, "%s", opts->event_out_file);
    cfg->event_log[sizeof cfg->event_log - 1] = '\0';
    DSD_SNPRINTF(cfg->frame_log, sizeof cfg->frame_log, "%s", opts->frame_log_file);
    cfg->frame_log[sizeof cfg->frame_log - 1] = '\0';
    DSD_SNPRINTF(cfg->p25_sm_log, sizeof cfg->p25_sm_log, "%s", opts->p25_sm_log_file);
    cfg->p25_sm_log[sizeof cfg->p25_sm_log - 1] = '\0';
}

static void
snapshot_alerts_config(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    cfg->has_alerts = 1;
    cfg->call_alert_enabled = opts->call_alert ? 1 : 0;
    cfg->call_alert_events = opts->call_alert ? dsd_call_alert_normalize_events(opts->call_alert_events)
                                              : dsd_call_alert_mask_events(opts->call_alert_events);
}

static void
snapshot_recording_config(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    cfg->has_recording = 1;
    cfg->per_call_wav = opts->dmr_stereo_wav ? 1 : 0;
    DSD_SNPRINTF(cfg->per_call_wav_dir, sizeof cfg->per_call_wav_dir, "%s", opts->wav_out_dir);
    cfg->per_call_wav_dir[sizeof cfg->per_call_wav_dir - 1] = '\0';
    if (opts->static_wav_file && opts->wav_out_file[0] != '\0') {
        DSD_SNPRINTF(cfg->static_wav_path, sizeof cfg->static_wav_path, "%s", opts->wav_out_file);
        cfg->static_wav_path[sizeof cfg->static_wav_path - 1] = '\0';
    } else {
        cfg->static_wav_path[0] = '\0';
    }
    DSD_SNPRINTF(cfg->raw_wav_path, sizeof cfg->raw_wav_path, "%s", opts->wav_out_file_raw);
    cfg->raw_wav_path[sizeof cfg->raw_wav_path - 1] = '\0';
    cfg->rdio_mode = opts->rdio_mode;
    cfg->rdio_system_id = opts->rdio_system_id;
    DSD_SNPRINTF(cfg->rdio_api_url, sizeof cfg->rdio_api_url, "%s", opts->rdio_api_url);
    cfg->rdio_api_url[sizeof cfg->rdio_api_url - 1] = '\0';
    DSD_SNPRINTF(cfg->rdio_api_key, sizeof cfg->rdio_api_key, "%s", opts->rdio_api_key);
    cfg->rdio_api_key[sizeof cfg->rdio_api_key - 1] = '\0';
    cfg->rdio_upload_timeout_ms = opts->rdio_upload_timeout_ms;
    cfg->rdio_upload_retries = opts->rdio_upload_retries;
    cfg->rdio_api_delete_after_upload = opts->rdio_api_delete_after_upload ? 1 : 0;
}

static void
snapshot_dsp_config(dsdneoUserConfig* cfg) {
    cfg->has_dsp = 1;
    const char* iqb = getenv("DSD_NEO_IQ_BALANCE");
    cfg->iq_balance = (parse_int_atoi_compat(iqb) != 0) ? 1 : 0;
    const char* dcb = getenv("DSD_NEO_IQ_DC_BLOCK");
    cfg->iq_dc_block = (parse_int_atoi_compat(dcb) != 0) ? 1 : 0;
}

void
dsd_snapshot_opts_to_user_config(const dsd_opts* opts, const dsd_state* state, dsdneoUserConfig* cfg) {
    if (!opts || !state || !cfg) {
        return;
    }

    user_cfg_reset(cfg);
    snapshot_input_config(opts, cfg);
    snapshot_output_config(opts, cfg);
    snapshot_mode_config(opts, cfg);
    snapshot_demod_config(opts, cfg);
    snapshot_trunking_config(opts, cfg);
    snapshot_trunk_scan_config(opts, cfg);
    snapshot_logging_config(opts, cfg);
    snapshot_alerts_config(opts, cfg);
    snapshot_recording_config(opts, cfg);
    snapshot_dsp_config(cfg);
}

// Template generation ---------------------------------------------------------

static void
render_template_intro(FILE* stream) {
    DSD_FPRINTF(stream, "# DSD-neo configuration template\n");
    DSD_FPRINTF(stream, "# Generated by: dsd-neo --dump-config-template\n");
    DSD_FPRINTF(stream, "#\n");
    DSD_FPRINTF(stream, "# Uncomment and modify values as needed.\n");
    DSD_FPRINTF(stream, "# Lines starting with # are comments.\n");
    DSD_FPRINTF(stream, "#\n");
    DSD_FPRINTF(stream, "# Precedence: CLI arguments > environment variables > config file > defaults\n");
    DSD_FPRINTF(stream, "\n");
    DSD_FPRINTF(stream, "version = 1\n\n");
}

static void
render_template_type_hint(FILE* stream, const dsdcfg_schema_entry_t* e) {
    if (!stream || !e) {
        return;
    }
    switch (e->type) {
        case DSDCFG_TYPE_ENUM:
            if (e->allowed) {
                DSD_FPRINTF(stream, "# Allowed: %s\n", e->allowed);
            }
            break;
        case DSDCFG_TYPE_INT:
            if (e->max_val > 0) {
                DSD_FPRINTF(stream, "# Range: %d to %d\n", e->min_val, e->max_val);
            } else if (e->min_val != 0) {
                DSD_FPRINTF(stream, "# Minimum: %d\n", e->min_val);
            }
            break;
        case DSDCFG_TYPE_BOOL: DSD_FPRINTF(stream, "# Values: true, false\n"); break;
        case DSDCFG_TYPE_PATH: DSD_FPRINTF(stream, "# Path (supports ~ and $VAR expansion)\n"); break;
        case DSDCFG_TYPE_FREQ: DSD_FPRINTF(stream, "# Frequency (supports K/M/G suffix)\n"); break;
        default: break;
    }
}

static void
render_template_default_value(FILE* stream, const dsdcfg_schema_entry_t* e) {
    if (!stream || !e) {
        return;
    }
    if (e->default_str && e->default_str[0]) {
        if (e->type == DSDCFG_TYPE_STRING || e->type == DSDCFG_TYPE_ENUM || e->type == DSDCFG_TYPE_PATH
            || e->type == DSDCFG_TYPE_FREQ) {
            DSD_FPRINTF(stream, "# %s = \"%s\"\n", e->key, e->default_str);
        } else {
            DSD_FPRINTF(stream, "# %s = %s\n", e->key, e->default_str);
        }
        return;
    }
    DSD_FPRINTF(stream, "# %s = \n", e->key);
}

static void
render_template_schema_entry(FILE* stream, const dsdcfg_schema_entry_t* e) {
    if (!stream || !e || e->deprecated) {
        return;
    }
    DSD_FPRINTF(stream, "# %s\n", e->description);
    render_template_type_hint(stream, e);
    render_template_default_value(stream, e);
    DSD_FPRINTF(stream, "\n");
}

static void
render_template_section(FILE* stream, const char* section) {
    if (!stream || !section) {
        return;
    }
    DSD_FPRINTF(stream, "[%s]\n", section);
    int schema_count = dsdcfg_schema_count();
    for (int i = 0; i < schema_count; i++) {
        const dsdcfg_schema_entry_t* e = dsdcfg_schema_get(i);
        if (!e || dsd_strcasecmp(e->section, section) != 0) {
            continue;
        }
        render_template_schema_entry(stream, e);
    }
}

static void
render_template_profile_example(FILE* stream) {
    DSD_FPRINTF(stream, "# --- Profiles ---\n");
    DSD_FPRINTF(stream, "# Define named profiles to quickly switch between configurations.\n");
    DSD_FPRINTF(stream, "# Use: dsd-neo --config config.ini --profile <name>\n");
    DSD_FPRINTF(stream, "#\n");
    DSD_FPRINTF(stream, "# [profile.example]\n");
    DSD_FPRINTF(stream, "# mode.decode = \"p25p1\"\n");
    DSD_FPRINTF(stream, "# trunking.enabled = true\n");
    DSD_FPRINTF(stream, "# trunk_scan.enabled = true\n");
    DSD_FPRINTF(stream, "# input.source = \"rtl\"\n");
    DSD_FPRINTF(stream, "# input.rtl_freq = \"851.375M\"\n");
}

void
dsd_user_config_render_template(FILE* stream) {
    if (!stream) {
        return;
    }

    render_template_intro(stream);

    const char* sections[16];
    int num_sections = dsdcfg_schema_sections(sections, 16);
    for (int s = 0; s < num_sections; s++) {
        render_template_section(stream, sections[s]);
    }

    render_template_profile_example(stream);
}
