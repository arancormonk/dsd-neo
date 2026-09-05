// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/p25_bandplan_export.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dsd-neo/core/csv_validate.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/platform/sockets.h"
#include "test_support.h"
#include "trunk_scan_internal.h"
#include "trunk_scan_test_support.h"

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

int
rtl_stream_get_tuner_autogain(void) {
    return 1;
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
    ctx->state = (opts && state && opts->trunk_enable == 1 && state->trunk_cc_freq != 0) ? P25_SM_ON_CC : P25_SM_IDLE;
}

int
p25_sm_restart_pending_cc_acquisition(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double tune_start_m,
                                      const char* source) {
    (void)opts;
    (void)source;
    if (!ctx) {
        return 0;
    }
    ctx->state = P25_SM_ON_CC;
    ctx->cc_tune_request_id = 0U;
    ctx->cc_tune_pending = 0;
    ctx->cc_sync_pending = 1;
    ctx->cc_acquisition_origin = P25_SM_CC_ACQUISITION_RETURN;
    ctx->t_cc_sync_m = tune_start_m;
    ctx->t_cc_tune_m = tune_start_m;
    ctx->t_hunt_try_m = 0.0;
    if (state) {
        if (state->p25_cc_eval_freq != 0) {
            state->p25_cc_eval_start_m = ctx->t_cc_tune_m;
        }
        state->last_cc_sync_time_m = tune_start_m;
        state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC;
    }
    return 1;
}

int
p25_sm_await_pending_cc_tune(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, uint64_t request_id,
                             const char* source) {
    (void)opts;
    (void)source;
    if (!ctx || request_id == 0U) {
        return 0;
    }
    ctx->state = P25_SM_ON_CC;
    ctx->cc_tune_request_id = request_id;
    ctx->cc_tune_pending = 1;
    ctx->cc_sync_pending = 1;
    ctx->cc_acquisition_origin = P25_SM_CC_ACQUISITION_RETURN;
    ctx->t_cc_tune_m = 0.0;
    if (state) {
        state->p25_cc_eval_start_m = 0.0;
        state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC;
    }
    return 1;
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
    ctx->state = (opts && state && opts->trunk_enable == 1 && state->p25_cc_freq == 0 && state->trunk_cc_freq != 0)
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
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
    }
}

dsd_trunk_tune_result
dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t* out_request_id) {
    if (out_request_id) {
        *out_request_id = 0U;
    }
    g_scan_tune_to_freq_ted_sps = ted_sps;
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
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
        /* The optional third column the real importer reads as a row name. This stub keeps
         * no scan-list rows, so every name it sees lands in row 0: enough to prove the store
         * was allocated, which is what the trunk-scan import then has to release. */
        char* name_text = strchr(freq_text, ',');
        if (name_text) {
            *name_text++ = '\0';
        }
        /* A fourth column seeds a per-row key set, the way the real importer
         * loads key cells: enough to prove the store was allocated, which is
         * what the trunk-scan import then has to release. */
        char* key_text = NULL;
        if (name_text) {
            key_text = strchr(name_text, ',');
            if (key_text) {
                *key_text++ = '\0';
            }
        }
        char* tail = key_text ? key_text : (name_text ? name_text : freq_text);
        while (*tail && *tail != '\r' && *tail != '\n') {
            tail++;
        }
        *tail = '\0';
        uint32_t channel = 0;
        long freq = 0;
        if (dsd_parse_uint32_strict(line, 10, 0xFFFFU, &channel) == 0
            && dsd_parse_long_strict(freq_text, 10, 0L, LONG_MAX, &freq) == 0) {
            dsd_state_set_trunk_chan_freq(state, channel, freq);
            if (name_text && name_text[0] != '\0') {
                (void)dsd_state_trunk_lcn_name_set(state, 0U, name_text);
            }
            if (key_text && key_text[0] != '\0') {
                dsd_key_set ks;
                DSD_MEMSET(&ks, 0, sizeof(ks));
                ks.entries = (dsd_key_set_entry*)calloc(1U, sizeof(*ks.entries));
                if (ks.entries != NULL) {
                    ks.count = 1U;
                    ks.present = 1;
                    ks.keyloader = 1;
                    ks.entries[0].index = 9U;
                    ks.entries[0].value = 777ULL;
                    ks.entries[0].loaded = 1U;
                    (void)dsd_state_trunk_lcn_keys_set(state, 0U, &ks);
                }
            }
        }
    }
    (void)fclose(fp);
    return g_csv_import_result;
}

/*
 * Per-target key stubs: a readable key file seeds one keyring slot, the way the
 * real importer loads rows, so the coordinator's install/restore is observable.
 * dsd_key_set_load_csv() runs these against its throwaway state and captures.
 */
static int
seed_scan_key_slot(dsd_state* state, const char* path) {
    if (!state || !path || path[0] == '\0') {
        return -1;
    }
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    (void)fclose(fp);
    state->rkey_array[9] = 999ULL;
    state->rkey_array_loaded[9] = 1U;
    return 0;
}

int
csvKeyImportHexPath(const char* path, int show_keys, dsd_state* state, dsd_csv_validation* stats) {
    (void)show_keys;
    (void)stats;
    return seed_scan_key_slot(state, path);
}

int
csvKeyImportDecPath(const char* path, int show_keys, dsd_state* state, dsd_csv_validation* stats) {
    (void)show_keys;
    (void)stats;
    return seed_scan_key_slot(state, path);
}

int
dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                  int encrypted, int data_call, dsd_tg_policy_decision* out) {
    (void)state;
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
                                    int encrypted, int data_call, dsd_tg_policy_decision* out) {
    (void)state;
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
write_targets_file_with_header(const char* dir, const char* header, const char* body, char* out_path, size_t out_sz) {
    if (dsd_test_path_join(out_path, out_sz, dir, "targets.csv") != 0) {
        return -1;
    }
    char content[8192];
    int n = DSD_SNPRINTF(content, sizeof content, "%s%s", header, body);
    if (n < 0 || (size_t)n >= sizeof content) {
        return -1;
    }
    return write_text_file(out_path, content);
}

static int
write_targets_file(const char* dir, const char* body, char* out_path, size_t out_sz) {
    return write_targets_file_with_header(dir, k_header, body, out_path, out_sz);
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
    DSD_MEMSET(&list, 0, sizeof list);
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
        /* The id is what the coordinator publishes as the channel label, so the parser
         * owes it verbatim rather than just "some non-empty string". */
        if (strcmp(list.targets[0].id, "p25") != 0 || strcmp(list.targets[1].id, "dmr") != 0
            || strcmp(list.targets[2].id, "conv") != 0) {
            DSD_FPRINTF(stderr, "parser target ids mismatch: '%s' '%s' '%s'\n", list.targets[0].id, list.targets[1].id,
                        list.targets[2].id);
            test_rc = 1;
        }
        if (list.targets[1].dwell_ms != 500 || list.targets[1].activity_hold_ms != 1500) {
            DSD_FPRINTF(stderr, "parser per-target dwell/hold mismatch\n");
            test_rc = 1;
        }
    }

    dsd_trunk_scan_target_list_reset(&list);
    cleanup_paths(dir, target_path, chan_path);
    return test_rc;
}

static int
test_parser_accepts_nxdn_targets(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char chan_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(chan_path, sizeof chan_path, dir, "chan.csv") != 0
        || write_text_file(chan_path, "channel,frequency\n1,461037500\n") != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation\n";
    if (write_targets_file_with_header(dir, header,
                                       "nxdn,nxdn-trunk,461000000,chan.csv,,,site,gfsk\n"
                                       "conv,nxdn-conventional,462000000,,250,500,,auto\n"
                                       "conv48,nxdn48-conventional,462000000,,250,500,,gfsk\n",
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
    DSD_MEMSET(&list, 0, sizeof list);
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);

    int test_rc = 0;
    if (rc != 0 || list.count != 3) {
        DSD_FPRINTF(stderr, "parser nxdn rc=%d count=%zu err=%s\n", rc, list.count, err);
        test_rc = 1;
    }
    if (test_rc == 0) {
        if (list.targets[0].type != DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK || list.targets[0].frequency_hz != 461000000U
            || list.targets[0].dwell_ms != 3000 || list.targets[0].activity_hold_ms != 1200
            || list.targets[0].modulation != DSD_TRUNK_SCAN_MODULATION_GFSK
            || !strstr(list.targets[0].chan_csv, "chan.csv")) {
            DSD_FPRINTF(stderr, "parser nxdn trunk target 0 mismatch\n");
            test_rc = 1;
        }
        if (list.targets[1].type != DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL
            || list.targets[1].frequency_hz != 462000000U || list.targets[1].dwell_ms != 250
            || list.targets[1].activity_hold_ms != 500
            || list.targets[1].modulation != DSD_TRUNK_SCAN_MODULATION_AUTO) {
            DSD_FPRINTF(stderr, "parser nxdn conventional target 1 mismatch\n");
            test_rc = 1;
        }
        /* Same frequency as target 1 on purpose: the two NXDN conventional variants are distinct
           target types, so the (type, frequency) duplicate check must not collapse them. */
        if (list.targets[2].type != DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL
            || list.targets[2].frequency_hz != 462000000U || list.targets[2].dwell_ms != 250
            || list.targets[2].activity_hold_ms != 500 || list.targets[2].modulation != DSD_TRUNK_SCAN_MODULATION_GFSK
            || list.targets[2].chan_csv[0] != '\0') {
            DSD_FPRINTF(stderr, "parser nxdn48 conventional target 2 mismatch\n");
            test_rc = 1;
        }
    }

    dsd_trunk_scan_target_list_reset(&list);
    cleanup_paths(dir, target_path, chan_path);
    return test_rc;
}

static int
test_parser_accepts_quoted_chan_csv_with_comma(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char chan_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(chan_path, sizeof chan_path, dir, "site,1.csv") != 0
        || write_text_file(chan_path, "channel,frequency\n1,851012500\n") != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    if (write_targets_file(dir, "p25,p25-trunk,851000000,\"site,1.csv\",,,quoted path\n", target_path,
                           sizeof target_path)
        != 0) {
        cleanup_paths(dir, NULL, chan_path);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.trunk_scan_idle_dwell_ms = 3000;
    opts.trunk_scan_activity_hold_ms = 1200;
    dsd_trunk_scan_target_list list;
    DSD_MEMSET(&list, 0, sizeof list);
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);

    int test_rc = 0;
    if (rc != 0 || list.count != 1 || strstr(list.targets[0].chan_csv, "site,1.csv") == NULL) {
        DSD_FPRINTF(stderr, "quoted chan_csv parse rc=%d count=%zu path='%s' err=%s\n", rc, list.count,
                    rc == 0 ? list.targets[0].chan_csv : "", err);
        test_rc = 1;
    }

    dsd_trunk_scan_target_list_reset(&list);
    cleanup_paths(dir, target_path, chan_path);
    return test_rc;
}

static int
test_parser_accepts_optional_modulation_and_gain_columns(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,rtl_gain,modulation\n";
    if (write_targets_file_with_header(dir, header,
                                       "p25,p25-trunk,851000000,,250,,primary,27,cqpsk\n"
                                       "dmr,dmr-trunk,452000000,,250,,tier iii,auto,gfsk\n"
                                       "plain,dmr-conventional,461000000,,250,,basic row\n",
                                       target_path, sizeof target_path)
        != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.trunk_scan_idle_dwell_ms = 3000;
    opts.trunk_scan_activity_hold_ms = 1200;
    dsd_trunk_scan_target_list list;
    DSD_MEMSET(&list, 0, sizeof list);
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);

    int test_rc = 0;
    if (rc != 0 || list.count != 3) {
        DSD_FPRINTF(stderr, "optional parser rc=%d count=%zu err=%s\n", rc, list.count, err);
        test_rc = 1;
    }
    if (test_rc == 0) {
        if (list.targets[0].modulation != DSD_TRUNK_SCAN_MODULATION_CQPSK || list.targets[0].rtl_gain_is_set != 1
            || list.targets[0].rtl_gain_db != 27) {
            DSD_FPRINTF(stderr, "optional parser P25 fields mismatch mod=%d gain_set=%d gain=%d\n",
                        list.targets[0].modulation, list.targets[0].rtl_gain_is_set, list.targets[0].rtl_gain_db);
            test_rc = 1;
        }
        if (list.targets[1].modulation != DSD_TRUNK_SCAN_MODULATION_GFSK || list.targets[1].rtl_gain_is_set != 1
            || list.targets[1].rtl_gain_db != 0) {
            DSD_FPRINTF(stderr, "optional parser DMR fields mismatch mod=%d gain_set=%d gain=%d\n",
                        list.targets[1].modulation, list.targets[1].rtl_gain_is_set, list.targets[1].rtl_gain_db);
            test_rc = 1;
        }
        if (list.targets[2].modulation != DSD_TRUNK_SCAN_MODULATION_UNSET || list.targets[2].rtl_gain_is_set != 0) {
            DSD_FPRINTF(stderr, "optional parser missing fields not treated as empty\n");
            test_rc = 1;
        }
    }

    dsd_trunk_scan_target_list_reset(&list);
    cleanup_paths(dir, target_path, NULL);
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
    static dsd_trunk_scan_target sentinel[7];
    DSD_MEMSET(sentinel, 0, sizeof sentinel);
    DSD_SNPRINTF(sentinel[0].id, sizeof sentinel[0].id, "%s", "sentinel");
    list.targets = sentinel;
    list.count = 7;
    list.capacity = 7;
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);
    int test_rc = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "%s should have been rejected\n", name);
        test_rc = 1;
    }
    if (list.count != 7 || list.targets != sentinel || list.capacity != 7
        || strcmp(list.targets[0].id, "sentinel") != 0) {
        DSD_FPRINTF(stderr, "%s mutated output on failure\n", name);
        test_rc = 1;
    }

    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
expect_parser_rejects_with_header(const char* name, const char* header, const char* body) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    if (write_targets_file_with_header(dir, header, body, target_path, sizeof target_path) != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.trunk_scan_idle_dwell_ms = 3000;
    opts.trunk_scan_activity_hold_ms = 1200;
    dsd_trunk_scan_target_list list;
    DSD_MEMSET(&list, 0, sizeof list);
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);
    int test_rc = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "%s should have been rejected\n", name);
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
#if LONG_MAX < 4294967295LL
    rc |= expect_parser_rejects("frequency-long-overflow", "a,p25-trunk,2400000000,,,,\n");
#endif
    rc |= expect_parser_rejects("invalid-dwell", "a,p25-trunk,851000000,,249,,\n");
    rc |= expect_parser_rejects("conventional-chan-csv", "a,dmr-conventional,461000000,chan.csv,,,\n");
    rc |= expect_parser_rejects_with_header(
        "duplicate-modulation-header",
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation,modulation\n",
        "a,p25-trunk,851000000,,,,,auto,c4fm\n");
    rc |= expect_parser_rejects_with_header(
        "invalid-modulation", "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation\n",
        "a,p25-trunk,851000000,,,,,wide\n");
    rc |= expect_parser_rejects_with_header(
        "unsupported-modulation", "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation\n",
        "a,dmr-trunk,452000000,,,,,cqpsk\n");
    rc |= expect_parser_rejects("nxdn-conventional-chan-csv", "a,nxdn-conventional,461000000,chan.csv,,,\n");
    rc |= expect_parser_rejects_with_header(
        "nxdn-unsupported-modulation", "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation\n",
        "a,nxdn-trunk,461000000,,,,,cqpsk\n");
    rc |= expect_parser_rejects("nxdn48-conventional-chan-csv", "a,nxdn48-conventional,461556250,chan.csv,,,\n");
    rc |= expect_parser_rejects_with_header(
        "nxdn48-unsupported-modulation", "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation\n",
        "a,nxdn48-conventional,461556250,,,,,c4fm\n");
    rc |= expect_parser_rejects_with_header("invalid-rtl-gain",
                                            "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,rtl_gain\n",
                                            "a,p25-trunk,851000000,,,,,50\n");
    return rc;
}

static int
test_parser_accepts_100_targets(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char body[8192];
    body[0] = '\0';
    for (int i = 0; i < 100; i++) {
        char row[128];
        DSD_SNPRINTF(row, sizeof row, "id%d,dmr-conventional,%u,,,,,%u\n", i, 461000000U + (unsigned)i, (unsigned)i);
        if (append_text(body, sizeof body, row) != 0) {
            cleanup_paths(dir, NULL, NULL);
            return 1;
        }
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,single_key_dec\n";
    if (write_targets_file_with_header(dir, header, body, target_path, sizeof target_path) != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.trunk_scan_idle_dwell_ms = 3000;
    opts.trunk_scan_activity_hold_ms = 1200;
    dsd_trunk_scan_target_list list;
    DSD_MEMSET(&list, 0, sizeof list);
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || list.count != 100 || list.targets == NULL || strcmp(list.targets[99].id, "id99") != 0
        || list.targets[99].frequency_hz != 461000099U || list.targets[99].dwell_ms != 3000
        || list.targets[99].activity_hold_ms != 1200 || list.targets[0].single_keys_present != 1U
        || list.targets[0].single_key_scalars.K != 0ULL || list.targets[99].single_keys_present != 1U
        || list.targets[99].single_key_scalars.K != 99ULL) {
        DSD_FPRINTF(stderr, "parser 100 targets rc=%d count=%zu err=%s\n", rc, list.count, err);
        test_rc = 1;
    }
    dsd_trunk_scan_target_list_reset(&list);
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
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
    dsd_state_ext_free_all(state);
    dsd_state_trunk_lcn_free(state);
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
    state->rtl_ctx = (struct RtlSdrContext*)state;
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
    state->p25_cc_prot_valid = 1;
    state->p25_cc_prot_algid = 0x84;
    state->p25_sys_time_valid = 1;
    state->p25_sys_time = 123456;
    state->p25_sys_time_offset_valid = 1;
    state->p25_sys_time_offset = -300;
    state->p25_sys_services_valid = 1;
    state->p25_sys_services_available = 0xABCDEF;
    state->p25_sys_services_supported = 0x123456;
    state->p25_sys_services_request_priority = 7;
    state->p25_site_lra_valid = 1;
    state->p25_site_lra = 0x22;
    state->p25_site_network_active_valid = 1;
    state->p25_site_network_active = 1;
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
    state->p25_nb_entries[0].wacn = 0xABCDE;
    state->p25_nb_entries[0].wacn_valid = 1;
    state->p25_nb_entries[0].sysid = 0x123;
    state->p25_nb_entries[0].rfss = 1;
    state->p25_nb_entries[0].site = 2;
    state->p25_nb_entries[0].cfva = 3;
    state->p25_nb_entries[0].lra = 0x55;
    state->p25_nb_entries[0].lra_valid = 1;
    state->p25_nb_entries[0].cfva_valid = 1;
    state->p25_nb_entries[0].last_seen = 444;
    state->p25_secondary_cc_count = 1;
    state->p25_secondary_cc_entries[0].freq = 851625000;
    state->p25_secondary_cc_entries[0].channel = 0x1012;
    state->p25_secondary_cc_entries[0].rfss = 1;
    state->p25_secondary_cc_entries[0].site = 2;
    state->p25_secondary_cc_entries[0].ssc = 0xA5;
    state->p25_secondary_cc_entries[0].last_seen = 446;
    state->p25_pending_announcement_count = 1;
    state->p25_pending_announcements[0].populated = 1;
    state->p25_pending_announcements[0].kind = P25_PENDING_ANNOUNCEMENT_NEIGHBOR;
    state->p25_pending_announcements[0].rfss = 1;
    state->p25_pending_announcements[0].site = 2;
    state->p25_pending_announcements[0].cfva = 3;
    state->p25_pending_announcements[0].lra = 0x55;
    state->p25_pending_announcements[0].wacn_valid = 1;
    state->p25_pending_announcements[0].lra_valid = 1;
    state->p25_pending_announcements[0].cfva_valid = 1;
    state->p25_pending_announcements[0].sysid = 0x123;
    state->p25_pending_announcements[0].channel = 0x300A;
    state->p25_pending_announcements[0].wacn = 0xABCDE;
    state->p25_pending_announcements[0].last_seen = 445;
    state->p25_src_nid = 0xABCDE;
}

static void
seed_target1_p25_state(dsd_state* state) {
    state->p25_prot_valid = 1;
    state->p25_prot_algid = 0x81;
    state->p25_prot_kid = 0x7777;
    state->p25_cc_prot_valid = 1;
    state->p25_cc_prot_algid = 0x81;
    state->p25_sys_time_valid = 1;
    state->p25_sys_time = 654321;
    state->p25_sys_time_offset_valid = 1;
    state->p25_sys_time_offset = 60;
    state->p25_sys_services_valid = 1;
    state->p25_sys_services_available = 0x111111;
    state->p25_sys_services_supported = 0x222222;
    state->p25_sys_services_request_priority = 3;
    state->p25_site_lra_valid = 1;
    state->p25_site_lra = 0x33;
    state->p25_site_network_active_valid = 1;
    state->p25_site_network_active = 0;
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
    state->p25_nb_entries[0].wacn = 0x77777;
    state->p25_nb_entries[0].wacn_valid = 1;
    state->p25_nb_entries[0].sysid = 0x777;
    state->p25_nb_entries[0].rfss = 7;
    state->p25_nb_entries[0].site = 8;
    state->p25_nb_entries[0].cfva = 9;
    state->p25_nb_entries[0].cfva_valid = 1;
    state->p25_nb_entries[0].last_seen = 908;
    state->p25_secondary_cc_count = 1;
    state->p25_secondary_cc_entries[0].freq = 852625000;
    state->p25_secondary_cc_entries[0].channel = 0x2002;
    state->p25_secondary_cc_entries[0].rfss = 7;
    state->p25_secondary_cc_entries[0].site = 8;
    state->p25_secondary_cc_entries[0].ssc = 0x09;
    state->p25_secondary_cc_entries[0].last_seen = 910;
    state->p25_pending_announcement_count = 1;
    state->p25_pending_announcements[0].populated = 1;
    state->p25_pending_announcements[0].kind = P25_PENDING_ANNOUNCEMENT_SECONDARY_CC;
    state->p25_pending_announcements[0].rfss = 7;
    state->p25_pending_announcements[0].site = 8;
    state->p25_pending_announcements[0].ssc = 9;
    state->p25_pending_announcements[0].channel = 0x400B;
    state->p25_pending_announcements[0].last_seen = 909;
    state->p25_src_nid = 0x77777;
}

static int
expect_empty_target_p25_state(const dsd_state* state) {
    if (state->p25_prot_valid != 0 || state->p25_cc_prot_valid != 0 || state->p25_sys_time_valid != 0
        || state->p25_sys_services_valid != 0 || state->p25_site_lra_valid != 0
        || state->p25_site_network_active_valid != 0 || state->p25_patch_count != 0 || state->p25_aff_count != 0
        || state->p25_ga_count != 0 || state->p25_nb_count != 0 || state->p25_secondary_cc_count != 0
        || state->p25_pending_announcement_count != 0 || state->p25_src_nid != 0) {
        DSD_FPRINTF(stderr, "target 0 P25 state leaked into empty target 1 snapshot\n");
        return 1;
    }
    for (int iden = 0; iden < 16; iden++) {
        if (state->p25_iden_fdma[iden].populated != 0 || state->p25_iden_tdma[iden].populated != 0
            || state->p25_chan_tdma_explicit[iden] != 0) {
            DSD_FPRINTF(stderr, "target 0 IDEN %d leaked into empty target 1 snapshot\n", iden);
            return 1;
        }
    }
    return 0;
}

static int
expect_target0_p25_state(const dsd_state* state) {
    int test_rc = 0;
    if (state->p25_prot_valid != 1 || state->p25_prot_algid != 0x80 || state->p25_prot_kid != 0x1234
        || state->p25_cc_prot_valid != 1 || state->p25_cc_prot_algid != 0x84 || state->p25_sys_time_valid != 1
        || state->p25_sys_time != 123456 || state->p25_sys_time_offset_valid != 1 || state->p25_sys_time_offset != -300
        || state->p25_sys_services_valid != 1 || state->p25_sys_services_available != 0xABCDEF
        || state->p25_sys_services_supported != 0x123456 || state->p25_sys_services_request_priority != 7
        || state->p25_site_lra_valid != 1 || state->p25_site_lra != 0x22 || state->p25_site_network_active_valid != 1
        || state->p25_site_network_active != 1) {
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
        || state->p25_nb_entries[0].wacn_valid != 1 || state->p25_nb_entries[0].wacn != 0xABCDE
        || state->p25_nb_entries[0].sysid != 0x123 || state->p25_nb_entries[0].rfss != 1
        || state->p25_nb_entries[0].site != 2 || state->p25_nb_entries[0].cfva != 3
        || state->p25_nb_entries[0].lra != 0x55 || state->p25_nb_entries[0].lra_valid != 1
        || state->p25_nb_entries[0].cfva_valid != 1 || state->p25_nb_entries[0].last_seen != 444
        || state->p25_src_nid != 0xABCDE) {
        DSD_FPRINTF(stderr, "P25 neighbor state leaked across scan targets\n");
        test_rc = 1;
    }
    if (state->p25_secondary_cc_count != 1 || state->p25_secondary_cc_entries[0].freq != 851625000
        || state->p25_secondary_cc_entries[0].channel != 0x1012 || state->p25_secondary_cc_entries[0].rfss != 1
        || state->p25_secondary_cc_entries[0].site != 2 || state->p25_secondary_cc_entries[0].ssc != 0xA5
        || state->p25_secondary_cc_entries[0].last_seen != 446) {
        DSD_FPRINTF(stderr, "P25 secondary CC state leaked across scan targets\n");
        test_rc = 1;
    }
    if (state->p25_pending_announcement_count != 1 || state->p25_pending_announcements[0].populated != 1
        || state->p25_pending_announcements[0].kind != P25_PENDING_ANNOUNCEMENT_NEIGHBOR
        || state->p25_pending_announcements[0].rfss != 1 || state->p25_pending_announcements[0].site != 2
        || state->p25_pending_announcements[0].cfva != 3 || state->p25_pending_announcements[0].lra != 0x55
        || state->p25_pending_announcements[0].wacn_valid != 1 || state->p25_pending_announcements[0].lra_valid != 1
        || state->p25_pending_announcements[0].cfva_valid != 1 || state->p25_pending_announcements[0].sysid != 0x123
        || state->p25_pending_announcements[0].channel != 0x300A || state->p25_pending_announcements[0].wacn != 0xABCDE
        || state->p25_pending_announcements[0].last_seen != 445) {
        DSD_FPRINTF(stderr, "P25 pending announcement state leaked across scan targets\n");
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.p25_iden_fdma[1].base_freq = 12345;
    dsd_state_set_trunk_chan_freq(&state, 99U, 851012500);
    state.dmr_rest_channel = 4;
    state.dmr_lcn_trust[4] = 2;
    seed_target0_p25_state(&state);

    trunk_scan_test_set_now(0.24);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "scan rotated before dwell\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "scan did not rotate after dwell\n");
        test_rc = 1;
    }
    test_rc |= expect_empty_target_p25_state(&state);

    state.p25_iden_fdma[1].base_freq = 99999;
    dsd_state_set_trunk_chan_freq(&state, 99U, 852012500);
    state.dmr_rest_channel = 8;
    state.dmr_lcn_trust[4] = 0;
    state.dmr_lcn_trust[8] = 2;
    seed_target1_p25_state(&state);

    trunk_scan_test_set_now(0.52);
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
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/* Compare the whole publication at once: an id that survives a rotation with a stale ordinal
 * beside it is as wrong as a stale id. */
static int
expect_published_target(const dsd_state* state, const char* stage, const char* id, unsigned ordinal, unsigned count) {
    if (strcmp(state->trunk_scan_active_id, id) != 0 || state->trunk_scan_active_ordinal != ordinal
        || state->trunk_scan_target_count != count) {
        DSD_FPRINTF(stderr, "published target after %s: '%s' %u of %u, want '%s' %u of %u\n", stage,
                    state->trunk_scan_active_id, (unsigned)state->trunk_scan_active_ordinal,
                    (unsigned)state->trunk_scan_target_count, id, ordinal, count);
        return 1;
    }
    return 0;
}

/*
 * The frontends label the channel being heard from the coordinator's publication in
 * dsd_state rather than by reaching into the coordinator, so the rotation has to keep
 * id/ordinal/count in step with the target actually on air, and shutdown has to take the
 * label back down rather than leave the last target's name on screen.
 */
static int
test_coordinator_publishes_active_target_label(void) {
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
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "publish scan init failed err=%s\n", err);
        test_rc = 1;
    }
    test_rc |= expect_published_target(&state, "init", "a", 1U, 2U);

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_published_target(&state, "rotation to b", "b", 2U, 2U);

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_published_target(&state, "rotation back to a", "a", 1U, 2U);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    test_rc |= expect_published_target(&state, "shutdown", "", 0U, 0U);

    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/*
 * A target's chan_csv may carry a name column, and the importer stores it. Trunk scan has
 * to drop it: the per-target snapshot carries the positional scan list but no names, so the
 * next target's list would land under the previous target's names.
 */
static int
test_target_chan_csv_names_are_discarded(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char chan_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(chan_path, sizeof chan_path, dir, "chan.csv") != 0
        || write_text_file(chan_path, "channel,frequency,name\n1,851012500,Dispatch\n") != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    if (write_targets_file(dir, "a,p25-trunk,851000000,chan.csv,250,,\n", target_path, sizeof target_path) != 0) {
        cleanup_paths(dir, NULL, chan_path);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "chan_csv name scan init failed err=%s\n", err);
        test_rc = 1;
    }
    if (state.trunk_lcn_name != NULL) {
        DSD_FPRINTF(stderr, "trunk scan kept a chan_csv name store: row 0 = '%s'\n",
                    dsd_state_trunk_lcn_name_get(&state, 0U));
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, chan_path);
    return test_rc;
}

