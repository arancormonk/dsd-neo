// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/path_policy.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int
profile_error(char* error, size_t size, const char* field, const char* reason) {
    if (error && size) {
        DSD_SNPRINTF(error, size, "%s: %s", field, reason);
    }
    return -1;
}

int
dsd_scan_options_keys(const dsd_scan_options* options, dsd_key_set* out) {
    if (!options || !out) {
        return -1;
    }
    dsd_key_set keys = {0};
    const uint32_t present = options->values.present;
    keys.present = (present & DSD_SCAN_OPT_DIRECT) != 0;
    keys.scalars.K = options->bp;
    if (present & (DSD_SCAN_OPT_SCALAR | DSD_SCAN_OPT_SCRAMBLER)) {
        keys.scalars.R = options->scalar;
    }
    if (present & DSD_SCAN_OPT_SCALAR) {
        keys.scalars.RR = options->scalar;
    }
    if (present & DSD_SCAN_OPT_HYTERA) {
        keys.scalars.H = keys.scalars.K1 = options->hytera[0];
        keys.scalars.K2 = options->hytera[1];
        keys.scalars.K3 = options->hytera[2];
        keys.scalars.K4 = options->hytera[3];
        unsigned int count = options->hytera_digits == 10 ? 1U : options->hytera_digits / 16U;
        int nonzero = options->hytera[0] || options->hytera[1] || options->hytera[2] || options->hytera[3];
        keys.scalars.hytera_key_segments = nonzero ? (uint8_t)count : 0;
        if (count > 1) {
            unsigned long long* slots[] = {keys.scalars.A1, keys.scalars.A2, keys.scalars.A3, keys.scalars.A4};
            for (unsigned int i = 0; i < count; i++) {
                slots[i][0] = slots[i][1] = options->hytera[i];
                for (unsigned int j = 0; j < 8; j++) {
                    keys.scalars.aes_key[i * 8 + j] = (uint8_t)(options->hytera[i] >> (56 - j * 8));
                }
            }
            keys.scalars.aes_key_loaded[0] = keys.scalars.aes_key_loaded[1] = nonzero;
            keys.scalars.aes_key_segments[0] = keys.scalars.aes_key_segments[1] = (uint8_t)count;
        }
    }
    dsd_key_set_free(out);
    *out = keys;
    DSD_SECURE_ZERO(&keys, sizeof(keys));
    return 0;
}

static int
profile_text_present(const char* text) {
    if (!text) {
        return 0;
    }
    return strspn(text, " \t\r\n\v\f") != strlen(text);
}

static uint32_t
profile_legacy_fields(const char* hex_file, const char* dec_file, const char* single_hex, const char* single_dec) {
    return (profile_text_present(hex_file) ? DSD_SCAN_OPT_HEX_FILE : 0U)
           | (profile_text_present(dec_file) ? DSD_SCAN_OPT_DEC_FILE : 0U)
           | (profile_text_present(single_hex) ? DSD_SCAN_OPT_HYTERA : 0U)
           | (profile_text_present(single_dec) ? DSD_SCAN_OPT_BP : 0U);
}

static unsigned int
profile_hex_digits(const char* text) {
    unsigned int digits = 0;
    const char* p = text + strspn(text, " \t\r\n\v\f");
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    for (; *p; p++) {
        if (!strchr(" \t\r\n\v\f", *p)) {
            digits++;
        }
    }
    return digits;
}

static int
profile_legacy_direct(dsd_scan_options* options, uint32_t legacy, const char* hex, const char* dec) {
    if (!(legacy & DSD_SCAN_OPT_DIRECT)) {
        return 0;
    }
    dsd_key_set direct = {0};
    if (dsd_key_set_load_direct(&direct, hex, dec) != DSD_KEY_DIRECT_OK) {
        return -1;
    }
    if (legacy & DSD_SCAN_OPT_HYTERA) {
        options->hytera[0] = direct.scalars.K1;
        options->hytera[1] = direct.scalars.K2;
        options->hytera[2] = direct.scalars.K3;
        options->hytera[3] = direct.scalars.K4;
        options->hytera_digits = profile_hex_digits(hex);
    }
    if (legacy & DSD_SCAN_OPT_BP) {
        options->bp = direct.scalars.K;
    }
    dsd_key_set_free(&direct);
    return 0;
}

