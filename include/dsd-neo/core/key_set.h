// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Sparse per-row keyring copy plus the scan key swap state machine.
 *
 * A `dsd_key_set` is a sparse copy of the live keyring (`rkey_array` /
 * `rkey_array_loaded`) with the `keyloader` flag and the scalar key block it
 * was captured with. File-backed row sets carry a zeroed scalar block so
 * installing them clears stale direct scalars; embedded direct-key sets carry
 * their parsed `-b`/`-H` equivalents. The lazily captured baseline carries the
 * live scalars so leaving a keyed row restores them.
 *
 * Only core headers plus `runtime/log.h`; engine and app_control call down
 * into it, so no hook table.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_SET_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_SET_H_H

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t index;
    uint64_t value;
    uint8_t loaded;
} dsd_key_set_entry;

/**
 * The scalar key slots that per-call activation and the keyloader-gated resets
 * write beside the keyring. Mirrors the `dsd_state` field types exactly so the
 * block moves with one copy.
 */
typedef struct {
    unsigned long long K;
    unsigned long long K1;
    unsigned long long K2;
    unsigned long long K3;
    unsigned long long K4;
    unsigned long long R;
    unsigned long long RR;
    unsigned long long H;
    uint8_t hytera_key_segments;
    unsigned long long A1[2];
    unsigned long long A2[2];
    unsigned long long A3[2];
    unsigned long long A4[2];
    int aes_key_loaded[2];
    uint8_t aes_key_segments[2];
    uint8_t aes_key[32];
} dsd_key_scalars;

typedef struct {
    dsd_key_set_entry* entries; /* heap, NULL when empty */
    size_t count;
    uint8_t present; /* a key source was supplied on this row/target */
    int keyloader;   /* state->keyloader to install with the entries */
    /* Captured for the baseline or parsed directly; zeroed for file-backed sets. */
    dsd_key_scalars scalars;
} dsd_key_set;

/**
 * Release a set's entries and zero the struct. Inline so translation units
 * that compile `dsd_state_trunk_lcn.c` directly (several tests) can free
 * without linking `key_set.c`. Wipes entry contents before free.
 */
static inline void
dsd_key_set_free(dsd_key_set* ks) {
    if (ks == NULL) {
        return;
    }
    if (ks->entries != NULL) {
        DSD_SECURE_ZERO(ks->entries, ks->count * sizeof(*ks->entries));
        free(ks->entries);
    }
    DSD_SECURE_ZERO(ks, sizeof(*ks));
}

typedef enum {
    DSD_KEY_DIRECT_OK = 0,
    DSD_KEY_DIRECT_INVALID_ARGUMENT = -1,
    DSD_KEY_DIRECT_INVALID_DEC = -2,
    DSD_KEY_DIRECT_INVALID_HEX = -3,
} dsd_key_direct_result;

/**
 * Capture the live keyring plus scalar block into @p out (frees prior).
 * Returns 0 on success, -1 on a bad argument or allocation failure; on
 * failure @p out is left empty.
 */
int dsd_key_set_capture(dsd_key_set* out, const dsd_state* state);

/**
 * Install @p ks onto the live state: zero both keyring arrays, write entries,
 * set `keyloader`, then write the scalar block. The zeroing lives here rather
 * than relying on the keyloader-gated resets, which is what makes trunk-scan
 * switches safe outside `noCarrier`.
 */
void dsd_key_set_install(dsd_state* state, const dsd_key_set* ks);

/**
 * Deep copy @p src into @p dst (frees prior @p dst entries). Returns 0 on
 * success, -1 on a bad argument or allocation failure; on failure @p dst is
 * left as it was.
 */
int dsd_key_set_copy(dsd_key_set* dst, const dsd_key_set* src);

/** Non-zero when two sets hold the same metadata, scalar block, and entries. */
int dsd_key_set_equal(const dsd_key_set* a, const dsd_key_set* b);

/**
 * Load hex then dec key files into @p out via a throwaway state. Either path
 * may be NULL/empty. Sets `present = 1`, `keyloader = 1`. Returns 0 on
 * success, -1 on any importer failure with @p out untouched.
 */
int dsd_key_set_load_csv(dsd_key_set* out, const char* hex_path, const char* dec_path, int show_keys);

/**
 * Parse direct `-H`/`-b` equivalents into a scalar-only set. Either value may
 * be NULL or ASCII-whitespace-only, but at least one must be present. Hex input
 * accepts an optional leading `0x`, ignores embedded ASCII whitespace, and
 * requires exactly 10, 32, or 64 digits. Decimal input is an unsigned value in
 * `[0, 255]`. Both inputs may be supplied together.
 *
 * On success the set has `present = 1` and `keyloader = 0`. On failure @p out
 * is untouched and the result identifies the invalid field without exposing
 * its value to callers' diagnostics.
 */
dsd_key_direct_result dsd_key_set_load_direct(dsd_key_set* out, const char* single_hex, const char* single_dec);

/** Prepared key transition owns all allocations needed at commit; zero-initialize before use. */
typedef struct {
    dsd_key_set baseline;
    dsd_key_set active;
    int capture_baseline;
    int keyed;
    int changed;
} dsd_scan_key_change;

void dsd_scan_key_change_clear(dsd_scan_key_change* change);
/** Prepare without changing live keys. NULL/absent row means restore globals. */
int dsd_scan_key_change_prepare(const dsd_state* state, const dsd_key_set* row, dsd_scan_key_change* change);
/** Install without allocation. Returns nonzero if effective key identity changed. */
int dsd_scan_key_change_commit(dsd_state* state, dsd_scan_key_change* change);

/**
 * Enter the scan swap: capture the baseline from live state when no set is
 * active, copy @p row_set into the active slot, install it. Returns 1 when
 * the installed set identity changed, 0 when it was already installed or the
 * swap could not be made (allocation failure leaves the live keyring as it
 * was, so a later leave never installs an empty baseline).
 */
int dsd_scan_keys_enter(dsd_state* state, const dsd_key_set* row_set);

/** Leave the scan swap: reinstall the baseline, free baseline and active. */
void dsd_scan_keys_leave(dsd_state* state);

/** Install the baseline without dropping it; runtime key commands edit globals. */
void dsd_scan_keys_suspend(dsd_state* state);

/** Re-capture the baseline from live state, reinstall the active set. */
void dsd_scan_keys_resume(dsd_state* state);

/**
 * `-Y` helper: look up the row's set; present sets enter, absent sets leave.
 * Bumps the encrypted-lockout key epoch when the installed set changed
 * (global ledger under `-Y`; trunk scan never bumps). Returns the bump flag.
 */
int dsd_scan_row_keys_apply(dsd_state* state, int row);

/**
 * Warn once that a channel map's per-row keys are stored but will never be
 * applied, because row keys ride the `-Y` scanner only. No-op when the map
 * carries no row keys or @p scanner_mode is on.
 */
void dsd_scan_row_keys_warn_if_unused(const dsd_state* state, int scanner_mode);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_SET_H_H */