/* A per-target scan list longer than the embedded slots lives in dsd_state's heap tail, so
 * the snapshot has to carry that tail across a rotation and hand it back intact. Nothing
 * else in the suite drives trunk_scan_snapshot_lcn_ext_reserve() or the bounded restore. */
static int
test_coordinator_preserves_long_lcn_list_across_rotation(void) {
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
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0
        || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "long lcn list scan init failed err=%s\n", err);
        dsd_engine_trunk_scan_shutdown(&opts, &state);
        trunk_scan_test_clear_now();
        cleanup_paths(dir, target_path, NULL);
        return 1;
    }

    enum { LONG_LCN_COUNT = 40 };

    if (dsd_state_trunk_lcn_reserve(&state, (size_t)LONG_LCN_COUNT) != 0) {
        DSD_FPRINTF(stderr, "could not reserve a %d-entry scan list\n", (int)LONG_LCN_COUNT);
        test_rc = 1;
    } else {
        for (int i = 0; i < LONG_LCN_COUNT; i++) {
            *dsd_state_trunk_lcn_slot(&state, i) = 851000000L + 12500L * (long)i;
        }
        state.lcn_freq_count = LONG_LCN_COUNT;
        state.lcn_freq_roll = 30;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "long lcn list scan did not rotate to target 1\n");
        test_rc = 1;
    }
    if (state.lcn_freq_count > DSD_TRUNK_LCN_EMBEDDED) {
        DSD_FPRINTF(stderr, "target 0's scan list leaked into target 1 (count=%d)\n", state.lcn_freq_count);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "long lcn list scan did not rotate back to target 0\n");
        test_rc = 1;
    }
    if (state.lcn_freq_count != LONG_LCN_COUNT || state.lcn_freq_roll != 30) {
        DSD_FPRINTF(stderr, "scan list was truncated across the rotation: count=%d roll=%d\n", state.lcn_freq_count,
                    state.lcn_freq_roll);
        test_rc = 1;
    } else {
        for (int i = 0; i < LONG_LCN_COUNT; i++) {
            const long want = 851000000L + 12500L * (long)i;
            if (*dsd_state_trunk_lcn_slot(&state, i) != want) {
                DSD_FPRINTF(stderr, "scan list slot %d restored as %ld, want %ld\n", i,
                            *dsd_state_trunk_lcn_slot(&state, i), want);
                test_rc = 1;
                break;
            }
        }
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_state_trunk_lcn_free(&state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/*
 * Write a targets CSV of @p rows generated rows. Streamed rather than formatted into a buffer:
 * a budget-sized list is far past write_targets_file()'s 8 KB body limit.
 */
static int
write_generated_targets_file(const char* dir, size_t rows, char* out_path, size_t out_sz) {
    if (dsd_test_path_join(out_path, out_sz, dir, "targets.csv") != 0) {
        return -1;
    }
    FILE* fp = dsd_fopen_private(out_path, "wb");
    if (!fp) {
        return -1;
    }
    int rc = (fputs(k_header, fp) >= 0) ? 0 : -1;
    for (size_t i = 0; rc == 0 && i < rows; i++) {
        if (DSD_FPRINTF(fp, "t%zu,p25-trunk,%lu,,250,,\n", i, 851000000UL + (unsigned long)(i * 12500U)) < 0) {
            rc = -1;
        }
    }
    if (fclose(fp) != 0) {
        rc = -1;
    }
    return rc;
}

/*
 * The target count is bounded by a memory budget rather than a fixed limit. One row past the
 * derived cap must be rejected while parsing, with an error naming the budget - not accepted into
 * an allocation that overcommit grants and the OOM killer later reclaims. Exactly the cap must
 * still load: the budget bounds the list, it does not shrink it.
 */
static int
test_targets_csv_rejects_rows_past_the_memory_budget(void) {
    const size_t max = dsd_trunk_scan_max_targets();
    if (max == 0U || max > 100000U) {
        DSD_FPRINTF(stderr, "implausible trunk scan target cap %zu\n", max);
        return 1;
    }

    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);

    int test_rc = 0;
    char err[256] = {0};

    if (write_generated_targets_file(dir, max + 1U, target_path, sizeof target_path) != 0) {
        DSD_FPRINTF(stderr, "could not write a %zu-row targets CSV\n", max + 1U);
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    dsd_trunk_scan_target_list over = {0};
    if (dsd_trunk_scan_load_targets_csv(target_path, &opts, &over, err, sizeof err) == 0) {
        DSD_FPRINTF(stderr, "a %zu-row targets CSV was accepted over a cap of %zu\n", max + 1U, max);
        test_rc = 1;
    } else if (strstr(err, "budget") == NULL) {
        DSD_FPRINTF(stderr, "budget rejection did not explain itself: %s\n", err);
        test_rc = 1;
    }
    dsd_trunk_scan_target_list_reset(&over);

    if (write_generated_targets_file(dir, max, target_path, sizeof target_path) != 0) {
        DSD_FPRINTF(stderr, "could not write a %zu-row targets CSV\n", max);
        cleanup_paths(dir, target_path, NULL);
        return 1;
    }
    err[0] = '\0';
    dsd_trunk_scan_target_list at_cap = {0};
    if (dsd_trunk_scan_load_targets_csv(target_path, &opts, &at_cap, err, sizeof err) != 0 || at_cap.count != max) {
        DSD_FPRINTF(stderr, "a targets CSV exactly at the cap (%zu) was refused: %s\n", max, err);
        test_rc = 1;
    }
    dsd_trunk_scan_target_list_reset(&at_cap);

    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/*
 * The parked-target snapshot stores the channel map sparsely, indexed through
 * state->trunk_chan_map_used[]. A map far larger than the old dense copy's practical working set
 * must survive a rotation entry for entry, and must not leak into the next target.
 */
static int
test_coordinator_preserves_large_chan_map_across_rotation(void) {
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
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0
        || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "large chan map scan init failed err=%s\n", err);
        dsd_engine_trunk_scan_shutdown(&opts, &state);
        trunk_scan_test_clear_now();
        cleanup_paths(dir, target_path, NULL);
        return 1;
    }

    /* Channels are deliberately non-contiguous and not in ascending insertion order, so the
     * snapshot cannot pass by accident: it has to carry the channel numbers, not just a run. */
    enum { CHAN_COUNT = 300 };

#define TARGET0_CHAN(i) ((uint32_t)(((CHAN_COUNT - 1 - (i)) * 7) + 3))
#define TARGET0_FREQ(i) (851000000L + 12500L * (long)(i))

    for (int i = 0; i < CHAN_COUNT; i++) {
        dsd_state_set_trunk_chan_freq(&state, TARGET0_CHAN(i), TARGET0_FREQ(i));
    }
    if (state.trunk_chan_map_used_count != (uint32_t)CHAN_COUNT) {
        DSD_FPRINTF(stderr, "seeded %u mapped channels, want %d\n", state.trunk_chan_map_used_count, (int)CHAN_COUNT);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "large chan map scan did not rotate to target 1\n");
        test_rc = 1;
    }
    if (state.trunk_chan_map_used_count != 0) {
        DSD_FPRINTF(stderr, "target 0's channel map leaked into target 1 (%u entries)\n",
                    state.trunk_chan_map_used_count);
        test_rc = 1;
    }
    for (int i = 0; i < CHAN_COUNT; i++) {
        if (state.trunk_chan_map[TARGET0_CHAN(i)] != 0) {
            DSD_FPRINTF(stderr, "channel %u still mapped on target 1\n", TARGET0_CHAN(i));
            test_rc = 1;
            break;
        }
    }

    /* Give target 1 a map of its own so the restore has stale entries to clear. */
    dsd_state_set_trunk_chan_freq(&state, 4097U, 852000000L);

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "large chan map scan did not rotate back to target 0\n");
        test_rc = 1;
    }
    if (state.trunk_chan_map_used_count != (uint32_t)CHAN_COUNT) {
        DSD_FPRINTF(stderr, "channel map restored with %u entries, want %d\n", state.trunk_chan_map_used_count,
                    (int)CHAN_COUNT);
        test_rc = 1;
    }
    if (state.trunk_chan_map[4097] != 0) {
        DSD_FPRINTF(stderr, "target 1's channel 4097 survived the rotation back to target 0\n");
        test_rc = 1;
    }
    for (int i = 0; i < CHAN_COUNT; i++) {
        if (state.trunk_chan_map[TARGET0_CHAN(i)] != TARGET0_FREQ(i)) {
            DSD_FPRINTF(stderr, "channel %u restored as %ld, want %ld\n", TARGET0_CHAN(i),
                        state.trunk_chan_map[TARGET0_CHAN(i)], TARGET0_FREQ(i));
            test_rc = 1;
            break;
        }
    }

#undef TARGET0_CHAN
#undef TARGET0_FREQ

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_coordinator_rotation_past_32_targets(void) {
    char body[8192];
    body[0] = '\0';
    for (int i = 0; i < 40; i++) {
        char row[128];
        DSD_SNPRINTF(row, sizeof row, "id%d,dmr-conventional,%u,,,,\n", i, 461000000U + (unsigned)i);
        if (append_text(body, sizeof body, row) != 0) {
            return 1;
        }
    }
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets(body, target_path, sizeof target_path, dir, sizeof dir) != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "scan init 40 targets failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }
    for (size_t i = 1; i < 40 && test_rc == 0; i++) {
        trunk_scan_test_set_now(0.25 * (double)i);
        dsd_engine_trunk_scan_tick(&opts, &state);
        if (dsd_engine_trunk_scan_active_index(&state) != i) {
            DSD_FPRINTF(stderr, "scan rotation stalled at index %zu (expected %zu)\n",
                        dsd_engine_trunk_scan_active_index(&state), i);
            test_rc = 1;
        }
    }
    if (test_rc == 0) {
        trunk_scan_test_set_now(0.25 * 40.0);
        dsd_engine_trunk_scan_tick(&opts, &state);
        if (dsd_engine_trunk_scan_active_index(&state) != 0) {
            DSD_FPRINTF(stderr, "scan did not wrap from index 39 to 0\n");
            test_rc = 1;
        }
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static void
seed_call_identity(dsd_state* state, int target0, int source0, int target1, int source1, int private0, int private1) {
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .kind = private0 ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = (uint64_t)target0,
        .policy_target_id = (uint64_t)target0,
        .ota_source_id = (uint64_t)source0,
    };
    observation.slot = 0U;
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) < 0) {
        abort();
    }
    observation.slot = 1U;
    observation.kind = private1 ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = (uint64_t)target1;
    observation.policy_target_id = (uint64_t)target1;
    observation.ota_source_id = (uint64_t)source1;
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) < 0) {
        abort();
    }
}

static int
expect_call_identity(const char* label, const dsd_state* state, int target0, int source0, int target1, int source1,
                     int private0, int private1) {
    dsd_call_snapshot calls[2];
    const int have0 = dsd_call_state_get(state, 0U, &calls[0]);
    const int have1 = dsd_call_state_get(state, 1U, &calls[1]);
    if (private0 < 0 && private1 < 0) {
        if (have0 > 0 || have1 > 0) {
            DSD_FPRINTF(stderr, "%s unexpectedly restored canonical calls\n", label);
            return 1;
        }
        return 0;
    }
    const dsd_call_kind kind0 = private0 ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE;
    const dsd_call_kind kind1 = private1 ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE;
    if (have0 <= 0 || have1 <= 0 || calls[0].phase != DSD_CALL_PHASE_ACTIVE || calls[1].phase != DSD_CALL_PHASE_ACTIVE
        || calls[0].ota_target_id != (uint64_t)target0 || calls[0].ota_source_id != (uint64_t)source0
        || calls[1].ota_target_id != (uint64_t)target1 || calls[1].ota_source_id != (uint64_t)source1
        || calls[0].kind != kind0 || calls[1].kind != kind1) {
        DSD_FPRINTF(stderr, "%s canonical call identity mismatch\n", label);
        return 1;
    }
    return 0;
}

