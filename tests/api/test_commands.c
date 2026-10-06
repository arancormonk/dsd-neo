// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The JSON command table against the real command queue:
 *
 * - every DSD_APP_CMD_* in include/dsd-neo/app_control/commands.h (read at run time, so a command added there without
 *   a table entry fails here) has exactly one named entry in the catalog;
 * - with a frontend session open, every command, sent with valid parameters, is admitted by the queue (whose typed
 *   setters refuse ids they do not know), and every command that takes parameters refuses an empty params object;
 * - with no session open, commands are refused.
 */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_internal.h"
#include "json.h"

#ifndef DSD_API_TEST_COMMANDS_H
#error "DSD_API_TEST_COMMANDS_H must name include/dsd-neo/app_control/commands.h"
#endif

enum { MAX_IDS = 512 };

/* Every DSD_APP_CMD_<NAME> = <number> in the header's command enum. */
static size_t
header_command_ids(int* ids, size_t cap) {
    FILE* fp = fopen(DSD_API_TEST_COMMANDS_H, "rb");
    assert(fp != NULL);
    char line[1024];
    size_t n = 0;
    while (fgets(line, sizeof line, fp) != NULL) {
        const char* p = strstr(line, "DSD_APP_CMD_");
        if (p == NULL) {
            continue;
        }
        p += strlen("DSD_APP_CMD_");
        while ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
        if (*p != '=') {
            continue;
        }
        p++;
        while (*p == ' ') {
            p++;
        }
        int value = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = (value * 10) + (*p - '0');
            p++;
            digits++;
        }
        if (digits > 0) {
            assert(n < cap);
            ids[n++] = value;
        }
    }
    (void)fclose(fp);
    return n;
}

/* Valid params for every command that takes any. */
typedef struct {
    const char* name;
    const char* params;
} sample;

