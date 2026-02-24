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
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/config_schema.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_user_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

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
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = 1;
    // Set trunking tune defaults to match main.c init defaults
    cfg->trunk_tune_group_calls = 1;
    cfg->trunk_tune_private_calls = 1;
    cfg->trunk_tune_data_calls = 0;
    cfg->trunk_tune_enc_calls = 1;

    cfg->rtl_auto_ppm = 0;

    // Recording defaults (match initOpts)
    cfg->per_call_wav = 0;
    snprintf(cfg->per_call_wav_dir, sizeof cfg->per_call_wav_dir, "%s", "./WAV");
    cfg->per_call_wav_dir[sizeof cfg->per_call_wav_dir - 1] = '\0';
    cfg->rdio_mode = DSD_RDIO_MODE_OFF;
    cfg->rdio_system_id = 0;
    snprintf(cfg->rdio_api_url, sizeof cfg->rdio_api_url, "%s", "http://127.0.0.1:3000");
    cfg->rdio_api_url[sizeof cfg->rdio_api_url - 1] = '\0';
    cfg->rdio_api_key[0] = '\0';
    cfg->rdio_upload_timeout_ms = 5000;
    cfg->rdio_upload_retries = 1;

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

typedef struct {
    dsdneoUserDecodeMode mode;
    const char* value;
} decode_mode_name_map_t;

typedef struct {
    const char* alias;
    dsdneoUserDecodeMode mode;
} decode_mode_alias_map_t;

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
    snprintf(dst, dst_size, "%s", src);
    dst[dst_size - 1] = '\0';
}

static size_t
split_colon_tokens(char* scratch, size_t scratch_size, const char* in, char** out_tokens, size_t max_tokens) {
    if (!scratch || scratch_size == 0 || !in || !out_tokens || max_tokens == 0) {
        return 0;
    }

    snprintf(scratch, scratch_size, "%.*s", (int)(scratch_size - 1), in);
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
is_valid_rtl_bw_khz(int bw) {
    return (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16 || bw == 24 || bw == 48);
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
    int ppm = cfg->rtl_ppm;
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
snapshot_apply_live_rtl_values(const dsd_opts* opts, dsdneoUserConfig* cfg) {
    if (!opts || !cfg) {
        return;
    }

    cfg->rtl_gain = opts->rtl_gain_value;
    cfg->rtl_ppm = opts->rtlsdr_ppm_error;
    cfg->rtl_bw_khz = opts->rtl_dsp_bw_khz;
    cfg->rtl_sql = (int)local_pwr_to_dB(opts->rtl_squelch_level);
    cfg->rtl_volume = opts->rtl_volume_multiplier;
    if (opts->rtlsdr_center_freq > 0) {
        snprintf(cfg->rtl_freq, sizeof cfg->rtl_freq, "%u", opts->rtlsdr_center_freq);
        cfg->rtl_freq[sizeof cfg->rtl_freq - 1] = '\0';
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
        cfg->rtl_device = atoi(tok[1]);
    }
    if (n > 2) {
        copy_token_string(cfg->rtl_freq, sizeof cfg->rtl_freq, tok[2]);
    }
    if (n > 3) {
        cfg->rtl_gain = atoi(tok[3]);
    }
    if (n > 4) {
        cfg->rtl_ppm = atoi(tok[4]);
    }
    if (n > 5) {
        cfg->rtl_bw_khz = atoi(tok[5]);
    }
    if (n > 6) {
        cfg->rtl_sql = atoi(tok[6]);
    }
    if (n > 7) {
        cfg->rtl_volume = atoi(tok[7]);
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
        cfg->rtltcp_port = atoi(tok[2]);
    }
    if (n > 3) {
        copy_token_string(cfg->rtl_freq, sizeof cfg->rtl_freq, tok[3]);
    }
    if (n > 4) {
        cfg->rtl_gain = atoi(tok[4]);
    }
    if (n > 5) {
        cfg->rtl_ppm = atoi(tok[5]);
    }
    if (n > 6) {
        cfg->rtl_bw_khz = atoi(tok[6]);
    }
    if (n > 7) {
        cfg->rtl_sql = atoi(tok[7]);
    }
    if (n > 8) {
        cfg->rtl_volume = atoi(tok[8]);
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
        *port = atoi(tok[2]);
    }
}

static void
ensure_dir_exists(const char* path) {
    if (!path || !*path) {
        return;
    }

    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
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
        snprintf(buf, sizeof buf, "%s\\dsd-neo\\config.ini", appdata);
        buf[sizeof buf - 1] = '\0';
    }
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(buf, sizeof buf, "%s/dsd-neo/config.ini", xdg);
        buf[sizeof buf - 1] = '\0';
    } else {
        const char* home = getenv("HOME");
        if (home && *home) {
            snprintf(buf, sizeof buf, "%s/.config/dsd-neo/config.ini", home);
            buf[sizeof buf - 1] = '\0';
        }
    }
#endif

    inited = 1;
    return buf[0] ? buf : NULL;
}

