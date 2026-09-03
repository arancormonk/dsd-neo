// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
free_test_state(dsd_state* state) {
    if (state) {
        dsd_state_ext_free_all(state);
        dsd_state_trunk_lcn_free(state);
    }
    free(state);
}

static int
write_text_file(const char* path, const char* text) {
    FILE* fp = dsd_fopen_private(path, "w");
    if (!fp) {
        return -1;
    }
    DSD_FPRINTF(fp, "%s", text);
    fclose(fp);
    return 0;
}

/* Capture keeps loaded-but-zero and AES segment cells plus the scalar block;
 * install clears prior entries, sets keyloader, and zeroes/restores scalars. */
static int
test_capture_install_roundtrip(void) {
    // dsd_state is a multi-megabyte struct; avoid Windows' default ~1MB stack.
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;

    state->rkey_array[5] = 0xABCDEULL;
    state->rkey_array_loaded[5] = 1U;
    state->rkey_array[7] = 0ULL;
    state->rkey_array_loaded[7] = 1U;
    state->rkey_array[0x10] = 0xAAAAAAAAAAAAAAAAULL;
    state->rkey_array_loaded[0x10] = 1U;
    state->rkey_array[0x10 + 0x101] = 0ULL;
    state->rkey_array_loaded[0x10 + 0x101] = 1U;
    state->rkey_array[0x10 + 0x201] = 0xCCCCCCCCCCCCCCCCULL;
    state->rkey_array_loaded[0x10 + 0x201] = 1U;
    state->keyloader = 1;
    state->K = 11ULL;
    state->K1 = 12ULL;
    state->R = 13ULL;
    state->RR = 14ULL;
    state->H = 15ULL;
    state->hytera_key_segments = 3U;
    state->A1[0] = 21ULL;
    state->aes_key_loaded[1] = 1;
    state->aes_key_segments[0] = 4U;

    dsd_key_set ks;
    DSD_MEMSET(&ks, 0, sizeof(ks));
    dsd_key_set_capture(&ks, state);
    if (ks.present != 0 || ks.keyloader != 1 || ks.count != 5) {
        DSD_FPRINTF(stderr, "capture header mismatch (present=%u keyloader=%d count=%zu)\n", ks.present, ks.keyloader,
                    ks.count);
        failed = 1;
    }
    if (ks.scalars.K != 11ULL || ks.scalars.K1 != 12ULL || ks.scalars.R != 13ULL || ks.scalars.RR != 14ULL
        || ks.scalars.H != 15ULL || ks.scalars.hytera_key_segments != 3U || ks.scalars.A1[0] != 21ULL
        || ks.scalars.aes_key_loaded[1] != 1 || ks.scalars.aes_key_segments[0] != 4U) {
        DSD_FPRINTF(stderr, "capture scalar block mismatch\n");
        failed = 1;
    }

    /* Mutate the live state, then install: prior entries must clear. */
    DSD_MEMSET(state->rkey_array, 0xAA, sizeof(state->rkey_array));
    DSD_MEMSET(state->rkey_array_loaded, 0xAA, sizeof(state->rkey_array_loaded));
    state->keyloader = 0;
    state->K = 0ULL;
    state->A1[0] = 0ULL;
    dsd_key_set_install(state, &ks);
    if (state->rkey_array[5] != 0xABCDEULL || state->rkey_array_loaded[5] != 1U) {
        DSD_FPRINTF(stderr, "install did not restore slot 5\n");
        failed = 1;
    }
    if (state->rkey_array[7] != 0ULL || state->rkey_array_loaded[7] != 1U) {
        DSD_FPRINTF(stderr, "install dropped the loaded-but-zero cell\n");
        failed = 1;
    }
    if (state->rkey_array[6] != 0ULL || state->rkey_array_loaded[6] != 0U) {
        DSD_FPRINTF(stderr, "install did not clear a prior entry\n");
        failed = 1;
    }
    if (state->keyloader != 1 || state->K != 11ULL || state->A1[0] != 21ULL) {
        DSD_FPRINTF(stderr, "install did not restore keyloader/scalars\n");
        failed = 1;
    }

    /* Installing an empty set zeroes the keyring and the scalar block. */
    dsd_key_set empty;
    DSD_MEMSET(&empty, 0, sizeof(empty));
    dsd_key_set_install(state, &empty);
    if (state->rkey_array[5] != 0ULL || state->rkey_array_loaded[5] != 0U || state->keyloader != 0 || state->K != 0ULL
        || state->A1[0] != 0ULL) {
        DSD_FPRINTF(stderr, "install of an empty set did not clear\n");
        failed = 1;
    }

    /* Copy preserves the set; equal only matches identical sets. */
    dsd_key_set copy;
    DSD_MEMSET(&copy, 0, sizeof(copy));
    dsd_key_set_copy(&copy, &ks);
    if (!dsd_key_set_equal(&copy, &ks)) {
        DSD_FPRINTF(stderr, "copy/equal mismatch\n");
        failed = 1;
    }
    copy.entries[0].value ^= 1ULL;
    if (dsd_key_set_equal(&copy, &ks)) {
        DSD_FPRINTF(stderr, "equal missed a value change\n");
        failed = 1;
    }

    dsd_key_set_free(&copy);
    dsd_key_set_free(&ks);
    free_test_state(state);
    return failed;
}

