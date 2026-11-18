// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/core/util/key_set_internal.h"
#include "dsd-neo/core/opts_fwd.h"
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
    state->aes_key[0] = 0xABU;
    state->aes_key[31] = 0xCDU;

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
        || ks.scalars.aes_key_loaded[1] != 1 || ks.scalars.aes_key_segments[0] != 4U || ks.scalars.aes_key[0] != 0xABU
        || ks.scalars.aes_key[31] != 0xCDU) {
        DSD_FPRINTF(stderr, "capture scalar block mismatch\n");
        failed = 1;
    }

    /* Mutate the live state, then install: prior entries must clear. */
    DSD_MEMSET(state->rkey_array, 0xAA, sizeof(state->rkey_array));
    DSD_MEMSET(state->rkey_array_loaded, 0xAA, sizeof(state->rkey_array_loaded));
    state->keyloader = 0;
    state->K = 0ULL;
    state->A1[0] = 0ULL;
    DSD_MEMSET(state->aes_key, 0, sizeof(state->aes_key));
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
    if (state->keyloader != 1 || state->K != 11ULL || state->A1[0] != 21ULL || state->aes_key[0] != 0xABU
        || state->aes_key[31] != 0xCDU) {
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
    copy.entries[0].value ^= 1ULL;
    copy.scalars.aes_key[31] ^= 1U;
    if (dsd_key_set_equal(&copy, &ks)) {
        DSD_FPRINTF(stderr, "equal missed an AES byte change\n");
        failed = 1;
    }

    dsd_key_set_free(&copy);
    dsd_key_set_free(&ks);
    free_test_state(state);
    return failed;
}