static const sample k_samples[] = {
    {"gain_delta", "{\"value\":1}"},
    {"again_delta", "{\"value\":-1}"},
    {"tg_hold_toggle", "{\"value\":1}"},
    {"call_alert_events_set", "{\"value\":255}"},
    {"const_gate_delta", "{\"value\":0.5}"},
    {"spec_size_delta", "{\"value\":1}"},
    {"ppm_delta", "{\"value\":-1}"},
    {"gain_set", "{\"value\":20}"},
    {"again_set", "{\"value\":0}"},
    {"input_warn_db_set", "{\"value\":-40.5}"},
    {"event_log_set", "{\"value\":\"/tmp/dsd-api-test-events.log\"}"},
    {"lockout_slot", "{\"value\":0}"},
    {"skip_slot", "{\"value\":1}"},
    {"wav_static_open", "{\"value\":\"/tmp/a.wav\"}"},
    {"wav_raw_open", "{\"value\":\"/tmp/b.wav\"}"},
    {"dsp_out_set", "{\"value\":\"out.bin\"}"},
    {"symcap_open", "{\"value\":\"/tmp/c.bin\"}"},
    {"symbol_in_open", "{\"value\":\"/tmp/d.bin\"}"},
    {"input_wav_set", "{\"value\":\"/tmp/e.wav\"}"},
    {"input_sym_stream_set", "{\"value\":\"/tmp/f.bin\"}"},
    {"udp_out_cfg", "{\"host\":\"127.0.0.1\",\"port\":23456}"},
    {"tcp_connect_audio_cfg", "{\"host\":\"localhost\",\"port\":7355}"},
    {"rigctl_connect_cfg", "{\"host\":\"localhost\",\"port\":4532}"},
    {"udp_input_cfg", "{\"bind\":\"127.0.0.1\",\"port\":7355}"},
    {"rtl_set_dev", "{\"value\":0}"},
    {"rtl_set_freq", "{\"value\":851375000}"},
    {"rtl_set_gain", "{\"value\":22}"},
    {"rtl_set_ppm", "{\"value\":2}"},
    {"rtl_set_bw", "{\"value\":24}"},
    {"rtl_set_sql_db", "{\"value\":-60}"},
    {"rtl_set_vol_mult", "{\"value\":2}"},
    {"rtl_set_bias_tee", "{\"value\":0}"},
    {"rtltcp_set_autotune", "{\"value\":1}"},
    {"rtl_set_auto_ppm", "{\"value\":1}"},
    {"manual_tune", "{\"value\":851375000}"},
    {"mod_set", "{\"value\":1}"},
    {"decode_mode_set", "{\"value\":2}"},
    {"trunk_set", "{\"value\":1}"},
    {"airspy_set", "{\"key\":\"airspy_lna_gain\",\"value\":\"10\"}"},
    {"rtl_set_sql_setting", "{\"mode\":1,\"margin_db\":10}"},
    {"rigctl_set_mod_bw", "{\"value\":12500}"},
    {"tg_hold_set", "{\"value\":101}"},
    {"hangtime_set", "{\"value\":2.5}"},
    {"slot_pref_set", "{\"value\":2}"},
    {"slots_onoff_set", "{\"value\":3}"},
    {"scan_voice_only_set", "{\"value\":1}"},
    {"scan_voice_qualify_ms_set", "{\"value\":1000}"},
    {"scan_voice_hold_ms_set", "{\"value\":2000}"},
    {"nfm_bandwidth_set", "{\"value\":12500}"},
    {"am_bandwidth_set", "{\"value\":0}"},
    {"tone_filter_set", "{\"mode\":1,\"list\":\"100.0\"}"},
    {"scan_row_edit", "{\"scanner\":2,\"session\":1,\"row\":0,\"field\":1,\"action\":1,\"squelch_db\":-50}"},
    {"pulse_out_set", "{\"value\":\"default\"}"},
    {"pulse_in_set", "{\"value\":\"default\"}"},
    {"input_vol_set", "{\"value\":2}"},
    {"lrrp_set_custom", "{\"value\":\"/tmp/lrrp.txt\"}"},
    {"import_channel_map", "{\"value\":\"/tmp/chan.csv\"}"},
    {"import_group_list", "{\"value\":\"/tmp/group.csv\"}"},
    {"import_keys_dec", "{\"value\":\"/tmp/keys.csv\"}"},
    {"import_keys_hex", "{\"value\":\"/tmp/keys_hex.csv\"}"},
    {"import_p25_bandplan", "{\"value\":\"/tmp/bandplan.csv\"}"},
    {"export_p25_bandplan", "{\"value\":\"/tmp/bandplan_out.csv\"}"},
    {"rr_apply_import", "{\"decode_mode\":2,\"trunking\":true,\"tune_hz\":851375000}"},
    {"rr_account_set", "{\"username\":\"user\",\"app_key\":\"key\"}"},
    {"import_src_list", "{\"value\":\"/tmp/src.csv\"}"},
    {"p25_p2_params_set", "{\"wacn\":\"0xBEE00\",\"sysid\":\"0x2D7\",\"cc\":\"0x2D7\"}"},
    {"tg_listen_set", "{\"id_start\":100,\"id_end\":100,\"listen\":true}"},
    {"tg_listen_set_all", "{\"listen\":false,\"tags\":\"fire\"}"},
    {"tg_row_set", "{\"id_start\":100,\"id_end\":199,\"fields\":9,\"listen\":1,\"name\":\"Fire\",\"policy_context\":"
                   "\"1\",\"policy_generation\":2}"},
    {"tg_row_remove", "{\"id_start\":100,\"id_end\":199,\"policy_context\":1,\"policy_generation\":2}"},
    {"tg_list_export", "{\"path\":\"/tmp/tg.csv\",\"policy_context\":\"1\",\"policy_generation\":2}"},
    {"tg_selection_set", "{\"path\":\"/tmp/sel.csv\",\"count\":1,\"listening\":true,\"policy_context\":\"1\","
                         "\"policy_generation\":2}"},
    {"tg_lockout_persist_set", "{\"value\":1}"},
    {"tg_session_avoid_clear", "{\"value\":\"18446744073709551615\"}"},
    {"key_basic_set", "{\"value\":4095}"},
    {"key_scrambler_set", "{\"value\":1234}"},
    {"key_rc4des_set", "{\"value\":\"0x0123456789ABCDEF\"}"},
    {"key_hytera_set", "{\"H\":1,\"K1\":\"0x1\",\"K2\":\"0x2\",\"K3\":\"0x3\",\"K4\":\"0x4\"}"},
    {"key_aes_set", "{\"K1\":\"0x1\",\"K2\":\"0x2\",\"K3\":\"0x3\",\"K4\":\"0x4\"}"},
    {"key_tyt_ap_set", "{\"value\":\"0123456789ABCDEF0123456789ABCDEF\"}"},
    {"key_retevis_rc2_set", "{\"value\":\"0123456789ABCDEF0123456789ABCDEF\"}"},
    {"key_tyt_ep_set", "{\"value\":\"0123456789ABCDEF0123456789ABCDEF\"}"},
    {"key_ken_scr_set", "{\"value\":\"12345\"}"},
    {"key_anytone_bp_set", "{\"value\":\"ABCD\"}"},
    {"key_xor_set", "{\"value\":\"2:ABCD\"}"},
    {"m17_user_data_set", "{\"value\":\"hello\"}"},
    {"key_direct_set", "{\"key_type\":\"basic\",\"value\":\"1234\"}"},
    {"force_key_set", "{\"value\":1}"},
    {"decryption_apply", "{\"type\":\"basic\",\"value\":\"1234\"}"},
    {"dsp_op", "{\"op\":7,\"a\":1}"},
    {"config_apply", "{\"sections\":{\"trunking\":{\"enabled\":true}}}"},
    {"config_metadata_set", "{\"autosave_enabled\":false}"},
};

static const char*
sample_for(const char* name) {
    for (size_t i = 0; i < sizeof k_samples / sizeof k_samples[0]; i++) {
        if (strcmp(k_samples[i].name, name) == 0) {
            return k_samples[i].params;
        }
    }
    return NULL;
}

