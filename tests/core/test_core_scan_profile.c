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
    dsd_tg_policy_call_route active = {
        .target_id = 123, .source_id = 1, .freq_hz = 851000000, .channel = 10, .slot = 0};
    dsd_tg_policy_call_route candidate = active;
    candidate.target_id = 456;
    dsd_tg_policy_decision decision = {.priority = 10, .tune_allowed = 1, .preempt_requested = 1};
    assert(dsd_tg_policy_note_active_call(state, &active, &decision, 1.0) == 0);
    decision.priority = 20;
    assert(dsd_tg_policy_should_preempt(NULL, state, &candidate, &decision, 100.0));
    assert(dsd_scan_groups_suspend(state));
    expect_label(state, "Global");
    assert(dsd_tg_policy_make_exact_entry(123, "A", "New global", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_upsert_exact(state, &entry, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    dsd_scan_groups_resume(state);
    expect_label(state, "Row edit");
    assert(dsd_tg_policy_should_preempt(NULL, state, &candidate, &decision, 100.0));
    /* A plain suspend/resume must also retain the same active route. */
    assert(dsd_scan_groups_suspend(state));
    dsd_scan_groups_resume(state);
    assert(dsd_tg_policy_should_preempt(NULL, state, &candidate, &decision, 100.0));
    for (int i = 0; i < 10; i++) {
        dsd_scan_groups_enter(state, row);
    }
    assert(!dsd_tg_policy_should_preempt(NULL, state, &candidate, &decision, 100.0));
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

    /* A map whose rows carry options but no mode still has to run through the typed
     * scanner, since the legacy scanner applies keys but never row options. */
    write_file(path, "channel,frequency_hz,options\n"
                     "1,150000000,--scan-voice-hold-ms 4000\n"
                     "2,150000001,\n");
    DSD_MEMSET(state, 0, sizeof(*state));
    assert(csvChanImport(opts, state) == 0);
    assert(state->lcn_freq_count == 2);
    assert(dsd_channel_mode_get(state, 0) == DSD_SCAN_MODE_INHERIT);
    assert(dsd_channel_modes_present(state));
    assert(dsd_channel_profile_get(state, 0)->values.hold_ms == 4000);
    assert(dsd_channel_profile_get(state, 1) == NULL);
    /* Replacing the only option-bearing profile with an empty one releases the gate. */
    dsd_scan_row_profile* blank = (dsd_scan_row_profile*)calloc(1, sizeof(*blank));
    assert(blank);
    assert(dsd_channel_profile_set(state, 0, blank) == 0);
    assert(!dsd_channel_modes_present(state));
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);

    /* Legacy key columns beside an options cell keep their key-only meaning: they never
     * decide encrypted-audio muting, while the option text's own `-b` does. */
    write_file(path, "channel,frequency_hz,mode,single_key_dec,options\n"
                     "1,150000000,dmr,5,--no-force-key\n"
                     "2,150000001,dmr,,-b 5\n"
                     "3,150000002,dmr,,-b 0\n");
    DSD_MEMSET(state, 0, sizeof(*state));
    assert(csvChanImport(opts, state) == 0);
    assert(state->lcn_freq_count == 3);
    assert(dsd_state_trunk_lcn_keys_get(state, 0)->scalars.K == 5);
    assert(dsd_state_trunk_lcn_keys_get(state, 1)->scalars.K == 5);
    assert(!(dsd_channel_profile_get(state, 0)->values.present & DSD_SCAN_OPT_MUTE_DMR));
    assert((dsd_channel_profile_get(state, 1)->values.present & DSD_SCAN_OPT_MUTE_DMR)
           && dsd_channel_profile_get(state, 1)->values.mute_dmr == 0);
    assert((dsd_channel_profile_get(state, 2)->values.present & DSD_SCAN_OPT_MUTE_DMR)
           && dsd_channel_profile_get(state, 2)->values.mute_dmr == 1);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    assert(remove(path) == 0);
}

/* Option-bearing rows with no declared mode: the scope applies the row's policy over the
 * baseline exactly as a declared row would, and the legacy columns leave muting alone. */
static void
test_legacy_columns_do_not_mute(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->wav_sample_rate = 48000;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->dmr_mute_encL = opts->dmr_mute_encR = 1;
    dsd_scan_options parsed = {0};
    char error[192];
    assert(dsd_scan_options_parse("--no-force-key", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert(dsd_scan_options_merge_keys(&parsed, "", "", "0000001f00", "5", error, sizeof(error)) == 0);
    assert(parsed.bp == 5 && parsed.hytera[0] == 0x1f00 && parsed.hytera_digits == 10);
    assert(!(parsed.values.present & DSD_SCAN_OPT_MUTE_DMR));
    dsd_key_set keys = {0};
    dsd_scan_row_profile* profile = NULL;
    assert(dsd_scan_profile_load(&parsed, 0, &profile, &keys) == 0);
    assert(keys.present && keys.scalars.K == 5 && keys.scalars.K1 == 0x1f00 && keys.scalars.hytera_key_segments == 1);
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_INHERIT) == 0);
    assert(dsd_scan_mode_options(opts, state, &profile->values) == 0);
    assert(opts->dmr_mute_encL == 1 && opts->dmr_mute_encR == 1);
    dsd_scan_profile_free(profile);
    profile = NULL;
    /* A rejected column leaves the options exactly as parsed. */
    unsigned char before_merge[sizeof(parsed)];
    unsigned char after_merge[sizeof(parsed)];
    DSD_MEMCPY(before_merge, &parsed, sizeof(before_merge));
    assert(dsd_scan_options_merge_keys(&parsed, "", "", "zz", "", error, sizeof(error)) != 0);
    DSD_MEMCPY(after_merge, &parsed, sizeof(after_merge));
    assert(memcmp(before_merge, after_merge, sizeof(after_merge)) == 0);
    DSD_SECURE_ZERO(before_merge, sizeof(before_merge));
    DSD_SECURE_ZERO(after_merge, sizeof(after_merge));
    /* `-b` in the option text decides muting from all of the row's material. */
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
    assert(dsd_scan_options_parse("-b 0", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 1);
    assert(dsd_scan_options_merge_keys(&parsed, "", "", "0000001f00", "", error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 0);
    assert(dsd_scan_profile_load(&parsed, 0, &profile, &keys) == 0);
    assert(dsd_scan_mode_options(opts, state, &profile->values) == 0);
    assert(opts->dmr_mute_encL == 0 && opts->dmr_mute_encR == 0);
    dsd_scan_mode_leave(opts, state);
    assert(opts->dmr_mute_encL == 1);
    dsd_scan_profile_free(profile);
    dsd_key_set_free(&keys);
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
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
    /* No context at all is a failure that leaves the output alone. */
    dsd_tg_policy_store* out = original;
    dsd_tg_policy_test_alloc_fail_after(0);
    assert(dsd_tg_policy_load(path, &out) != 0);
    assert(out == original);
    dsd_tg_policy_test_alloc_reset();
    /* A row the importer could not store is skipped with a warning, as the global `-G`
     * import has always done, and the load still yields a (here empty) policy. */
    dsd_tg_policy_store* partial = NULL;
    dsd_tg_policy_test_alloc_fail_after(1);
    assert(dsd_tg_policy_load(path, &partial) == 0);
    dsd_tg_policy_test_alloc_reset();
    assert(partial != NULL && partial != original);
    dsd_state* probe = (dsd_state*)calloc(1, sizeof(*probe));
    assert(probe);
    dsd_tg_policy_install(probe, partial);
    char name[64];
    assert(!dsd_tg_policy_lookup_label(probe, 123, NULL, 0, name, sizeof(name)));
    dsd_tg_policy_install(probe, original);
    assert(dsd_tg_policy_lookup_label(probe, 123, NULL, 0, name, sizeof(name)) && strcmp(name, "Expected row") == 0);
    dsd_state_ext_free_all(probe);
    free(probe);
    dsd_tg_policy_release(partial);
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
    /* Each path resolves within its own capacity: a key path may use the legacy column limit,
     * while a group path is bounded by the option field it overrides. A failure leaves the
     * object untouched. */
    char base[DSD_SCAN_OPTIONS_GROUP_PATH_MAX];
    DSD_MEMSET(base, 'b', sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    base[sizeof(base) - 5] = '/';
    unsigned char before_resolve[sizeof(options)];
    unsigned char after_resolve[sizeof(options)];
    DSD_MEMCPY(before_resolve, &options, sizeof(before_resolve));
    assert(dsd_scan_options_resolve(&options, base, error, sizeof(error)) != 0);
    DSD_MEMCPY(after_resolve, &options, sizeof(after_resolve));
    assert(memcmp(before_resolve, after_resolve, sizeof(after_resolve)) == 0);
    DSD_SECURE_ZERO(before_resolve, sizeof(before_resolve));
    DSD_SECURE_ZERO(after_resolve, sizeof(after_resolve));
    options.values.present &= ~(uint32_t)DSD_SCAN_OPT_GROUP;
    options.values.group_file[0] = '\0';
    assert(dsd_scan_options_resolve(&options, base, error, sizeof(error)) == 0);
    assert(strlen(options.hex_file) > DSD_SCAN_OPTIONS_GROUP_PATH_MAX);
    assert(strlen(options.dec_file) > DSD_SCAN_OPTIONS_GROUP_PATH_MAX);
    DSD_SECURE_ZERO(&options, sizeof(options));
}

static void
test_move_unwinds_both_group_scopes(void) {
    for (int suspended = 0; suspended < 2; suspended++) {
        dsd_state* src = (dsd_state*)calloc(1, sizeof(*src));
        dsd_state* dst = (dsd_state*)calloc(1, sizeof(*dst));
        assert(src && dst);
        dsd_tg_policy_entry entry;
        assert(dsd_tg_policy_make_exact_entry(123, "A", "Source global", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
        assert(dsd_tg_policy_append_exact(src, &entry) == 0);
        assert(dsd_tg_policy_make_exact_entry(123, "A", "Destination global", DSD_TG_POLICY_SOURCE_IMPORTED, &entry)
               == 0);
        assert(dsd_tg_policy_append_exact(dst, &entry) == 0);
        dsd_scan_row_profile* row = (dsd_scan_row_profile*)calloc(1, sizeof(*row));
        assert(row);
        row->values.present = DSD_SCAN_OPT_GROUP;
        /* An empty row policy still owns a scope and must not carry its source baseline. */
        assert(dsd_channel_profile_set(src, 0, row) == 0);
        assert(dsd_scan_groups_begin(dst) == 0);
        dsd_scan_groups_enter(src, row);
        dsd_scan_groups_enter(dst, row);
        if (suspended) {
            assert(dsd_scan_groups_suspend(src));
            assert(dsd_scan_groups_suspend(dst));
        }
        dsd_channel_modes_move(dst, src);
        expect_label(src, "Source global");
        expect_label(dst, "Destination global");
        assert(dsd_channel_profile_get(src, 0) == NULL);
        assert(dsd_channel_profile_get(dst, 0) == row);
        assert(!dsd_scan_groups_suspend(dst));
        dsd_scan_groups_enter(dst, row);
        dsd_scan_groups_leave(dst);
        expect_label(dst, "Destination global");
        dsd_state_ext_free_all(src);
        dsd_state_ext_free_all(dst);
        free(src);
        free(dst);
    }
}

static void
test_slotless_options_validate_files(void) {
    char path[1024];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsd_slotless_options");
    assert(fd >= 0);
    dsd_close(fd);
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    assert(state && opts);
    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", path);
    const char* switches[] = {"-G", "-K", "-k"};
    for (size_t i = 0; i < sizeof(switches) / sizeof(switches[0]); i++) {
        char csv[2048];
        DSD_SNPRINTF(csv, sizeof(csv),
                     "channel,frequency_hz,mode,options\n"
                     "invalid,150000000,dmr,%s '%s.missing'\n",
                     switches[i], path);
        write_file(path, csv);
        assert(csvChanImport(opts, state) != 0);
        assert(state->lcn_freq_count == 0);
    }
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    assert(remove(path) == 0);
}

/* The column parser supplies the width even for prefixed, spaced and all-zero keys. */
static void
test_legacy_hex_widths(void) {
    const char* values[] = {" 0X00 0000 0000 ", "00000000000000000000000000000000",
                            "0x00000000000000000000000000000000 00000000000000000000000000000001"};
    const unsigned int widths[] = {10, 32, 64};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        dsd_scan_options options = {0};
        dsd_key_set keys = {0};
        char error[192];
        assert(dsd_scan_options_merge_keys(&options, "", "", values[i], "", error, sizeof(error)) == 0);
        assert(options.hytera_digits == widths[i]);
        assert(dsd_scan_options_keys(&options, &keys) == 0);
        assert(keys.present);
        if (i > 0) {
            assert(keys.scalars.aes_key_segments[0] == widths[i] / 16);
        }
        if (i == 2) {
            assert(keys.scalars.K4 == 1 && keys.scalars.aes_key_loaded[0]);
        }
        dsd_key_set_free(&keys);
        DSD_SECURE_ZERO(&options, sizeof(options));
    }
}

int
main(void) {
    test_move_unwinds_both_group_scopes();
    test_slotless_options_validate_files();
    test_legacy_hex_widths();
    test_keys_and_scope();
    test_prepared_key_rollback();
    test_group_load_failure();
    test_source_conflicts();
    test_relative_paths();
    test_group_ownership();
    test_csv_import();
    test_legacy_columns_do_not_mute();
    return 0;
}