static int
test_load_direct(void) {
    int failed = 0;
    dsd_key_set direct;
    DSD_MEMSET(&direct, 0, sizeof(direct));

    if (dsd_key_set_load_direct(&direct, NULL, " 255 \t") != DSD_KEY_DIRECT_OK || direct.present != 1U
        || direct.keyloader != 0 || direct.count != 0U || direct.scalars.K != 255ULL) {
        DSD_FPRINTF(stderr, "direct decimal parse mismatch\n");
        failed = 1;
    }

    if (dsd_key_set_load_direct(&direct, " 0x01 23 45 67 89 ", NULL) != DSD_KEY_DIRECT_OK
        || direct.scalars.H != 0x0123456789ULL || direct.scalars.K1 != 0x0123456789ULL || direct.scalars.K2 != 0ULL
        || direct.scalars.hytera_key_segments != 1U || direct.scalars.aes_key_loaded[0] != 0
        || direct.scalars.aes_key_segments[0] != 0U) {
        DSD_FPRINTF(stderr, "direct 40-bit hex parse mismatch\n");
        failed = 1;
    }

    if (dsd_key_set_load_direct(&direct, "00112233445566778899AABBCCDDEEFF", "7") != DSD_KEY_DIRECT_OK
        || direct.scalars.K != 7ULL || direct.scalars.K1 != 0x0011223344556677ULL
        || direct.scalars.K2 != 0x8899AABBCCDDEEFFULL || direct.scalars.hytera_key_segments != 2U
        || direct.scalars.A1[0] != direct.scalars.K1 || direct.scalars.A1[1] != direct.scalars.K1
        || direct.scalars.A2[0] != direct.scalars.K2 || direct.scalars.aes_key_loaded[0] != 1
        || direct.scalars.aes_key_loaded[1] != 1 || direct.scalars.aes_key_segments[0] != 2U
        || direct.scalars.aes_key[0] != 0x00U || direct.scalars.aes_key[15] != 0xFFU
        || direct.scalars.aes_key[16] != 0U) {
        DSD_FPRINTF(stderr, "direct combined AES-128/decimal parse mismatch\n");
        failed = 1;
    }

    if (dsd_key_set_load_direct(&direct, "00112233445566778899AABBCCDDEEFF102132435465768798A9BACBDCEDFE0F", NULL)
            != DSD_KEY_DIRECT_OK
        || direct.scalars.K3 != 0x1021324354657687ULL || direct.scalars.K4 != 0x98A9BACBDCEDFE0FULL
        || direct.scalars.hytera_key_segments != 4U || direct.scalars.aes_key_segments[1] != 4U
        || direct.scalars.aes_key[16] != 0x10U || direct.scalars.aes_key[31] != 0x0FU) {
        DSD_FPRINTF(stderr, "direct AES-256 parse mismatch\n");
        failed = 1;
    }

    if (dsd_key_set_load_direct(&direct, "00000000000000000000000000000000", "0") != DSD_KEY_DIRECT_OK
        || direct.present != 1U || direct.scalars.K != 0ULL || direct.scalars.hytera_key_segments != 2U
        || direct.scalars.aes_key_loaded[0] != 1 || direct.scalars.aes_key_segments[0] != 2U) {
        DSD_FPRINTF(stderr, "explicit zero direct key mismatch\n");
        failed = 1;
    }

    dsd_key_set before;
    DSD_MEMSET(&before, 0, sizeof(before));
    if (dsd_key_set_copy(&before, &direct) != 0) {
        failed = 1;
    }
    static const char* const bad_dec[] = {"-1", "+1", "256", "999999999999999999999", "1 2"};
    for (size_t i = 0U; i < sizeof(bad_dec) / sizeof(bad_dec[0]); i++) {
        if (dsd_key_set_load_direct(&direct, NULL, bad_dec[i]) != DSD_KEY_DIRECT_INVALID_DEC
            || !dsd_key_set_equal(&direct, &before)) {
            DSD_FPRINTF(stderr, "invalid direct decimal was accepted or mutated output at %zu\n", i);
            failed = 1;
        }
    }
    static const char* const bad_hex[] = {"123", "00112233445566778899AABBCCDDEEFG", "0x", "-0123456789"};
    for (size_t i = 0U; i < sizeof(bad_hex) / sizeof(bad_hex[0]); i++) {
        if (dsd_key_set_load_direct(&direct, bad_hex[i], NULL) != DSD_KEY_DIRECT_INVALID_HEX
            || !dsd_key_set_equal(&direct, &before)) {
            DSD_FPRINTF(stderr, "invalid direct hex was accepted or mutated output at %zu\n", i);
            failed = 1;
        }
    }
    if (dsd_key_set_load_direct(&direct, " \t ", NULL) != DSD_KEY_DIRECT_INVALID_ARGUMENT
        || !dsd_key_set_equal(&direct, &before)
        || dsd_key_set_load_direct(NULL, NULL, "1") != DSD_KEY_DIRECT_INVALID_ARGUMENT) {
        DSD_FPRINTF(stderr, "direct invalid-argument handling mismatch\n");
        failed = 1;
    }

    dsd_key_set other;
    DSD_MEMSET(&other, 0, sizeof(other));
    if (dsd_key_set_load_direct(&other, NULL, "1") != DSD_KEY_DIRECT_OK) {
        failed = 1;
    } else {
        before.scalars.K = 2ULL;
        if (dsd_key_set_equal(&before, &other)) {
            DSD_FPRINTF(stderr, "scalar-only direct sets compared equal\n");
            failed = 1;
        }
    }

    unsigned char secret[8];
    DSD_MEMSET(secret, 0xA5, sizeof(secret));
    (void)DSD_SECURE_ZERO(secret, sizeof(secret));
    for (size_t i = 0U; i < sizeof(secret); i++) {
        if (secret[i] != 0U) {
            DSD_FPRINTF(stderr, "secure zero left byte %zu\n", i);
            failed = 1;
            break;
        }
    }

    dsd_key_set_free(&other);
    dsd_key_set_free(&before);
    dsd_key_set_free(&direct);
    return failed;
}

