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
    DSD_SCAN_OPT_DIRECT = DSD_SCAN_OPT_BP | DSD_SCAN_OPT_HYTERA | DSD_SCAN_OPT_SCALAR | DSD_SCAN_OPT_SCRAMBLER,
    DSD_SCAN_OPT_FILES = DSD_SCAN_OPT_HEX_FILE | DSD_SCAN_OPT_DEC_FILE
};

/** Nonsecret overrides copied into runtime/frontend scan scopes. */
typedef struct {
    uint32_t present;
    int force;
    int strict_crc;
    int voice_only;
    int qualify_ms;
    int hold_ms;
    int mute_dmr;
    int unmute_p25;
    char group_file[1024];
} dsd_scan_option_values;

/** Parsed import metadata. Wipe after materializing; never publish this object to a frontend. */
typedef struct {
    dsd_scan_option_values values;
    uint64_t bp;
    uint64_t scalar;
    uint64_t hytera[4];
    unsigned int hytera_digits;
    char hex_file[1024];
    char dec_file[1024];
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
