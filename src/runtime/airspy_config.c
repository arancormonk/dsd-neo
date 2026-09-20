// SPDX-License-Identifier: GPL-3.0-or-later
#include <ctype.h>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/airspy_config.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/log.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char* key;
    size_t offset;
    int maximum;
    int boolean;
} airspy_integer_setting;

#define AIRSPY_INT(name, max_value, is_bool) {"airspy_" #name, offsetof(dsd_airspy_config, name), max_value, is_bool}
static const airspy_integer_setting integer_settings[] = {
    AIRSPY_INT(sensitivity_gain, 21, 0), AIRSPY_INT(linearity_gain, 21, 0), AIRSPY_INT(lna_gain, 15, 0),
    AIRSPY_INT(mixer_gain, 15, 0),       AIRSPY_INT(vga_gain, 15, 0),       AIRSPY_INT(lna_agc, 1, 1),
    AIRSPY_INT(mixer_agc, 1, 1),         AIRSPY_INT(bias_tee, 1, 1)};
#undef AIRSPY_INT

static int
serial_valid(const char serial[17]) {
    if (serial[16] != '\0') {
        return 0;
    }
    size_t n = strlen(serial);
    if (n != 0 && n != 16) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        if (!isxdigit((unsigned char)serial[i])) {
            return 0;
        }
    }
    return 1;
}

int
dsd_airspy_config_valid(const dsd_airspy_config* c) {
    if (!c || !serial_valid(c->serial) || c->gain_mode < 0 || c->gain_mode > 2) {
        return 0;
    }
    if (c->sample_rate && (c->sample_rate < 225000 || c->sample_rate > DSD_AIRSPY_MAX_RATE)) {
        return 0;
    }
    for (size_t i = 0; i < sizeof integer_settings / sizeof integer_settings[0]; ++i) {
        const int value = *(const int*)((const char*)c + integer_settings[i].offset);
        if (value < 0 || value > integer_settings[i].maximum) {
            return 0;
        }
    }
    return 1;
}

static int
set_integer(dsd_airspy_config* c, const char* key, const char* value) {
    for (size_t i = 0; i < sizeof integer_settings / sizeof integer_settings[0]; ++i) {
        const airspy_integer_setting* setting = &integer_settings[i];
        if (strcmp(setting->key, key) != 0) {
            continue;
        }
        int parsed = 0;
        if (setting->boolean
            && (dsd_strcasecmp(value, "true") == 0 || dsd_strcasecmp(value, "on") == 0
                || dsd_strcasecmp(value, "yes") == 0)) {
            parsed = 1;
        } else if (setting->boolean
                   && (dsd_strcasecmp(value, "false") == 0 || dsd_strcasecmp(value, "off") == 0
                       || dsd_strcasecmp(value, "no") == 0)) {
            parsed = 0;
        } else if (dsd_parse_int_strict(value, 10, 0, setting->maximum, &parsed) != 0) {
            return -1;
        }
        *(int*)((char*)c + setting->offset) = parsed;
        return 0;
    }
    return -1;
}

static int
set_mode(dsd_airspy_config* c, const char* value) {
    const char* modes[] = {"sensitivity", "linearity", "manual"};
    for (int i = 0; i < 3; ++i) {
        if (dsd_strcasecmp(value, modes[i]) == 0) {
            c->gain_mode = i;
            return 0;
        }
    }
    return -1;
}

int
dsd_airspy_config_set(dsd_airspy_config* cfg, const char* key, const char* value) {
    if (!cfg || !key || !value) {
        return -1;
    }
    dsd_airspy_config c = *cfg;
    if (strcmp(key, "airspy_serial") == 0) {
        if (strlen(value) > 16) {
            return -1;
        }
        DSD_SNPRINTF(c.serial, sizeof c.serial, "%s", value);
    } else if (strcmp(key, "airspy_gain_mode") == 0) {
        if (set_mode(&c, value) != 0) {
            return -1;
        }
    } else if (strcmp(key, "airspy_sample_rate") == 0) {
        int rate = 0;
        if (dsd_strcasecmp(value, "auto") != 0 && dsd_parse_int_strict(value, 10, 0, DSD_AIRSPY_MAX_RATE, &rate) != 0) {
            return -1;
        }
        c.sample_rate = (uint32_t)rate;
    } else if (set_integer(&c, key, value) != 0) {
        return -1;
    }
    if (!dsd_airspy_config_valid(&c)) {
        return -1;
    }
    *cfg = c;
    return 0;
}

void
dsd_airspy_config_render(FILE* out, const dsd_airspy_config* c) {
    const char* modes[] = {"sensitivity", "linearity", "manual"};
    if (!out || !dsd_airspy_config_valid(c)) {
        return;
    }
    DSD_FPRINTF(out, "airspy_serial = \"%s\"\nairspy_sample_rate = %u\nairspy_gain_mode = \"%s\"\n", c->serial,
                c->sample_rate, modes[c->gain_mode]);
    DSD_FPRINTF(out,
                "airspy_sensitivity_gain = %d\nairspy_linearity_gain = %d\nairspy_lna_gain = %d\n"
                "airspy_mixer_gain = %d\nairspy_vga_gain = %d\nairspy_lna_agc = %s\n"
                "airspy_mixer_agc = %s\nairspy_bias_tee = %s\n",
                c->sensitivity_gain, c->linearity_gain, c->lna_gain, c->mixer_gain, c->vga_gain,
                c->lna_agc ? "true" : "false", c->mixer_agc ? "true" : "false", c->bias_tee ? "true" : "false");
}