static int
test_direct_key_store_growth(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (state == NULL) {
        return 1;
    }
    int failed = 0;
    for (size_t i = 0U; i < 20U; i++) {
        char value[8];
        (void)DSD_SNPRINTF(value, sizeof(value), "%zu", i);
        dsd_key_set direct;
        DSD_MEMSET(&direct, 0, sizeof(direct));
        if (dsd_key_set_load_direct(&direct, NULL, value) != DSD_KEY_DIRECT_OK
            || dsd_state_trunk_lcn_keys_set(state, i, &direct) != 0) {
            dsd_key_set_free(&direct);
            failed = 1;
            break;
        }
    }
    if (state->trunk_lcn_keys_capacity < 20U) {
        DSD_FPRINTF(stderr, "direct key store did not grow\n");
        failed = 1;
    }
    for (size_t i = 0U; i < 20U && !failed; i++) {
        const dsd_key_set* direct = dsd_state_trunk_lcn_keys_get(state, i);
        if (direct == NULL || direct->scalars.K != (unsigned long long)i || direct->present != 1U) {
            DSD_FPRINTF(stderr, "direct key store lost row %zu across growth\n", i);
            failed = 1;
        }
    }
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

    dsd_key_set direct_one;
    dsd_key_set direct_two;
    DSD_MEMSET(&direct_one, 0, sizeof(direct_one));
    DSD_MEMSET(&direct_two, 0, sizeof(direct_two));
    if (dsd_key_set_load_direct(&direct_one, NULL, "1") != DSD_KEY_DIRECT_OK
        || dsd_key_set_load_direct(&direct_two, NULL, "2") != DSD_KEY_DIRECT_OK
        || dsd_state_trunk_lcn_keys_set(state, 2U, &direct_one) != 0
        || dsd_state_trunk_lcn_keys_set(state, 3U, &direct_two) != 0) {
        dsd_key_set_free(&direct_one);
        dsd_key_set_free(&direct_two);
        free_test_state(state);
        return 1;
    }
    if (dsd_scan_row_keys_apply(state, 2) != 1 || state->K != 1ULL || state->enc_lockout_key_epoch != epoch0 + 3U) {
        DSD_FPRINTF(stderr, "first direct row did not install and bump the epoch\n");
        failed = 1;
    }
    if (dsd_scan_row_keys_apply(state, 3) != 1 || state->K != 2ULL || state->enc_lockout_key_epoch != epoch0 + 4U) {
        DSD_FPRINTF(stderr, "distinct scalar-only row did not install and bump the epoch\n");
        failed = 1;
    }
    if (dsd_scan_row_keys_apply(state, 3) != 0 || state->enc_lockout_key_epoch != epoch0 + 4U) {
        DSD_FPRINTF(stderr, "repeat direct row changed the epoch\n");
        failed = 1;
    }
    if (dsd_scan_row_keys_apply(state, -1) != 0) {
        DSD_FPRINTF(stderr, "negative row must be a no-op\n");
        failed = 1;
    }

    free_test_state(state);
    return failed;
}

/* The talkgroup key autoload has the working Basic Privacy key: talkgroup 123's 0x33 replaced the operator's own (none,
   or a -b key), as keyring_dmr_tg_autoload_apply() leaves it. */
static void
seed_autoloaded_key(dsd_state* state, unsigned long long int operator_key) {
    state->K = 0x33ULL;
    state->K1 = state->H = 0x33ULL;
    state->basic_key_present = 1U;
    state->hytera_key_segments = 1U;
    const unsigned long long int installed[6] = {0x33ULL, 0x33ULL, 0ULL, 0ULL, 0ULL, 0x33ULL};
    for (int i = 0; i < 6; i++) {
        state->dmr_tg_autoload.installed[i] = installed[i];
        state->dmr_tg_autoload.baseline[i] = 0ULL;
    }
    state->dmr_tg_autoload.baseline[0] = operator_key;
    state->dmr_tg_autoload.installed_flags[0] = 1U;
    state->dmr_tg_autoload.installed_flags[1] = 1U;
    state->dmr_tg_autoload.baseline_flags[0] = operator_key != 0ULL ? 1U : 0U;
    state->dmr_tg_autoload.baseline_flags[1] = 0U;
    state->dmr_tg_autoload.active = 1U;
}

/* A scan row's keys entered while the autoload's key is the working key: leaving restores the operator's own key, not
   a talkgroup's key turned into one set directly. The secure wipe clears the autoload's key copies with the keys. */
