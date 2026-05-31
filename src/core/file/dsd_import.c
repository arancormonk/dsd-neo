// SPDX-License-Identifier: ISC
#include <ctype.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/path_policy.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"

#define BSIZE               999
#define CSV_IMPORT_PATH_MAX 2048

static int
csv_rkey_index(unsigned long long keynumber, unsigned long long offset, size_t* out_index) {
    const size_t capacity = sizeof(((dsd_state*)0)->rkey_array) / sizeof(((dsd_state*)0)->rkey_array[0]);
    const unsigned long long capacity_ull = (unsigned long long)capacity;
    if (out_index == NULL || keynumber >= capacity_ull || offset >= capacity_ull) {
        return 0;
    }
    if (keynumber > capacity_ull - 1ULL - offset) {
        return 0;
    }
    *out_index = (size_t)(keynumber + offset);
    return 1;
}

static FILE*
csv_open_user_read_file(const char* label, const char* requested, char* resolved, size_t resolved_size) {
    if (!label || !requested || requested[0] == '\0' || !resolved || resolved_size == 0) {
        LOG_ERROR("CSV import path is missing.\n");
        return NULL;
    }

    FILE* fp = dsd_path_fopen_user_read_file(requested, resolved, resolved_size);
    if (fp == NULL) {
        LOG_ERROR("Unable to open %s '%s'\n", label, requested);
        return NULL;
    }
    return fp;
}

