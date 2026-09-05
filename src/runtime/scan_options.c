// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MODE_BIT(m) (1U << (m))
#define DMR         MODE_BIT(DSD_SCAN_MODE_DMR)
#define P25         MODE_BIT(DSD_SCAN_MODE_P25)
#define NXDN        (MODE_BIT(DSD_SCAN_MODE_NXDN48) | MODE_BIT(DSD_SCAN_MODE_NXDN96))
#define DPMR        MODE_BIT(DSD_SCAN_MODE_DPMR)
#define ALL_MODES   0x1FFU

typedef struct {
    const char* name;
    uint32_t field;
    unsigned int modes;
    int argument;
    int conventional;
    int value;
} scan_option_spec;

static const scan_option_spec specifications[] = {
    {"-b", DSD_SCAN_OPT_BP, DMR, 1, 0, 0},
    {"-H", DSD_SCAN_OPT_HYTERA, DMR | P25 | NXDN, 1, 0, 0},
    {"-1", DSD_SCAN_OPT_SCALAR, DMR | P25 | NXDN, 1, 0, 0},
    {"-R", DSD_SCAN_OPT_SCRAMBLER, NXDN | DPMR, 1, 0, 0},
    {"-K", DSD_SCAN_OPT_HEX_FILE, DMR | P25 | NXDN, 1, 0, 0},
    {"-k", DSD_SCAN_OPT_DEC_FILE, DMR | P25 | NXDN, 1, 0, 0},
    {"-G", DSD_SCAN_OPT_GROUP, ALL_MODES, 1, 0, 0},
    {"-4", DSD_SCAN_OPT_FORCE, DMR | NXDN, 0, 0, 1},
    {"-0", DSD_SCAN_OPT_FORCE, DMR, 0, 0, 0x21},
    {"--dmr-force-algid", DSD_SCAN_OPT_FORCE, DMR, 1, 0, 0},
    {"--no-force-key", DSD_SCAN_OPT_FORCE, ALL_MODES, 0, 0, 0},
    {"-F", DSD_SCAN_OPT_CRC, DMR | P25 | MODE_BIT(DSD_SCAN_MODE_M17), 0, 0, 0},
    {"--strict-crc", DSD_SCAN_OPT_CRC, ALL_MODES, 0, 0, 1},
    {"--scan-voice-only", DSD_SCAN_OPT_VOICE, ALL_MODES, 0, 1, 1},
    {"--no-scan-voice-only", DSD_SCAN_OPT_VOICE, ALL_MODES, 0, 1, 0},
    {"--scan-voice-qualify-ms", DSD_SCAN_OPT_QUALIFY, ALL_MODES, 1, 1, 0},
    {"--scan-voice-hold-ms", DSD_SCAN_OPT_HOLD, ALL_MODES, 1, 1, 0},
};

static int
option_error(char* error, size_t size, const char* name, const char* reason) {
    if (error && size) {
        DSD_SNPRINTF(error, size, "%s: %s", name, reason);
    }
    return -1;
}

static int
option_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

/* Copy one argument, removing grouping quotes without interpreting escapes. */
static int
option_token(const char** cursor, char* out, size_t size) {
    const char* p = *cursor;
    while (option_space((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        *cursor = p;
        return 0;
    }
    size_t used = 0;
    char quote = 0;
    while (*p && (quote || !option_space((unsigned char)*p))) {
        char c = *p++;
        if (c == quote) {
            quote = 0;
            continue;
        }
        if (!quote && (c == '\'' || c == '"')) {
            quote = c;
            continue;
        }
        if (used + 1 >= size) {
            return -1;
        }
        out[used++] = c;
    }
    if (quote) {
        return -1;
    }
    out[used] = '\0';
    *cursor = p;
    return 1;
}

static const scan_option_spec*
option_find(const char* name) {
    for (size_t i = 0; i < sizeof(specifications) / sizeof(specifications[0]); i++) {
        if (strcmp(name, specifications[i].name) == 0) {
            return &specifications[i];
        }
    }
    return NULL;
}

static int
option_decimal(const char* text, unsigned long max, unsigned long* out) {
    if (!text || !*text) {
        return -1;
    }
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
    }
    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value > max) {
        return -1;
    }
    *out = value;
    return 0;
}