static int
test_scan_keys_and_wipe_leave_no_autoloaded_key(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;
    for (int with_direct = 0; with_direct < 2; with_direct++) {
        const unsigned long long int operator_key = with_direct ? 70ULL : 0ULL;
        seed_autoloaded_key(state, operator_key);
        dsd_key_set row;
        DSD_MEMSET(&row, 0, sizeof(row));
        row.present = 1;
        row.keyloader = 1;
        if (dsd_scan_keys_enter(state, &row) != 1) {
            DSD_FPRINTF(stderr, "enter must report a change\n");
            failed = 1;
        }
        dsd_scan_keys_leave(state);
        if (state->K != operator_key || state->basic_key_present != (with_direct ? 1U : 0U) || state->K1 != 0ULL
            || state->H != 0ULL) {
            DSD_FPRINTF(stderr,
                        "leave reinstalled K=%llu (operator key %llu): the autoloaded key became a direct key\n",
                        state->K, operator_key);
            failed = 1;
        }
        dsd_key_set_free(&row);
    }
    seed_autoloaded_key(state, 70ULL);
    dsd_key_state_secure_wipe(state);
    const unsigned char* bytes = (const unsigned char*)&state->dmr_tg_autoload;
    for (size_t i = 0; i < sizeof(state->dmr_tg_autoload); i++) {
        if (bytes[i] != 0U) {
            DSD_FPRINTF(stderr, "the secure wipe left the autoload's key copies\n");
            failed = 1;
            break;
        }
    }
    free_test_state(state);
    return failed;
}

/* Slot 1's call keyed from talkgroup 123's key, which its row shows: the autoload's key is the working key, and the
   slot keeps its copy of it. */
static void
seed_slot_keyed_from_tg(dsd_state* state) {
    seed_autoloaded_key(state, 0ULL);
    for (int i = 0; i < 6; i++) {
        state->dmr_tg_autoload.slot_key[0][i] = state->dmr_tg_autoload.installed[i];
    }
    state->dmr_tg_autoload.slot_key_flags[0][0] = 1U;
    state->dmr_tg_autoload.slot_key_flags[0][1] = 1U;
    state->dmr_tg_autoload.slot_tg[0] = 123U;
}

/* Whether slot 1 holds no copy of a key, and the key its row shows on talkgroup 123 is none. */
static int
slot_key_copy_gone(const dsd_state* state) {
    for (size_t slot = 0; slot < 2U; slot++) {
        for (size_t i = 0; i < 6U; i++) {
            if (state->dmr_tg_autoload.slot_key[slot][i] != 0ULL) {
                return 0;
            }
        }
    }
    unsigned long long int words[6];
    uint8_t flags[2];
    dsd_dmr_slot_call_key(state, 0U, 123U, words, flags);
    return state->dmr_tg_autoload.slot_key_flags[0][0] == 0U && state->dmr_tg_autoload.slot_key_flags[0][1] == 0U
           && words[0] == 0ULL && words[1] == 0ULL && words[5] == 0ULL && flags[0] == 0U && flags[1] == 0U;
}

/* Replacing the key collection (the decryption settings' apply clearing it, a profile or scan row installed) wipes
   each slot's copy of the talkgroup key its call keyed from: the row no longer shows a key the keyring does not hold.
   The talkgroup the slot's call keyed from stays, and the autoload's key leaves the working key. */
static int
test_key_set_install_wipes_slot_key_copies(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;
    seed_slot_keyed_from_tg(state);
    dsd_key_set empty;
    DSD_MEMSET(&empty, 0, sizeof(empty));
    dsd_key_set_install(state, &empty);
    if (!slot_key_copy_gone(state)) {
        DSD_FPRINTF(stderr, "installing a key set kept slot 1's copy of talkgroup 123's key\n");
        failed = 1;
    }
    if (state->dmr_tg_autoload.slot_tg[0] != 123U || state->K != 0ULL || dsd_dmr_tg_autoload_holds(state)) {
        DSD_FPRINTF(stderr, "installing a key set lost slot 1's talkgroup or kept the autoloaded working key\n");
        failed = 1;
    }
    free_test_state(state);
    return failed;
}

