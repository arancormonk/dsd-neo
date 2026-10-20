// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <stdint.h>
#include <string.h>

int
dsd_receive_filter_equal(const dsd_receive_filter* a, const dsd_receive_filter* b) {
    return a && b && a->enabled == b->enabled && memcmp(a->allow.bits, b->allow.bits, sizeof a->allow.bits) == 0
           && memcmp(a->block.bits, b->block.bits, sizeof a->block.bits) == 0;
}

int
dsd_receive_names_equal(const dsd_service_names* a, const dsd_service_names* b) {
    if (!a || !b || a->count != b->count || a->count > DSD_RECEIVE_NAME_MAX) {
        return 0;
    }
    for (unsigned int i = 0; i < a->count; i++) {
        const dsd_service_name* x = &a->rules[i];
        const dsd_service_name* y = &b->rules[i];
        if (x->kind != y->kind || x->value != y->value || strcmp(x->name, y->name) != 0) {
            return 0;
        }
    }
    return 1;
}

int
dsd_receive_policy_filters_enabled(const dsd_receive_policy* policy) {
    int any_enabled = 0;
    for (int kind = 0; policy && kind < DSD_RECEIVE_FILTER_COUNT; kind++) {
        any_enabled |= policy->filters[kind].enabled;
    }
    return any_enabled;
}

int
dsd_receive_policy_equal(const dsd_receive_policy* a, const dsd_receive_policy* b) {
    if (!a || !b) {
        return 0;
    }
    for (int kind = 0; kind < DSD_RECEIVE_FILTER_COUNT; kind++) {
        if (!dsd_receive_filter_equal(&a->filters[kind], &b->filters[kind])) {
            return 0;
        }
    }
    return dsd_receive_names_equal(&a->names, &b->names);
}

static int
receive_error(char* out, size_t capacity, const char* message) {
    if (out && capacity) {
        DSD_SNPRINTF(out, capacity, "%s", message);
    }
    return -1;
}

const char*
dsd_receive_kind_name(int kind) {
    static const char* const names[] = {"slot", "dcc", "ran", "nac", "tone", "tone"};
    return kind >= 0 && kind < DSD_RECEIVE_NONE ? names[kind] : "";
}

