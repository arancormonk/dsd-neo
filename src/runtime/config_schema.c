// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Configuration schema data and accessor implementation.
 *
 * Defines all valid configuration keys, their types, descriptions,
 * and constraints for validation and template generation.
 */

#include <dsd-neo/runtime/config_schema.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Schema data for all configuration keys */
static const dsdcfg_schema_entry_t s_schema[] = {
    /* [input] section */
    {"input", "source", DSDCFG_TYPE_ENUM, "Input source type", "pulse", 0, 0, "pulse|rtl|rtltcp|file|tcp|udp", 0},
    {"input", "pulse_source", DSDCFG_TYPE_STRING, "PulseAudio source device name", "", 0, 0, NULL, 0},
    {"input", "pulse_input", DSDCFG_TYPE_STRING, "PulseAudio input device (alias for pulse_source)", "", 0, 0, NULL,
     1}, /* deprecated alias */
    {"input", "rtl_device", DSDCFG_TYPE_INT, "RTL-SDR device index (0-based)", "0", 0, 255, NULL, 0},
    {"input", "rtl_freq", DSDCFG_TYPE_FREQ, "RTL-SDR frequency (supports K/M/G suffix)", "851.375M", 0, 0, NULL, 0},
    {"input", "rtl_gain", DSDCFG_TYPE_INT, "RTL-SDR gain in dB (0 for AGC)", "0", 0, 49, NULL, 0},
    {"input", "rtl_ppm", DSDCFG_TYPE_INT, "RTL-SDR frequency correction in PPM", "0", -1000, 1000, NULL, 0},
    {"input", "rtl_bw_khz", DSDCFG_TYPE_INT, "RTL-SDR DSP bandwidth in kHz", "48", 6, 48, NULL, 0},
    {"input", "rtl_sql", DSDCFG_TYPE_INT, "RTL-SDR squelch level in dB (0 to disable)", "0", -100, 0, NULL, 0},
    {"input", "rtl_volume", DSDCFG_TYPE_INT, "RTL-SDR volume multiplier", "2", 1, 10, NULL, 0},
    {"input", "rtltcp_host", DSDCFG_TYPE_STRING, "RTL-TCP server hostname or IP", "127.0.0.1", 0, 0, NULL, 0},
    {"input", "rtltcp_port", DSDCFG_TYPE_INT, "RTL-TCP server port", "1234", 1, 65535, NULL, 0},
    {"input", "file_path", DSDCFG_TYPE_PATH, "Input audio file path", "", 0, 0, NULL, 0},
    {"input", "file_sample_rate", DSDCFG_TYPE_INT, "Input file sample rate in Hz", "48000", 8000, 192000, NULL, 0},
    {"input", "tcp_host", DSDCFG_TYPE_STRING, "TCP direct input hostname", "127.0.0.1", 0, 0, NULL, 0},
    {"input", "tcp_port", DSDCFG_TYPE_INT, "TCP direct input port", "7355", 1, 65535, NULL, 0},
    {"input", "udp_addr", DSDCFG_TYPE_STRING, "UDP input bind address", "127.0.0.1", 0, 0, NULL, 0},
    {"input", "udp_port", DSDCFG_TYPE_INT, "UDP input port", "7355", 1, 65535, NULL, 0},

    /* [output] section */
    {"output", "backend", DSDCFG_TYPE_ENUM, "Audio output backend", "pulse", 0, 0, "pulse|null", 0},
    {"output", "pulse_sink", DSDCFG_TYPE_STRING, "PulseAudio sink device name", "", 0, 0, NULL, 0},
    {"output", "pulse_output", DSDCFG_TYPE_STRING, "PulseAudio output device (alias for pulse_sink)", "", 0, 0, NULL,
     1}, /* deprecated alias */
    {"output", "ncurses_ui", DSDCFG_TYPE_BOOL, "Enable ncurses terminal UI", "false", 0, 0, NULL, 0},

    /* [mode] section */
    {"mode", "decode", DSDCFG_TYPE_ENUM, "Decode mode preset", "auto", 0, 0,
     "auto|p25p1|p25p2|dmr|nxdn48|nxdn96|x2tdma|ysf|dstar|edacs_pv|dpmr|m17|tdma|analog", 0},
    {"mode", "demod", DSDCFG_TYPE_ENUM, "Demodulator path (auto/C4FM/GFSK/QPSK)", "auto", 0, 0, "auto|c4fm|gfsk|qpsk",
     0},

    /* [trunking] section */
    {"trunking", "enabled", DSDCFG_TYPE_BOOL, "Enable trunking support", "false", 0, 0, NULL, 0},
    {"trunking", "chan_csv", DSDCFG_TYPE_PATH, "Channel map CSV file path", "", 0, 0, NULL, 0},
    {"trunking", "group_csv", DSDCFG_TYPE_PATH, "Group list CSV file path", "", 0, 0, NULL, 0},
    {"trunking", "allow_list", DSDCFG_TYPE_BOOL, "Use group.csv as allow list (vs block list)", "false", 0, 0, NULL, 0},
    {"trunking", "tune_group_calls", DSDCFG_TYPE_BOOL, "Tune to group voice calls", "true", 0, 0, NULL, 0},
    {"trunking", "tune_private_calls", DSDCFG_TYPE_BOOL, "Tune to private/individual calls", "true", 0, 0, NULL, 0},
    {"trunking", "tune_data_calls", DSDCFG_TYPE_BOOL, "Tune to data channel grants", "false", 0, 0, NULL, 0},
    {"trunking", "tune_enc_calls", DSDCFG_TYPE_BOOL, "Tune to encrypted calls", "true", 0, 0, NULL, 0},
};