/* Whether no byte of the autoload's key copies (the key it replaced, the one it installed, their flags) is set. */
static int
autoload_copies_wiped(const dsd_state* state) {
    const unsigned char* parts[4] = {(const unsigned char*)state->dmr_tg_autoload.baseline,
                                     (const unsigned char*)state->dmr_tg_autoload.installed,
                                     state->dmr_tg_autoload.baseline_flags, state->dmr_tg_autoload.installed_flags};
    const size_t sizes[4] = {sizeof(state->dmr_tg_autoload.baseline), sizeof(state->dmr_tg_autoload.installed),
                             sizeof(state->dmr_tg_autoload.baseline_flags),
                             sizeof(state->dmr_tg_autoload.installed_flags)};
    for (size_t p = 0; p < 4U; p++) {
        for (size_t i = 0; i < sizes[p]; i++) {
            if (parts[p][i] != 0U) {
                return 0;
            }
        }
    }
    return 1;
}

/* A call on talkgroup 123 keys from its imported key, replacing the operator's -b 70; a call on talkgroup 456, which
   has none, then gives the operator's key back. The autoload owns the working key no more, so its copies of both keys
   are wiped then: a later keyring clear disarms the no-carrier unload, which would otherwise be the next to wipe them. */
static int
test_autoload_miss_wipes_its_copies(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;
    state->rkey_array[123] = 0x33ULL;
    state->rkey_array_loaded[123] = 1U;
    state->keyloader = 1;
    state->K = 70ULL;
    state->basic_key_present = 1U;
    uint32_t keyed_from = 0U;
    if (keyring_dmr_tg_autoload_apply(state, 0U, 123U, &keyed_from) != 1 || state->K != 0x33ULL) {
        DSD_FPRINTF(stderr, "talkgroup 123's call did not key from its imported key\n");
        failed = 1;
    }
    if (keyring_dmr_tg_autoload_apply(state, 0U, 456U, &keyed_from) != 0 || state->K != 70ULL
        || state->basic_key_present != 1U || state->K1 != 0ULL) {
        DSD_FPRINTF(stderr, "talkgroup 456's call did not give the operator's key back\n");
        failed = 1;
    }
    if (state->dmr_tg_autoload.active != 0U || !autoload_copies_wiped(state)) {
        DSD_FPRINTF(stderr, "the autoload kept its copies of the keys after it gave the working key back\n");
        failed = 1;
    }
    free_test_state(state);
    return failed;
}

/* Forgetting a slot's talkgroup key wipes the copy its row shows on the change from a key to none, an imported zero key
   (a copy of zeros whose flags say it is a key) included; a slot that holds none stays empty. */
static int
test_forget_slot_wipes_on_the_change_to_none(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;
    for (int zero_key = 0; zero_key <= 1; zero_key++) {
        seed_slot_keyed_from_tg(state);
        if (zero_key) {
            DSD_MEMSET(state->dmr_tg_autoload.slot_key[0], 0, sizeof(state->dmr_tg_autoload.slot_key[0]));
        }
        keyring_dmr_tg_autoload_forget_slot(state, 0U);
        if (!slot_key_copy_gone(state) || state->dmr_tg_autoload.slot_tg[0] != 0U) {
            DSD_FPRINTF(stderr, "forgetting slot 1 kept its copy of %s key\n", zero_key ? "an imported zero" : "a");
            failed = 1;
        }
        keyring_dmr_tg_autoload_forget_slot(state, 0U);
        if (!slot_key_copy_gone(state) || state->dmr_tg_autoload.slot_tg[0] != 0U) {
            DSD_FPRINTF(stderr, "forgetting an empty slot 1 left something in it\n");
            failed = 1;
        }
    }
    free_test_state(state);
    return failed;
}

/* The final teardown wipes the keys the state holds, the autoload's copies of them included, whether or not a keyring
   armed the no-carrier unload. */
