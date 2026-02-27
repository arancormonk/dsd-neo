// SPDX-License-Identifier: ISC
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#define BSIZE 999

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
parse_hex_u64_strict(const char* token, unsigned long long* out) {
    if (token == NULL || out == NULL) {
        return 0;
    }

    while (*token != '\0' && is_ascii_space((unsigned char)*token)) {
        token++;
    }
    if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        token += 2;
    }
    if (token[0] == '\0') {
        return 0;
    }

    unsigned long long v = 0ULL;
    int digits = 0;
    for (const unsigned char* p = (const unsigned char*)token; *p != '\0'; p++) {
        if (is_ascii_space(*p)) {
            return 0;
        }
        int nib = -1;
        if (*p >= '0' && *p <= '9') {
            nib = (int)(*p - '0');
        } else if (*p >= 'a' && *p <= 'f') {
            nib = 10 + (int)(*p - 'a');
        } else if (*p >= 'A' && *p <= 'F') {
            nib = 10 + (int)(*p - 'A');
        } else {
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

int
csvGroupImport(dsd_opts* opts, dsd_state* state) {
    char filename[1024] = "filename.csv";
    snprintf(filename, sizeof filename, "%s", opts->group_in_file);
    //filename[1023] = '\0'; //necessary?
    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        LOG_ERROR("Unable to open group file '%s'\n", filename);
        return -1;
    }
    int row_count = 0;
    int field_count = 0;
    long int group_number = 0; //local group number for array index value
    UNUSED(group_number);
    const size_t group_cap = sizeof(state->group_array) / sizeof(state->group_array[0]);
    size_t dropped_rows = 0;
    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }

        if (state->group_tally >= group_cap) {
            dropped_rows++;
            continue;
        }

        size_t idx = state->group_tally;
        char* saveptr = NULL;
        char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
        while (field) {
            trim_eol(field);

            if (field_count == 0) {
                //group_number = atol(field);
                state->group_array[idx].groupNumber = atol(field);
                LOG_INFO("%ld, ", state->group_array[idx].groupNumber);
            }
            if (field_count == 1) {
                strncpy(state->group_array[idx].groupMode, field, sizeof(state->group_array[idx].groupMode) - 1);
                state->group_array[idx].groupMode[sizeof(state->group_array[idx].groupMode) - 1] = '\0';
                LOG_INFO("%s, ", state->group_array[idx].groupMode);
            }
            if (field_count == 2) {
                strncpy(state->group_array[idx].groupName, field, sizeof(state->group_array[idx].groupName) - 1);
                state->group_array[idx].groupName[sizeof(state->group_array[idx].groupName) - 1] = '\0';
                LOG_INFO("%s ", state->group_array[idx].groupName);
            }

            field = dsd_strtok_r(NULL, ",", &saveptr);
            field_count++;
        }
        LOG_INFO("\n");
        state->group_tally++;
    }

    if (dropped_rows > 0) {
        LOG_WARNING("Group file '%s' exceeded capacity (%zu entries); ignored %zu additional rows.\n", filename,
                    group_cap, dropped_rows);
    }

    fclose(fp);
    return 0;
}

//LCN import for EDACS, migrated to channel map (channel map does both)
int
csvLCNImport(dsd_opts* opts, dsd_state* state) {
    char filename[1024] = "filename.csv";
    snprintf(filename, sizeof filename, "%s", opts->lcn_in_file);
    //filename[1023] = '\0'; //necessary?
    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        LOG_ERROR("Unable to open lcn file '%s'\n", filename);
        return -1;
    }
    int row_count = 0;
    int field_count = 0;

    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* saveptr = NULL;
        char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
        while (field) {

            state->trunk_lcn_freq[field_count] = atol(field);
            state->lcn_freq_count++; //keep tally of number of Frequencies imported
            LOG_INFO("LCN [%d] [%ld]", field_count + 1, state->trunk_lcn_freq[field_count]);
            LOG_INFO("\n");

            field = dsd_strtok_r(NULL, ",", &saveptr);
            field_count++;
        }
        LOG_INFO("LCN Count %d\n", state->lcn_freq_count);
    }
    fclose(fp);
    return 0;
}