// INI writer ------------------------------------------------------------------

int
dsd_user_config_save_atomic(const char* path, const dsdneoUserConfig* cfg) {
    if (!path || !*path || !cfg) {
        return -1;
    }

    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    dir[sizeof dir - 1] = '\0';
    char* last_sep = strrchr(dir, '/');
#if defined(_WIN32)
    char* last_sep2 = strrchr(dir, '\\');
    if (!last_sep || (last_sep2 && last_sep2 > last_sep)) {
        last_sep = last_sep2;
    }
#endif
    if (last_sep) {
        *last_sep = '\0';
        ensure_dir_exists(dir);
    }

    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    tmp[sizeof tmp - 1] = '\0';

    FILE* fp = fopen(tmp, "w");
    if (!fp) {
        return -1;
    }

    dsd_user_config_render_ini(cfg, fp);

    if (fflush(fp) != 0) {
        fclose(fp);
        return -1;
    }

    int fd = dsd_fileno(fp);
    if (fd >= 0) {
        (void)dsd_fchmod(fd, 0600);
        (void)dsd_fsync(fd);
    }

    fclose(fp);

#if defined(_WIN32)
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        (void)remove(tmp);
        return -1;
    }
#else
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        return -1;
    }
#endif

    return 0;
}

void
dsd_user_config_render_ini(const dsdneoUserConfig* cfg, FILE* out) {
    if (!cfg || !out) {
        return;
    }

    fprintf(out, "version = %d\n\n", cfg->version > 0 ? cfg->version : 1);

    if (cfg->has_input) {
        fprintf(out, "[input]\n");
        switch (cfg->input_source) {
            case DSDCFG_INPUT_PULSE: fprintf(out, "source = \"pulse\"\n"); break;
            case DSDCFG_INPUT_RTL: fprintf(out, "source = \"rtl\"\n"); break;
            case DSDCFG_INPUT_RTLTCP: fprintf(out, "source = \"rtltcp\"\n"); break;
            case DSDCFG_INPUT_SOAPY: fprintf(out, "source = \"soapy\"\n"); break;
            case DSDCFG_INPUT_FILE: fprintf(out, "source = \"file\"\n"); break;
            case DSDCFG_INPUT_TCP: fprintf(out, "source = \"tcp\"\n"); break;
            case DSDCFG_INPUT_UDP: fprintf(out, "source = \"udp\"\n"); break;
            default: break;
        }
        if (cfg->input_source == DSDCFG_INPUT_PULSE) {
            if (cfg->pulse_input[0]) {
                fprintf(out, "pulse_source = \"%s\"\n", cfg->pulse_input);
            }
        } else if (cfg->input_source == DSDCFG_INPUT_RTL) {
            fprintf(out, "rtl_device = %d\n", cfg->rtl_device);
            if (cfg->rtl_freq[0]) {
                fprintf(out, "rtl_freq = \"%s\"\n", cfg->rtl_freq);
            }
            if (cfg->rtl_gain) {
                fprintf(out, "rtl_gain = %d\n", cfg->rtl_gain);
            }
            if (cfg->rtl_ppm) {
                fprintf(out, "rtl_ppm = %d\n", cfg->rtl_ppm);
            }
            if (cfg->rtl_bw_khz) {
                fprintf(out, "rtl_bw_khz = %d\n", cfg->rtl_bw_khz);
            }
            fprintf(out, "rtl_sql = %d\n", cfg->rtl_sql);
            if (cfg->rtl_volume) {
                fprintf(out, "rtl_volume = %d\n", cfg->rtl_volume);
            }
            fprintf(out, "auto_ppm = %s\n", cfg->rtl_auto_ppm ? "true" : "false");
        } else if (cfg->input_source == DSDCFG_INPUT_RTLTCP) {
            if (cfg->rtltcp_host[0]) {
                fprintf(out, "rtltcp_host = \"%s\"\n", cfg->rtltcp_host);
            }
            if (cfg->rtltcp_port) {
                fprintf(out, "rtltcp_port = %d\n", cfg->rtltcp_port);
            }
            if (cfg->rtl_freq[0]) {
                fprintf(out, "rtl_freq = \"%s\"\n", cfg->rtl_freq);
            }
            if (cfg->rtl_gain) {
                fprintf(out, "rtl_gain = %d\n", cfg->rtl_gain);
            }
            if (cfg->rtl_ppm) {
                fprintf(out, "rtl_ppm = %d\n", cfg->rtl_ppm);
            }
            if (cfg->rtl_bw_khz) {
                fprintf(out, "rtl_bw_khz = %d\n", cfg->rtl_bw_khz);
            }
            fprintf(out, "rtl_sql = %d\n", cfg->rtl_sql);
            if (cfg->rtl_volume) {
                fprintf(out, "rtl_volume = %d\n", cfg->rtl_volume);
            }
            fprintf(out, "auto_ppm = %s\n", cfg->rtl_auto_ppm ? "true" : "false");
        } else if (cfg->input_source == DSDCFG_INPUT_SOAPY) {
            if (cfg->soapy_args[0]) {
                fprintf(out, "soapy_args = \"%s\"\n", cfg->soapy_args);
            }
            if (cfg->rtl_freq[0]) {
                fprintf(out, "rtl_freq = \"%s\"\n", cfg->rtl_freq);
            }
            if (cfg->rtl_gain) {
                fprintf(out, "rtl_gain = %d\n", cfg->rtl_gain);
            }
            if (cfg->rtl_ppm) {
                fprintf(out, "rtl_ppm = %d\n", cfg->rtl_ppm);
            }
            if (cfg->rtl_bw_khz) {
                fprintf(out, "rtl_bw_khz = %d\n", cfg->rtl_bw_khz);
            }
            fprintf(out, "rtl_sql = %d\n", cfg->rtl_sql);
            if (cfg->rtl_volume) {
                fprintf(out, "rtl_volume = %d\n", cfg->rtl_volume);
            }
        } else if (cfg->input_source == DSDCFG_INPUT_FILE) {
            if (cfg->file_path[0]) {
                fprintf(out, "file_path = \"%s\"\n", cfg->file_path);
            }
            if (cfg->file_sample_rate) {
                fprintf(out, "file_sample_rate = %d\n", cfg->file_sample_rate);
            }
        } else if (cfg->input_source == DSDCFG_INPUT_TCP) {
            if (cfg->tcp_host[0]) {
                fprintf(out, "tcp_host = \"%s\"\n", cfg->tcp_host);
            }
            if (cfg->tcp_port) {
                fprintf(out, "tcp_port = %d\n", cfg->tcp_port);
            }
        } else if (cfg->input_source == DSDCFG_INPUT_UDP) {
            if (cfg->udp_addr[0]) {
                fprintf(out, "udp_addr = \"%s\"\n", cfg->udp_addr);
            }
            if (cfg->udp_port) {
                fprintf(out, "udp_port = %d\n", cfg->udp_port);
            }
        }
        fprintf(out, "\n");
    }

    if (cfg->has_output) {
        fprintf(out, "[output]\n");
        switch (cfg->output_backend) {
            case DSDCFG_OUTPUT_PULSE: fprintf(out, "backend = \"pulse\"\n"); break;
            case DSDCFG_OUTPUT_NULL: fprintf(out, "backend = \"null\"\n"); break;
            default: break;
        }
        if (cfg->pulse_output[0]) {
            fprintf(out, "pulse_sink = \"%s\"\n", cfg->pulse_output);
        }
        fprintf(out, "ncurses_ui = %s\n", cfg->ncurses_ui ? "true" : "false");
        fprintf(out, "\n");
    }

    if (cfg->has_mode) {
        fprintf(out, "[mode]\n");
        const char* decode_name = decode_mode_to_ini_name(cfg->decode_mode);
        if (decode_name) {
            fprintf(out, "decode = \"%s\"\n", decode_name);
        }
        if (cfg->has_demod) {
            switch (cfg->demod_path) {
                case DSDCFG_DEMOD_AUTO: fprintf(out, "demod = \"auto\"\n"); break;
                case DSDCFG_DEMOD_C4FM: fprintf(out, "demod = \"c4fm\"\n"); break;
                case DSDCFG_DEMOD_GFSK: fprintf(out, "demod = \"gfsk\"\n"); break;
                case DSDCFG_DEMOD_QPSK: fprintf(out, "demod = \"qpsk\"\n"); break;
                default: break;
            }
        }
        fprintf(out, "\n");
    }

    if (cfg->has_trunking) {
        fprintf(out, "[trunking]\n");
        fprintf(out, "enabled = %s\n", cfg->trunk_enabled ? "true" : "false");
        if (cfg->trunk_chan_csv[0]) {
            fprintf(out, "chan_csv = \"%s\"\n", cfg->trunk_chan_csv);
        }
        if (cfg->trunk_group_csv[0]) {
            fprintf(out, "group_csv = \"%s\"\n", cfg->trunk_group_csv);
        }
        fprintf(out, "allow_list = %s\n", cfg->trunk_use_allow_list ? "true" : "false");
        fprintf(out, "tune_group_calls = %s\n", cfg->trunk_tune_group_calls ? "true" : "false");
        fprintf(out, "tune_private_calls = %s\n", cfg->trunk_tune_private_calls ? "true" : "false");
        fprintf(out, "tune_data_calls = %s\n", cfg->trunk_tune_data_calls ? "true" : "false");
        fprintf(out, "tune_enc_calls = %s\n", cfg->trunk_tune_enc_calls ? "true" : "false");
        fprintf(out, "\n");
    }

    if (cfg->has_logging) {
        fprintf(out, "[logging]\n");
        if (cfg->event_log[0]) {
            fprintf(out, "event_log = \"%s\"\n", cfg->event_log);
        }
        if (cfg->frame_log[0]) {
            fprintf(out, "frame_log = \"%s\"\n", cfg->frame_log);
        }
        fprintf(out, "\n");
    }

    if (cfg->has_recording) {
        fprintf(out, "[recording]\n");
        fprintf(out, "per_call_wav = %s\n", cfg->per_call_wav ? "true" : "false");
        if (cfg->per_call_wav_dir[0]) {
            fprintf(out, "per_call_wav_dir = \"%s\"\n", cfg->per_call_wav_dir);
        }
        if (cfg->static_wav_path[0]) {
            fprintf(out, "static_wav = \"%s\"\n", cfg->static_wav_path);
        }
        if (cfg->raw_wav_path[0]) {
            fprintf(out, "raw_wav = \"%s\"\n", cfg->raw_wav_path);
        }
        fprintf(out, "rdio_mode = \"%s\"\n", dsd_rdio_mode_to_string(cfg->rdio_mode));
        if (cfg->rdio_system_id > 0) {
            fprintf(out, "rdio_system_id = %d\n", cfg->rdio_system_id);
        }
        if (cfg->rdio_api_url[0]) {
            fprintf(out, "rdio_api_url = \"%s\"\n", cfg->rdio_api_url);
        }
        if (cfg->rdio_api_key[0]) {
            fprintf(out, "rdio_api_key = \"%s\"\n", cfg->rdio_api_key);
        }
        if (cfg->rdio_upload_timeout_ms > 0) {
            fprintf(out, "rdio_upload_timeout_ms = %d\n", cfg->rdio_upload_timeout_ms);
        }
        if (cfg->rdio_upload_retries >= 0) {
            fprintf(out, "rdio_upload_retries = %d\n", cfg->rdio_upload_retries);
        }
        fprintf(out, "\n");
    }

    if (cfg->has_dsp) {
        fprintf(out, "[dsp]\n");
        fprintf(out, "iq_balance = %s\n", cfg->iq_balance ? "true" : "false");
        fprintf(out, "iq_dc_block = %s\n", cfg->iq_dc_block ? "true" : "false");
        fprintf(out, "\n");
    }
}