typedef struct {
    uint32_t hz;
    int bw;
    double squelch;
    int volume;
} airspy_tuning;

static int
valid_dsp_bandwidth(int bw) {
    const int allowed[] = {4, 6, 8, 12, 16, 24, 48};
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; ++i) {
        if (bw == allowed[i]) {
            return 1;
        }
    }
    return 0;
}

static void
parse_airspy_bandwidth(const char* text, airspy_tuning* tuning) {
    if (dsd_parse_int_strict(text, 10, 4, 48, &tuning->bw) != 0 || !valid_dsp_bandwidth(tuning->bw)) {
        LOG_WARN("Airspy bandwidth '%s' is unsupported; using 48 kHz.\n", text);
        tuning->bw = 48;
    }
}

static void
parse_airspy_squelch(const char* text, airspy_tuning* tuning) {
    double sql = 0.0;
    /* Squelch is a dB figure; anything beyond this span is a typo rather than a level. */
    if (dsd_parse_double_strict(text, -300.0, 300.0, &sql) != 0 || !isfinite(sql)) {
        LOG_WARN("Invalid Airspy squelch '%s'; keeping previous/default value.\n", text);
        return;
    }
    tuning->squelch = dsd_squelch_level_from_sql(sql);
}

static void
parse_airspy_volume(const char* text, airspy_tuning* tuning) {
    int volume = 0;
    if (dsd_parse_int_strict(text, 10, INT_MIN, INT_MAX, &volume) != 0) {
        LOG_WARN("Invalid Airspy volume '%s'; keeping previous/default value.\n", text);
        return;
    }
    if (volume < 0 || volume > 3) {
        LOG_WARN("Airspy volume '%s' is outside 0–3; clamping.\n", text);
    }
    tuning->volume = volume < 0 ? 0 : (volume > 3 ? 3 : volume);
}

static int
parse_airspy_tuning(char* text, airspy_tuning* tuning) {
    char* fields[4];
    size_t count = 0;
    while (text) {
        if (count == 4) {
            return -1;
        }
        fields[count++] = text;
        text = strchr(text, ':');
        if (text) {
            *text++ = '\0';
        }
    }
    if (!count) {
        return -1;
    }
    tuning->hz = dsd_parse_freq_hz(fields[0]);
    if (tuning->hz < 24000000U || tuning->hz > 1700000000U) {
        LOG_ERROR("Invalid Airspy frequency '%s': expected 24–1700 MHz.\n", fields[0]);
        return -1;
    }
    if (count > 1) {
        parse_airspy_bandwidth(fields[1], tuning);
    }
    if (count > 2) {
        parse_airspy_squelch(fields[2], tuning);
    }
    if (count > 3) {
        parse_airspy_volume(fields[3], tuning);
    }
    return 0;
}

int
dsd_normalize_airspy_input_spec(dsd_opts* opts) {
    if (!opts || !dsd_opts_audio_in_dev_is_airspy_spec(opts->audio_in_dev)) {
        return 0;
    }
    int serial_error = opts->airspy_config_error;
    dsd_airspy_config cfg = opts->airspy;
    char spec[sizeof opts->audio_in_dev];
    DSD_SNPRINTF(spec, sizeof spec, "%s", opts->audio_in_dev);
    char* tail = strchr(spec, ':');
    airspy_tuning tuning = {opts->rtlsdr_center_freq, opts->rtl_dsp_bw_khz, opts->rtl_squelch_level,
                            opts->rtl_volume_multiplier};
    if (tail) {
        ++tail;
        if (strncmp(tail, "serial=", 7) == 0) {
            char* freq = strchr(tail, ':');
            if (freq) {
                *freq++ = '\0';
            }
            if (dsd_airspy_config_set(&cfg, "airspy_serial", tail + 7) != 0 || !cfg.serial[0]) {
                LOG_ERROR("Invalid airspy_serial '%s': expected 16 hexadecimal digits.\n", tail + 7);
                return -1;
            }
            serial_error = 0;
            tail = freq;
        }
        if (tail) {
            if (parse_airspy_tuning(tail, &tuning) != 0) {
                return -1;
            }
        }
    }
    if (serial_error) {
        LOG_ERROR("Invalid airspy_serial in config; override with --airspy-serial or -i airspy:serial=... .\n");
        return -1;
    }
    if (!dsd_airspy_config_valid(&cfg)) {
        return -1;
    }
    opts->airspy_config_error = 0;
    opts->airspy = cfg;
    opts->rtlsdr_center_freq = tuning.hz;
    opts->rtl_dsp_bw_khz = tuning.bw;
    opts->rtl_squelch_level = tuning.squelch;
    opts->rtl_volume_multiplier = tuning.volume;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtltcp_enabled = 0;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "airspy%s%s", cfg.serial[0] ? ":serial=" : "",
                 cfg.serial);
    return 0;
}
