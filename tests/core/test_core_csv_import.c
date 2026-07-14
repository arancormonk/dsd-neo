// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
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
    if (test_group_import_range_after_many_exact_rows() != 0) {
        return 1;
    }
    if (test_vertex_import_missing_file() != 0) {
        return 1;
    }
    if (test_vertex_import_and_apply() != 0) {
        return 1;
    }
    return 0;
}