static int
test_call_identity_state_isolated_per_target(void) {
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "call identity scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    seed_call_identity(&state, 101, 201, 102, 202, 0, 1);
    dsd_call_snapshot target0_slot0 = {0};
    if (dsd_call_state_get(&state, 0U, &target0_slot0) <= 0) {
        DSD_FPRINTF(stderr, "target 0 call epoch unavailable\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "call identity scan did not rotate to second target\n");
        test_rc = 1;
    }
    test_rc |= expect_call_identity("fresh target", &state, 0, 0, 0, 0, -1, -1);

    seed_call_identity(&state, 301, 401, 302, 402, 1, 0);
    dsd_call_snapshot target1_slot0 = {0};
    if (dsd_call_state_get(&state, 0U, &target1_slot0) <= 0 || target1_slot0.epoch <= target0_slot0.epoch) {
        DSD_FPRINTF(stderr, "call epoch reused across scan targets\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "call identity scan did not rotate back to first target\n");
        test_rc = 1;
    }
    test_rc |= expect_call_identity("restored target", &state, 101, 201, 102, 202, 0, 1);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_call_event_lifecycle_isolated_per_target(void) {
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
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 101U,
        .policy_target_id = 101U,
        .ota_source_id = 201U,
    };
    dsd_call_context_snapshot context = {0};
    uint64_t ended_epoch = 0U;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "call lifecycle scan init failed: %s\n", err);
        test_rc = 1;
        goto cleanup;
    }

    if (dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) < 0
        || dsd_call_state_end(&state, 0U, 0.1) <= 0) {
        DSD_FPRINTF(stderr, "failed to stage ended call lifecycle for scan target\n");
        test_rc = 1;
        goto cleanup;
    }
    if (dsd_call_context_copy_snapshot(&state, &context) <= 0) {
        DSD_FPRINTF(stderr, "failed to copy staged call lifecycle\n");
        test_rc = 1;
        goto cleanup;
    }
    ended_epoch = context.calls.slots[0].epoch;
    context.events[0].epoch = ended_epoch;
    context.events[0].ended_committed = 1U;
    if (dsd_call_context_restore_snapshot(&state, &context) <= 0) {
        DSD_FPRINTF(stderr, "failed to install staged call lifecycle\n");
        test_rc = 1;
        goto cleanup;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1U || dsd_call_context_copy_snapshot(&state, &context) <= 0
        || context.events[0].ended_committed != 0U) {
        DSD_FPRINTF(stderr, "fresh scan target inherited another target's call lifecycle\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0U || dsd_call_context_copy_snapshot(&state, &context) <= 0
        || context.calls.slots[0].phase != DSD_CALL_PHASE_ENDED || context.calls.slots[0].epoch != ended_epoch
        || context.events[0].epoch != ended_epoch || context.events[0].ended_committed != 1U) {
        DSD_FPRINTF(stderr, "scan target did not restore its committed call lifecycle\n");
        test_rc = 1;
    }

cleanup:
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_call_event_current_rows_isolated_per_target(void) {
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
    static Event_History_I event_history[DSD_CALL_STATE_SLOT_COUNT];
    reset_scan_opts_state(&opts, &state);
    DSD_MEMSET(event_history, 0, sizeof(event_history));
    state.event_history_s = event_history;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "event row scan init failed: %s\n", err);
        test_rc = 1;
        goto cleanup;
    }

    Event_History* current = &event_history[0].Event_History_Items[0];
    current->target_id = 101U;
    current->source_id = 201U;
    DSD_SNPRINTF(current->event_string, sizeof current->event_string, "%s", "target-a-current");

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1U
        || event_history[0].Event_History_Items[0].event_string[0] != '\0') {
        DSD_FPRINTF(stderr, "fresh scan target inherited another target's current event row\n");
        test_rc = 1;
    }

    current = &event_history[0].Event_History_Items[0];
    current->target_id = 301U;
    current->source_id = 401U;
    DSD_SNPRINTF(current->event_string, sizeof current->event_string, "%s", "target-b-current");
    DSD_SNPRINTF(event_history[0].Event_History_Items[1].event_string,
                 sizeof event_history[0].Event_History_Items[1].event_string, "%s", "shared-committed");

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0U
        || strcmp(event_history[0].Event_History_Items[0].event_string, "target-a-current") != 0
        || event_history[0].Event_History_Items[0].target_id != 101U
        || strcmp(event_history[0].Event_History_Items[1].event_string, "shared-committed") != 0) {
        DSD_FPRINTF(stderr, "scan target did not restore its current event row without changing history\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.78);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1U
        || strcmp(event_history[0].Event_History_Items[0].event_string, "target-b-current") != 0
        || event_history[0].Event_History_Items[0].target_id != 301U
        || strcmp(event_history[0].Event_History_Items[1].event_string, "shared-committed") != 0) {
        DSD_FPRINTF(stderr, "scan target lost its saved current event row across context switches\n");
        test_rc = 1;
    }

cleanup:
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static void
seed_dmr_identity(dsd_state* state, int mfid, unsigned int syscode, const char* branding, const char* branding_sub,
                  const char* site_parms) {
    state->dmr_mfid = mfid;
    state->dmr_t3_syscode = syscode;
    DSD_SNPRINTF(state->dmr_branding, sizeof state->dmr_branding, "%s", branding);
    DSD_SNPRINTF(state->dmr_branding_sub, sizeof state->dmr_branding_sub, "%s", branding_sub);
    DSD_SNPRINTF(state->dmr_site_parms, sizeof state->dmr_site_parms, "%s", site_parms);
}

static int
expect_dmr_identity(const char* label, const dsd_state* state, int mfid, const char* branding, const char* branding_sub,
                    const char* site_parms, unsigned int syscode) {
    if (state->dmr_mfid != mfid || state->dmr_t3_syscode != syscode || strcmp(state->dmr_branding, branding) != 0
        || strcmp(state->dmr_branding_sub, branding_sub) != 0 || strcmp(state->dmr_site_parms, site_parms) != 0) {
        DSD_FPRINTF(stderr, "%s DMR identity mismatch mfid=%d syscode=%u branding='%s' sub='%s' site='%s'\n", label,
                    state->dmr_mfid, state->dmr_t3_syscode, state->dmr_branding, state->dmr_branding_sub,
                    state->dmr_site_parms);
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr identity scan init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    seed_dmr_identity(&state, 0x10, 0x123U, "Motorola", "Cap+ ", "cap-site ");
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "dmr identity scan did not rotate to second target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_identity("fresh target", &state, -1, "", "", "", 0U);

    seed_dmr_identity(&state, 0x68, 0x456U, "  Hytera", "XPT ", "xpt-site ");
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr identity scan did not rotate back to first target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_identity("restored target", &state, 0x10, "Motorola", "Cap+ ", "cap-site ", 0x123U);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr confidence scan init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    seed_dmr_confidence(&state, 3);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "dmr confidence scan did not rotate to second target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_confidence("fresh target", &state, 16, 0);

    seed_dmr_confidence(&state, 7);
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr confidence scan did not rotate back to first target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_confidence("restored target", &state, 3, 1);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static void
seed_dmr_service_options(dsd_state* state, unsigned int fid, unsigned int so, unsigned int fidR, unsigned int soR) {
    state->dmr_fid = fid;
    state->dmr_so = so;
    state->dmr_fidR = fidR;
    state->dmr_soR = soR;
}

static int
expect_dmr_service_options(const char* label, const dsd_state* state, unsigned int fid, unsigned int so,
                           unsigned int fidR, unsigned int soR) {
    if (state->dmr_fid != fid || state->dmr_so != so || state->dmr_fidR != fidR || state->dmr_soR != soR) {
        DSD_FPRINTF(stderr, "%s DMR service options mismatch fid=%u so=%u fidR=%u soR=%u\n", label, state->dmr_fid,
                    state->dmr_so, state->dmr_fidR, state->dmr_soR);
        return 1;
    }
    return 0;
}

static int
test_dmr_service_options_state_isolated_per_target(void) {
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr service option scan init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    seed_dmr_service_options(&state, 0x10U, 0x40U, 0x68U, 0x41U);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "dmr service option scan did not rotate to second target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_service_options("fresh target", &state, 0U, 0U, 0U, 0U);

    seed_dmr_service_options(&state, 0x22U, 0x02U, 0x33U, 0x03U);
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr service option scan did not rotate back to first target\n");
        test_rc = 1;
    }
    test_rc |= expect_dmr_service_options("restored target", &state, 0x10U, 0x40U, 0x68U, 0x41U);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "p25 timing scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }
    if (state.samplesPerSymbol != 10 || state.symbolCenter != 4 || state.rf_mod != 0) {
        DSD_FPRINTF(stderr, "initial P25 scan target demod invalid sps=%d center=%d rf_mod=%d\n",
                    state.samplesPerSymbol, state.symbolCenter, state.rf_mod);
        test_rc = 1;
    }
    state.p25_cc_is_tdma = 1;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    state.rf_mod = 1;

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.samplesPerSymbol != 10 || state.symbolCenter != 4
        || state.rf_mod != 0) {
        DSD_FPRINTF(stderr, "second P25 scan target demod invalid active=%zu sps=%d center=%d rf_mod=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.samplesPerSymbol, state.symbolCenter,
                    state.rf_mod);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.samplesPerSymbol != 8 || state.symbolCenter != 3
        || state.rf_mod != 1) {
        DSD_FPRINTF(stderr, "restored P25 TDMA scan target demod invalid active=%zu sps=%d center=%d rf_mod=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.samplesPerSymbol, state.symbolCenter,
                    state.rf_mod);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_p25_nac_state_isolated_per_target(void) {
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "p25 NAC scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.nac = 0x2A1;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.nac != 0) {
        DSD_FPRINTF(stderr, "fresh P25 scan target inherited stale NAC active=%zu nac=0x%03X\n",
                    dsd_engine_trunk_scan_active_index(&state), state.nac);
        test_rc = 1;
    }

    state.nac = 0x345;
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.nac != 0x2A1) {
        DSD_FPRINTF(stderr, "P25 scan target did not restore its own NAC active=%zu nac=0x%03X\n",
                    dsd_engine_trunk_scan_active_index(&state), state.nac);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_p25_target_switch_resyncs_sm_mode(void) {
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "p25 SM mode scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    p25_sm_ctx_t* first_ctx = (p25_sm_ctx_t*)dsd_engine_trunk_scan_active_p25_ctx();
    if (!first_ctx) {
        DSD_FPRINTF(stderr, "missing active P25 context for first scan target\n");
        test_rc = 1;
    } else {
        first_ctx->state = P25_SM_HUNTING;
        state.p25_sm_mode = DSD_P25_SM_MODE_UNKNOWN;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.p25_sm_mode != DSD_P25_SM_MODE_ON_CC) {
        DSD_FPRINTF(stderr, "second P25 scan target SM mode invalid active=%zu mode=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.p25_sm_mode);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.p25_sm_mode != DSD_P25_SM_MODE_ON_CC) {
        DSD_FPRINTF(stderr, "retuned P25 scan target SM mode invalid active=%zu mode=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.p25_sm_mode);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_p25_scan_retune_restarts_pending_cc_acquisition(void) {
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
    trunk_scan_test_set_now(1.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    p25_sm_ctx_t* first_ctx = (p25_sm_ctx_t*)dsd_engine_trunk_scan_active_p25_ctx();
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || !first_ctx) {
        DSD_FPRINTF(stderr, "pending CC timer scan init failed rc=%d active=%zu ctx=%p err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), (void*)first_ctx, err);
        test_rc = 1;
    } else {
        first_ctx->state = P25_SM_HUNTING;
        first_ctx->cc_sync_pending = 1;
        first_ctx->t_cc_sync_m = 1.0;
        first_ctx->t_cc_tune_m = 1.0;
        first_ctx->t_hunt_try_m = 0.5;
        state.p25_sm_mode = DSD_P25_SM_MODE_HUNTING;
        state.p25_cc_freq = 853000000;
        state.trunk_cc_freq = 853000000;
        state.p25_cc_eval_freq = 853000000;
        state.p25_cc_eval_start_m = 1.0;
        state.p25_last_cc_msg_time_m = 0.75;
    }

    trunk_scan_test_set_now(1.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "pending CC timer scan did not rotate away from first target\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(4.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    p25_sm_ctx_t* restored_ctx = (p25_sm_ctx_t*)dsd_engine_trunk_scan_active_p25_ctx();
    const double timestamp_epsilon_s = 1.0e-9;
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || !restored_ctx || restored_ctx->cc_sync_pending != 1
        || restored_ctx->state != P25_SM_ON_CC || restored_ctx->t_cc_sync_m <= 1.0
        || restored_ctx->cc_acquisition_origin != P25_SM_CC_ACQUISITION_RETURN
        || !(fabs(restored_ctx->t_cc_tune_m - restored_ctx->t_cc_sync_m) <= timestamp_epsilon_s)
        || restored_ctx->t_hunt_try_m != 0.0 || state.p25_sm_mode != DSD_P25_SM_MODE_ON_CC
        || state.p25_cc_freq != 853000000 || state.trunk_cc_freq != 853000000 || state.p25_cc_eval_freq != 853000000
        || !(fabs(state.p25_cc_eval_start_m - restored_ctx->t_cc_tune_m) <= timestamp_epsilon_s)
        || state.p25_last_cc_msg_time_m != 0.75) {
        DSD_FPRINTF(stderr,
                    "pending CC timer did not restart after retune active=%zu ctx=%p state=%d pending=%d sync=%.3f "
                    "tune=%.3f hunt=%.3f mode=%d cc=%ld trunk_cc=%ld eval_freq=%ld eval_start=%.3f decoded=%.3f\n",
                    dsd_engine_trunk_scan_active_index(&state), (void*)restored_ctx,
                    restored_ctx ? (int)restored_ctx->state : -1, restored_ctx ? restored_ctx->cc_sync_pending : -1,
                    restored_ctx ? restored_ctx->t_cc_sync_m : -1.0, restored_ctx ? restored_ctx->t_cc_tune_m : -1.0,
                    restored_ctx ? restored_ctx->t_hunt_try_m : -1.0, state.p25_sm_mode, state.p25_cc_freq,
                    state.trunk_cc_freq, state.p25_cc_eval_freq, state.p25_cc_eval_start_m,
                    state.p25_last_cc_msg_time_m);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "mixed demod scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.rf_mod = 1;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    trunk_scan_test_set_now(0.26);
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
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 2 || state.rf_mod != 2 || state.samplesPerSymbol != 10
        || state.symbolCenter != 4) {
        DSD_FPRINTF(stderr, "DMR conventional target inherited P25 demod state active=%zu rf_mod=%d sps=%d center=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod, state.samplesPerSymbol,
                    state.symbolCenter);
        test_rc = 1;
    }

    state.rf_mod = 2;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    trunk_scan_test_set_now(0.78);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.rf_mod != 0 || state.samplesPerSymbol != 10
        || state.symbolCenter != 4) {
        DSD_FPRINTF(stderr, "P25 target inherited DMR demod state active=%zu rf_mod=%d sps=%d center=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod, state.samplesPerSymbol,
                    state.symbolCenter);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "conventional scan init failed: %s\n", err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_dmr_conventional_activity(&opts, &state, 1001, 2002, 0, 0, 0);
    trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "allowed conventional activity did not hold target\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.61);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(0.87);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "target did not rotate after conventional hold and dwell\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);

    reset_scan_opts_state(&opts, &state);
    opts.trunk_use_allow_list = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    trunk_scan_test_set_now(0.0);
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "allowlist scan init failed: %s\n", err);
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_dmr_conventional_activity(&opts, &state, 1001, 2002, 0, 0, 0);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "blocked allow-list traffic held conventional target\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "encrypted lockout scan init failed: %s\n", err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_dmr_conventional_activity(&opts, &state, 1001, 2002, 0, 1, 0);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "encrypted conventional traffic held target despite lockout\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static void
seed_nxdn_identity(dsd_state* state, uint16_t grant_chan, long int grant_freq, unsigned int ran, uint32_t sys_code,
                   uint16_t site_code, const char* category, uint8_t rcn, uint8_t base_freq, uint8_t step, uint8_t bw) {
    state->nxdn_grant_chan = grant_chan;
    state->nxdn_grant_freq = grant_freq;
    state->nxdn_last_ran = ran;
    state->nxdn_location_sys_code = sys_code;
    state->nxdn_location_site_code = site_code;
    DSD_SNPRINTF(state->nxdn_location_category, sizeof state->nxdn_location_category, "%s", category);
    state->nxdn_rcn = rcn;
    state->nxdn_base_freq = base_freq;
    state->nxdn_step = step;
    state->nxdn_bw = bw;
}

static int
expect_nxdn_identity(const char* label, const dsd_state* state, uint16_t grant_chan, long int grant_freq,
                     unsigned int ran, uint32_t sys_code, uint16_t site_code, const char* category, uint8_t rcn,
                     uint8_t base_freq, uint8_t step, uint8_t bw) {
    if (state->nxdn_grant_chan != grant_chan || state->nxdn_grant_freq != grant_freq || state->nxdn_last_ran != ran
        || state->nxdn_location_sys_code != sys_code || state->nxdn_location_site_code != site_code
        || strcmp(state->nxdn_location_category, category) != 0 || state->nxdn_rcn != rcn
        || state->nxdn_base_freq != base_freq || state->nxdn_step != step || state->nxdn_bw != bw) {
        DSD_FPRINTF(stderr,
                    "%s NXDN identity mismatch chan=%u freq=%ld ran=%u sys=%u site=%u cat='%s' rcn=%u "
                    "base=%u step=%u bw=%u\n",
                    label, state->nxdn_grant_chan, state->nxdn_grant_freq, state->nxdn_last_ran,
                    state->nxdn_location_sys_code, state->nxdn_location_site_code, state->nxdn_location_category,
                    state->nxdn_rcn, state->nxdn_base_freq, state->nxdn_step, state->nxdn_bw);
        return 1;
    }
    return 0;
}

static int
test_nxdn_trunk_target_seeds_control_channel(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("n,nxdn-trunk,461000000,,250,,\n", target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "nxdn seed scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }
    if (test_rc == 0
        && (state.p25_cc_freq != 461000000 || state.trunk_cc_freq != 461000000 || state.lcn_freq_count < 1
            || state.trunk_lcn_freq[0] != 461000000)) {
        DSD_FPRINTF(stderr, "nxdn trunk seed invalid cc=%ld trunk_cc=%ld lcn_count=%d lcn0=%ld\n", state.p25_cc_freq,
                    state.trunk_cc_freq, state.lcn_freq_count, state.trunk_lcn_freq[0]);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_mixed_target_switch_resets_nxdn_demod_profile(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("p25,p25-trunk,851000000,,250,,\n"
                             "nxdn,nxdn-trunk,461000000,,250,,\n"
                             "conv,nxdn-conventional,462000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "nxdn demod scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.rf_mod = 1;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.rf_mod != 2 || state.samplesPerSymbol != 10
        || state.symbolCenter != 4) {
        DSD_FPRINTF(stderr, "NXDN trunk target inherited P25 demod state active=%zu rf_mod=%d sps=%d center=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod, state.samplesPerSymbol,
                    state.symbolCenter);
        test_rc = 1;
    }

    state.rf_mod = 1;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 2 || state.rf_mod != 2 || state.samplesPerSymbol != 10
        || state.symbolCenter != 4) {
        DSD_FPRINTF(
            stderr, "NXDN conventional target inherited P25 demod state active=%zu rf_mod=%d sps=%d center=%d\n",
            dsd_engine_trunk_scan_active_index(&state), state.rf_mod, state.samplesPerSymbol, state.symbolCenter);
        test_rc = 1;
    }

    state.rf_mod = 2;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    trunk_scan_test_set_now(0.78);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.rf_mod != 0 || state.samplesPerSymbol != 10
        || state.symbolCenter != 4) {
        DSD_FPRINTF(stderr, "P25 target inherited NXDN demod state active=%zu rf_mod=%d sps=%d center=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod, state.samplesPerSymbol,
                    state.symbolCenter);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_nxdn48_target_selects_2400_demod_profile(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("n96,nxdn-conventional,461000000,,250,,\n"
                             "n48,nxdn48-conventional,461556250,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    g_scan_tune_to_freq_ted_sps = 0;
    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "nxdn48 demod scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }
    if (test_rc == 0
        && (state.sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4 || state.samplesPerSymbol != 10
            || state.symbolCenter != 4 || state.rf_mod != 2 || g_scan_tune_to_freq_ted_sps != 10)) {
        DSD_FPRINTF(stderr, "nxdn96 target profile wrong idx=%d sps=%d center=%d rf_mod=%d ted=%d\n",
                    state.sps_hunt_idx, state.samplesPerSymbol, state.symbolCenter, state.rf_mod,
                    g_scan_tune_to_freq_ted_sps);
        test_rc = 1;
    }

    /* Poison every axis the coordinator owns: parking an NXDN48 target must re-seed all of them. */
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.sps_hunt_counter = 17;
    state.samplesPerSymbol = 8;
    state.symbolCenter = 3;
    state.rf_mod = 0;
    g_scan_tune_to_freq_ted_sps = 0;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_2400_4
        || state.sps_hunt_counter != 0 || state.samplesPerSymbol != 20 || state.symbolCenter != 9 || state.rf_mod != 2
        || g_scan_tune_to_freq_ted_sps != 20) {
        DSD_FPRINTF(stderr,
                    "nxdn48 target profile wrong active=%zu idx=%d counter=%d sps=%d center=%d rf_mod=%d ted=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.sps_hunt_idx, state.sps_hunt_counter,
                    state.samplesPerSymbol, state.symbolCenter, state.rf_mod, g_scan_tune_to_freq_ted_sps);
        test_rc = 1;
    }

    /* Rotating back must not leak the 2400 timing into the NXDN96 target. */
    g_scan_tune_to_freq_ted_sps = 0;
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4
        || state.samplesPerSymbol != 10 || state.symbolCenter != 4 || state.rf_mod != 2
        || g_scan_tune_to_freq_ted_sps != 10) {
        DSD_FPRINTF(stderr, "nxdn96 target inherited 2400 timing active=%zu idx=%d sps=%d center=%d ted=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.sps_hunt_idx, state.samplesPerSymbol,
                    state.symbolCenter, g_scan_tune_to_freq_ted_sps);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_nxdn48_target_uses_rtl_output_rate_for_sps(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("n48,nxdn48-conventional,461556250,,250,,\n"
                             "n96,nxdn-conventional,461000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    dsd_rtl_stream_metrics_hooks metrics_hooks = {0};
    metrics_hooks.output_rate_hz = fake_rtl_output_rate_hz;
    g_fake_rtl_output_rate_hz = 24000U;
    dsd_rtl_stream_metrics_hooks_set(&metrics_hooks);

    g_scan_tune_to_freq_ted_sps = 0;
    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    /* 24000 / 2400 = 10, the same sps NXDN96 gets at 48 kHz -- so only the hunt profile separates them. */
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0
        || state.sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_2400_4 || state.samplesPerSymbol != 10
        || state.symbolCenter != 4 || g_scan_tune_to_freq_ted_sps != 10) {
        DSD_FPRINTF(stderr, "nxdn48 target ignored RTL output rate rc=%d idx=%d sps=%d center=%d ted=%d err=%s\n", rc,
                    state.sps_hunt_idx, state.samplesPerSymbol, state.symbolCenter, g_scan_tune_to_freq_ted_sps, err);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/* One init cycle with the given NXDN frame flags; returns the captured stderr in `buf`. */
static int
run_nxdn_decoder_warning_case(const char* label, const char* target_path, int frame_nxdn48, int frame_nxdn96, char* buf,
                              size_t buf_sz) {
    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    opts.frame_nxdn48 = frame_nxdn48;
    opts.frame_nxdn96 = frame_nxdn96;

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "trunkscan48warn") != 0) {
        DSD_FPRINTF(stderr, "%s: stderr capture failed\n", label);
        return 1;
    }
    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc == 0 && (opts.frame_nxdn48 != 1 || opts.frame_nxdn96 != 0 || opts.frame_dmr != 0)) {
        rc = -1;
    }
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (rc == 0 && (opts.frame_nxdn48 != 0 || opts.frame_nxdn96 != 1 || opts.frame_p25p1 != 0)) {
        rc = -1;
    }
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    if (opts.frame_nxdn48 != frame_nxdn48 || opts.frame_nxdn96 != frame_nxdn96) {
        rc = -1;
    }
    trunk_scan_test_clear_now();
    (void)dsd_test_capture_stderr_end(&cap);
    if (dsd_test_capture_stderr_read(&cap, buf, buf_sz) != 0) {
        DSD_FPRINTF(stderr, "%s: stderr read failed\n", label);
        return 1;
    }
    if (rc != 0) {
        DSD_FPRINTF(stderr, "%s: scan init failed rc=%d err=%s\n", label, rc, err);
        return 1;
    }
    return 0;
}

static int
test_target_classes_enable_missing_decoders(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("n48,nxdn48-conventional,461556250,,250,,\n"
                             "n96,nxdn-conventional,461000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    int test_rc = 0;
    char buf[4096];

    /* Either global NXDN variant may be disabled: the target class enables its decoder. */
    for (int n48 = 0; n48 <= 1; n48++) {
        if (run_nxdn_decoder_warning_case("target-class", target_path, n48, !n48, buf, sizeof buf) != 0
            || strstr(buf, "have no enabled") != NULL) {
            DSD_FPRINTF(stderr, "target class must not require global decoder flags: %s\n", buf);
            test_rc = 1;
        }
    }

    /* Both enabled (-fa): neither variant warns. */
    if (run_nxdn_decoder_warning_case("both-enabled", target_path, 1, 1, buf, sizeof buf) != 0) {
        test_rc = 1;
    } else if (strstr(buf, "NXDN48 decoder") != NULL || strstr(buf, "NXDN96 decoder") != NULL) {
        DSD_FPRINTF(stderr, "both-enabled warned anyway:\n%s\n", buf);
        test_rc = 1;
    }

    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_nxdn_conventional_activity_hold(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,nxdn-conventional,461000000,,250,250,\n"
                             "b,nxdn-conventional,462000000,,250,250,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "nxdn conventional scan init failed: %s\n", err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_nxdn_conventional_activity(&opts, &state, 1001, 2002, 0, 0, 0);
    trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "allowed NXDN conventional activity did not hold target\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.61);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(0.87);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "NXDN target did not rotate after conventional hold and dwell\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);

    reset_scan_opts_state(&opts, &state);
    opts.trunk_use_allow_list = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    trunk_scan_test_set_now(0.0);
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "nxdn allowlist scan init failed: %s\n", err);
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_nxdn_conventional_activity(&opts, &state, 1001, 2002, 0, 0, 0);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "blocked allow-list traffic held NXDN conventional target\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);

    reset_scan_opts_state(&opts, &state);
    opts.trunk_tune_enc_calls = 0;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    trunk_scan_test_set_now(0.0);
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "nxdn encrypted lockout scan init failed: %s\n", err);
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_nxdn_conventional_activity(&opts, &state, 1001, 2002, 1, 1, 0);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "encrypted NXDN conventional traffic held target despite lockout\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/*
 * Data traffic holds a conventional target exactly as far as data-call tuning allows: reported
 * activity is run through the same talkgroup policy as voice, and --trunk-tune-data-calls is off
 * by default, so a data header holds the park only when the operator asked to follow data.
 */
static int
run_conventional_data_call_hold_case(const char* tag, const char* body, int tune_data_calls, size_t expect_active,
                                     void (*report)(const dsd_opts*, const dsd_state*, uint32_t, uint32_t, int, int,
                                                    int)) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets(body, target_path, sizeof target_path, dir, sizeof dir) != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.trunk_tune_data_calls = tune_data_calls;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "%s: data-call scan init failed: %s\n", tag, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.10);
    report(&opts, &state, 1001, 2002, 0, 0, 1);
    trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != expect_active) {
        DSD_FPRINTF(stderr, "%s: data-call activity left active=%zu want %zu\n", tag,
                    dsd_engine_trunk_scan_active_index(&state), expect_active);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_conventional_activity_data_call_respects_tune_data_calls(void) {
    static const char nxdn_body[] = "a,nxdn-conventional,461000000,,250,250,\n"
                                    "b,nxdn-conventional,462000000,,250,250,\n";
    static const char dmr_body[] = "a,dmr-conventional,461000000,,250,250,\n"
                                   "b,dmr-conventional,462000000,,250,250,\n";
    int rc = 0;

    rc |= run_conventional_data_call_hold_case("nxdn-data-followed", nxdn_body, 1, 0,
                                               dsd_engine_trunk_scan_nxdn_conventional_activity);
    rc |= run_conventional_data_call_hold_case("nxdn-data-not-followed", nxdn_body, 0, 1,
                                               dsd_engine_trunk_scan_nxdn_conventional_activity);
    rc |= run_conventional_data_call_hold_case("dmr-data-followed", dmr_body, 1, 0,
                                               dsd_engine_trunk_scan_dmr_conventional_activity);
    rc |= run_conventional_data_call_hold_case("dmr-data-not-followed", dmr_body, 0, 1,
                                               dsd_engine_trunk_scan_dmr_conventional_activity);

    /* The NXDN entry point serves both conventional variants: nxdn_element.c reports VCALL/DCALL
       identity without knowing whether the site is 6.25 or 12.5 kHz. */
    static const char nxdn48_body[] = "a,nxdn48-conventional,461556250,,250,250,\n"
                                      "b,nxdn48-conventional,462556250,,250,250,\n";
    rc |= run_conventional_data_call_hold_case("nxdn48-data-followed", nxdn48_body, 1, 0,
                                               dsd_engine_trunk_scan_nxdn_conventional_activity);
    rc |= run_conventional_data_call_hold_case("nxdn48-data-not-followed", nxdn48_body, 0, 1,
                                               dsd_engine_trunk_scan_nxdn_conventional_activity);
    return rc;
}

/*
 * The NXDN entry point holds either NXDN conventional variant, and neither conventional family
 * may claim the other's park: a DMR voice header decoded while an NXDN48 target is parked says
 * nothing about that target's channel.
 */
static int
test_conventional_activity_families_do_not_cross(void) {
    static const char nxdn48_first[] = "n48,nxdn48-conventional,461556250,,250,250,\n"
                                       "dmrc,dmr-conventional,461112500,,250,250,\n";
    static const char dmr_first[] = "dmrc,dmr-conventional,461112500,,250,250,\n"
                                    "n48,nxdn48-conventional,461556250,,250,250,\n";
    int rc = 0;

    /* NXDN hook holds a parked nxdn48-conventional target (data-call tuning on to reuse the helper). */
    rc |= run_conventional_data_call_hold_case("nxdn-hook-holds-nxdn48", nxdn48_first, 1, 0,
                                               dsd_engine_trunk_scan_nxdn_conventional_activity);
    /* DMR hook must not: the target rotates away despite the reported activity. */
    rc |= run_conventional_data_call_hold_case("dmr-hook-skips-nxdn48", nxdn48_first, 1, 1,
                                               dsd_engine_trunk_scan_dmr_conventional_activity);
    /* And the NXDN hook must not claim a parked dmr-conventional target. */
    rc |= run_conventional_data_call_hold_case("nxdn-hook-skips-dmr", dmr_first, 1, 1,
                                               dsd_engine_trunk_scan_nxdn_conventional_activity);
    return rc;
}

static int
test_nxdn48_conventional_activity_hold(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,nxdn48-conventional,461556250,,250,250,\n"
                             "b,nxdn48-conventional,462556250,,250,250,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "nxdn48 conventional scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_nxdn_conventional_activity(&opts, &state, 1001, 2002, 0, 0, 0);
    trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "NXDN48 conventional activity did not hold the target\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.61);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(0.87);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "NXDN48 target did not rotate after conventional hold and dwell\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);

    reset_scan_opts_state(&opts, &state);
    opts.trunk_use_allow_list = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    trunk_scan_test_set_now(0.0);
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "nxdn48 allowlist scan init failed: %s\n", err);
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_nxdn_conventional_activity(&opts, &state, 1001, 2002, 0, 0, 0);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "blocked allow-list traffic held NXDN48 conventional target\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);

    reset_scan_opts_state(&opts, &state);
    opts.trunk_tune_enc_calls = 0;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    trunk_scan_test_set_now(0.0);
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "nxdn48 encrypted lockout scan init failed: %s\n", err);
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_nxdn_conventional_activity(&opts, &state, 1001, 2002, 1, 1, 0);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "encrypted NXDN48 conventional traffic held target despite lockout\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/*
 * Voice-gated scan (issue #381): seed an ACTIVE media-active voice epoch on slot 0 with
 * explicit media times, so the gate's probe sees decoded voice without running a decoder.
 * The span must reach DSD_SCAN_VOICE_MIN_SPAN_S (0.10 s) before the probe counts it.
 */
static int
seed_voice_gate_media_epoch(dsd_state* state, double begin_m, double media_m, dsd_call_crypto_state crypto) {
    if (dsd_call_state_ensure(state) <= 0) {
        DSD_FPRINTF(stderr, "voice-gate seed: call-state ensure failed\n");
        return -1;
    }
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof(obs));
    obs.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    obs.slot = 0U;
    obs.kind = DSD_CALL_KIND_GROUP_VOICE;
    obs.ota_target_id = 1001U;
    obs.policy_target_id = 1001U;
    obs.ota_source_id = 2002U;
    obs.observed_m = begin_m;
    if (dsd_call_state_observe(state, &obs, DSD_CALL_BOUNDARY_BEGIN) != 1) {
        DSD_FPRINTF(stderr, "voice-gate seed: observe failed\n");
        return -1;
    }
    if (crypto != DSD_CALL_CRYPTO_CLEAR && crypto != DSD_CALL_CRYPTO_UNKNOWN) {
        dsd_call_crypto_update update;
        DSD_MEMSET(&update, 0, sizeof(update));
        update.classification = crypto;
        update.observed_m = begin_m;
        (void)dsd_call_state_update_crypto(state, 0U, &update);
    }
    if (dsd_call_state_update_media(state, 0U, 1, begin_m) != 1
        || dsd_call_state_update_media(state, 0U, 1, media_m) != 1) {
        DSD_FPRINTF(stderr, "voice-gate seed: media update failed\n");
        return -1;
    }
    return 0;
}

/* With the gate on, a header alone never holds a conventional target: not a data header
 * even when data-call tuning is on, and not a policy-allowed voice header either. Only
 * decoded voice media refreshes the hold. One case per conventional family, each through
 * its own activity entry point. */
static int
run_voice_gate_header_case(const char* tag, const char* body, int data_call,
                           void (*report)(const dsd_opts*, const dsd_state*, uint32_t, uint32_t, int, int, int)) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets(body, target_path, sizeof target_path, dir, sizeof dir) != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.scan_voice_only = 1;
    opts.scan_voice_qualify_ms = 1000;
    opts.scan_voice_hold_ms = 2000;
    opts.trunk_tune_data_calls = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "%s: voice-gate data scan init failed: %s\n", tag, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.10);
    report(&opts, &state, 1001, 2002, 0, 0, data_call);
    trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "%s: %s header held conventional target with gate on\n", tag, data_call ? "data" : "voice");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_conventional_voice_gate_data_header_no_hold(void) {
    static const char dmr_body[] = "a,dmr-conventional,461000000,,250,250,\n"
                                   "b,dmr-conventional,462000000,,250,250,\n";
    static const char nxdn_body[] = "a,nxdn-conventional,461000000,,250,250,\n"
                                    "b,nxdn-conventional,462000000,,250,250,\n";
    static const char nxdn48_body[] = "a,nxdn48-conventional,461556250,,250,250,\n"
                                      "b,nxdn48-conventional,462556250,,250,250,\n";
    int rc = 0;
    rc |= run_voice_gate_header_case("dmr-data-gate-on", dmr_body, 1, dsd_engine_trunk_scan_dmr_conventional_activity);
    rc |=
        run_voice_gate_header_case("nxdn-data-gate-on", nxdn_body, 1, dsd_engine_trunk_scan_nxdn_conventional_activity);
    rc |= run_voice_gate_header_case("nxdn48-data-gate-on", nxdn48_body, 1,
                                     dsd_engine_trunk_scan_nxdn_conventional_activity);
    return rc;
}

/* A policy-allowed voice header (DMR voice LC, NXDN VCALL) with no decoded voice media
 * behind it must not hold either: that is exactly the header-only carrier the gate exists
 * to skip, and holding on it would publish TAIL with no voice ever decoded. */
static int
test_conventional_voice_gate_voice_header_no_hold(void) {
    static const char dmr_body[] = "a,dmr-conventional,461000000,,250,250,\n"
                                   "b,dmr-conventional,462000000,,250,250,\n";
    static const char nxdn_body[] = "a,nxdn-conventional,461000000,,250,250,\n"
                                    "b,nxdn-conventional,462000000,,250,250,\n";
    static const char nxdn48_body[] = "a,nxdn48-conventional,461556250,,250,250,\n"
                                      "b,nxdn48-conventional,462556250,,250,250,\n";
    int rc = 0;
    rc |= run_voice_gate_header_case("dmr-voice-gate-on", dmr_body, 0, dsd_engine_trunk_scan_dmr_conventional_activity);
    rc |= run_voice_gate_header_case("nxdn-voice-gate-on", nxdn_body, 0,
                                     dsd_engine_trunk_scan_nxdn_conventional_activity);
    rc |= run_voice_gate_header_case("nxdn48-voice-gate-on", nxdn48_body, 0,
                                     dsd_engine_trunk_scan_nxdn_conventional_activity);
    return rc;
}

/* With the gate on, decoded voice media holds a conventional target with no header ever
 * reported: the 250 ms dwell would otherwise rotate at 0.25. Fresh media refreshes the
 * hold, and the target rotates only after the hold lapses and the dwell re-elapses. */
static int
test_conventional_voice_gate_media_hold(void) {
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
    opts.scan_voice_only = 1;
    opts.scan_voice_qualify_ms = 1000;
    opts.scan_voice_hold_ms = 2000;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "voice-gate media scan init failed: %s\n", err);
        test_rc = 1;
    }

    if (seed_voice_gate_media_epoch(&state, 0.10, 0.25, DSD_CALL_CRYPTO_CLEAR) != 0) {
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "voice media did not hold conventional target past dwell\n");
        test_rc = 1;
    }
    if (state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_VOICE) {
        DSD_FPRINTF(stderr, "fresh voice media did not publish the VOICE phase (got %u)\n",
                    (unsigned)state.scan_voice_gate_phase);
        test_rc = 1;
    }

    // Fresh media refreshes the hold: without it the 0.25 media would lapse at 0.50.
    if (dsd_call_state_update_media(&state, 0U, 1, 0.49) != 1) {
        DSD_FPRINTF(stderr, "voice-gate media refresh failed\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.49);
    dsd_engine_trunk_scan_tick(&opts, &state);
    // The call ends on sync loss at 0.50; the hold still runs from the 0.49 media until
    // 0.74, so the target stays and the status line shows TAIL rather than VOICE.
    if (dsd_call_state_end_ex(&state, 0U, 0.50, DSD_CALL_END_SYNC_LOSS) != 1) {
        DSD_FPRINTF(stderr, "voice-gate media epoch end failed\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.70);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "refreshed voice media did not extend the hold\n");
        test_rc = 1;
    }
    if (state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_TAIL) {
        DSD_FPRINTF(stderr, "stale voice media did not publish the TAIL phase (got %u)\n",
                    (unsigned)state.scan_voice_gate_phase);
        test_rc = 1;
    }
    // The hold lapses at 0.74, re-arming the dwell; the rotation follows a dwell later.
    trunk_scan_test_set_now(0.75);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "target rotated before the dwell re-elapsed\n");
        test_rc = 1;
    }
    if (state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY) {
        DSD_FPRINTF(stderr, "lapsed hold did not publish the QUALIFY phase (got %u)\n",
                    (unsigned)state.scan_voice_gate_phase);
        test_rc = 1;
    }
    trunk_scan_test_set_now(1.01);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "target did not rotate after media hold and dwell\n");
        test_rc = 1;
    }
    // The hop clears the phase; the next tick publishes it for the new conventional target.
    if (state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_OFF) {
        DSD_FPRINTF(stderr, "hop did not clear the voice-gate phase (got %u)\n", (unsigned)state.scan_voice_gate_phase);
        test_rc = 1;
    }
    trunk_scan_test_set_now(1.02);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY) {
        DSD_FPRINTF(stderr, "new target did not publish the QUALIFY phase (got %u)\n",
                    (unsigned)state.scan_voice_gate_phase);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_state_ext_free_all(&state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/* A protocol terminator may end the canonical call before the coordinator gets its first
 * post-dispatch tick. The retained last-media stamp must still start the target's full hold;
 * otherwise this sequence rotates on dwell and makes activity_hold_ms appear ineffective. */
static int
test_conventional_voice_gate_terminator_before_first_tick(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,dmr-conventional,461000000,,250,5000,\n"
                             "b,dmr-conventional,462000000,,250,5000,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.scan_voice_only = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "terminator-first voice-gate scan init failed: %s\n", err);
        test_rc = 1;
    }
    if (seed_voice_gate_media_epoch(&state, 0.10, 0.25, DSD_CALL_CRYPTO_CLEAR) != 0
        || dsd_call_state_end_ex(&state, 0U, 0.26, DSD_CALL_END_TERMINATOR) != 1) {
        DSD_FPRINTF(stderr, "terminator-first voice-gate media seed failed\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0
        || state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_TAIL) {
        DSD_FPRINTF(stderr, "terminator-first media did not enter TAIL on the original target\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(5.249);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "terminator-first target rotated before the five-second hold\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(5.25);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0
        || state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY) {
        DSD_FPRINTF(stderr, "terminator-first target did not leave TAIL at the hold boundary\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(5.501);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1
        || state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_OFF) {
        DSD_FPRINTF(stderr, "terminator-first target did not rotate after hold plus dwell\n");
        test_rc = 1;
    }
    dsd_call_snapshot incoming_call;
    if (dsd_call_state_get(&state, 0U, &incoming_call) != 0) {
        DSD_FPRINTF(stderr, "retained media crossed into the next target's call snapshot\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(5.51);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_QUALIFY) {
        DSD_FPRINTF(stderr, "incoming conventional target did not enter QUALIFY\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_state_ext_free_all(&state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/* Policy still gates the media hold: encrypted voice the operator locks out
 * (trunk_tune_enc_calls=0) never refreshes it, so the target rotates on dwell. */
static int
test_conventional_voice_gate_enc_lockout_media_rotates(void) {
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
    opts.scan_voice_only = 1;
    opts.scan_voice_qualify_ms = 1000;
    opts.scan_voice_hold_ms = 2000;
    opts.trunk_tune_enc_calls = 0;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "voice-gate lockout scan init failed: %s\n", err);
        test_rc = 1;
    }

    if (seed_voice_gate_media_epoch(&state, 0.10, 0.25, DSD_CALL_CRYPTO_ENCRYPTED) != 0) {
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "locked-out encrypted media held conventional target\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_state_ext_free_all(&state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/* Trunked targets are unchanged by the gate: control-channel-only traffic carries no
 * voice media, so a P25/DMR pair rotates after dwell; a tuned trunked target still
 * holds while trunk_is_tuned says the receiver is following a call. */
static int
test_trunked_voice_gate_control_only_unchanged(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("p,p25-trunk,851000000,,250,,\n"
                             "d,dmr-trunk,852000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.scan_voice_only = 1;
    opts.scan_voice_qualify_ms = 1000;
    opts.scan_voice_hold_ms = 2000;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "voice-gate trunked scan init failed: %s\n", err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.24);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "trunked scan rotated before dwell with gate on\n");
        test_rc = 1;
    }
    if (opts.trunk_is_tuned != 0) {
        DSD_FPRINTF(stderr, "trunked tick disturbed trunk_is_tuned\n");
        test_rc = 1;
    }
    if (state.scan_voice_gate_phase != (uint8_t)DSD_SCAN_VOICE_GATE_OFF) {
        DSD_FPRINTF(stderr, "trunked target published a voice-gate phase (got %u)\n",
                    (unsigned)state.scan_voice_gate_phase);
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "control-only trunked target did not rotate after dwell with gate on\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);

    reset_scan_opts_state(&opts, &state);
    opts.scan_voice_only = 1;
    opts.scan_voice_qualify_ms = 1000;
    opts.scan_voice_hold_ms = 2000;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    trunk_scan_test_set_now(0.0);
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "voice-gate tuned scan init failed: %s\n", err);
        test_rc = 1;
    }
    opts.trunk_is_tuned = 1;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "tuned trunked target rotated away with gate on\n");
        test_rc = 1;
    }
    if (opts.trunk_is_tuned != 1) {
        DSD_FPRINTF(stderr, "trunked tick cleared trunk_is_tuned while tuned\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_nxdn_trunk_target_holds_while_tuned(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("n,nxdn-trunk,461000000,,250,,\n"
                             "c,dmr-conventional,462000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "nxdn hold scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    opts.trunk_is_tuned = 1;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "tuned NXDN trunk target rotated away while tuned\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "tuned NXDN trunk target rotated away on second tick\n");
        test_rc = 1;
    }

    opts.trunk_is_tuned = 0;
    trunk_scan_test_set_now(0.78);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "NXDN trunk target rotated on idle clock restart tick\n");
        test_rc = 1;
    }
    trunk_scan_test_set_now(1.04);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "NXDN trunk target did not rotate after release and dwell\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_nxdn_state_isolated_per_target(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,nxdn-trunk,461000000,,250,,\n"
                             "b,nxdn-trunk,462000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "nxdn identity scan init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    seed_nxdn_identity(&state, 12, 461012500, 7U, 0x25U, 3, "Type-C", 1, 96, 12, 4);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "nxdn identity scan did not rotate to second target\n");
        test_rc = 1;
    }
    /* A never-visited target must come back with the "no RAN decoded" sentinel, not a
     * fabricated RAN 0. */
    test_rc |= expect_nxdn_identity("fresh target", &state, 0, 0, (unsigned int)-1, 0U, 0, " ", 0, 0, 0, 0);

    seed_nxdn_identity(&state, 34, 462037500, 9U, 0x44U, 5, "Type-C", 2, 110, 25, 6);
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "nxdn identity scan did not rotate back to first target\n");
        test_rc = 1;
    }
    test_rc |= expect_nxdn_identity("restored target", &state, 12, 461012500, 7U, 0x25U, 3, "Type-C", 1, 96, 12, 4);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/*
 * The NXDN missing-channel diagnostics have to work per target under trunk scan: the coordinator
 * is the only holder of the parked target's chan_csv path, and one decoder state is shared by
 * every target, so the ledger of channels seen without a mapping has to travel with the snapshot.
 */