static const int s_schema_count = sizeof(s_schema) / sizeof(s_schema[0]);

int
dsdcfg_schema_count(void) {
    return s_schema_count;
}

const dsdcfg_schema_entry_t*
dsdcfg_schema_get(int index) {
    if (index < 0 || index >= s_schema_count) {
        return NULL;
    }
    return &s_schema[index];
}

const dsdcfg_schema_entry_t*
dsdcfg_schema_find(const char* section, const char* key) {
    if (!section || !key) {
        return NULL;
    }
    for (int i = 0; i < s_schema_count; i++) {
        if (strcasecmp(s_schema[i].section, section) == 0 && strcasecmp(s_schema[i].key, key) == 0) {
            return &s_schema[i];
        }
    }
    return NULL;
}

const char*
dsd_config_key_description(const char* section, const char* key) {
    const dsdcfg_schema_entry_t* e = dsdcfg_schema_find(section, key);
    return e ? e->description : NULL;
}

int
dsd_config_key_is_deprecated(const char* section, const char* key) {
    const dsdcfg_schema_entry_t* e = dsdcfg_schema_find(section, key);
    return e ? e->deprecated : 0;
}

const char*
dsdcfg_type_name(dsdcfg_type_t type) {
    switch (type) {
        case DSDCFG_TYPE_STRING: return "string";
        case DSDCFG_TYPE_INT: return "int";
        case DSDCFG_TYPE_BOOL: return "bool";
        case DSDCFG_TYPE_ENUM: return "enum";
        case DSDCFG_TYPE_PATH: return "path";
        case DSDCFG_TYPE_FREQ: return "freq";
        default: return "unknown";
    }
}

void
dsdcfg_diags_init(dsdcfg_diagnostics_t* diags) {
    if (!diags) {
        return;
    }
    diags->items = NULL;
    diags->count = 0;
    diags->capacity = 0;
    diags->error_count = 0;
    diags->warning_count = 0;
}

void
dsdcfg_diags_add(dsdcfg_diagnostics_t* diags, dsdcfg_diag_level_t level, int line, const char* section, const char* key,
                 const char* message) {
    if (!diags) {
        return;
    }

    /* Grow array if needed */
    if (diags->count >= diags->capacity) {
        int new_cap = diags->capacity == 0 ? 8 : diags->capacity * 2;
        dsdcfg_diagnostic_t* new_items =
            (dsdcfg_diagnostic_t*)realloc(diags->items, (size_t)new_cap * sizeof(*new_items));
        if (!new_items) {
            return; /* allocation failed, drop diagnostic */
        }
        diags->items = new_items;
        diags->capacity = new_cap;
    }

    dsdcfg_diagnostic_t* d = &diags->items[diags->count++];
    d->level = level;
    d->line_number = line;

    d->section[0] = '\0';
    if (section) {
        snprintf(d->section, sizeof(d->section), "%s", section);
        d->section[sizeof(d->section) - 1] = '\0';
    }

    d->key[0] = '\0';
    if (key) {
        snprintf(d->key, sizeof(d->key), "%s", key);
        d->key[sizeof(d->key) - 1] = '\0';
    }

    d->message[0] = '\0';
    if (message) {
        snprintf(d->message, sizeof(d->message), "%s", message);
        d->message[sizeof(d->message) - 1] = '\0';
    }

    if (level == DSDCFG_DIAG_ERROR) {
        diags->error_count++;
    } else if (level == DSDCFG_DIAG_WARNING) {
        diags->warning_count++;
    }
}

void
dsdcfg_diags_free(dsdcfg_diagnostics_t* diags) {
    if (!diags) {
        return;
    }
    free(diags->items);
    diags->items = NULL;
    diags->count = 0;
    diags->capacity = 0;
    diags->error_count = 0;
    diags->warning_count = 0;
}

void
dsdcfg_diags_print(const dsdcfg_diagnostics_t* diags, FILE* stream, const char* path) {
    if (!diags || !stream) {
        return;
    }

    for (int i = 0; i < diags->count; i++) {
        const dsdcfg_diagnostic_t* d = &diags->items[i];
        const char* level_str = "info";
        if (d->level == DSDCFG_DIAG_WARNING) {
            level_str = "warning";
        } else if (d->level == DSDCFG_DIAG_ERROR) {
            level_str = "error";
        }

        if (path && d->line_number > 0) {
            fprintf(stream, "%s:%d: %s", path, d->line_number, level_str);
        } else if (d->line_number > 0) {
            fprintf(stream, "line %d: %s", d->line_number, level_str);
        } else {
            fprintf(stream, "%s", level_str);
        }

        if (d->section[0] && d->key[0]) {
            fprintf(stream, " [%s] %s: ", d->section, d->key);
        } else if (d->section[0]) {
            fprintf(stream, " [%s]: ", d->section);
        } else {
            fprintf(stream, ": ");
        }

        fprintf(stream, "%s\n", d->message);
    }

    if (diags->error_count > 0 || diags->warning_count > 0) {
        fprintf(stream, "\nSummary: %d error(s), %d warning(s)\n", diags->error_count, diags->warning_count);
    }
}

int
dsdcfg_schema_sections(const char** sections, int max_sections) {
    if (!sections || max_sections <= 0) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < s_schema_count && count < max_sections; i++) {
        const char* sec = s_schema[i].section;

        /* Check if section already in output list */
        int found = 0;
        for (int j = 0; j < count; j++) {
            if (strcasecmp(sections[j], sec) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            sections[count++] = sec;
        }
    }

    return count;
}
