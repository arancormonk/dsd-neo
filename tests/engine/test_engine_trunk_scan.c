// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/platform/sockets.h"
#include "test_support.h"

static const char k_header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n";
static int g_dmr_tick_calls = 0;
static int g_dmr_tick_release_tuned = 0;
static int g_csv_import_result = 0;
static int g_scan_tune_to_freq_ted_sps = 0;
static int g_p25_tick_guard_available = 1;
static int g_p25_tick_guard_depth = 0;
static int g_p25_tick_guard_enter_calls = 0;
static int g_p25_tick_guard_leave_calls = 0;
static unsigned int g_fake_rtl_output_rate_hz = 0;

static unsigned int
fake_rtl_output_rate_hz(void) {
    return g_fake_rtl_output_rate_hz;
}

static int
test_tg_policy_tune_allowed(const dsd_opts* opts, int call_type_enabled, int encrypted, int data_call) {
    if (!opts || !call_type_enabled || opts->trunk_use_allow_list) {
        return 0;
    }
    if (encrypted && opts->trunk_tune_enc_calls == 0) {
        return 0;
    }
    if (data_call && opts->trunk_tune_data_calls == 0) {
        return 0;
    }
    return 1;
}

void
p25_sm_init_ctx(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state) {
    if (!ctx) {
        return;
    }
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->initialized = 1;
    ctx->state = (opts && state && opts->p25_trunk == 1 && state->trunk_cc_freq != 0) ? P25_SM_ON_CC : P25_SM_IDLE;
}

int
p25_sm_tick_guard_try_enter(void) {
    g_p25_tick_guard_enter_calls++;
    if (!g_p25_tick_guard_available) {
        return 0;
    }
    g_p25_tick_guard_depth++;
    return 1;
}

void
p25_sm_tick_guard_leave(void) {
    g_p25_tick_guard_leave_calls++;
    if (g_p25_tick_guard_depth > 0) {
        g_p25_tick_guard_depth--;
    }
}

void
dmr_sm_init_ctx(dmr_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state) {
    if (!ctx) {
        return;
    }
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->initialized = 1;
    ctx->state = (opts && state && opts->trunk_enable == 1 && opts->p25_trunk == 0 && state->trunk_cc_freq != 0)
                     ? DMR_SM_ON_CC
                     : DMR_SM_IDLE;
}

void
dmr_sm_tick_ctx(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    g_dmr_tick_calls++;
    if (!ctx || !g_dmr_tick_release_tuned || ctx->state != DMR_SM_TUNED) {
        return;
    }
    ctx->state = DMR_SM_ON_CC;
    if (opts) {
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
    }
}

dsd_trunk_tune_result
dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    g_scan_tune_to_freq_ted_sps = ted_sps;
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 0;
    state->last_cc_sync_time_m = dsd_engine_trunk_scan_active_index(state) == (size_t)-1 ? 0.0 : 1.0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

int
csvChanImport(const dsd_opts* opts, dsd_state* state) {
    if (g_csv_import_result != 0) {
        return g_csv_import_result;
    }
    if (!opts || !state || opts->chan_in_file[0] == '\0') {
        return 0;
    }
    FILE* fp = fopen(opts->chan_in_file, "rb");
    if (!fp) {
        return 0;
    }
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        char* comma = strchr(line, ',');
        if (!comma) {
            continue;
        }
        *comma = '\0';
        char* freq_text = comma + 1;
        char* freq_end = freq_text;
        while (*freq_end && *freq_end != '\r' && *freq_end != '\n') {
            freq_end++;
        }
        *freq_end = '\0';
        uint32_t channel = 0;
        long freq = 0;
        if (dsd_parse_uint32_strict(line, 10, 0xFFFFU, &channel) == 0
            && dsd_parse_long_strict(freq_text, 10, 0L, LONG_MAX, &freq) == 0) {
            dsd_state_set_trunk_chan_freq(state, channel, freq);
        }
    }
    (void)fclose(fp);
    return g_csv_import_result;
}

int
dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                  int encrypted, int data_call, dsd_tg_policy_hold_behavior hold_behavior,
                                  dsd_tg_policy_decision* out) {
    (void)state;
    (void)hold_behavior;
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->target_id = tg;
    out->source_id = src;
    out->encrypted = encrypted;
    out->data_call = data_call;
    out->audio_allowed = 1;
    out->record_allowed = 1;
    out->stream_allowed = 1;
    out->tune_allowed =
        test_tg_policy_tune_allowed(opts, opts ? opts->trunk_tune_group_calls : 0, encrypted, data_call);
    return 0;
}

int
dsd_tg_policy_evaluate_private_call(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                    int encrypted, int data_call, dsd_tg_policy_private_allowlist_mode allowlist_mode,
                                    dsd_tg_policy_hold_behavior hold_behavior, dsd_tg_policy_decision* out) {
    (void)state;
    (void)allowlist_mode;
    (void)hold_behavior;
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->target_id = dst;
    out->source_id = src;
    out->encrypted = encrypted;
    out->data_call = data_call;
    out->audio_allowed = 1;
    out->record_allowed = 1;
    out->stream_allowed = 1;
    out->tune_allowed =
        test_tg_policy_tune_allowed(opts, opts ? opts->trunk_tune_private_calls : 0, encrypted, data_call);
    return 0;
}

static int
write_text_file(const char* path, const char* content) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    size_t len = strlen(content);
    int rc = (fwrite(content, 1, len, fp) == len) ? 0 : -1;
    if (fclose(fp) != 0) {
        rc = -1;
    }
    return rc;
}

static int
make_temp_dir(char* dir, size_t dir_sz) {
    return dsd_test_mkdtemp(dir, dir_sz, "dsdneo_trunk_scan") ? 0 : -1;
}