// Mapping helpers -------------------------------------------------------------

void
dsd_apply_user_config_to_opts(const dsdneoUserConfig* cfg, dsd_opts* opts, dsd_state* state) {
    if (!cfg || !opts || !state) {
        return;
    }

    // Input source
    if (cfg->has_input) {
        switch (cfg->input_source) {
            case DSDCFG_INPUT_PULSE:
                if (cfg->pulse_input[0]) {
                    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "pulse:%s", cfg->pulse_input);
                } else {
                    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
                }
                break;
            case DSDCFG_INPUT_RTL:
                if (cfg->rtl_freq[0]) {
                    /* When gain/volume are omitted in config, preserve whatever
                     * defaults initOpts()/CLI already established (AGC and
                     * multiplier 2) instead of forcing hardcoded values. */
                    int gain = cfg->rtl_gain ? cfg->rtl_gain : opts->rtl_gain_value;
                    int ppm = cfg->rtl_ppm;
                    int bw = cfg->rtl_bw_khz ? cfg->rtl_bw_khz : opts->rtl_dsp_bw_khz;
                    int sql = cfg->rtl_sql;
                    int vol = cfg->rtl_volume ? cfg->rtl_volume : opts->rtl_volume_multiplier;
                    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtl:%d:%s:%d:%d:%d:%d:%d", cfg->rtl_device,
                             cfg->rtl_freq, gain, ppm, bw, sql, vol);
                }
                break;
            case DSDCFG_INPUT_RTLTCP:
                if (cfg->rtltcp_host[0]) {
                    if (cfg->rtl_freq[0]) {
                        int gain = cfg->rtl_gain ? cfg->rtl_gain : opts->rtl_gain_value;
                        int ppm = cfg->rtl_ppm;
                        int bw = cfg->rtl_bw_khz ? cfg->rtl_bw_khz : opts->rtl_dsp_bw_khz;
                        int sql = cfg->rtl_sql;
                        int vol = cfg->rtl_volume ? cfg->rtl_volume : opts->rtl_volume_multiplier;
                        snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:%s:%d:%s:%d:%d:%d:%d:%d",
                                 cfg->rtltcp_host, cfg->rtltcp_port ? cfg->rtltcp_port : 1234, cfg->rtl_freq, gain, ppm,
                                 bw, sql, vol);
                    } else {
                        snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:%s:%d", cfg->rtltcp_host,
                                 cfg->rtltcp_port ? cfg->rtltcp_port : 1234);
                    }
                }
                break;
            case DSDCFG_INPUT_SOAPY:
                if (cfg->soapy_args[0]) {
                    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "soapy:%s", cfg->soapy_args);
                } else {
                    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
                }
                apply_shared_radio_tuning_from_config(cfg, opts);
                break;
            case DSDCFG_INPUT_FILE:
                if (cfg->file_path[0]) {
                    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", cfg->file_path);
                    if (cfg->file_sample_rate > 0 && cfg->file_sample_rate != 48000) {
                        opts->wav_sample_rate = cfg->file_sample_rate;
                        if (opts->wav_decimator != 0) {
                            opts->wav_interpolator = opts->wav_sample_rate / opts->wav_decimator;
                        }
                    }
                }
                break;
            case DSDCFG_INPUT_TCP:
                if (cfg->tcp_host[0]) {
                    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "tcp:%s:%d", cfg->tcp_host,
                             cfg->tcp_port ? cfg->tcp_port : 7355);
                }
                break;
            case DSDCFG_INPUT_UDP:
                if (cfg->udp_addr[0]) {
                    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "udp:%s:%d", cfg->udp_addr,
                             cfg->udp_port ? cfg->udp_port : 7355);
                }
                break;
            default: break;
        }

        // RTL-only helpers
        if (cfg->input_source == DSDCFG_INPUT_RTL || cfg->input_source == DSDCFG_INPUT_RTLTCP) {
            opts->rtl_auto_ppm = cfg->rtl_auto_ppm ? 1 : 0;
            if (getenv("DSD_NEO_AUTO_PPM") == NULL) {
                dsd_setenv("DSD_NEO_AUTO_PPM", opts->rtl_auto_ppm ? "1" : "0", 0);
            }
        }
    }

    // Output backend and UI
    if (cfg->has_output) {
        switch (cfg->output_backend) {
            case DSDCFG_OUTPUT_PULSE:
                if (cfg->pulse_output[0]) {
                    snprintf(opts->audio_out_dev, sizeof opts->audio_out_dev, "pulse:%s", cfg->pulse_output);
                } else {
                    snprintf(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "pulse");
                }
                break;
            case DSDCFG_OUTPUT_NULL: snprintf(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null"); break;
            default: break;
        }
        if (cfg->ncurses_ui) {
            opts->use_ncurses_terminal = 1;
        }
    }

    // Decode mode mapping (mirror config preset semantics)
    if (cfg->has_mode) {
        (void)dsd_apply_decode_mode_preset(cfg->decode_mode, DSD_DECODE_PRESET_PROFILE_CONFIG, opts, state);
    }

    // Demodulator path lock/unlock
    if (cfg->has_demod) {
        switch (cfg->demod_path) {
            case DSDCFG_DEMOD_AUTO:
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 1;
                opts->mod_gfsk = 1;
                opts->mod_cli_lock = 0;
                state->rf_mod = 0;
                break;
            case DSDCFG_DEMOD_C4FM:
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                opts->mod_cli_lock = 1;
                state->rf_mod = 0;
                break;
            case DSDCFG_DEMOD_GFSK:
                opts->mod_c4fm = 0;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 1;
                opts->mod_cli_lock = 1;
                state->rf_mod = 2;
                break;
            case DSDCFG_DEMOD_QPSK:
                opts->mod_c4fm = 0;
                opts->mod_qpsk = 1;
                opts->mod_gfsk = 0;
                opts->mod_cli_lock = 1;
                state->rf_mod = 1;
                break;
            default: break;
        }
    }

    // Trunking: enable/CSV/allow-list flags.
    if (cfg->has_trunking) {
        if (cfg->trunk_enabled) {
            opts->p25_trunk = 1;
            opts->trunk_enable = 1;
        }
        if (cfg->trunk_chan_csv[0]) {
            snprintf(opts->chan_in_file, sizeof opts->chan_in_file, "%s", cfg->trunk_chan_csv);
            opts->chan_in_file[sizeof opts->chan_in_file - 1] = '\0';
        }
        if (cfg->trunk_group_csv[0]) {
            snprintf(opts->group_in_file, sizeof opts->group_in_file, "%s", cfg->trunk_group_csv);
            opts->group_in_file[sizeof opts->group_in_file - 1] = '\0';
        }
        opts->trunk_use_allow_list = cfg->trunk_use_allow_list ? 1 : 0;
        opts->trunk_tune_group_calls = cfg->trunk_tune_group_calls ? 1 : 0;
        opts->trunk_tune_private_calls = cfg->trunk_tune_private_calls ? 1 : 0;
        opts->trunk_tune_data_calls = cfg->trunk_tune_data_calls ? 1 : 0;
        opts->trunk_tune_enc_calls = cfg->trunk_tune_enc_calls ? 1 : 0;
    }

    if (cfg->has_logging) {
        snprintf(opts->event_out_file, sizeof opts->event_out_file, "%s", cfg->event_log);
        opts->event_out_file[sizeof opts->event_out_file - 1] = '\0';

        char frame_log_file_next[sizeof opts->frame_log_file];
        snprintf(frame_log_file_next, sizeof frame_log_file_next, "%s", cfg->frame_log);
        frame_log_file_next[sizeof frame_log_file_next - 1] = '\0';
        if (strcmp(opts->frame_log_file, frame_log_file_next) != 0) {
            close_frame_log_handle(opts);
            opts->frame_log_open_error_reported = 0;
            opts->frame_log_write_error_reported = 0;
        }
        snprintf(opts->frame_log_file, sizeof opts->frame_log_file, "%s", frame_log_file_next);
        opts->frame_log_file[sizeof opts->frame_log_file - 1] = '\0';
    }

    if (cfg->has_recording) {
        if (cfg->per_call_wav_dir[0]) {
            snprintf(opts->wav_out_dir, sizeof opts->wav_out_dir, "%s", cfg->per_call_wav_dir);
            opts->wav_out_dir[sizeof opts->wav_out_dir - 1] = '\0';
        }

        // Per-call and static WAV are mutually exclusive (mirror CLI behavior).
        if (cfg->per_call_wav) {
            opts->dmr_stereo_wav = 1;
            opts->static_wav_file = 0;
        } else if (cfg->static_wav_path[0]) {
            opts->dmr_stereo_wav = 0;
            opts->static_wav_file = 1;
            snprintf(opts->wav_out_file, sizeof opts->wav_out_file, "%s", cfg->static_wav_path);
            opts->wav_out_file[sizeof opts->wav_out_file - 1] = '\0';
        } else {
            opts->dmr_stereo_wav = 0;
            opts->static_wav_file = 0;
        }

        if (cfg->raw_wav_path[0]) {
            snprintf(opts->wav_out_file_raw, sizeof opts->wav_out_file_raw, "%s", cfg->raw_wav_path);
            opts->wav_out_file_raw[sizeof opts->wav_out_file_raw - 1] = '\0';
        } else {
            opts->wav_out_file_raw[0] = '\0';
        }

        opts->rdio_mode = cfg->rdio_mode;
        opts->rdio_system_id = cfg->rdio_system_id;
        opts->rdio_upload_timeout_ms = cfg->rdio_upload_timeout_ms;
        opts->rdio_upload_retries = cfg->rdio_upload_retries;
        if (cfg->rdio_api_url[0]) {
            snprintf(opts->rdio_api_url, sizeof opts->rdio_api_url, "%s", cfg->rdio_api_url);
            opts->rdio_api_url[sizeof opts->rdio_api_url - 1] = '\0';
        }
        if (cfg->rdio_api_key[0]) {
            snprintf(opts->rdio_api_key, sizeof opts->rdio_api_key, "%s", cfg->rdio_api_key);
            opts->rdio_api_key[sizeof opts->rdio_api_key - 1] = '\0';
        }
    }

    if (cfg->has_dsp) {
        if (getenv("DSD_NEO_IQ_BALANCE") == NULL) {
            dsd_setenv("DSD_NEO_IQ_BALANCE", cfg->iq_balance ? "1" : "0", 0);
        }
        if (getenv("DSD_NEO_IQ_DC_BLOCK") == NULL) {
            dsd_setenv("DSD_NEO_IQ_DC_BLOCK", cfg->iq_dc_block ? "1" : "0", 0);
        }
    }
}

