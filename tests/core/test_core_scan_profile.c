// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_support.h"

void dsd_tg_policy_test_alloc_reset(void);
void dsd_tg_policy_test_alloc_fail_after(long fail_after);

static void
write_file(const char* path, const char* contents) {
    FILE* fp = dsd_fopen_private(path, "w");
    assert(fp);
    assert(fputs(contents, fp) >= 0);
    assert(fclose(fp) == 0);
}

static void
expect_label(const dsd_state* state, const char* label) {
    char name[64];
    assert(dsd_tg_policy_lookup_label(state, 123, NULL, 0, name, sizeof(name)));
    assert(strcmp(name, label) == 0);
}

static void
test_keys_and_scope(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->frame_dstar = 1;
    opts->wav_sample_rate = 48000;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->scan_voice_hold_ms = 2000;
    opts->aggressive_framesync = 1;
    state->R = 999;
    state->M = 1;

    const struct {
        const char* text;
        uint64_t r;
        uint64_t rr;
        uint64_t k;
        unsigned int mode;
        int force;
    } rows[] = {{"-1 0123456789 -0 -F --scan-voice-only --scan-voice-hold-ms 4000", 0x123456789ULL, 0x123456789ULL, 0,
                 DSD_SCAN_MODE_DMR, 0x21},
                {"-H 0000001f00 -4", 0, 0, 0, DSD_SCAN_MODE_DMR, 1},
                {"-b 1 --no-force-key --strict-crc --no-scan-voice-only", 0, 0, 1, DSD_SCAN_MODE_DMR, 0},
                {"-R 1 --no-force-key", 1, 0, 0, DSD_SCAN_MODE_NXDN48, 0}};

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        dsd_scan_options parsed = {0};
        char error[192];
        dsd_key_set keys = {0};
        dsd_scan_row_profile* profile = NULL;
        assert(dsd_scan_options_parse(rows[i].text, rows[i].mode, 1, &parsed, error, sizeof(error)) == 0);
        assert(dsd_scan_profile_load(&parsed, 0, &profile, &keys) == 0);
        dsd_scan_key_change change = {0};
        const uint64_t old_r = state->R;
        assert(dsd_scan_key_change_prepare(state, &keys, &change) == 0);
        assert(state->R == old_r);
        assert(dsd_scan_mode_enter(opts, state, (dsd_scan_mode)rows[i].mode) == 0);
        dsd_scan_mode_options(opts, state, &profile->values);
        assert(dsd_scan_key_change_commit(state, &change));
        assert(state->R == rows[i].r && state->RR == rows[i].rr && state->K == rows[i].k && state->M == rows[i].force);
        assert(state->keyloader == 0);
        if (i == 0) {
            assert(opts->scan_voice_hold_ms == 4000 && opts->scan_voice_only == 1 && opts->dmr_crc_relaxed_default);
        }
        if (i == 1) {
            assert(state->K1 == 0x1f00 && opts->scan_voice_hold_ms == 2000 && !opts->scan_voice_only);
        }
        if (i == 2) {
            assert(opts->aggressive_framesync == 1 && !opts->dmr_crc_relaxed_default && opts->dmr_mute_encL == 0);
        }
        dsd_scan_profile_free(profile);
        dsd_key_set_free(&keys);
        DSD_SECURE_ZERO(&parsed, sizeof(parsed));
    }
    dsd_scan_key_change restore = {0};
    assert(dsd_scan_key_change_prepare(state, NULL, &restore) == 0);
    assert(dsd_scan_key_change_commit(state, &restore));
    dsd_scan_mode_leave(opts, state);
    assert(state->R == 999 && state->M == 1 && opts->frame_dstar && opts->scan_voice_hold_ms == 2000);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
}