int
csvChanImport(dsd_opts* opts, dsd_state* state) //channel map import
{
    char filename[1024] = "filename.csv";
    snprintf(filename, sizeof filename, "%s", opts->chan_in_file);

    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        LOG_ERROR("Unable to open channel map file '%s'\n", filename);
        return -1;
    }
    int row_count = 0;
    int field_count = 0;

    long int chan_number = 0;

    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* saveptr = NULL;
        char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
        while (field) {

            if (field_count == 0) {
                sscanf(field, "%ld", &chan_number);
            }

            if (field_count == 1) {
                if (chan_number >= 0 && chan_number < 0xFFFF) {
                    sscanf(field, "%ld", &state->trunk_chan_map[chan_number]);
                }
                // adding this should be compatible with EDACS, test and obsolete the LCN Import function if desired
                if (state->lcn_freq_count >= 0
                    && state->lcn_freq_count
                           < (int)(sizeof(state->trunk_lcn_freq) / sizeof(state->trunk_lcn_freq[0]))) {
                    sscanf(field, "%ld", &state->trunk_lcn_freq[state->lcn_freq_count]);
                    state->lcn_freq_count++; // keep tally of number of Frequencies imported
                }
            }

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
csvKeyImportDec(dsd_opts* opts, dsd_state* state) //multi-key support
{
    char filename[1024] = "filename.csv";
    snprintf(filename, sizeof filename, "%s", opts->key_in_file);

    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        LOG_ERROR("Unable to open file '%s'\n", filename);
        return -1;
    }
    int row_count = 0;
    int field_count = 0;

    unsigned long long int keynumber = 0;
    unsigned long long int keyvalue = 0;

    uint16_t hash = 0;
    uint8_t hash_bits[24];
    memset(hash_bits, 0, sizeof(hash_bits));

    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* saveptr = NULL;
        char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
        while (field) {

            if (field_count == 0) {
                sscanf(field, "%llu", &keynumber);
                if (keynumber > 0xFFFF) //if larger than 16-bits, get its hash instead
                {
                    keynumber = keynumber & 0xFFFFFF; //truncate to 24-bits (max allowed)
                    for (int i = 0; i < 24; i++) {
                        hash_bits[i] = ((keynumber << i) & 0x800000) >> 23; //load into array for CRC16
                    }
                    hash = ComputeCrcCCITT16d(hash_bits, 24);
                    keynumber = hash & 0xFFFF; //make sure its no larger than 16-bits
                    LOG_INFO("Hashed ");
                }
            }

            if (field_count == 1) {
                sscanf(field, "%llu", &keyvalue);
                if (keynumber < 0x1FFFFULL) {
                    state->rkey_array[keynumber] = keyvalue & 0xFFFFFFFFFF; // doesn't exceed 40-bit value
                }
            }

            field = dsd_strtok_r(NULL, ",", &saveptr);
            field_count++;
        }
        LOG_INFO("Key [%03lld] [%05lld]", keynumber, state->rkey_array[keynumber]);
        LOG_INFO("\n");
    }
    fclose(fp);
    return 0;
}