static int
option_hex(const char* text, uint64_t words[4], unsigned int* digits) {
    char hex[65] = {0};
    size_t n = 0;
    while (option_space((unsigned char)*text)) {
        text++;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    int rc = -1;
    for (; *text; text++) {
        if (option_space((unsigned char)*text)) {
            continue;
        }
        if (n == sizeof(hex) - 1) {
            goto done;
        }
        hex[n++] = *text;
    }
    if (!n) {
        goto done;
    }
    for (size_t offset = 0; offset < n; offset += 16) {
        size_t width = n - offset < 16 ? n - offset : 16;
        if (dsd_parse_hex_u64_n(hex + offset, width, &words[offset / 16]) != 0) {
            goto done;
        }
    }
    *digits = (unsigned int)n;
    rc = 0;
done:
    DSD_SECURE_ZERO(hex, sizeof(hex));
    return rc;
}

static int
option_hytera_width(unsigned int digits, unsigned int mode) {
    if (digits != 10 && digits != 32 && digits != 64) {
        return 0;
    }
    if (mode == DSD_SCAN_MODE_P25 && digits == 10) {
        return 0;
    }
    return !(MODE_BIT(mode) & NXDN) || digits == 64;
}

static int
option_has_privacy_material(const dsd_scan_options* parsed) {
    return parsed->bp || parsed->hytera[0] || parsed->hytera[1] || parsed->hytera[2] || parsed->hytera[3];
}

static int
option_set_hex(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    uint64_t words[4] = {0};
    unsigned int digits = 0;
    int rc = option_hex(argument, words, &digits);
    if (rc == 0 && spec->field == DSD_SCAN_OPT_HYTERA) {
        if (!option_hytera_width(digits, mode)) {
            rc = -1;
        }
        if (rc == 0) {
            DSD_MEMCPY(parsed->hytera, words, sizeof(words));
            parsed->hytera_digits = digits;
        }
    } else if (rc == 0 && spec->field == DSD_SCAN_OPT_SCALAR) {
        if (digits > 16) {
            rc = -1;
        } else {
            parsed->scalar = words[0];
        }
    } else if (rc == 0) {
        if (digits > 2 || words[0] == 1 || words[0] == 0x16) {
            rc = -1;
        } else {
            parsed->values.force = (int)words[0];
        }
    }
    DSD_SECURE_ZERO(words, sizeof(words));
    return rc;
}

static int
option_set_number(const scan_option_spec* spec, const char* argument, dsd_scan_options* parsed) {
    unsigned long number = 0;
    switch (spec->field) {
        case DSD_SCAN_OPT_BP:
        case DSD_SCAN_OPT_SCRAMBLER:
            if (option_decimal(argument, spec->field == DSD_SCAN_OPT_BP ? 255UL : 32767UL, &number)) {
                return -1;
            }
            if (spec->field == DSD_SCAN_OPT_BP) {
                parsed->bp = number;
            } else {
                parsed->scalar = number;
            }
            return 0;
        case DSD_SCAN_OPT_QUALIFY:
        case DSD_SCAN_OPT_HOLD:
            if (option_decimal(argument, 600000UL, &number) || number < 100) {
                return -1;
            }
            if (spec->field == DSD_SCAN_OPT_QUALIFY) {
                parsed->values.qualify_ms = (int)number;
            } else {
                parsed->values.hold_ms = (int)number;
            }
            return 0;
        default: return -1;
    }
}

static int
option_set(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    switch (spec->field) {
        case DSD_SCAN_OPT_FORCE:
            if (spec->argument) {
                return option_set_hex(spec, argument, mode, parsed);
            }
            parsed->values.force = spec->value;
            return 0;
        case DSD_SCAN_OPT_HYTERA:
        case DSD_SCAN_OPT_SCALAR: return option_set_hex(spec, argument, mode, parsed);
        case DSD_SCAN_OPT_BP:
        case DSD_SCAN_OPT_SCRAMBLER:
        case DSD_SCAN_OPT_QUALIFY:
        case DSD_SCAN_OPT_HOLD: return option_set_number(spec, argument, parsed);
        case DSD_SCAN_OPT_CRC: parsed->values.strict_crc = spec->value; return 0;
        case DSD_SCAN_OPT_VOICE: parsed->values.voice_only = spec->value; return 0;
        default: break;
    }
    char* path = parsed->values.group_file;
    size_t capacity = sizeof(parsed->values.group_file);
    if (spec->field == DSD_SCAN_OPT_HEX_FILE) {
        path = parsed->hex_file;
        capacity = sizeof(parsed->hex_file);
    } else if (spec->field == DSD_SCAN_OPT_DEC_FILE) {
        path = parsed->dec_file;
        capacity = sizeof(parsed->dec_file);
    }
    if (!argument[0] || strlen(argument) >= capacity) {
        return -1;
    }
    DSD_SNPRINTF(path, capacity, "%s", argument);
    return 0;
}

static int
option_apply(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed,
             unsigned int* force_sources, char* error, size_t error_size) {
    const int old_force = parsed->values.force;
    if ((parsed->values.present & spec->field) && spec->field != DSD_SCAN_OPT_FORCE) {
        return option_error(error, error_size, spec->name, "duplicate option");
    }
    if (option_set(spec, argument, mode, parsed)) {
        return option_error(error, error_size, spec->name, "invalid value");
    }
    if ((parsed->values.present & DSD_SCAN_OPT_FORCE) && spec->field == DSD_SCAN_OPT_FORCE
        && old_force != parsed->values.force) {
        return option_error(error, error_size, spec->name, "conflicting force settings");
    }
    if (spec->field == DSD_SCAN_OPT_FORCE) {
        const unsigned int source = spec->argument ? 1U : spec->value == 0x21 ? 2U : 4U;
        if (*force_sources && ((*force_sources & source) || parsed->values.force != 0x21)) {
            return option_error(error, error_size, spec->name, "duplicate force setting");
        }
        *force_sources |= source;
    }
    parsed->values.present |= spec->field;
    return 1;
}

static int
option_read(const char** cursor, unsigned int mode, int conventional, dsd_scan_options* parsed,
            unsigned int* force_sources, char* error, size_t error_size) {
    char token[1024] = {0};
    char argument[DSD_SCAN_OPTIONS_KEY_PATH_MAX] = {0};
    int rc = option_token(cursor, token, sizeof(token));
    if (rc <= 0) {
        if (rc < 0) {
            option_error(error, error_size, "options", "invalid quoting or oversized argument");
        }
        goto done;
    }
    char* equals = strncmp(token, "--", 2) == 0 ? strchr(token, '=') : NULL;
    if (equals) {
        *equals++ = '\0';
    }
    const scan_option_spec* spec = option_find(token);
    rc = -1;
    if (!spec) {
        option_error(error, error_size, "options", "unsupported switch or positional argument");
        goto done;
    }
    if (!(spec->modes & MODE_BIT(mode)) || (spec->conventional && !conventional)) {
        option_error(error, error_size, spec->name, "not supported for this mode/target");
        goto done;
    }
    if (equals && !spec->argument) {
        option_error(error, error_size, spec->name, "takes no argument");
        goto done;
    }
    if (spec->argument && !equals && option_token(cursor, argument, sizeof(argument)) != 1) {
        option_error(error, error_size, spec->name, "requires a valid argument");
        goto done;
    }
    rc = option_apply(spec, equals ? equals : argument, mode, parsed, force_sources, error, error_size);
done:
    DSD_SECURE_ZERO(token, sizeof(token));
    DSD_SECURE_ZERO(argument, sizeof(argument));
    return rc;
}

static int
option_sources_valid(uint32_t present, char* error, size_t size) {
    if ((present & DSD_SCAN_OPT_DIRECT) && (present & DSD_SCAN_OPT_FILES)) {
        return option_error(error, size, "options", "direct keys cannot be combined with key files");
    }
    if ((present & DSD_SCAN_OPT_SCALAR) && (present & DSD_SCAN_OPT_SCRAMBLER)) {
        return option_error(error, size, "options", "scalar key families conflict");
    }
    return 0;
}

int
dsd_scan_options_parse(const char* text, unsigned int mode, int conventional, dsd_scan_options* out, char* error,
                       size_t error_size) {
    if (!out || mode > DSD_SCAN_MODE_M17) {
        return option_error(error, error_size, "options", "invalid argument");
    }
    dsd_scan_options parsed = {0};
    const char* cursor = text ? text : "";
    int rc;
    unsigned int force_sources = 0;
    do {
        rc = option_read(&cursor, mode, conventional, &parsed, &force_sources, error, error_size);
    } while (rc > 0);
    if (rc == 0) {
        rc = option_sources_valid(parsed.values.present, error, error_size);
    }
    if (rc == 0) {
        /* The CLI `-b`/`-H` switches decide DMR encrypted-audio muting from whether any privacy
         * material was supplied; explicit zero mutes. Only the option text claims that decision. */
        if (parsed.values.present & (DSD_SCAN_OPT_BP | DSD_SCAN_OPT_HYTERA)) {
            parsed.values.present |= DSD_SCAN_OPT_MUTE_DMR;
            parsed.values.mute_dmr = !option_has_privacy_material(&parsed);
        }
        *out = parsed;
    }
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
    return rc;
}