static inline void
trim_eol(char* s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static int
is_ascii_space(unsigned char c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
}

static char*
trim_ws(char* s) {
    if (s == NULL) {
        return NULL;
    }
    size_t start = 0;
    size_t len = strlen(s);
    while (start < len && is_ascii_space((unsigned char)s[start])) {
        start++;
    }
    while (len > start && is_ascii_space((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s + start;
}

static int
hex_nibble_value(unsigned char c, int* out_nibble) {
    if (!out_nibble) {
        return 0;
    }
    if (c >= '0' && c <= '9') {
        *out_nibble = (int)(c - '0');
        return 1;
    }
    if (c >= 'a' && c <= 'f') {
        *out_nibble = 10 + (int)(c - 'a');
        return 1;
    }
    if (c >= 'A' && c <= 'F') {
        *out_nibble = 10 + (int)(c - 'A');
        return 1;
    }
    return 0;
}

static int
parse_hex_digits_u64_strict(const unsigned char* token, const unsigned char* end, unsigned long long* out) {
    unsigned long long v = 0ULL;
    int digits = 0;
    for (const unsigned char* p = token; p != end; p++) {
        int nib = -1;
        if (!hex_nibble_value(*p, &nib)) {
            return 0;
        }
        if (digits >= 16) {
            return 0;
        }
        v = (v << 4) | (unsigned long long)nib;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    *out = v;
    return 1;
}

static const char*
skip_ascii_space(const char* token) {
    while (*token != '\0' && is_ascii_space((unsigned char)*token)) {
        token++;
    }
    return token;
}

static int
parse_hex_u64_strict(const char* token, unsigned long long* out) {
    if (token == NULL || out == NULL) {
        return 0;
    }

    token = skip_ascii_space(token);
    if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        token += 2;
    }

    const unsigned char* p = (const unsigned char*)token;
    const unsigned char* end = p;
    while (*end != '\0' && !is_ascii_space(*end)) {
        end++;
    }
    p = end;
    while (*p != '\0') {
        if (is_ascii_space(*p)) {
            p++;
            continue;
        }
        return 0;
    }

    return parse_hex_digits_u64_strict((const unsigned char*)token, end, out);
}

static int
parse_dec_u64_strict(const char* token, unsigned long long* out) {
    char* end = NULL;
    unsigned long long v = 0ULL;
    if (token == NULL || out == NULL) {
        return 0;
    }
    while (*token != '\0' && is_ascii_space((unsigned char)*token)) {
        token++;
    }
    if (*token == '\0' || *token == '-' || *token == '+') {
        return 0;
    }
    errno = 0;
    v = strtoull(token, &end, 10);
    if (errno != 0 || end == token) {
        return 0;
    }
    while (*end != '\0' && is_ascii_space((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

static int
parse_dec_long_strict(const char* token, long int* out) {
    char* end = NULL;
    long int v = 0;
    if (token == NULL || out == NULL) {
        return 0;
    }
    while (*token != '\0' && is_ascii_space((unsigned char)*token)) {
        token++;
    }
    if (*token == '\0') {
        return 0;
    }
    errno = 0;
    v = strtol(token, &end, 10);
    if (errno != 0 || end == token) {
        return 0;
    }
    while (*end != '\0' && is_ascii_space((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

static uint16_t
compute_crc_ccitt16_bits(const uint8_t* buf, uint32_t len) {
    uint16_t crc = 0x0000;
    const uint16_t polynomial = 0x1021;
    if (!buf) {
        return 0;
    }
    for (uint32_t i = 0; i < len; i++) {
        if ((((crc >> 15) & 1u) ^ (buf[i] & 1u)) != 0u) {
            crc = (uint16_t)((crc << 1) ^ polynomial);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return (uint16_t)(crc ^ 0xFFFFu);
}

static size_t
group_split_csv_preserve_empty(char* line, char** fields, size_t max_fields) {
    size_t count = 0;
    char* p = line;
    if (!line || !fields || max_fields == 0) {
        return 0;
    }
    fields[count++] = p;
    while (*p != '\0') {
        if (*p == ',') {
            *p = '\0';
            if (count < max_fields) {
                fields[count++] = p + 1;
            }
        }
        p++;
    }
    return count;
}

static int
group_ascii_casecmp(const char* a, const char* b) {
    if (!a || !b) {
        return (a == b) ? 0 : 1;
    }
    while (*a && *b) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int
group_parse_u32_token(const char* token, uint32_t* out) {
    unsigned long long v = 0;
    char* end = NULL;
    const char* p = token;
    if (!token || !out) {
        return 0;
    }
    while (*p != '\0' && is_ascii_space((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0' || *p == '+' || *p == '-') {
        return 0;
    }
    errno = 0;
    v = strtoull(p, &end, 10);
    if (errno != 0 || end == p || v > UINT32_MAX) {
        return 0;
    }
    while (*end != '\0' && is_ascii_space((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

static int
group_parse_single_id(const char* token, uint32_t* out_start, uint32_t* out_end, int* out_is_range) {
    if (!token || !out_start || !out_end || !out_is_range) {
        return 0;
    }
    if (!group_parse_u32_token(token, out_start)) {
        return 0;
    }
    *out_end = *out_start;
    *out_is_range = 0;
    return 1;
}

static int
group_parse_range_id(char* token, char* dash, uint32_t* out_start, uint32_t* out_end, int* out_is_range) {
    uint32_t start = 0;
    uint32_t end = 0;
    if (!token || !dash || !out_start || !out_end || !out_is_range) {
        return 0;
    }
    if (strchr(dash + 1, '-') != NULL) {
        return 0;
    }

    *dash = '\0';
    const char* start_token = trim_ws(token);
    const char* end_token = trim_ws(dash + 1);
    if (!start_token || !end_token || start_token[0] == '\0' || end_token[0] == '\0') {
        return 0;
    }
    if (!group_parse_u32_token(start_token, &start) || !group_parse_u32_token(end_token, &end)) {
        return 0;
    }
    if (start > end) {
        return 0;
    }
    *out_start = start;
    *out_end = end;
    *out_is_range = (start != end) ? 1 : 0;
    return 1;
}

static int
group_parse_id_field(char* token, uint32_t* out_start, uint32_t* out_end, int* out_is_range) {
    if (!token || !out_start || !out_end || !out_is_range) {
        return 0;
    }
    token = trim_ws(token);
    if (!token || token[0] == '\0') {
        return 0;
    }

    char* dash = strchr(token, '-');
    if (!dash) {
        return group_parse_single_id(token, out_start, out_end, out_is_range);
    }
    return group_parse_range_id(token, dash, out_start, out_end, out_is_range);
}

enum group_parse_value_result {
    GROUP_PARSE_VALUE_MISSING = 0,
    GROUP_PARSE_VALUE_OK = 1,
    GROUP_PARSE_VALUE_INVALID = -1,
};

static int
group_parse_bool_field(char* token, int* out) {
    const char* p = NULL;
    if (!token || !out) {
        return GROUP_PARSE_VALUE_INVALID;
    }
    p = trim_ws(token);
    if (!p || p[0] == '\0') {
        return GROUP_PARSE_VALUE_MISSING;
    }
    if (group_ascii_casecmp(p, "true") == 0 || group_ascii_casecmp(p, "yes") == 0 || group_ascii_casecmp(p, "on") == 0
        || strcmp(p, "1") == 0) {
        *out = 1;
        return GROUP_PARSE_VALUE_OK;
    }
    if (group_ascii_casecmp(p, "false") == 0 || group_ascii_casecmp(p, "no") == 0 || group_ascii_casecmp(p, "off") == 0
        || strcmp(p, "0") == 0) {
        *out = 0;
        return GROUP_PARSE_VALUE_OK;
    }
    return GROUP_PARSE_VALUE_INVALID;
}

static int
group_parse_priority_field(char* token, int* out) {
    const char* p = NULL;
    char* end = NULL;
    long v = 0;
    if (!token || !out) {
        return GROUP_PARSE_VALUE_INVALID;
    }
    p = trim_ws(token);
    if (!p || p[0] == '\0') {
        return GROUP_PARSE_VALUE_MISSING;
    }
    if (*p == '+' || *p == '-') {
        return GROUP_PARSE_VALUE_INVALID;
    }
    errno = 0;
    v = strtol(p, &end, 10);
    if (errno != 0 || end == p) {
        return GROUP_PARSE_VALUE_INVALID;
    }
    while (*end != '\0' && is_ascii_space((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0' || v < 0 || v > 100) {
        return GROUP_PARSE_VALUE_INVALID;
    }
    *out = (int)v;
    return GROUP_PARSE_VALUE_OK;
}

typedef struct {
    int policy_active;
    unsigned int prefix_len;
    int invalid_order;
} group_policy_header;

typedef struct {
    unsigned long long key[DSD_VERTEX_KS_MAP_MAX];
    uint8_t bits[DSD_VERTEX_KS_MAP_MAX][882];
    int mod[DSD_VERTEX_KS_MAP_MAX];
    int frame_mode[DSD_VERTEX_KS_MAP_MAX];
    int frame_off[DSD_VERTEX_KS_MAP_MAX];
    int frame_step[DSD_VERTEX_KS_MAP_MAX];
    int count;
} vertex_map_tmp_t;

static group_policy_header
group_parse_policy_header(char* header_line) {
    group_policy_header info = {0, 0, 0};
    char* fields[16];
    static const char* expected[] = {"preempt", "audio", "record", "stream", "tags"};
    size_t field_count = group_split_csv_preserve_empty(header_line, fields, sizeof(fields) / sizeof(fields[0]));
    size_t expected_idx = 0;

    if (field_count < 4) {
        return info;
    }
    if (group_ascii_casecmp(trim_ws(fields[3]), "priority") != 0) {
        return info;
    }
    info.policy_active = 1;
    info.prefix_len = 1;

    for (size_t i = 4; i < field_count && expected_idx < (sizeof(expected) / sizeof(expected[0])); i++) {
        const char* col = trim_ws(fields[i]);
        if (group_ascii_casecmp(col, expected[expected_idx]) == 0) {
            info.prefix_len++;
            expected_idx++;
            continue;
        }
        info.invalid_order = 1;
        break;
    }

    return info;
}

static void
group_entry_init(dsd_tg_policy_entry* entry, uint32_t id_start, uint32_t id_end, int is_range, const char* mode_field,
                 const char* name_field, unsigned int row_count, int* out_mode_blocking) {
    if (!entry || !out_mode_blocking) {
        return;
    }
    DSD_MEMSET(entry, 0, sizeof(*entry));
    entry->id_start = id_start;
    entry->id_end = id_end;
    entry->is_range = is_range ? 1u : 0u;
    entry->source = DSD_TG_POLICY_SOURCE_IMPORTED;
    entry->row = row_count;
    DSD_SNPRINTF(entry->mode, sizeof(entry->mode), "%s", mode_field ? mode_field : "");
    DSD_SNPRINTF(entry->name, sizeof(entry->name), "%s", name_field ? name_field : "");
    entry->priority = 0;
    entry->preempt = 0;
    *out_mode_blocking = (strcmp(entry->mode, "B") == 0 || strcmp(entry->mode, "DE") == 0);
    entry->audio = *out_mode_blocking ? 0u : 1u;
    entry->record = entry->audio;
    entry->stream = entry->audio;
}

static void
group_apply_optional_bool_field(const char* filename, unsigned int row_count, const char* label, char* token,
                                uint8_t* target, int* has_flag) {
    int parsed = 0;
    int br = group_parse_bool_field(token, &parsed);
    if (br == GROUP_PARSE_VALUE_OK) {
        *target = parsed ? 1u : 0u;
        if (has_flag) {
            *has_flag = 1;
        }
        return;
    }
    if (br == GROUP_PARSE_VALUE_INVALID) {
        LOG_WARNING("Group file '%s' row %u has invalid %s value '%s'; using default.\n", filename, row_count, label,
                    token);
    }
}

static void
group_apply_priority_field(const group_policy_header* header, const char* filename, unsigned int row_count,
                           size_t field_count, char** fields, dsd_tg_policy_entry* entry) {
    if (!header || !fields || !entry) {
        return;
    }
    if (header->prefix_len >= 1 && field_count > 3) {
        int parsed_priority = 0;
        int pr = group_parse_priority_field(fields[3], &parsed_priority);
        if (pr == GROUP_PARSE_VALUE_OK) {
            entry->priority = parsed_priority;
        } else if (pr == GROUP_PARSE_VALUE_INVALID) {
            LOG_WARNING("Group file '%s' row %u has invalid priority '%s'; defaulting to 0.\n", filename, row_count,
                        fields[3]);
            entry->priority = 0;
        }
    }
}

static void
group_apply_preempt_field(const group_policy_header* header, const char* filename, unsigned int row_count,
                          size_t field_count, char** fields, dsd_tg_policy_entry* entry) {
    if (!header || !fields || !entry) {
        return;
    }
    if (header->prefix_len >= 2 && field_count > 4) {
        int parsed = 0;
        int br = group_parse_bool_field(fields[4], &parsed);
        if (br == GROUP_PARSE_VALUE_OK) {
            entry->preempt = parsed ? 1u : 0u;
        } else if (br == GROUP_PARSE_VALUE_INVALID) {
            LOG_WARNING("Group file '%s' row %u has invalid preempt value '%s'; defaulting to false.\n", filename,
                        row_count, fields[4]);
            entry->preempt = 0;
        }
    }
}

static void
group_apply_media_fields(const group_policy_header* header, const char* filename, unsigned int row_count,
                         size_t field_count, char** fields, dsd_tg_policy_entry* entry, int* has_audio, int* has_record,
                         int* has_stream) {
    if (!header || !fields || !entry || !has_audio || !has_record || !has_stream) {
        return;
    }
    if (header->prefix_len >= 3 && field_count > 5) {
        group_apply_optional_bool_field(filename, row_count, "audio", fields[5], &entry->audio, has_audio);
    }
    if (header->prefix_len >= 4 && field_count > 6) {
        group_apply_optional_bool_field(filename, row_count, "record", fields[6], &entry->record, has_record);
    }
    if (header->prefix_len >= 5 && field_count > 7) {
        group_apply_optional_bool_field(filename, row_count, "stream", fields[7], &entry->stream, has_stream);
    }
}

static void
group_enforce_media_constraints(const char* filename, unsigned int row_count, dsd_tg_policy_entry* entry,
                                int mode_blocking, int has_audio, int has_record, int has_stream) {
    if (!entry) {
        return;
    }
    if (mode_blocking) {
        if ((has_audio && entry->audio) || (has_record && entry->record) || (has_stream && entry->stream)) {
            LOG_WARNING("Group file '%s' row %u has blocking mode with enabled media flags; forcing media off.\n",
                        filename, row_count);
        }
        entry->audio = 0u;
        entry->record = 0u;
        entry->stream = 0u;
        return;
    }
    if (entry->audio == 0u && ((has_record && entry->record) || (has_stream && entry->stream))) {
        LOG_WARNING("Group file '%s' row %u sets audio off with record/stream on; forcing record/stream off.\n",
                    filename, row_count);
        entry->record = 0u;
        entry->stream = 0u;
    }
}

static void
group_apply_policy_fields(const group_policy_header* header, const char* filename, unsigned int row_count,
                          size_t field_count, char** fields, dsd_tg_policy_entry* entry, int mode_blocking) {
    int has_audio = 0;
    int has_record = 0;
    int has_stream = 0;

    if (!header || !entry || !fields || !header->policy_active) {
        return;
    }

    group_apply_priority_field(header, filename, row_count, field_count, fields, entry);
    group_apply_preempt_field(header, filename, row_count, field_count, fields, entry);
    group_apply_media_fields(header, filename, row_count, field_count, fields, entry, &has_audio, &has_record,
                             &has_stream);
    group_enforce_media_constraints(filename, row_count, entry, mode_blocking, has_audio, has_record, has_stream);
}

static void
group_commit_entry(dsd_state* state, const dsd_tg_policy_entry* entry, int is_range, const char* filename,
                   unsigned int row_count, size_t* dropped_policy_alloc_rows) {
    int rc = 0;
    if (!state || !entry || !filename || !dropped_policy_alloc_rows) {
        return;
    }

    rc = is_range ? dsd_tg_policy_add_range_entry(state, entry) : dsd_tg_policy_append_exact(state, entry);
    if (rc == -1) {
        (*dropped_policy_alloc_rows)++;
        return;
    }
    if (rc == 1) {
        if (!is_range) {
            LOG_WARNING("Group file '%s' row %u has invalid exact entry and was skipped.\n", filename, row_count);
        } else {
            LOG_WARNING("Group file '%s' row %u has invalid range and was skipped.\n", filename, row_count);
        }
    }
}

int
csvGroupImportPath(const char* group_file_path, dsd_state* state) {
    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";
    char buffer[BSIZE];
    FILE* fp = NULL;
    unsigned int row_count = 0;
    size_t dropped_policy_alloc_rows = 0;
    group_policy_header header = {0, 0, 0};
    int warned_header_order = 0;

    if (!group_file_path || group_file_path[0] == '\0' || !state) {
        return -1;
    }

    fp = csv_open_user_read_file("group file", group_file_path, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }

    while (fgets(buffer, BSIZE, fp)) {
        char* fields[32];
        size_t field_count = 0;
        uint32_t id_start = 0;
        uint32_t id_end = 0;
        int is_range = 0;
        const char* mode_field = NULL;
        const char* name_field = NULL;
        dsd_tg_policy_entry entry;
        int mode_blocking = 0;

        row_count++;
        trim_eol(buffer);

        if (row_count == 1) {
            char header_copy[BSIZE];
            DSD_SNPRINTF(header_copy, sizeof(header_copy), "%s", buffer);
            header = group_parse_policy_header(header_copy);
            if (header.policy_active && header.invalid_order && !warned_header_order) {
                warned_header_order = 1;
                LOG_WARNING(
                    "Group file '%s' header optional policy columns are out of order; ignoring mismatched and later "
                    "optional columns.\n",
                    filename);
            }
            continue; //don't want labels
        }

        field_count = group_split_csv_preserve_empty(buffer, fields, sizeof(fields) / sizeof(fields[0]));
        if (field_count < 3) {
            LOG_WARNING("Group file '%s' row %u missing required fields; skipping.\n", filename, row_count);
            continue;
        }

        if (!group_parse_id_field(fields[0], &id_start, &id_end, &is_range)) {
            LOG_WARNING("Group file '%s' row %u has invalid id '%s'; skipping.\n", filename, row_count, fields[0]);
            continue;
        }

        mode_field = trim_ws(fields[1]);
        name_field = fields[2];
        group_entry_init(&entry, id_start, id_end, is_range, mode_field, name_field, row_count, &mode_blocking);
        group_apply_policy_fields(&header, filename, row_count, field_count, fields, &entry, mode_blocking);
        group_commit_entry(state, &entry, is_range, filename, row_count, &dropped_policy_alloc_rows);
    }

    if (dropped_policy_alloc_rows > 0) {
        LOG_WARNING("Group file '%s' skipped %zu rows due to policy allocation failure.\n", filename,
                    dropped_policy_alloc_rows);
    }

    fclose(fp);
    return 0;
}

int
csvGroupImport(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    return csvGroupImportPath(opts->group_in_file, state);
}

//LCN import for EDACS, migrated to channel map (channel map does both)
int
csvLCNImport(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || opts->lcn_in_file[0] == '\0') {
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";
    char buffer[BSIZE];
    FILE* fp = csv_open_user_read_file("lcn file", opts->lcn_in_file, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }
    int row_count = 0;
    int warned_capacity = 0;
    const int lcn_capacity = (int)(sizeof(state->trunk_lcn_freq) / sizeof(state->trunk_lcn_freq[0]));

    while (fgets(buffer, BSIZE, fp)) {
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* saveptr = NULL;
        const char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
        while (field) {
            int lcn_index = state->lcn_freq_count;
            if (lcn_index < 0) {
                lcn_index = 0;
                state->lcn_freq_count = 0;
            }
            if (lcn_index >= lcn_capacity) {
                if (!warned_capacity) {
                    LOG_WARNING("LCN file '%s' has more than %d frequencies; ignoring extra fields.\n", filename,
                                lcn_capacity);
                    warned_capacity = 1;
                }
                field = dsd_strtok_r(NULL, ",", &saveptr);
                continue;
            }

            long int freq = 0;
            if (parse_dec_long_strict(field, &freq)) {
                state->trunk_lcn_freq[lcn_index] = freq;
            } else {
                state->trunk_lcn_freq[lcn_index] = 0;
            }
            state->lcn_freq_count++; //keep tally of number of Frequencies imported
            LOG_INFO("LCN [%d] [%ld]", lcn_index + 1, state->trunk_lcn_freq[lcn_index]);
            LOG_INFO("\n");

            field = dsd_strtok_r(NULL, ",", &saveptr);
        }
        LOG_INFO("LCN Count %d\n", state->lcn_freq_count);
    }
    fclose(fp);
    return 0;
}

static void
csv_chan_import_apply_field(dsd_state* state, int field_count, const char* field, long int* chan_number) {
    if (!state || !field || !chan_number) {
        return;
    }
    if (field_count == 0) {
        long int parsed_chan = 0;
        if (parse_dec_long_strict(field, &parsed_chan)) {
            *chan_number = parsed_chan;
        }
        return;
    }
    if (field_count != 1) {
        return;
    }

    if (*chan_number >= 0 && *chan_number < 0xFFFF) {
        long int freq = 0;
        if (parse_dec_long_strict(field, &freq)) {
            dsd_state_set_trunk_chan_freq(state, (uint32_t)*chan_number, freq);
        }
    }

    if (state->lcn_freq_count < 0
        || state->lcn_freq_count >= (int)(sizeof(state->trunk_lcn_freq) / sizeof(state->trunk_lcn_freq[0]))) {
        return;
    }

    long int freq = 0;
    if (parse_dec_long_strict(field, &freq)) {
        state->trunk_lcn_freq[state->lcn_freq_count] = freq;
    } else {
        state->trunk_lcn_freq[state->lcn_freq_count] = 0;
    }
    state->lcn_freq_count++; // keep tally of number of Frequencies imported
}

static unsigned long long
csv_key_import_dec_normalize_keynumber(const char* field) {
    unsigned long long keynumber = 0;
    if (!parse_dec_u64_strict(field, &keynumber)) {
        return 0;
    }

    if (keynumber <= 0xFFFFULL) {
        return keynumber;
    }

    uint8_t hash_bits[24];
    keynumber &= 0xFFFFFFULL; // truncate to 24-bits (max allowed)
    for (int i = 0; i < 24; i++) {
        hash_bits[i] = (uint8_t)(((keynumber << i) & 0x800000ULL) >> 23); // load into array for CRC16
    }
    const uint16_t hash = compute_crc_ccitt16_bits(hash_bits, 24);
    LOG_INFO("Hashed ");
    return hash & 0xFFFFULL; // make sure its no larger than 16-bits
}

static void
csv_key_import_dec_store_value(dsd_state* state, unsigned long long keynumber, const char* field) {
    unsigned long long keyvalue = 0;
    const int parsed_keyvalue = parse_dec_u64_strict(field, &keyvalue);
    if (!parsed_keyvalue) {
        keyvalue = 0;
    }

    size_t key_index = 0;
    if (csv_rkey_index(keynumber, 0ULL, &key_index)) {
        state->rkey_array[key_index] = keyvalue & 0xFFFFFFFFFFULL; // doesn't exceed 40-bit value
        state->rkey_array_loaded[key_index] = parsed_keyvalue ? 1U : 0U;
    }
}

static void
csv_key_import_dec_apply_field(dsd_state* state, int field_count, const char* field, unsigned long long* keynumber) {
    if (field_count == 0) {
        *keynumber = csv_key_import_dec_normalize_keynumber(field);
        return;
    }
    if (field_count == 1) {
        csv_key_import_dec_store_value(state, *keynumber, field);
    }
}

int
csvChanImport(const dsd_opts* opts, dsd_state* state) //channel map import
{
    if (!opts || !state || opts->chan_in_file[0] == '\0') {
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";

    char buffer[BSIZE];
    FILE* fp = csv_open_user_read_file("channel map file", opts->chan_in_file, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }
    int row_count = 0;

    long int chan_number = 0;

    while (fgets(buffer, BSIZE, fp)) {
        int field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* saveptr = NULL;
        const char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
        while (field) {
            csv_chan_import_apply_field(state, field_count, field, &chan_number);
            field = dsd_strtok_r(NULL, ",", &saveptr);
            field_count++;
        }
        if (field_count >= 2 && chan_number >= 0 && chan_number < 0xFFFF) {
            LOG_INFO("Channel [%05ld] [%09ld]", chan_number, state->trunk_chan_map[chan_number]);
        }
        LOG_INFO("\n");
    }
    fclose(fp);
    return 0;
}

//Decimal Variant of Key Import
int
csvKeyImportDec(const dsd_opts* opts, dsd_state* state) //multi-key support
{
    if (!opts || !state || opts->key_in_file[0] == '\0') {
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";

    char buffer[BSIZE];
    FILE* fp = csv_open_user_read_file("key file", opts->key_in_file, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }
    int row_count = 0;

    while (fgets(buffer, BSIZE, fp)) {
        unsigned long long int keynumber = 0;
        int field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* saveptr = NULL;
        const char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
        while (field) {
            csv_key_import_dec_apply_field(state, field_count, field, &keynumber);
            field = dsd_strtok_r(NULL, ",", &saveptr);
            field_count++;
        }
        LOG_INFO("Key [%03lld] loaded: %s", keynumber, DSD_SECRET_REDACTED);
        LOG_INFO("\n");
    }
    fclose(fp);
    return 0;
}

static void
csv_key_import_hex_store_value(dsd_state* state, unsigned long long keynumber, unsigned long long offset,
                               const char* field) {
    if (!state || !field) {
        return;
    }
    size_t idx = 0;
    if (!csv_rkey_index(keynumber, offset, &idx)) {
        return;
    }
    unsigned long long v = 0;
    if (parse_hex_u64_strict(field, &v)) {
        state->rkey_array[idx] = v;
        state->rkey_array_loaded[idx] = 1U;
    }
}

static void
csv_key_import_hex_log_offsets(const dsd_state* state, unsigned long long keynumber) {
    unsigned long long out1 = 0, out2 = 0, out3 = 0;
    size_t idx1 = 0, idx2 = 0, idx3 = 0;
    if (csv_rkey_index(keynumber, 0x101ULL, &idx1)) {
        out1 = state->rkey_array[idx1];
    }
    if (csv_rkey_index(keynumber, 0x201ULL, &idx2)) {
        out2 = state->rkey_array[idx2];
    }
    if (csv_rkey_index(keynumber, 0x301ULL, &idx3)) {
        out3 = state->rkey_array[idx3];
    }
    // cppcheck-suppress knownConditionTrueFalse
    if (out1 != 0 || out2 != 0 || out3 != 0) {
        LOG_INFO(" [additional key segments loaded: %s]", DSD_SECRET_REDACTED);
    }
}

static unsigned long long
csv_key_import_hex_parse_row(dsd_state* state, char* buffer) {
    static const unsigned long long offsets[] = {0x0ULL, 0x101ULL, 0x201ULL, 0x301ULL};
    unsigned long long keynumber = 0;
    char* saveptr = NULL;
    const char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
    for (int field_count = 0; field != NULL; field_count++) {
        if (field_count == 0) {
            if (!parse_hex_u64_strict(field, &keynumber)) {
                keynumber = 0;
            }
        } else if (field_count <= (int)(sizeof(offsets) / sizeof(offsets[0]))) {
            csv_key_import_hex_store_value(state, keynumber, offsets[field_count - 1], field);
        }
        field = dsd_strtok_r(NULL, ",", &saveptr);
    }
    return keynumber;
}

static int
vertex_ks_find_or_add_index(vertex_map_tmp_t* tmp, unsigned long long key, const char* path, int row_count,
                            int* out_idx) {
    if (!tmp || !out_idx) {
        return -1;
    }
    for (int i = 0; i < tmp->count; i++) {
        if (tmp->key[i] == key) {
            *out_idx = i;
            LOG_WARNING("Vertex KS CSV '%s' line %d: duplicate key, replacing previous mapping.\n", path, row_count);
            return 0;
        }
    }
    if (tmp->count >= DSD_VERTEX_KS_MAP_MAX) {
        LOG_ERROR("Vertex KS CSV '%s' exceeds capacity (%d rows max)\n", path, DSD_VERTEX_KS_MAP_MAX);
        return -1;
    }
    *out_idx = tmp->count++;
    return 0;
}

static int
vertex_ks_parse_row(const char* path, int row_count, char* line, vertex_map_tmp_t* tmp) {
    char* saveptr = NULL;
    char* key_tok = dsd_strtok_r(line, ",", &saveptr);
    char* ks_tok = dsd_strtok_r(NULL, ",", &saveptr);
    if (key_tok == NULL || ks_tok == NULL) {
        LOG_ERROR("Vertex KS CSV '%s' line %d: expected key_hex,keystream_spec\n", path, row_count);
        return -1;
    }

    key_tok = trim_ws(key_tok);
    ks_tok = trim_ws(ks_tok);
    if (key_tok == NULL || key_tok[0] == '\0' || ks_tok == NULL || ks_tok[0] == '\0') {
        LOG_ERROR("Vertex KS CSV '%s' line %d: empty key or keystream field\n", path, row_count);
        return -1;
    }

    unsigned long long key = 0ULL;
    if (parse_hex_u64_strict(key_tok, &key) != 1) {
        LOG_ERROR("Vertex KS CSV '%s' line %d: invalid key (expected hex)\n", path, row_count);
        return -1;
    }

    uint8_t parsed_bits[882];
    int parsed_mod = 0;
    int parsed_frame_mode = 0;
    int parsed_frame_off = 0;
    int parsed_frame_step = 0;
    char err[128] = {0};
    if (dmr_parse_static_keystream_spec(ks_tok, parsed_bits, &parsed_mod, &parsed_frame_mode, &parsed_frame_off,
                                        &parsed_frame_step, err, sizeof err)
        != 1) {
        if (err[0] != '\0') {
            LOG_ERROR("Vertex KS CSV '%s' line %d: invalid keystream spec (%s)\n", path, row_count, err);
        } else {
            LOG_ERROR("Vertex KS CSV '%s' line %d: invalid keystream spec\n", path, row_count);
        }
        return -1;
    }

    int idx = -1;
    if (vertex_ks_find_or_add_index(tmp, key, path, row_count, &idx) != 0) {
        return -1;
    }

    tmp->key[idx] = key;
    tmp->mod[idx] = parsed_mod;
    tmp->frame_mode[idx] = parsed_frame_mode;
    tmp->frame_off[idx] = parsed_frame_off;
    tmp->frame_step[idx] = parsed_frame_step;
    DSD_MEMSET(tmp->bits[idx], 0, sizeof(tmp->bits[idx]));
    DSD_MEMCPY(tmp->bits[idx], parsed_bits, sizeof(parsed_bits));
    return 0;
}

static void
vertex_ks_apply_to_state(dsd_state* state, const vertex_map_tmp_t* tmp, const char* path) {
    DSD_MEMSET(state->vertex_ks_key, 0, sizeof(state->vertex_ks_key));
    DSD_MEMSET(state->vertex_ks_bits, 0, sizeof(state->vertex_ks_bits));
    DSD_MEMSET(state->vertex_ks_mod, 0, sizeof(state->vertex_ks_mod));
    DSD_MEMSET(state->vertex_ks_frame_mode, 0, sizeof(state->vertex_ks_frame_mode));
    DSD_MEMSET(state->vertex_ks_frame_off, 0, sizeof(state->vertex_ks_frame_off));
    DSD_MEMSET(state->vertex_ks_frame_step, 0, sizeof(state->vertex_ks_frame_step));
    state->vertex_ks_count = tmp->count;
    DSD_MEMCPY(state->vertex_ks_key, tmp->key, sizeof(state->vertex_ks_key));
    DSD_MEMCPY(state->vertex_ks_bits, tmp->bits, sizeof(state->vertex_ks_bits));
    DSD_MEMCPY(state->vertex_ks_mod, tmp->mod, sizeof(state->vertex_ks_mod));
    DSD_MEMCPY(state->vertex_ks_frame_mode, tmp->frame_mode, sizeof(state->vertex_ks_frame_mode));
    DSD_MEMCPY(state->vertex_ks_frame_off, tmp->frame_off, sizeof(state->vertex_ks_frame_off));
    DSD_MEMCPY(state->vertex_ks_frame_step, tmp->frame_step, sizeof(state->vertex_ks_frame_step));
    state->vertex_ks_active_idx[0] = -1;
    state->vertex_ks_active_idx[1] = -1;
    state->vertex_ks_counter[0] = 0;
    state->vertex_ks_counter[1] = 0;
    state->vertex_ks_warned[0] = 0;
    state->vertex_ks_warned[1] = 0;
    LOG_NOTICE("Loaded %d Vertex key->keystream mappings from '%s'.\n", tmp->count, path);
}

//Hex Variant of Key Import
int
csvKeyImportHex(const dsd_opts* opts, dsd_state* state) //key import for hex keys
{
    if (!opts || !state || opts->key_in_file[0] == '\0') {
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";
    char buffer[BSIZE];
    FILE* fp = csv_open_user_read_file("key file", opts->key_in_file, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }
    int row_count = 0;

    while (fgets(buffer, BSIZE, fp)) {
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        unsigned long long keynumber = csv_key_import_hex_parse_row(state, buffer);
        size_t key_index = 0;
        if (csv_rkey_index(keynumber, 0ULL, &key_index)) {
            LOG_INFO("Key [%04llX] loaded: %s", keynumber, DSD_SECRET_REDACTED);
            // If longer key is loaded (or clash with the 0x101, 0x201, 0x301 offset), then print the full key listing.
            csv_key_import_hex_log_offsets(state, keynumber);
        } else {
            LOG_INFO("Key [%04llX] [out-of-range]", keynumber);
        }

        LOG_INFO("\n");
    }
    fclose(fp);
    return 0;
}

int
csvVertexKsImport(dsd_state* state, const char* path) {
    if (state == NULL || path == NULL || path[0] == '\0') {
        LOG_ERROR("Vertex KS CSV path is missing.\n");
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";
    FILE* fp = csv_open_user_read_file("Vertex KS mapping file", path, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }

    vertex_map_tmp_t* tmp = (vertex_map_tmp_t*)calloc(1, sizeof(*tmp));
    if (tmp == NULL) {
        fclose(fp);
        LOG_ERROR("Out of memory while importing Vertex KS map.\n");
        return -1;
    }

    char buffer[BSIZE];
    int row_count = 0;
    int rc = 0;

    while (fgets(buffer, BSIZE, fp) != NULL) {
        row_count++;
        if (row_count == 1) {
            continue; //header
        }

        trim_eol(buffer);
        char* line = trim_ws(buffer);
        if (line == NULL || line[0] == '\0') {
            continue;
        }
        if (vertex_ks_parse_row(filename, row_count, line, tmp) != 0) {
            rc = -1;
            break;
        }
    }

    fclose(fp);

    if (rc == 0 && tmp->count == 0) {
        LOG_ERROR("Vertex KS CSV '%s' contains no mappings.\n", filename);
        rc = -1;
    }

    if (rc == 0) {
        vertex_ks_apply_to_state(state, tmp, filename);
    }

    free(tmp);
    return rc;
}