static int
test_nxdn_trunk_diag_isolated_per_target(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char chan_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(chan_path, sizeof chan_path, dir, "chan.csv") != 0
        || write_text_file(chan_path, "channel,frequency\n1,461012500\n") != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    if (write_targets_file(dir,
                           "a,nxdn-trunk,461000000,chan.csv,250,,\n"
                           "b,nxdn-trunk,462000000,,250,,\n",
                           target_path, sizeof target_path)
        != 0) {
        cleanup_paths(dir, NULL, chan_path);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "nxdn diag scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    const char* active_csv = dsd_trunk_scan_hook_active_chan_csv(&state);
    if (!active_csv || !strstr(active_csv, "chan.csv")) {
        DSD_FPRINTF(stderr, "parked target chan_csv not visible to protocol code: %s\n",
                    active_csv ? active_csv : "(null)");
        test_rc = 1;
    }
    if (nxdn_trunk_diag_chan_map_path(&opts, &state) == NULL) {
        DSD_FPRINTF(stderr, "diag path unresolved for a target with a chan_csv\n");
        test_rc = 1;
    }

    /* Target A decodes a grant for a channel its map does not cover. */
    nxdn_trunk_diag_log_missing_channel_once(&opts, &state, 5, "grant");
    if (nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0) != 1) {
        DSD_FPRINTF(stderr, "missing channel not recorded for parked scan target\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "nxdn diag scan did not rotate to second target\n");
        test_rc = 1;
    }
    if (nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0) != 0) {
        DSD_FPRINTF(stderr, "second target inherited the first target's missing-channel ledger\n");
        test_rc = 1;
    }
    if (dsd_trunk_scan_hook_active_chan_csv(&state) != NULL) {
        DSD_FPRINTF(stderr, "target without a chan_csv reported one\n");
        test_rc = 1;
    }
    /* Without a channel map there is nothing to report a missing mapping against. */
    nxdn_trunk_diag_log_missing_channel_once(&opts, &state, 7, "grant");
    if (nxdn_trunk_diag_collect_unmapped_channels(&state, NULL, 0) != 0) {
        DSD_FPRINTF(stderr, "target without a chan_csv recorded a missing channel\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "nxdn diag scan did not rotate back to first target\n");
        test_rc = 1;
    }
    uint16_t missing[4];
    DSD_MEMSET(missing, 0, sizeof missing);
    if (nxdn_trunk_diag_collect_unmapped_channels(&state, missing, 4) != 1 || missing[0] != 5) {
        DSD_FPRINTF(stderr, "first target did not get its missing-channel ledger back\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    if (dsd_trunk_scan_hook_active_chan_csv(&state) != NULL) {
        DSD_FPRINTF(stderr, "scan chan_csv hook still installed after shutdown\n");
        test_rc = 1;
    }
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, chan_path);
    return test_rc;
}

static int
test_state_ext_cleanup_clears_scan_hooks(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,dmr-trunk,461000000,,250,250,\n", target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
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

    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_protocol_hooks_only_expose_matching_target_contexts(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("dmr,dmr-trunk,451000000,,250,,\n", target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "dmr hook gating scan init failed: %s\n", err);
        test_rc = 1;
    }
    if (test_rc == 0 && dsd_trunk_scan_hook_dmr_ctx() == NULL) {
        DSD_FPRINTF(stderr, "dmr hook missing for active DMR trunk target\n");
        test_rc = 1;
    }
    if (test_rc == 0 && dsd_trunk_scan_hook_p25_ctx() != NULL) {
        DSD_FPRINTF(stderr, "p25 hook exposed context for active DMR trunk target\n");
        test_rc = 1;
    }
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_state_ext_free_all(&state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    if (test_rc != 0) {
        return test_rc;
    }

    if (make_runtime_targets("p25,p25-trunk,851000000,,250,,\n", target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    DSD_MEMSET(err, 0, sizeof err);
    trunk_scan_test_set_now(0.0);
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "p25 hook gating scan init failed: %s\n", err);
        test_rc = 1;
    }
    if (test_rc == 0 && dsd_trunk_scan_hook_p25_ctx() == NULL) {
        DSD_FPRINTF(stderr, "p25 hook missing for active P25 trunk target\n");
        test_rc = 1;
    }
    if (test_rc == 0 && dsd_trunk_scan_hook_dmr_ctx() != NULL) {
        DSD_FPRINTF(stderr, "dmr hook exposed context for active P25 trunk target\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    dsd_state_ext_free_all(&state);
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
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
    opts.trunk_is_tuned = 1;
    g_dmr_tick_calls = 0;

    trunk_scan_test_set_now(0.10);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr active call did not hold scan target\n");
        test_rc = 1;
    }

    opts.trunk_is_tuned = 0;
    g_dmr_tick_calls = 0;
    g_dmr_tick_release_tuned = 1;

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (g_dmr_tick_calls == 0) {
        DSD_FPRINTF(stderr, "dmr target SM was not ticked before scan hold check\n");
        test_rc = 1;
    }
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr scan rotated before post-release idle dwell restart\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "dmr scan did not rotate after SM timeout released hold\n");
        test_rc = 1;
    }

    g_dmr_tick_release_tuned = 0;
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static dsd_trunk_tune_result
failing_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
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
static dsd_trunk_tune_result g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

static uint64_t g_counting_tune_to_cc_last_request_id = 0U;

static dsd_trunk_tune_result
counting_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    g_counting_tune_to_cc_last_request_id = request_id;
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
    return g_counting_tune_to_cc_result;
}

static int
test_p25_pending_retune_holds_scan_dwell(void) {
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
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_PENDING;

    char err[256] = {0};
    trunk_scan_test_set_now(1.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    p25_sm_ctx_t* ctx = (p25_sm_ctx_t*)dsd_engine_trunk_scan_active_p25_ctx();
    if (rc != 0 || !ctx || !ctx->cc_tune_pending || ctx->cc_tune_request_id == 0U || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "pending scan init failed rc=%d ctx=%p pending=%d request=%llu calls=%d err=%s\n", rc,
                    (void*)ctx, ctx ? ctx->cc_tune_pending : -1,
                    (unsigned long long)(ctx ? ctx->cc_tune_request_id : 0U), g_counting_tune_to_cc_calls, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(2.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "pending P25 retune did not hold scan dwell active=%zu calls=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls);
        test_rc = 1;
    }

    if (ctx && ctx->cc_tune_request_id != 0U) {
        dsd_trunk_tuning_request_complete(ctx->cc_tune_request_id, DSD_TRUNK_TUNE_RESULT_OK);
        /* This fixture stubs the P25 SM, so model the live-loop ordering where
         * its tick observes completion before the scan coordinator runs. */
        (void)p25_sm_restart_pending_cc_acquisition(ctx, &opts, &state, 2.0, "test-complete");
    }
    if (ctx && ctx->cc_tune_pending) {
        DSD_FPRINTF(stderr, "P25 tick did not resolve completed scan retune\n");
        test_rc = 1;
    }
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    trunk_scan_test_set_now(10.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "P25 pending completion consumed scan dwell active=%zu calls=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls);
        test_rc = 1;
    }

    trunk_scan_test_set_now(10.20);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(10.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || g_counting_tune_to_cc_calls != 2) {
        DSD_FPRINTF(stderr, "scan dwell did not restart after pending completion active=%zu calls=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/*
 * Once the NXDN decoder corrects an nxdn-trunk target's control channel from the site broadcast
 * (nxdn_cch_info_dfa_version), the coordinator must carry that correction across rotations and
 * re-park on it rather than on the stale CSV frequency. Pins the snapshot + retune contract the
 * decoder-side adoption depends on.
 */
static int
test_nxdn_trunk_target_follows_corrected_cc(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,nxdn-trunk,461000000,,250,,\n"
                             "b,nxdn-trunk,462000000,,250,,\n",
                             target_path, sizeof target_path, dir, sizeof dir)
        != 0) {
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_freq = 0;

    const long int corrected_cc = 461012500L;
    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_freq != 461000000L) {
        DSD_FPRINTF(stderr, "corrected-cc scan init failed rc=%d active=%zu freq=%ld err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_freq, err);
        test_rc = 1;
    }

    /* The decoder adopts the site-broadcast outbound control channel while parked on target A. */
    state.trunk_lcn_freq[0] = corrected_cc;
    state.p25_cc_freq = corrected_cc;
    state.trunk_cc_freq = corrected_cc;

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || g_counting_tune_to_cc_freq != 462000000L) {
        DSD_FPRINTF(stderr, "corrected-cc scan did not rotate to second target active=%zu freq=%ld\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_freq);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "corrected-cc scan did not rotate back to first target\n");
        test_rc = 1;
    }
    if (g_counting_tune_to_cc_freq != corrected_cc) {
        DSD_FPRINTF(stderr, "re-park used stale CSV frequency %ld instead of corrected %ld\n",
                    g_counting_tune_to_cc_freq, corrected_cc);
        test_rc = 1;
    }
    if (state.p25_cc_freq != corrected_cc || state.trunk_cc_freq != corrected_cc) {
        DSD_FPRINTF(stderr, "re-park did not restore corrected CC p25=%ld trunk=%ld\n", state.p25_cc_freq,
                    state.trunk_cc_freq);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_p25_pending_retune_adopts_sm_retry(void) {
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
    dsd_trunk_tuning_requests_reset();

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_PENDING;

    char err[256] = {0};
    trunk_scan_test_set_now(1.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    p25_sm_ctx_t* ctx = (p25_sm_ctx_t*)dsd_engine_trunk_scan_active_p25_ctx();
    const uint64_t failed_request_id = ctx ? ctx->cc_tune_request_id : 0U;
    if (rc != 0 || !ctx || !ctx->cc_tune_pending || failed_request_id == 0U || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "P25 recovery adoption init failed rc=%d ctx=%p request=%llu calls=%d err=%s\n", rc,
                    (void*)ctx, (unsigned long long)failed_request_id, g_counting_tune_to_cc_calls, err);
        test_rc = 1;
    }
    if (!ctx) {
        DSD_MEMSET(&hooks, 0, sizeof hooks);
        dsd_trunk_tuning_hooks_set(hooks);
        dsd_engine_trunk_scan_shutdown(&opts, &state);
        trunk_scan_test_clear_now();
        dsd_trunk_tuning_requests_reset();
        cleanup_paths(dir, target_path, NULL);
        return 1;
    }

    dsd_trunk_tuning_request_publish(failed_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    const uint64_t retry_request_id = dsd_trunk_tuning_request_begin();
    if (retry_request_id == 0U) {
        DSD_FPRINTF(stderr, "P25 recovery adoption could not allocate retry request\n");
        test_rc = 1;
    } else {
        dsd_trunk_tuning_request_mark_ready(retry_request_id);
        ctx->cc_tune_request_id = retry_request_id;
        ctx->cc_tune_pending = 1;
        ctx->cc_sync_pending = 1;
        ctx->state = P25_SM_ON_CC;
    }

    trunk_scan_test_set_now(2.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_calls != 1 || !ctx->cc_tune_pending
        || ctx->cc_tune_request_id != retry_request_id) {
        DSD_FPRINTF(stderr, "scan did not adopt P25 retry active=%zu calls=%d pending=%d request=%llu/%llu\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls, ctx->cc_tune_pending,
                    (unsigned long long)ctx->cc_tune_request_id, (unsigned long long)retry_request_id);
        test_rc = 1;
    }

    dsd_trunk_tuning_request_publish(retry_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    (void)p25_sm_restart_pending_cc_acquisition(ctx, &opts, &state, 2.1, "test-sm-recovery");
    trunk_scan_test_set_now(2.1);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_calls != 1 || ctx->cc_tune_pending) {
        DSD_FPRINTF(stderr, "adopted P25 retry completion advanced scan active=%zu calls=%d pending=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls, ctx->cc_tune_pending);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    dsd_trunk_tuning_requests_reset();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_p25_pending_retune_preserves_completed_sm_recovery(void) {
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
    dsd_trunk_tuning_requests_reset();

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_PENDING;

    char err[256] = {0};
    trunk_scan_test_set_now(1.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    p25_sm_ctx_t* ctx = (p25_sm_ctx_t*)dsd_engine_trunk_scan_active_p25_ctx();
    const uint64_t failed_request_id = ctx ? ctx->cc_tune_request_id : 0U;
    if (rc != 0 || !ctx || failed_request_id == 0U || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "P25 completed recovery init failed rc=%d ctx=%p request=%llu calls=%d err=%s\n", rc,
                    (void*)ctx, (unsigned long long)failed_request_id, g_counting_tune_to_cc_calls, err);
        test_rc = 1;
    }
    if (!ctx) {
        DSD_MEMSET(&hooks, 0, sizeof hooks);
        dsd_trunk_tuning_hooks_set(hooks);
        dsd_engine_trunk_scan_shutdown(&opts, &state);
        trunk_scan_test_clear_now();
        dsd_trunk_tuning_requests_reset();
        cleanup_paths(dir, target_path, NULL);
        return 1;
    }

    dsd_trunk_tuning_request_publish(failed_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    const uint64_t retry_request_id = dsd_trunk_tuning_request_begin();
    double retry_completed_m = 0.0;
    if (retry_request_id == 0U) {
        DSD_FPRINTF(stderr, "P25 completed recovery could not allocate retry request\n");
        test_rc = 1;
    } else {
        dsd_trunk_tuning_request_mark_ready(retry_request_id);
        dsd_trunk_tuning_request_publish(retry_request_id, DSD_TRUNK_TUNE_RESULT_OK);
        (void)dsd_trunk_tuning_request_status(retry_request_id, &retry_completed_m);
        (void)p25_sm_restart_pending_cc_acquisition(ctx, &opts, &state, retry_completed_m, "test-sm-recovery");
    }

    trunk_scan_test_set_now(2.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_calls != 1
        || ctx->state != P25_SM_ON_CC || ctx->cc_tune_pending) {
        DSD_FPRINTF(stderr, "completed P25 recovery was abandoned active=%zu calls=%d state=%d pending=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls, (int)ctx->state,
                    ctx->cc_tune_pending);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    dsd_trunk_tuning_requests_reset();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_generic_pending_retune_holds_and_recovers(void) {
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

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_PENDING;

    char err[256] = {0};
    trunk_scan_test_set_now(1.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    uint64_t request_id = dsd_trunk_tuning_pending_request();
    if (rc != 0 || request_id == 0U || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "generic pending scan init failed rc=%d request=%llu calls=%d err=%s\n", rc,
                    (unsigned long long)request_id, g_counting_tune_to_cc_calls, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(2.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "generic pending retune did not hold scan active=%zu calls=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls);
        test_rc = 1;
    }

    dsd_trunk_tuning_request_publish(request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    trunk_scan_test_set_now(2.10);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || g_counting_tune_to_cc_calls != 2
        || dsd_trunk_tuning_pending_request() != 0U
        || !dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation())) {
        DSD_FPRINTF(stderr, "generic async failure did not recover active=%zu calls=%d pending=%llu\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls,
                    (unsigned long long)dsd_trunk_tuning_pending_request());
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
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
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_ted_sps = 0;

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
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
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || g_counting_tune_to_cc_ted_sps != 10) {
        DSD_FPRINTF(stderr, "p25 fdma target retune did not receive FDMA CC sps active=%zu sps=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_ted_sps);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_ted_sps != 8) {
        DSD_FPRINTF(stderr, "p25 tdma target retune did not receive TDMA CC sps active=%zu sps=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_ted_sps);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_ted_sps = 0;
    g_counting_tune_to_cc_freq = 0;

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || g_counting_tune_to_cc_calls != 1 || g_counting_tune_to_cc_ted_sps != 5) {
        DSD_FPRINTF(stderr, "p25 initial retune did not use RTL output rate rc=%d calls=%d sps=%d err=%s\n", rc,
                    g_counting_tune_to_cc_calls, g_counting_tune_to_cc_ted_sps, err);
        test_rc = 1;
    }

    state.p25_cc_is_tdma = 1;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(0.52);
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
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
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

    trunk_scan_test_set_now(0.26);
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

    trunk_scan_test_set_now(0.52);
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
    trunk_scan_test_clear_now();
    (void)remove(chan_b_path);
    cleanup_paths(dir, target_path, chan_a_path);
    return test_rc;
}

static int
test_p25_encrypted_call_cache_state_isolated_per_target(void) {
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "encrypted-call cache snapshot init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    (void)dsd_enc_lockout_note(&state, 2468U, 1, 0x84, 0x1234);

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || dsd_enc_lockout_lookup(&state, 2468U, 1, NULL)) {
        DSD_FPRINTF(stderr, "enc lockout ledger leaked into target 1 active=%zu\n",
                    dsd_engine_trunk_scan_active_index(&state));
        test_rc = 1;
    }

    (void)dsd_enc_lockout_note(&state, 3579U, 0, 0xAA, 0x0001);

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || !dsd_enc_lockout_entry_active(&state, 2468U, 1)
        || dsd_enc_lockout_lookup(&state, 3579U, 0, NULL)) {
        DSD_FPRINTF(stderr, "enc lockout target 0 restore failed active=%zu\n",
                    dsd_engine_trunk_scan_active_index(&state));
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.78);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || !dsd_enc_lockout_entry_active(&state, 3579U, 0)
        || dsd_enc_lockout_lookup(&state, 2468U, 1, NULL)) {
        DSD_FPRINTF(stderr, "enc lockout target 1 restore failed active=%zu\n",
                    dsd_engine_trunk_scan_active_index(&state));
        test_rc = 1;
    }

    // Key material changed: the global epoch moves while entries stay put, so
    // every target's lockouts (restored snapshots included) stop blocking
    // until re-confirmed on that target.
    dsd_enc_lockout_bump_key_epoch(&state);
    if (!dsd_enc_lockout_lookup(&state, 3579U, 0, NULL) || dsd_enc_lockout_entry_active(&state, 3579U, 0)) {
        DSD_FPRINTF(stderr, "key epoch bump did not invalidate the active target's lockouts\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(1.04);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || dsd_enc_lockout_entry_active(&state, 2468U, 1)
        || !dsd_enc_lockout_lookup(&state, 2468U, 1, NULL)) {
        DSD_FPRINTF(stderr, "key epoch bump did not invalidate target 0's restored lockouts\n");
        test_rc = 1;
    }

    // Re-confirmation after the epoch change re-locks the target.
    if (dsd_enc_lockout_note(&state, 2468U, 1, 0x84, 0x1234) != 1 || !dsd_enc_lockout_entry_active(&state, 2468U, 1)) {
        DSD_FPRINTF(stderr, "stale entry did not re-lock on re-confirmation\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(1.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || dsd_enc_lockout_entry_active(&state, 3579U, 0)) {
        DSD_FPRINTF(stderr, "target 1 snapshot regained blocking without re-confirmation\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

// A user purge must reach the ledger copies parked in every scan-target
// snapshot, not just the target currently on air -- otherwise the next target
// switch restores the very entries the user asked to forget.
static int
test_enc_lockout_purge_clears_scan_snapshots(void) {
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "enc lockout purge init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    // Arm a lockout on each target so both a parked snapshot and the live
    // ledger hold entries when the purge lands.
    (void)dsd_enc_lockout_note(&state, 2468U, 1, 0x84, 0x1234);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    (void)dsd_enc_lockout_note(&state, 3579U, 0, 0xAA, 0x0001);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || !dsd_enc_lockout_entry_active(&state, 3579U, 0)) {
        DSD_FPRINTF(stderr, "enc lockout purge setup failed active=%zu\n", dsd_engine_trunk_scan_active_index(&state));
        test_rc = 1;
    }

    // The UI purge path: live ledger plus every parked snapshot.
    dsd_enc_lockout_clear_all(&state);
    dsd_trunk_scan_hook_enc_lockout_clear_snapshots(&state);
    if (dsd_enc_lockout_lookup(&state, 3579U, 0, NULL)) {
        DSD_FPRINTF(stderr, "purge left the live ledger populated\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || dsd_enc_lockout_lookup(&state, 2468U, 1, NULL)) {
        DSD_FPRINTF(stderr, "target 0 snapshot restored a purged lockout active=%zu\n",
                    dsd_engine_trunk_scan_active_index(&state));
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.78);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || dsd_enc_lockout_lookup(&state, 3579U, 0, NULL)) {
        DSD_FPRINTF(stderr, "target 1 snapshot restored a purged lockout active=%zu\n",
                    dsd_engine_trunk_scan_active_index(&state));
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
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
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_ted_sps = 0;
    g_counting_tune_to_cc_freq = 0;

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "p25 learned CC scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.p25_cc_freq = 851500000L;
    state.trunk_cc_freq = 851500000L;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    state.p25_cc_freq = 852500000L;
    state.trunk_cc_freq = 852500000L;
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_freq != 851500000L
        || state.p25_cc_freq != 851500000L || state.trunk_cc_freq != 851500000L) {
        DSD_FPRINTF(stderr, "p25 target did not reuse learned CC active=%zu tune=%ld p25=%ld trunk=%ld\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_freq, state.p25_cc_freq,
                    state.trunk_cc_freq);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
    rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr learned CC scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    state.trunk_cc_freq = 451500000L;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    state.trunk_cc_freq = 452500000L;
    trunk_scan_test_set_now(0.52);
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
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || state.rf_mod != 1) {
        DSD_FPRINTF(stderr, "locked P25 demod not preserved on init rc=%d active=%zu rf_mod=%d err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.rf_mod != 1) {
        DSD_FPRINTF(stderr, "locked demod overwritten on DMR target active=%zu rf_mod=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod);
        test_rc = 1;
    }

    state.rf_mod = 1;
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.rf_mod != 1) {
        DSD_FPRINTF(stderr, "locked demod overwritten on P25 return active=%zu rf_mod=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_target_retunes_select_four_level_sps_profile(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation\n";
    if (make_temp_dir(dir, sizeof dir) != 0
        || write_targets_file_with_header(dir, header,
                                          "p25,p25-trunk,851000000,,250,,P25 forced,c4fm\n"
                                          "dmr,dmr-trunk,452000000,,250,,DMR forced,gfsk\n",
                                          target_path, sizeof target_path)
               != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.sps_hunt_counter = 17;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0
        || state.sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4 || state.sps_hunt_counter != 0) {
        DSD_FPRINTF(stderr, "P25 target retained stale SPS profile rc=%d active=%zu profile=%d counter=%d err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), state.sps_hunt_idx, state.sps_hunt_counter, err);
        test_rc = 1;
    }

    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.sps_hunt_counter = 23;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4
        || state.sps_hunt_counter != 0) {
        DSD_FPRINTF(stderr, "DMR target retained stale SPS profile active=%zu profile=%d counter=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.sps_hunt_idx, state.sps_hunt_counter);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_per_target_modulation_overrides_global_lock(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation\n";
    if (make_temp_dir(dir, sizeof dir) != 0
        || write_targets_file_with_header(dir, header,
                                          "p25,p25-trunk,851000000,,250,,P25 auto,auto\n"
                                          "dmr,dmr-trunk,452000000,,250,,DMR forced,gfsk\n",
                                          target_path, sizeof target_path)
               != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.mod_cli_lock = 1;
    opts.trunk_is_tuned = 1;
    opts.mod_qpsk = 1;
    opts.mod_c4fm = 0;
    opts.mod_gfsk = 0;
    state.rf_mod = 1;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || state.rf_mod != 0 || opts.mod_cli_lock != 0) {
        DSD_FPRINTF(stderr, "target auto modulation did not override global lock rc=%d active=%zu rf_mod=%d lock=%d\n",
                    rc, dsd_engine_trunk_scan_active_index(&state), state.rf_mod, opts.mod_cli_lock);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.rf_mod != 2 || opts.mod_gfsk != 1
        || opts.mod_cli_lock != 1) {
        DSD_FPRINTF(stderr, "target GFSK modulation did not apply active=%zu rf_mod=%d gfsk=%d lock=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), state.rf_mod, opts.mod_gfsk, opts.mod_cli_lock);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    if (opts.trunk_is_tuned != 1 || opts.mod_cli_lock != 1 || opts.mod_qpsk != 1 || opts.mod_gfsk != 0) {
        DSD_FPRINTF(stderr, "shutdown did not restore saved modulation opts lock=%d qpsk=%d gfsk=%d\n",
                    opts.mod_cli_lock, opts.mod_qpsk, opts.mod_gfsk);
        test_rc = 1;
    }
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_active_p25_cqpsk_request_tracks_target_modulation(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation\n";
    if (make_temp_dir(dir, sizeof dir) != 0
        || write_targets_file_with_header(dir, header,
                                          "c4fm,p25-trunk,851000000,,250,,C4FM,c4fm\n"
                                          "cqpsk,p25-trunk,852000000,,250,,CQPSK,cqpsk\n"
                                          "auto,p25-trunk,853000000,,250,,Auto,auto\n"
                                          "dmr,dmr-trunk,454000000,,250,,DMR,gfsk\n",
                                          target_path, sizeof target_path)
               != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    int cqpsk_enable = -1;
    if (rc != 0 || !dsd_engine_trunk_scan_active_p25_cqpsk_request(&state, &cqpsk_enable) || cqpsk_enable != 0) {
        DSD_FPRINTF(stderr, "active C4FM target did not request CQPSK off rc=%d active=%zu enable=%d err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), cqpsk_enable, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    cqpsk_enable = -1;
    if (dsd_engine_trunk_scan_active_index(&state) != 1
        || !dsd_engine_trunk_scan_active_p25_cqpsk_request(&state, &cqpsk_enable) || cqpsk_enable != 1) {
        DSD_FPRINTF(stderr, "active CQPSK target did not request CQPSK on active=%zu enable=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), cqpsk_enable);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    state.p25_cc_is_tdma = 0;
    cqpsk_enable = -1;
    if (dsd_engine_trunk_scan_active_index(&state) != 2
        || !dsd_engine_trunk_scan_active_p25_cqpsk_request(&state, &cqpsk_enable) || cqpsk_enable != 0) {
        DSD_FPRINTF(stderr, "active auto P25 target did not request FDMA default active=%zu enable=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), cqpsk_enable);
        test_rc = 1;
    }
    state.p25_cc_is_tdma = 1;
    cqpsk_enable = -1;
    if (!dsd_engine_trunk_scan_active_p25_cqpsk_request(&state, &cqpsk_enable) || cqpsk_enable != 1) {
        DSD_FPRINTF(stderr, "active auto P25 target did not request TDMA default enable=%d\n", cqpsk_enable);
        test_rc = 1;
    }
    /* Issue #423: an FDMA target that has decoded a P25p1 NID through the CQPSK chain keeps it
     * across the scan rotation instead of falling back to the C4FM default. */
    state.p25_cc_is_tdma = 0;
    state.p25_p1_validated_rf_mod = 1;
    cqpsk_enable = -1;
    if (!dsd_engine_trunk_scan_active_p25_cqpsk_request(&state, &cqpsk_enable) || cqpsk_enable != 1) {
        DSD_FPRINTF(stderr, "active auto P25 target dropped a learned CQPSK enable=%d\n", cqpsk_enable);
        test_rc = 1;
    }
    state.p25_p1_validated_rf_mod = -1;

    trunk_scan_test_set_now(0.78);
    dsd_engine_trunk_scan_tick(&opts, &state);
    cqpsk_enable = -1;
    if (dsd_engine_trunk_scan_active_index(&state) != 3
        || dsd_engine_trunk_scan_active_p25_cqpsk_request(&state, &cqpsk_enable) || cqpsk_enable != -1) {
        DSD_FPRINTF(stderr, "active DMR target unexpectedly returned a P25 CQPSK request active=%zu enable=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), cqpsk_enable);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_per_target_rtl_gain_overrides_and_restores_global_default(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,rtl_gain\n";
    if (make_temp_dir(dir, sizeof dir) != 0
        || write_targets_file_with_header(dir, header,
                                          "strong,p25-trunk,851000000,,250,,strong,10\n"
                                          "default,dmr-trunk,452000000,,250,,default\n"
                                          "auto,dmr-conventional,461000000,,250,,auto,auto\n",
                                          target_path, sizeof target_path)
               != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.rtl_gain_value = 22;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || opts.rtl_gain_value != 10) {
        DSD_FPRINTF(stderr, "target gain init mismatch rc=%d active=%zu gain=%d err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), opts.rtl_gain_value, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || opts.rtl_gain_value != 22) {
        DSD_FPRINTF(stderr, "empty target did not restore global gain active=%zu gain=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), opts.rtl_gain_value);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 2 || opts.rtl_gain_value != 0) {
        DSD_FPRINTF(stderr, "auto target did not request autogain active=%zu gain=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), opts.rtl_gain_value);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    if (opts.rtl_gain_value != 22) {
        DSD_FPRINTF(stderr, "shutdown did not restore global gain=%d\n", opts.rtl_gain_value);
        test_rc = 1;
    }
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "guard scan init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }

    g_p25_tick_guard_available = 0;
    trunk_scan_test_set_now(0.26);
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
    trunk_scan_test_clear_now();
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
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 1;
    g_counting_tune_to_cc_ted_sps = 0;

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "single target init rc=%d calls=%d err=%s\n", rc, g_counting_tune_to_cc_calls, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(1.99);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "single target retried before cooldown expired\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(2.01);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (g_counting_tune_to_cc_calls != 2) {
        DSD_FPRINTF(stderr, "single target did not retry after cooldown; calls=%d\n", g_counting_tune_to_cc_calls);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "retune scan init failed: %s\n", err);
        test_rc = 1;
    }

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = failing_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "failed retune should not leave scanner on failed target\n");
        test_rc = 1;
    }

    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    trunk_scan_test_set_now(2.40);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "target did not rotate after retry cooldown expired\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "cooldown scan init failed rc=%d active=%zu calls=%d err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls, err);
        test_rc = 1;
    }

    g_counting_tune_to_cc_failures_remaining = 1;
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    int calls_after_failed_alternate = g_counting_tune_to_cc_calls;
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || calls_after_failed_alternate < 2) {
        DSD_FPRINTF(stderr, "failed alternate retune did not leave active target restored active=%zu calls=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), calls_after_failed_alternate);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
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
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/*
 * trunk_scan_advance() puts the original target back when every candidate retune fails, and
 * the last thing tried was an alternate: publishing only from the switch would leave that
 * alternate's id on screen while the receiver sits on the target it never left. Two failures
 * at init drive exactly that -- target "a" fails and starts cooling, the advance tries "b"
 * and it fails too, and "a" is then skipped by its own cooldown, so the restore is the only
 * thing that can put "a" back.
 */
static int
test_failed_alternate_retune_republishes_restored_target(void) {
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
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 2;

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "failed alternate scan init failed err=%s\n", err);
        test_rc = 1;
    }
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || g_counting_tune_to_cc_calls != 2) {
        DSD_FPRINTF(stderr, "failed alternate did not restore target 0: active=%zu calls=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_calls);
        test_rc = 1;
    }
    test_rc |= expect_published_target(&state, "failed alternate retune", "a", 1U, 2U);

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_counting_tune_to_cc_calls = 0;
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_ted_sps = 0;
    g_scan_tune_to_freq_ted_sps = 0;

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "dmr sps scan init failed rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || g_counting_tune_to_cc_ted_sps != 10) {
        DSD_FPRINTF(stderr, "dmr trunk retune did not receive DMR sps active=%zu sps=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_counting_tune_to_cc_ted_sps);
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 2 || g_scan_tune_to_freq_ted_sps != 10) {
        DSD_FPRINTF(stderr, "dmr conventional retune did not receive DMR sps active=%zu sps=%d\n",
                    dsd_engine_trunk_scan_active_index(&state), g_scan_tune_to_freq_ted_sps);
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
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
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
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
    if (opts.trunk_enable != 1 || opts.trunk_is_tuned != 1) {
        DSD_FPRINTF(stderr, "scan init failure did not restore trunk opts enabled=%d tuned=%d\n", opts.trunk_enable,
                    opts.trunk_is_tuned);
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
    if (rc == 0 || strstr(err, "requires an open RTL input or rigctl") == NULL) {
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
test_trunk_scan_rejects_unopened_rtl_without_rigctl(void) {
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
    state.rtl_ctx = NULL;
    opts.use_rigctl = 0;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc == 0 || strstr(err, "requires an open RTL input or rigctl") == NULL) {
        DSD_FPRINTF(stderr, "unopened RTL scan should reject without rigctl rc=%d err=%s\n", rc, err);
        test_rc = 1;
    }
    if (dsd_engine_trunk_scan_active_index(&state) != (size_t)-1 || dsd_engine_trunk_scan_target_count(&state) != 0) {
        DSD_FPRINTF(stderr, "unopened RTL rejection attached a coordinator\n");
        test_rc = 1;
    }
    if (dsd_trunk_scan_hook_p25_ctx() != NULL || dsd_trunk_scan_hook_dmr_ctx() != NULL) {
        DSD_FPRINTF(stderr, "unopened RTL rejection installed scan hooks\n");
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

/*
 * On-the-fly scan controls (#380). The coordinator owns the truth for hold and avoid
 * under --trunk-scan and publishes it beside the target id; app_control reaches it
 * only through the control hook, so the hook install is checked once here too.
 */
static int
scan_control_init(const char* body, dsd_opts* opts, dsd_state* state, char* dir, size_t dir_sz, char* target_path,
                  size_t path_sz) {
    if (make_runtime_targets(body, target_path, path_sz, dir, dir_sz) != 0) {
        return -1;
    }
    reset_scan_opts_state(opts, state);
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", target_path);
    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    if (dsd_engine_trunk_scan_init(opts, state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "scan control init failed err=%s\n", err);
        return -1;
    }
    return 0;
}

static int
expect_scan_control_publication(const dsd_state* state, const char* stage, unsigned hold, unsigned active_avoided,
                                unsigned avoided_count) {
    if (state->trunk_scan_hold != hold || state->trunk_scan_active_avoided != active_avoided
        || state->trunk_scan_avoided_count != avoided_count) {
        DSD_FPRINTF(stderr, "scan control publication after %s: hold=%u active_avoided=%u avoided=%u, want %u/%u/%u\n",
                    stage, (unsigned)state->trunk_scan_hold, (unsigned)state->trunk_scan_active_avoided,
                    (unsigned)state->trunk_scan_avoided_count, hold, active_avoided, avoided_count);
        return 1;
    }
    return 0;
}

static int
expect_active_target(const dsd_state* state, const char* stage, size_t want) {
    const size_t got = dsd_engine_trunk_scan_active_index(state);
    if (got != want) {
        DSD_FPRINTF(stderr, "active target after %s: %zu, want %zu\n", stage, got, want);
        return 1;
    }
    return 0;
}

static int
expect_control_rc(const char* stage, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "scan control %s returned %d, want %d\n", stage, got, want);
        return 1;
    }
    return 0;
}

static int
test_scan_control_unavailable_without_coordinator(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    int test_rc = expect_control_rc("without coordinator",
                                    dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE),
                                    DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    test_rc |= expect_control_rc("hook without coordinator",
                                 dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE),
                                 DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    return test_rc;
}

static int
test_scan_hold_pauses_rotation_and_release_restarts_dwell(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (scan_control_init("a,p25-trunk,851000000,,250,,\n"
                          "b,p25-trunk,852000000,,250,,\n",
                          &opts, &state, dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;

    /* Through the runtime hook once, to prove the coordinator installed it. */
    test_rc |=
        expect_control_rc("hold on", dsd_trunk_scan_hook_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE), 1);
    test_rc |= expect_scan_control_publication(&state, "hold on", 1U, 0U, 0U);

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(3.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "held past the dwell", 0U);

    test_rc |= expect_control_rc("hold off",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE), 0);
    test_rc |= expect_scan_control_publication(&state, "hold off", 0U, 0U, 0U);
    /* The dwell restarts from the release: a full 250 ms before the rotation resumes. */
    trunk_scan_test_set_now(3.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(3.24);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "release before a full dwell", 0U);
    trunk_scan_test_set_now(3.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "release after a full dwell", 1U);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_scan_advance_moves_now_and_keeps_hold(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (scan_control_init("a,p25-trunk,851000000,,250,,\n"
                          "b,p25-trunk,852000000,,250,,\n"
                          "c,p25-trunk,853000000,,250,,\n",
                          &opts, &state, dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;
    test_rc |=
        expect_control_rc("advance", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    test_rc |= expect_active_target(&state, "advance", 1U);
    test_rc |= expect_published_target(&state, "advance", "b", 2U, 3U);

    test_rc |= expect_control_rc("hold on",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE), 1);
    test_rc |= expect_control_rc("advance while held",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    test_rc |= expect_active_target(&state, "advance while held", 2U);
    test_rc |= expect_scan_control_publication(&state, "advance while held", 1U, 0U, 0U);
    trunk_scan_test_set_now(5.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(6.0);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "still held after manual advance", 2U);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_scan_avoid_steps_on_and_skips_the_target_thereafter(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (scan_control_init("a,p25-trunk,851000000,,250,,\n"
                          "b,p25-trunk,852000000,,250,,\n"
                          "c,p25-trunk,853000000,,250,,\n",
                          &opts, &state, dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;
    test_rc |= expect_control_rc("avoid a",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE), 0);
    test_rc |= expect_active_target(&state, "avoid a", 1U);
    test_rc |= expect_scan_control_publication(&state, "avoid a", 0U, 0U, 1U);
    test_rc |= expect_published_target(&state, "avoid a", "b", 2U, 3U);

    /* b -> c -> b: a never comes back. The switch arms the dwell, so each expired tick moves. */
    trunk_scan_test_set_now(0.3);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "rotation to c", 2U);
    trunk_scan_test_set_now(0.56);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "rotation skips a", 1U);
    test_rc |= expect_scan_control_publication(&state, "rotation skips a", 0U, 0U, 1U);

    /* Clear puts a back: the next rotation from b goes to c, then a. */
    test_rc |=
        expect_control_rc("clear", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR), 1);
    test_rc |= expect_scan_control_publication(&state, "clear", 0U, 0U, 0U);
    trunk_scan_test_set_now(0.9);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "after clear, to c", 2U);
    trunk_scan_test_set_now(1.16);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "after clear, back to a", 0U);
    test_rc |= expect_control_rc("clear with nothing avoided",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR), 0);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_scan_avoid_refuses_the_last_usable_target(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (scan_control_init("a,p25-trunk,851000000,,250,,\n"
                          "b,p25-trunk,852000000,,250,,\n",
                          &opts, &state, dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;
    test_rc |= expect_control_rc("avoid a",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE), 0);
    test_rc |= expect_active_target(&state, "avoid a", 1U);
    test_rc |=
        expect_control_rc("avoid b", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE),
                          DSD_TRUNK_SCAN_CONTROL_REFUSED);
    test_rc |= expect_active_target(&state, "refused avoid", 1U);
    test_rc |= expect_scan_control_publication(&state, "refused avoid", 0U, 0U, 1U);
    /* With a alone avoided the rotation has nowhere to go and stays on b. */
    trunk_scan_test_set_now(0.3);
    dsd_engine_trunk_scan_tick(&opts, &state);
    trunk_scan_test_set_now(0.56);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "rotation with one usable target", 1U);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_scan_avoid_falls_back_on_the_original_when_alternates_fail(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (scan_control_init("a,p25-trunk,851000000,,250,,\n"
                          "b,p25-trunk,852000000,,250,,\n"
                          "c,p25-trunk,853000000,,250,,\n",
                          &opts, &state, dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;
    g_counting_tune_to_cc_failures_remaining = 2;
    test_rc |= expect_control_rc("avoid with failing alternates",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE), 1);
    g_counting_tune_to_cc_failures_remaining = 0;
    test_rc |= expect_active_target(&state, "fallback", 0U);
    test_rc |= expect_published_target(&state, "fallback", "a", 1U, 3U);
    /* The row has to say the receiver is parked on a target the operator avoided. */
    test_rc |= expect_scan_control_publication(&state, "fallback", 0U, 1U, 1U);

    /* Once the alternates cool down the rotation leaves a. */
    trunk_scan_test_set_now(2.5);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "leaves the avoided fallback", 1U);
    test_rc |= expect_scan_control_publication(&state, "leaves the avoided fallback", 0U, 0U, 1U);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_scan_controls_on_a_single_target(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (scan_control_init("a,p25-trunk,851000000,,250,,\n", &opts, &state, dir, sizeof dir, target_path,
                          sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;
    test_rc |= expect_control_rc("single advance",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE),
                                 DSD_TRUNK_SCAN_CONTROL_REFUSED);
    test_rc |= expect_control_rc("single avoid",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE),
                                 DSD_TRUNK_SCAN_CONTROL_REFUSED);
    test_rc |= expect_control_rc("single hold",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE), 1);
    test_rc |= expect_scan_control_publication(&state, "single hold", 1U, 0U, 0U);
    test_rc |= expect_active_target(&state, "single target", 0U);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_scan_controls_report_busy_while_p25_guard_held(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (scan_control_init("a,p25-trunk,851000000,,250,,\n"
                          "b,p25-trunk,852000000,,250,,\n",
                          &opts, &state, dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;
    g_p25_tick_guard_available = 0;
    test_rc |=
        expect_control_rc("busy advance", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE),
                          DSD_TRUNK_SCAN_CONTROL_BUSY);
    test_rc |= expect_control_rc("busy avoid",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE),
                                 DSD_TRUNK_SCAN_CONTROL_BUSY);
    /* Hold and clear only flip flags, so they never need the guard. */
    test_rc |= expect_control_rc("hold under guard",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE), 1);
    test_rc |= expect_control_rc("clear under guard",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR), 0);
    test_rc |= expect_active_target(&state, "busy controls", 0U);
    test_rc |= expect_scan_control_publication(&state, "busy controls", 1U, 0U, 0U);
    g_p25_tick_guard_available = 1;
    if (g_p25_tick_guard_depth != 0) {
        DSD_FPRINTF(stderr, "busy scan controls leaked the P25 guard depth=%d\n", g_p25_tick_guard_depth);
        test_rc = 1;
    }
    test_rc |= expect_control_rc("advance after guard",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    test_rc |= expect_active_target(&state, "advance after guard", 1U);
    if (g_p25_tick_guard_depth != 0) {
        DSD_FPRINTF(stderr, "scan advance left the P25 guard held depth=%d\n", g_p25_tick_guard_depth);
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/* A retune that fails asynchronously normally moves the scan on; while held it retries
 * the held target after the cooldown instead. DMR trunk targets take the asynchronous
 * request path without the P25 recovery adoption on top of it. */
static int
test_scan_hold_retries_the_held_target_after_a_pending_retune_fails(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    dsd_trunk_tuning_requests_reset();
    if (scan_control_init("a,dmr-trunk,461000000,,250,,\n"
                          "b,dmr-trunk,462000000,,250,,\n",
                          &opts, &state, dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;
    test_rc |= expect_control_rc("hold on",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE), 1);

    /* Advance to b while held, with the retune left pending. */
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    g_counting_tune_to_cc_calls = 0;
    trunk_scan_test_set_now(1.0);
    test_rc |= expect_control_rc("advance pending",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_ADVANCE), 0);
    test_rc |= expect_active_target(&state, "advance pending", 1U);
    const uint64_t pending_id = g_counting_tune_to_cc_last_request_id;
    if (g_counting_tune_to_cc_calls != 1 || pending_id == 0U) {
        DSD_FPRINTF(stderr, "held advance did not leave a pending retune calls=%d id=%llu\n",
                    g_counting_tune_to_cc_calls, (unsigned long long)pending_id);
        test_rc = 1;
    }
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

    dsd_trunk_tuning_request_publish(pending_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    trunk_scan_test_set_now(1.1);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "pending failure while held", 1U);
    if (g_counting_tune_to_cc_calls != 1) {
        DSD_FPRINTF(stderr, "pending failure while held retuned early calls=%d\n", g_counting_tune_to_cc_calls);
        test_rc = 1;
    }
    /* After the cooldown the held target is retried in place. */
    trunk_scan_test_set_now(3.2);
    dsd_engine_trunk_scan_tick(&opts, &state);
    test_rc |= expect_active_target(&state, "retry while held", 1U);
    if (g_counting_tune_to_cc_calls != 2 || g_counting_tune_to_cc_freq != 462000000L) {
        DSD_FPRINTF(stderr, "held target was not retried calls=%d freq=%ld\n", g_counting_tune_to_cc_calls,
                    g_counting_tune_to_cc_freq);
        test_rc = 1;
    }
    test_rc |= expect_scan_control_publication(&state, "retry while held", 1U, 0U, 0U);

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    dsd_trunk_tuning_requests_reset();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_scan_shutdown_clears_hold_and_avoid_publication(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (scan_control_init("a,p25-trunk,851000000,,250,,\n"
                          "b,p25-trunk,852000000,,250,,\n",
                          &opts, &state, dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;
    test_rc |= expect_control_rc("avoid",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE), 0);
    test_rc |=
        expect_control_rc("hold", dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE), 1);
    test_rc |= expect_scan_control_publication(&state, "before shutdown", 1U, 0U, 1U);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    test_rc |= expect_scan_control_publication(&state, "shutdown", 0U, 0U, 0U);
    test_rc |= expect_control_rc("after shutdown",
                                 dsd_engine_trunk_scan_control(&opts, &state, DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE),
                                 DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_parser_accepts_target_key_columns(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] =
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv,keys_dec_csv\n";
    if (write_targets_file_with_header(dir, header,
                                       "a,p25-trunk,851000000,,250,,primary,hexkeys.csv,deckeys.csv\n"
                                       "b,dmr-trunk,452000000,,250,,tier iii,,\n"
                                       "c,nxdn-conventional,461000000,,250,,conv,hexkeys.csv,\n"
                                       "d,nxdn-trunk,461037500,,250,,type-c,,deckeys.csv\n"
                                       "e,dmr-conventional,461112500,,250,,plant,hexkeys.csv,deckeys.csv\n"
                                       "f,nxdn48-conventional,461556250,,250,,narrow,hexkeys.csv,\n",
                                       target_path, sizeof target_path)
        != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.trunk_scan_idle_dwell_ms = 3000;
    opts.trunk_scan_activity_hold_ms = 1200;
    dsd_trunk_scan_target_list list;
    DSD_MEMSET(&list, 0, sizeof list);
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);

    int test_rc = 0;
    char want_hex[DSD_TEST_PATH_MAX] = {0};
    char want_dec[DSD_TEST_PATH_MAX] = {0};
    if (rc != 0 || list.count != 6 || dsd_test_path_join(want_hex, sizeof want_hex, dir, "hexkeys.csv") != 0
        || dsd_test_path_join(want_dec, sizeof want_dec, dir, "deckeys.csv") != 0) {
        DSD_FPRINTF(stderr, "target keys parser rc=%d count=%zu err=%s\n", rc, list.count, err);
        test_rc = 1;
    }
    if (test_rc == 0) {
        if (strcmp(list.targets[0].keys_hex_csv, want_hex) != 0
            || strcmp(list.targets[0].keys_dec_csv, want_dec) != 0) {
            DSD_FPRINTF(stderr, "keyed target paths mismatch hex='%s' dec='%s'\n", list.targets[0].keys_hex_csv,
                        list.targets[0].keys_dec_csv);
            test_rc = 1;
        }
        if (list.targets[1].keys_hex_csv[0] != '\0' || list.targets[1].keys_dec_csv[0] != '\0') {
            DSD_FPRINTF(stderr, "unkeyed target carries key paths\n");
            test_rc = 1;
        }
        // No type gating: every target type may carry keys, conventional ones included.
        if (strcmp(list.targets[2].keys_hex_csv, want_hex) != 0 || list.targets[2].keys_dec_csv[0] != '\0') {
            DSD_FPRINTF(stderr, "conventional target key paths mismatch\n");
            test_rc = 1;
        }
        if (list.targets[3].keys_hex_csv[0] != '\0' || strcmp(list.targets[3].keys_dec_csv, want_dec) != 0) {
            DSD_FPRINTF(stderr, "nxdn-trunk target key paths mismatch\n");
            test_rc = 1;
        }
        if (strcmp(list.targets[4].keys_hex_csv, want_hex) != 0
            || strcmp(list.targets[4].keys_dec_csv, want_dec) != 0) {
            DSD_FPRINTF(stderr, "dmr-conventional target key paths mismatch\n");
            test_rc = 1;
        }
        if (strcmp(list.targets[5].keys_hex_csv, want_hex) != 0 || list.targets[5].keys_dec_csv[0] != '\0') {
            DSD_FPRINTF(stderr, "nxdn48-conventional target key paths mismatch\n");
            test_rc = 1;
        }
    }

    dsd_trunk_scan_target_list_reset(&list);
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_parser_accepts_direct_target_key_columns(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] =
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,single_key_dec,single_key_hex\n";
    if (write_targets_file_with_header(dir, header,
                                       "a,p25-trunk,851000000,,250,,primary,7,00112233445566778899AABBCCDDEEFF\n"
                                       "b,dmr-trunk,452000000,,250,,plain,,\n"
                                       "c,nxdn-conventional,461000000,,250,,zero,0,0000000000\n",
                                       target_path, sizeof target_path)
        != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    dsd_trunk_scan_target_list list;
    DSD_MEMSET(&list, 0, sizeof list);
    char err[256] = {0};
    const int load_rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);
    int test_rc = 0;
    if (load_rc != 0 || list.count != 3U) {
        DSD_FPRINTF(stderr, "direct target parser rc=%d count=%zu err=%s\n", load_rc, list.count, err);
        test_rc = 1;
    } else {
        if (list.targets[0].single_keys_present != 1U || list.targets[0].single_key_scalars.K != 7ULL
            || list.targets[0].single_key_scalars.K1 != 0x0011223344556677ULL
            || list.targets[0].single_key_scalars.K2 != 0x8899AABBCCDDEEFFULL
            || list.targets[0].single_key_scalars.aes_key[15] != 0xFFU) {
            DSD_FPRINTF(stderr, "direct target parser lost combined key fields\n");
            test_rc = 1;
        }
        if (list.targets[1].single_keys_present != 0U) {
            DSD_FPRINTF(stderr, "blank direct target fields stored a key set\n");
            test_rc = 1;
        }
        if (list.targets[2].single_keys_present != 1U || list.targets[2].single_key_scalars.K != 0ULL
            || list.targets[2].single_key_scalars.K1 != 0ULL) {
            DSD_FPRINTF(stderr, "explicit zero direct target key was not preserved\n");
            test_rc = 1;
        }
    }

    dsd_trunk_scan_target_list_reset(&list);

    static const char duplicate_header[] =
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,single_key_dec,single_key_dec\n";
    if (write_targets_file_with_header(dir, duplicate_header, "a,p25-trunk,851000000,,250,,dup,1,2\n", target_path,
                                       sizeof target_path)
            != 0
        || dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err) == 0) {
        DSD_FPRINTF(stderr, "duplicate direct target key header was accepted\n");
        test_rc = 1;
        dsd_trunk_scan_target_list_reset(&list);
    }

    static const char conflict_header[] =
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv,single_key_dec\n";
    static const char conflict_value[] = "241";
    if (write_targets_file_with_header(dir, conflict_header, "a,p25-trunk,851000000,,250,,mixed,private-keys.csv,241\n",
                                       target_path, sizeof target_path)
            != 0
        || dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err) == 0) {
        DSD_FPRINTF(stderr, "mixed direct/file target key sources were accepted\n");
        test_rc = 1;
        dsd_trunk_scan_target_list_reset(&list);
    } else if (strstr(err, conflict_value) != NULL || strstr(err, "private-keys.csv") != NULL) {
        DSD_FPRINTF(stderr, "mixed target key diagnostic exposed a supplied value\n");
        test_rc = 1;
    }

    static const char invalid_header[] =
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,single_key_hex\n";
    static const char invalid_value[] = "private-invalid-key";
    if (write_targets_file_with_header(dir, invalid_header, "a,p25-trunk,851000000,,250,,invalid,private-invalid-key\n",
                                       target_path, sizeof target_path)
            != 0
        || dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err) == 0) {
        DSD_FPRINTF(stderr, "invalid direct target key was accepted\n");
        test_rc = 1;
        dsd_trunk_scan_target_list_reset(&list);
    } else if (strstr(err, invalid_value) != NULL || strstr(err, "single_key_hex") == NULL) {
        DSD_FPRINTF(stderr, "invalid direct target diagnostic was unsafe or imprecise: %s\n", err);
        test_rc = 1;
    }

    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_parser_rejects_duplicate_target_key_columns(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] =
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv,keys_dec_csv,keys_hex_csv\n";
    if (write_targets_file_with_header(dir, header,
                                       "a,p25-trunk,851000000,,250,,dup,hexkeys.csv,deckeys.csv,hexkeys.csv\n",
                                       target_path, sizeof target_path)
        != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.trunk_scan_idle_dwell_ms = 3000;
    opts.trunk_scan_activity_hold_ms = 1200;
    dsd_trunk_scan_target_list list;
    DSD_MEMSET(&list, 0, sizeof list);
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);
    int test_rc = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "duplicate key header accepted\n");
        test_rc = 1;
    }
    if (list.count != 0) {
        DSD_FPRINTF(stderr, "duplicate key header left %zu targets\n", list.count);
        test_rc = 1;
    }

    dsd_trunk_scan_target_list_reset(&list);
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_target_keys_install_and_restore_across_switches(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char hex_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(hex_path, sizeof hex_path, dir, "hexkeys.csv") != 0
        || write_text_file(hex_path, "key id(hex),key value (hex)\n0010,AAAAAAAAAAAAAAAA\n") != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv\n";
    if (write_targets_file_with_header(dir, header,
                                       "a,p25-trunk,851000000,,250,,keyed,hexkeys.csv\n"
                                       "b,dmr-conventional,461000000,,250,,plain,\n",
                                       target_path, sizeof target_path)
        != 0) {
        (void)remove(hex_path);
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    state.keyloader = 0;
    state.K = 0xBEEFULL;
    state.rkey_array[3] = 111ULL;
    state.rkey_array_loaded[3] = 1U;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || state.rkey_array[9] != 999ULL
        || state.rkey_array_loaded[9] != 1U || state.keyloader != 1 || state.K != 0ULL) {
        DSD_FPRINTF(stderr, "keyed target init mismatch rc=%d active=%zu loaded=%u keyloader=%d err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), state.rkey_array_loaded[9], state.keyloader, err);
        test_rc = 1;
    }
    const uint64_t epoch0 = state.enc_lockout_key_epoch;

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1 || state.rkey_array[9] != 0ULL || state.keyloader != 0
        || state.K != 0xBEEFULL || state.rkey_array[3] != 111ULL || state.enc_lockout_key_epoch != epoch0) {
        DSD_FPRINTF(stderr, "unkeyed target did not restore globals active=%zu\n",
                    dsd_engine_trunk_scan_active_index(&state));
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.rkey_array[9] != 999ULL || state.keyloader != 1
        || state.enc_lockout_key_epoch != epoch0) {
        DSD_FPRINTF(stderr, "keyed target did not reinstall active=%zu\n", dsd_engine_trunk_scan_active_index(&state));
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    if (state.rkey_array[9] != 0ULL || state.keyloader != 0 || state.K != 0xBEEFULL || state.rkey_array[3] != 111ULL) {
        DSD_FPRINTF(stderr, "shutdown did not restore globals\n");
        test_rc = 1;
    }

    trunk_scan_test_clear_now();
    (void)remove(hex_path);
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_direct_target_keys_install_and_restore_across_switches(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] =
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,single_key_dec,single_key_hex\n";
    if (write_targets_file_with_header(dir, header,
                                       "a,dmr-conventional,461000000,,250,,first,7,00112233445566778899AABBCCDDEEFF\n"
                                       "b,dmr-conventional,461000001,,250,,global,,\n"
                                       "c,dmr-conventional,461000002,,250,,second,9,0123456789\n",
                                       target_path, sizeof target_path)
        != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;
    state.K = 0xBEEFULL;
    state.K1 = 0x1111111111111111ULL;
    state.H = state.K1;
    state.aes_key[0] = 0xCCU;
    state.rkey_array[3] = 111ULL;
    state.rkey_array_loaded[3] = 1U;
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    const int init_rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (init_rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0U || state.K != 7ULL
        || state.K1 != 0x0011223344556677ULL || state.K2 != 0x8899AABBCCDDEEFFULL || state.keyloader != 0
        || state.aes_key[15] != 0xFFU || state.rkey_array[3] != 0ULL) {
        DSD_FPRINTF(stderr, "direct target init mismatch rc=%d active=%zu err=%s\n", init_rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }
    if (opts.dmr_mute_encL != 1 || opts.dmr_mute_encR != 1) {
        DSD_FPRINTF(stderr, "direct target changed encrypted-audio mute preferences\n");
        test_rc = 1;
    }
    for (size_t i = 0U; i < 3U; i++) {
        if (!trunk_scan_test_target_embedded_keys_cleared(&state, i)) {
            DSD_FPRINTF(stderr, "runtime target %zu retained embedded direct key metadata\n", i);
            test_rc = 1;
        }
    }
    const uint64_t epoch0 = state.enc_lockout_key_epoch;

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1U || state.K != 0xBEEFULL || state.K1 != 0x1111111111111111ULL
        || state.aes_key[0] != 0xCCU || state.rkey_array[3] != 111ULL || state.enc_lockout_key_epoch != epoch0) {
        DSD_FPRINTF(stderr, "unkeyed target did not restore direct-key baseline\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 2U || state.K != 9ULL || state.K1 != 0x0123456789ULL
        || state.H != 0x0123456789ULL || state.aes_key[0] != 0U || state.aes_key_segments[0] != 0U
        || state.enc_lockout_key_epoch != epoch0) {
        DSD_FPRINTF(stderr, "second direct target did not install distinct scalar keys\n");
        test_rc = 1;
    }

    trunk_scan_test_set_now(0.78);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0U || state.K != 7ULL || state.K1 != 0x0011223344556677ULL
        || state.enc_lockout_key_epoch != epoch0) {
        DSD_FPRINTF(stderr, "first direct target did not reinstall\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    if (state.K != 0xBEEFULL || state.K1 != 0x1111111111111111ULL || state.aes_key[0] != 0xCCU
        || state.rkey_array[3] != 111ULL || opts.dmr_mute_encL != 1 || opts.dmr_mute_encR != 1) {
        DSD_FPRINTF(stderr, "direct target shutdown did not restore global key state\n");
        test_rc = 1;
    }

    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_target_chan_csv_keys_are_discarded(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char hex_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(hex_path, sizeof hex_path, dir, "hexkeys.csv") != 0
        || write_text_file(hex_path, "key id(hex),key value (hex)\n0010,AAAAAAAAAAAAAAAA\n") != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    char chan_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(chan_path, sizeof chan_path, dir, "chan.csv") != 0
        || write_text_file(chan_path, "channel,frequency,name,keys_hex_csv\n1,851012500,Dispatch,hexkeys.csv\n") != 0) {
        (void)remove(hex_path);
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    if (write_targets_file(dir, "a,p25-trunk,851000000,chan.csv,250,,\n", target_path, sizeof target_path) != 0) {
        (void)remove(hex_path);
        cleanup_paths(dir, NULL, chan_path);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "chan_csv keys scan init failed err=%s\n", err);
        test_rc = 1;
    }
    if (state.trunk_lcn_keys != NULL || dsd_state_trunk_lcn_keys_get(&state, 0U) != NULL) {
        DSD_FPRINTF(stderr, "trunk scan kept a chan_csv key store\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    (void)remove(hex_path);
    cleanup_paths(dir, target_path, chan_path);
    return test_rc;
}

static int
test_target_keys_survive_failed_alternate_retune(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char hex_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(hex_path, sizeof hex_path, dir, "hexkeys.csv") != 0
        || write_text_file(hex_path, "key id(hex),key value (hex)\n0010,AAAAAAAAAAAAAAAA\n") != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv\n";
    if (write_targets_file_with_header(dir, header,
                                       "a,p25-trunk,851000000,,250,,keyed,hexkeys.csv\n"
                                       "b,p25-trunk,852000000,,250,,plain,\n",
                                       target_path, sizeof target_path)
        != 0) {
        (void)remove(hex_path);
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);

    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err);
    int test_rc = 0;
    if (rc != 0 || dsd_engine_trunk_scan_active_index(&state) != 0 || state.rkey_array[9] != 999ULL) {
        DSD_FPRINTF(stderr, "keyed fallback scan init failed rc=%d active=%zu err=%s\n", rc,
                    dsd_engine_trunk_scan_active_index(&state), err);
        test_rc = 1;
    }
    const uint64_t epoch0 = state.enc_lockout_key_epoch;

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = failing_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0 || state.rkey_array[9] != 999ULL || state.keyloader != 1
        || state.enc_lockout_key_epoch != epoch0) {
        DSD_FPRINTF(stderr, "failed alternate retune did not re-park the original keys active=%zu\n",
                    dsd_engine_trunk_scan_active_index(&state));
        test_rc = 1;
    }

    DSD_MEMSET(&hooks, 0, sizeof hooks);
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    if (state.rkey_array[9] != 0ULL || state.keyloader != 0) {
        DSD_FPRINTF(stderr, "fallback shutdown did not restore globals\n");
        test_rc = 1;
    }
    trunk_scan_test_clear_now();
    (void)remove(hex_path);
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

/* --- P25 band plan: peer IDEN sharing, per-target plan column, snapshot, export (#402) --- */

static const unsigned long long k_sys_a_wacn = 0xBEE00ULL;
static const unsigned long long k_sys_a_sysid = 0x3A1ULL;
static const unsigned long long k_sys_b_sysid = 0x3A2ULL;

static void
seed_iden_entry(p25_iden_entry_t* e, long int base_freq, int chan_type, unsigned long long wacn,
                unsigned long long sysid, uint8_t trust) {
    DSD_MEMSET(e, 0, sizeof *e);
    e->base_freq = base_freq;
    e->chan_type = chan_type;
    e->chan_spac = 100;
    e->trans_off = 0x8E;
    e->bw_vu = 0;
    e->trust = trust;
    e->populated = 1;
    e->wacn = wacn;
    e->sysid = sysid;
    e->rfss = 1ULL;
    e->site = 5ULL;
}

static int
expect_iden_entry(const char* stage, const p25_iden_entry_t* e, long int base_freq, unsigned long long wacn,
                  unsigned long long sysid, uint8_t trust) {
    if (e->populated != 1 || e->base_freq != base_freq || e->chan_spac != 100 || e->wacn != wacn || e->sysid != sysid
        || e->rfss != 1ULL || e->site != 5ULL || e->trust != trust) {
        DSD_FPRINTF(stderr,
                    "%s: IDEN entry mismatch populated=%u base=%ld wacn=%llx sysid=%llx rfss=%llu site=%llu "
                    "trust=%u\n",
                    stage, e->populated, e->base_freq, e->wacn, e->sysid, e->rfss, e->site, e->trust);
        return 1;
    }
    return 0;
}

static int
init_two_p25_targets(dsd_opts* opts, dsd_state* state, const char* body, char* dir, size_t dir_sz, char* target_path,
                     size_t target_sz) {
    if (make_runtime_targets(body, target_path, target_sz, dir, dir_sz) != 0) {
        return -1;
    }
    reset_scan_opts_state(opts, state);
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", target_path);
    char err[256] = {0};
    trunk_scan_test_set_now(0.0);
    if (dsd_engine_trunk_scan_init(opts, state, err, sizeof err) != 0
        || dsd_engine_trunk_scan_active_index(state) != 0) {
        DSD_FPRINTF(stderr, "scan init failed err=%s\n", err);
        return -1;
    }
    return 0;
}

static int
test_peer_idens_shared_by_system_identity(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (init_two_p25_targets(&opts, &state,
                             "a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n"
                             "c,p25-trunk,853000000,,250,,\n",
                             dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    int test_rc = 0;

    /* Target a learns three identifiers over the air on system A. */
    state.p2_wacn = k_sys_a_wacn;
    state.p2_sysid = k_sys_a_sysid;
    seed_iden_entry(&state.p25_iden_fdma[1], 170201250, 1, k_sys_a_wacn, k_sys_a_sysid, 2);
    state.p25_chan_tdma_explicit[1] |= 0x01;
    seed_iden_entry(&state.p25_iden_tdma[2], 152401250, 4, k_sys_a_wacn, k_sys_a_sysid, 2);
    state.p25_chan_tdma_explicit[2] |= 0x02;
    seed_iden_entry(&state.p25_iden_fdma[3], 160001250, 1, k_sys_a_wacn, k_sys_a_sysid, 2);
    state.p25_chan_tdma_explicit[3] |= 0x01;
    /* An entry with no provenance at all is never shared. */
    seed_iden_entry(&state.p25_iden_fdma[4], 165001250, 1, 0ULL, 0ULL, 2);
    state.p25_iden_fdma[4].rfss = 0ULL;
    state.p25_iden_fdma[4].site = 0ULL;

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "peer share: scan did not rotate to b\n");
        test_rc = 1;
    }
    /* b has no identity yet: nothing may be copied. */
    test_rc |= expect_empty_target_p25_state(&state);

    /* b already learned IDEN 3 itself before its identity resolved; that stays. */
    seed_iden_entry(&state.p25_iden_fdma[3], 555, 1, k_sys_a_wacn, k_sys_a_sysid, 2);
    state.p25_chan_tdma_explicit[3] |= 0x01;
    state.p2_wacn = k_sys_a_wacn;
    state.p2_sysid = k_sys_a_sysid;
    trunk_scan_test_set_now(0.30);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "peer share: b rotated early\n");
        test_rc = 1;
    }
    test_rc |= expect_iden_entry("b fdma[1]", &state.p25_iden_fdma[1], 170201250, k_sys_a_wacn, k_sys_a_sysid, 1);
    test_rc |= expect_iden_entry("b tdma[2]", &state.p25_iden_tdma[2], 152401250, k_sys_a_wacn, k_sys_a_sysid, 1);
    test_rc |= expect_iden_entry("b fdma[3]", &state.p25_iden_fdma[3], 555, k_sys_a_wacn, k_sys_a_sysid, 2);
    if ((state.p25_chan_tdma_explicit[1] & 0x01) == 0 || (state.p25_chan_tdma_explicit[2] & 0x02) == 0) {
        DSD_FPRINTF(stderr, "peer share: explicit table bits not set (%02x %02x)\n", state.p25_chan_tdma_explicit[1],
                    state.p25_chan_tdma_explicit[2]);
        test_rc = 1;
    }
    if (state.p25_iden_fdma[4].populated != 0 || state.p25_iden_tdma[1].populated != 0
        || state.p25_iden_fdma[2].populated != 0) {
        DSD_FPRINTF(stderr, "peer share: copied an entry it must not (fdma4=%u tdma1=%u fdma2=%u)\n",
                    state.p25_iden_fdma[4].populated, state.p25_iden_tdma[1].populated,
                    state.p25_iden_fdma[2].populated);
        test_rc = 1;
    }

    /* c is a different system on the same WACN: nothing matches. */
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 2) {
        DSD_FPRINTF(stderr, "peer share: scan did not rotate to c\n");
        test_rc = 1;
    }
    state.p2_wacn = k_sys_a_wacn;
    state.p2_sysid = k_sys_b_sysid;
    trunk_scan_test_set_now(0.56);
    dsd_engine_trunk_scan_tick(&opts, &state);
    for (int iden = 0; iden < 16; iden++) {
        if (state.p25_iden_fdma[iden].populated != 0 || state.p25_iden_tdma[iden].populated != 0) {
            DSD_FPRINTF(stderr, "peer share: IDEN %d copied onto a different system\n", iden);
            test_rc = 1;
        }
    }

    /* Back on a: its own confirmed entries are untouched. */
    trunk_scan_test_set_now(0.78);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "peer share: scan did not rotate back to a\n");
        test_rc = 1;
    }
    test_rc |= expect_iden_entry("a fdma[1]", &state.p25_iden_fdma[1], 170201250, k_sys_a_wacn, k_sys_a_sysid, 2);
    test_rc |= expect_iden_entry("a tdma[2]", &state.p25_iden_tdma[2], 152401250, k_sys_a_wacn, k_sys_a_sysid, 2);
    test_rc |= expect_iden_entry("a fdma[3]", &state.p25_iden_fdma[3], 160001250, k_sys_a_wacn, k_sys_a_sysid, 2);
    if (state.p2_wacn != k_sys_a_wacn || state.p2_sysid != k_sys_a_sysid) {
        DSD_FPRINTF(stderr, "peer share: a lost its identity\n");
        test_rc = 1;
    }

    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static const char k_bandplan_header[] = "iden,base_hz,spacing_hz,type,tx_offset_hz,bandwidth_hz,wacn,sysid\n";
static const char k_bandplan_rows[] = "0,851006250,6250,1,-45000000,12500,,\n"
                                      "2,762006250,6250,3,-30000000,,BEE00,3A1\n";

static int
write_bandplan_file(const char* dir, const char* leaf, char* out_path, size_t out_sz) {
    if (dsd_test_path_join(out_path, out_sz, dir, leaf) != 0) {
        return -1;
    }
    char content[512];
    int n = DSD_SNPRINTF(content, sizeof content, "%s%s", k_bandplan_header, k_bandplan_rows);
    if (n < 0 || (size_t)n >= sizeof content) {
        return -1;
    }
    return write_text_file(out_path, content);
}

static int
test_parser_accepts_p25_bandplan_csv_column(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,p25_bandplan_csv\n";
    if (write_targets_file_with_header(dir, header,
                                       "a,p25-trunk,851000000,,250,,primary,plan.csv\n"
                                       "b,dmr-trunk,452000000,,250,,tier iii,\n",
                                       target_path, sizeof target_path)
        != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.trunk_scan_idle_dwell_ms = 3000;
    opts.trunk_scan_activity_hold_ms = 1200;
    dsd_trunk_scan_target_list list;
    DSD_MEMSET(&list, 0, sizeof list);
    char err[256] = {0};
    int rc = dsd_trunk_scan_load_targets_csv(target_path, &opts, &list, err, sizeof err);

    int test_rc = 0;
    char want[DSD_TEST_PATH_MAX] = {0};
    if (rc != 0 || list.count != 2 || dsd_test_path_join(want, sizeof want, dir, "plan.csv") != 0) {
        DSD_FPRINTF(stderr, "bandplan column parser rc=%d count=%zu err=%s\n", rc, list.count, err);
        test_rc = 1;
    }
    if (test_rc == 0) {
        if (strcmp(list.targets[0].p25_bandplan_csv, want) != 0) {
            DSD_FPRINTF(stderr, "bandplan column path mismatch '%s' want '%s'\n", list.targets[0].p25_bandplan_csv,
                        want);
            test_rc = 1;
        }
        if (list.targets[1].p25_bandplan_csv[0] != '\0') {
            DSD_FPRINTF(stderr, "target without a band plan carries a path\n");
            test_rc = 1;
        }
    }
    dsd_trunk_scan_target_list_reset(&list);
    cleanup_paths(dir, target_path, NULL);

    test_rc |= expect_parser_rejects_with_header("conventional target with p25_bandplan_csv", header,
                                                 "a,dmr-conventional,461000000,,250,,plant,plan.csv\n");
    test_rc |= expect_parser_rejects_with_header(
        "duplicate p25_bandplan_csv header",
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,p25_bandplan_csv,p25_bandplan_csv\n",
        "a,p25-trunk,851000000,,250,,dup,plan.csv,plan.csv\n");
    return test_rc;
}

static int
test_trunk_scan_rejects_global_p25_bandplan(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    if (make_runtime_targets("a,p25-trunk,851000000,,250,,\n", target_path, sizeof target_path, dir, sizeof dir) != 0) {
        return 1;
    }
    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    DSD_SNPRINTF(opts.p25_bandplan_in_file, sizeof opts.p25_bandplan_in_file, "%s", target_path);

    char err[256] = {0};
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) == 0) {
        DSD_FPRINTF(stderr, "global P25 band plan accepted under trunk scan\n");
        dsd_engine_trunk_scan_shutdown(&opts, &state);
        test_rc = 1;
    } else if (strstr(err, "band plan") == NULL) {
        DSD_FPRINTF(stderr, "global P25 band plan rejected for the wrong reason: %s\n", err);
        test_rc = 1;
    }
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
test_target_p25_bandplan_loads_and_survives_rotation(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof dir) != 0) {
        return 1;
    }
    char plan_path[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static const char header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,p25_bandplan_csv\n";
    if (write_bandplan_file(dir, "plan.csv", plan_path, sizeof plan_path) != 0
        || write_targets_file_with_header(dir, header,
                                          "a,p25-trunk,851000000,,250,,primary,plan.csv\n"
                                          "b,p25-trunk,852000000,,250,,secondary,\n",
                                          target_path, sizeof target_path)
               != 0) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    char err[512] = {0};
    trunk_scan_test_set_now(0.0);
    int test_rc = 0;
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) != 0
        || dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "band plan target init failed err=%s\n", err);
        (void)remove(plan_path);
        cleanup_paths(dir, target_path, NULL);
        return 1;
    }
    /* a: the plan is stored and its global row already seeds IDEN 0. */
    if (state.p25_bandplan_row_count != 2 || state.p25_iden_fdma[0].populated != 1 || state.p25_iden_fdma[0].trust != 1
        || (state.p25_chan_tdma_explicit[0] & 0x01) == 0) {
        DSD_FPRINTF(stderr, "band plan target a: rows=%d fdma0 populated=%u trust=%u explicit=%02x\n",
                    state.p25_bandplan_row_count, state.p25_iden_fdma[0].populated, state.p25_iden_fdma[0].trust,
                    state.p25_chan_tdma_explicit[0]);
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "band plan target: scan did not rotate to b\n");
        test_rc = 1;
    }
    if (state.p25_bandplan_row_count != 0 || state.p25_iden_fdma[0].populated != 0) {
        DSD_FPRINTF(stderr, "band plan target a leaked into b: rows=%d fdma0=%u\n", state.p25_bandplan_row_count,
                    state.p25_iden_fdma[0].populated);
        test_rc = 1;
    }
    trunk_scan_test_set_now(0.52);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 0) {
        DSD_FPRINTF(stderr, "band plan target: scan did not rotate back to a\n");
        test_rc = 1;
    }
    if (state.p25_bandplan_row_count != 2 || state.p25_bandplan_rows[1].entry.wacn != k_sys_a_wacn
        || state.p25_iden_fdma[0].populated != 1) {
        DSD_FPRINTF(stderr, "band plan target a did not survive rotation: rows=%d fdma0=%u\n",
                    state.p25_bandplan_row_count, state.p25_iden_fdma[0].populated);
        test_rc = 1;
    }
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    trunk_scan_test_clear_now();

    /* A plan that cannot be read fails init with a message naming the target. */
    (void)remove(plan_path);
    reset_scan_opts_state(&opts, &state);
    DSD_SNPRINTF(opts.trunk_scan_targets_csv, sizeof opts.trunk_scan_targets_csv, "%s", target_path);
    DSD_MEMSET(err, 0, sizeof err);
    if (dsd_engine_trunk_scan_init(&opts, &state, err, sizeof err) == 0) {
        DSD_FPRINTF(stderr, "missing per-target band plan accepted\n");
        dsd_engine_trunk_scan_shutdown(&opts, &state);
        test_rc = 1;
    } else if (strstr(err, "p25_bandplan_csv") == NULL || strstr(err, "'a'") == NULL) {
        DSD_FPRINTF(stderr, "missing per-target band plan error does not name the target: %s\n", err);
        test_rc = 1;
    }
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
count_bandplan_rows_for(const dsd_state* state, unsigned long long wacn, unsigned long long sysid) {
    int n = 0;
    for (int i = 0; i < state->p25_bandplan_row_count; i++) {
        if (state->p25_bandplan_rows[i].entry.wacn == wacn && state->p25_bandplan_rows[i].entry.sysid == sysid) {
            n++;
        }
    }
    return n;
}

static int
test_p25_bandplan_export_collects_every_target(void) {
    char dir[DSD_TEST_PATH_MAX];
    char target_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    if (init_two_p25_targets(&opts, &state,
                             "a,p25-trunk,851000000,,250,,\n"
                             "b,p25-trunk,852000000,,250,,\n",
                             dir, sizeof dir, target_path, sizeof target_path)
        != 0) {
        return 1;
    }
    char out_path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(out_path, sizeof out_path, "dsdneo_bandplan_export");
    if (fd < 0) {
        dsd_engine_trunk_scan_shutdown(&opts, &state);
        cleanup_paths(dir, target_path, NULL);
        return 1;
    }
    (void)dsd_close(fd);
    int test_rc = 0;

    /* Nothing learned anywhere: nothing to write. */
    if (dsd_engine_p25_bandplan_export(&opts, &state, out_path) != -1) {
        DSD_FPRINTF(stderr, "export with no ready entries did not fail\n");
        test_rc = 1;
    }

    state.p2_wacn = k_sys_a_wacn;
    state.p2_sysid = k_sys_a_sysid;
    seed_iden_entry(&state.p25_iden_fdma[1], 170201250, 1, k_sys_a_wacn, k_sys_a_sysid, 2);
    /* Incomplete entries are not rows. */
    seed_iden_entry(&state.p25_iden_fdma[5], 0, 1, k_sys_a_wacn, k_sys_a_sysid, 2);

    trunk_scan_test_set_now(0.26);
    dsd_engine_trunk_scan_tick(&opts, &state);
    if (dsd_engine_trunk_scan_active_index(&state) != 1) {
        DSD_FPRINTF(stderr, "export: scan did not rotate to b\n");
        test_rc = 1;
    }
    state.p2_wacn = k_sys_a_wacn;
    state.p2_sysid = k_sys_b_sysid;
    seed_iden_entry(&state.p25_iden_fdma[1], 851006250 / 5, 1, k_sys_a_wacn, k_sys_b_sysid, 2);
    seed_iden_entry(&state.p25_iden_tdma[2], 762006250 / 5, 4, k_sys_a_wacn, k_sys_b_sysid, 1);

    /* b is live, a is parked: both systems come out. */
    int rows = dsd_engine_p25_bandplan_export(&opts, &state, out_path);
    if (rows != 3) {
        DSD_FPRINTF(stderr, "export under trunk scan wrote %d rows, want 3\n", rows);
        test_rc = 1;
    }
    static dsd_state scratch;
    DSD_MEMSET(&scratch, 0, sizeof scratch);
    if (csvP25BandplanImportPath(out_path, &scratch) != 0 || scratch.p25_bandplan_row_count != 3
        || count_bandplan_rows_for(&scratch, k_sys_a_wacn, k_sys_a_sysid) != 1
        || count_bandplan_rows_for(&scratch, k_sys_a_wacn, k_sys_b_sysid) != 2) {
        DSD_FPRINTF(stderr, "re-import of the export: rows=%d A=%d B=%d\n", scratch.p25_bandplan_row_count,
                    count_bandplan_rows_for(&scratch, k_sys_a_wacn, k_sys_a_sysid),
                    count_bandplan_rows_for(&scratch, k_sys_a_wacn, k_sys_b_sysid));
        test_rc = 1;
    }

    /* Without the coordinator only the live tables are written. */
    dsd_engine_trunk_scan_shutdown(&opts, &state);
    rows = dsd_engine_p25_bandplan_export(&opts, &state, out_path);
    if (rows != 2) {
        DSD_FPRINTF(stderr, "export without trunk scan wrote %d rows, want 2\n", rows);
        test_rc = 1;
    }
    DSD_MEMSET(&scratch, 0, sizeof scratch);
    if (csvP25BandplanImportPath(out_path, &scratch) != 0 || scratch.p25_bandplan_row_count != 2
        || count_bandplan_rows_for(&scratch, k_sys_a_wacn, k_sys_b_sysid) != 2) {
        DSD_FPRINTF(stderr, "re-import of the live-only export: rows=%d\n", scratch.p25_bandplan_row_count);
        test_rc = 1;
    }

    trunk_scan_test_clear_now();
    (void)remove(out_path);
    cleanup_paths(dir, target_path, NULL);
    return test_rc;
}

static int
run_with_default_tune_hook(int (*test_fn)(void)) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = counting_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_trunk_tuning_requests_reset();
    g_counting_tune_to_cc_failures_remaining = 0;
    g_counting_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

    int rc = test_fn();

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    return rc;
}

void dsd_key_set_test_alloc_fail_after(long count);

static int
test_initial_key_allocation_failure(void) {
    char dir[DSD_TEST_PATH_MAX];
    char path[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof(dir))) {
        return 1;
    }
    if (write_targets_file_with_header(dir, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n",
                                       "a,dmr-conventional,461000000,,250,,,-b 1 -4\n", path, sizeof(path))) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        cleanup_paths(dir, path, NULL);
        return 1;
    }
    reset_scan_opts_state(opts, state);
    state->R = 99;
    state->rkey_array_loaded[7] = 1;
    state->rkey_array[7] = 123;
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof(opts->trunk_scan_targets_csv), "%s", path);
    dsd_key_set_test_alloc_fail_after(0);
    char error[256];
    int rc = dsd_engine_trunk_scan_init(opts, state, error, sizeof(error)) == 0;
    dsd_key_set_test_alloc_fail_after(-1);
    rc |= dsd_engine_trunk_scan_active_index(state) != (size_t)-1;
    rc |= state->R != 99 || state->rkey_array[7] != 123 || !state->rkey_array_loaded[7];
    rc |= state->scan_keys_active_set || state->M != 0;
    if (rc) {
        DSD_FPRINTF(stderr, "initial key allocation failure did not preserve the baseline and reject startup\n");
    }
    dsd_engine_trunk_scan_shutdown(opts, state);
    dsd_state_ext_free_all(state);
    dsd_state_trunk_lcn_free(state);
    free(state);
    free(opts);
    cleanup_paths(dir, path, NULL);
    return rc;
}

static int
test_target_options_rotate_and_restore(void) {
    char dir[DSD_TEST_PATH_MAX];
    char path[DSD_TEST_PATH_MAX];
    if (make_temp_dir(dir, sizeof(dir))) {
        return 1;
    }
    const char* header = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,OPTIONS\n";
    if (write_targets_file_with_header(
            dir, header,
            "a,dmr-conventional,461000000,,250,,rc4,-1 0123456789 -0 -F --scan-voice-hold-ms 4000\n"
            "b,dmr-conventional,461000001,,250,,hytera,-H 0000001f00 -4\n"
            "c,dmr-conventional,461000002,,250,,mixed,-b 1 --no-force-key --strict-crc\n"
            "d,nxdn48-conventional,461000003,,250,,scrambler,-R 1 --no-force-key\n",
            path, sizeof(path))) {
        cleanup_paths(dir, NULL, NULL);
        return 1;
    }
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        cleanup_paths(dir, path, NULL);
        return 1;
    }
    reset_scan_opts_state(opts, state);
    state->R = 999;
    state->M = 1;
    opts->aggressive_framesync = 1;
    opts->scan_voice_hold_ms = 2000;
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof(opts->trunk_scan_targets_csv), "%s", path);
    char error[256] = {0};
    trunk_scan_test_set_now(0.0);
    int rc = dsd_engine_trunk_scan_init(opts, state, error, sizeof(error));
    int failed = rc != 0;
    if (rc == 0) {
        failed |= state->R != 0x123456789ULL || state->RR != state->R || state->M != 0x21
                  || opts->aggressive_framesync != 0 || opts->scan_voice_hold_ms != 4000;
        trunk_scan_test_set_now(0.26);
        dsd_engine_trunk_scan_tick(opts, state);
        failed |= state->R != 0 || state->K1 != 0x1f00 || state->M != 1 || opts->scan_voice_hold_ms != 2000;
        trunk_scan_test_set_now(0.52);
        dsd_engine_trunk_scan_tick(opts, state);
        failed |= state->K != 1 || state->K1 != 0 || state->M != 0 || !opts->aggressive_framesync;
        trunk_scan_test_set_now(0.78);
        dsd_engine_trunk_scan_tick(opts, state);
        failed |= state->R != 1 || state->RR != 0 || state->K != 0 || state->M != 0 || !opts->frame_nxdn48;
        dsd_engine_trunk_scan_shutdown(opts, state);
        failed |= state->R != 999 || state->M != 1 || opts->scan_voice_hold_ms != 2000;
    }
    if (failed) {
        DSD_FPRINTF(stderr, "target options regression: %s\n", error);
    }
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    trunk_scan_test_clear_now();
    cleanup_paths(dir, path, NULL);
    failed |= expect_parser_rejects_with_header("options-conflict", header, "a,dmr-conventional,461000000,,,,,-4 -0\n");
    failed |= expect_parser_rejects_with_header("options-trunk-voice", header,
                                                "a,p25-trunk,851000000,,,,,--scan-voice-only\n");
    failed |= expect_parser_rejects_with_header(
        "options-duplicate-header",
        "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options,relevant_CLI_switches\n",
        "a,dmr-conventional,461000000,,,,,-b 1,-F\n");
    return failed;
}

int
main(void) {
    int rc = 0;
    rc |= run_with_default_tune_hook(test_parser_valid_mixed_targets_and_relative_chan_csv);
    rc |= run_with_default_tune_hook(test_parser_accepts_quoted_chan_csv_with_comma);
    rc |= run_with_default_tune_hook(test_parser_accepts_optional_modulation_and_gain_columns);
    rc |= run_with_default_tune_hook(test_parser_accepts_target_key_columns);
    rc |= run_with_default_tune_hook(test_parser_accepts_direct_target_key_columns);
    rc |= run_with_default_tune_hook(test_parser_rejects_duplicate_target_key_columns);
    rc |= run_with_default_tune_hook(test_parser_accepts_nxdn_targets);
    rc |= run_with_default_tune_hook(test_parser_rejects_invalid_inputs);
    rc |= run_with_default_tune_hook(test_parser_accepts_100_targets);
    rc |= run_with_default_tune_hook(test_coordinator_idle_rotation_and_state_restore);
    rc |= run_with_default_tune_hook(test_coordinator_publishes_active_target_label);
    rc |= run_with_default_tune_hook(test_target_chan_csv_names_are_discarded);
    rc |= run_with_default_tune_hook(test_coordinator_rotation_past_32_targets);
    rc |= run_with_default_tune_hook(test_coordinator_preserves_long_lcn_list_across_rotation);
    rc |= run_with_default_tune_hook(test_target_keys_install_and_restore_across_switches);
    rc |= run_with_default_tune_hook(test_direct_target_keys_install_and_restore_across_switches);
    rc |= run_with_default_tune_hook(test_initial_key_allocation_failure);
    rc |= run_with_default_tune_hook(test_target_options_rotate_and_restore);
    rc |= run_with_default_tune_hook(test_target_chan_csv_keys_are_discarded);
    rc |= run_with_default_tune_hook(test_target_keys_survive_failed_alternate_retune);
    rc |= run_with_default_tune_hook(test_coordinator_preserves_large_chan_map_across_rotation);
    rc |= test_targets_csv_rejects_rows_past_the_memory_budget();
    rc |= run_with_default_tune_hook(test_call_identity_state_isolated_per_target);
    rc |= run_with_default_tune_hook(test_call_event_lifecycle_isolated_per_target);
    rc |= run_with_default_tune_hook(test_call_event_current_rows_isolated_per_target);
    rc |= run_with_default_tune_hook(test_dmr_branding_state_isolated_per_target);
    rc |= run_with_default_tune_hook(test_dmr_confidence_state_isolated_per_target);
    rc |= run_with_default_tune_hook(test_dmr_service_options_state_isolated_per_target);
    rc |= run_with_default_tune_hook(test_p25_targets_seed_valid_control_channel_timing);
    rc |= run_with_default_tune_hook(test_p25_nac_state_isolated_per_target);
    rc |= run_with_default_tune_hook(test_p25_target_switch_resyncs_sm_mode);
    rc |= run_with_default_tune_hook(test_p25_scan_retune_restarts_pending_cc_acquisition);
    rc |= run_with_default_tune_hook(test_mixed_target_switch_resets_dmr_demod_profile);
    rc |= run_with_default_tune_hook(test_conventional_activity_hold_and_allowlist_block);
    rc |= run_with_default_tune_hook(test_conventional_activity_encrypted_lockout_does_not_hold);
    rc |= run_with_default_tune_hook(test_nxdn_trunk_target_seeds_control_channel);
    rc |= run_with_default_tune_hook(test_mixed_target_switch_resets_nxdn_demod_profile);
    rc |= run_with_default_tune_hook(test_nxdn48_target_selects_2400_demod_profile);
    rc |= run_with_default_tune_hook(test_nxdn48_target_uses_rtl_output_rate_for_sps);
    rc |= run_with_default_tune_hook(test_target_classes_enable_missing_decoders);
    rc |= run_with_default_tune_hook(test_nxdn_conventional_activity_hold);
    rc |= run_with_default_tune_hook(test_conventional_activity_data_call_respects_tune_data_calls);
    rc |= run_with_default_tune_hook(test_conventional_activity_families_do_not_cross);
    rc |= run_with_default_tune_hook(test_nxdn48_conventional_activity_hold);
    rc |= run_with_default_tune_hook(test_conventional_voice_gate_data_header_no_hold);
    rc |= run_with_default_tune_hook(test_conventional_voice_gate_voice_header_no_hold);
    rc |= run_with_default_tune_hook(test_conventional_voice_gate_media_hold);
    rc |= run_with_default_tune_hook(test_conventional_voice_gate_terminator_before_first_tick);
    rc |= run_with_default_tune_hook(test_conventional_voice_gate_enc_lockout_media_rotates);
    rc |= run_with_default_tune_hook(test_trunked_voice_gate_control_only_unchanged);
    rc |= run_with_default_tune_hook(test_nxdn_trunk_target_holds_while_tuned);
    rc |= run_with_default_tune_hook(test_nxdn_trunk_target_follows_corrected_cc);
    rc |= run_with_default_tune_hook(test_nxdn_state_isolated_per_target);
    rc |= run_with_default_tune_hook(test_nxdn_trunk_diag_isolated_per_target);
    rc |= run_with_default_tune_hook(test_state_ext_cleanup_clears_scan_hooks);
    rc |= run_with_default_tune_hook(test_protocol_hooks_only_expose_matching_target_contexts);
    rc |= run_with_default_tune_hook(test_dmr_trunk_sm_timeout_releases_scan_hold);
    rc |= run_with_default_tune_hook(test_p25_pending_retune_holds_scan_dwell);
    rc |= run_with_default_tune_hook(test_p25_pending_retune_adopts_sm_retry);
    rc |= run_with_default_tune_hook(test_p25_pending_retune_preserves_completed_sm_recovery);
    rc |= run_with_default_tune_hook(test_generic_pending_retune_holds_and_recovers);
    rc |= run_with_default_tune_hook(test_p25_targets_pass_cc_sps_to_retune_paths);
    rc |= run_with_default_tune_hook(test_p25_targets_use_rtl_output_rate_for_retune_sps);
    rc |= run_with_default_tune_hook(test_channel_map_sequence_advances_on_equal_count_target_switches);
    rc |= run_with_default_tune_hook(test_p25_encrypted_call_cache_state_isolated_per_target);
    rc |= run_with_default_tune_hook(test_enc_lockout_purge_clears_scan_snapshots);
    rc |= run_with_default_tune_hook(test_trunk_targets_reuse_restored_control_channel);
    rc |= run_with_default_tune_hook(test_locked_demod_mode_preserved_when_seeding_targets);
    rc |= run_with_default_tune_hook(test_target_retunes_select_four_level_sps_profile);
    rc |= run_with_default_tune_hook(test_per_target_modulation_overrides_global_lock);
    rc |= run_with_default_tune_hook(test_active_p25_cqpsk_request_tracks_target_modulation);
    rc |= run_with_default_tune_hook(test_per_target_rtl_gain_overrides_and_restores_global_default);
    rc |= run_with_default_tune_hook(test_scan_tick_skips_rotation_when_p25_guard_busy);
    rc |= run_with_default_tune_hook(test_scan_control_unavailable_without_coordinator);
    rc |= run_with_default_tune_hook(test_scan_hold_pauses_rotation_and_release_restarts_dwell);
    rc |= run_with_default_tune_hook(test_scan_advance_moves_now_and_keeps_hold);
    rc |= run_with_default_tune_hook(test_scan_avoid_steps_on_and_skips_the_target_thereafter);
    rc |= run_with_default_tune_hook(test_scan_avoid_refuses_the_last_usable_target);
    rc |= run_with_default_tune_hook(test_scan_avoid_falls_back_on_the_original_when_alternates_fail);
    rc |= run_with_default_tune_hook(test_scan_controls_on_a_single_target);
    rc |= run_with_default_tune_hook(test_scan_controls_report_busy_while_p25_guard_held);
    rc |= run_with_default_tune_hook(test_scan_hold_retries_the_held_target_after_a_pending_retune_fails);
    rc |= run_with_default_tune_hook(test_scan_shutdown_clears_hold_and_avoid_publication);
    rc |= run_with_default_tune_hook(test_single_target_retune_failure_retries_after_cooldown);
    rc |= run_with_default_tune_hook(test_retune_failure_cooldown);
    rc |= run_with_default_tune_hook(test_scan_does_not_retune_active_target_while_alternates_cool_down);
    rc |= run_with_default_tune_hook(test_failed_alternate_retune_republishes_restored_target);
    rc |= run_with_default_tune_hook(test_dmr_targets_pass_sps_to_retune_paths);
    rc |= run_with_default_tune_hook(test_init_failure_restores_saved_trunk_opts);
    rc |= run_with_default_tune_hook(test_trunk_scan_rejects_fixed_input_without_tuner);
    rc |= run_with_default_tune_hook(test_trunk_scan_rejects_unopened_rtl_without_rigctl);
    rc |= run_with_default_tune_hook(test_trunk_scan_rejects_iq_replay_input);
    rc |= run_with_default_tune_hook(test_peer_idens_shared_by_system_identity);
    rc |= run_with_default_tune_hook(test_parser_accepts_p25_bandplan_csv_column);
    rc |= run_with_default_tune_hook(test_trunk_scan_rejects_global_p25_bandplan);
    rc |= run_with_default_tune_hook(test_target_p25_bandplan_loads_and_survives_rotation);
    rc |= run_with_default_tune_hook(test_p25_bandplan_export_collects_every_target);
    return rc;
}

void
dsd_engine_reset_no_carrier_state(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

/* Coordinator tests stub DSP; acquisition contents are covered by FRAME_SYNC_INTERNAL_HELPERS. */
void
dsd_frame_sync_reset_acquisition(const dsd_opts* opts, dsd_state* state, int forget) {
    (void)opts;
    (void)forget;
    state->profile_proof_valid = 0;
    state->sps_hunt_counter = 0;
}
