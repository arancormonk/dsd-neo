// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Import-time scanner options. This is a restricted argument grammar, never a command. */
#ifndef DSD_NEO_RUNTIME_SCAN_OPTIONS_H
#define DSD_NEO_RUNTIME_SCAN_OPTIONS_H
#include <dsd-neo/core/analog_tone.h>
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
    /** The row sets its own squelch threshold (--squelch-db, issue #521). */
    DSD_SCAN_OPT_SQUELCH = 1U << 21,
    /** The analog row sets its own channel width (--nfm-bandwidth-hz on nfm, --am-bandwidth-hz on am; issue #526). */
    DSD_SCAN_OPT_BANDWIDTH = 1U << 22,
    /** The nfm row sets its own CTCSS/DCS receive policy (--tone-allow, --tone-block or --no-tone-filter: one
     * option in three spellings, issue #527). */
    DSD_SCAN_OPT_TONE = 1U << 23,
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
    /** Whole dB from -100 to 0 in the rtl_sql convention; 0 = explicit per-row off. The LEVEL value. */
    int squelch_db;
    /** The row's squelch mode (dsd_squelch_mode): LEVEL (squelch_db, --squelch-db or --squelch <dB>) or AUTO
     * (--squelch auto[+N], nfm and am rows and targets only), whose margin is squelch_margin_db. */
    int squelch_mode;
    int squelch_margin_db;
    /** Full RF channel width in Hz for the row's analog demodulator (runtime/analog_channel.h ranges). */
    int channel_bw_hz;
    /** The analog demodulator (dsd_analog_demod) channel_bw_hz is a width of: DSD_ANALOG_DEMOD_FM for
     * --nfm-bandwidth-hz, DSD_ANALOG_DEMOD_AM for --am-bandwidth-hz. Meaningful with DSD_SCAN_OPT_BANDWIDTH only. */
    int channel_bw_kind;
    /** The row's tone policy (dsd_tone_filter_mode; OFF for --no-tone-filter, the explicit row disable). */
    int tone_filter;
    /** Its list: every entry a standard tone or code, held once (core/analog_tone.h); empty with --no-tone-filter. */
    dsd_tone_set tone_set;
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

/** Visit file operands using the scoped-option tokenizer and registry. Offsets
 * cover the complete operand token, or the complete option token for --name=value.
 * In the latter case prefix is the canonical option name followed by '='.
 * Strings expire on callback return. No direct key operands are exposed.
 * Returns -1 on malformed input or callback failure; callbacks may precede failure. */
typedef int (*dsd_scan_option_file_cb)(void* context, const char* option, const char* path, size_t offset,
                                       size_t length, int includes_option);
int dsd_scan_options_visit_files(const char* text, void* context, dsd_scan_option_file_cb callback);

/** Whether a row's options cell lost the rest of a tone list to a comma (issue #527). The CSV splitters end a cell at
 * every unquoted comma, so `--tone-allow 100.0,67.0` reaches dsd_scan_options_parse() as `--tone-allow 100.0` and
 * leaves `67.0` in the field after the cell. @p options is the cell, @p next the raw field after it (NULL when the row
 * has none) and @p next_past_header whether that field lies past the header's columns, where nothing reads it.
 * A cell that ends with a --tone-allow/--tone-block list is refused when the field after it goes on with it: any
 * text past the header, or, in one of the file's own columns, a field that can only be the rest of a list -- one run
 * of '/'-separated entries with no space in it, starting with a standard CTCSS tone or DCS code ("67.0",
 * "67.0/D023N"), alone or followed by a row option's switch, the rest of the cell the comma cut off too ("67.0
 * --squelch-db -60"). An RTL gain, and any other name with a space in it ("100 Main St", "D023 Repeater"), are that
 * column's own; a one-word name that is itself a standard tone or code ("100") reads as the list's rest.
 * Returns 1 with "--tone-allow: use / between entries, not commas" in @p error when refused, else 0. Never echoes the
 * cell or the field. */
int dsd_scan_options_tone_list_split(const char* options, const char* next, int next_past_header, char* error,
                                     size_t error_size);

/** The DSD_SCAN_OPT_* fields a row of class @p mode (dsd_scan_mode) may set in its `options` cell: the fields of every
 * switch dsd_scan_options_parse() takes on that class, the conventional-only ones only when @p conventional. */
uint32_t dsd_scan_options_fields_for_mode(unsigned int mode, int conventional);

/** Hold a row's channel width (DSD_SCAN_OPT_BANDWIDTH) to the DSP rate it would run at, for the
 * demodulator its class @p mode uses. Returns 0 when the row carries no width, @p rate_hz is not known
 * (<= 0) or the width fits; otherwise -1 with dsd_analog_width_check()'s message, which names the width,
 * the rate, the widest width that rate fits and the fix. The width's range was checked when it parsed. */
int dsd_scan_option_width_check(unsigned int mode, const dsd_scan_option_values* values, int rate_hz, char* error,
                                size_t error_size);

/** A row's own tone policy (DSD_SCAN_OPT_TONE, issue #527) as the import previews show it: returns its
 * dsd_tone_filter_mode (OFF for --no-tone-filter) and writes its list as displayed
 * (dsd_tone_set_format_display(): "100.0 Hz/D023N", the entries past the room counted) to @p list, "" for OFF.
 * Returns -1 with "" when the row runs the configured policy (@p values NULL or without the option). @p list must
 * hold at least 24 bytes. */
int dsd_scan_option_tone_summary(const dsd_scan_option_values* values, char* list, size_t list_size);
#ifdef __cplusplus
}
#endif
#endif