static int
write_targets_file(const char* dir, const char* body, char* out_path, size_t out_sz) {
    if (dsd_test_path_join(out_path, out_sz, dir, "targets.csv") != 0) {
        return -1;
    }
    char content[8192];
    int n = DSD_SNPRINTF(content, sizeof content, "%s%s", k_header, body);
    if (n < 0 || (size_t)n >= sizeof content) {
        return -1;
    }
    return write_text_file(out_path, content);
}

static void
cleanup_paths(const char* dir, const char* targets, const char* chan) {
    if (targets) {
        (void)remove(targets);
    }
    if (chan) {
        (void)remove(chan);
    }
#if DSD_PLATFORM_WIN_NATIVE
    if (dir) {
        (void)_rmdir(dir);
    }
#else
    if (dir) {
        (void)rmdir(dir);
    }
#endif
}

static int
append_text(char* dst, size_t dst_sz, const char* src) {
    size_t dst_len = strlen(dst);
    size_t src_len = strlen(src);
    if (dst_len + src_len + 1 > dst_sz) {
        return -1;
    }
    DSD_MEMCPY(dst + dst_len, src, src_len + 1);
    return 0;
}

static int
test_parser_valid_mixed_targets_and_relative_chan_csv(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char chan_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(chan_path, sizeof chan_path, dir, "chan.csv") != 0
        || write_text_file(chan_path, "channel,frequency\n1,851012500\n") != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    if (write_targets_file(dir,
                           "p25,p25-trunk,851000000,chan.csv,,,primary\n"
                           "dmr,dmr-trunk,452000000,,500,1500,tier iii\n"
                           "conv,dmr-conventional,461000000,,750,,simplex\n",
                           target_path, sizeof target_path)
        != 0) {
        cleanup_paths(dir, NULL, chan_path);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.trunk_scan_idle_dwell_ms = 3000;
    opts.trunk_scan_activity_hold_ms = 1200;
    dsd_trunk_scan_target_list list;
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);

    int test_rc = 0;
    if (rc != 0 || list.count != 3) {
        DSD_FPRINTF(stderr, "parser valid mixed rc=%d count=%zu err=%s\n", rc, list.count, err);
        test_rc = 1;
    }
    if (test_rc == 0) {
        if (list.targets[0].type != DSD_TRUNK_SCAN_TARGET_P25_TRUNK || list.targets[0].frequency_hz != 851000000U
            || list.targets[0].dwell_ms != 3000 || list.targets[0].activity_hold_ms != 1200
            || !strstr(list.targets[0].chan_csv, "chan.csv")) {
            DSD_FPRINTF(stderr, "parser target 0 mismatch\n");
            test_rc = 1;
        }
        if (list.targets[1].dwell_ms != 500 || list.targets[1].activity_hold_ms != 1500) {
            DSD_FPRINTF(stderr, "parser per-target dwell/hold mismatch\n");
            test_rc = 1;
        }
    }

    cleanup_paths(dir, target_path, chan_path);
    return test_rc;
}

static int
expect_parser_rejects(const char* name, const char* body) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    if (write_targets_file(dir, body, target_path, sizeof target_path) != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.trunk_scan_idle_dwell_ms = 3000;
    opts.trunk_scan_activity_hold_ms = 1200;
    dsd_trunk_scan_target_list list;
    DSD_MEMSET(&list, 0xA5, sizeof list);
    list.count = 7;
    DSD_SNPRINTF(list.targets[0].id, sizeof list.targets[0].id, "%s", "sentinel");
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);
    int test_rc = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "%s should have been rejected\n", name);
        test_rc = 1;
    }
    if (list.count != 7 || strcmp(list.targets[0].id, "sentinel") != 0) {
        DSD_FPRINTF(stderr, "%s mutated output on failure\n", name);
        test_rc = 1;
    }

    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_parser_rejects_invalid_inputs(void) {
    int rc = 0;
    rc |= expect_parser_rejects("duplicate-id", "a,p25-trunk,851000000,,,,\n"
                                                "a,dmr-trunk,852000000,,,,\n");
    rc |= expect_parser_rejects("duplicate-type-frequency", "a,p25-trunk,851000000,,,,\n"
                                                            "b,p25-trunk,851000000,,,,\n");
    rc |= expect_parser_rejects("invalid-type", "a,nxdn,851000000,,,,\n");
    rc |= expect_parser_rejects("invalid-frequency", "a,p25-trunk,0,,,,\n");
    rc |= expect_parser_rejects("invalid-dwell", "a,p25-trunk,851000000,,249,,\n");
    rc |= expect_parser_rejects("conventional-chan-csv", "a,dmr-conventional,461000000,chan.csv,,,\n");
    return rc;
}

static int
test_parser_rejects_too_many_targets(void) {
    char body[8192];
    body[0] = '\0';
    for (int i = 0; i < DSD_TRUNK_SCAN_MAX_TARGETS + 1; i++) {
        char row[128];
        DSD_SNPRINTF(row, sizeof row, "id%d,dmr-conventional,%u,,,,\n", i, 461000000U + (unsigned)i);
        if (append_text(body, sizeof body, row) != 0) {
            return 1;
        }
    }
    return expect_parser_rejects("too-many-targets", body);
}

static int
make_runtime_targets(const char* body, char* out_path, size_t out_sz, char* out_dir, size_t out_dir_sz) {
    if (make_temp_dir(out_dir, out_dir_sz) != 0) {
        return -1;
    }
    return write_targets_file(out_dir, body, out_path, out_sz);
}