static int
receive_tone_value(int kind, const char* text, unsigned int* out) {
    dsd_tone_set set;
    if (dsd_tone_set_parse(text, &set, NULL, 0) != 0 || dsd_tone_set_count(&set) != 1) {
        return -1;
    }
    if (kind == DSD_RECEIVE_CTCSS) {
        for (int i = 0; i < dsd_ctcss_tone_count(); i++) {
            int value = dsd_ctcss_tone_tenths(i);
            if (dsd_tone_set_contains_ctcss(&set, value)) {
                *out = (unsigned int)value;
                return 0;
            }
        }
    } else {
        for (int i = 0; i < dsd_dcs_code_count(); i++) {
            for (int p = 0; p < 2; p++) {
                int code = dsd_dcs_code(i), canonical = 0, inverted = 0;
                if (dsd_tone_set_contains_dcs(&set, code, p)
                    && dsd_dcs_canonical(code, p, &canonical, &inverted) == 0) {
                    *out = (unsigned int)(canonical * 2 + inverted);
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int
receive_numeric_value(int kind, const char* text, unsigned int* out) {
    const char* digits = text;
    if (kind == DSD_RECEIVE_NAC && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        digits += 2;
    }
    if (!*digits
        || strspn(digits, kind == DSD_RECEIVE_NAC ? "0123456789abcdefABCDEF" : "0123456789") != strlen(digits)) {
        return -1;
    }
    static const long minimum[] = {1, 0, 0, 1};
    static const long maximum[] = {2, 15, 63, 0xFFE};
    long value = 0;
    if (dsd_parse_long_strict(digits, kind == DSD_RECEIVE_NAC ? 16 : 10, minimum[kind], maximum[kind], &value)) {
        return -1;
    }
    *out = (unsigned int)value;
    return 0;
}

int
dsd_receive_value_parse(int kind, const char* text, unsigned int* out) {
    if (!text || !out || !text[0] || kind < 0 || kind >= DSD_RECEIVE_NONE) {
        return -1;
    }
    if (kind == DSD_RECEIVE_CTCSS || kind == DSD_RECEIVE_DCS) {
        return receive_tone_value(kind, text, out);
    }
    return receive_numeric_value(kind, text, out);
}

int
dsd_receive_value_format(int kind, unsigned int value, char* out, size_t capacity) {
    if (!out || !capacity) {
        return -1;
    }
    if (kind == DSD_RECEIVE_CTCSS) {
        return dsd_ctcss_format((int)value, out, capacity);
    }
    if (kind == DSD_RECEIVE_DCS) {
        return dsd_dcs_format((int)(value / 2), (int)(value % 2), out, capacity);
    }
    int n = DSD_SNPRINTF(out, capacity, kind == DSD_RECEIVE_NAC ? "%03X" : "%u", value);
    return n >= 0 && (size_t)n < capacity ? n : -1;
}

int
dsd_receive_set_contains(const dsd_receive_set* set, unsigned int value) {
    return set && value < 4096 && ((set->bits[value / 64] >> (value % 64)) & 1U) != 0;
}

static int
receive_append(char* out, size_t capacity, const char* text) {
    size_t used = strlen(out), length = strlen(text);
    if (used + length >= capacity) {
        return -1;
    }
    DSD_MEMCPY(out + used, text, length + 1);
    return 0;
}

int
dsd_receive_set_parse(int kind, const char* text, dsd_receive_set* out, char* error, size_t capacity) {
    if (!out || !text || kind < 0 || kind >= DSD_RECEIVE_FILTER_COUNT || strlen(text) > DSD_RECEIVE_LIST_MAX) {
        return receive_error(error, capacity, "invalid or oversized receive list");
    }
    dsd_receive_set parsed = {{0}};
    const char* p = text;
    while (*p) {
        const size_t length = strcspn(p, "/");
        char entry[16];
        unsigned int value = 0;
        if (length == 0 || length >= sizeof(entry)) {
            return receive_error(error, capacity, "use / between valid identifiers; no empty entries or commas");
        }
        DSD_MEMCPY(entry, p, length);
        entry[length] = '\0';
        if (dsd_receive_value_parse(kind, entry, &value) != 0) {
            static const char* const hints[] = {"slots must be 1 or 2", "DCC must be 0..15", "RAN must be 0..63",
                                                "NAC must be hexadecimal 001..FFE (optional 0x)"};
            return receive_error(error, capacity, hints[kind]);
        }
        if (dsd_receive_set_contains(&parsed, value)) {
            return receive_error(error, capacity, "duplicate identifier");
        }
        parsed.bits[value / 64] |= UINT64_C(1) << (value % 64);
        p += length;
        if (*p) {
            p++;
        }
        if (!*p && p[-1] == '/') {
            return receive_error(error, capacity, "empty identifier after /");
        }
    }
    /* Every accepted list must also fit as saved: the canonical spelling (three hex digits per NAC) can be longer than
       the text it came from, and a list that could not be written back would be lost on the next save. */
    char canonical[DSD_RECEIVE_LIST_MAX + 1];
    if (dsd_receive_set_format(kind, &parsed, canonical, sizeof canonical) < 0) {
        return receive_error(error, capacity, "receive list too long once saved");
    }
    *out = parsed;
    return 0;
}

int
dsd_receive_set_format(int kind, const dsd_receive_set* set, char* out, size_t capacity) {
    if (!set || !out || !capacity || kind < 0 || kind >= DSD_RECEIVE_FILTER_COUNT) {
        return -1;
    }
    out[0] = '\0';
    for (unsigned int i = 0; i < 4096; i++) {
        if (dsd_receive_set_contains(set, i)) {
            char entry[16];
            if (dsd_receive_value_format(kind, i, entry, sizeof(entry)) < 0
                || (out[0] && receive_append(out, capacity, "/")) || receive_append(out, capacity, entry)) {
                out[0] = '\0';
                return -1;
            }
        }
    }
    return (int)strlen(out);
}

static int
receive_set_empty(const dsd_receive_set* set) {
    for (size_t i = 0; i < sizeof(set->bits) / sizeof(set->bits[0]); i++) {
        if (set->bits[i]) {
            return 0;
        }
    }
    return 1;
}

static dsd_receive_verdict
receive_filter_match(const dsd_receive_filter* filter, int known, unsigned int value) {
    if (!filter->enabled) {
        return DSD_RECEIVE_OFF;
    }
    if (!known) {
        return DSD_RECEIVE_CHECKING;
    }
    return dsd_receive_set_contains(&filter->block, value)
                   || (!receive_set_empty(&filter->allow) && !dsd_receive_set_contains(&filter->allow, value))
               ? DSD_RECEIVE_REJECTED
               : DSD_RECEIVE_ALLOWED;
}

dsd_receive_verdict
dsd_receive_policy_match(const dsd_receive_policy* policy, const dsd_receive_identity* identity) {
    if (!policy || !identity) {
        return DSD_RECEIVE_OFF;
    }
    dsd_receive_verdict slot = DSD_RECEIVE_OFF;
    if (identity->slot) {
        slot = receive_filter_match(&policy->filters[DSD_RECEIVE_SLOT], 1, identity->slot);
    } else if (identity->slot_pending) {
        slot = receive_filter_match(&policy->filters[DSD_RECEIVE_SLOT], 0, 0);
    }
    dsd_receive_verdict code =
        identity->kind > DSD_RECEIVE_SLOT && identity->kind < DSD_RECEIVE_FILTER_COUNT
            ? receive_filter_match(&policy->filters[identity->kind], identity->known, identity->value)
            : DSD_RECEIVE_OFF;
    if (slot == DSD_RECEIVE_REJECTED || code == DSD_RECEIVE_REJECTED) {
        return DSD_RECEIVE_REJECTED;
    }
    if (code == DSD_RECEIVE_CHECKING || slot == DSD_RECEIVE_CHECKING) {
        return DSD_RECEIVE_CHECKING;
    }
    return slot == DSD_RECEIVE_ALLOWED || code == DSD_RECEIVE_ALLOWED ? DSD_RECEIVE_ALLOWED : DSD_RECEIVE_OFF;
}

const char*
dsd_receive_service_name(const dsd_service_names* names, const dsd_receive_identity* identity) {
    const char* slot = "";
    if (!names || !identity) {
        return slot;
    }
    for (unsigned int i = 0; i < names->count && i < DSD_RECEIVE_NAME_MAX; i++) {
        const dsd_service_name* rule = &names->rules[i];
        if (identity->known && rule->kind == identity->kind && rule->value == identity->value) {
            return rule->name;
        }
        if (identity->slot && rule->kind == DSD_RECEIVE_SLOT && rule->value == identity->slot) {
            slot = rule->name;
        }
    }
    return slot;
}

/* Length of the well-formed UTF-8 sequence at @p s (shortest form, no surrogates, at most U+10FFFF), or 0. */
static size_t
receive_utf8_sequence(const unsigned char* s) {
    if (s[0] < 0x80U) {
        return 1;
    }
    size_t length;
    uint32_t scalar, minimum;
    if ((s[0] & 0xE0U) == 0xC0U) {
        length = 2, scalar = s[0] & 0x1FU, minimum = 0x80U;
    } else if ((s[0] & 0xF0U) == 0xE0U) {
        length = 3, scalar = s[0] & 0x0FU, minimum = 0x800U;
    } else if ((s[0] & 0xF8U) == 0xF0U) {
        length = 4, scalar = s[0] & 0x07U, minimum = 0x10000U;
    } else {
        return 0;
    }
    for (size_t i = 1; i < length; i++) {
        if ((s[i] & 0xC0U) != 0x80U) {
            return 0;
        }
        scalar = (scalar << 6) | (s[i] & 0x3FU);
    }
    if (scalar < minimum || scalar > 0x10FFFFU || (scalar >= 0xD800U && scalar <= 0xDFFFU)) {
        return 0;
    }
    return length;
}

static int
receive_name_valid(const char* name, char* error, size_t capacity) {
    size_t length = strlen(name);
    if (length == 0 || length >= DSD_SERVICE_LABEL_SIZE || name[0] == ' ' || name[length - 1] == ' ') {
        return receive_error(error, capacity, "name must be 1..63 bytes, without leading or trailing spaces");
    }
    for (size_t i = 0; i < length;) {
        const unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127 || c == ',' || c == '"') {
            return receive_error(error, capacity, "names cannot contain commas, double quotes or control characters");
        }
        /* Names reach UTF-8 displays, history files, JSON and WAV names: only well-formed UTF-8 is taken. */
        const size_t sequence = receive_utf8_sequence((const unsigned char*)name + i);
        if (sequence == 0) {
            return receive_error(error, capacity, "names must be valid UTF-8");
        }
        if (c == 0xC2U && (unsigned char)name[i + 1] < 0xA0U) { /* U+0080..U+009F, the C1 controls (CSI among them). */
            return receive_error(error, capacity, "names cannot contain commas, double quotes or control characters");
        }
        i += sequence;
    }
    return 0;
}

static int
receive_name_store(dsd_service_names* names, int kind, unsigned int value, const char* name, char* error,
                   size_t capacity) {
    const size_t length = strlen(name);
    for (unsigned int i = 0; i < names->count && i < DSD_RECEIVE_NAME_MAX; i++) {
        if (names->rules[i].kind == kind && names->rules[i].value == value) {
            return strcmp(names->rules[i].name, name) == 0
                       ? 0
                       : receive_error(error, capacity, "conflicting names for the same identifier");
        }
    }
    if (names->count >= DSD_RECEIVE_NAME_MAX) {
        return receive_error(error, capacity, "at most 32 service names per scope");
    }
    dsd_service_name* rule = &names->rules[names->count++];
    DSD_MEMSET(rule, 0, sizeof(*rule));
    rule->kind = (uint8_t)kind;
    rule->value = (uint16_t)value;
    DSD_MEMCPY(rule->name, name, length + 1);
    return 0;
}

int
dsd_receive_name_add(dsd_service_names* names, int kind, const char* argument, char* error, size_t capacity) {
    const char* equals = argument ? strchr(argument, '=') : NULL;
    if (!names || !equals || equals == argument || (size_t)(equals - argument) >= 16) {
        return receive_error(error, capacity, "expected identifier=name");
    }
    char selector[16];
    size_t length = (size_t)(equals - argument);
    DSD_MEMCPY(selector, argument, length);
    selector[length] = '\0';
    if (kind == DSD_RECEIVE_CTCSS && (selector[0] == 'D' || selector[0] == 'd')) {
        kind = DSD_RECEIVE_DCS;
    }
    unsigned int value = 0;
    if (dsd_receive_value_parse(kind, selector, &value) != 0) {
        return receive_error(error, capacity, "invalid service-name identifier");
    }
    const char* name = equals + 1;
    if (receive_name_valid(name, error, capacity) != 0) {
        return -1;
    }
    return receive_name_store(names, kind, value, name, error, capacity);
}

static int
receive_option_kind(const char* option, int* action) {
    if (!option) {
        return -1;
    }
    for (int kind = 0; kind <= DSD_RECEIVE_CTCSS; kind++) {
        const char* suffix[] = {"allow", "block", "filter", "name"};
        for (int a = 0; a < 4; a++) {
            if (kind == DSD_RECEIVE_CTCSS && a != 3) {
                continue;
            }
            char name[32];
            DSD_SNPRINTF(name, sizeof(name), "--%s%s-%s", a == 2 ? "no-" : "", dsd_receive_kind_name(kind), suffix[a]);
            if (strcmp(name, option) == 0) {
                *action = a;
                return kind;
            }
        }
    }
    if (strcmp(option, "--no-service-names") == 0) {
        *action = 4;
        return DSD_RECEIVE_NONE;
    }
    return -1;
}

int
dsd_receive_option_argument(const char* option) {
    int action = 0;
    return receive_option_kind(option, &action) < 0 ? -1 : (action == 2 || action == 4 ? 0 : 1);
}

static int
receive_names_option(dsd_receive_policy* policy, unsigned int* seen, int kind, int action, const char* argument,
                     char* error, size_t capacity) {
    dsd_service_names names = {0};
    if (*seen & (1U << 16)) {
        names = policy->names;
    }
    if (action == 4) {
        if (*seen & (1U << 17)) {
            return receive_error(error, capacity, "duplicate no-service-names");
        }
        if (*seen & (1U << 16)) {
            return receive_error(error, capacity, "cannot combine no-service-names with naming options");
        }
    } else {
        if (*seen & (1U << 17)) {
            return receive_error(error, capacity, "service names explicitly disabled");
        }
        if (dsd_receive_name_add(&names, kind, argument, error, capacity)) {
            return -1;
        }
    }
    policy->names = names;
    *seen |= (1U << 16) | (action == 4 ? (1U << 17) : 0U);
    return 0;
}

static int
receive_filter_option(dsd_receive_policy* policy, unsigned int* seen, int kind, int action, const char* argument,
                      char* error, size_t capacity) {
    unsigned int bit = 1U << (kind * 3 + action);
    unsigned int group = 7U << (kind * 3);
    if ((*seen & bit) || ((action == 2 || (*seen & (4U << (kind * 3)))) && (*seen & group))) {
        return receive_error(error, capacity, "duplicate or conflicting receive filter option");
    }
    dsd_receive_filter filter = {0};
    if (*seen & group) {
        filter = policy->filters[kind];
    }
    if (action != 2) {
        if (!argument || !argument[0]) {
            return receive_error(error, capacity, "allow and block switches need a nonempty /-separated list");
        }
        if (dsd_receive_set_parse(kind, argument, action == 0 ? &filter.allow : &filter.block, error, capacity)) {
            return -1;
        }
        filter.enabled = 1;
    }
    policy->filters[kind] = filter;
    *seen |= bit;
    return 0;
}

int
dsd_receive_option_apply(dsd_receive_policy* policy, unsigned int* seen, const char* option, const char* argument,
                         char* error, size_t capacity) {
    int action = 0;
    int kind = receive_option_kind(option, &action);
    if (!policy || !seen || kind < 0 || ((action == 2 || action == 4) && argument && argument[0])) {
        return receive_error(error, capacity, "invalid receive option");
    }
    if (action >= 3) {
        return receive_names_option(policy, seen, kind, action, argument, error, capacity);
    }
    return receive_filter_option(policy, seen, kind, action, argument, error, capacity);
}

static int
receive_name_argument(const char** cursor, char* argument, size_t argument_size, char* error, size_t capacity) {
    const char* p = *cursor;
    char quote = 0;
    size_t used = 0;
    while (*p && (quote || (*p != ' ' && *p != '\t'))) {
        char c = *p++;
        if (c == quote) {
            quote = 0;
        } else if (!quote && (c == '\'' || c == '"')) {
            quote = c;
        } else {
            if (used + 1 >= argument_size) {
                return receive_error(error, capacity, "oversized service name");
            }
            argument[used++] = c;
        }
    }
    argument[used] = '\0';
    if (quote) {
        return receive_error(error, capacity, "unclosed quote in service name");
    }
    *cursor = p;
    return 0;
}

int
dsd_receive_names_parse(const char* text, dsd_service_names* out, char* error, size_t capacity) {
    if (!out || !text || strlen(text) > DSD_RECEIVE_NAMES_TEXT_MAX) {
        return receive_error(error, capacity, "oversized naming table");
    }
    dsd_service_names parsed = {0};
    const char* p = text;
    while (*p) {
        p += strspn(p, " \t");
        if (!*p) {
            break;
        }
        size_t length = strcspn(p, " \t=");
        char option[32];
        if (!length || length >= sizeof(option)) {
            return receive_error(error, capacity, "expected a --slot/dcc/ran/nac/tone-name option");
        }
        DSD_MEMCPY(option, p, length);
        option[length] = '\0';
        int action = 0;
        int kind = receive_option_kind(option, &action);
        if (kind < 0 || action != 3) {
            return receive_error(error, capacity, "only naming options belong in the naming table");
        }
        p += length;
        if (*p == '=') {
            p++;
        } else {
            p += strspn(p, " \t");
        }
        char argument[DSD_SERVICE_LABEL_SIZE + 16];
        if (receive_name_argument(&p, argument, sizeof argument, error, capacity)) {
            return -1;
        }
        if (dsd_receive_name_add(&parsed, kind, argument, error, capacity)) {
            return -1;
        }
    }
    *out = parsed;
    return 0;
}

int
dsd_receive_names_format(const dsd_service_names* names, char* out, size_t capacity) {
    if (!names || names->count > DSD_RECEIVE_NAME_MAX || !out || !capacity) {
        return -1;
    }
    out[0] = '\0';
    for (unsigned int i = 0; i < names->count; i++) {
        const dsd_service_name* rule = &names->rules[i];
        if (rule->kind >= DSD_RECEIVE_NONE || !memchr(rule->name, '\0', sizeof rule->name)) {
            return -1;
        }
        char value[16], option[DSD_SERVICE_LABEL_SIZE + 64];
        if (dsd_receive_value_format(rule->kind, rule->value, value, sizeof(value)) < 0) {
            return -1;
        }
        DSD_SNPRINTF(option, sizeof(option), "%s--%s-name \"%s=%s\"", i ? " " : "", dsd_receive_kind_name(rule->kind),
                     value, rule->name);
        if (receive_append(out, capacity, option)) {
            out[0] = '\0';
            return -1;
        }
    }
    return (int)strlen(out);
}

static int
receive_enabled_parse(const char* value, int* out, char* error, size_t capacity) {
    int enabled;
    /* Spelled as every other INI boolean is (user_config_parse_bool_value()): case does not matter. */
    if (dsd_strcasecmp(value, "true") == 0 || dsd_strcasecmp(value, "1") == 0 || dsd_strcasecmp(value, "yes") == 0
        || dsd_strcasecmp(value, "on") == 0) {
        enabled = 1;
    } else if (dsd_strcasecmp(value, "false") == 0 || dsd_strcasecmp(value, "0") == 0
               || dsd_strcasecmp(value, "no") == 0 || dsd_strcasecmp(value, "off") == 0) {
        enabled = 0;
    } else {
        return receive_error(error, capacity, "enabled must be true or false");
    }
    *out = enabled;
    return 0;
}

int
dsd_receive_config_set(dsd_receive_policy* policy, const char* key, const char* value, char* error, size_t capacity) {
    if (!policy || !key || !value) {
        return receive_error(error, capacity, "invalid receive setting");
    }
    if (strcmp(key, "names") == 0) {
        return dsd_receive_names_parse(value, &policy->names, error, capacity);
    }
    for (int kind = 0; kind < DSD_RECEIVE_FILTER_COUNT; kind++) {
        char field[32];
        DSD_SNPRINTF(field, sizeof(field), "%s_enabled", dsd_receive_kind_name(kind));
        if (strcmp(key, field) == 0) {
            return receive_enabled_parse(value, &policy->filters[kind].enabled, error, capacity);
        }
        DSD_SNPRINTF(field, sizeof(field), "%s_allow", dsd_receive_kind_name(kind));
        if (strcmp(key, field) == 0) {
            return dsd_receive_set_parse(kind, value, &policy->filters[kind].allow, error, capacity);
        }
        DSD_SNPRINTF(field, sizeof(field), "%s_block", dsd_receive_kind_name(kind));
        if (strcmp(key, field) == 0) {
            return dsd_receive_set_parse(kind, value, &policy->filters[kind].block, error, capacity);
        }
    }
    return receive_error(error, capacity, "unknown receive setting");
}

int
dsd_receive_edit_parse(dsd_receive_policy* policy, int kind, int enabled, const char* allow, const char* block,
                       const char* names, char* error, size_t capacity) {
    if (!policy || kind < 0 || kind > DSD_RECEIVE_FILTER_COUNT) {
        return receive_error(error, capacity, "unknown receive setting");
    }
    if (kind == DSD_RECEIVE_FILTER_COUNT) {
        return dsd_receive_names_parse(names, &policy->names, error, capacity);
    }
    dsd_receive_filter filter = {0};
    if (enabled != 0 && enabled != 1) {
        return receive_error(error, capacity, "enabled must be 0 or 1");
    }
    filter.enabled = enabled;
    if (dsd_receive_set_parse(kind, allow, &filter.allow, error, capacity)
        || dsd_receive_set_parse(kind, block, &filter.block, error, capacity)) {
        return -1;
    }
    policy->filters[kind] = filter;
    return 0;
}

int
dsd_receive_policy_validate(const dsd_receive_policy* policy, int kind, char* error, size_t capacity) {
    if (!policy || kind < 0 || kind > DSD_RECEIVE_FILTER_COUNT) {
        return receive_error(error, capacity, "unknown receive setting");
    }
    if (kind == DSD_RECEIVE_FILTER_COUNT) {
        char text[DSD_RECEIVE_NAMES_TEXT_MAX + 1];
        dsd_service_names checked;
        if (policy->names.count > DSD_RECEIVE_NAME_MAX
            || dsd_receive_names_format(&policy->names, text, sizeof text) < 0) {
            return receive_error(error, capacity, "invalid service names");
        }
        return dsd_receive_names_parse(text, &checked, error, capacity);
    }
    char allow[DSD_RECEIVE_LIST_MAX + 1], block[DSD_RECEIVE_LIST_MAX + 1];
    dsd_receive_policy checked = {0};
    if (dsd_receive_set_format(kind, &policy->filters[kind].allow, allow, sizeof allow) < 0
        || dsd_receive_set_format(kind, &policy->filters[kind].block, block, sizeof block) < 0) {
        return receive_error(error, capacity, "receive list is too long");
    }
    return dsd_receive_edit_parse(&checked, kind, policy->filters[kind].enabled, allow, block, "", error, capacity);
}
