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

#define MODE_BIT(m)   (1U << (m))
#define DMR           MODE_BIT(DSD_SCAN_MODE_DMR)
#define P25           MODE_BIT(DSD_SCAN_MODE_P25)
#define NXDN          (MODE_BIT(DSD_SCAN_MODE_NXDN48) | MODE_BIT(DSD_SCAN_MODE_NXDN96))
#define DPMR          MODE_BIT(DSD_SCAN_MODE_DPMR)
/* Blank rows and every digital class (INHERIT through M17). */
#define DIGITAL_MODES 0x1FFU
/* Every class a row or target can declare. Equal to DIGITAL_MODES until analog classes exist;
 * an option that is meaningful whatever the class (squelch) uses this, not DIGITAL_MODES. */
#define ANY_MODES     DIGITAL_MODES

typedef struct scan_option_spec scan_option_spec;

/* Store one accepted switch into the parse result. Returns 0, or -1 when the value is invalid. */
typedef int (*scan_option_setter)(const scan_option_spec* spec, const char* argument, unsigned int mode,
                                  dsd_scan_options* parsed);

struct scan_option_spec {
    const char* name;
    uint32_t field;
    unsigned int modes;
    int argument;
    int conventional;
    int value;
    /* The separate-token value may be a negative number: a minus sign, a digit, then only
     * number characters (^-[0-9][0-9.eE+-]*$), so -60 is taken and a malformed -5.5 reaches the
     * setter's own diagnostic. Every other switch refuses a following token that starts with
     * '-', and no switch is spelled -<digit><more>, so a missing value can never swallow the
     * next switch; this flag narrows that rule for one numeric switch only. */
    int signed_numeric;
    /* Fixed explanation for an invalid value; NULL reads "invalid value". */
    const char* hint;
    scan_option_setter set;
};

static int option_set_none(const scan_option_spec* spec, const char* argument, unsigned int mode,
                           dsd_scan_options* parsed);
static int option_set_flag(const scan_option_spec* spec, const char* argument, unsigned int mode,
                           dsd_scan_options* parsed);
static int option_set_force(const scan_option_spec* spec, const char* argument, unsigned int mode,
                            dsd_scan_options* parsed);
static int option_set_hex(const scan_option_spec* spec, const char* argument, unsigned int mode,
                          dsd_scan_options* parsed);
static int option_set_bp(const scan_option_spec* spec, const char* argument, unsigned int mode,
                         dsd_scan_options* parsed);
static int option_set_scrambler(const scan_option_spec* spec, const char* argument, unsigned int mode,
                                dsd_scan_options* parsed);
static int option_set_voice_ms(const scan_option_spec* spec, const char* argument, unsigned int mode,
                               dsd_scan_options* parsed);
static int option_set_max_visit(const scan_option_spec* spec, const char* argument, unsigned int mode,
                                dsd_scan_options* parsed);
static int option_set_squelch(const scan_option_spec* spec, const char* argument, unsigned int mode,
                              dsd_scan_options* parsed);
static int option_set_path(const scan_option_spec* spec, const char* argument, unsigned int mode,
                           dsd_scan_options* parsed);

#define SQUELCH_HINT "expects whole dB from -100 to 0 (0 = off)"

