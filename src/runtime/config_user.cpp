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

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/runtime/config.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

// Internal helpers ------------------------------------------------------------

static void
user_cfg_reset(dsdneoUserConfig* cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = 1;
}

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
    if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0
        || strcasecmp(v, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0
        || strcasecmp(v, "off") == 0) {
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
#if defined(_WIN32)
                _mkdir(buf);
#else
                mkdir(buf, 0700);
#endif
            }
            *p = saved;
        }
        p++;
    }
#if defined(_WIN32)
    _mkdir(buf);
#else
    mkdir(buf, 0700);
#endif
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

// INI loader ------------------------------------------------------------------

int
dsd_user_config_load(const char* path, dsdneoUserConfig* cfg) {
    if (!cfg) {
        return -1;
    }

    user_cfg_reset(cfg);

    if (!path || !*path) {
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

        if (strcmp(current_section, "input") == 0) {
            cfg->has_input = 1;
            if (strcmp(key_lc, "source") == 0) {
                if (strcasecmp(val, "pulse") == 0) {
                    cfg->input_source = DSDCFG_INPUT_PULSE;
                } else if (strcasecmp(val, "rtl") == 0) {
                    cfg->input_source = DSDCFG_INPUT_RTL;
                } else if (strcasecmp(val, "rtltcp") == 0) {
                    cfg->input_source = DSDCFG_INPUT_RTLTCP;
                } else if (strcasecmp(val, "file") == 0) {
                    cfg->input_source = DSDCFG_INPUT_FILE;
                } else if (strcasecmp(val, "tcp") == 0) {
                    cfg->input_source = DSDCFG_INPUT_TCP;
                } else if (strcasecmp(val, "udp") == 0) {
                    cfg->input_source = DSDCFG_INPUT_UDP;
                }
            } else if (strcmp(key_lc, "pulse_source") == 0 || strcmp(key_lc, "pulse_input") == 0) {
                snprintf(cfg->pulse_input, sizeof cfg->pulse_input, "%s", val);
                cfg->pulse_input[sizeof cfg->pulse_input - 1] = '\0';
            } else if (strcmp(key_lc, "rtl_device") == 0) {
                cfg->rtl_device = (int)parse_int(val, 0);
            } else if (strcmp(key_lc, "rtl_freq") == 0) {
                snprintf(cfg->rtl_freq, sizeof cfg->rtl_freq, "%s", val);
                cfg->rtl_freq[sizeof cfg->rtl_freq - 1] = '\0';
            } else if (strcmp(key_lc, "rtl_gain") == 0) {
                cfg->rtl_gain = (int)parse_int(val, 22);
            } else if (strcmp(key_lc, "rtl_ppm") == 0) {
                cfg->rtl_ppm = (int)parse_int(val, 0);
            } else if (strcmp(key_lc, "rtl_bw_khz") == 0) {
                cfg->rtl_bw_khz = (int)parse_int(val, 12);
            } else if (strcmp(key_lc, "rtl_sql") == 0) {
                cfg->rtl_sql = (int)parse_int(val, 0);
            } else if (strcmp(key_lc, "rtl_volume") == 0) {
                cfg->rtl_volume = (int)parse_int(val, 1);
            } else if (strcmp(key_lc, "rtltcp_host") == 0) {
                snprintf(cfg->rtltcp_host, sizeof cfg->rtltcp_host, "%s", val);
                cfg->rtltcp_host[sizeof cfg->rtltcp_host - 1] = '\0';
            } else if (strcmp(key_lc, "rtltcp_port") == 0) {
                cfg->rtltcp_port = (int)parse_int(val, 1234);
            } else if (strcmp(key_lc, "file_path") == 0) {
                snprintf(cfg->file_path, sizeof cfg->file_path, "%s", val);
                cfg->file_path[sizeof cfg->file_path - 1] = '\0';
            } else if (strcmp(key_lc, "file_sample_rate") == 0) {
                cfg->file_sample_rate = (int)parse_int(val, 48000);
            } else if (strcmp(key_lc, "tcp_host") == 0) {
                snprintf(cfg->tcp_host, sizeof cfg->tcp_host, "%s", val);
                cfg->tcp_host[sizeof cfg->tcp_host - 1] = '\0';
            } else if (strcmp(key_lc, "tcp_port") == 0) {
                cfg->tcp_port = (int)parse_int(val, 7355);
            } else if (strcmp(key_lc, "udp_addr") == 0) {
                snprintf(cfg->udp_addr, sizeof cfg->udp_addr, "%s", val);
                cfg->udp_addr[sizeof cfg->udp_addr - 1] = '\0';
            } else if (strcmp(key_lc, "udp_port") == 0) {
                cfg->udp_port = (int)parse_int(val, 7355);
            }
            continue;
        }

        if (strcmp(current_section, "output") == 0) {
            cfg->has_output = 1;
            if (strcmp(key_lc, "backend") == 0) {
                if (strcasecmp(val, "pulse") == 0) {
                    cfg->output_backend = DSDCFG_OUTPUT_PULSE;
                } else if (strcasecmp(val, "null") == 0) {
                    cfg->output_backend = DSDCFG_OUTPUT_NULL;
                }
            } else if (strcmp(key_lc, "pulse_sink") == 0 || strcmp(key_lc, "pulse_output") == 0) {
                snprintf(cfg->pulse_output, sizeof cfg->pulse_output, "%s", val);
                cfg->pulse_output[sizeof cfg->pulse_output - 1] = '\0';
            } else if (strcmp(key_lc, "ncurses_ui") == 0) {
                int b = 0;
                if (parse_bool(val, &b) == 0) {
                    cfg->ncurses_ui = b;
                }
            }
            continue;
        }

        if (strcmp(current_section, "mode") == 0) {
            cfg->has_mode = 1;
            if (strcmp(key_lc, "decode") == 0) {
                if (strcasecmp(val, "auto") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_AUTO;
                } else if (strcasecmp(val, "p25p1") == 0 || strcasecmp(val, "p25p1_only") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_P25P1;
                } else if (strcasecmp(val, "p25p2") == 0 || strcasecmp(val, "p25p2_only") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_P25P2;
                } else if (strcasecmp(val, "dmr") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_DMR;
                } else if (strcasecmp(val, "nxdn48") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_NXDN48;
                } else if (strcasecmp(val, "nxdn96") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_NXDN96;
                } else if (strcasecmp(val, "x2tdma") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_X2TDMA;
                } else if (strcasecmp(val, "ysf") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_YSF;
                } else if (strcasecmp(val, "dstar") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_DSTAR;
                } else if (strcasecmp(val, "edacs") == 0 || strcasecmp(val, "provoice") == 0
                           || strcasecmp(val, "edacs_pv") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_EDACS_PV;
                } else if (strcasecmp(val, "dpmr") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_DPMR;
                } else if (strcasecmp(val, "m17") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_M17;
                } else if (strcasecmp(val, "tdma") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_TDMA;
                } else if (strcasecmp(val, "analog") == 0 || strcasecmp(val, "analog_monitor") == 0) {
                    cfg->decode_mode = DSDCFG_MODE_ANALOG;
                }
            }
            continue;
        }

        if (strcmp(current_section, "trunking") == 0) {
            cfg->has_trunking = 1;
            if (strcmp(key_lc, "enabled") == 0) {
                int b = 0;
                if (parse_bool(val, &b) == 0) {
                    cfg->trunk_enabled = b;
                }
            } else if (strcmp(key_lc, "chan_csv") == 0) {
                snprintf(cfg->trunk_chan_csv, sizeof cfg->trunk_chan_csv, "%s", val);
                cfg->trunk_chan_csv[sizeof cfg->trunk_chan_csv - 1] = '\0';
            } else if (strcmp(key_lc, "group_csv") == 0) {
                snprintf(cfg->trunk_group_csv, sizeof cfg->trunk_group_csv, "%s", val);
                cfg->trunk_group_csv[sizeof cfg->trunk_group_csv - 1] = '\0';
            } else if (strcmp(key_lc, "allow_list") == 0) {
                int b = 0;
                if (parse_bool(val, &b) == 0) {
                    cfg->trunk_use_allow_list = b;
                }
            }
            continue;
        }
    }

    fclose(fp);
    return 0;
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

#if !defined(_WIN32)
    int fd = fileno(fp);
    if (fd >= 0) {
        (void)fchmod(fd, 0600);
        (void)fsync(fd);
    }
#endif

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
        switch (cfg->decode_mode) {
            case DSDCFG_MODE_AUTO: fprintf(out, "decode = \"auto\"\n"); break;
            case DSDCFG_MODE_P25P1: fprintf(out, "decode = \"p25p1\"\n"); break;
            case DSDCFG_MODE_P25P2: fprintf(out, "decode = \"p25p2\"\n"); break;
            case DSDCFG_MODE_DMR: fprintf(out, "decode = \"dmr\"\n"); break;
            case DSDCFG_MODE_NXDN48: fprintf(out, "decode = \"nxdn48\"\n"); break;
            case DSDCFG_MODE_NXDN96: fprintf(out, "decode = \"nxdn96\"\n"); break;
            case DSDCFG_MODE_X2TDMA: fprintf(out, "decode = \"x2tdma\"\n"); break;
            case DSDCFG_MODE_YSF: fprintf(out, "decode = \"ysf\"\n"); break;
            case DSDCFG_MODE_DSTAR: fprintf(out, "decode = \"dstar\"\n"); break;
            case DSDCFG_MODE_EDACS_PV: fprintf(out, "decode = \"edacs_pv\"\n"); break;
            case DSDCFG_MODE_DPMR: fprintf(out, "decode = \"dpmr\"\n"); break;
            case DSDCFG_MODE_M17: fprintf(out, "decode = \"m17\"\n"); break;
            case DSDCFG_MODE_TDMA: fprintf(out, "decode = \"tdma\"\n"); break;
            case DSDCFG_MODE_ANALOG: fprintf(out, "decode = \"analog\"\n"); break;
            default: break;
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

    // Decode mode mapping (mirror interactive/bootstrap and -f presets)
    if (cfg->has_mode) {
        switch (cfg->decode_mode) {
            case DSDCFG_MODE_AUTO:
                // AUTO: keep init defaults and just tag name
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "AUTO");
                break;
            case DSDCFG_MODE_P25P1:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 1;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                opts->dmr_stereo = 0;
                state->dmr_stereo = 0;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                state->rf_mod = 0;
                opts->dmr_mono = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 1;
                opts->ssize = 36;
                opts->msize = 15;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "P25p1");
                break;
            case DSDCFG_MODE_P25P2:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 1;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                state->samplesPerSymbol = 8;
                state->symbolCenter = 3;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                state->rf_mod = 0;
                opts->dmr_stereo = 1;
                state->dmr_stereo = 0;
                opts->dmr_mono = 0;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "P25p2");
                break;
            case DSDCFG_MODE_DMR:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->inverted_p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 1;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                state->rf_mod = 0;
                opts->dmr_stereo = 1;
                opts->dmr_mono = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 2;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "DMR");
                break;
            case DSDCFG_MODE_NXDN48:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 1;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                state->samplesPerSymbol = 20;
                state->symbolCenter = 10;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                state->rf_mod = 0;
                opts->dmr_stereo = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 1;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "NXDN48");
                break;
            case DSDCFG_MODE_NXDN96:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 1;
                opts->frame_dmr = 0;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                state->samplesPerSymbol = 20;
                state->symbolCenter = 10;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                state->rf_mod = 0;
                opts->dmr_stereo = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 1;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "NXDN96");
                break;
            case DSDCFG_MODE_X2TDMA:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 1;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 2;
                opts->dmr_stereo = 0;
                opts->dmr_mono = 0;
                state->dmr_stereo = 0;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "X2-TDMA");
                break;
            case DSDCFG_MODE_YSF:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 1;
                opts->frame_m17 = 0;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                state->rf_mod = 0;
                opts->dmr_stereo = 1;
                opts->dmr_mono = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 2;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "YSF");
                break;
            case DSDCFG_MODE_DSTAR:
                opts->frame_dstar = 1;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 1;
                opts->dmr_stereo = 0;
                opts->dmr_mono = 0;
                state->dmr_stereo = 0;
                state->rf_mod = 0;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "DSTAR");
                break;
            case DSDCFG_MODE_EDACS_PV:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 1;
                state->ea_mode = 0;
                state->esk_mask = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                state->samplesPerSymbol = 5;
                state->symbolCenter = 2;
                opts->mod_c4fm = 0;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 1;
                state->rf_mod = 2;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 1;
                opts->dmr_stereo = 0;
                opts->dmr_mono = 0;
                state->dmr_stereo = 0;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "EDACS/PV");
                break;
            case DSDCFG_MODE_DPMR:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_provoice = 0;
                opts->frame_dpmr = 1;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                state->samplesPerSymbol = 20;
                state->symbolCenter = 10;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                state->rf_mod = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 1;
                opts->dmr_stereo = 0;
                opts->dmr_mono = 0;
                state->dmr_stereo = 0;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "dPMR");
                break;
            case DSDCFG_MODE_M17:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_provoice = 0;
                opts->frame_dpmr = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 1;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                state->rf_mod = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 1;
                opts->dmr_stereo = 0;
                opts->dmr_mono = 0;
                state->dmr_stereo = 0;
                opts->use_cosine_filter = 0;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "M17");
                break;
            case DSDCFG_MODE_TDMA:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 1;
                opts->frame_p25p2 = 1;
                opts->inverted_p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 1;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
                state->rf_mod = 0;
                opts->dmr_stereo = 1;
                opts->dmr_mono = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 2;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "TDMA");
                break;
            case DSDCFG_MODE_ANALOG:
                opts->frame_dstar = 0;
                opts->frame_x2tdma = 0;
                opts->frame_p25p1 = 0;
                opts->frame_p25p2 = 0;
                opts->frame_nxdn48 = 0;
                opts->frame_nxdn96 = 0;
                opts->frame_dmr = 0;
                opts->frame_dpmr = 0;
                opts->frame_provoice = 0;
                opts->frame_ysf = 0;
                opts->frame_m17 = 0;
                opts->pulse_digi_rate_out = 8000;
                opts->pulse_digi_out_channels = 1;
                opts->dmr_stereo = 0;
                state->dmr_stereo = 0;
                opts->dmr_mono = 0;
                state->rf_mod = 0;
                opts->monitor_input_audio = 1;
                opts->analog_only = 1;
                snprintf(opts->output_name, sizeof opts->output_name, "%s", "Analog Monitor");
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
        if (cfg->trunk_use_allow_list) {
            opts->trunk_use_allow_list = 1;
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
        char buf[1024];
        snprintf(buf, sizeof buf, "%.*s", (int)(sizeof buf - 1), opts->audio_in_dev);
        buf[sizeof buf - 1] = '\0';
        char* save = NULL;
        char* tok = strtok_r(buf, ":", &save); // "rtl"
        int idx = 0;
        while (tok) {
            if (idx == 1) {
                cfg->rtl_device = atoi(tok);
            } else if (idx == 2) {
                snprintf(cfg->rtl_freq, sizeof cfg->rtl_freq, "%s", tok);
                cfg->rtl_freq[sizeof cfg->rtl_freq - 1] = '\0';
            } else if (idx == 3) {
                cfg->rtl_gain = atoi(tok);
            } else if (idx == 4) {
                cfg->rtl_ppm = atoi(tok);
            } else if (idx == 5) {
                cfg->rtl_bw_khz = atoi(tok);
            } else if (idx == 6) {
                cfg->rtl_sql = atoi(tok);
            } else if (idx == 7) {
                cfg->rtl_volume = atoi(tok);
            }
            tok = strtok_r(NULL, ":", &save);
            idx++;
        }
    } else if (strncmp(opts->audio_in_dev, "rtltcp:", 7) == 0) {
        cfg->input_source = DSDCFG_INPUT_RTLTCP;
        char buf[1024];
        snprintf(buf, sizeof buf, "%.*s", (int)(sizeof buf - 1), opts->audio_in_dev);
        buf[sizeof buf - 1] = '\0';
        char* save = NULL;
        char* tok = strtok_r(buf, ":", &save); // "rtltcp"
        int idx = 0;
        while (tok) {
            if (idx == 1) {
                snprintf(cfg->rtltcp_host, sizeof cfg->rtltcp_host, "%s", tok);
                cfg->rtltcp_host[sizeof cfg->rtltcp_host - 1] = '\0';
            } else if (idx == 2) {
                cfg->rtltcp_port = atoi(tok);
            } else if (idx == 3) {
                snprintf(cfg->rtl_freq, sizeof cfg->rtl_freq, "%s", tok);
                cfg->rtl_freq[sizeof cfg->rtl_freq - 1] = '\0';
            } else if (idx == 4) {
                cfg->rtl_gain = atoi(tok);
            } else if (idx == 5) {
                cfg->rtl_ppm = atoi(tok);
            } else if (idx == 6) {
                cfg->rtl_bw_khz = atoi(tok);
            } else if (idx == 7) {
                cfg->rtl_sql = atoi(tok);
            } else if (idx == 8) {
                cfg->rtl_volume = atoi(tok);
            }
            tok = strtok_r(NULL, ":", &save);
            idx++;
        }
    } else if (strncmp(opts->audio_in_dev, "tcp:", 4) == 0) {
        cfg->input_source = DSDCFG_INPUT_TCP;
        char buf[512];
        snprintf(buf, sizeof buf, "%.*s", (int)(sizeof buf - 1), opts->audio_in_dev);
        buf[sizeof buf - 1] = '\0';
        char* save = NULL;
        char* tok = strtok_r(buf, ":", &save); // "tcp"
        int idx = 0;
        while (tok) {
            if (idx == 1) {
                snprintf(cfg->tcp_host, sizeof cfg->tcp_host, "%s", tok);
                cfg->tcp_host[sizeof cfg->tcp_host - 1] = '\0';
            } else if (idx == 2) {
                cfg->tcp_port = atoi(tok);
            }
            tok = strtok_r(NULL, ":", &save);
            idx++;
        }
    } else if (strncmp(opts->audio_in_dev, "udp:", 4) == 0) {
        cfg->input_source = DSDCFG_INPUT_UDP;
        char buf[512];
        snprintf(buf, sizeof buf, "%.*s", (int)(sizeof buf - 1), opts->audio_in_dev);
        buf[sizeof buf - 1] = '\0';
        char* save = NULL;
        char* tok = strtok_r(buf, ":", &save); // "udp"
        int idx = 0;
        while (tok) {
            if (idx == 1) {
                snprintf(cfg->udp_addr, sizeof cfg->udp_addr, "%s", tok);
                cfg->udp_addr[sizeof cfg->udp_addr - 1] = '\0';
            } else if (idx == 2) {
                cfg->udp_port = atoi(tok);
            }
            tok = strtok_r(NULL, ":", &save);
            idx++;
        }
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

    // Mode snapshot: try to recognize common presets by flags.
    cfg->has_mode = 1;
    if (opts->analog_only && opts->monitor_input_audio) {
        cfg->decode_mode = DSDCFG_MODE_ANALOG;
    } else if (opts->frame_p25p1 && opts->frame_p25p2 && opts->frame_dmr && !opts->frame_dstar && !opts->frame_ysf
               && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_TDMA;
    } else if (opts->frame_dmr && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dstar && !opts->frame_ysf
               && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_DMR;
    } else if (opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr && !opts->frame_dstar && !opts->frame_ysf
               && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_P25P1;
    } else if (opts->frame_p25p2 && !opts->frame_p25p1 && !opts->frame_dmr && !opts->frame_dstar && !opts->frame_ysf
               && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_P25P2;
    } else if (opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr
               && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_provoice && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_NXDN48;
    } else if (!opts->frame_nxdn48 && opts->frame_nxdn96 && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr
               && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_provoice && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_NXDN96;
    } else if (opts->frame_x2tdma && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr && !opts->frame_dstar
               && !opts->frame_ysf && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice
               && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_X2TDMA;
    } else if (opts->frame_ysf && !opts->frame_dstar && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr
               && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_YSF;
    } else if (opts->frame_dstar && !opts->frame_ysf && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr
               && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_DSTAR;
    } else if (opts->frame_provoice && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_p25p1
               && !opts->frame_p25p2 && !opts->frame_dmr && !opts->frame_nxdn48 && !opts->frame_nxdn96
               && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_EDACS_PV;
    } else if (opts->frame_dpmr && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_p25p1 && !opts->frame_p25p2
               && !opts->frame_dmr && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice
               && !opts->frame_m17) {
        cfg->decode_mode = DSDCFG_MODE_DPMR;
    } else if (opts->frame_m17 && !opts->frame_dstar && !opts->frame_ysf && !opts->frame_p25p1 && !opts->frame_p25p2
               && !opts->frame_dmr && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_provoice) {
        cfg->decode_mode = DSDCFG_MODE_M17;
    } else {
        cfg->decode_mode = DSDCFG_MODE_AUTO;
    }

    // Trunking snapshot
    cfg->has_trunking = 1;
    cfg->trunk_enabled = (opts->p25_trunk || opts->trunk_enable) ? 1 : 0;
    snprintf(cfg->trunk_chan_csv, sizeof cfg->trunk_chan_csv, "%s", opts->chan_in_file);
    cfg->trunk_chan_csv[sizeof cfg->trunk_chan_csv - 1] = '\0';
    snprintf(cfg->trunk_group_csv, sizeof cfg->trunk_group_csv, "%s", opts->group_in_file);
    cfg->trunk_group_csv[sizeof cfg->trunk_group_csv - 1] = '\0';
    cfg->trunk_use_allow_list = opts->trunk_use_allow_list ? 1 : 0;
}