static void
reset_scan_opts_state(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_scan_enabled = 1;
    opts->trunk_scan_idle_dwell_ms = 250;
    opts->trunk_scan_activity_hold_ms = 250;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_dsp_bw_khz = 48;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_enc_calls = 1;
    state->dmr_mfid = -1;
    state->dmr_color_code = 16;
    state->dmr_confidence_color_code = 16;
    state->dmr_confidence_candidate_cc = 16;
    g_p25_tick_guard_available = 1;
    g_p25_tick_guard_depth = 0;
    g_p25_tick_guard_enter_calls = 0;
    g_p25_tick_guard_leave_calls = 0;
    g_fake_rtl_output_rate_hz = 0;
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
seed_target0_p25_state(dsd_state* state) {
    state->p25_prot_valid = 1;
    state->p25_prot_algid = 0x80;
    state->p25_prot_kid = 0x1234;
    state->p25_sys_time_valid = 1;
    state->p25_sys_time = 123456;
    state->p25_sys_time_offset_valid = 1;
    state->p25_sys_time_offset = -300;
    state->p25_patch_count = 1;
    state->p25_patch_sgid[0] = 100;
    state->p25_patch_is_patch[0] = 1;
    state->p25_patch_active[0] = 1;
    state->p25_patch_last_update[0] = 111;
    state->p25_patch_wgid_count[0] = 1;
    state->p25_patch_wgid[0][0] = 200;
    state->p25_patch_wuid_count[0] = 1;
    state->p25_patch_wuid[0][0] = 300;
    state->p25_patch_key[0] = 0x2222;
    state->p25_patch_alg[0] = 0x80;
    state->p25_patch_ssn[0] = 7;
    state->p25_patch_key_valid[0] = 1;
    state->p25_aff_count = 1;
    state->p25_aff_rid[0] = 400;
    state->p25_aff_last_seen[0] = 222;
    state->p25_ga_count = 1;
    state->p25_ga_rid[0] = 500;
    state->p25_ga_tg[0] = 600;
    state->p25_ga_last_seen[0] = 333;
    state->p25_nb_count = 1;
    state->p25_nb_entries[0].freq = 851500000;
    state->p25_nb_entries[0].sysid = 0x123;
    state->p25_nb_entries[0].rfss = 1;
    state->p25_nb_entries[0].site = 2;
    state->p25_nb_entries[0].cfva = 3;
    state->p25_nb_entries[0].last_seen = 444;
    state->p25_src_nid = 0xABCDE;
    state->p25_call_emergency[0] = 1;
    state->p25_call_priority[0] = 7;
    state->p25_call_is_packet[0] = 1;
}

static void
seed_target1_p25_state(dsd_state* state) {
    state->p25_prot_valid = 1;
    state->p25_prot_algid = 0x81;
    state->p25_prot_kid = 0x7777;
    state->p25_sys_time_valid = 1;
    state->p25_sys_time = 654321;
    state->p25_sys_time_offset_valid = 1;
    state->p25_sys_time_offset = 60;
    state->p25_patch_count = 1;
    state->p25_patch_sgid[0] = 900;
    state->p25_patch_is_patch[0] = 0;
    state->p25_patch_active[0] = 1;
    state->p25_patch_last_update[0] = 999;
    state->p25_patch_wgid_count[0] = 1;
    state->p25_patch_wgid[0][0] = 901;
    state->p25_patch_wuid_count[0] = 1;
    state->p25_patch_wuid[0][0] = 902;
    state->p25_patch_key[0] = 0x7777;
    state->p25_patch_alg[0] = 0x81;
    state->p25_patch_ssn[0] = 9;
    state->p25_patch_key_valid[0] = 1;
    state->p25_aff_count = 1;
    state->p25_aff_rid[0] = 903;
    state->p25_aff_last_seen[0] = 904;
    state->p25_ga_count = 1;
    state->p25_ga_rid[0] = 905;
    state->p25_ga_tg[0] = 906;
    state->p25_ga_last_seen[0] = 907;
    state->p25_nb_count = 1;
    state->p25_nb_entries[0].freq = 852500000;
    state->p25_nb_entries[0].sysid = 0x777;
    state->p25_nb_entries[0].rfss = 7;
    state->p25_nb_entries[0].site = 8;
    state->p25_nb_entries[0].cfva = 9;
    state->p25_nb_entries[0].last_seen = 908;
    state->p25_src_nid = 0x77777;
    state->p25_call_emergency[0] = 0;
    state->p25_call_priority[0] = 2;
    state->p25_call_is_packet[0] = 0;
}

static int
expect_empty_target_p25_state(const dsd_state* state) {
    if (state->p25_prot_valid != 0 || state->p25_sys_time_valid != 0 || state->p25_patch_count != 0
        || state->p25_aff_count != 0 || state->p25_ga_count != 0 || state->p25_nb_count != 0 || state->p25_src_nid != 0
        || state->p25_call_emergency[0] != 0 || state->p25_call_priority[0] != 0 || state->p25_call_is_packet[0] != 0) {
        DSD_FPRINTF(stderr, "target 0 P25 state leaked into empty target 1 snapshot\n");
        return 1;
    }
    return 0;
}

static int
expect_target0_p25_state(const dsd_state* state) {
    int test_rc = 0;
    if (state->p25_prot_valid != 1 || state->p25_prot_algid != 0x80 || state->p25_prot_kid != 0x1234
        || state->p25_sys_time_valid != 1 || state->p25_sys_time != 123456 || state->p25_sys_time_offset_valid != 1
        || state->p25_sys_time_offset != -300) {
        DSD_FPRINTF(stderr, "P25 protection/time state leaked across scan targets\n");
        test_rc = 1;
    }
    if (state->p25_patch_count != 1 || state->p25_patch_sgid[0] != 100 || state->p25_patch_is_patch[0] != 1
        || state->p25_patch_active[0] != 1 || state->p25_patch_last_update[0] != 111
        || state->p25_patch_wgid_count[0] != 1 || state->p25_patch_wgid[0][0] != 200
        || state->p25_patch_wuid_count[0] != 1 || state->p25_patch_wuid[0][0] != 300
        || state->p25_patch_key[0] != 0x2222 || state->p25_patch_alg[0] != 0x80 || state->p25_patch_ssn[0] != 7
        || state->p25_patch_key_valid[0] != 1) {
        DSD_FPRINTF(stderr, "P25 patch state leaked across scan targets\n");
        test_rc = 1;
    }
    if (state->p25_aff_count != 1 || state->p25_aff_rid[0] != 400 || state->p25_aff_last_seen[0] != 222
        || state->p25_ga_count != 1 || state->p25_ga_rid[0] != 500 || state->p25_ga_tg[0] != 600
        || state->p25_ga_last_seen[0] != 333) {
        DSD_FPRINTF(stderr, "P25 affiliation state leaked across scan targets\n");
        test_rc = 1;
    }
    if (state->p25_nb_count != 1 || state->p25_nb_entries[0].freq != 851500000
        || state->p25_nb_entries[0].sysid != 0x123 || state->p25_nb_entries[0].rfss != 1
        || state->p25_nb_entries[0].site != 2 || state->p25_nb_entries[0].cfva != 3
        || state->p25_nb_entries[0].last_seen != 444 || state->p25_src_nid != 0xABCDE
        || state->p25_call_emergency[0] != 1 || state->p25_call_priority[0] != 7 || state->p25_call_is_packet[0] != 1) {
        DSD_FPRINTF(stderr, "P25 neighbor/current-call state leaked across scan targets\n");
        test_rc = 1;
    }
    return test_rc;
}

static int
test_coordinator_idle_rotation_and_state_restore(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.p25_iden_fdma[1].base_freq = 12345;
    state.trunk_chan_map[99] = 851012500;
    state.dmr_rest_channel = 4;
    state.dmr_lcn_trust[4] = 2;
    seed_target0_p25_state(&state);

    dsd_engine_trunk_scan_test_set_now(0.24);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "scan rotated before dwell\n");
        test_rc = 1;
    }
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "scan did not rotate after dwell\n");
        test_rc = 1;
    }
    test_rc |= expect_empty_target_p25_state(&state);

    state.p25_iden_fdma[1].base_freq = 99999;
    state.trunk_chan_map[99] = 852012500;
    state.dmr_rest_channel = 8;
    state.dmr_lcn_trust[4] = 0;
    state.dmr_lcn_trust[8] = 2;
    seed_target1_p25_state(&state);

    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "scan did not rotate back to target 0\n");
        test_rc = 1;
    }
    if (state.p25_iden_fdma[1].base_freq != 12345 || state.trunk_chan_map[99] != 851012500
        || state.dmr_rest_channel != 4 || state.dmr_lcn_trust[4] != 2 || state.dmr_lcn_trust[8] != 0) {
        DSD_FPRINTF(stderr, "target state leaked across scan targets\n");
        test_rc = 1;
    }
    test_rc |= expect_target0_p25_state(&state);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static void