static int
profile_copy_path(char* dest, size_t capacity, const char* path) {
    if (!profile_text_present(path)) {
        return 0;
    }
    if (strlen(path) >= capacity) {
        return -1;
    }
    DSD_SNPRINTF(dest, capacity, "%s", path);
    return 0;
}

static int
profile_mute_dmr(const dsd_scan_options* options) {
    return !(options->bp || options->hytera[0] || options->hytera[1] || options->hytera[2] || options->hytera[3]);
}

int
dsd_scan_options_merge_keys(dsd_scan_options* options, const char* hex_file, const char* dec_file,
                            const char* single_hex, const char* single_dec, char* error, size_t error_size) {
    if (!options) {
        return -1;
    }
    const uint32_t legacy = profile_legacy_fields(hex_file, dec_file, single_hex, single_dec);
    if (legacy & options->values.present) {
        return profile_error(error, error_size, "options", "duplicate key source");
    }
    const uint32_t combined = legacy | options->values.present;
    if ((combined & DSD_SCAN_OPT_DIRECT) && (combined & DSD_SCAN_OPT_FILES)) {
        return profile_error(error, error_size, "options", "direct keys cannot be combined with key files");
    }
    if (profile_legacy_direct(options, legacy, single_hex, single_dec)) {
        return profile_error(error, error_size, "single_key_hex/single_key_dec", "invalid value");
    }
    if (profile_copy_path(options->hex_file, sizeof(options->hex_file), hex_file)) {
        return profile_error(error, error_size, "keys_hex_csv", "path too long");
    }
    if (profile_copy_path(options->dec_file, sizeof(options->dec_file), dec_file)) {
        return profile_error(error, error_size, "keys_dec_csv", "path too long");
    }
    options->values.present = combined;
    options->values.mute_dmr = profile_mute_dmr(options);
    return 0;
}

int
dsd_scan_options_resolve(dsd_scan_options* options, const char* base, char* error, size_t error_size) {
    if (!options) {
        return profile_error(error, error_size, "options", "invalid argument");
    }
    const char* paths[] = {options->hex_file, options->dec_file, options->values.group_file};
    char resolved[3][1024] = {{0}};
    const char* names[] = {"keys_hex_csv/-K", "keys_dec_csv/-k", "-G"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (!paths[i][0]) {
            continue;
        }
        if (dsd_path_resolve_relative_to_file(base, paths[i], resolved[i], sizeof(resolved[i]))) {
            return profile_error(error, error_size, names[i], "invalid or oversized path");
        }
    }
    DSD_MEMCPY(options->hex_file, resolved[0], sizeof(options->hex_file));
    DSD_MEMCPY(options->dec_file, resolved[1], sizeof(options->dec_file));
    DSD_MEMCPY(options->values.group_file, resolved[2], sizeof(options->values.group_file));
    return 0;
}

int
dsd_scan_profile_load(const dsd_scan_options* options, int show_keys, dsd_scan_row_profile** profile,
                      dsd_key_set* keys) {
    if (!options || !profile || !keys) {
        return -1;
    }
    dsd_scan_row_profile* loaded = (dsd_scan_row_profile*)calloc(1, sizeof(*loaded));
    if (!loaded) {
        return -1;
    }
    loaded->values = options->values;
    int rc = 0;
    if (options->values.present & DSD_SCAN_OPT_GROUP) {
        rc = dsd_tg_policy_load(options->values.group_file, &loaded->groups);
    }
    dsd_key_set loaded_keys = {0};
    if (!rc && (options->values.present & DSD_SCAN_OPT_FILES)) {
        rc = dsd_key_set_load_csv(&loaded_keys, options->hex_file, options->dec_file, show_keys);
    } else if (!rc) {
        rc = dsd_scan_options_keys(options, &loaded_keys);
    }
    if (rc) {
        dsd_scan_profile_free(loaded);
        dsd_key_set_free(&loaded_keys);
        return -1;
    }
    *profile = loaded;
    dsd_key_set_free(keys);
    *keys = loaded_keys;
    DSD_SECURE_ZERO(&loaded_keys, sizeof(loaded_keys));
    return 0;
}