static int
test_free_state_wipes_keys(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    initState(state);
    seed_slot_keyed_from_tg(state);
    state->keyloader = 0;
    state->R = 0x0102030405ULL;
    state->rkey_array[123] = 0x33ULL;
    state->aes_key[0] = 0x5AU;
    freeState(state);
    int failed = 0;
    const unsigned char* autoload = (const unsigned char*)&state->dmr_tg_autoload;
    for (size_t i = 0; i < sizeof(state->dmr_tg_autoload); i++) {
        failed |= autoload[i] != 0U;
    }
    if (failed || state->K != 0ULL || state->K1 != 0ULL || state->H != 0ULL || state->R != 0ULL
        || state->rkey_array[123] != 0ULL || state->aes_key[0] != 0U) {
        DSD_FPRINTF(stderr, "freeState left key material in the state\n");
        failed = 1;
    }
    free(state);
    return failed;
}

/* The final teardown also wipes the vendor and static keys and keystreams and the working keystreams (CSI EE72, -S,
   the Vertex ALG 0x07 map), and wipes and frees the Retevis AP RC2 key schedule. freeOpts() wipes the options'
   secrets. */
static int
test_teardown_wipes_keystreams_and_opts_secrets(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!state || !opts) {
        free(state);
        free(opts);
        return 1;
    }
    initState(state);
    state->csi_ee_key[0] = 0x5AU;
    state->static_ks_bits[1][5] = 1U;
    state->vertex_ks_key[3] = 0x77ULL;
    state->vertex_ks_bits[2][7] = 1U;
    state->ks_octetL[9] = 0xA5U;
    state->ks_octetR[9] = 0xA5U;
    state->ks_bitstreamL[11] = 1U;
    state->ks_bitstreamR[11] = 1U;
    retevis_rc2_keystream_creation(state, "00112233445566778899AABBCCDDEEFF", 0);
    int failed = state->rc2_context == NULL;
    freeState(state);
    if (state->csi_ee_key[0] != 0U || state->static_ks_bits[1][5] != 0U || state->vertex_ks_key[3] != 0ULL
        || state->vertex_ks_bits[2][7] != 0U || state->ks_octetL[9] != 0U || state->ks_octetR[9] != 0U
        || state->ks_bitstreamL[11] != 0U || state->ks_bitstreamR[11] != 0U || state->rc2_context != NULL) {
        DSD_FPRINTF(stderr, "freeState left a keystream, vendor key or key schedule in the state\n");
        failed = 1;
    }
    initOpts(opts);
    DSD_SNPRINTF(opts->rdio_api_key, sizeof opts->rdio_api_key, "%s", "rdio-test-key");
    DSD_SNPRINTF(opts->rdio_api_url, sizeof opts->rdio_api_url, "%s", "https://name:word@rdio.invalid/api");
    DSD_SNPRINTF(opts->rr_app_key, sizeof opts->rr_app_key, "%s", "rr-test-key");
    DSD_SNPRINTF(opts->api_token, sizeof opts->api_token, "%s", "api-test-token");
    freeOpts(opts);
    if (opts->rdio_api_key[0] != '\0' || opts->rdio_api_url[0] != '\0' || opts->rr_app_key[0] != '\0'
        || opts->api_token[0] != '\0') {
        DSD_FPRINTF(stderr, "freeOpts left an API key or token in the options\n");
        failed = 1;
    }
    free(state);
    free(opts);
    return failed;
}

/* A Basic Privacy key typed while the autoload's key is the working key (the menu's or a command's direct entry,
   overlay mode) is the user's direct key on the key state the autoload replaced: talkgroup 123's Hytera key (K1, H)
   does not come along as a direct key, and the autoload no longer holds, so it never takes the typed key out. */
