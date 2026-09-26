// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TRUNK_SCAN_VALIDATE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TRUNK_SCAN_VALIDATE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Validate with the engine's CSV parser without exposing owned targets or decoder
 * state to frontends. Returns 0 on success; count is zero on failure. Optional
 * output pointers may be NULL. Referenced side files (channel/key CSVs and
 * profiles) are not opened or checked for readability. Parsed key storage is erased before returning. */
int dsd_app_trunk_scan_validate_targets_csv(const char* path, int* target_count, char* err, size_t err_sz);

/** Nonsecret configured target. Unset dwell/hold/gain/bandwidth are -1; empty modulation
 * inherits. squelch_db (whole dB, 0 = off) is meaningful only with squelch_db_set, since
 * -1 dB is a real threshold. bandwidth_hz is an analog target's channel width in Hz
 * (--nfm-bandwidth-hz). Strings belong to this fixed-size record, never to decoder state. */
typedef struct {
    char id[64];
    char type[24];
    char modulation[8];
    uint32_t frequency_hz;
    unsigned int row;
    int dwell_ms;
    int hold_ms;
    int gain_db;
    int squelch_db_set;
    int squelch_db;
    int bandwidth_hz;
} dsd_app_scan_csv_target;

typedef enum {
    DSD_APP_SCAN_FILE_CHANNEL,
    DSD_APP_SCAN_FILE_BANDPLAN,
    DSD_APP_SCAN_FILE_KEYS_HEX,
    DSD_APP_SCAN_FILE_KEYS_DEC,
    DSD_APP_SCAN_FILE_GROUP,
    DSD_APP_SCAN_FILE_DMR_MAP
} dsd_app_scan_file_kind;

typedef struct {
    size_t index;
    unsigned int row;
    dsd_app_scan_file_kind kind;
    const char* field;
    const char* path;
    const char* resolved_path;
} dsd_app_scan_csv_reference;

typedef void (*dsd_app_scan_csv_target_cb)(const dsd_app_scan_csv_target* target, void* context);
typedef void (*dsd_app_scan_csv_reference_cb)(const dsd_app_scan_csv_reference* reference, void* context);

typedef struct {
    dsd_app_scan_csv_target_cb target;
    dsd_app_scan_csv_reference_cb reference;
    void* context;
} dsd_app_scan_csv_callbacks;

/** Synchronous inspection, without opening companions. base_path specifies the
 * original document's path base when inspecting a private copy (NULL uses path).
 * channel_map selects the channel import grammar instead of target CSV grammar.
 * Callbacks run only after inspection succeeds; pointers expire on return.
 * Input documents are limited to 64 MiB. Errors never include option/key cells. */
int dsd_app_scan_csv_inspect(const char* path, const char* base_path, int channel_map,
                             const dsd_app_scan_csv_callbacks* callbacks, char* err, size_t err_sz);

typedef struct {
    size_t reference_index;
    const char* relative_path;
} dsd_app_scan_csv_replacement;

/** Reinspect and rewrite only dependency tokens into a new private destination.
 * Every reference must have exactly one replacement. Generated paths may contain
 * ASCII letters/digits, slash, dot, underscore and hyphen; no leading dash.
 * Caller owns staging/atomic publication. No raw CSV or options cross this API. */
int dsd_app_scan_csv_rewrite(const char* path, const char* base_path, int channel_map, const char* destination,
                             const dsd_app_scan_csv_replacement* replacements, size_t count, char* err, size_t err_sz);

/** Complete preflight using isolated dry-run loaders, never engine initialization.
 * count/first_hz are zero on failure. Referenced channel maps validate their own
 * companions. Empty band plans and DMR mappings are refused as at engine start. */
int dsd_app_trunk_scan_validate_bundle(const char* path, int* count, uint32_t* first_hz, char* err, size_t err_sz);

#ifdef __cplusplus
}
#endif
#endif