//Hex Variant of Key Import
int
csvKeyImportHex(dsd_opts* opts, dsd_state* state) //key import for hex keys
{
    char filename[1024] = "filename.csv";
    snprintf(filename, sizeof filename, "%s", opts->key_in_file);
    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        LOG_ERROR("Unable to open file '%s'\n", filename);
        return -1;
    }
    int row_count = 0;
    int field_count = 0;
    unsigned long long int keynumber = 0;

    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* saveptr = NULL;
        char* field = dsd_strtok_r(buffer, ",", &saveptr); //seperate by comma
        while (field) {

            if (field_count == 0) {
                sscanf(field, "%llX", &keynumber);
            }

            if (field_count == 1) {
                if (keynumber < 0x1FFFFULL) {
                    sscanf(field, "%llX", &state->rkey_array[keynumber]);
                }
            }

            //this could also theoretically nuke other keys that are at the same offset
            if (field_count == 2) {
                if (keynumber + 0x101 < 0x1FFFF) {
                    sscanf(field, "%llX", &state->rkey_array[keynumber + 0x101]);
                }
            }

            if (field_count == 3) {
                if (keynumber + 0x201 < 0x1FFFF) {
                    sscanf(field, "%llX", &state->rkey_array[keynumber + 0x201]);
                }
            }

            if (field_count == 4) {
                if (keynumber + 0x301 < 0x1FFFF) {
                    sscanf(field, "%llX", &state->rkey_array[keynumber + 0x301]);
                }
            }

            field = dsd_strtok_r(NULL, ",", &saveptr);
            field_count++;
        }

        if (keynumber < 0x1FFFFULL) {
            LOG_INFO("Key [%04llX] [%016llX]", keynumber, state->rkey_array[keynumber]);
        } else {
            LOG_INFO("Key [%04llX] [out-of-range]", keynumber);
        }

        // If longer key is loaded (or clash with the 0x101, 0x201, 0x301 offset), then print the full key listing.
        if (keynumber < 0x1FFFFULL) {
            unsigned long long out1 = 0, out2 = 0, out3 = 0;
            unsigned long long idx1 = keynumber + 0x101ULL;
            unsigned long long idx2 = keynumber + 0x201ULL;
            unsigned long long idx3 = keynumber + 0x301ULL;
            if (idx1 < 0x1FFFFULL) {
                out1 = state->rkey_array[idx1];
            }
            if (idx2 < 0x1FFFFULL) {
                out2 = state->rkey_array[idx2];
            }
            if (idx3 < 0x1FFFFULL) {
                out3 = state->rkey_array[idx3];
            }
            if (out1 != 0 || out2 != 0 || out3 != 0) {
                LOG_INFO(" [%016llX] [%016llX] [%016llX]", out1, out2, out3);
            }
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

    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        LOG_ERROR("Unable to open Vertex KS mapping file '%s'\n", path);
        return -1;
    }

    typedef struct {
        unsigned long long key[DSD_VERTEX_KS_MAP_MAX];
        uint8_t bits[DSD_VERTEX_KS_MAP_MAX][882];
        int mod[DSD_VERTEX_KS_MAP_MAX];
        int frame_mode[DSD_VERTEX_KS_MAP_MAX];
        int frame_off[DSD_VERTEX_KS_MAP_MAX];
        int frame_step[DSD_VERTEX_KS_MAP_MAX];
        int count;
    } vertex_map_tmp_t;

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

        char* saveptr = NULL;
        char* key_tok = dsd_strtok_r(line, ",", &saveptr);
        char* ks_tok = dsd_strtok_r(NULL, ",", &saveptr);
        if (key_tok == NULL || ks_tok == NULL) {
            LOG_ERROR("Vertex KS CSV '%s' line %d: expected key_hex,keystream_spec\n", path, row_count);
            rc = -1;
            break;
        }

        key_tok = trim_ws(key_tok);
        ks_tok = trim_ws(ks_tok);
        if (key_tok == NULL || key_tok[0] == '\0' || ks_tok == NULL || ks_tok[0] == '\0') {
            LOG_ERROR("Vertex KS CSV '%s' line %d: empty key or keystream field\n", path, row_count);
            rc = -1;
            break;
        }

        unsigned long long key = 0ULL;
        if (parse_hex_u64_strict(key_tok, &key) != 1) {
            LOG_ERROR("Vertex KS CSV '%s' line %d: invalid key '%s' (expected hex)\n", path, row_count, key_tok);
            rc = -1;
            break;
        }

        uint8_t parsed_bits[882];
        int parsed_mod = 0;
        int parsed_frame_mode = 0;
        int parsed_frame_off = 0;
        int parsed_frame_step = 0;
        char err[128];
        if (dmr_parse_static_keystream_spec(ks_tok, parsed_bits, &parsed_mod, &parsed_frame_mode, &parsed_frame_off,
                                            &parsed_frame_step, err, sizeof err)
            != 1) {
            if (err[0] != '\0') {
                LOG_ERROR("Vertex KS CSV '%s' line %d: invalid keystream spec '%s' (%s)\n", path, row_count, ks_tok,
                          err);
            } else {
                LOG_ERROR("Vertex KS CSV '%s' line %d: invalid keystream spec '%s'\n", path, row_count, ks_tok);
            }
            rc = -1;
            break;
        }

        int idx = -1;
        for (int i = 0; i < tmp->count; i++) {
            if (tmp->key[i] == key) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            if (tmp->count >= DSD_VERTEX_KS_MAP_MAX) {
                LOG_ERROR("Vertex KS CSV '%s' exceeds capacity (%d rows max)\n", path, DSD_VERTEX_KS_MAP_MAX);
                rc = -1;
                break;
            }
            idx = tmp->count++;
        } else {
            LOG_WARNING("Vertex KS CSV '%s' line %d: duplicate key 0x%llX, replacing previous mapping.\n", path,
                        row_count, key);
        }

        tmp->key[idx] = key;
        tmp->mod[idx] = parsed_mod;
        tmp->frame_mode[idx] = parsed_frame_mode;
        tmp->frame_off[idx] = parsed_frame_off;
        tmp->frame_step[idx] = parsed_frame_step;
        memset(tmp->bits[idx], 0, sizeof(tmp->bits[idx]));
        memcpy(tmp->bits[idx], parsed_bits, sizeof(parsed_bits));
    }

    fclose(fp);

    if (rc == 0 && tmp->count == 0) {
        LOG_ERROR("Vertex KS CSV '%s' contains no mappings.\n", path);
        rc = -1;
    }

    if (rc == 0) {
        memset(state->vertex_ks_key, 0, sizeof(state->vertex_ks_key));
        memset(state->vertex_ks_bits, 0, sizeof(state->vertex_ks_bits));
        memset(state->vertex_ks_mod, 0, sizeof(state->vertex_ks_mod));
        memset(state->vertex_ks_frame_mode, 0, sizeof(state->vertex_ks_frame_mode));
        memset(state->vertex_ks_frame_off, 0, sizeof(state->vertex_ks_frame_off));
        memset(state->vertex_ks_frame_step, 0, sizeof(state->vertex_ks_frame_step));
        state->vertex_ks_count = tmp->count;
        memcpy(state->vertex_ks_key, tmp->key, sizeof(state->vertex_ks_key));
        memcpy(state->vertex_ks_bits, tmp->bits, sizeof(state->vertex_ks_bits));
        memcpy(state->vertex_ks_mod, tmp->mod, sizeof(state->vertex_ks_mod));
        memcpy(state->vertex_ks_frame_mode, tmp->frame_mode, sizeof(state->vertex_ks_frame_mode));
        memcpy(state->vertex_ks_frame_off, tmp->frame_off, sizeof(state->vertex_ks_frame_off));
        memcpy(state->vertex_ks_frame_step, tmp->frame_step, sizeof(state->vertex_ks_frame_step));
        state->vertex_ks_active_idx[0] = -1;
        state->vertex_ks_active_idx[1] = -1;
        state->vertex_ks_counter[0] = 0;
        state->vertex_ks_counter[1] = 0;
        state->vertex_ks_warned[0] = 0;
        state->vertex_ks_warned[1] = 0;
        LOG_NOTICE("Loaded %d Vertex key->keystream mappings from '%s'.\n", tmp->count, path);
    }

    free(tmp);
    return rc;
}