seed_dmr_identity(dsd_state* state, int mfid, const char* branding, const char* branding_sub, const char* site_parms) {
    state->dmr_mfid = mfid;
    DSD_SNPRINTF(state->dmr_branding, sizeof state->dmr_branding, "%s", branding);
    DSD_SNPRINTF(state->dmr_branding_sub, sizeof state->dmr_branding_sub, "%s", branding_sub);
    DSD_SNPRINTF(state->dmr_site_parms, sizeof state->dmr_site_parms, "%s", site_parms);
}

static int
expect_dmr_identity(const char* label, const dsd_state* state, int mfid, const char* branding, const char* branding_sub,
                    const char* site_parms) {
    if (state->dmr_mfid != mfid || strcmp(state->dmr_branding, branding) != 0
        || strcmp(state->dmr_branding_sub, branding_sub) != 0 || strcmp(state->dmr_site_parms, site_parms) != 0) {
        DSD_FPRINTF(stderr, "%s DMR identity mismatch mfid=%d branding='%s' sub='%s' site='%s'\n", label,
                    state->dmr_mfid, state->dmr_branding, state->dmr_branding_sub, state->dmr_site_parms);
        return 1;
    }
    return 0;
}

static int
test_dmr_branding_state_isolated_per_target(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("cap,dmr-trunk,451000000,,250,,\n"
                             "xpt,dmr-trunk,452000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr identity scan init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    seed_dmr_identity(&state, 0x10, "Motorola", "Cap+ ", "cap-site ");
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "dmr identity scan did not rotate to second target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_identity("fresh target", &state, -1, "", "", "");

    seed_dmr_identity(&state, 0x68, "  Hytera", "XPT ", "xpt-site ");
    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr identity scan did not rotate back to first target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_identity("restored target", &state, 0x10, "Motorola", "Cap+ ", "cap-site ");

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static void
seed_dmr_confidence(dsd_state* state, unsigned int cc) {
    state->dmr_color_code = cc;
    state->dmr_confidence_locked = 1;
    state->dmr_confidence_color_code = (uint8_t)cc;
    state->dmr_confidence_candidate_cc = (uint8_t)cc;
    state->dmr_confidence_candidate_count = 3;
    state->dmr_confidence_voice_sync_seen[0] = 1;
    state->dmr_confidence_voice_open[0] = 1;
    state->dmr_confidence_voice_count[0] = 2;
    state->dmr_confidence_mismatch_count = 1;
}

static int
expect_dmr_confidence(const char* label, const dsd_state* state, unsigned int cc, uint8_t locked) {
    if (state->dmr_color_code != cc || state->dmr_confidence_locked != locked || state->dmr_confidence_color_code != cc
        || state->dmr_confidence_candidate_cc != cc) {
        DSD_FPRINTF(stderr, "%s DMR confidence mismatch cc=%u locked=%u conf_cc=%u candidate=%u\n", label,
                    state->dmr_color_code, state->dmr_confidence_locked, state->dmr_confidence_color_code,
                    state->dmr_confidence_candidate_cc);
        return 1;
    }
    if (!locked) {
        return 0;
    }
    if (state->dmr_confidence_candidate_count != 3 || state->dmr_confidence_voice_sync_seen[0] != 1
        || state->dmr_confidence_voice_open[0] != 1 || state->dmr_confidence_voice_count[0] != 2
        || state->dmr_confidence_mismatch_count != 1) {
        DSD_FPRINTF(stderr, "%s DMR confidence counters mismatch count=%u seen=%u open=%u voice=%u mismatch=%u\n",
                    label, state->dmr_confidence_candidate_count, state->dmr_confidence_voice_sync_seen[0],
                    state->dmr_confidence_voice_open[0], state->dmr_confidence_voice_count[0],
                    state->dmr_confidence_mismatch_count);
        return 1;
    }
    return 0;
}

static int
test_dmr_confidence_state_isolated_per_target(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("cap,dmr-trunk,451000000,,250,,\n"
                             "xpt,dmr-trunk,452000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr confidence scan init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    seed_dmr_confidence(&state, 3);
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "dmr confidence scan did not rotate to second target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_confidence("fresh target", &state, 16, 0);

    seed_dmr_confidence(&state, 7);
    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr confidence scan did not rotate back to first target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_confidence("restored target", &state, 3, 1);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_p25_targets_seed_valid_control_channel_timing(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "p25 timing scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }
    if (state.samplesPerSymbol != 10 || state.symbolCenter != 4) {
        DSD_FPRINTF(stderr, "initial P25 scan target timing invalid sps=%d center=%d\n", state.samplesPerSymbol,
                    state.symbolCenter);
        test_rc = 1;
    }
    state.p25_cc_is_tdma = 1;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;

    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.samplesPerSymbol != 10 || state.symbolCenter != 4) {
        DSD_FPRINTF(stderr, "second P25 scan target timing invalid active=%zu sps=%d center=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.samplesPerSymbol, state.symbolCenter);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.samplesPerSymbol != 8 || state.symbolCenter != 3) {
        DSD_FPRINTF(stderr, "restored P25 TDMA scan target timing invalid active=%zu sps=%d center=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.samplesPerSymbol, state.symbolCenter);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_mixed_target_switch_resets_dmr_demod_profile(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("p25,p25-trunk,851000000,,250,,\n"
                             "dmr,dmr-trunk,452000000,,250,,\n"
                             "conv,dmr-conventional,461000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "mixed demod scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.rf_mod = 1;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.rf_mod != 2 || state.samplesPerSymbol != 10
        || state.symbolCenter != 4) {
        DSD_FPRINTF(stderr, "DMR trunk target inherited P25 demod state active=%zu rf_mod=%d sps=%d center=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod, state.samplesPerSymbol,
                    state.symbolCenter);
        test_rc = 1;
    }

    state.rf_mod = 1;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 2 || state.rf_mod != 2 || state.samplesPerSymbol != 10
        || state.symbolCenter != 4) {
        DSD_FPRINTF(stderr, "DMR conventional target inherited P25 demod state active=%zu rf_mod=%d sps=%d center=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod, state.samplesPerSymbol,
                    state.symbolCenter);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_conventional_activity_hold_and_allowlist_block(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,dmr-conventional,461000000,,250,250,\n"
                             "b,dmr-conventional,462000000,,250,250,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "conventional scan init failed: %s\n", err);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_dmr_conventional_activity(&opts, &state, 1001, 2002, 0, 0, 0);
    dsd_engine_trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "allowed conventional activity did not hold target\n");
        test_rc = 1;
    }
    dsd_engine_trunk_scan_test_set_now(0.61);
    dsd_engine_trunk_scan_tick(&opts, &state);
    dsd_engine_trunk_scan_test_set_now(0.87);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "target did not rotate after conventional hold and dwell\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);

    reset_scan_opts_state(&opts, &state);
    opts.trunk_use_allow_list = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    dsd_engine_trunk_scan_test_set_now(0.0);
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "allowlist scan init failed: %s\n", err);
        test_rc = 1;
    }
    dsd_engine_trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_dmr_conventional_activity(&opts, &state, 1001, 2002, 0, 0, 0);
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "blocked allow-list traffic held conventional target\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_conventional_activity_encrypted_lockout_does_not_hold(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,dmr-conventional,461000000,,250,250,\n"
                             "b,dmr-conventional,462000000,,250,250,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.trunk_tune_enc_calls = 0;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "encrypted lockout scan init failed: %s\n", err);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_dmr_conventional_activity(&opts, &state, 1001, 2002, 0, 1, 0);
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "encrypted conventional traffic held target despite lockout\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_state_ext_cleanup_clears_scan_hooks(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,dmr-conventional,461000000,,250,250,\n", target_path, sizeof target_path, dir,
                             sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "cleanup hook scan init failed: %s\n", err);
        test_rc = 1;
    }
    if (test_rc == 0 && dsd_trunk_scan_hook_dmr_ctx() == NULL) {
        DSD_FPRINTF(stderr, "scan hook dmr ctx missing before cleanup\n");
        test_rc = 1;
    }

    dsd_state_ext_free_all(&state);
    if (dsd_trunk_scan_hook_dmr_ctx() != NULL || dsd_trunk_scan_hook_p25_ctx() != NULL) {
        DSD_FPRINTF(stderr, "scan hooks remained installed after state extension cleanup\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_dmr_trunk_sm_timeout_releases_scan_hold(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,dmr-trunk,451000000,,250,,\n"
                             "b,dmr-trunk,452000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    dmr_sm_ctx_t* active_dmr = (dmr_sm_ctx_t*)dsd_engine_trunk_scan_active_dmr_ctx();
    if (!active_dmr) {
        DSD_FPRINTF(stderr, "dmr scan active ctx missing\n");
        test_rc = 1;
    } else {
        active_dmr->state = DMR_SM_TUNED;
    }
    opts.p25_is_tuned = 0;
    opts.trunk_is_tuned = 1;
    g_dmr_tick_calls = 0;

    dsd_engine_trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr active call did not hold scan target\n");
        test_rc = 1;
    }

    opts.trunk_is_tuned = 0;
    g_dmr_tick_calls = 0;
    g_dmr_tick_release_tuned = 1;

    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (g_dmr_tick_calls == 0) {
        DSD_FPRINTF(stderr, "dmr target SM was not ticked before scan hold check\n");
        test_rc = 1;
    }
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr scan rotated before post-release idle dwell restart\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "dmr scan did not rotate after SM timeout released hold\n");
        test_rc = 1;
    }

    g_dmr_tick_release_tuned = 0;
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static dsd_trunk_tune_result
failing_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