/* Run one request through the command table; returns the parsed response (caller frees). */
static dsd_json_node*
execute(const char* name, const char* params) {
    char request[2048];
    DSD_SNPRINTF(request, sizeof request, "{\"id\":1,\"cmd\":\"%s\"%s%s}", name, params ? ",\"params\":" : "",
                 params ? params : "");
    dsd_json_node* req = NULL;
    char err[96] = "";
    if (dsd_json_parse(request, &req, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "bad test request %s: %s\n", request, err);
        assert(0);
    }
    dsd_json_buf out;
    dsd_json_buf_init(&out);
    assert(dsd_api_command_execute(req, &out) == 0);
    assert(out.len > 0U && out.data[out.len - 1U] == '\n');
    out.data[out.len - 1U] = '\0';
    dsd_json_node* resp = NULL;
    assert(dsd_json_parse(out.data, &resp, err, sizeof err) == 0);
    dsd_json_buf_free(&out);
    dsd_json_free(req);
    return resp;
}

static const char*
error_code(const dsd_json_node* resp) {
    return dsd_json_as_str(dsd_json_obj_get(dsd_json_obj_get(resp, "error"), "code"));
}

static int
is_ok(const dsd_json_node* resp) {
    int ok = 0;
    return dsd_json_as_bool(dsd_json_obj_get(resp, "ok"), &ok) == 0 && ok;
}

int
main(void) {
    int ids[MAX_IDS];
    const size_t id_count = header_command_ids(ids, MAX_IDS);
    assert(id_count > 100U);

    dsd_json_buf catalog;
    dsd_json_buf_init(&catalog);
    assert(dsd_api_command_catalog(&catalog) == 0);
    dsd_json_node* root = NULL;
    char err[96] = "";
    assert(dsd_json_parse(catalog.data, &root, err, sizeof err) == 0);
    assert(root->type == DSD_JSON_ARRAY);
    assert((size_t)dsd_api_command_count_total() == root->count);

    /* Every header id appears exactly once; the catalog has nothing else. */
    for (size_t i = 0; i < id_count; i++) {
        int seen = 0;
        for (size_t j = 0; j < root->count; j++) {
            int64_t entry_id = 0;
            assert(dsd_json_as_i64(dsd_json_obj_get(root->items[j], "id"), &entry_id) == 0);
            seen += (entry_id == ids[i]) ? 1 : 0;
        }
        if (seen != 1) {
            DSD_FPRINTF(stderr, "command id %d appears %d times in the API catalog\n", ids[i], seen);
            assert(0);
        }
    }
    assert(root->count == id_count);
    for (size_t i = 0; i < root->count; i++) {
        const char* a = dsd_json_as_str(dsd_json_obj_get(root->items[i], "name"));
        assert(a != NULL && a[0] != '\0');
        for (size_t j = i + 1U; j < root->count; j++) {
            assert(strcmp(a, dsd_json_as_str(dsd_json_obj_get(root->items[j], "name"))) != 0);
        }
    }

    /* No session: nothing is admitted. */
    dsd_json_node* resp = execute("toggle_mute", NULL);
    assert(!is_ok(resp));
    assert(strcmp(error_code(resp), "rejected") == 0);
    dsd_json_free(resp);

    dsd_app_frontend_runtime_start(NULL, NULL);
    for (size_t i = 0; i < root->count; i++) {
        const char* name = dsd_json_as_str(dsd_json_obj_get(root->items[i], "name"));
        const char* doc = dsd_json_as_str(dsd_json_obj_get(root->items[i], "params"));
        const int takes_params = doc != NULL && doc[0] != '\0';
        const char* params = takes_params ? sample_for(name) : NULL;
        if (takes_params && params == NULL) {
            DSD_FPRINTF(stderr, "no test sample for command %s\n", name);
            assert(0);
        }
        resp = execute(name, params);
        if (!is_ok(resp)) {
            DSD_FPRINTF(stderr, "command %s with %s was not admitted (%s)\n", name, params ? params : "no params",
                        error_code(resp) ? error_code(resp) : "?");
            assert(0);
        }
        dsd_json_free(resp);
        /* Every decryption field is optional; every other command with params needs at least one. */
        if (takes_params && strcmp(name, "decryption_apply") != 0) {
            resp = execute(name, "{}");
            if (is_ok(resp) || strcmp(error_code(resp), "invalid_params") != 0) {
                DSD_FPRINTF(stderr, "command %s accepted empty params\n", name);
                assert(0);
            }
            dsd_json_free(resp);
        }
    }

    resp = execute("not_a_command", NULL);
    assert(!is_ok(resp));
    assert(strcmp(error_code(resp), "unknown_command") == 0);
    dsd_json_free(resp);
    dsd_app_frontend_runtime_stop();

    dsd_json_free(root);
    dsd_json_buf_free(&catalog);
    printf("api command table complete (%zu commands)\n", id_count);
    return 0;
}