/* Hex+dec load from real files; missing file fails with out untouched. */
static int
test_load_hex_dec(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;

    char hex_tmpl[] = "dsd-neo-test-rowkey-hex-XXXXXX";
    int hex_fd = dsd_mkstemp(hex_tmpl);
    if (hex_fd < 0) {
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(hex_fd);
    char dec_tmpl[] = "dsd-neo-test-rowkey-dec-XXXXXX";
    int dec_fd = dsd_mkstemp(dec_tmpl);
    if (dec_fd < 0) {
        (void)remove(hex_tmpl);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(dec_fd);
    if (write_text_file(hex_tmpl,
                        "key id(hex),key value (hex)\n0010,AAAAAAAAAAAAAAAA,0,CCCCCCCCCCCCCCCC,DDDDDDDDDDDDDDDD\n")
            != 0
        || write_text_file(dec_tmpl, "key id or tg id (dec),key number or value (dec)\n20,12345\n") != 0) {
        (void)remove(hex_tmpl);
        (void)remove(dec_tmpl);
        free_test_state(state);
        return 1;
    }

    dsd_key_set ks;
    DSD_MEMSET(&ks, 0, sizeof(ks));
    if (dsd_key_set_load_csv(&ks, hex_tmpl, dec_tmpl, 0) != 0) {
        DSD_FPRINTF(stderr, "load_csv failed on valid files\n");
        failed = 1;
    } else {
        if (ks.present != 1 || ks.keyloader != 1) {
            DSD_FPRINTF(stderr, "load_csv header mismatch\n");
            failed = 1;
        }
        dsd_key_set_install(state, &ks);
        if (state->rkey_array[0x10] != 0xAAAAAAAAAAAAAAAAULL || state->rkey_array_loaded[0x10] != 1U) {
            DSD_FPRINTF(stderr, "load_csv missed the hex base segment\n");
            failed = 1;
        }
        if (state->rkey_array[0x10 + 0x101] != 0ULL || state->rkey_array_loaded[0x10 + 0x101] != 1U) {
            DSD_FPRINTF(stderr, "load_csv missed the hex zero segment\n");
            failed = 1;
        }
        if (state->rkey_array[20] != 12345ULL || state->rkey_array_loaded[20] != 1U) {
            DSD_FPRINTF(stderr, "load_csv missed the dec entry\n");
            failed = 1;
        }
        if (state->keyloader != 1 || state->K != 0ULL) {
            DSD_FPRINTF(stderr, "row set must arm keyloader with a zeroed scalar block\n");
            failed = 1;
        }
    }

    /* Missing file: -1 with out untouched. */
    const size_t keep_count = ks.count;
    const uint64_t keep_first = ks.count > 0 ? ks.entries[0].value : 0ULL;
    if (dsd_key_set_load_csv(&ks, "dsd-neo-test-missing-dir/missing.csv", NULL, 0) == 0) {
        DSD_FPRINTF(stderr, "load_csv succeeded on a missing file\n");
        failed = 1;
    }
    if (ks.count != keep_count || (ks.count > 0 && ks.entries[0].value != keep_first)) {
        DSD_FPRINTF(stderr, "load_csv touched out on failure\n");
        failed = 1;
    }
    if (dsd_key_set_load_csv(&ks, NULL, NULL, 0) == 0) {
        DSD_FPRINTF(stderr, "load_csv succeeded with no paths\n");
        failed = 1;
    }

    dsd_key_set_free(&ks);
    (void)remove(hex_tmpl);
    (void)remove(dec_tmpl);
    free_test_state(state);
    return failed;
}

/* Enter/leave/suspend/resume, including a keyloader=0 baseline with -b-style K. */
static int
test_enter_leave_suspend_resume(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;

    state->keyloader = 0;
    state->K = 0xBEEFULL;
    state->rkey_array[3] = 111ULL;
    state->rkey_array_loaded[3] = 1U;

    dsd_key_set row;
    DSD_MEMSET(&row, 0, sizeof(row));
    row.entries = (dsd_key_set_entry*)calloc(1, sizeof(*row.entries));
    if (!row.entries) {
        free_test_state(state);
        return 1;
    }
    row.count = 1;
    row.present = 1;
    row.keyloader = 1;
    row.entries[0].index = 9U;
    row.entries[0].value = 999ULL;
    row.entries[0].loaded = 1U;

    if (dsd_scan_keys_enter(state, &row) != 1) {
        DSD_FPRINTF(stderr, "first enter must report a change\n");
        failed = 1;
    }
    if (state->rkey_array[9] != 999ULL || state->rkey_array[3] != 0ULL || state->K != 0ULL || state->keyloader != 1) {
        DSD_FPRINTF(stderr, "enter did not install the row set\n");
        failed = 1;
    }
    if (dsd_scan_keys_enter(state, &row) != 0) {
        DSD_FPRINTF(stderr, "repeat enter must report no change\n");
        failed = 1;
    }

    dsd_scan_keys_suspend(state);
    if (state->rkey_array[3] != 111ULL || state->K != 0xBEEFULL || state->keyloader != 0
        || state->rkey_array[9] != 0ULL) {
        DSD_FPRINTF(stderr, "suspend did not restore the baseline\n");
        failed = 1;
    }
    /* Runtime key edit while suspended lands in the globals. */
    state->rkey_array[5] = 555ULL;
    state->rkey_array_loaded[5] = 1U;
    dsd_scan_keys_resume(state);
    if (state->rkey_array[9] != 999ULL || state->rkey_array[5] != 0ULL) {
        DSD_FPRINTF(stderr, "resume did not reinstall the row set\n");
        failed = 1;
    }

    dsd_scan_keys_leave(state);
    if (state->rkey_array[5] != 555ULL || state->rkey_array[3] != 111ULL || state->K != 0xBEEFULL
        || state->keyloader != 0 || state->scan_keys_active_set != 0) {
        DSD_FPRINTF(stderr, "leave did not restore the edited baseline\n");
        failed = 1;
    }
    dsd_scan_keys_leave(state);

    dsd_key_set_free(&row);
    free_test_state(state);
    return failed;
}

/* Row apply bumps the lockout epoch only when the installed set changes. */
static int
test_row_apply_epoch(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;

    dsd_key_set row;
    DSD_MEMSET(&row, 0, sizeof(row));
    row.entries = (dsd_key_set_entry*)calloc(1, sizeof(*row.entries));
    if (!row.entries) {
        free_test_state(state);
        return 1;
    }
    row.count = 1;
    row.present = 1;
    row.keyloader = 1;
    row.entries[0].index = 9U;
    row.entries[0].value = 999ULL;
    row.entries[0].loaded = 1U;
    if (dsd_state_trunk_lcn_keys_set(state, 0, &row) != 0) {
        free_test_state(state);
        return 1;
    }

    const uint64_t epoch0 = state->enc_lockout_key_epoch;
    if (dsd_scan_row_keys_apply(state, 0) != 1 || state->enc_lockout_key_epoch != epoch0 + 1U) {
        DSD_FPRINTF(stderr, "keyed apply must install and bump the epoch\n");
        failed = 1;
    }
    if (state->rkey_array[9] != 999ULL) {
        DSD_FPRINTF(stderr, "keyed apply did not install entries\n");
        failed = 1;
    }
    if (dsd_scan_row_keys_apply(state, 0) != 0 || state->enc_lockout_key_epoch != epoch0 + 1U) {
        DSD_FPRINTF(stderr, "repeat apply must not bump the epoch\n");
        failed = 1;
    }
    if (dsd_scan_row_keys_apply(state, 1) != 1 || state->enc_lockout_key_epoch != epoch0 + 2U) {
        DSD_FPRINTF(stderr, "unkeyed apply must restore and bump the epoch\n");
        failed = 1;
    }
    if (state->rkey_array[9] != 0ULL) {
        DSD_FPRINTF(stderr, "unkeyed apply did not restore the baseline\n");
        failed = 1;
    }
    if (dsd_scan_row_keys_apply(state, 1) != 0 || state->enc_lockout_key_epoch != epoch0 + 2U) {
        DSD_FPRINTF(stderr, "repeat unkeyed apply must not bump the epoch\n");
        failed = 1;
    }
    if (dsd_scan_row_keys_apply(state, -1) != 0) {
        DSD_FPRINTF(stderr, "negative row must be a no-op\n");
        failed = 1;
    }

    free_test_state(state);
    return failed;
}

int
main(void) {
    if (test_capture_install_roundtrip() != 0) {
        return 1;
    }
    if (test_load_hex_dec() != 0) {
        return 1;
    }
    if (test_enter_leave_suspend_resume() != 0) {
        return 1;
    }
    if (test_row_apply_epoch() != 0) {
        return 1;
    }
    return 0;
}