static int g_counting_tune_to_cc_calls = 0;
static int g_counting_tune_to_cc_failures_remaining = 0;
static int g_counting_tune_to_cc_ted_sps = 0;
static long int g_counting_tune_to_cc_freq = 0;

static dsd_trunk_tune_result
counting_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    g_counting_tune_to_cc_calls++;
    g_counting_tune_to_cc_ted_sps = ted_sps;
    g_counting_tune_to_cc_freq = freq;
    if (g_counting_tune_to_cc_failures_remaining > 0) {
        g_counting_tune_to_cc_failures_remaining--;
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    if (state) {
        state->trunk_cc_freq = freq;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static int
test_p25_targets_pass_cc_sps_to_retune_paths(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_result = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_ted_sps = 0;

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || g_counting_tune_to_cc_calls != 1 || g_counting_tune_to_cc_ted_sps != 10) {
        DSD_FPRINTF(stderr, "p25 initial retune did not receive P25 CC sps rc=%d calls=%d sps=%d err=%s\n", rc,
                    g_counting_tune_to_cc_calls, g_counting_tune_to_cc_ted_sps, err);
        test_rc = 1;
    }

    state.p25_cc_is_tdma = 1;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || g_counting_tune_to_cc_ted_sps != 10) {
        DSD_FPRINTF(stderr, "p25 fdma target retune did not receive FDMA CC sps active=%zu sps=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_ted_sps);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_ted_sps != 8) {
        DSD_FPRINTF(stderr, "p25 tdma target retune did not receive TDMA CC sps active=%zu sps=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_ted_sps);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_p25_targets_use_rtl_output_rate_for_retune_sps(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.rtl_dsp_bw_khz = 48;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    dsd_rtl_stream_metrics_hooks metrics_hooks = {0};
    metrics_hooks.output_rate_hz = fake_rtl_output_rate_hz;
    g_fake_rtl_output_rate_hz = 24000U;
    dsd_rtl_stream_metrics_hooks_set(&metrics_hooks);

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_result = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_ted_sps = 0;
    g_counting_tune_to_cc_freq = 0;

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || g_counting_tune_to_cc_calls != 1 || g_counting_tune_to_cc_ted_sps != 5) {
        DSD_FPRINTF(stderr, "p25 initial retune did not use RTL output rate rc=%d calls=%d sps=%d err=%s\n", rc,
                    g_counting_tune_to_cc_calls, g_counting_tune_to_cc_ted_sps, err);
        test_rc = 1;
    }

    state.p25_cc_is_tdma = 1;
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_ted_sps != 4) {
        DSD_FPRINTF(stderr, "p25 TDMA retune did not use RTL output rate active=%zu sps=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_ted_sps);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_channel_map_sequence_advances_on_equal_count_target_switches(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }

    char chan_a_path[DSD_TEST_PATH_MAX];
    char chan_b_path[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(chan_a_path, sizeof chan_a_path, dir, "chan_a.csv") != 0
        || dsd_test_path_join(chan_b_path, sizeof chan_b_path, dir, "chan_b.csv") != 0
        || write_text_file(chan_a_path, "channel,frequency\n101,851012500\n") != 0
        || write_text_file(chan_b_path, "channel,frequency\n202,852012500\n") != 0
        || write_targets_file(dir,
                              "a,p25-trunk,851000000,chan_a.csv,250,,\n"
                              "b,p25-trunk,852000000,chan_b.csv,250,,\n",
                              target_path, sizeof target_path)
               != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    const uint64_t seq0 = state.trunk_chan_map_seq;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || state.trunk_chan_map_used_count != 1U
        || state.trunk_chan_map[101] != 851012500L || state.trunk_chan_map[202] != 0) {
        DSD_FPRINTF(stderr, "channel-map scan init failed rc=%d active=%zu count=%u a=%ld b=%ld err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), state.trunk_chan_map_used_count,
                    state.trunk_chan_map[101], state.trunk_chan_map[202], err);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    const uint64_t seq1 = state.trunk_chan_map_seq;
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.trunk_chan_map_used_count != 1U
        || state.trunk_chan_map[101] != 0 || state.trunk_chan_map[202] != 852012500L || seq1 <= seq0) {
        DSD_FPRINTF(
            stderr, "channel-map target switch kept stale map or seq active=%zu count=%u a=%ld b=%ld seq=%llu/%llu\n",
            dsd_engine_trunk_scan_active_index(&state), state.trunk_chan_map_used_count, state.trunk_chan_map[101],
            state.trunk_chan_map[202], (unsigned long long)seq0, (unsigned long long)seq1);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    const uint64_t seq2 = state.trunk_chan_map_seq;
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.trunk_chan_map_used_count != 1U
        || state.trunk_chan_map[101] != 851012500L || state.trunk_chan_map[202] != 0 || seq2 <= seq1) {
        DSD_FPRINTF(
            stderr, "channel-map return switch kept stale map or seq active=%zu count=%u a=%ld b=%ld seq=%llu/%llu\n",
            dsd_engine_trunk_scan_active_index(&state), state.trunk_chan_map_used_count, state.trunk_chan_map[101],
            state.trunk_chan_map[202], (unsigned long long)seq1, (unsigned long long)seq2);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    (void)remove(chan_b_path);
    cleanup_paths(dir, target_path, chan_a_path);
    return test_rc;
}

static int
test_trunk_targets_reuse_restored_control_channel(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("p25a,p25-trunk,851000000,,250,,\n"
                             "p25b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_result = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_ted_sps = 0;
    g_counting_tune_to_cc_freq = 0;

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "p25 learned CC scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.p25_cc_freq = 851500000L;
    state.trunk_cc_freq = 851500000L;
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    state.p25_cc_freq = 852500000L;
    state.trunk_cc_freq = 852500000L;
    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_freq != 851500000L
        || state.p25_cc_freq != 851500000L || state.trunk_cc_freq != 851500000L) {
        DSD_FPRINTF(stderr, "p25 target did not reuse learned CC active=%zu tune=%ld p25=%ld trunk=%ld\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_freq, state.p25_cc_freq,
                    state.trunk_cc_freq);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);

    if (make_runtime_targets("dmra,dmr-trunk,451000000,,250,,\n"
                             "dmrb,dmr-trunk,452000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        DSD_MEMSET(&hooks, 0, sizeof hooks);
        dsd_trunk_tuning_hooks_set(hooks);
        return 1;
    }

    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_freq = 0;
    dsd_engine_trunk_scan_test_set_now(0.0);
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr learned CC scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.trunk_cc_freq = 451500000L;
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    state.trunk_cc_freq = 452500000L;
    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_freq != 451500000L
        || state.p25_cc_freq != 0 || state.trunk_cc_freq != 451500000L) {
        DSD_FPRINTF(stderr, "dmr target did not reuse learned CC active=%zu tune=%ld p25=%ld trunk=%ld\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_freq, state.p25_cc_freq,
                    state.trunk_cc_freq);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_locked_demod_mode_preserved_when_seeding_targets(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("p25,p25-trunk,851000000,,250,,\n"
                             "dmr,dmr-trunk,452000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || state.rf_mod != 1) {
        DSD_FPRINTF(stderr, "locked P25 demod not preserved on init rc=%d active=%zu rf_mod=%d err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod, err);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.rf_mod != 1) {
        DSD_FPRINTF(stderr, "locked demod overwritten on DMR target active=%zu rf_mod=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_scan_tick_skips_rotation_when_p25_guard_busy(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "guard scan init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    g_p25_tick_guard_available = 0;
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_p25_tick_guard_leave_calls != 0) {
        DSD_FPRINTF(stderr, "scan rotated or left guard while P25 guard busy active=%zu leaves=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_p25_tick_guard_leave_calls);
        test_rc = 1;
    }

    g_p25_tick_guard_available = 1;
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || g_p25_tick_guard_depth != 0
        || g_p25_tick_guard_leave_calls != 1) {
        DSD_FPRINTF(stderr, "scan did not rotate cleanly after P25 guard released active=%zu depth=%d leaves=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_p25_tick_guard_depth, g_p25_tick_guard_leave_calls);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_single_target_retune_failure_retries_after_cooldown(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n", target_path, sizeof target_path, dir, sizeof dir) != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_result = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 1;
    g_counting_tune_to_cc_ted_sps = 0;

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "single target init rc=%d calls=%d err=%s\n", rc, g_counting_tune_to_cc_calls, err);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(1.99);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "single target retried before cooldown expired\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(2.01);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (g_counting_tune_to_cc_calls != 2) {
        DSD_FPRINTF(stderr, "single target did not retry after cooldown; calls=%d\n", g_counting_tune_to_cc_calls);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_retune_failure_cooldown(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "retune scan init failed: %s\n", err);
        test_rc = 1;
    }

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_result = failing_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "failed retune should not leave scanner on failed target\n");
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_test_set_now(2.40);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "target did not rotate after retry cooldown expired\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_scan_does_not_retune_active_target_while_alternates_cool_down(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_result = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "cooldown scan init failed rc=%d active=%zu calls=%d err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls, err);
        test_rc = 1;
    }

    g_counting_tune_to_cc_failures_remaining = 1;
    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    int calls_after_failed_alternate = g_counting_tune_to_cc_calls;
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || calls_after_failed_alternate < 2) {
        DSD_FPRINTF(stderr, "failed alternate retune did not leave active target restored active=%zu calls=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), calls_after_failed_alternate);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0
        || g_counting_tune_to_cc_calls != calls_after_failed_alternate) {
        DSD_FPRINTF(stderr, "active target retuned while alternate cooling active=%zu calls=%d was=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls,
                    calls_after_failed_alternate);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_dmr_targets_pass_sps_to_retune_paths(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("p25,p25-trunk,851000000,,250,,\n"
                             "dmr,dmr-trunk,452000000,,250,,\n"
                             "conv,dmr-conventional,461000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_result = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_ted_sps = 0;
    g_scan_tune_to_freq_ted_sps = 0;

    char err[256] = {0};
    dsd_engine_trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr sps scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || g_counting_tune_to_cc_ted_sps != 10) {
        DSD_FPRINTF(stderr, "dmr trunk retune did not receive DMR sps active=%zu sps=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_ted_sps);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 2 || g_scan_tune_to_freq_ted_sps != 10) {
        DSD_FPRINTF(stderr, "dmr conventional retune did not receive DMR sps active=%zu sps=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_scan_tune_to_freq_ted_sps);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_engine_trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_init_failure_restores_saved_trunk_opts(void) {
    char dir[DSD_TEST_PATH_MAX];
    char chan_path[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    if (dsd_test_path_join(chan_path, sizeof chan_path, dir, "chan.csv") != 0
        || write_text_file(chan_path, "channel,frequency\n1,851012500\n") != 0
        || write_targets_file(dir, "dmr,dmr-trunk,452000000,chan.csv,250,,\n", target_path, sizeof target_path) != 0) {
        cleanup_paths(dir, NULL, chan_path);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.p25_trunk = 1;
    opts.trunk_enable = 0;
    opts.p25_is_tuned = 1;
    opts.trunk_is_tuned = 0;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    g_csv_import_result = -1;
    char err[256] = {0};
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    g_csv_import_result = 0;

    int test_rc = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "scan init should have failed on chan_csv import\n");
        test_rc = 1;
    }
    if (opts.p25_trunk != 1 || opts.trunk_enable != 0 || opts.p25_is_tuned != 1 || opts.trunk_is_tuned != 0) {
        DSD_FPRINTF(stderr,
                    "scan init failure did not restore trunk opts p25=%d trunk=%d p25_tuned=%d trunk_tuned=%d\n",
                    opts.p25_trunk, opts.trunk_enable, opts.p25_is_tuned, opts.trunk_is_tuned);
        test_rc = 1;
    }
    if (dsd_engine_trunk_scan_active_index(&state) != (size_t)-1) {
        DSD_FPRINTF(stderr, "failed scan init attached a coordinator\n");
        test_rc = 1;
    }

    cleanup_paths(dir, target_path, chan_path);
    return test_rc;
}

static int
test_trunk_scan_rejects_fixed_input_without_tuner(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.use_rigctl = 0;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc == 0 || strstr(err, "requires RTL input or rigctl") == NULL) {
        DSD_FPRINTF(stderr, "fixed input scan should reject without tuner rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }
    if (dsd_engine_trunk_scan_active_index(&state) != (size_t)-1) {
        DSD_FPRINTF(stderr, "fixed input rejection attached a coordinator\n");
        test_rc = 1;
    }

    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_trunk_scan_rejects_iq_replay_input(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.iq_replay_requested = 1;
    opts.use_rigctl = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc == 0 || strstr(err, "IQ replay") == NULL) {
        DSD_FPRINTF(stderr, "IQ replay scan should reject requested replay rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }
    if (dsd_engine_trunk_scan_active_index(&state) != (size_t)-1) {
        DSD_FPRINTF(stderr, "IQ replay rejection attached a coordinator\n");
        test_rc = 1;
    }

    reset_scan_opts_state(&opts, &state);
    opts.iq_replay_active = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    err[0] = '\0';
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc == 0 || strstr(err, "IQ replay") == NULL) {
        DSD_FPRINTF(stderr, "IQ replay scan should reject active replay rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }
    if (dsd_engine_trunk_scan_active_index(&state) != (size_t)-1) {
        DSD_FPRINTF(stderr, "active IQ replay rejection attached a coordinator\n");
        test_rc = 1;
    }

    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_parser_valid_mixed_targets_and_relative_chan_csv();
    rc |= test_parser_rejects_invalid_inputs();
    rc |= test_parser_rejects_too_many_targets();
    rc |= test_coordinator_idle_rotation_and_state_restore();
    rc |= test_dmr_branding_state_isolated_per_target();
    rc |= test_dmr_confidence_state_isolated_per_target();
    rc |= test_p25_targets_seed_valid_control_channel_timing();
    rc |= test_mixed_target_switch_resets_dmr_demod_profile();
    rc |= test_conventional_activity_hold_and_allowlist_block();
    rc |= test_conventional_activity_encrypted_lockout_does_not_hold();
    rc |= test_state_ext_cleanup_clears_scan_hooks();
    rc |= test_dmr_trunk_sm_timeout_releases_scan_hold();
    rc |= test_p25_targets_pass_cc_sps_to_retune_paths();
    rc |= test_p25_targets_use_rtl_output_rate_for_retune_sps();
    rc |= test_channel_map_sequence_advances_on_equal_count_target_switches();
    rc |= test_trunk_targets_reuse_restored_control_channel();
    rc |= test_locked_demod_mode_preserved_when_seeding_targets();
    rc |= test_scan_tick_skips_rotation_when_p25_guard_busy();
    rc |= test_single_target_retune_failure_retries_after_cooldown();
    rc |= test_retune_failure_cooldown();
    rc |= test_scan_does_not_retune_active_target_while_alternates_cool_down();
    rc |= test_dmr_targets_pass_sps_to_retune_paths();
    rc |= test_init_failure_restores_saved_trunk_opts();
    rc |= test_trunk_scan_rejects_fixed_input_without_tuner();
    rc |= test_trunk_scan_rejects_iq_replay_input();
    return rc;
}