void
dsd_snapshot_opts_to_user_config(const dsd_opts* opts, const dsd_state* state, dsdneoUserConfig* cfg) {
    if (!opts || !state || !cfg) {
        return;
    }

    user_cfg_reset(cfg);

    // Input snapshot: infer from audio_in_dev prefix and parse fields where
    // possible so that a rendered INI can faithfully recreate the source.
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
            snprintf(cfg->pulse_input, sizeof cfg->pulse_input, "%s", dev);
            cfg->pulse_input[sizeof cfg->pulse_input - 1] = '\0';
        }
    } else {
        cfg->input_source = DSDCFG_INPUT_FILE;
        snprintf(cfg->file_path, sizeof cfg->file_path, "%.*s", (int)(sizeof cfg->file_path - 1), opts->audio_in_dev);
        cfg->file_path[sizeof cfg->file_path - 1] = '\0';
        cfg->file_sample_rate = opts->wav_sample_rate;
    }

    if (cfg->input_source == DSDCFG_INPUT_RTL || cfg->input_source == DSDCFG_INPUT_RTLTCP) {
        cfg->rtl_auto_ppm = opts->rtl_auto_ppm ? 1 : 0;
    }

    // Output snapshot: backend + UI
    cfg->has_output = 1;
    if (strncmp(opts->audio_out_dev, "pulse", 5) == 0) {
        cfg->output_backend = DSDCFG_OUTPUT_PULSE;
        const char* dev = NULL;
        if (opts->audio_out_dev[5] == ':') {
            dev = opts->audio_out_dev + 6;
        }
        if (dev && *dev) {
            snprintf(cfg->pulse_output, sizeof cfg->pulse_output, "%s", dev);
            cfg->pulse_output[sizeof cfg->pulse_output - 1] = '\0';
        }
    } else if (strcmp(opts->audio_out_dev, "null") == 0) {
        cfg->output_backend = DSDCFG_OUTPUT_NULL;
    } else {
        cfg->output_backend = DSDCFG_OUTPUT_UNSET;
    }
    cfg->ncurses_ui = opts->use_ncurses_terminal ? 1 : 0;

    // Mode snapshot: recognize common presets by flags.
    cfg->has_mode = 1;
    cfg->decode_mode = dsd_infer_decode_mode_preset(opts);

    // Demod path snapshot (capture explicit CLI/UI locks only)
    if (opts->mod_cli_lock) {
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

    // Trunking snapshot
    cfg->has_trunking = 1;
    cfg->trunk_enabled = (opts->p25_trunk || opts->trunk_enable) ? 1 : 0;
    snprintf(cfg->trunk_chan_csv, sizeof cfg->trunk_chan_csv, "%s", opts->chan_in_file);
    cfg->trunk_chan_csv[sizeof cfg->trunk_chan_csv - 1] = '\0';
    snprintf(cfg->trunk_group_csv, sizeof cfg->trunk_group_csv, "%s", opts->group_in_file);
    cfg->trunk_group_csv[sizeof cfg->trunk_group_csv - 1] = '\0';
    cfg->trunk_use_allow_list = opts->trunk_use_allow_list ? 1 : 0;
    cfg->trunk_tune_group_calls = opts->trunk_tune_group_calls ? 1 : 0;
    cfg->trunk_tune_private_calls = opts->trunk_tune_private_calls ? 1 : 0;
    cfg->trunk_tune_data_calls = opts->trunk_tune_data_calls ? 1 : 0;
    cfg->trunk_tune_enc_calls = opts->trunk_tune_enc_calls ? 1 : 0;

    // Logging snapshot
    cfg->has_logging = 1;
    snprintf(cfg->event_log, sizeof cfg->event_log, "%s", opts->event_out_file);
    cfg->event_log[sizeof cfg->event_log - 1] = '\0';
    snprintf(cfg->frame_log, sizeof cfg->frame_log, "%s", opts->frame_log_file);
    cfg->frame_log[sizeof cfg->frame_log - 1] = '\0';

    // Recording snapshot
    cfg->has_recording = 1;
    cfg->per_call_wav = opts->dmr_stereo_wav ? 1 : 0;
    snprintf(cfg->per_call_wav_dir, sizeof cfg->per_call_wav_dir, "%s", opts->wav_out_dir);
    cfg->per_call_wav_dir[sizeof cfg->per_call_wav_dir - 1] = '\0';
    if (opts->static_wav_file && opts->wav_out_file[0] != '\0') {
        snprintf(cfg->static_wav_path, sizeof cfg->static_wav_path, "%s", opts->wav_out_file);
        cfg->static_wav_path[sizeof cfg->static_wav_path - 1] = '\0';
    } else {
        cfg->static_wav_path[0] = '\0';
    }
    snprintf(cfg->raw_wav_path, sizeof cfg->raw_wav_path, "%s", opts->wav_out_file_raw);
    cfg->raw_wav_path[sizeof cfg->raw_wav_path - 1] = '\0';
    cfg->rdio_mode = opts->rdio_mode;
    cfg->rdio_system_id = opts->rdio_system_id;
    snprintf(cfg->rdio_api_url, sizeof cfg->rdio_api_url, "%s", opts->rdio_api_url);
    cfg->rdio_api_url[sizeof cfg->rdio_api_url - 1] = '\0';
    snprintf(cfg->rdio_api_key, sizeof cfg->rdio_api_key, "%s", opts->rdio_api_key);
    cfg->rdio_api_key[sizeof cfg->rdio_api_key - 1] = '\0';
    cfg->rdio_upload_timeout_ms = opts->rdio_upload_timeout_ms;
    cfg->rdio_upload_retries = opts->rdio_upload_retries;

    // DSP snapshot (persist runtime toggles via env for the next run)
    cfg->has_dsp = 1;
    const char* iqb = getenv("DSD_NEO_IQ_BALANCE");
    cfg->iq_balance = (iqb && *iqb && atoi(iqb) != 0) ? 1 : 0;
    const char* dcb = getenv("DSD_NEO_IQ_DC_BLOCK");
    cfg->iq_dc_block = (dcb && *dcb && atoi(dcb) != 0) ? 1 : 0;
}

