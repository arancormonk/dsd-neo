// SPDX-License-Identifier: ISC
#include <ctype.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
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

/*
 * A blank line is filler, not a data row. Editors and several exporters leave a
 * trailing empty line; counting it would make the dry-run validators report a
 * clean file as "N rows skipped" (and the group importer warn about it).
 */
static int
csv_line_is_blank(const char* s) {
    if (!s) {
        return 1;
    }
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; p++) {
        if (!is_ascii_space(*p)) {
            return 0;
        }
    }
    return 1;
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

    uint64_t parsed = 0U;
    if (dsd_parse_hex_u64_n(token, (size_t)(end - (const unsigned char*)token), &parsed) != 0) {
        return 0;
    }
    *out = (unsigned long long)parsed;
    return 1;
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

static size_t
csv_split_preserve_empty(char* line, char** fields, size_t max_fields) {
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
csv_ascii_casecmp(const char* a, const char* b) {
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
    if (csv_ascii_casecmp(p, "true") == 0 || csv_ascii_casecmp(p, "yes") == 0 || csv_ascii_casecmp(p, "on") == 0
        || strcmp(p, "1") == 0) {
        *out = 1;
        return GROUP_PARSE_VALUE_OK;
    }
    if (csv_ascii_casecmp(p, "false") == 0 || csv_ascii_casecmp(p, "no") == 0 || csv_ascii_casecmp(p, "off") == 0
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
    size_t field_count = csv_split_preserve_empty(header_line, fields, sizeof(fields) / sizeof(fields[0]));
    size_t expected_idx = 0;

    if (field_count < 4) {
        return info;
    }
    if (csv_ascii_casecmp(trim_ws(fields[3]), "priority") != 0) {
        return info;
    }
    info.policy_active = 1;
    info.prefix_len = 1;

    for (size_t i = 4; i < field_count && expected_idx < (sizeof(expected) / sizeof(expected[0])); i++) {
        const char* col = trim_ws(fields[i]);
        if (csv_ascii_casecmp(col, expected[expected_idx]) == 0) {
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
        LOG_WARN("WARNING: Group file '%s' row %u has invalid %s value '%s'; using default.\n", filename, row_count,
                 label, token);
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
            LOG_WARN("WARNING: Group file '%s' row %u has invalid priority '%s'; defaulting to 0.\n", filename,
                     row_count, fields[3]);
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
            LOG_WARN("WARNING: Group file '%s' row %u has invalid preempt value '%s'; defaulting to false.\n", filename,
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
            LOG_WARN("WARNING: Group file '%s' row %u has blocking mode with enabled media flags; forcing media off.\n",
                     filename, row_count);
        }
        entry->audio = 0u;
        entry->record = 0u;
        entry->stream = 0u;
        return;
    }
    if (entry->audio == 0u && ((has_record && entry->record) || (has_stream && entry->stream))) {
        LOG_WARN("WARNING: Group file '%s' row %u sets audio off with record/stream on; forcing record/stream off.\n",
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

static int
group_commit_entry(dsd_state* state, const dsd_tg_policy_entry* entry, int is_range, const char* filename,
                   unsigned int row_count, size_t* dropped_policy_alloc_rows) {
    int rc = 0;
    if (!state || !entry || !filename || !dropped_policy_alloc_rows) {
        return -1;
    }

    rc = is_range ? dsd_tg_policy_add_range_entry(state, entry) : dsd_tg_policy_append_exact(state, entry);
    if (rc == -1) {
        (*dropped_policy_alloc_rows)++;
        return rc;
    }
    if (rc == 1) {
        if (!is_range) {
            LOG_WARN("WARNING: Group file '%s' row %u has invalid exact entry and was skipped.\n", filename, row_count);
        } else {
            LOG_WARN("WARNING: Group file '%s' row %u has invalid range and was skipped.\n", filename, row_count);
        }
    }
    return rc;
}

/** @brief Parse and commit one group data row. @return 0 when the row loaded. */
static int
group_import_row(dsd_state* state, const char* filename, unsigned int row_count, char* buffer,
                 const group_policy_header* header, size_t* dropped_policy_alloc_rows) {
    char* fields[32];
    size_t field_count = 0;
    uint32_t id_start = 0;
    uint32_t id_end = 0;
    int is_range = 0;
    const char* mode_field = NULL;
    const char* name_field = NULL;
    dsd_tg_policy_entry entry;
    int mode_blocking = 0;

    field_count = csv_split_preserve_empty(buffer, fields, sizeof(fields) / sizeof(fields[0]));
    if (field_count < 3) {
        LOG_WARN("WARNING: Group file '%s' row %u missing required fields; skipping.\n", filename, row_count);
        return -1;
    }

    if (!group_parse_id_field(fields[0], &id_start, &id_end, &is_range)) {
        LOG_WARN("WARNING: Group file '%s' row %u has invalid id '%s'; skipping.\n", filename, row_count, fields[0]);
        return -1;
    }

    mode_field = trim_ws(fields[1]);
    name_field = fields[2];
    group_entry_init(&entry, id_start, id_end, is_range, mode_field, name_field, row_count, &mode_blocking);
    group_apply_policy_fields(header, filename, row_count, field_count, fields, &entry, mode_blocking);
    return group_commit_entry(state, &entry, is_range, filename, row_count, dropped_policy_alloc_rows);
}

/* stats may be NULL; when set, counts data rows so a dry run can report them. */
static int
group_import_path_stats(const char* group_file_path, dsd_state* state, dsd_csv_validation* stats) {
    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";
    char buffer[BSIZE];
    FILE* fp = NULL;
    unsigned int row_count = 0;
    size_t dropped_policy_alloc_rows = 0;
    group_policy_header header = {0, 0, 0};

    if (!group_file_path || group_file_path[0] == '\0' || !state) {
        return -1;
    }

    fp = csv_open_user_read_file("group file", group_file_path, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }

    while (fgets(buffer, BSIZE, fp)) {
        row_count++;
        trim_eol(buffer);

        if (row_count == 1) {
            char header_copy[BSIZE];
            DSD_SNPRINTF(header_copy, sizeof(header_copy), "%s", buffer);
            header = group_parse_policy_header(header_copy);
            if (header.policy_active && header.invalid_order) {
                LOG_WARN("WARNING: Group file '%s' header optional policy columns are out of order; ignoring "
                         "mismatched and later "
                         "optional columns.\n",
                         filename);
            }
            continue; //don't want labels
        }

        if (csv_line_is_blank(buffer)) {
            continue;
        }
        if (stats) {
            stats->total++;
        }
        if (group_import_row(state, filename, row_count, buffer, &header, &dropped_policy_alloc_rows) == 0 && stats) {
            stats->accepted++;
        }
    }

    if (dropped_policy_alloc_rows > 0) {
        LOG_WARN("WARNING: Group file '%s' skipped %zu rows due to policy allocation failure.\n", filename,
                 dropped_policy_alloc_rows);
    }

    fclose(fp);
    return 0;
}

int
csvGroupImportPath(const char* group_file_path, dsd_state* state) {
    return group_import_path_stats(group_file_path, state, NULL);
}

int
csvGroupImport(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    return csvGroupImportPath(opts->group_in_file, state);
}

/*
 * A channel map row is `channel,frequency_hz`, which is the same shape as a
 * decimal key row (`key_id,value`) -- nothing in the file distinguishes them,
 * and the header line is free text by convention, so it cannot either. Picking
 * a key list as a channel map used to load its values as frequencies: the
 * doc's own example row `2,70` became a channel at 70 Hz, which the trunking SM
 * would then try to tune.
 *
 * The range is what the front ends can actually reach, generously bounded: the
 * low end sits under any HF-capable SoapySDR device, the high end above the
 * 6 GHz ceiling of the widest-tuning ones (LimeSDR, B210, HackRF). It is not a
 * band plan -- it only rejects numbers that cannot be radio frequencies at all,
 * including the 0 a blank column parses to. Long is 32-bit on the MSVC presets,
 * so the comparison is done in long long or the upper bound would not fit.
 */
#define CSV_CHAN_FREQ_MIN_HZ 100000LL
#define CSV_CHAN_FREQ_MAX_HZ 6000000000LL

static int
csv_chan_freq_plausible(long int freq) {
    const long long hz = (long long)freq;
    return hz >= CSV_CHAN_FREQ_MIN_HZ && hz <= CSV_CHAN_FREQ_MAX_HZ;
}

static int
csv_chan_import_apply_field(dsd_state* state, int field_count, const char* field, long int* chan_number,
                            int* freq_parsed) {
    if (!state || !field || !chan_number) {
        return 0;
    }
    if (field_count == 0) {
        long int parsed_chan = 0;
        if (parse_dec_long_strict(field, &parsed_chan)) {
            *chan_number = parsed_chan;
        } else {
            *chan_number = -1;
        }
        return 0;
    }
    if (field_count != 1) {
        return 0;
    }

    if (*chan_number < 0 || *chan_number >= 0xFFFF) {
        return 0;
    }

    long int freq = 0;
    const int usable = parse_dec_long_strict(field, &freq) && csv_chan_freq_plausible(freq);
    if (usable) {
        dsd_state_set_trunk_chan_freq(state, (uint32_t)*chan_number, freq);
        if (freq_parsed) {
            *freq_parsed = 1;
        }
    }

    if (state->lcn_freq_count < 0) {
        return 0;
    }
    if (dsd_state_trunk_lcn_reserve(state, (size_t)state->lcn_freq_count + 1) != 0) {
        LOG_ERROR("channel map import out of memory\n");
        return -1;
    }

    // The LCN list is positional -- EDACS reads it in row order -- so a row the
    // map refused still has to take its slot, as a 0 every consumer reads as
    // "unknown". Dropping it would renumber every LCN below it. Slots past the
    // 26 embedded entries land in the heap tail via dsd_state_trunk_lcn_slot().
    *dsd_state_trunk_lcn_slot(state, state->lcn_freq_count) = usable ? freq : 0L;
    state->lcn_freq_count++; // keep tally of number of Frequencies imported
    return 0;
}

/* id_ok may be NULL; set when the key-id column actually parsed as decimal. */
static unsigned long long
csv_key_import_dec_normalize_keynumber(const char* field, int* id_ok) {
    unsigned long long keynumber = 0;
    if (!parse_dec_u64_strict(field, &keynumber)) {
        return 0;
    }
    if (id_ok) {
        *id_ok = 1;
    }

    if (keynumber <= 0xFFFFULL) {
        return keynumber;
    }

    uint8_t hash_bits[24];
    keynumber &= 0xFFFFFFULL; // truncate to 24-bits (max allowed)
    for (int i = 0; i < 24; i++) {
        hash_bits[i] = (uint8_t)(((keynumber << i) & 0x800000ULL) >> 23); // load into array for CRC16
    }
    const uint16_t hash = dsd_crc_ccitt16_bits(hash_bits, 24U);
    LOG_INFO("Hashed ");
    return hash & 0xFFFFULL; // make sure its no larger than 16-bits
}

static int
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
        return parsed_keyvalue ? 1 : 0;
    }
    return 0;
}

static void
csv_key_import_dec_apply_field(dsd_state* state, int field_count, const char* field, unsigned long long* keynumber,
                               int* id_ok, int* stored) {
    if (field_count == 0) {
        *keynumber = csv_key_import_dec_normalize_keynumber(field, id_ok);
        return;
    }
    if (field_count == 1) {
        if (csv_key_import_dec_store_value(state, *keynumber, field) && stored) {
            *stored = 1;
        }
    }
}

/*
 * The channel map's columns past the frequency have always been free-text notes
 * -- two shipped examples put commas in theirs -- so column 3 is a channel name
 * only when the header line says `name`. The per-row key columns opt in the
 * same way, by header name at any field index >= 2; a duplicated key header
 * rejects the file.
 */
typedef struct {
    int has_name;
    int keys_hex_idx;
    int keys_dec_idx;
} chan_header_cols;

static const char*
chan_key_cell(char** fields, size_t field_count, int idx) {
    if (idx < 0 || (size_t)idx >= field_count || !fields[idx]) {
        return NULL;
    }
    return trim_ws(fields[idx]);
}

static int
chan_resolve_key_cell(const char* base_path, const char* cell, char* out, size_t out_sz, int row_number) {
    if (!cell || cell[0] == '\0') {
        return 0;
    }
    if (dsd_path_resolve_relative_to_file(base_path, cell, out, out_sz) != 0) {
        char row_text[32] = "?";
        (void)DSD_SNPRINTF(row_text, sizeof(row_text), "%d", row_number);
        LOG_ERROR("channel map file '%s' row %s: key path is too long or invalid\n", base_path, row_text);
        return -1;
    }
    return 0;
}

static int
chan_parse_header(char* header_line, chan_header_cols* out) {
    char* fields[16];
    out->has_name = 0;
    out->keys_hex_idx = -1;
    out->keys_dec_idx = -1;
    const size_t field_count = csv_split_preserve_empty(header_line, fields, sizeof(fields) / sizeof(fields[0]));
    if (field_count >= 3 && csv_ascii_casecmp(trim_ws(fields[2]), "name") == 0) {
        out->has_name = 1;
    }
    for (size_t i = 2; i < field_count; i++) {
        const char* name = trim_ws(fields[i]);
        if (csv_ascii_casecmp(name, "keys_hex_csv") == 0) {
            if (out->keys_hex_idx >= 0) {
                LOG_ERROR("channel map header duplicates 'keys_hex_csv'\n");
                return -1;
            }
            out->keys_hex_idx = (int)i;
        } else if (csv_ascii_casecmp(name, "keys_dec_csv") == 0) {
            if (out->keys_dec_idx >= 0) {
                LOG_ERROR("channel map header duplicates 'keys_dec_csv'\n");
                return -1;
            }
            out->keys_dec_idx = (int)i;
        }
    }
    return 0;
}

/**
 * @brief Load the key files named by a row's key cells into that row's key set.
 *
 * Paths resolve against the map file, so a map and its key files relocate as a
 * unit. Key paths cannot contain commas: the splitter does no quote handling.
 *
 * @return 0 when the row named no key file or its keys loaded, -1 on an unusable
 *         path, a load failure or allocation failure.
 */
static int
chan_import_row_keys(dsd_state* state, char** fields, size_t field_count, const chan_header_cols* cols,
                     const char* base_path, int row_number, int show_keys, size_t slot) {
    const char* hex_cell = chan_key_cell(fields, field_count, cols->keys_hex_idx);
    const char* dec_cell = chan_key_cell(fields, field_count, cols->keys_dec_idx);
    const int have_hex = (hex_cell != NULL && hex_cell[0] != '\0');
    const int have_dec = (dec_cell != NULL && dec_cell[0] != '\0');
    if (!have_hex && !have_dec) {
        return 0;
    }
    char hex_path[CSV_IMPORT_PATH_MAX] = "";
    char dec_path[CSV_IMPORT_PATH_MAX] = "";
    if (chan_resolve_key_cell(base_path, hex_cell, hex_path, sizeof(hex_path), row_number) != 0
        || chan_resolve_key_cell(base_path, dec_cell, dec_path, sizeof(dec_path), row_number) != 0) {
        return -1;
    }
    dsd_key_set ks;
    DSD_MEMSET(&ks, 0, sizeof(ks));
    if (dsd_key_set_load_csv(&ks, have_hex ? hex_path : NULL, have_dec ? dec_path : NULL, show_keys) != 0) {
        char row_text[32] = "?";
        (void)DSD_SNPRINTF(row_text, sizeof(row_text), "%d", row_number);
        LOG_ERROR("channel map file '%s' row %s: failed to load row key file\n", base_path, row_text);
        return -1;
    }
    if (dsd_state_trunk_lcn_keys_set(state, slot, &ks) != 0) {
        dsd_key_set_free(&ks);
        LOG_ERROR("channel map import out of memory\n");
        return -1;
    }
    return 0;
}

/**
 * @brief Parse one channel row into @p state.
 *
 * Empty fields are preserved rather than collapsed, so `1,,851000000` reads as a
 * blank frequency and is skipped instead of promoting column 3 into its place.
 *
 * When the header opted into per-row keys, a non-blank key cell on a row that
 * took a slot resolves its path against the map file and loads it into the
 * row's key set. Key paths cannot contain commas: the splitter does no quote
 * handling. A load failure fails the whole import, like a bad `-K`.
 *
 * @return 1 when a frequency loaded, -1 on allocation failure or key load failure.
 */
static int
chan_import_row(dsd_state* state, char* buffer, const chan_header_cols* cols, const char* base_path, int row_number,
                int show_keys, int* out_field_count, long int* out_chan_number) {
    char* fields[16];
    int freq_parsed = 0;
    long int chan_number = -1;
    // The row's scan-list slot is the count before it: every row that takes one
    // appends, so the name index and the slot index stay equal by construction.
    const int lcn_before = state->lcn_freq_count;

    const size_t field_count = csv_split_preserve_empty(buffer, fields, sizeof(fields) / sizeof(fields[0]));
    for (size_t i = 0; i < field_count && i < 2; i++) {
        if (csv_chan_import_apply_field(state, (int)i, fields[i], &chan_number, &freq_parsed) != 0) {
            return -1;
        }
    }
    if (cols->has_name && field_count >= 3 && state->lcn_freq_count > lcn_before) {
        if (dsd_state_trunk_lcn_name_set(state, (size_t)lcn_before, fields[2]) != 0) {
            LOG_ERROR("channel map import out of memory\n");
            return -1;
        }
    }
    if ((cols->keys_hex_idx >= 0 || cols->keys_dec_idx >= 0) && state->lcn_freq_count > lcn_before) {
        if (chan_import_row_keys(state, fields, field_count, cols, base_path, row_number, show_keys, (size_t)lcn_before)
            != 0) {
            return -1;
        }
    }
    *out_field_count = (int)field_count;
    *out_chan_number = chan_number;
    return freq_parsed;
}

/*
 * Report one parsed channel row: a dry run only wants the counters, so it never
 * echoes rows through the process-global logger for an import that never happened.
 */
static void
chan_import_row_report(const dsd_state* state, dsd_csv_validation* stats, const char* filename, int row_count,
                       int freq_parsed, int field_count, long int chan_number) {
    if (stats) {
        stats->total++;
        stats->accepted += (freq_parsed ? 1U : 0U);
        return;
    }
    if (!freq_parsed) {
        // Say so rather than echoing the map slot, which still holds
        // whatever was there before this row failed to land.
        LOG_WARN("WARNING: Channel map file '%s' row %d has no usable frequency; skipping.\n", filename, row_count);
        return;
    }
    if (field_count >= 2 && chan_number >= 0 && chan_number < 0xFFFF) {
        LOG_INFO("Channel [%05ld] [%09ld]", chan_number, state->trunk_chan_map[chan_number]);
    }
    LOG_INFO("\n");
}

/* stats may be NULL; when set, counts data rows so a dry run can report them. */
static int
chan_import_stats(const char* chan_file_path, dsd_state* state, dsd_csv_validation* stats, int show_keys) {
    if (!chan_file_path || chan_file_path[0] == '\0' || !state) {
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";

    char buffer[BSIZE];
    FILE* fp = csv_open_user_read_file("channel map file", chan_file_path, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }
    int row_count = 0;
    chan_header_cols cols;
    DSD_MEMSET(&cols, 0, sizeof(cols));
    cols.keys_hex_idx = -1;
    cols.keys_dec_idx = -1;

    while (fgets(buffer, BSIZE, fp)) {
        int field_count = 0;
        long int chan_number = -1;
        row_count++;
        if (row_count == 1) {
            // Split in place: the header is not needed again, and the next fgets refills the buffer.
            if (chan_parse_header(buffer, &cols) != 0) {
                fclose(fp);
                return -1;
            }
            continue; //don't want labels
        }
        if (csv_line_is_blank(buffer)) {
            continue;
        }
        const int freq_parsed =
            chan_import_row(state, buffer, &cols, filename, row_count, show_keys, &field_count, &chan_number);
        if (freq_parsed < 0) {
            fclose(fp);
            return -1;
        }
        chan_import_row_report(state, stats, filename, row_count, freq_parsed, field_count, chan_number);
    }
    fclose(fp);
    return 0;
}

/* Dry-run entry for the validator: counts rows and never reveals key values. */
static int
chan_validate_run(const char* chan_file_path, dsd_state* state, dsd_csv_validation* stats) {
    return chan_import_stats(chan_file_path, state, stats, 0);
}

int
csvChanImport(const dsd_opts* opts, dsd_state* state) //channel map import
{
    if (!opts || !state) {
        return -1;
    }
    return chan_import_stats(opts->chan_in_file, state, NULL, opts->show_keys);
}

//Decimal Variant of Key Import
/* stats may be NULL; when set, counts data rows so a dry run can report them. */
static int
key_import_dec_stats(int show_keys, const char* key_file_path, dsd_state* state, dsd_csv_validation* stats) {
    if (!key_file_path || key_file_path[0] == '\0' || !state) {
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";

    char buffer[BSIZE];
    FILE* fp = csv_open_user_read_file("key file", key_file_path, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }
    int row_count = 0;

    while (fgets(buffer, BSIZE, fp)) {
        unsigned long long int keynumber = 0;
        int field_count = 0;
        int id_ok = 0;
        int stored = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        if (csv_line_is_blank(buffer)) {
            continue;
        }
        if (stats) {
            stats->total++;
        }
        char* saveptr = NULL;
        const char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
        while (field) {
            csv_key_import_dec_apply_field(state, field_count, field, &keynumber, &id_ok, &stored);
            field = dsd_strtok_r(NULL, ",", &saveptr);
            field_count++;
        }
        // An unparsed key-id column normalizes to 0, so the row would still
        // "store" — onto slot 0, together with every other broken row. Counting
        // that as loaded is how a hex-id file validates as "N keys".
        if (stats && stored && id_ok) {
            stats->accepted++;
        }
        if (stats) {
            continue; // dry run: the counters are the product, not the log
        }
        char key_id_text[32];
        (void)DSD_SNPRINTF(key_id_text, sizeof key_id_text, "%03lld", keynumber);
        char key_text[16];
        if (show_keys != 0) {
            LOG_INFO("Key [%s] [%s]", key_id_text,
                     dsd_secret_format_decimal(key_text, sizeof key_text, show_keys, state->rkey_array[keynumber], 5U));
        } else {
            LOG_INFO("Key [%s] loaded: %s", key_id_text,
                     dsd_secret_format_decimal(key_text, sizeof key_text, show_keys, state->rkey_array[keynumber], 5U));
        }
        LOG_INFO("\n");
    }
    fclose(fp);
    return 0;
}

int
csvKeyImportDec(const dsd_opts* opts, dsd_state* state) //multi-key support
{
    if (!opts || !state) {
        return -1;
    }
    return key_import_dec_stats(opts->show_keys, opts->key_in_file, state, NULL);
}

int
csvKeyImportDecPath(const char* path, int show_keys, dsd_state* state, dsd_csv_validation* stats) {
    return key_import_dec_stats(show_keys, path, state, stats);
}

static int
csv_key_import_hex_store_value(dsd_state* state, unsigned long long keynumber, unsigned long long offset,
                               const char* field) {
    if (!state || !field) {
        return 0;
    }
    size_t idx = 0;
    if (!csv_rkey_index(keynumber, offset, &idx)) {
        return 0;
    }
    unsigned long long v = 0;
    if (parse_hex_u64_strict(field, &v)) {
        state->rkey_array[idx] = v;
        state->rkey_array_loaded[idx] = 1U;
        return 1;
    }
    return 0;
}

static void
csv_key_import_hex_log_offsets(const dsd_state* state, unsigned long long keynumber, int show_keys) {
    static const unsigned long long offsets[] = {0x101ULL, 0x201ULL, 0x301ULL};
    unsigned long long values[3] = {0ULL, 0ULL, 0ULL};
    unsigned char loaded[3] = {0U, 0U, 0U};
    int any_loaded = 0;

    for (size_t i = 0U; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        size_t idx = 0U;
        if (csv_rkey_index(keynumber, offsets[i], &idx) && state->rkey_array_loaded[idx] != 0U) {
            values[i] = state->rkey_array[idx];
            loaded[i] = 1U;
            any_loaded = 1;
        }
    }

    if (any_loaded == 0) {
        return;
    }

    if (show_keys == 0) {
        char seg[17];
        LOG_INFO(" [additional key segments loaded: %s]",
                 dsd_secret_format_hex(seg, sizeof seg, show_keys, 0ULL, 16U, 0));
        return;
    }

    for (size_t i = 0U; i < sizeof(values) / sizeof(values[0]); i++) {
        if (loaded[i] != 0U) {
            char seg[17];
            LOG_INFO(" [%s]", dsd_secret_format_hex(seg, sizeof seg, show_keys, values[i], 16U, 0));
        }
    }
}

static unsigned long long
csv_key_import_hex_parse_row(dsd_state* state, char* buffer, int* stored_segments) {
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
            if (csv_key_import_hex_store_value(state, keynumber, offsets[field_count - 1], field) && stored_segments) {
                (*stored_segments)++;
            }
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
            LOG_WARN("WARNING: Vertex KS CSV '%s' line %d: duplicate key, replacing previous mapping.\n", path,
                     row_count);
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
vertex_ks_parse_row(const char* path, int row_count, char* line, void* ctx) {
    vertex_map_tmp_t* tmp = (vertex_map_tmp_t*)ctx;
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
    LOG_INFO("NOTICE: Loaded %d Vertex key->keystream mappings from '%s'.\n", tmp->count, path);
}

//Hex Variant of Key Import
/* stats may be NULL; when set, counts data rows so a dry run can report them. */
static int
key_import_hex_stats(int show_keys, const char* key_file_path, dsd_state* state, dsd_csv_validation* stats) {
    if (!key_file_path || key_file_path[0] == '\0' || !state) {
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";
    char buffer[BSIZE];
    FILE* fp = csv_open_user_read_file("key file", key_file_path, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }
    int row_count = 0;

    while (fgets(buffer, BSIZE, fp)) {
        int stored_segments = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        if (csv_line_is_blank(buffer)) {
            continue;
        }
        if (stats) {
            stats->total++;
        }
        unsigned long long keynumber = csv_key_import_hex_parse_row(state, buffer, &stored_segments);
        if (stats && stored_segments > 0) {
            stats->accepted++;
        }
        if (stats) {
            continue; // dry run: the counters are the product, not the log
        }
        size_t key_index = 0;
        if (csv_rkey_index(keynumber, 0ULL, &key_index)) {
            char key_id_text[32];
            (void)DSD_SNPRINTF(key_id_text, sizeof key_id_text, "%04llX", keynumber);
            char key_text[17];
            if (show_keys != 0) {
                LOG_INFO(
                    "Key [%s] [%s]", key_id_text,
                    dsd_secret_format_hex(key_text, sizeof key_text, show_keys, state->rkey_array[key_index], 16U, 0));
            } else {
                LOG_INFO(
                    "Key [%s] loaded: %s", key_id_text,
                    dsd_secret_format_hex(key_text, sizeof key_text, show_keys, state->rkey_array[key_index], 16U, 0));
            }
            // If longer key is loaded (or clash with the 0x101, 0x201, 0x301 offset), then print the full key listing.
            csv_key_import_hex_log_offsets(state, keynumber, show_keys);
        } else {
            char key_id_text[32];
            (void)DSD_SNPRINTF(key_id_text, sizeof key_id_text, "%04llX", keynumber);
            LOG_INFO("Key [%s] [out-of-range]", key_id_text);
        }

        LOG_INFO("\n");
    }
    fclose(fp);
    return 0;
}

int
csvKeyImportHex(const dsd_opts* opts, dsd_state* state) //key import for hex keys
{
    if (!opts || !state) {
        return -1;
    }
    return key_import_hex_stats(opts->show_keys, opts->key_in_file, state, NULL);
}

int
csvKeyImportHexPath(const char* path, int show_keys, dsd_state* state, dsd_csv_validation* stats) {
    return key_import_hex_stats(show_keys, path, state, stats);
}

typedef int (*csv_validate_run_fn)(const char* path, dsd_state* state, dsd_csv_validation* out);

/*
 * Dry-run harness: parses through the real import loop, but into throwaway
 * heap state (dsd_state is multi-megabyte, and the group import allocates a
 * TG-policy state extension that must be freed) so no live state is touched.
 */
static int
csv_validate_into_throwaway(const char* path, dsd_csv_validation* out, csv_validate_run_fn run) {
    if (!path || path[0] == '\0' || !out || !run) {
        return -1;
    }
    out->accepted = 0U;
    out->skipped = 0U;
    out->total = 0U;

    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    int rc = -1;
    if (state) {
        rc = run(path, state, out);
        dsd_state_ext_free_all(state);
    }
    dsd_state_trunk_lcn_free(state);
    free(state);

    if (rc != 0) {
        out->accepted = 0U;
        out->skipped = 0U;
        out->total = 0U;
        return -1;
    }
    out->skipped = out->total - out->accepted;
    return 0;
}

static int
csv_validate_run_key_dec(const char* path, dsd_state* state, dsd_csv_validation* out) {
    return key_import_dec_stats(0, path, state, out);
}

static int
csv_validate_run_key_hex(const char* path, dsd_state* state, dsd_csv_validation* out) {
    return key_import_hex_stats(0, path, state, out);
}

int
dsd_csv_validate_group_file(const char* path, dsd_csv_validation* out) {
    return csv_validate_into_throwaway(path, out, group_import_path_stats);
}

int
dsd_csv_validate_chan_file(const char* path, dsd_csv_validation* out) {
    return csv_validate_into_throwaway(path, out, chan_validate_run);
}

int
dsd_csv_validate_key_file_dec(const char* path, dsd_csv_validation* out) {
    return csv_validate_into_throwaway(path, out, csv_validate_run_key_dec);
}

int
dsd_csv_validate_key_file_hex(const char* path, dsd_csv_validation* out) {
    return csv_validate_into_throwaway(path, out, csv_validate_run_key_hex);
}

typedef struct {
    uint32_t tg[DSD_DMR_TG_KEY_MAP_MAX];
    uint8_t kid[DSD_DMR_TG_KEY_MAP_MAX];
    int count;
} dmr_tg_key_tmp_t;

static int
dmr_tg_key_parse_row(const char* path, int row_count, char* line, void* ctx) {
    dmr_tg_key_tmp_t* tmp = (dmr_tg_key_tmp_t*)ctx;
    char* saveptr = NULL;
    char* tg_tok = dsd_strtok_r(line, ",", &saveptr);
    char* kid_tok = dsd_strtok_r(NULL, ",", &saveptr);
    if (tg_tok == NULL || kid_tok == NULL) {
        LOG_ERROR("DMR TG key ID map CSV '%s' line %d: expected tg_dec,keyid_hex\n", path, row_count);
        return -1;
    }

    tg_tok = trim_ws(tg_tok);
    kid_tok = trim_ws(kid_tok);
    if (tg_tok == NULL || tg_tok[0] == '\0' || kid_tok == NULL || kid_tok[0] == '\0') {
        LOG_ERROR("DMR TG key ID map CSV '%s' line %d: empty talkgroup or key id field\n", path, row_count);
        return -1;
    }

    unsigned long long tg = 0ULL;
    if (!parse_dec_u64_strict(tg_tok, &tg) || tg == 0ULL || tg > 0xFFFFFFULL) {
        LOG_ERROR("DMR TG key ID map CSV '%s' line %d: invalid talkgroup (expected decimal 1..16777215)\n", path,
                  row_count);
        return -1;
    }

    // DMR signals an 8-bit key id, so the mapped replacement is held to the same range.
    unsigned long long kid = 0ULL;
    if (parse_hex_u64_strict(kid_tok, &kid) != 1 || kid > 0xFFULL) {
        LOG_ERROR("DMR TG key ID map CSV '%s' line %d: invalid key id (expected hex 00..FF)\n", path, row_count);
        return -1;
    }

    for (int i = 0; i < tmp->count; i++) {
        if (tmp->tg[i] == (uint32_t)tg) {
            LOG_WARN("WARNING: DMR TG key ID map CSV '%s' line %d: duplicate talkgroup, replacing previous mapping.\n",
                     path, row_count);
            tmp->kid[i] = (uint8_t)kid;
            return 0;
        }
    }
    if (tmp->count >= DSD_DMR_TG_KEY_MAP_MAX) {
        LOG_ERROR("DMR TG key ID map CSV '%s' exceeds capacity (%d rows max)\n", path, DSD_DMR_TG_KEY_MAP_MAX);
        return -1;
    }
    tmp->tg[tmp->count] = (uint32_t)tg;
    tmp->kid[tmp->count] = (uint8_t)kid;
    tmp->count++;
    return 0;
}

static void
dmr_tg_key_apply_to_state(dsd_state* state, const dmr_tg_key_tmp_t* tmp, const char* path) {
    keyring_dmr_tg_map_reset(state);
    state->dmr_tg_key_map_count = tmp->count;
    // tmp is zeroed before parsing, so the full-array copies also clear the unused tail.
    DSD_MEMCPY(state->dmr_tg_key_map_tg, tmp->tg, sizeof(state->dmr_tg_key_map_tg));
    DSD_MEMCPY(state->dmr_tg_key_map_kid, tmp->kid, sizeof(state->dmr_tg_key_map_kid));
    LOG_INFO("NOTICE: Loaded %d DMR talkgroup->key ID mappings from '%s'.\n", tmp->count, path);
}

// Shared skeleton of the mapping-CSV importers (Vertex KS, DMR TG -> key ID): open, skip the
// header row, trim, hand each non-empty line to parse_row, and refuse the whole file on the first
// bad row or on an empty one -- the caller applies only after a clean return, so a malformed file
// never half-mutates live state. `csv_label` names the file kind in diagnostics ("Vertex KS CSV");
// `open_label` is what csv_open_user_read_file() reports when the path cannot be opened.
// Returns 0 with at least one row parsed and `filename` set, -1 otherwise.
static int
csv_mapping_import_rows(const char* csv_label, const char* open_label, const char* path, char* filename,
                        size_t filename_size, int (*parse_row)(const char* path, int row_count, char* line, void* ctx),
                        void* ctx) {
    FILE* fp = csv_open_user_read_file(open_label, path, filename, filename_size);
    if (fp == NULL) {
        return -1;
    }

    char buffer[BSIZE];
    int row_count = 0;
    int rows = 0;
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
        if (parse_row(filename, row_count, line, ctx) != 0) {
            rc = -1;
            break;
        }
        rows++;
    }

    fclose(fp);

    if (rc == 0 && rows == 0) {
        LOG_ERROR("%s '%s' contains no mappings.\n", csv_label, filename);
        rc = -1;
    }
    return rc;
}

int
csvDmrTgKeyImport(dsd_state* state, const char* path) {
    if (state == NULL || path == NULL || path[0] == '\0') {
        LOG_ERROR("DMR TG key ID map CSV path is missing.\n");
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";
    dmr_tg_key_tmp_t tmp;
    DSD_MEMSET(&tmp, 0, sizeof tmp);

    const int rc = csv_mapping_import_rows("DMR TG key ID map CSV", "DMR TG key ID mapping file", path, filename,
                                           sizeof filename, dmr_tg_key_parse_row, &tmp);
    if (rc == 0) {
        dmr_tg_key_apply_to_state(state, &tmp, filename);
    }
    return rc;
}

int
csvVertexKsImport(dsd_state* state, const char* path) {
    if (state == NULL || path == NULL || path[0] == '\0') {
        LOG_ERROR("Vertex KS CSV path is missing.\n");
        return -1;
    }

    vertex_map_tmp_t* tmp = (vertex_map_tmp_t*)calloc(1, sizeof(*tmp));
    if (tmp == NULL) {
        LOG_ERROR("Out of memory while importing Vertex KS map.\n");
        return -1;
    }

    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";
    const int rc = csv_mapping_import_rows("Vertex KS CSV", "Vertex KS mapping file", path, filename, sizeof filename,
                                           vertex_ks_parse_row, tmp);
    if (rc == 0) {
        vertex_ks_apply_to_state(state, tmp, filename);
    }

    free(tmp);
    return rc;
}