static const scan_option_spec specifications[] = {
    {"-b", DSD_SCAN_OPT_BP, DMR, 1, 0, 0, 0, NULL, option_set_bp},
    {"-H", DSD_SCAN_OPT_HYTERA, DMR | P25 | NXDN, 1, 0, 0, 0, NULL, option_set_hex},
    {"-1", DSD_SCAN_OPT_SCALAR, DMR | P25 | NXDN, 1, 0, 0, 0, NULL, option_set_hex},
    {"-R", DSD_SCAN_OPT_SCRAMBLER, NXDN | DPMR, 1, 0, 0, 0, NULL, option_set_scrambler},
    {"-K", DSD_SCAN_OPT_HEX_FILE, DMR | P25 | NXDN, 1, 0, 0, 0, NULL, option_set_path},
    {"-k", DSD_SCAN_OPT_DEC_FILE, DMR | P25 | NXDN, 1, 0, 0, 0, NULL, option_set_path},
    {"-G", DSD_SCAN_OPT_GROUP, DIGITAL_MODES, 1, 0, 0, 0, NULL, option_set_path},
    {"--dmr-tg-key-csv", DSD_SCAN_OPT_DMR_MAP, DMR, 1, 0, 0, 0, NULL, option_set_path},
    {"--dmr-tg-key-clear", DSD_SCAN_OPT_DMR_MAP, DMR, 0, 0, 0, 0, NULL, option_set_path},
    /* Recognised here and acted on elsewhere: neither carries a value of its own to store. */
    {"--no-decryption-keys", DSD_SCAN_OPT_CLEAR_KEYS, DIGITAL_MODES, 0, 0, 0, 0, NULL, option_set_none},
    {"--key-profile-ref", DSD_SCAN_OPT_KEY_PROFILE_REF, DIGITAL_MODES, 1, 0, 0, 0, NULL, option_set_path},
    {"-4", DSD_SCAN_OPT_FORCE, DMR | NXDN, 0, 0, 1, 0, NULL, option_set_force},
    {"-0", DSD_SCAN_OPT_FORCE, DMR, 0, 0, 0x21, 0, NULL, option_set_force},
    {"--dmr-force-algid", DSD_SCAN_OPT_FORCE, DMR, 1, 0, 0, 0, NULL, option_set_force},
    {"--no-force-key", DSD_SCAN_OPT_FORCE, DIGITAL_MODES, 0, 0, 0, 0, NULL, option_set_force},
    {"-F", DSD_SCAN_OPT_CRC, DMR | P25 | MODE_BIT(DSD_SCAN_MODE_M17), 0, 0, 0, 0, NULL, option_set_flag},
    {"--strict-crc", DSD_SCAN_OPT_CRC, DIGITAL_MODES, 0, 0, 1, 0, NULL, option_set_flag},
    {"-^", DSD_SCAN_OPT_P25_CANDIDATES, P25, 0, 0, 1, 0, NULL, option_set_none},
    {"-e", DSD_SCAN_OPT_DATA, DIGITAL_MODES, 0, 0, 1, 0, NULL, option_set_flag},
    {"--no-data-calls", DSD_SCAN_OPT_DATA, DIGITAL_MODES, 0, 0, 0, 0, NULL, option_set_flag},
    {"--enc-follow", DSD_SCAN_OPT_ENC, DIGITAL_MODES, 0, 0, 1, 0, NULL, option_set_flag},
    {"--enc-lockout", DSD_SCAN_OPT_ENC, DIGITAL_MODES, 0, 0, 0, 0, NULL, option_set_flag},
    {"--scan-voice-only", DSD_SCAN_OPT_VOICE, DIGITAL_MODES, 0, 1, 1, 0, NULL, option_set_flag},
    {"--no-scan-voice-only", DSD_SCAN_OPT_VOICE, DIGITAL_MODES, 0, 1, 0, 0, NULL, option_set_flag},
    {"--scan-voice-qualify-ms", DSD_SCAN_OPT_QUALIFY, DIGITAL_MODES, 1, 1, 0, 0, NULL, option_set_voice_ms},
    {"--scan-voice-hold-ms", DSD_SCAN_OPT_HOLD, DIGITAL_MODES, 1, 1, 0, 0, NULL, option_set_voice_ms},
    /* The per-visit cap applies to every trunk-scan target type, so unlike the voice-gate
     * switches above it is not conventional-only. */
    {"--scan-max-visit-ms", DSD_SCAN_OPT_MAX_VISIT, DIGITAL_MODES, 1, 0, 0, 0, NULL, option_set_max_visit},
    /* Squelch is a receiver setting, so it is legal on every class and target type, trunked
     * control channels included (issue #521). Units and range are rtl_sql's. */
    {"--squelch-db", DSD_SCAN_OPT_SQUELCH, ANY_MODES, 1, 0, 0, 1, SQUELCH_HINT, option_set_squelch},
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
static const char*
option_skip_space(const char* cursor) {
    while (option_space((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static int
option_token(const char** cursor, char* out, size_t size) {
    const char* p = option_skip_space(*cursor);
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
option_set_none(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    (void)spec;
    (void)argument;
    (void)mode;
    (void)parsed;
    return 0;
}

/* The plain on/off switches: the spec already carries the value the row asked for, so there is
 * nothing to parse. */
static int
option_set_flag(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    (void)argument;
    (void)mode;
    switch (spec->field) {
        case DSD_SCAN_OPT_CRC: parsed->values.strict_crc = spec->value; return 0;
        case DSD_SCAN_OPT_VOICE: parsed->values.voice_only = spec->value; return 0;
        case DSD_SCAN_OPT_DATA: parsed->values.tune_data_calls = spec->value; return 0;
        case DSD_SCAN_OPT_ENC: parsed->values.tune_enc_calls = spec->value; return 0;
        default: return -1;
    }
}

static int
option_set_force(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    if (spec->argument) {
        return option_set_hex(spec, argument, mode, parsed);
    }
    parsed->values.force = spec->value;
    return 0;
}

static int
option_set_bp(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    (void)spec;
    (void)mode;
    unsigned long number = 0;
    if (option_decimal(argument, 255UL, &number)) {
        return -1;
    }
    parsed->bp = number;
    return 0;
}

static int
option_set_scrambler(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    (void)spec;
    (void)mode;
    unsigned long number = 0;
    if (option_decimal(argument, 32767UL, &number)) {
        return -1;
    }
    parsed->scalar = number;
    return 0;
}

static int
option_set_voice_ms(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    (void)mode;
    unsigned long number = 0;
    if (option_decimal(argument, 600000UL, &number) || number < 100) {
        return -1;
    }
    if (spec->field == DSD_SCAN_OPT_QUALIFY) {
        parsed->values.qualify_ms = (int)number;
    } else {
        parsed->values.hold_ms = (int)number;
    }
    return 0;
}

static int
option_set_max_visit(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    (void)spec;
    (void)mode;
    unsigned long number = 0;
    /* A row disables the cap outright with 0; any other value is a real cap and shares the
     * CLI bounds, so the voice-gate bounds do not apply. */
    if (option_decimal(argument, 3600000UL, &number) || (number != 0UL && number < 1000UL)) {
        return -1;
    }
    parsed->values.max_visit_ms = (int)number;
    return 0;
}

/* Digits with an optional leading minus and nothing else: no '+', spaces, exponent or fraction. */
static int
option_signed_integer(const char* text) {
    const char* digits = text[0] == '-' ? text + 1 : text;
    return digits[0] != '\0' && strspn(digits, "0123456789") == strlen(digits);
}

static int
option_set_squelch(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    (void)spec;
    (void)mode;
    int db = 0;
    /* Whole dB, rtl_sql's own range. A positive number was a linear mean power in the legacy
     * CLI contract; it is refused here rather than read in a second unit. */
    if (!option_signed_integer(argument) || dsd_parse_int_strict(argument, 10, -100, 0, &db) != 0) {
        return -1;
    }
    parsed->values.squelch_db = db;
    return 0;
}

static int
option_set_path(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed) {
    (void)mode;
    char* path = parsed->values.group_file;
    size_t capacity = sizeof(parsed->values.group_file);
    if (spec->field == DSD_SCAN_OPT_KEY_PROFILE_REF) {
        path = parsed->values.key_profile_ref;
        capacity = sizeof(parsed->values.key_profile_ref);
        if (strspn(argument, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(argument)) {
            return -1;
        }
    }
    if (spec->field == DSD_SCAN_OPT_DMR_MAP) {
        path = parsed->values.dmr_map_file;
        capacity = sizeof(parsed->values.dmr_map_file);
        if (!spec->argument) {
            path[0] = '\0';
            return 0;
        }
    }
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

typedef struct {
    const scan_option_spec* first;
    int alias_used;
} scan_force_options;

/* The only redundant spelling allowed is -0 paired once with --dmr-force-algid 21. */
static int
option_force_alias(const scan_option_spec* first, const scan_option_spec* next, int force) {
    return force == 0x21
           && ((strcmp(first->name, "-0") == 0 && strcmp(next->name, "--dmr-force-algid") == 0)
               || (strcmp(first->name, "--dmr-force-algid") == 0 && strcmp(next->name, "-0") == 0));
}

static int
option_apply(const scan_option_spec* spec, const char* argument, unsigned int mode, dsd_scan_options* parsed,
             scan_force_options* forces, char* error, size_t error_size) {
    const int old_force = parsed->values.force;
    if ((parsed->values.present & spec->field) && spec->field != DSD_SCAN_OPT_FORCE) {
        return option_error(error, error_size, spec->name, "duplicate option");
    }
    if (spec->set(spec, argument, mode, parsed)) {
        return option_error(error, error_size, spec->name, spec->hint ? spec->hint : "invalid value");
    }
    if ((parsed->values.present & DSD_SCAN_OPT_FORCE) && spec->field == DSD_SCAN_OPT_FORCE
        && old_force != parsed->values.force) {
        return option_error(error, error_size, spec->name, "conflicting force settings");
    }
    if (spec->field == DSD_SCAN_OPT_FORCE) {
        if (forces->first) {
            if (forces->alias_used || !option_force_alias(forces->first, spec, parsed->values.force)) {
                return option_error(error, error_size, spec->name, "duplicate force setting");
            }
            forces->alias_used = 1;
        } else {
            forces->first = spec;
        }
    }
    parsed->values.present |= spec->field;
    return 1;
}

/* Whether a separate token reads as a negative number, well formed (-60) or not (-5.5, -1e1):
 * a minus sign, a digit, then only digits, '.', 'e', 'E', '+' or '-'. */
static int
option_negative_number(const char* text) {
    return text[0] == '-' && text[1] >= '0' && text[1] <= '9'
           && strspn(text + 1, "0123456789.eE+-") == strlen(text + 1);
}

/* A missing value cannot consume the following switch as a file path. A signed_numeric switch
 * may take a negative number instead, and its setter judges it; this is the one place the rule
 * lives, and both the parser and the file visitor come through it, so the two always agree on a
 * row. */
static int
option_argument(const scan_option_spec* spec, const char** cursor, char* argument, size_t size) {
    if (option_token(cursor, argument, size) != 1) {
        return 0;
    }
    return argument[0] != '-' || (spec->signed_numeric && option_negative_number(argument));
}

static int
option_read(const char** cursor, unsigned int mode, int conventional, dsd_scan_options* parsed,
            scan_force_options* forces, char* error, size_t error_size) {
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
    if (spec->argument && !equals && !option_argument(spec, cursor, argument, sizeof(argument))) {
        option_error(error, error_size, spec->name, "requires a valid argument");
        goto done;
    }
    rc = option_apply(spec, equals ? equals : argument, mode, parsed, forces, error, error_size);
done:
    DSD_SECURE_ZERO(token, sizeof(token));
    DSD_SECURE_ZERO(argument, sizeof(argument));
    return rc;
}

static int
option_file_read(const char* text, const char** cursor, dsd_scan_option_file_cb callback, void* context) {
    *cursor = option_skip_space(*cursor);
    const char* start = *cursor;
    char token[1024] = {0};
    char argument[DSD_SCAN_OPTIONS_KEY_PATH_MAX] = {0};
    int rc = option_token(cursor, token, sizeof(token));
    if (rc <= 0) {
        goto done;
    }
    char* equals = strncmp(token, "--", 2) == 0 ? strchr(token, '=') : NULL;
    if (equals) {
        *equals++ = '\0';
    }
    const scan_option_spec* spec = option_find(token);
    rc = -1;
    if (!spec || (equals && !spec->argument)) {
        goto done;
    }
    if (spec->argument && !equals) {
        *cursor = option_skip_space(*cursor);
        start = *cursor;
        if (!option_argument(spec, cursor, argument, sizeof(argument))) {
            goto done;
        }
    }
    rc = 0;
    if (spec->argument && (spec->field & (DSD_SCAN_OPT_FILES | DSD_SCAN_OPT_GROUP | DSD_SCAN_OPT_DMR_MAP))) {
        rc = callback(context, spec->name, equals ? equals : argument, (size_t)(start - text),
                      (size_t)(*cursor - start), equals != NULL);
    }
    rc = rc ? -1 : 1;
done:
    DSD_SECURE_ZERO(token, sizeof(token));
    DSD_SECURE_ZERO(argument, sizeof(argument));
    return rc;
}

int
dsd_scan_options_visit_files(const char* text, void* context, dsd_scan_option_file_cb callback) {
    if (!text || !callback) {
        return -1;
    }
    const char* cursor = text;
    int rc;
    do {
        rc = option_file_read(text, &cursor, callback, context);
    } while (rc > 0);
    return rc;
}

static int
option_sources_valid(uint32_t present, char* error, size_t size) {
    if ((present & DSD_SCAN_OPT_CLEAR_KEYS) && (present & (DSD_SCAN_OPT_DIRECT | DSD_SCAN_OPT_FILES))) {
        return option_error(error, size, "options", "no-keys cannot be combined with key material");
    }
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
    scan_force_options forces = {0};
    do {
        rc = option_read(&cursor, mode, conventional, &parsed, &forces, error, error_size);
    } while (rc > 0);
    if (rc == 0) {
        rc = option_sources_valid(parsed.values.present, error, error_size);
    }
    if (rc == 0) {
        /* Every accepted direct switch arms decryption, including zero-valued keys.
         * Legacy material-only columns never acquire this policy override. */
        if (parsed.values.present & DSD_SCAN_OPT_DIRECT) {
            parsed.values.present |= DSD_SCAN_OPT_MUTE_DMR | DSD_SCAN_OPT_MUTE_P25;
            parsed.values.mute_dmr = 0;
        }
        *out = parsed;
    }
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
    return rc;
}