// Template generation ---------------------------------------------------------

void
dsd_user_config_render_template(FILE* stream) {
    if (!stream) {
        return;
    }

    fprintf(stream, "# DSD-neo configuration template\n");
    fprintf(stream, "# Generated by: dsd-neo --dump-config-template\n");
    fprintf(stream, "#\n");
    fprintf(stream, "# Uncomment and modify values as needed.\n");
    fprintf(stream, "# Lines starting with # are comments.\n");
    fprintf(stream, "#\n");
    fprintf(stream, "# Precedence: CLI arguments > environment variables > config file > defaults\n");
    fprintf(stream, "\n");
    fprintf(stream, "version = 1\n\n");

    const char* sections[16];
    int num_sections = dsdcfg_schema_sections(sections, 16);

    for (int s = 0; s < num_sections; s++) {
        const char* section = sections[s];
        fprintf(stream, "[%s]\n", section);

        int schema_count = dsdcfg_schema_count();
        for (int i = 0; i < schema_count; i++) {
            const dsdcfg_schema_entry_t* e = dsdcfg_schema_get(i);
            if (!e || dsd_strcasecmp(e->section, section) != 0) {
                continue;
            }

            /* Skip deprecated keys in template */
            if (e->deprecated) {
                continue;
            }

            /* Print description */
            fprintf(stream, "# %s\n", e->description);

            /* Print type info and constraints */
            switch (e->type) {
                case DSDCFG_TYPE_ENUM:
                    if (e->allowed) {
                        fprintf(stream, "# Allowed: %s\n", e->allowed);
                    }
                    break;
                case DSDCFG_TYPE_INT:
                    if (e->max_val > 0) {
                        fprintf(stream, "# Range: %d to %d\n", e->min_val, e->max_val);
                    } else if (e->min_val != 0) {
                        fprintf(stream, "# Minimum: %d\n", e->min_val);
                    }
                    break;
                case DSDCFG_TYPE_BOOL: fprintf(stream, "# Values: true, false\n"); break;
                case DSDCFG_TYPE_PATH: fprintf(stream, "# Path (supports ~ and $VAR expansion)\n"); break;
                case DSDCFG_TYPE_FREQ: fprintf(stream, "# Frequency (supports K/M/G suffix)\n"); break;
                default: break;
            }

            /* Print commented-out default value */
            if (e->default_str && e->default_str[0]) {
                if (e->type == DSDCFG_TYPE_STRING || e->type == DSDCFG_TYPE_ENUM || e->type == DSDCFG_TYPE_PATH
                    || e->type == DSDCFG_TYPE_FREQ) {
                    fprintf(stream, "# %s = \"%s\"\n", e->key, e->default_str);
                } else {
                    fprintf(stream, "# %s = %s\n", e->key, e->default_str);
                }
            } else {
                fprintf(stream, "# %s = \n", e->key);
            }
            fprintf(stream, "\n");
        }
    }

    /* Add profile section example */
    fprintf(stream, "# --- Profiles ---\n");
    fprintf(stream, "# Define named profiles to quickly switch between configurations.\n");
    fprintf(stream, "# Use: dsd-neo --config config.ini --profile <name>\n");
    fprintf(stream, "#\n");
    fprintf(stream, "# [profile.example]\n");
    fprintf(stream, "# mode.decode = \"p25p1\"\n");
    fprintf(stream, "# trunking.enabled = true\n");
    fprintf(stream, "# input.source = \"rtl\"\n");
    fprintf(stream, "# input.rtl_freq = \"851.375M\"\n");
}