static void
test_group_ownership(void) {
    char path[1024];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsd_scan_groups");
    assert(fd >= 0);
    dsd_close(fd);
    write_file(path, "id,mode,name\n123,B,Row group\n");
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_state* snapshot = (dsd_state*)calloc(1, sizeof(*snapshot));
    assert(state && snapshot);
    dsd_tg_policy_entry entry;
    assert(dsd_tg_policy_make_exact_entry(123, "A", "Global", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(state, &entry) == 0);
    dsd_scan_options parsed = {0};
    parsed.values.present = DSD_SCAN_OPT_GROUP;
    DSD_SNPRINTF(parsed.values.group_file, sizeof(parsed.values.group_file), "%s", path);
    dsd_key_set keys = {0};
    dsd_scan_row_profile* row = NULL;
    assert(dsd_scan_profile_load(&parsed, 0, &row, &keys) == 0);
    assert(remove(path) == 0); /* Entry must never reopen its file. */
    assert(dsd_scan_groups_begin(state) == 0);
    dsd_scan_groups_enter(state, row);
    expect_label(state, "Row group");
    assert(dsd_tg_policy_copy_snapshot(snapshot, state) == 0);
    assert(dsd_tg_policy_make_exact_entry(123, "A", "Row edit", DSD_TG_POLICY_SOURCE_USER_LOCKOUT, &entry) == 0);
    assert(dsd_tg_policy_upsert_exact(state, &entry, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    expect_label(snapshot, "Row group");
    dsd_scan_groups_enter(state, NULL);
    expect_label(state, "Global");
    dsd_scan_groups_enter(state, row);
    expect_label(state, "Row edit");
    assert(dsd_scan_groups_suspend(state));
    expect_label(state, "Global");
    assert(dsd_tg_policy_make_exact_entry(123, "A", "New global", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_upsert_exact(state, &entry, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    dsd_scan_groups_resume(state);
    expect_label(state, "Row edit");
    for (int i = 0; i < 10; i++) {
        dsd_scan_groups_enter(state, row);
    }
    dsd_scan_groups_leave(state);
    expect_label(state, "New global");
    dsd_scan_profile_free(row);
    dsd_key_set_free(&keys);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    dsd_state_ext_free_all(snapshot);
    free(state);
    free(snapshot);
}

static void
test_csv_import(void) {
    char path[1024];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsd_scan_map");
    assert(fd >= 0);
    dsd_close(fd);
    write_file(path, "channel,frequency_hz,relevant_CLI_switches,mode,name\n"
                     "1,150000000,-1 0123456789 -0 -F,dmr,RC4\n"
                     "1,150000000,-R 1,nxdn48,NXDN\n"
                     "2,0,-b 1,dmr,Placeholder\n");
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", path);
    assert(csvChanImport(opts, state) == 0);
    assert(state->lcn_freq_count == 3);
    assert(dsd_channel_mode_get(state, 0) == DSD_SCAN_MODE_DMR);
    assert(dsd_channel_mode_get(state, 1) == DSD_SCAN_MODE_NXDN48);
    assert(dsd_channel_profile_get(state, 0)->values.force == 0x21);
    assert(dsd_state_trunk_lcn_keys_get(state, 0)->scalars.R == 0x123456789ULL);
    assert(dsd_state_trunk_lcn_keys_get(state, 1)->scalars.R == 1);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    assert(remove(path) == 0);
}

static void
test_prepared_key_rollback(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(state);
    state->rkey_array[7] = 123;
    state->rkey_array_loaded[7] = 1;
    state->keyloader = 1;
    state->R = 99;
    dsd_key_set row = {0};
    row.present = 1;
    row.scalars.R = 1;
    assert(dsd_scan_keys_enter(state, &row));
    dsd_scan_key_change rollback = {0};
    assert(dsd_scan_key_change_prepare(state, &row, &rollback) == 0);
    dsd_scan_keys_leave(state);
    assert(state->rkey_array[7] == 123 && state->R == 99);
    row.scalars.R = 2;
    assert(dsd_scan_keys_enter(state, &row));
    (void)dsd_scan_key_change_commit(state, &rollback);
    assert(state->R == 1 && state->keyloader == 0 && !state->rkey_array_loaded[7]);
    dsd_scan_keys_leave(state);
    assert(state->R == 99 && state->keyloader == 1 && state->rkey_array_loaded[7] && state->rkey_array[7] == 123);
    dsd_key_set_free(&row);
    free(state);
}

static void
test_group_load_failure(void) {
    char path[1024];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsd_scan_group_fail");
    assert(fd >= 0);
    dsd_close(fd);
    write_file(path, "id,mode,name\n123,A,Expected row\n");
    dsd_tg_policy_store* original = NULL;
    assert(dsd_tg_policy_load(path, &original) == 0);
    for (long fail = 0; fail < 2; fail++) {
        dsd_tg_policy_store* out = original;
        dsd_tg_policy_test_alloc_fail_after(fail);
        assert(dsd_tg_policy_load(path, &out) != 0);
        assert(out == original);
        dsd_tg_policy_test_alloc_reset();
    }
    dsd_tg_policy_release(original);
    assert(remove(path) == 0);
}

static void
test_source_conflicts(void) {
    const struct {
        const char* text;
        const char* hex_file;
        const char* dec_file;
        const char* hex;
        const char* dec;
    } rows[] = {
        {"-1 12345", "keys.csv", "", "", ""},        {"-R 1", "", "keys.csv", "", ""},
        {"-K keys.csv", "keys.csv", "", "", ""},     {"-K keys.csv", "", "", "", "1"},
        {"-k keys.csv", "", "", "0000000001", ""},   {"-b 1", "", "", "", "1"},
        {"-H 0000000001", "", "", "0000000001", ""},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        dsd_scan_options options = {0};
        char error[192];
        const unsigned mode = i == 1 ? DSD_SCAN_MODE_NXDN48 : DSD_SCAN_MODE_DMR;
        assert(dsd_scan_options_parse(rows[i].text, mode, 1, &options, error, sizeof(error)) == 0);
        assert(dsd_scan_options_merge_keys(&options, rows[i].hex_file, rows[i].dec_file, rows[i].hex, rows[i].dec,
                                           error, sizeof(error))
               != 0);
        DSD_SECURE_ZERO(&options, sizeof(options));
    }
}

static void
test_relative_paths(void) {
    dsd_scan_options options = {0};
    char error[192];
    assert(dsd_scan_options_parse("-G 'group list.csv' -K 'hex keys.csv' -k decimal.csv", DSD_SCAN_MODE_DMR, 1,
                                  &options, error, sizeof(error))
           == 0);
    assert(dsd_scan_options_resolve(&options, "lists/channels.csv", error, sizeof(error)) == 0);
    assert(strcmp(options.values.group_file, "lists/group list.csv") == 0);
    assert(strcmp(options.hex_file, "lists/hex keys.csv") == 0);
    assert(strcmp(options.dec_file, "lists/decimal.csv") == 0);
    DSD_SECURE_ZERO(&options, sizeof(options));
}

int
main(void) {
    test_keys_and_scope();
    test_prepared_key_rollback();
    test_group_load_failure();
    test_source_conflicts();
    test_relative_paths();
    test_group_ownership();
    test_csv_import();
    return 0;
}
