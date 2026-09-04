// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE
#include <unistd.h>
#endif

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

static int
pick_missing_dir(char* out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return -1;
    }
    for (int i = 0; i < 1000; ++i) {
        (void)DSD_SNPRINTF(out, out_sz, "dsd-neo-test-missing-dir-%d", i);
        dsd_stat_t st;
        if (dsd_stat_path(out, &st) != 0) {
            return 0;
        }
    }
    return -1;
}

static void
free_test_state(dsd_state* state) {
    if (state) {
        dsd_state_ext_free_all(state);
        dsd_state_trunk_lcn_free(state);
    }
    free(state);
}

static int
test_group_import_missing_file(void) {
    // dsd_state is a multi-megabyte struct; avoid Windows' default ~1MB stack.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    char dir[128];
    if (pick_missing_dir(dir, sizeof dir) != 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    {
        dsd_tg_policy_entry row;
        if (dsd_tg_policy_make_exact_entry(123U, "A", "UNCHANGED", DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0
            || dsd_tg_policy_append_exact(state, &row) != 0) {
            free(opts);
            free_test_state(state);
            return 1;
        }
    }
    (void)DSD_SNPRINTF(opts->group_in_file, sizeof opts->group_in_file, "%s/%s", dir, "missing.csv");
    int rc = csvGroupImport(opts, state);
    if (rc == 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    {
        dsd_tg_policy_lookup lookup;
        if (dsd_tg_policy_lookup_id(state, 123U, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
            || strcmp(lookup.entry.name, "UNCHANGED") != 0) {
            free(opts);
            free_test_state(state);
            return 1;
        }
    }

    free(opts);
    free_test_state(state);
    return 0;
}

static int
test_channel_import_missing_file(void) {
    // dsd_state is a multi-megabyte struct; avoid Windows' default ~1MB stack.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    char dir[128];
    if (pick_missing_dir(dir, sizeof dir) != 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    state->lcn_freq_count = 456;
    (void)DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s/%s", dir, "missing.csv");
    int rc = csvChanImport(opts, state);
    if (rc == 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    if (state->lcn_freq_count != 456) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    free(opts);
    free_test_state(state);
    return 0;
}

static int
test_channel_import_rejects_directory(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    char dir[] = "dsd-neo-test-csv-dir-XXXXXX";
    if (dsd_mkdtemp(dir) == NULL) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    state->lcn_freq_count = 456;
    DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", dir);
    int rc = csvChanImport(opts, state);
    int failed = (rc == 0 || state->lcn_freq_count != 456);

    (void)remove(dir);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
test_decimal_key_import_and_group_hash(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    char tmpl[] = "dsd-neo-test-key-dec-XXXXXX";
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    FILE* fp = dsd_fopen_private(tmpl, "w");
    if (!fp) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }
    DSD_FPRINTF(fp, "key id or tg id (dec),key number or value (dec)\n");
    DSD_FPRINTF(fp, "2,70\n");
    DSD_FPRINTF(fp, "672560,254\n");
    fclose(fp);

    DSD_SNPRINTF(opts->key_in_file, sizeof opts->key_in_file, "%s", tmpl);
    int failed = 0;
    if (csvKeyImportDec(opts, state) != 0) {
        DSD_FPRINTF(stderr, "decimal key import returned error\n");
        failed = 1;
    }
    if (state->rkey_array[2] != 70ULL || state->rkey_array_loaded[2] != 1U) {
        DSD_FPRINTF(stderr, "decimal key import mismatch for key 2\n");
        failed = 1;
    }
    if (state->rkey_array[0x56F2] != 254ULL || state->rkey_array_loaded[0x56F2] != 1U) {
        DSD_FPRINTF(stderr, "decimal key import mismatch for hashed key 0x56F2\n");
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
test_hex_key_import_preserves_zero_segments_for_keyring(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    char tmpl[] = "dsd-neo-test-key-hex-XXXXXX";
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    FILE* fp = dsd_fopen_private(tmpl, "w");
    if (!fp) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }
    DSD_FPRINTF(fp, "key id(hex),key value (hex)\n");
    DSD_FPRINTF(fp, "C197,A753BC945DE5E0F1,0,D9DF2FAC6278FA93,0\n");
    fclose(fp);

    DSD_SNPRINTF(opts->key_in_file, sizeof opts->key_in_file, "%s", tmpl);
    int failed = 0;
    if (csvKeyImportHex(opts, state) != 0) {
        DSD_FPRINTF(stderr, "hex key import returned error\n");
        failed = 1;
    }

    const int key_id = 0xC197;
    if (state->rkey_array[key_id] != 0xA753BC945DE5E0F1ULL || state->rkey_array_loaded[key_id] != 1U) {
        DSD_FPRINTF(stderr, "hex key import mismatch for base segment\n");
        failed = 1;
    }
    if (state->rkey_array[key_id + 0x101] != 0ULL || state->rkey_array_loaded[key_id + 0x101] != 1U) {
        DSD_FPRINTF(stderr, "hex key import mismatch for zero second segment\n");
        failed = 1;
    }
    if (state->rkey_array[key_id + 0x201] != 0xD9DF2FAC6278FA93ULL || state->rkey_array_loaded[key_id + 0x201] != 1U) {
        DSD_FPRINTF(stderr, "hex key import mismatch for third segment\n");
        failed = 1;
    }
    if (state->rkey_array[key_id + 0x301] != 0ULL || state->rkey_array_loaded[key_id + 0x301] != 1U) {
        DSD_FPRINTF(stderr, "hex key import mismatch for zero fourth segment\n");
        failed = 1;
    }

    state->currentslot = 0;
    state->payload_keyid = key_id;
    keyring_activate_slot(opts, state, state->currentslot);
    if (state->R != 0xA753BC945DE5E0F1ULL || state->A1[0] != 0xA753BC945DE5E0F1ULL || state->A2[0] != 0ULL
        || state->A3[0] != 0xD9DF2FAC6278FA93ULL || state->A4[0] != 0ULL || state->aes_key_segments[0] != 4U
        || state->aes_key_loaded[0] != 1) {
        DSD_FPRINTF(stderr, "keyring mismatch for imported hex key\n");
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

#if !DSD_PLATFORM_WIN_NATIVE
static int
test_channel_import_rejects_final_symlink(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    const char* target = "dsd-neo-test-csv-symlink-target.csv";
    const char* link_name = "dsd-neo-test-csv-symlink-link.csv";
    (void)remove(link_name);
    (void)remove(target);

    FILE* fp = dsd_fopen_private(target, "w");
    if (!fp) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    DSD_FPRINTF(fp, "channel,freq\n1,851000000\n");
    fclose(fp);

    if (symlink(target, link_name) != 0) {
        (void)remove(target);
        free(opts);
        free_test_state(state);
        return 1;
    }

    state->lcn_freq_count = 456;
    DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", link_name);
    int rc = csvChanImport(opts, state);
    int failed = (rc == 0 || state->lcn_freq_count != 456);

    (void)remove(link_name);
    (void)remove(target);
    free(opts);
    free_test_state(state);
    return failed;
}
#endif

static int
test_group_import_large_exact_file(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    char tmpl[] = "dsd-neo-test-group-overflow-XXXXXX";
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    FILE* fp = dsd_fopen_private(tmpl, "w");
    if (!fp) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }

    DSD_FPRINTF(fp, "group,mode,name\n");
    const size_t rows = 1048;
    for (size_t i = 0; i < rows; i++) {
        DSD_FPRINTF(fp, "%zu,D,Alias %zu\n", i + 1, i + 1);
    }
    fclose(fp);

    (void)DSD_SNPRINTF(opts->group_in_file, sizeof opts->group_in_file, "%s", tmpl);
    int rc = csvGroupImport(opts, state);

    int failed = 0;
    dsd_tg_policy_lookup lookup;
    if (rc != 0) {
        failed = 1;
    }
    if (dsd_tg_policy_lookup_id(state, 1U, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
        || strcmp(lookup.entry.name, "Alias 1") != 0) {
        failed = 1;
    }
    if (dsd_tg_policy_lookup_id(state, (uint32_t)rows, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
        || strcmp(lookup.entry.name, "Alias 1048") != 0) {
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
test_group_import_large_file_policy(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_tg_policy_lookup lookup;
    dsd_tg_policy_decision decision;
    char tmpl[] = "dsd-neo-test-group-large-XXXXXX";
    int fd = -1;
    const size_t rows = 6864;
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    {
        FILE* fp = dsd_fopen_private(tmpl, "w");
        if (!fp) {
            (void)remove(tmpl);
            free(opts);
            free_test_state(state);
            return 1;
        }
        DSD_FPRINTF(fp, "id,mode,name\n");
        for (size_t i = 1; i <= rows; i++) {
            if (i == 6500) {
                DSD_FPRINTF(fp, "%zu,A,Late Allow\n", i);
            } else if (i == rows) {
                DSD_FPRINTF(fp, "%zu,B,This Alias Name Is Definitely Longer Than Forty Nine Characters For Safety\n",
                            i);
            } else {
                DSD_FPRINTF(fp, "%zu,A,Alias %zu\n", i, i);
            }
        }
        fclose(fp);
    }

    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->trunk_tune_enc_calls = 1;
    opts->trunk_use_allow_list = 1;
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", tmpl);

    if (csvGroupImport(opts, state) != 0) {
        failed = 1;
    }
    if (dsd_tg_policy_lookup_id(state, 6500U, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
        || strcmp(lookup.entry.name, "Late Allow") != 0) {
        failed = 1;
    }
    if (dsd_tg_policy_evaluate_group_call(opts, state, 6500U, 1U, 0, 0, &decision) != 0
        || decision.match != DSD_TG_POLICY_MATCH_EXACT || decision.tune_allowed != 1) {
        failed = 1;
    }
    if (dsd_tg_policy_evaluate_group_call(opts, state, 7000U, 1U, 0, 0, &decision) != 0 || decision.tune_allowed != 0
        || (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) == 0) {
        failed = 1;
    }
    if (dsd_tg_policy_lookup_id(state, (uint32_t)rows, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
        || lookup.entry.name[sizeof(lookup.entry.name) - 1] != '\0'
        || strlen(lookup.entry.name) != sizeof(lookup.entry.name) - 1) {
        failed = 1;
    }
    if (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)rows, 1U, 0, 0, &decision) != 0
        || decision.tune_allowed != 0 || (decision.block_reasons & DSD_TG_POLICY_BLOCK_MODE) == 0) {
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
write_text_file(const char* path, const char* text) {
    FILE* fp = dsd_fopen_private(path, "w");
    if (!fp) {
        return -1;
    }
    if (fputs(text, fp) < 0) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int
test_channel_import_rejects_malformed_rows_without_reusing_previous_channel(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    char tmpl[] = "dsd-neo-test-channel-malformed-XXXXXX";
    int fd = -1;

    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    if (write_text_file(tmpl, "channel,freq\n"
                              "12,851000000\n"
                              "bad,852000000\n"
                              "13,badfreq\n"
                              "14,853000000\n"
                              "65535,854000000\n")
        != 0) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }

    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", tmpl);
    if (csvChanImport(opts, state) != 0) {
        failed = 1;
    }
    if (state->trunk_chan_map[12] != 851000000L) {
        failed = 1;
    }
    if (state->trunk_chan_map[13] != 0L) {
        failed = 1;
    }
    if (state->trunk_chan_map[14] != 853000000L) {
        failed = 1;
    }
    if (state->trunk_chan_map_used_count != 2U || state->trunk_chan_map_used[0] != 12U
        || state->trunk_chan_map_used[1] != 14U) {
        failed = 1;
    }
    if (state->lcn_freq_count != 3 || state->trunk_lcn_freq[0] != 851000000L || state->trunk_lcn_freq[1] != 0L
        || state->trunk_lcn_freq[2] != 853000000L) {
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
test_channel_import_extends_past_26_entries(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    char tmpl[] = "dsd-neo-test-channel-large-XXXXXX";
    int fd = -1;

    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    // 40 data rows: every row takes its positional LCN slot, so slots run
    // past the 26 embedded trunk_lcn_freq entries into the heap tail. Channel
    // 2 carries a garbage frequency to prove the 0-placeholder pattern
    // (existing test) survives on both sides of the 26-entry boundary.
    char body[2048];
    body[0] = '\0';
    (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body), "channel,frequency\n");
    for (int i = 1; i <= 40; i++) {
        if (i == 2) {
            (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body), "%d,notafrequency\n", i);
        } else {
            (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body), "%d,%ld\n", i,
                               851000000L + (long)(i - 1) * 12500L);
        }
    }
    if (write_text_file(tmpl, body) != 0) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }

    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", tmpl);
    if (csvChanImport(opts, state) != 0) {
        DSD_FPRINTF(stderr, "40-row channel import returned error\n");
        failed = 1;
    }
    if (state->lcn_freq_count != 40 || state->trunk_lcn_freq_ext == NULL || state->trunk_lcn_freq_ext_capacity < 14U) {
        DSD_FPRINTF(stderr, "channel import tail state wrong count=%d ext=%p cap=%zu\n", state->lcn_freq_count,
                    (void*)state->trunk_lcn_freq_ext, state->trunk_lcn_freq_ext_capacity);
        failed = 1;
    }
    if (state->trunk_lcn_freq[0] != 851000000L || state->trunk_lcn_freq[1] != 0L
        || state->trunk_lcn_freq[2] != 851025000L) {
        DSD_FPRINTF(stderr, "embedded LCN slot pattern broken\n");
        failed = 1;
    }
    if (*dsd_state_trunk_lcn_slot(state, 26) != 851325000L || *dsd_state_trunk_lcn_slot(state, 39) != 851487500L) {
        DSD_FPRINTF(stderr, "ext LCN tail rows 27/40 mismatch\n");
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

/* Names are stored by LCN row index, not by channel number: a row that took no
 * scan-list slot stores nothing, and a 0-placeholder row keeps its name, so the
 * name index and the slot index stay equal by construction. */
static int
test_channel_import_name_column_stores_names_by_row_index(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    char tmpl[] = "dsd-neo-test-channel-names-XXXXXX";
    char long_name[71];
    char body[4096];
    int fd = -1;

    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    DSD_MEMSET(long_name, 'N', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    body[0] = '\0';
    (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body),
                       "channel,frequency,name\n"
                       "1,851000000,  Dispatch  \n"
                       "2,notafrequency,Placeholder\n"
                       "bad,852000000,NoSlot\n"
                       "3,852000000,\n");
    for (int i = 4; i <= 40; i++) {
        if (i == 40) {
            (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body), "%d,%ld,%s\n", i,
                               851000000L + (long)(i - 1) * 12500L, long_name);
        } else {
            (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body), "%d,%ld,Row%d\n", i,
                               851000000L + (long)(i - 1) * 12500L, i);
        }
    }
    if (write_text_file(tmpl, body) != 0) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }

    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", tmpl);
    if (csvChanImport(opts, state) != 0) {
        DSD_FPRINTF(stderr, "named channel import returned error\n");
        failed = 1;
    }
    if (state->lcn_freq_count != 40) {
        DSD_FPRINTF(stderr, "named import slot count wrong: %d\n", state->lcn_freq_count);
        failed = 1;
    }
    if (strcmp(dsd_state_trunk_lcn_name_get(state, 0), "Dispatch") != 0) {
        DSD_FPRINTF(stderr, "row 1 name not trimmed: '%s'\n", dsd_state_trunk_lcn_name_get(state, 0));
        failed = 1;
    }
    // The unparseable frequency still takes its positional slot, so its name rides along.
    if (strcmp(dsd_state_trunk_lcn_name_get(state, 1), "Placeholder") != 0) {
        DSD_FPRINTF(stderr, "0-placeholder row lost its name: '%s'\n", dsd_state_trunk_lcn_name_get(state, 1));
        failed = 1;
    }
    // 'bad,852000000,NoSlot' took no slot, so index 2 belongs to the row after it.
    if (dsd_state_trunk_lcn_name_get(state, 2)[0] != '\0') {
        DSD_FPRINTF(stderr, "empty name column stored something: '%s'\n", dsd_state_trunk_lcn_name_get(state, 2));
        failed = 1;
    }
    if (strcmp(dsd_state_trunk_lcn_name_get(state, 3), "Row4") != 0
        || strcmp(dsd_state_trunk_lcn_name_get(state, 26), "Row27") != 0) {
        DSD_FPRINTF(stderr, "row-index names wrong: [3]='%s' [26]='%s'\n", dsd_state_trunk_lcn_name_get(state, 3),
                    dsd_state_trunk_lcn_name_get(state, 26));
        failed = 1;
    }
    if (strlen(dsd_state_trunk_lcn_name_get(state, 39)) != (size_t)(DSD_CHANNEL_LABEL_SIZE - 1)) {
        DSD_FPRINTF(stderr, "long name not truncated: len=%zu\n", strlen(dsd_state_trunk_lcn_name_get(state, 39)));
        failed = 1;
    }
    if (dsd_state_trunk_lcn_name_get(state, 40)[0] != '\0') {
        DSD_FPRINTF(stderr, "name past the last row is not empty\n");
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

/* Names reach a terminal and a fixed-size buffer: a control byte becomes a space
 * and is then trimmed like any other padding, and the byte cap never cuts a
 * UTF-8 sequence in half. */
static int
test_channel_import_name_column_sanitizes_names(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    char tmpl[] = "dsd-neo-test-channel-sanitize-XXXXXX";
    // 61 ASCII bytes then a 3-byte euro sign: the 63-byte cap lands inside it.
    char utf8_name[65];
    char body[512];
    int fd = -1;

    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    DSD_MEMSET(utf8_name, 'A', 61);
    DSD_MEMCPY(utf8_name + 61, "\xE2\x82\xAC", 3);
    utf8_name[64] = '\0';

    body[0] = '\0';
    (void)DSD_SNPRINTF(body, sizeof(body),
                       "channel,frequency,name\n"
                       "1,851000000,\x01"
                       "Dispatch\n"
                       "2,851012500,Ops\tTwo\n"
                       "3,851025000,%s\n",
                       utf8_name);
    if (write_text_file(tmpl, body) != 0) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }

    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", tmpl);
    if (csvChanImport(opts, state) != 0) {
        DSD_FPRINTF(stderr, "sanitising channel import returned error\n");
        failed = 1;
    }
    // A leading control byte becomes a space, and the trim that follows removes it.
    if (strcmp(dsd_state_trunk_lcn_name_get(state, 0), "Dispatch") != 0) {
        DSD_FPRINTF(stderr, "leading control byte left in place: '%s'\n", dsd_state_trunk_lcn_name_get(state, 0));
        failed = 1;
    }
    if (strcmp(dsd_state_trunk_lcn_name_get(state, 1), "Ops Two") != 0) {
        DSD_FPRINTF(stderr, "interior tab not replaced: '%s'\n", dsd_state_trunk_lcn_name_get(state, 1));
        failed = 1;
    }
    {
        const char* stored = dsd_state_trunk_lcn_name_get(state, 2);
        const size_t stored_len = strlen(stored);
        if (stored_len != 61 || strspn(stored, "A") != stored_len) {
            DSD_FPRINTF(stderr, "utf-8 name not cut at a character boundary: len=%zu '%s'\n", stored_len, stored);
            failed = 1;
        }
        // Belt and braces: whatever the length, the last byte must start a character.
        if (stored_len == 0 || ((unsigned char)stored[stored_len - 1] & 0xC0U) == 0x80U) {
            DSD_FPRINTF(stderr, "stored name ends on a UTF-8 continuation byte\n");
            failed = 1;
        }
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

/* The channel map's extra columns have always been free-text notes -- two shipped
 * examples put commas in theirs -- so only a header that names column 3 `name`
 * turns it into a label. */
static int
test_channel_import_name_column_requires_header_opt_in(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    char tmpl[] = "dsd-neo-test-channel-optin-XXXXXX";
    int fd = -1;

    static const struct {
        const char* text;
        int expect_stored;
    } cases[] = {
        // Column 3 is a note unless the header says otherwise.
        {"channel,frequency,note\n1,851000000,Dispatch\n", 0},
        // The opt-in ignores case and surrounding spaces.
        {"Channel, Freq, Name \n1,851000000,Dispatch\n", 1},
        {"CHANNEL,FREQ,NAME\n1,851000000,Dispatch\n", 1},
        // Opted in, but the rows carry no third column.
        {"channel,frequency,name\n1,851000000\n", 0},
        // Opted in, but every name is blank.
        {"channel,frequency,name\n1,851000000,\n", 0},
    };

    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);
    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", tmpl);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dsd_state_trunk_lcn_free(state);
        DSD_MEMSET(state, 0, sizeof(*state));
        if (write_text_file(tmpl, cases[i].text) != 0) {
            failed = 1;
            break;
        }
        if (csvChanImport(opts, state) != 0) {
            DSD_FPRINTF(stderr, "opt-in case %zu import returned error\n", i);
            failed = 1;
            continue;
        }
        // Whether the column is a label or a note, the map and the scan list load the same.
        if (state->lcn_freq_count != 1 || state->trunk_lcn_freq[0] != 851000000L
            || state->trunk_chan_map[1] != 851000000L) {
            DSD_FPRINTF(stderr, "opt-in case %zu changed the map/LCN load\n", i);
            failed = 1;
        }
        if (cases[i].expect_stored) {
            if (strcmp(dsd_state_trunk_lcn_name_get(state, 0), "Dispatch") != 0) {
                DSD_FPRINTF(stderr, "opt-in case %zu did not store the name: '%s'\n", i,
                            dsd_state_trunk_lcn_name_get(state, 0));
                failed = 1;
            }
        } else if (state->trunk_lcn_name != NULL) {
            // A file that never names a row must not pay for the store at all.
            DSD_FPRINTF(stderr, "opt-in case %zu allocated a name store\n", i);
            failed = 1;
        }
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
test_group_import_policy_and_basic_headers(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_tg_policy_lookup lookup;
    char tmpl[] = "dsd-neo-test-group-policy-XXXXXX";
    int fd = -1;
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    if (write_text_file(tmpl, "id,mode,name,tag\n100,B,LOCK,90,true,on,on,on\n101,A,ALLOW,meta\n") != 0) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", tmpl);
    if (csvGroupImport(opts, state) != 0) {
        failed = 1;
    }
    if (dsd_tg_policy_lookup_id(state, 100, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
        || lookup.entry.priority != 0 || lookup.entry.preempt != 0 || lookup.entry.audio != 0) {
        failed = 1;
    }
    if (dsd_tg_policy_lookup_id(state, 101, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
        || lookup.entry.priority != 0 || lookup.entry.preempt != 0 || lookup.entry.audio != 1) {
        failed = 1;
    }

    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    if (write_text_file(tmpl, "id,mode,name,priority,preempt,audio,record,stream,tags\n"
                              "200,A,Fire,90,true,on,on,on,fire\n"
                              "201,A,Ops,,true,,,off,ops\n"
                              "202,B,Block,10,false,on,on,on,x\n"
                              "203,A,AudioOff,0,false,off,on,on,x\n"
                              "0,A,Zero,1,false,on,on,on,z\n"
                              "305,B,Exact,0,false,off,off,off,e\n"
                              "300-399,A,Range,70,true,on,on,on,r\n")
        != 0) {
        failed = 1;
    } else {
        if (csvGroupImport(opts, state) != 0) {
            failed = 1;
        }
        if (dsd_tg_policy_lookup_id(state, 200, &lookup) != 0 || lookup.entry.priority != 90
            || lookup.entry.preempt != 1 || lookup.entry.audio != 1 || lookup.entry.record != 1
            || lookup.entry.stream != 1) {
            failed = 1;
        }
        if (dsd_tg_policy_lookup_id(state, 201, &lookup) != 0 || lookup.entry.priority != 0 || lookup.entry.preempt != 1
            || lookup.entry.audio != 1 || lookup.entry.record != 1 || lookup.entry.stream != 0) {
            failed = 1;
        }
        if (dsd_tg_policy_lookup_id(state, 202, &lookup) != 0 || lookup.entry.audio != 0 || lookup.entry.record != 0
            || lookup.entry.stream != 0) {
            failed = 1;
        }
        if (dsd_tg_policy_lookup_id(state, 203, &lookup) != 0 || lookup.entry.audio != 0 || lookup.entry.record != 0
            || lookup.entry.stream != 0) {
            failed = 1;
        }
        if (dsd_tg_policy_lookup_id(state, 0, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT) {
            failed = 1;
        }
        if (dsd_tg_policy_lookup_id(state, 305, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
            || strcmp(lookup.entry.name, "Exact") != 0) {
            failed = 1;
        }
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
test_group_import_invalid_ids_and_required_fields(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_tg_policy_lookup lookup;
    char tmpl[] = "dsd-neo-test-group-invalid-XXXXXX";
    int fd = -1;
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    if (write_text_file(tmpl, "id,mode,name,priority,preempt,audio,record,stream,tags\n"
                              ",A,NoId\n"
                              "1201,A\n"
                              "\n"
                              "-1,A,Neg\n"
                              "+1,A,Plus\n"
                              "123abc,A,Partial\n"
                              "1300-,A,OpenEnd\n"
                              "-1399,A,OpenStart\n"
                              "1400-1300,A,Reversed\n"
                              "4294967296,A,TooBig\n"
                              "1-4294967296,A,TooBigRange\n"
                              "400,A,Valid\n")
        != 0) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", tmpl);
    if (csvGroupImport(opts, state) != 0) {
        failed = 1;
    }
    if (dsd_tg_policy_lookup_id(state, 400U, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT
        || strcmp(lookup.entry.name, "Valid") != 0) {
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
test_group_import_range_after_many_exact_rows(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_tg_policy_lookup lookup;
    char tmpl[] = "dsd-neo-test-group-range-after-cap-XXXXXX";
    int fd = -1;
    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    {
        FILE* fp = dsd_fopen_private(tmpl, "w");
        const size_t exact_rows = 1031;
        if (!fp) {
            (void)remove(tmpl);
            free(opts);
            free_test_state(state);
            return 1;
        }
        DSD_FPRINTF(fp, "id,mode,name,priority,preempt,audio,record,stream,tags\n");
        for (size_t i = 0; i < exact_rows; i++) {
            DSD_FPRINTF(fp, "%zu,D,Alias %zu,0,false,on,on,on,x\n", i + 1, i + 1);
        }
        DSD_FPRINTF(fp, "5000-5005,A,Range,70,true,on,on,on,r\n");
        fclose(fp);
    }

    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", tmpl);
    if (csvGroupImport(opts, state) != 0) {
        failed = 1;
    }
    if (dsd_tg_policy_lookup_id(state, 1031, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT) {
        failed = 1;
    }
    if (dsd_tg_policy_lookup_id(state, 5003, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_RANGE) {
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static unsigned
bits_to_u8(const char* bits, int start) {
    unsigned v = 0U;
    for (int i = 0; i < 8; i++) {
        v = (v << 1) | (unsigned)(bits[start + i] & 1);
    }
    return v;
}

static int
frame_all_zero(const char bits[49]) {
    for (int i = 0; i < 49; i++) {
        if ((bits[i] & 1) != 0) {
            return 0;
        }
    }
    return 1;
}

static int
test_vertex_import_missing_file(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    char dir[128];
    if (pick_missing_dir(dir, sizeof dir) != 0) {
        free_test_state(state);
        return 1;
    }

    state->vertex_ks_count = 7;
    int rc = csvVertexKsImport(state, dir);
    if (rc == 0) {
        free_test_state(state);
        return 1;
    }
    if (state->vertex_ks_count != 7) {
        free_test_state(state);
        return 1;
    }

    free_test_state(state);
    return 0;
}

static int
test_vertex_import_and_apply(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    /*
     * Build a minimal Vertex keystream CSV on disk.
     * The rows exercise frame-stepped, repeating, and zero-key mappings.
     * Applying the rows below verifies import state and frame output together.
     */
    char tmpl[] = "dsd-neo-test-vertex-ks-XXXXXX";
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    FILE* fp = dsd_fopen_private(tmpl, "w");
    if (!fp) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    DSD_FPRINTF(fp, "key_hex,keystream_spec\n");
    DSD_FPRINTF(fp, "1234567891,8:F0:2:3\n");
    DSD_FPRINTF(fp, "ABCDEF,8:0F\n");
    DSD_FPRINTF(fp, "0,8:AA\n");
    fclose(fp);

    int rc = csvVertexKsImport(state, tmpl);
    if (rc != 0 || state->vertex_ks_count != 3) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (state->vertex_ks_key[0] != 0x1234567891ULL || state->vertex_ks_mod[0] != 8
        || state->vertex_ks_frame_mode[0] != 1 || state->vertex_ks_frame_off[0] != 2
        || state->vertex_ks_frame_step[0] != 3) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }

    char frame0[49];
    char frame1[49];
    char frame_slot1[49];
    char frame_skip[49];
    char frame2[49];
    char frame_zero_key[49];
    DSD_MEMSET(frame0, 0, sizeof(frame0));
    DSD_MEMSET(frame1, 0, sizeof(frame1));
    DSD_MEMSET(frame_slot1, 0, sizeof(frame_slot1));
    DSD_MEMSET(frame_skip, 0, sizeof(frame_skip));
    DSD_MEMSET(frame2, 0, sizeof(frame2));
    DSD_MEMSET(frame_zero_key, 0, sizeof(frame_zero_key));
    frame0[24] = 1;
    frame1[24] = 1;
    frame_slot1[24] = 1;
    frame2[24] = 1;
    frame_zero_key[24] = 1;

    // Repeated application advances only the configured frame-stepped mapping.
    if (vertex_key_map_apply_frame49(state, 0, 0x1234567891ULL, frame0) != 1) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (vertex_key_map_apply_frame49(state, 0, 0x1234567891ULL, frame1) != 1) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (vertex_key_map_apply_frame49(state, 1, 0x1234567891ULL, frame_slot1) != 1) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (bits_to_u8(frame0, 0) != 0xC3U || bits_to_u8(frame1, 0) != 0x1EU || bits_to_u8(frame_slot1, 0) != 0xC3U) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (vertex_key_map_apply_frame49(state, 0, 0x1234567891ULL, frame_skip) != 1) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (frame_all_zero(frame_skip) != 1 || state->vertex_ks_counter[0] != 3) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }

    if (vertex_key_map_apply_frame49(state, 0, 0xABCDEFULL, frame2) != 1) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (bits_to_u8(frame2, 0) != 0x0FU) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }

    if (vertex_key_map_apply_frame49(state, 0, 0ULL, frame_zero_key) != 1) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (bits_to_u8(frame_zero_key, 0) != 0xAAU) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }

    char unknown[49];
    DSD_MEMSET(unknown, 0, sizeof(unknown));
    // Unknown keys must leave the destination frame untouched.
    if (vertex_key_map_apply_frame49(state, 0, 0x999999ULL, unknown) != 0) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (bits_to_u8(unknown, 0) != 0x00U) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }

    (void)remove(tmpl);
    free_test_state(state);
    return 0;
}

static int
write_tg_key_csv(char* tmpl, size_t tmpl_size, const char* rows) {
    DSD_SNPRINTF(tmpl, tmpl_size, "%s", "dsd-neo-test-tg-key-XXXXXX");
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);
    FILE* fp = dsd_fopen_private(tmpl, "w");
    if (!fp) {
        (void)remove(tmpl);
        return -1;
    }
    DSD_FPRINTF(fp, "tg (dec),keyid (hex)\n");
    DSD_FPRINTF(fp, "%s", rows);
    fclose(fp);
    return 0;
}

static int
test_dmr_tg_key_import_and_lookup(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    // The duplicate TG row must replace the earlier mapping, not add a row.
    char tmpl[64];
    if (write_tg_key_csv(tmpl, sizeof tmpl, "123,7B\n4567,03\n123,1F\n") != 0) {
        free_test_state(state);
        return 1;
    }

    int failed = 0;
    uint8_t kid = 0;
    if (csvDmrTgKeyImport(state, tmpl) != 0 || state->dmr_tg_key_map_count != 2) {
        failed = 1;
    }
    if (keyring_dmr_tg_map_kid(state, 123U, &kid) != 1 || kid != 0x1F) {
        failed = 1;
    }
    if (keyring_dmr_tg_map_kid(state, 4567U, &kid) != 1 || kid != 0x03) {
        failed = 1;
    }
    if (keyring_dmr_tg_map_kid(state, 999U, &kid) != 0) {
        failed = 1;
    }

    (void)remove(tmpl);
    free_test_state(state);
    return failed;
}

static int
test_dmr_tg_key_import_rejects_bad_rows(void) {
    // Each malformed file must fail the import and leave the map untouched.
    static const char* bad_rows[] = {
        "123,100\n",     // key id above the DMR 8-bit range
        "0,7B\n",        // talkgroup zero
        "16777216,7B\n", // talkgroup above 24 bits
        "abc,7B\n",      // non-numeric talkgroup
        "123\n",         // missing key id column
        "",              // header only, no mappings
    };

    for (size_t i = 0; i < sizeof(bad_rows) / sizeof(bad_rows[0]); i++) {
        dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
        if (!state) {
            return 1;
        }
        char tmpl[64];
        if (write_tg_key_csv(tmpl, sizeof tmpl, bad_rows[i]) != 0) {
            free_test_state(state);
            return 1;
        }
        // Seed a mapping first, or "leaves the map untouched" is unfalsifiable: a fresh state
        // already has count 0, so a rejecting import that wiped the live map would still pass.
        state->dmr_tg_key_map_tg[0] = 999U;
        state->dmr_tg_key_map_kid[0] = 0x11;
        state->dmr_tg_key_map_count = 1;
        int failed = 0;
        if (csvDmrTgKeyImport(state, tmpl) == 0 || state->dmr_tg_key_map_count != 1
            || state->dmr_tg_key_map_tg[0] != 999U || state->dmr_tg_key_map_kid[0] != 0x11) {
            DSD_FPRINTF(stderr, "tg-key bad row case %zu not rejected\n", i);
            failed = 1;
        }
        (void)remove(tmpl);
        free_test_state(state);
        if (failed) {
            return 1;
        }
    }
    return 0;
}

static int
test_dmr_tg_key_import_missing_file(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    char dir[128];
    if (pick_missing_dir(dir, sizeof dir) != 0) {
        free_test_state(state);
        return 1;
    }

    state->dmr_tg_key_map_count = 5;
    int failed = 0;
    if (csvDmrTgKeyImport(state, dir) == 0 || state->dmr_tg_key_map_count != 5) {
        failed = 1;
    }

    free_test_state(state);
    return failed;
}

/* Per-row key columns opt in by header name, in either order, with or without
 * `name`: a non-blank cell loads into the row's slot-indexed key set, a blank
 * cell stores nothing, and a row that took no slot stores nothing. The live
 * keyring is untouched; the sets install on the `-Y` hop. */
/* A key cell resolves against the map file's directory, not the working directory:
 * the map sits in its own directory and names its key file through a subdirectory. */
static int
test_channel_import_row_key_relative_subdir(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_state* scratch = (dsd_state*)calloc(1, sizeof(*scratch));
    char dir_tmpl[] = "dsd-neo-test-rowkey-dir-XXXXXX";
    char sub_dir[256];
    char key_path[256];
    char map_path[256];
    int fd = -1;

    if (!opts || !state || !scratch) {
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    /* mkstemp reserves a unique name; reuse it for the directory. */
    fd = dsd_mkstemp(dir_tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(dir_tmpl);
    (void)DSD_SNPRINTF(sub_dir, sizeof(sub_dir), "%s/sub", dir_tmpl);
    (void)DSD_SNPRINTF(key_path, sizeof(key_path), "%s/keys.csv", sub_dir);
    (void)DSD_SNPRINTF(map_path, sizeof(map_path), "%s/map.csv", dir_tmpl);
    if (dsd_mkdir(dir_tmpl, 0700) != 0 || dsd_mkdir(sub_dir, 0700) != 0
        || write_text_file(key_path, "key id(hex),key value (hex)\n0010,AAAAAAAAAAAAAAAA\n") != 0
        || write_text_file(map_path, "channel,frequency_hz,keys_hex_csv\n"
                                     "1,851000000,sub/keys.csv\n")
               != 0) {
        (void)remove(key_path);
        (void)remove(map_path);
        (void)remove(sub_dir);
        (void)remove(dir_tmpl);
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }

    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", map_path);
    if (csvChanImport(opts, state) != 0) {
        DSD_FPRINTF(stderr, "relative-subdir row-key import returned error\n");
        failed = 1;
    }
    const dsd_key_set* row0 = dsd_state_trunk_lcn_keys_get(state, 0);
    if (!row0 || row0->present == 0) {
        DSD_FPRINTF(stderr, "relative-subdir row stored no key set\n");
        failed = 1;
    } else {
        dsd_key_set_install(scratch, row0);
        if (scratch->rkey_array[0x10] != 0xAAAAAAAAAAAAAAAAULL || scratch->rkey_array_loaded[0x10] != 1U) {
            DSD_FPRINTF(stderr, "relative-subdir row missed its key entry\n");
            failed = 1;
        }
    }

    (void)remove(key_path);
    (void)remove(map_path);
    (void)remove(sub_dir);
    (void)remove(dir_tmpl);
    free(opts);
    free_test_state(state);
    free_test_state(scratch);
    return failed;
}

static int
test_channel_import_row_key_columns(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_state* scratch = (dsd_state*)calloc(1, sizeof(*scratch));
    char hex_tmpl[] = "dsd-neo-test-rowkey-hex-XXXXXX";
    char dec_tmpl[] = "dsd-neo-test-rowkey-dec-XXXXXX";
    char map_tmpl[] = "dsd-neo-test-rowkey-map-XXXXXX";
    char body[4096];
    int fd = -1;

    if (!opts || !state || !scratch) {
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    fd = dsd_mkstemp(hex_tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    (void)dsd_close(fd);
    fd = dsd_mkstemp(dec_tmpl);
    if (fd < 0) {
        (void)remove(hex_tmpl);
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    (void)dsd_close(fd);
    fd = dsd_mkstemp(map_tmpl);
    if (fd < 0) {
        (void)remove(hex_tmpl);
        (void)remove(dec_tmpl);
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    (void)dsd_close(fd);
    if (write_text_file(hex_tmpl,
                        "key id(hex),key value (hex)\n0010,AAAAAAAAAAAAAAAA,0,CCCCCCCCCCCCCCCC,DDDDDDDDDDDDDDDD\n")
            != 0
        || write_text_file(dec_tmpl, "key id or tg id (dec),key number or value (dec)\n20,12345\n") != 0) {
        (void)remove(hex_tmpl);
        (void)remove(dec_tmpl);
        (void)remove(map_tmpl);
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }

    body[0] = '\0';
    (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body),
                       "channel,frequency_hz,name,keys_hex_csv,keys_dec_csv\n"
                       "1,851000000,Dispatch,%s,%s\n"
                       "2,851012500,Ops,,\n"
                       "bad,851025000,NoSlot,%s,%s\n"
                       "3,851025000,Fire,,%s\n",
                       hex_tmpl, dec_tmpl, hex_tmpl, dec_tmpl, dec_tmpl);
    if (write_text_file(map_tmpl, body) != 0) {
        (void)remove(hex_tmpl);
        (void)remove(dec_tmpl);
        (void)remove(map_tmpl);
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", map_tmpl);
    if (csvChanImport(opts, state) != 0) {
        DSD_FPRINTF(stderr, "row-key channel import returned error\n");
        failed = 1;
    }
    if (state->lcn_freq_count != 3) {
        DSD_FPRINTF(stderr, "row-key slot count wrong: %d\n", state->lcn_freq_count);
        failed = 1;
    }
    if (!dsd_state_trunk_lcn_keys_present(state)) {
        DSD_FPRINTF(stderr, "row-key store reports empty after a keyed import\n");
        failed = 1;
    }
    {
        const dsd_key_set* row0 = dsd_state_trunk_lcn_keys_get(state, 0);
        if (!row0 || row0->present == 0) {
            DSD_FPRINTF(stderr, "row 1 stored no key set\n");
            failed = 1;
        } else {
            dsd_key_set_install(scratch, row0);
            if (scratch->rkey_array[0x10] != 0xAAAAAAAAAAAAAAAAULL || scratch->rkey_array_loaded[0x10] != 1U) {
                DSD_FPRINTF(stderr, "row 1 missed the hex base segment\n");
                failed = 1;
            }
            if (scratch->rkey_array[0x10 + 0x101] != 0ULL || scratch->rkey_array_loaded[0x10 + 0x101] != 1U) {
                DSD_FPRINTF(stderr, "row 1 missed the hex zero segment\n");
                failed = 1;
            }
            if (scratch->rkey_array[20] != 12345ULL || scratch->rkey_array_loaded[20] != 1U) {
                DSD_FPRINTF(stderr, "row 1 missed the dec entry\n");
                failed = 1;
            }
            if (scratch->keyloader != 1) {
                DSD_FPRINTF(stderr, "row 1 did not arm keyloader\n");
                failed = 1;
            }
        }
    }
    if (dsd_state_trunk_lcn_keys_get(state, 1) != NULL) {
        DSD_FPRINTF(stderr, "blank key cells stored a set on row 2\n");
        failed = 1;
    }
    // 'bad,...' took no slot, so index 1 belongs to row 2 and index 2 to row 3.
    {
        const dsd_key_set* row3 = dsd_state_trunk_lcn_keys_get(state, 2);
        if (!row3 || row3->present == 0) {
            DSD_FPRINTF(stderr, "row 3 stored no key set\n");
            failed = 1;
        } else {
            DSD_MEMSET(scratch->rkey_array, 0, sizeof(scratch->rkey_array));
            DSD_MEMSET(scratch->rkey_array_loaded, 0, sizeof(scratch->rkey_array_loaded));
            dsd_key_set_install(scratch, row3);
            if (scratch->rkey_array[20] != 12345ULL) {
                DSD_FPRINTF(stderr, "row 3 missed the dec-only entry\n");
                failed = 1;
            }
            if (scratch->rkey_array[0x10] != 0ULL) {
                DSD_FPRINTF(stderr, "dec-only row 3 carries hex material\n");
                failed = 1;
            }
        }
    }
    if (state->rkey_array[0x10] != 0ULL || state->rkey_array[20] != 0ULL) {
        DSD_FPRINTF(stderr, "row-key import leaked into the live keyring\n");
        failed = 1;
    }
    if (strcmp(dsd_state_trunk_lcn_name_get(state, 0), "Dispatch") != 0) {
        DSD_FPRINTF(stderr, "name column broke beside key columns: '%s'\n", dsd_state_trunk_lcn_name_get(state, 0));
        failed = 1;
    }

    // Reversed column order without `name` opts in the same way.
    dsd_state_ext_free_all(state);
    dsd_state_trunk_lcn_free(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    body[0] = '\0';
    (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body),
                       "channel,frequency_hz,keys_dec_csv,keys_hex_csv\n"
                       "1,851000000,%s,%s\n",
                       dec_tmpl, hex_tmpl);
    if (write_text_file(map_tmpl, body) != 0) {
        (void)remove(hex_tmpl);
        (void)remove(dec_tmpl);
        (void)remove(map_tmpl);
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    if (csvChanImport(opts, state) != 0 || dsd_state_trunk_lcn_keys_get(state, 0) == NULL) {
        DSD_FPRINTF(stderr, "reversed key columns did not opt in\n");
        failed = 1;
    }

    // An unloadable key path fails the whole import, like a bad `-K`.
    dsd_state_ext_free_all(state);
    dsd_state_trunk_lcn_free(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    body[0] = '\0';
    (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body),
                       "channel,frequency_hz,keys_hex_csv\n"
                       "1,851000000,dsd-neo-test-missing-dir/missing.csv\n");
    if (write_text_file(map_tmpl, body) != 0) {
        (void)remove(hex_tmpl);
        (void)remove(dec_tmpl);
        (void)remove(map_tmpl);
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    if (csvChanImport(opts, state) == 0) {
        DSD_FPRINTF(stderr, "row-key import accepted an unloadable path\n");
        failed = 1;
    }

    // A duplicated key header rejects the file.
    dsd_state_ext_free_all(state);
    dsd_state_trunk_lcn_free(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    if (write_text_file(map_tmpl, "channel,frequency_hz,keys_hex_csv,keys_dec_csv,keys_hex_csv\n"
                                  "1,851000000,,,\n")
        != 0) {
        (void)remove(hex_tmpl);
        (void)remove(dec_tmpl);
        (void)remove(map_tmpl);
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    if (csvChanImport(opts, state) == 0) {
        DSD_FPRINTF(stderr, "row-key import accepted a duplicated key header\n");
        failed = 1;
    }

    (void)remove(hex_tmpl);
    (void)remove(dec_tmpl);
    (void)remove(map_tmpl);
    free(opts);
    free_test_state(state);
    free_test_state(scratch);
    return failed;
}

static int
test_channel_import_direct_key_columns(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_state* scratch = (dsd_state*)calloc(1, sizeof(*scratch));
    char map_tmpl[] = "dsd-neo-test-direct-rowkey-map-XXXXXX";
    if (!opts || !state || !scratch) {
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    int fd = dsd_mkstemp(map_tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    (void)dsd_close(fd);

    static const char valid_map[] = "channel,frequency_hz,name,SINGLE_KEY_HEX,single_key_DEC\n"
                                    "1,851000000,Dispatch,00112233445566778899AABBCCDDEEFF,7\n"
                                    "2,851012500,Ops,,\n"
                                    "bad,851025000,NoSlot,0123456789,99\n"
                                    "3,851025000,Clear,,0\n";
    if (write_text_file(map_tmpl, valid_map) != 0) {
        (void)remove(map_tmpl);
        free(opts);
        free_test_state(state);
        free_test_state(scratch);
        return 1;
    }
    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", map_tmpl);
    state->K = 88ULL;
    state->aes_key[0] = 0xAAU;
    if (csvChanImport(opts, state) != 0 || state->lcn_freq_count != 3) {
        DSD_FPRINTF(stderr, "direct row-key channel import failed\n");
        failed = 1;
    }
    const dsd_key_set* row0 = dsd_state_trunk_lcn_keys_get(state, 0U);
    if (row0 == NULL || row0->keyloader != 0 || row0->scalars.K != 7ULL || row0->scalars.K1 != 0x0011223344556677ULL
        || row0->scalars.K2 != 0x8899AABBCCDDEEFFULL || row0->scalars.aes_key[15] != 0xFFU) {
        DSD_FPRINTF(stderr, "direct row-key values did not land in slot 0\n");
        failed = 1;
    } else {
        dsd_key_set_install(scratch, row0);
        if (scratch->K != 7ULL || scratch->K1 != 0x0011223344556677ULL || scratch->keyloader != 0
            || scratch->aes_key[15] != 0xFFU) {
            DSD_FPRINTF(stderr, "direct row-key set did not install\n");
            failed = 1;
        }
    }
    if (dsd_state_trunk_lcn_keys_get(state, 1U) != NULL) {
        DSD_FPRINTF(stderr, "blank direct cells stored a key set\n");
        failed = 1;
    }
    const dsd_key_set* row2 = dsd_state_trunk_lcn_keys_get(state, 2U);
    if (row2 == NULL || row2->present != 1U || row2->scalars.K != 0ULL) {
        DSD_FPRINTF(stderr, "explicit zero direct key did not override globals\n");
        failed = 1;
    }
    if (state->K != 88ULL || state->aes_key[0] != 0xAAU) {
        DSD_FPRINTF(stderr, "direct row-key import leaked into live key state\n");
        failed = 1;
    }

    dsd_state_trunk_lcn_free(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    if (write_text_file(map_tmpl, "channel,frequency_hz,keys_hex_csv,single_key_dec\n"
                                  "1,851000000,keys.csv,1\n")
            != 0
        || csvChanImport(opts, state) == 0) {
        DSD_FPRINTF(stderr, "direct/file row-key conflict was accepted\n");
        failed = 1;
    }

    dsd_state_trunk_lcn_free(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    if (write_text_file(map_tmpl, "channel,frequency_hz,single_key_hex,single_key_hex\n"
                                  "1,851000000,0123456789,0123456789\n")
            != 0
        || csvChanImport(opts, state) == 0) {
        DSD_FPRINTF(stderr, "duplicate direct row-key header was accepted\n");
        failed = 1;
    }

    dsd_state_trunk_lcn_free(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    if (write_text_file(map_tmpl, "channel,frequency_hz,single_key_dec\n"
                                  "1,851000000,256\n")
            != 0
        || csvChanImport(opts, state) == 0) {
        DSD_FPRINTF(stderr, "invalid direct decimal row key was accepted\n");
        failed = 1;
    }

    dsd_state_trunk_lcn_free(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    if (write_text_file(map_tmpl, "channel,frequency_hz,single_key_hex\n"
                                  "1,851000000,not-a-key\n")
            != 0
        || csvChanImport(opts, state) == 0) {
        DSD_FPRINTF(stderr, "invalid direct hexadecimal row key was accepted\n");
        failed = 1;
    }

    (void)remove(map_tmpl);
    free(opts);
    free_test_state(state);
    free_test_state(scratch);
    return failed;
}

static int
test_channel_import_accepts_hex_and_iden_chan_keys(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    char tmpl[] = "dsd-neo-test-channel-keys-XXXXXX";
    int fd = -1;

    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }

    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    // 0x2A46 == 10822 == iden 2, chan 0xA46 (2630); 3-100 == 0x3064 == 12388; 0X0A == 10.
    // The rejected spellings must take no slot at all (chan -1), unlike a bad frequency.
    if (write_text_file(tmpl, "channel,freq\n"
                              "0x2A46,851000000\n"
                              "3-100,852000000\n"
                              "16-0,853000000\n"
                              "0-4096,854000000\n"
                              "2-,855000000\n"
                              "-5,856000000\n"
                              "15-4095,857000000\n"
                              "0X0A,858000000\n"
                              "2-2630,859000000\n")
        != 0) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }

    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", tmpl);
    if (csvChanImport(opts, state) != 0) {
        DSD_FPRINTF(stderr, "chan key spellings: import failed\n");
        failed = 1;
    }
    // The last row re-keys 10822 (2-2630 is the same key as 0x2A46), so it wins.
    if (state->trunk_chan_map[10822] != 859000000L) {
        DSD_FPRINTF(stderr, "chan key spellings: 0x2A46/2-2630 -> %ld\n", state->trunk_chan_map[10822]);
        failed = 1;
    }
    if (state->trunk_chan_map[12388] != 852000000L) {
        DSD_FPRINTF(stderr, "chan key spellings: 3-100 -> %ld\n", state->trunk_chan_map[12388]);
        failed = 1;
    }
    if (state->trunk_chan_map[10] != 858000000L) {
        DSD_FPRINTF(stderr, "chan key spellings: 0X0A -> %ld\n", state->trunk_chan_map[10]);
        failed = 1;
    }
    if (state->trunk_chan_map_used_count != 3U) {
        DSD_FPRINTF(stderr, "chan key spellings: used_count=%u want 3\n", state->trunk_chan_map_used_count);
        failed = 1;
    }
    // Four accepted rows (two of them the same key) take four positional LCN slots; the rejected
    // spellings and the sentinel 15-4095 take none.
    if (state->lcn_freq_count != 4) {
        DSD_FPRINTF(stderr, "chan key spellings: lcn_freq_count=%d want 4\n", state->lcn_freq_count);
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
bandplan_row_matches(const p25_bandplan_row_t* row, int iden, int is_tdma, long base_5hz, int spac_125hz, int type,
                     int trans_off, int bw_vu, unsigned long long wacn, unsigned long long sysid) {
    return row->iden == iden && row->is_tdma == is_tdma && row->entry.base_freq == base_5hz
           && row->entry.chan_spac == spac_125hz && row->entry.chan_type == type && row->entry.trans_off == trans_off
           && row->entry.bw_vu == bw_vu && row->entry.wacn == wacn && row->entry.sysid == sysid && row->entry.trust == 1
           && row->entry.populated == 1 && row->entry.rfss == 0 && row->entry.site == 0;
}

static int
test_p25_bandplan_import_rows_units_and_seed(void) {
    int failed = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    char tmpl[] = "dsd-neo-test-p25-bandplan-XXXXXX";
    int fd = -1;

    if (!opts || !state) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);

    if (write_text_file(tmpl, "iden,base_hz,spacing_hz,type,tx_offset_hz,bandwidth_hz,wacn,sysid\n"
                              "0,851006250,6250,1,-45000000,12500,,\n"
                              "2,762006250,6250,3,-30000000,,BEE00,3A1\n"
                              "1,851006251,6250,1,,,,\n"
                              "3,851006250,6300,,,,,\n"
                              "4,851006250,6250,1,,7000,,\n"
                              "5,851006250,6250,1,,,BEE00,\n"
                              "16,851006250,6250,,,,,\n"
                              "6,851006250,6250,1,-45000001,,,\n"
                              "\n"
                              "7,851006250,6250\n"
                              "8,851006250,6250,1,-30000000,,,\n")
        != 0) {
        (void)remove(tmpl);
        free(opts);
        free_test_state(state);
        return 1;
    }

    DSD_SNPRINTF(opts->p25_bandplan_in_file, sizeof(opts->p25_bandplan_in_file), "%s", tmpl);
    if (csvP25BandplanImport(opts, state) != 0) {
        DSD_FPRINTF(stderr, "bandplan import: failed\n");
        failed = 1;
    }
    if (state->p25_bandplan_row_count != 4) {
        DSD_FPRINTF(stderr, "bandplan import: row_count=%d want 4\n", state->p25_bandplan_row_count);
        failed = 1;
    } else {
        // 851006250 Hz / 5 = 170201250; 6250 Hz / 125 = 50; VU row: offset in spacing units
        // (-45 MHz / 6250 = -7200); 12500 Hz -> bw_vu 5.
        if (!bandplan_row_matches(&state->p25_bandplan_rows[0], 0, 0, 170201250L, 50, 1, -7200, 5, 0ULL, 0ULL)) {
            DSD_FPRINTF(stderr, "bandplan import: row 0 wrong\n");
            failed = 1;
        }
        // TDMA row: type 3 routes to the TDMA table and, like a VU row, its offset is in spacing
        // units (-30 MHz / 6250 = -4800) -- IDEN_UP_TDMA carries the 13-bit VU offset field.
        if (!bandplan_row_matches(&state->p25_bandplan_rows[1], 2, 1, 152401250L, 50, 3, -4800, 0, 0xBEE00ULL,
                                  0x3A1ULL)) {
            DSD_FPRINTF(stderr, "bandplan import: row 1 wrong\n");
            failed = 1;
        }
        if (!bandplan_row_matches(&state->p25_bandplan_rows[2], 7, 0, 170201250L, 50, 1, 0, 0, 0ULL, 0ULL)) {
            DSD_FPRINTF(stderr, "bandplan import: row 2 wrong\n");
            failed = 1;
        }
        // Standard FDMA row: offset in 250 kHz units (-30 MHz -> -120).
        if (!bandplan_row_matches(&state->p25_bandplan_rows[3], 8, 0, 170201250L, 50, 1, -120, 0, 0ULL, 0ULL)) {
            DSD_FPRINTF(stderr, "bandplan import: row 3 wrong\n");
            failed = 1;
        }
    }

    // Import seeds the live tables: global rows land now, the system row waits for its WACN/SYS.
    if (!state->p25_iden_fdma[0].populated || state->p25_iden_fdma[0].base_freq != 170201250L
        || state->p25_iden_fdma[0].trust != 1 || state->p25_chan_tdma_explicit[0] != 1) {
        DSD_FPRINTF(stderr, "bandplan import: fdma[0] not seeded\n");
        failed = 1;
    }
    if (!state->p25_iden_fdma[7].populated || state->p25_chan_tdma_explicit[7] != 1
        || !state->p25_iden_fdma[8].populated) {
        DSD_FPRINTF(stderr, "bandplan import: fdma[7] not seeded\n");
        failed = 1;
    }
    if (state->p25_iden_tdma[2].populated || state->p25_chan_tdma_explicit[2] != 0) {
        DSD_FPRINTF(stderr, "bandplan import: system row seeded before identity known\n");
        failed = 1;
    }
    state->p2_wacn = 0xBEE00ULL;
    state->p2_sysid = 0x3A1ULL;
    if (dsd_state_p25_bandplan_seed(state) != 1) {
        DSD_FPRINTF(stderr, "bandplan import: seed after identity should add exactly one slot\n");
        failed = 1;
    }
    if (!state->p25_iden_tdma[2].populated || state->p25_iden_tdma[2].chan_type != 3
        || state->p25_chan_tdma_explicit[2] != 2) {
        DSD_FPRINTF(stderr, "bandplan import: tdma[2] not seeded after identity\n");
        failed = 1;
    }

    // A second import replaces the stored plan wholesale.
    if (write_text_file(tmpl, "iden,base_hz,spacing_hz,type,tx_offset_hz,bandwidth_hz\n"
                              "9,851006250,6250,1,0,0\n")
        != 0) {
        failed = 1;
    } else if (csvP25BandplanImport(opts, state) != 0 || state->p25_bandplan_row_count != 1
               || state->p25_bandplan_rows[0].iden != 9) {
        DSD_FPRINTF(stderr, "bandplan import: re-import did not replace the plan\n");
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free_test_state(state);
    return failed;
}

static int
test_p25_bandplan_import_rejects_empty_and_missing(void) {
    int failed = 0;
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    char tmpl[] = "dsd-neo-test-p25-bandplan-empty-XXXXXX";
    if (!state) {
        return 1;
    }
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free_test_state(state);
        return 1;
    }
    (void)dsd_close(fd);
    state->p25_bandplan_row_count = 2;
    state->p25_bandplan_rows[0].iden = 4;
    if (write_text_file(tmpl, "iden,base_hz,spacing_hz\n"
                              "16,851006250,6250\n")
        != 0) {
        (void)remove(tmpl);
        free_test_state(state);
        return 1;
    }
    if (csvP25BandplanImportPath(tmpl, state) == 0) {
        DSD_FPRINTF(stderr, "bandplan import: accepted a file with no usable rows\n");
        failed = 1;
    }
    if (state->p25_bandplan_row_count != 2 || state->p25_bandplan_rows[0].iden != 4) {
        DSD_FPRINTF(stderr, "bandplan import: failed import touched the stored plan\n");
        failed = 1;
    }
    if (csvP25BandplanImportPath("dsd-neo-test-missing-dir/none.csv", state) == 0) {
        DSD_FPRINTF(stderr, "bandplan import: accepted a missing file\n");
        failed = 1;
    }
    (void)remove(tmpl);
    free_test_state(state);
    return failed;
}

static int
test_p25_bandplan_export_round_trip(void) {
    int failed = 0;
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_state* back = (dsd_state*)calloc(1, sizeof(*back));
    char tmpl[] = "dsd-neo-test-p25-bandplan-export-XXXXXX";
    if (!state || !back) {
        free_test_state(state);
        free_test_state(back);
        return 1;
    }
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free_test_state(state);
        free_test_state(back);
        return 1;
    }
    (void)dsd_close(fd);

    // One VU FDMA entry and one TDMA entry (both carry the offset in spacing units),
    // both learned on WACN BEE00 / SYS 3A1; an unready entry (no spacing) must not export.
    p25_iden_entry_t* f0 = &state->p25_iden_fdma[0];
    f0->base_freq = 170201250L;
    f0->chan_spac = 50;
    f0->chan_type = 1;
    f0->trans_off = -7200;
    f0->bw_vu = 5;
    f0->trust = 2;
    f0->populated = 1;
    f0->wacn = 0xBEE00ULL;
    f0->sysid = 0x3A1ULL;
    f0->rfss = 1;
    f0->site = 7;
    p25_iden_entry_t* t2 = &state->p25_iden_tdma[2];
    t2->base_freq = 152401250L;
    t2->chan_spac = 50;
    t2->chan_type = 3;
    t2->trans_off = -4800;
    t2->trust = 2;
    t2->populated = 1;
    t2->wacn = 0xBEE00ULL;
    t2->sysid = 0x3A1ULL;
    state->p25_iden_fdma[5].populated = 1;
    state->p25_iden_fdma[5].base_freq = 170201250L;

    p25_bandplan_row_t rows[DSD_P25_BANDPLAN_MAX_ROWS];
    int count =
        dsd_p25_bandplan_append_tables(rows, 0, DSD_P25_BANDPLAN_MAX_ROWS, state->p25_iden_fdma, state->p25_iden_tdma);
    if (count != 2) {
        DSD_FPRINTF(stderr, "bandplan export: collected %d rows want 2\n", count);
        failed = 1;
    }
    // Appending the same tables again must not duplicate rows with the same iden/table/system.
    count = dsd_p25_bandplan_append_tables(rows, count, DSD_P25_BANDPLAN_MAX_ROWS, state->p25_iden_fdma,
                                           state->p25_iden_tdma);
    if (count != 2) {
        DSD_FPRINTF(stderr, "bandplan export: duplicate append gave %d rows want 2\n", count);
        failed = 1;
    }
    if (csvP25BandplanExportRows(tmpl, rows, count) != 0) {
        DSD_FPRINTF(stderr, "bandplan export: write failed\n");
        failed = 1;
    }
    if (csvP25BandplanImportPath(tmpl, back) != 0 || back->p25_bandplan_row_count != 2) {
        DSD_FPRINTF(stderr, "bandplan export: re-import failed (count=%d)\n", back->p25_bandplan_row_count);
        failed = 1;
    } else {
        if (!bandplan_row_matches(&back->p25_bandplan_rows[0], 0, 0, 170201250L, 50, 1, -7200, 5, 0xBEE00ULL,
                                  0x3A1ULL)) {
            DSD_FPRINTF(stderr, "bandplan export: fdma row did not round-trip\n");
            failed = 1;
        }
        if (!bandplan_row_matches(&back->p25_bandplan_rows[1], 2, 1, 152401250L, 50, 3, -4800, 0, 0xBEE00ULL,
                                  0x3A1ULL)) {
            DSD_FPRINTF(stderr, "bandplan export: tdma row did not round-trip\n");
            failed = 1;
        }
    }
    if (csvP25BandplanExportRows(tmpl, rows, 0) == 0) {
        DSD_FPRINTF(stderr, "bandplan export: wrote a file with no rows\n");
        failed = 1;
    }

    (void)remove(tmpl);
    free_test_state(state);
    free_test_state(back);
    return failed;
}

int
main(void) {
    if (test_group_import_missing_file() != 0) {
        return 1;
    }
    if (test_channel_import_missing_file() != 0) {
        return 1;
    }
    if (test_channel_import_rejects_directory() != 0) {
        return 1;
    }
    if (test_decimal_key_import_and_group_hash() != 0) {
        return 1;
    }
    if (test_hex_key_import_preserves_zero_segments_for_keyring() != 0) {
        return 1;
    }
#if !DSD_PLATFORM_WIN_NATIVE
    if (test_channel_import_rejects_final_symlink() != 0) {
        return 1;
    }
#endif
    if (test_group_import_large_exact_file() != 0) {
        return 1;
    }
    if (test_group_import_large_file_policy() != 0) {
        return 1;
    }
    if (test_group_import_policy_and_basic_headers() != 0) {
        return 1;
    }
    if (test_group_import_invalid_ids_and_required_fields() != 0) {
        return 1;
    }
    if (test_channel_import_rejects_malformed_rows_without_reusing_previous_channel() != 0) {
        return 1;
    }
    if (test_channel_import_extends_past_26_entries() != 0) {
        return 1;
    }
    if (test_channel_import_accepts_hex_and_iden_chan_keys() != 0) {
        return 1;
    }
    if (test_p25_bandplan_import_rows_units_and_seed() != 0) {
        return 1;
    }
    if (test_p25_bandplan_import_rejects_empty_and_missing() != 0) {
        return 1;
    }
    if (test_p25_bandplan_export_round_trip() != 0) {
        return 1;
    }
    if (test_channel_import_name_column_stores_names_by_row_index() != 0) {
        return 1;
    }
    if (test_channel_import_name_column_requires_header_opt_in() != 0) {
        return 1;
    }
    if (test_channel_import_name_column_sanitizes_names() != 0) {
        return 1;
    }
    if (test_channel_import_row_key_columns() != 0) {
        return 1;
    }
    if (test_channel_import_direct_key_columns() != 0) {
        return 1;
    }
    if (test_channel_import_row_key_relative_subdir() != 0) {
        return 1;
    }
    if (test_group_import_range_after_many_exact_rows() != 0) {
        return 1;
    }
    if (test_vertex_import_missing_file() != 0) {
        return 1;
    }
    if (test_vertex_import_and_apply() != 0) {
        return 1;
    }
    if (test_dmr_tg_key_import_and_lookup() != 0) {
        return 1;
    }
    if (test_dmr_tg_key_import_rejects_bad_rows() != 0) {
        return 1;
    }
    if (test_dmr_tg_key_import_missing_file() != 0) {
        return 1;
    }
    return 0;
}
