// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Import-time scanner options. This is a restricted argument grammar, never a command. */
#ifndef DSD_NEO_RUNTIME_SCAN_OPTIONS_H
#define DSD_NEO_RUNTIME_SCAN_OPTIONS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_SCAN_OPT_FORCE = 1U << 0,
    DSD_SCAN_OPT_CRC = 1U << 1,
    DSD_SCAN_OPT_VOICE = 1U << 2,
    DSD_SCAN_OPT_QUALIFY = 1U << 3,
    DSD_SCAN_OPT_HOLD = 1U << 4,
    DSD_SCAN_OPT_BP = 1U << 5,
    DSD_SCAN_OPT_HYTERA = 1U << 6,
    DSD_SCAN_OPT_SCALAR = 1U << 7,
    DSD_SCAN_OPT_SCRAMBLER = 1U << 8,
    DSD_SCAN_OPT_HEX_FILE = 1U << 9,
    DSD_SCAN_OPT_DEC_FILE = 1U << 10,
    DSD_SCAN_OPT_GROUP = 1U << 11,
    /** The row decides DMR encrypted-audio muting. Set by `-b`/`-H`/`-1`/`-R` in the option text,
     * so legacy single_key_dec/single_key_hex columns keep their key-only meaning. */
    DSD_SCAN_OPT_MUTE_DMR = 1U << 12,
    DSD_SCAN_OPT_DATA = 1U << 13,
    DSD_SCAN_OPT_ENC = 1U << 14,
    /** Direct option text mutes undecodable P25 audio; material-only sources do not. */
    DSD_SCAN_OPT_MUTE_P25 = 1U << 15,
    /** Prefer learned P25 control-channel candidates (-^). */
    DSD_SCAN_OPT_P25_CANDIDATES = 1U << 16,
    DSD_SCAN_OPT_DMR_MAP = 1U << 17,
    DSD_SCAN_OPT_CLEAR_KEYS = 1U << 18,
    DSD_SCAN_OPT_KEY_PROFILE_REF = 1U << 19,
    /** The row caps how long one scan-target visit may last (issue #507). */
    DSD_SCAN_OPT_MAX_VISIT = 1U << 20,
    DSD_SCAN_OPT_DIRECT = DSD_SCAN_OPT_BP | DSD_SCAN_OPT_HYTERA | DSD_SCAN_OPT_SCALAR | DSD_SCAN_OPT_SCRAMBLER,
    DSD_SCAN_OPT_FILES = DSD_SCAN_OPT_HEX_FILE | DSD_SCAN_OPT_DEC_FILE
};

/** Key-file path capacity, matching the legacy keys_hex_csv/keys_dec_csv column limit (CSV_IMPORT_PATH_MAX). */
#define DSD_SCAN_OPTIONS_KEY_PATH_MAX   2048
/** Group-file path capacity; must equal the capacity of dsd_opts::group_in_file, which it overrides. */
#define DSD_SCAN_OPTIONS_GROUP_PATH_MAX 1024

/** Nonsecret overrides copied into runtime/frontend scan scopes. A field is meaningful only when its
 * DSD_SCAN_OPT_* bit is set in `present`. Direct option text (DSD_SCAN_OPT_MUTE_P25) always mutes undecodable P25 audio,
 * as the CLI switch does, so it carries no value here. */
typedef struct {
    uint32_t present;
    int force;
    int strict_crc;
    int voice_only;
    int qualify_ms;
    int hold_ms;
    /** 0 = explicit per-row disable */
    int max_visit_ms;
    int mute_dmr;
    int tune_data_calls;
    int tune_enc_calls;
    char group_file[DSD_SCAN_OPTIONS_GROUP_PATH_MAX];
    /** Empty with DSD_SCAN_OPT_DMR_MAP set explicitly clears the row mapping. */
    char dmr_map_file[DSD_SCAN_OPTIONS_KEY_PATH_MAX];
    char key_profile_ref[64];
} dsd_scan_option_values;

/** Parsed import metadata. Wipe after materializing; never publish this object to a frontend. */
typedef struct {
    dsd_scan_option_values values;
    uint64_t bp;
    uint64_t scalar;
    uint64_t hytera[4];
    unsigned int hytera_digits;
    char hex_file[DSD_SCAN_OPTIONS_KEY_PATH_MAX];
    char dec_file[DSD_SCAN_OPTIONS_KEY_PATH_MAX];
} dsd_scan_options;

/** Parse and validate against a dsd_scan_mode value and conventional/trunk context.
 * Empty text inherits everything. Protocol-specific options require a declared mode.
 * On failure out is unchanged; error contains only fixed option names and explanations.
 * Single/double quotes group arguments; backslashes are literal. No CSV comma escaping. */
int dsd_scan_options_parse(const char* text, unsigned int mode, int conventional, dsd_scan_options* out, char* error,
                           size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