static int
test_direct_entry_keeps_no_autoloaded_key(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    int failed = 0;
    seed_autoloaded_key(state, 0ULL);
    if (dsd_key_apply_direct(state, DSD_KEY_TYPE_BASIC, "44", DSD_KEY_APPLY_OVERLAY) != DSD_KEY_DIRECT_OK) {
        DSD_FPRINTF(stderr, "direct entry of a Basic Privacy key failed\n");
        failed = 1;
    }
    if (state->K != 44ULL || state->basic_key_present != 1U) {
        DSD_FPRINTF(stderr, "direct entry did not keep the typed key as the Basic Privacy key\n");
        failed = 1;
    }
    if (state->K1 != 0ULL || state->H != 0ULL || state->hytera_key_segments != 0U) {
        DSD_FPRINTF(stderr, "direct entry kept the autoloaded talkgroup's Hytera key as a direct key\n");
        failed = 1;
    }
    if (dsd_dmr_tg_autoload_holds(state)) {
        DSD_FPRINTF(stderr, "the autoload still holds the typed key as its own\n");
        failed = 1;
    }
    free_test_state(state);
    return failed;
}

/* Whether the key copy holds zeros only. */
static int
key_copy_is_zero(const unsigned long long int words[6], const uint8_t flags[2]) {
    for (int i = 0; i < 6; i++) {
        if (words[i] != 0ULL) {
            return 0;
        }
    }
    return flags[0] == 0U && flags[1] == 0U;
}

/* The key readers fill every word and flag they are given on every path: with no state, the slot's call key and the
   autoload's preview are no key, and while the autoload does not hold, its baseline is no key (and it says so). A caller's uninitialized copy is
   never read back, and a copy that held an earlier key never keeps it. */
static int
test_key_readers_fill_their_outputs(void) {
    int failed = 0;
    unsigned long long int words[6];
    uint8_t flags[2];
    DSD_MEMSET(words, 0xA5, sizeof words);
    DSD_MEMSET(flags, 0xA5, sizeof flags);
    dsd_dmr_slot_call_key(NULL, 0U, 123U, words, flags);
    if (!key_copy_is_zero(words, flags)) {
        DSD_FPRINTF(stderr, "the slot call key with no state left its outputs unwritten\n");
        failed = 1;
    }
    DSD_MEMSET(words, 0xA5, sizeof words);
    DSD_MEMSET(flags, 0xA5, sizeof flags);
    keyring_dmr_tg_autoload_preview(NULL, 0U, 123U, words, flags);
    if (!key_copy_is_zero(words, flags)) {
        DSD_FPRINTF(stderr, "the autoload preview with no state left its outputs unwritten\n");
        failed = 1;
    }

    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }
    state->K = 70ULL;
    state->basic_key_present = 1U;
    DSD_MEMSET(words, 0xA5, sizeof words);
    DSD_MEMSET(flags, 0xA5, sizeof flags);
    if (dsd_dmr_tg_autoload_baseline(state, words, flags) != 0 || !key_copy_is_zero(words, flags)) {
        DSD_FPRINTF(stderr, "the autoload baseline while it does not hold is not an empty key\n");
        failed = 1;
    }
    DSD_MEMSET(words, 0xA5, sizeof words);
    DSD_MEMSET(flags, 0xA5, sizeof flags);
    if (dsd_dmr_tg_autoload_baseline(NULL, words, flags) != 0 || !key_copy_is_zero(words, flags)) {
        DSD_FPRINTF(stderr, "the autoload baseline with no state is not an empty key\n");
        failed = 1;
    }
    free_test_state(state);
    return failed;
}

int
main(void) {
    if (test_key_readers_fill_their_outputs() != 0) {
        return 1;
    }
    if (test_scan_keys_and_wipe_leave_no_autoloaded_key() != 0) {
        return 1;
    }
    if (test_direct_entry_keeps_no_autoloaded_key() != 0) {
        return 1;
    }
    if (test_key_set_install_wipes_slot_key_copies() != 0) {
        return 1;
    }
    if (test_autoload_miss_wipes_its_copies() != 0) {
        return 1;
    }
    if (test_free_state_wipes_keys() != 0) {
        return 1;
    }
    if (test_teardown_wipes_keystreams_and_opts_secrets() != 0) {
        return 1;
    }
    if (test_forget_slot_wipes_on_the_change_to_none() != 0) {
        return 1;
    }
    if (test_capture_install_roundtrip() != 0) {
        return 1;
    }
    if (test_load_hex_dec() != 0) {
        return 1;
    }
    if (test_load_direct() != 0) {
        return 1;
    }
    if (test_direct_key_store_growth() != 0) {
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
