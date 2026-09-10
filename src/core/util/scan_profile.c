// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/dmr_key_map.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/path_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "key_set_internal.h"

int
dsd_scan_profile_ensure(dsd_scan_row_profile** profile) {
    if (!profile) {
        return -1;
    }
    if (*profile) {
        return 0;
    }
    dsd_scan_row_profile* fresh = calloc(1, sizeof(*fresh));
    if (!fresh) {
        return -1;
    }
    *profile = fresh;
    return 0;
}

static int
scan_aes_compatible(const dsd_key_scalars* k, unsigned int mode, int nxdn) {
    if (k->hytera_key_segments == 1 && mode != DSD_SCAN_MODE_DMR) {
        return 0;
    }
    if (k->aes_key_loaded[0]
        && (k->aes_key_segments[0] != 2 && k->aes_key_segments[0] != 4
            && !(mode == DSD_SCAN_MODE_P25 && k->aes_key_segments[0] == 3))) {
        return 0;
    }
    if (nxdn && k->aes_key_loaded[0] && k->aes_key_segments[0] != 4) {
        return 0;
    }
    return 1;
}

int
dsd_scan_keys_compatible(const dsd_key_set* keys, unsigned int mode) {
    if (!keys || !keys->present || keys->keyloader) {
        return keys != NULL;
    }
    const dsd_key_scalars* k = &keys->scalars;
    const int nxdn = mode == DSD_SCAN_MODE_NXDN48 || mode == DSD_SCAN_MODE_NXDN96;
    if (k->basic_key_present && (mode != DSD_SCAN_MODE_DMR || k->K > 255U)) {
        return 0;
    }
    if (k->scalar_key_present[0] && !k->scalar_key_present[1]
        && ((!nxdn && mode != DSD_SCAN_MODE_DPMR) || k->R > 32767U)) {
        return 0;
    }
    return scan_aes_compatible(k, mode, nxdn);
}

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
    keys.present = (present & (DSD_SCAN_OPT_DIRECT | DSD_SCAN_OPT_CLEAR_KEYS)) != 0;
    keys.scalars.K = options->bp;
    keys.scalars.basic_key_present = (present & DSD_SCAN_OPT_BP) != 0;
    if (present & (DSD_SCAN_OPT_SCALAR | DSD_SCAN_OPT_SCRAMBLER)) {
        keys.scalars.R = options->scalar;
        keys.scalars.scalar_key_present[0] = 1;
    }
    if (present & DSD_SCAN_OPT_SCALAR) {
        keys.scalars.RR = options->scalar;
        keys.scalars.scalar_key_present[1] = 1;
    }
    if (present & DSD_SCAN_OPT_HYTERA) {
        dsd_key_scalars_store_direct_hex(&keys.scalars, options->hytera, options->hytera_digits);
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

static int
profile_legacy_direct(dsd_scan_options* options, uint32_t legacy, const char* hex, const char* dec) {
    if (!(legacy & DSD_SCAN_OPT_DIRECT)) {
        return 0;
    }
    dsd_key_set direct = {0};
    size_t hex_digits = 0;
    if (dsd_key_set_load_direct_width(&direct, hex, dec, &hex_digits) != DSD_KEY_DIRECT_OK) {
        return -1;
    }
    if (legacy & DSD_SCAN_OPT_HYTERA) {
        options->hytera[0] = direct.scalars.K1;
        options->hytera[1] = direct.scalars.K2;
        options->hytera[2] = direct.scalars.K3;
        options->hytera[3] = direct.scalars.K4;
        options->hytera_digits = (unsigned int)hex_digits;
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
    if ((combined & DSD_SCAN_OPT_CLEAR_KEYS) && (combined & (DSD_SCAN_OPT_DIRECT | DSD_SCAN_OPT_FILES))) {
        return profile_error(error, error_size, "options", "no-keys cannot be combined with key material");
    }
    if ((combined & DSD_SCAN_OPT_DIRECT) && (combined & DSD_SCAN_OPT_FILES)) {
        return profile_error(error, error_size, "options", "direct keys cannot be combined with key files");
    }
    /* Merge into a copy so a rejected column leaves the caller's object exactly as it was. */
    dsd_scan_options merged = *options;
    int rc = 0;
    if (profile_legacy_direct(&merged, legacy, single_hex, single_dec)) {
        rc = profile_error(error, error_size, "single_key_hex/single_key_dec", "invalid value");
    } else if (profile_copy_path(merged.hex_file, sizeof(merged.hex_file), hex_file)) {
        rc = profile_error(error, error_size, "keys_hex_csv", "path too long");
    } else if (profile_copy_path(merged.dec_file, sizeof(merged.dec_file), dec_file)) {
        rc = profile_error(error, error_size, "keys_dec_csv", "path too long");
    }
    if (rc == 0) {
        merged.values.present = combined;
        /* Legacy columns only load material. Preserve the option text's policy. */
        *options = merged;
    }
    DSD_SECURE_ZERO(&merged, sizeof(merged));
    return rc;
}

static int
profile_resolve_path(const char* base, char* path, size_t capacity) {
    if (!path[0]) {
        return 0;
    }
    char resolved[DSD_SCAN_OPTIONS_KEY_PATH_MAX] = {0};
    const size_t limit = capacity < sizeof(resolved) ? capacity : sizeof(resolved);
    if (dsd_path_resolve_relative_to_file(base, path, resolved, limit)) {
        return -1;
    }
    DSD_MEMCPY(path, resolved, capacity);
    return 0;
}

int
dsd_scan_options_resolve(dsd_scan_options* options, const char* base, char* error, size_t error_size) {
    if (!options) {
        return profile_error(error, error_size, "options", "invalid argument");
    }
    /* Resolve into a copy so a failure leaves the caller's object exactly as it was. */
    dsd_scan_options resolved = *options;

    const struct {
        char* path;
        size_t capacity;
        const char* name;
    } paths[] = {
        {resolved.hex_file, sizeof(resolved.hex_file), "keys_hex_csv/-K"},
        {resolved.dec_file, sizeof(resolved.dec_file), "keys_dec_csv/-k"},
        {resolved.values.group_file, sizeof(resolved.values.group_file), "-G"},
        {resolved.values.dmr_map_file, sizeof(resolved.values.dmr_map_file), "--dmr-tg-key-csv"},
    };

    int rc = 0;
    for (size_t i = 0; rc == 0 && i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (profile_resolve_path(base, paths[i].path, paths[i].capacity)) {
            rc = profile_error(error, error_size, paths[i].name, "invalid or oversized path");
        }
    }
    if (rc == 0) {
        *options = resolved;
    }
    DSD_SECURE_ZERO(&resolved, sizeof(resolved));
    return rc;
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
    if (!rc && (options->values.present & DSD_SCAN_OPT_DMR_MAP) && options->values.dmr_map_file[0]) {
        rc = dsd_dmr_key_map_load(options->values.dmr_map_file, &loaded->dmr_map);
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
    DSD_SNPRINTF(loaded_keys.profile_ref, sizeof(loaded_keys.profile_ref), "%s", options->values.key_profile_ref);
    dsd_key_set_free(keys);
    *keys = loaded_keys;
    DSD_SECURE_ZERO(&loaded_keys, sizeof(loaded_keys));
    return 0;
}
