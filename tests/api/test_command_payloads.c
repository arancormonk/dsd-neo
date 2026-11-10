// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The JSON command table builds the exact payload each command's handler reads. The table (api_commands.c) is
 * compiled here against stand-ins for the app-control submit functions that keep a copy of the last payload, so each
 * case checks the bytes: ranges and types refused rather than wrapped, strings refused rather than cut, 64-bit values
 * exact, request ids tagged, and a config built from the loader's defaults.
 */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/config.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_internal.h"
#include "json.h"

/*============================================================================
 * Stand-ins for the app-control submit API
 *============================================================================*/

static int g_last_id = 0;
static size_t g_last_n = 0;
static unsigned char* g_last = NULL;
static int g_submits = 0;

static int
capture(int cmd_id, const void* payload, size_t n) {
    free(g_last);
    g_last = NULL;
    g_last_id = cmd_id;
    g_last_n = n;
    if (n > 0U) {
        g_last = (unsigned char*)malloc(n);
        assert(g_last != NULL);
        DSD_MEMCPY(g_last, payload, n);
    }
    g_submits++;
    return DSD_APP_COMMAND_SUBMIT_QUEUED;
}

int
dsd_app_command_submit(int cmd_id, const void* payload, size_t payload_sz) {
    return capture(cmd_id, payload, payload_sz);
}

/* What the queue answers a scan_list_set: QUEUED, or REJECTED as it does a full queue (the session stand-in below is
   always open). */
static int g_scan_list_submit_rc = DSD_APP_COMMAND_SUBMIT_QUEUED;
static uint32_t g_scan_list_requests = 5150U;

int
dsd_app_command_scan_list_set(const dsd_app_scan_list_set_payload* payload) {
    if (g_scan_list_submit_rc != DSD_APP_COMMAND_SUBMIT_QUEUED) {
        return g_scan_list_submit_rc;
    }
    return capture(DSD_APP_CMD_SCAN_LIST_SET, payload, sizeof *payload);
}

uint32_t
dsd_app_scan_list_request_next(void) {
    return ++g_scan_list_requests;
}

int
dsd_app_command_action(int cmd_id) {
    return capture(cmd_id, NULL, 0U);
}

int
dsd_app_command_set_i32(int cmd_id, int32_t value) {
    return capture(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u8(int cmd_id, uint8_t value) {
    return capture(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u32(int cmd_id, uint32_t value) {
    return capture(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_u64(int cmd_id, uint64_t value) {
    return capture(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_double(int cmd_id, double value) {
    return capture(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_float(int cmd_id, float value) {
    return capture(cmd_id, &value, sizeof value);
}

int
dsd_app_command_set_string(int cmd_id, const char* value) {
    return capture(cmd_id, value, strlen(value) + 1U);
}

int
dsd_app_command_set_p25_p2_params(const dsd_app_p25_p2_params_payload* payload) {
    return capture(DSD_APP_CMD_P25_P2_PARAMS_SET, payload, sizeof *payload);
}

int
dsd_app_command_set_tg_listen(const dsd_app_tg_listen_payload* payload) {
    return capture(DSD_APP_CMD_TG_LISTEN_SET, payload, sizeof *payload);
}

int
dsd_app_command_set_tg_listen_all(const dsd_app_tg_listen_all_payload* payload) {
    return capture(DSD_APP_CMD_TG_LISTEN_SET_ALL, payload, sizeof *payload);
}

int
dsd_app_command_set_hytera_key(const dsd_app_hytera_key_payload* payload) {
    return capture(DSD_APP_CMD_KEY_HYTERA_SET, payload, sizeof *payload);
}

int
dsd_app_command_set_aes_key(const dsd_app_aes_key_payload* payload) {
    return capture(DSD_APP_CMD_KEY_AES_SET, payload, sizeof *payload);
}

int
dsd_app_command_dsp_op(const dsd_app_dsp_payload* payload) {
    return capture(DSD_APP_CMD_DSP_OP, payload, sizeof *payload);
}

int
dsd_app_command_apply_config(const dsdneoUserConfig* config) {
    return capture(DSD_APP_CMD_CONFIG_APPLY, config, sizeof *config);
}

int
dsd_app_command_set_config_metadata(const dsd_app_config_metadata_payload* payload) {
    return capture(DSD_APP_CMD_CONFIG_METADATA_SET, payload, sizeof *payload);
}

int
dsd_app_command_set_tone_filter(int32_t mode, const char* list) {
    dsd_app_tone_filter_payload payload = {0};
    DSD_MEMSET(&payload, 0, sizeof payload);
    payload.mode = mode;
    DSD_SNPRINTF(payload.list, sizeof payload.list, "%s", list ? list : "");
    return capture(DSD_APP_CMD_TONE_FILTER_SET, &payload, sizeof payload);
}

int
dsd_app_command_set_tone_filter_mode(int32_t mode) {
    dsd_app_tone_filter_payload payload = {0};
    DSD_MEMSET(&payload, 0, sizeof payload);
    payload.mode = mode;
    payload.keep_list = 1;
    return capture(DSD_APP_CMD_TONE_FILTER_SET, &payload, sizeof payload);
}

int
dsd_app_command_scan_row_edit(const dsd_app_scan_row_edit_payload* payload) {
    return capture(DSD_APP_CMD_SCAN_ROW_EDIT, payload, sizeof *payload);
}

int
dsd_app_command_set_rr_apply(const dsd_app_rr_apply_payload* payload) {
    return capture(DSD_APP_CMD_RR_APPLY_IMPORT, payload, sizeof *payload);
}

int
dsd_app_command_set_rr_account(const dsd_app_rr_account_payload* payload) {
    return capture(DSD_APP_CMD_RR_ACCOUNT_SET, payload, sizeof *payload);
}

uint64_t
dsd_app_command_session_generation(void) {
    return 77U;
}

/*============================================================================
 * Helpers
 *============================================================================*/

/* Run a request; returns 1 when the response says ok, 0 otherwise. The response line is left in @p out. */
static int
run(const char* request, dsd_json_buf* out) {
    dsd_json_node* req = NULL;
    char err[96] = "";
    if (dsd_json_parse(request, &req, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "bad test request %s: %s\n", request, err);
        assert(0);
    }
    assert(dsd_api_command_execute(req, out) == 0);
    dsd_json_free(req);
    return strstr(out->data, "\"ok\":true") != NULL;
}

static int
ok(const char* request) {
    dsd_json_buf out;
    dsd_json_buf_init(&out);
    const int before = g_submits;
    const int rc = run(request, &out);
    if (rc) {
        assert(g_submits == before + 1);
    }
    dsd_json_buf_free(&out);
    return rc;
}

/* Refused as invalid_params, with nothing submitted. */
static void
refused(const char* request) {
    dsd_json_buf out;
    dsd_json_buf_init(&out);
    const int before = g_submits;
    if (run(request, &out) || strstr(out.data, "\"invalid_params\"") == NULL || g_submits != before) {
        DSD_FPRINTF(stderr, "request not refused as invalid_params: %s -> %s", request, out.data);
        assert(0);
    }
    dsd_json_buf_free(&out);
}

static char*
repeat_char(char c, size_t n) {
    char* s = (char*)malloc(n + 1U);
    assert(s != NULL);
    DSD_MEMSET(s, c, n);
    s[n] = '\0';
    return s;
}

/*============================================================================
 * Cases
 *============================================================================*/

/* B5/B6/B15 (issue #518 voice level): gain_set and again_set take a JSON number, whole or fractional, sent as a double
   with its fraction. gain_set refuses anything but 0 (auto) or 0.001..50; again_set sends anything under 1 as 0 (auto)
   and refuses values above 100. Nothing is clamped. */
static void
expect_gain_sent(const char* request, int cmd_id, double want) {
    if (!ok(request) || g_last_id != cmd_id || g_last_n != sizeof(double)) {
        DSD_FPRINTF(stderr, "gain request not sent as a double: %s\n", request);
        assert(0);
    }
    double v = -1.0;
    DSD_MEMCPY(&v, g_last, sizeof v);
    if (!(fabs(v - want) < 1e-12)) {
        DSD_FPRINTF(stderr, "%s sent %.12g, want %.12g\n", request, v, want);
        assert(0);
    }
}

static void
test_gain_values(void) {
    char request[160];

    const struct {
        const char* value;
        double want;
    } dig_ok[] = {{"0", 0.0},    {"0.0", 0.0}, {"0.001", 0.001}, {"0.0011", 0.0011}, {"0.5", 0.5},
                  {"5e-1", 0.5}, {"20", 20.0}, {"50", 50.0},     {"50.0", 50.0}};

    for (size_t i = 0; i < sizeof dig_ok / sizeof dig_ok[0]; i++) {
        DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"gain_set\",\"params\":{\"value\":%s}}", dig_ok[i].value);
        expect_gain_sent(request, DSD_APP_CMD_GAIN_SET, dig_ok[i].want);
    }
    const char* const dig_bad[] = {"0.0004", "0.000999",    "1e-50",      "50.0001", "51",      "-1",   "-0.5",
                                   "1e39",   "-2147483648", "2147483648", "true",    "\"0.5\"", "null", "{}"};
    for (size_t i = 0; i < sizeof dig_bad / sizeof dig_bad[0]; i++) {
        DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"gain_set\",\"params\":{\"value\":%s}}", dig_bad[i]);
        refused(request);
    }
    refused("{\"cmd\":\"gain_set\",\"params\":{\"gain\":1}}");
    refused("{\"cmd\":\"gain_set\"}");

    const struct {
        const char* value;
        double want;
    } ana_ok[] = {{"0", 0.0}, {"0.5", 0.0}, {"0.999", 0.0}, {"1e-50", 0.0}, {"-9", 0.0},
                  {"1", 1.0}, {"1.5", 1.5}, {"37.5", 37.5}, {"100", 100.0}};

    for (size_t i = 0; i < sizeof ana_ok / sizeof ana_ok[0]; i++) {
        DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"again_set\",\"params\":{\"value\":%s}}", ana_ok[i].value);
        expect_gain_sent(request, DSD_APP_CMD_AGAIN_SET, ana_ok[i].want);
    }
    const char* const ana_bad[] = {"100.5", "150", "1e39", "false", "\"1\""};
    for (size_t i = 0; i < sizeof ana_bad / sizeof ana_bad[0]; i++) {
        DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"again_set\",\"params\":{\"value\":%s}}", ana_bad[i]);
        refused(request);
    }
}

static void
test_scalars(void) {
    assert(ok("{\"cmd\":\"manual_tune\",\"params\":{\"value\":851375000}}"));
    assert(g_last_id == DSD_APP_CMD_MANUAL_TUNE && g_last_n == sizeof(uint32_t));
    uint32_t hz = 0U;
    DSD_MEMCPY(&hz, g_last, sizeof hz);
    assert(hz == 851375000U);
    assert(ok("{\"cmd\":\"manual_tune\",\"params\":{\"value\":4294967295}}"));
    refused("{\"cmd\":\"manual_tune\",\"params\":{\"value\":4294967296}}");
    refused("{\"cmd\":\"manual_tune\",\"params\":{\"value\":-1}}");
    refused("{\"cmd\":\"manual_tune\",\"params\":{\"value\":851375000.5}}");
    refused("{\"cmd\":\"manual_tune\",\"params\":{\"value\":\"851375000\"}}");
    refused("{\"cmd\":\"manual_tune\",\"params\":{\"hz\":851375000}}");
    refused("{\"cmd\":\"manual_tune\"}");

    int32_t i32 = 0;

    assert(ok("{\"cmd\":\"lockout_slot\",\"params\":{\"value\":1}}"));
    assert(g_last_id == DSD_APP_CMD_LOCKOUT_SLOT && g_last_n == 1U && g_last[0] == 1U);
    refused("{\"cmd\":\"lockout_slot\",\"params\":{\"value\":2}}");
    refused("{\"cmd\":\"mod_set\",\"params\":{\"value\":3}}");
    /* The decode-mode range ends at the last preset, TETRA (17). */
    assert(ok("{\"cmd\":\"decode_mode_set\",\"params\":{\"value\":17}}"));
    DSD_MEMCPY(&i32, g_last, sizeof i32);
    assert(g_last_id == DSD_APP_CMD_DECODE_MODE_SET && i32 == (int32_t)DSDCFG_MODE_TETRA);
    refused("{\"cmd\":\"decode_mode_set\",\"params\":{\"value\":18}}");
    refused("{\"cmd\":\"decode_mode_set\",\"params\":{\"value\":0}}");
    refused("{\"cmd\":\"trunk_set\",\"params\":{\"value\":-1}}");
    /* The NFM max gap (issue #756) takes 0..10000 ms; a value outside is refused, never clamped. */
    assert(ok("{\"cmd\":\"analog_record_max_gap_set\",\"params\":{\"value\":10000}}"));
    DSD_MEMCPY(&i32, g_last, sizeof i32);
    assert(g_last_id == DSD_APP_CMD_ANALOG_RECORD_MAX_GAP_SET && g_last_n == sizeof(int32_t) && i32 == 10000);
    assert(ok("{\"cmd\":\"analog_record_max_gap_set\",\"params\":{\"value\":0}}"));
    DSD_MEMCPY(&i32, g_last, sizeof i32);
    assert(i32 == 0);
    refused("{\"cmd\":\"analog_record_max_gap_set\",\"params\":{\"value\":10001}}");
    refused("{\"cmd\":\"analog_record_max_gap_set\",\"params\":{\"value\":-1}}");
    /* The NFM pre-roll (issue #668) takes 0..1000 ms; a value outside is refused, never clamped. */
    assert(ok("{\"cmd\":\"analog_record_preroll_set\",\"params\":{\"value\":1000}}"));
    DSD_MEMCPY(&i32, g_last, sizeof i32);
    assert(g_last_id == DSD_APP_CMD_ANALOG_RECORD_PREROLL_SET && g_last_n == sizeof(int32_t) && i32 == 1000);
    assert(ok("{\"cmd\":\"analog_record_preroll_set\",\"params\":{\"value\":0}}"));
    DSD_MEMCPY(&i32, g_last, sizeof i32);
    assert(i32 == 0);
    refused("{\"cmd\":\"analog_record_preroll_set\",\"params\":{\"value\":1001}}");
    refused("{\"cmd\":\"analog_record_preroll_set\",\"params\":{\"value\":-1}}");

    assert(ok("{\"cmd\":\"key_rc4des_set\",\"params\":{\"value\":\"0xFFEEDDCCBBAA9988\"}}"));
    uint64_t u64 = 0U;
    DSD_MEMCPY(&u64, g_last, sizeof u64);
    assert(g_last_id == DSD_APP_CMD_KEY_RC4DES_SET && u64 == UINT64_C(0xFFEEDDCCBBAA9988));
    /* A JSON number above 2^53 keeps every digit: it is read from its literal, never through a double. */
    assert(ok("{\"cmd\":\"key_rc4des_set\",\"params\":{\"value\":18446744073709551615}}"));
    DSD_MEMCPY(&u64, g_last, sizeof u64);
    assert(u64 == UINT64_MAX);
    assert(ok("{\"cmd\":\"tg_session_avoid_clear\",\"params\":{\"value\":\"9007199254740993\"}}"));
    DSD_MEMCPY(&u64, g_last, sizeof u64);
    assert(u64 == UINT64_C(9007199254740993));

    assert(ok("{\"cmd\":\"const_gate_delta\",\"params\":{\"value\":-0.25}}"));
    float f = 0.0F;
    DSD_MEMCPY(&f, g_last, sizeof f);
    assert(g_last_id == DSD_APP_CMD_CONST_GATE_DELTA && f < -0.24F && f > -0.26F);
    refused("{\"cmd\":\"const_gate_delta\",\"params\":{\"value\":1e300}}");

    assert(ok("{\"cmd\":\"toggle_mute\"}"));
    assert(g_last_id == DSD_APP_CMD_TOGGLE_MUTE && g_last_n == 0U);
}

static void
test_strings(void) {
    char request[4096];
    char* path = repeat_char('p', 1023U);
    DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"event_log_set\",\"params\":{\"value\":\"%s\"}}", path);
    assert(ok(request));
    assert(g_last_id == DSD_APP_CMD_EVENT_LOG_SET && g_last_n == 1024U && g_last[1023] == '\0');
    free(path);
    /* One byte more would not fit the decoder's path fields: refused, not cut. */
    char* long_path = repeat_char('p', 1024U);
    DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"event_log_set\",\"params\":{\"value\":\"%s\"}}", long_path);
    refused(request);
    free(long_path);

    char* host = repeat_char('h', 255U);
    DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"udp_out_cfg\",\"params\":{\"host\":\"%s\",\"port\":7355}}", host);
    assert(ok(request));
    dsd_app_endpoint_payload ep = {0};
    assert(g_last_id == DSD_APP_CMD_UDP_OUT_CFG && g_last_n == sizeof ep);
    DSD_MEMCPY(&ep, g_last, sizeof ep);
    assert(strlen(ep.host) == 255U && ep.port == 7355);
    free(host);
    char* long_host = repeat_char('h', 256U);
    DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"udp_out_cfg\",\"params\":{\"host\":\"%s\",\"port\":7355}}",
                 long_host);
    refused(request);
    free(long_host);
    refused("{\"cmd\":\"udp_out_cfg\",\"params\":{\"host\":\"\",\"port\":7355}}");
    refused("{\"cmd\":\"udp_out_cfg\",\"params\":{\"host\":\"a\",\"port\":0}}");
    refused("{\"cmd\":\"udp_out_cfg\",\"params\":{\"host\":\"a\",\"port\":65536}}");

    /* Each string command's limit is its handler's field: M17 user data holds 49 bytes, a Pulse name 99. */
    assert(ok("{\"cmd\":\"m17_user_data_set\",\"params\":{\"value\":"
              "\"0123456789012345678901234567890123456789012345678\"}}"));
    assert(g_last_id == DSD_APP_CMD_M17_USER_DATA_SET && g_last_n == 50U);
    refused("{\"cmd\":\"m17_user_data_set\",\"params\":{\"value\":"
            "\"01234567890123456789012345678901234567890123456789\"}}");
    char* pulse = repeat_char('n', 100U);
    DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"pulse_out_set\",\"params\":{\"value\":\"%s\"}}", pulse);
    refused(request);
    free(pulse);
    char* wav = repeat_char('w', 2047U);
    DSD_SNPRINTF(request, sizeof request, "{\"cmd\":\"input_wav_set\",\"params\":{\"value\":\"%s\"}}", wav);
    assert(ok(request));
    assert(g_last_n == 2048U);
    free(wav);

    refused("{\"cmd\":\"airspy_set\",\"params\":{\"key\":\"airspy_lna_gain\",\"value\":"
            "\"0123456789012345678901234567890123456789\"}}");
}

static void
test_talkgroup_edits(void) {
    refused("{\"cmd\":\"tg_row_set\",\"params\":{\"id_start\":1,\"id_end\":2,\"fields\":1,\"listen\":1}}");
    refused("{\"cmd\":\"tg_row_set\",\"params\":{\"id_start\":2,\"id_end\":1,\"fields\":1,\"policy_context\":1,"
            "\"policy_generation\":1}}");
    /* The first bit past the last field (32, record: issue #708). */
    refused("{\"cmd\":\"tg_row_set\",\"params\":{\"id_start\":1,\"id_end\":2,\"fields\":64,\"policy_context\":1,"
            "\"policy_generation\":1}}");
    refused("{\"cmd\":\"tg_row_set\",\"params\":{\"id_start\":1,\"id_end\":2,\"fields\":32,\"record\":2,"
            "\"policy_context\":1,\"policy_generation\":1}}");
    refused("{\"cmd\":\"tg_row_set\",\"params\":{\"id_start\":1,\"id_end\":2,\"fields\":8,\"name\":"
            "\"01234567890123456789012345678901234567890123456789\",\"policy_context\":1,\"policy_generation\":1}}");
    assert(ok("{\"cmd\":\"tg_row_set\",\"params\":{\"id_start\":100,\"id_end\":199,\"fields\":11,\"listen\":true,"
              "\"priority\":-3,\"name\":\"Fire\",\"policy_context\":\"18446744073709551614\",\"policy_generation\":"
              "4294967295}}"));
    dsd_app_tg_row_payload row = {0};
    assert(g_last_id == DSD_APP_CMD_TG_ROW_SET && g_last_n == sizeof row);
    DSD_MEMCPY(&row, g_last, sizeof row);
    assert(row.id_start == 100U && row.id_end == 199U && row.fields == 11U && row.listen == 1 && row.priority == -3);
    assert(strcmp(row.name, "Fire") == 0 && row.tags[0] == '\0');
    assert(row.policy_context == UINT64_C(18446744073709551614) && row.policy_generation == 4294967295U);
    assert(row.record == 0U);
    assert(ok("{\"cmd\":\"tg_row_set\",\"params\":{\"id_start\":100,\"id_end\":100,\"fields\":33,\"listen\":true,"
              "\"record\":true,\"policy_context\":1,\"policy_generation\":2}}"));
    assert(g_last_id == DSD_APP_CMD_TG_ROW_SET && g_last_n == sizeof row);
    DSD_MEMCPY(&row, g_last, sizeof row);
    assert(row.fields == 33U && row.listen == 1 && row.record == 1U);

    assert(ok("{\"cmd\":\"tg_list_export\",\"params\":{\"path\":\"/tmp/x.csv\",\"policy_context\":5,"
              "\"policy_generation\":6}}"));
    const size_t base = offsetof(dsd_app_tg_export_payload, path);
    assert(g_last_id == DSD_APP_CMD_TG_LIST_EXPORT && g_last_n == base + strlen("/tmp/x.csv") + 1U);
    dsd_app_tg_export_payload head = {0};
    DSD_MEMCPY(&head, g_last, base);
    assert(head.policy_context == 5U && head.policy_generation == 6U);
    assert(strcmp((const char*)g_last + base, "/tmp/x.csv") == 0);

    refused("{\"cmd\":\"tg_selection_set\",\"params\":{\"path\":\"/tmp/s.csv\",\"count\":1,\"policy_context\":1,"
            "\"policy_generation\":1}}");
}

/* B14: single-row edits carry an optional row token (table_index, pattern); without one they stay bounds-only. */
static void
test_talkgroup_row_tokens(void) {
    assert(ok("{\"cmd\":\"tg_row_set\",\"params\":{\"table_index\":3,\"pattern\":\"12*\",\"fields\":8,"
              "\"name\":\"Twelves\",\"policy_context\":5,\"policy_generation\":6}}"));
    dsd_app_tg_row_payload row = {0};
    assert(g_last_id == DSD_APP_CMD_TG_ROW_SET && g_last_n == sizeof row);
    DSD_MEMCPY(&row, g_last, sizeof row);
    assert(row.table_index == 3 && row.kind == DSD_APP_TG_ROW_PATTERN && strcmp(row.pattern, "12*") == 0);
    assert(row.id_start == 0U && row.id_end == 0U && strcmp(row.name, "Twelves") == 0);
    assert(row.policy_context == 5U && row.policy_generation == 6U);

    assert(ok("{\"cmd\":\"tg_row_set\",\"params\":{\"id_start\":100,\"id_end\":199,\"fields\":8,\"name\":\"R\","
              "\"policy_context\":5,\"policy_generation\":6}}"));
    DSD_MEMCPY(&row, g_last, sizeof row);
    assert(row.table_index == -1 && row.kind == DSD_APP_TG_ROW_RANGE && row.pattern[0] == '\0');
    assert(row.id_start == 100U && row.id_end == 199U);
    assert(ok("{\"cmd\":\"tg_row_set\",\"params\":{\"table_index\":1,\"id_start\":7,\"id_end\":7,\"fields\":8,"
              "\"name\":\"E\",\"policy_context\":5,\"policy_generation\":6}}"));
    DSD_MEMCPY(&row, g_last, sizeof row);
    assert(row.table_index == 1 && row.kind == DSD_APP_TG_ROW_EXACT && row.id_start == 7U);

    refused("{\"cmd\":\"tg_row_set\",\"params\":{\"pattern\":\"12*\",\"fields\":8,\"policy_context\":5,"
            "\"policy_generation\":6}}");
    refused("{\"cmd\":\"tg_row_set\",\"params\":{\"table_index\":3,\"pattern\":\"12*\",\"id_start\":12,\"id_end\":0,"
            "\"fields\":8,\"policy_context\":5,\"policy_generation\":6}}");
    refused("{\"cmd\":\"tg_row_set\",\"params\":{\"table_index\":-2,\"id_start\":1,\"id_end\":1,\"fields\":8,"
            "\"policy_context\":5,\"policy_generation\":6}}");
    refused("{\"cmd\":\"tg_row_set\",\"params\":{\"table_index\":3,\"pattern\":\"1234567890123456\",\"fields\":8,"
            "\"policy_context\":5,\"policy_generation\":6}}");

    assert(ok("{\"cmd\":\"tg_row_remove\",\"params\":{\"table_index\":2,\"pattern\":\"1xxx\",\"policy_context\":5,"
              "\"policy_generation\":6}}"));
    dsd_app_tg_range_payload range = {0};
    assert(g_last_id == DSD_APP_CMD_TG_ROW_REMOVE && g_last_n == sizeof range);
    DSD_MEMCPY(&range, g_last, sizeof range);
    assert(range.table_index == 2 && range.kind == DSD_APP_TG_ROW_PATTERN && strcmp(range.pattern, "1xxx") == 0);

    assert(ok("{\"cmd\":\"tg_listen_set\",\"params\":{\"id_start\":100,\"id_end\":100,\"listen\":true}}"));
    dsd_app_tg_listen_payload listen = {0};
    assert(g_last_id == DSD_APP_CMD_TG_LISTEN_SET && g_last_n == sizeof listen);
    DSD_MEMCPY(&listen, g_last, sizeof listen);
    assert(listen.table_index == -1 && listen.kind == DSD_APP_TG_ROW_EXACT && listen.listen == 1);
    assert(listen.policy_context == 0U && listen.policy_generation == 0U);
    assert(ok("{\"cmd\":\"tg_listen_set\",\"params\":{\"table_index\":4,\"pattern\":\"*05\",\"listen\":false,"
              "\"policy_context\":\"18446744073709551614\",\"policy_generation\":9}}"));
    DSD_MEMCPY(&listen, g_last, sizeof listen);
    assert(listen.table_index == 4 && listen.kind == DSD_APP_TG_ROW_PATTERN && strcmp(listen.pattern, "*05") == 0);
    assert(listen.policy_context == UINT64_C(18446744073709551614) && listen.policy_generation == 9U);
    refused("{\"cmd\":\"tg_listen_set\",\"params\":{\"table_index\":4,\"id_start\":1,\"id_end\":1,\"listen\":true}}");
    refused("{\"cmd\":\"tg_listen_set\",\"params\":{\"id_start\":1,\"id_end\":1,\"listen\":true,"
            "\"policy_context\":5}}");
}

static void
test_keys(void) {
    assert(ok("{\"cmd\":\"key_aes_set\",\"params\":{\"K1\":\"0x0123456789ABCDEF\",\"K2\":18446744073709551615,"
              "\"K3\":\"42\",\"K4\":0}}"));
    dsd_app_aes_key_payload aes = {0};
    assert(g_last_id == DSD_APP_CMD_KEY_AES_SET && g_last_n == sizeof aes);
    DSD_MEMCPY(&aes, g_last, sizeof aes);
    assert(aes.K1 == UINT64_C(0x0123456789ABCDEF) && aes.K2 == UINT64_MAX && aes.K3 == 42U && aes.K4 == 0U);
    refused("{\"cmd\":\"key_aes_set\",\"params\":{\"K1\":1,\"K2\":2,\"K3\":3}}");
    refused("{\"cmd\":\"key_aes_set\",\"params\":{\"K1\":1.5,\"K2\":2,\"K3\":3,\"K4\":4}}");

    assert(ok("{\"cmd\":\"key_direct_set\",\"params\":{\"key_type\":\"m17_aes\",\"value\":\"00ff\"}}"));
    dsd_app_key_direct_payload kd = {0};
    DSD_MEMCPY(&kd, g_last, sizeof kd);
    assert(g_last_id == DSD_APP_CMD_KEY_DIRECT_SET && kd.key_type == DSD_APP_KEY_TYPE_M17_AES);
    assert(strcmp(kd.value, "00ff") == 0);
    refused("{\"cmd\":\"key_direct_set\",\"params\":{\"key_type\":\"bogus\",\"value\":\"1\"}}");

    dsd_json_buf out;
    dsd_json_buf_init(&out);
    assert(run("{\"id\":9,\"cmd\":\"decryption_apply\",\"params\":{\"type\":\"rc4\",\"value\":\"0102030405\","
               "\"scope\":1,\"target_id\":\"county-p25\",\"tune_generation\":\"12\",\"key_epoch\":\"3\"}}",
               &out));
    dsd_app_decryption_payload dec = {0};
    assert(g_last_id == DSD_APP_CMD_DECRYPTION_APPLY && g_last_n == sizeof dec);
    DSD_MEMCPY(&dec, g_last, sizeof dec);
    assert((dec.request_id & DSD_API_REQUEST_ID_TAG) != 0U && dec.session_generation == 77U);
    assert(dec.key_type == DSD_APP_KEY_TYPE_RC4 && dec.scope == DSD_APP_KEY_SCOPE_TARGET);
    assert(dec.source == DSD_APP_KEY_SOURCE_DIRECT && dec.fields == (uint32_t)DSD_APP_DECRYPTION_MATERIAL);
    assert(strcmp(dec.target_id, "county-p25") == 0 && dec.tune_generation == 12U && dec.key_epoch == 3U);
    /* The response names the request id the asynchronous result will carry. */
    char expect[64];
    DSD_SNPRINTF(expect, sizeof expect, "\"request_id\":\"%llu\"", (unsigned long long)dec.request_id);
    assert(strstr(out.data, expect) != NULL);
    dsd_json_buf_free(&out);
    /* A field named with an empty value is an edit (a clear), not an omission. */
    assert(ok("{\"cmd\":\"decryption_apply\",\"params\":{\"source\":0}}"));
    DSD_MEMCPY(&dec, g_last, sizeof dec);
    assert(dec.fields == (uint32_t)DSD_APP_DECRYPTION_MATERIAL && dec.source == DSD_APP_KEY_SOURCE_NONE);
    assert(ok("{\"cmd\":\"decryption_apply\",\"params\":{\"map\":\"\",\"force\":0}}"));
    DSD_MEMCPY(&dec, g_last, sizeof dec);
    assert(dec.fields == (uint32_t)(DSD_APP_DECRYPTION_MAP | DSD_APP_DECRYPTION_FORCE) && dec.map_file[0] == '\0');
    assert(ok("{\"cmd\":\"decryption_apply\",\"params\":{\"value\":\"1\",\"fields\":0}}"));
    DSD_MEMCPY(&dec, g_last, sizeof dec);
    assert(dec.fields == 0U);
    refused("{\"cmd\":\"decryption_apply\",\"params\":{\"scope\":2}}");
    refused("{\"cmd\":\"decryption_apply\",\"params\":{\"type\":\"m17Aes\"}}");
}

static void
test_receive(void) {
    assert(ok("{\"cmd\":\"receive_set\",\"params\":{\"kind\":3,\"enabled\":1,\"allow\":\"293/0x370\","
              "\"block\":\"370\"}}"));
    dsd_app_receive_payload receive = {0};
    assert(g_last_id == DSD_APP_CMD_RECEIVE_SET && g_last_n == sizeof receive);
    DSD_MEMCPY(&receive, g_last, sizeof receive);
    assert(receive.kind == 3 && receive.enabled == 1);
    assert(strcmp(receive.allow, "293/0x370") == 0 && strcmp(receive.block, "370") == 0);

    assert(
        ok("{\"cmd\":\"scan_row_edit\",\"params\":{\"scanner\":2,\"session\":7,\"row\":3,\"mode\":2,"
           "\"field\":256,\"action\":1,\"receive\":{\"kind\":4,\"names\":\"--dcc-name \\\"7=Hospital West\\\"\"}}}"));
    dsd_app_scan_row_edit_payload edit = {0};
    assert(g_last_id == DSD_APP_CMD_SCAN_ROW_EDIT && g_last_n == sizeof edit);
    DSD_MEMCPY(&edit, g_last, sizeof edit);
    assert(edit.session == 7U && edit.row == 3 && edit.field == 256 && edit.receive.kind == 4);
    assert(strcmp(edit.receive.names, "--dcc-name \"7=Hospital West\"") == 0);

    refused("{\"cmd\":\"receive_set\",\"params\":{\"kind\":3,\"allow\":293}}");
    refused("{\"cmd\":\"receive_set\",\"params\":{\"kind\":3,\"enabled\":true}}");
    refused("{\"cmd\":\"scan_row_edit\",\"params\":{\"scanner\":2,\"session\":7,\"row\":3,\"field\":128,"
            "\"action\":1,\"receive\":\"293\"}}");
}

static void
test_structured(void) {
    assert(ok("{\"cmd\":\"rtl_set_sql_setting\",\"params\":{\"mode\":2,\"margin_db\":12}}"));
    dsd_app_squelch_setting_payload sql = {0};
    assert(g_last_id == DSD_APP_CMD_RTL_SET_SQL_SETTING && g_last_n == sizeof sql);
    DSD_MEMCPY(&sql, g_last, sizeof sql);
    assert(sql.mode == 2 && sql.margin_db == 12);
    refused("{\"cmd\":\"rtl_set_sql_setting\",\"params\":{\"mode\":3}}");

    assert(ok("{\"cmd\":\"scan_row_edit\",\"params\":{\"scanner\":1,\"session\":7,\"row\":3,\"mode\":2,\"field\":4,"
              "\"action\":1,\"tone_mode\":1,\"tone_list\":\"100.0/D023N\",\"target_id\":\"county-p25\"}}"));
    dsd_app_scan_row_edit_payload edit = {0};
    assert(g_last_id == DSD_APP_CMD_SCAN_ROW_EDIT && g_last_n == sizeof edit);
    DSD_MEMCPY(&edit, g_last, sizeof edit);
    assert(edit.scanner == 1 && edit.session == 7U && edit.row == 3 && edit.mode == 2 && edit.field == 4);
    assert(edit.action == 1 && edit.tone_mode == 1 && strcmp(edit.tone_list, "100.0/D023N") == 0);
    assert(strcmp(edit.target_id, "county-p25") == 0);

    assert(ok("{\"cmd\":\"tone_filter_set\",\"params\":{\"mode\":0,\"keep_list\":true}}"));
    dsd_app_tone_filter_payload tone = {0};
    DSD_MEMCPY(&tone, g_last, sizeof tone);
    assert(tone.keep_list == 1 && tone.mode == 0);
    refused("{\"cmd\":\"tone_filter_set\",\"params\":{\"mode\":1,\"keep_list\":true,\"list\":\"100.0\"}}");

    assert(ok("{\"cmd\":\"dsp_op\",\"params\":{\"op\":7,\"a\":-1}}"));
    dsd_app_dsp_payload dsp = {0};
    DSD_MEMCPY(&dsp, g_last, sizeof dsp);
    assert(dsp.op == 7 && dsp.a == -1 && dsp.b == 0 && dsp.c == 0 && dsp.d == 0);
    refused("{\"cmd\":\"dsp_op\",\"params\":{\"op\":7,\"a\":2147483648}}");

    refused("{\"cmd\":\"rr_apply_import\",\"params\":{\"decode_mode\":2,\"trunking\":true,\"scanner\":true}}");
    refused("{\"cmd\":\"rr_apply_import\",\"params\":{\"decode_mode\":0}}");
    refused("{\"cmd\":\"rr_apply_import\",\"params\":{\"decode_mode\":18}}");
    assert(ok("{\"cmd\":\"rr_apply_import\",\"params\":{\"decode_mode\":17}}"));
    dsd_app_rr_apply_payload rr;
    DSD_MEMSET(&rr, 0, sizeof rr);
    DSD_MEMCPY(&rr, g_last, sizeof rr);
    assert(g_last_id == DSD_APP_CMD_RR_APPLY_IMPORT && rr.decode_mode == (int32_t)DSDCFG_MODE_TETRA);
}

/* scan_avoid_remove (issue #672): each kind's identity reaches the payload exact; a missing field, the other kind's
   name, a negative row and a session that is 0 or past UINT32_MAX (which would alias another) are refused. */
static void
test_scan_avoid_remove(void) {
    dsd_app_scan_avoid_remove_payload p = {0};
    assert(ok("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"lcn\",\"instance\":\"18446744073709551615\","
              "\"list_rev\":7,\"row\":3,\"frequency_hz\":851012500}}"));
    assert(g_last_id == DSD_APP_CMD_SCAN_AVOID_REMOVE && g_last_n == sizeof p);
    DSD_MEMCPY(&p, g_last, sizeof p);
    assert(p.kind == DSD_APP_SCAN_AVOID_KIND_LCN && p.instance == UINT64_MAX && p.list_rev == 7U && p.row == 3);
    assert(p.frequency_hz == 851012500U && p.session == 0U && p.target_id[0] == '\0' && p.pad == 0U);

    assert(ok("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scan\",\"session\":4294967295,"
              "\"target_id\":\"county-p25\"}}"));
    DSD_MEMCPY(&p, g_last, sizeof p);
    assert(p.kind == DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN && p.session == UINT32_MAX);
    assert(strcmp(p.target_id, "county-p25") == 0 && p.row == -1 && p.instance == 0U);

    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"channel\",\"session\":1,\"target_id\":\"a\"}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"lcn\",\"instance\":1,\"list_rev\":2,"
            "\"frequency_hz\":3}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"lcn\",\"instance\":1,\"list_rev\":2,\"row\":-1,"
            "\"frequency_hz\":3}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scan\",\"session\":0,\"target_id\":\"a\"}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scan\",\"session\":4294967297,"
            "\"target_id\":\"a\"}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scan\",\"session\":1,\"target_id\":\"\"}}");
    /* Session and the 64-bit fields as a client whose numbers are doubles sends them; garbage of each type refused. */
    assert(ok("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"lcn\",\"instance\":\"0x10\",\"list_rev\":\"9\","
              "\"row\":2147483647,\"frequency_hz\":\"0\",\"label\":\"Row 1\"}}"));
    DSD_MEMCPY(&p, g_last, sizeof p);
    assert(p.instance == 16U && p.list_rev == 9U && p.row == INT32_MAX && p.frequency_hz == 0U);
    assert(ok(
        "{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scan\",\"session\":\"7\",\"target_id\":\"a\"}}"));
    DSD_MEMCPY(&p, g_last, sizeof p);
    assert(p.session == 7U);
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":1,\"session\":1,\"target_id\":\"a\"}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scan\",\"session\":-1,\"target_id\":\"a\"}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scan\",\"session\":\"4294967296\","
            "\"target_id\":\"a\"}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scan\",\"session\":1,\"target_id\":7}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"lcn\",\"instance\":1,\"list_rev\":2,"
            "\"row\":2147483648,\"frequency_hz\":3}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"lcn\",\"instance\":1,\"list_rev\":-2,"
            "\"row\":0,\"frequency_hz\":3}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"lcn\",\"instance\":1,\"list_rev\":2,"
            "\"row\":0,\"frequency_hz\":-3}}");
    refused("{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scanx\",\"session\":1,\"target_id\":\"a\"}}");
    char* long_id = repeat_char('a', sizeof p.target_id);
    char request[256];
    DSD_SNPRINTF(
        request, sizeof request,
        "{\"cmd\":\"scan_avoid_remove\",\"params\":{\"kind\":\"trunk_scan\",\"session\":1,\"target_id\":\"%s\"}}",
        long_id);
    refused(request);
    free(long_id);
}

/* Run a scan_list_set the queue admits: the payload it submitted, and the reply's echo of the request id it carries. */
static dsd_app_scan_list_set_payload
scan_list_set_admitted(const char* params) {
    char request[256];
    DSD_SNPRINTF(request, sizeof request, "{\"id\":4,\"cmd\":\"scan_list_set\",\"params\":%s}", params);
    dsd_json_buf out;
    dsd_json_buf_init(&out);
    const int before = g_submits;
    assert(run(request, &out) && g_submits == before + 1);
    dsd_app_scan_list_set_payload p = {0};
    assert(g_last_id == DSD_APP_CMD_SCAN_LIST_SET && g_last_n == sizeof p);
    DSD_MEMCPY(&p, g_last, sizeof p);
    /* The id is the app-control counter's, never the client's, and the reply names it as its result will. */
    assert(p.request == g_scan_list_requests && p.pad == 0U);
    char expect[96];
    DSD_SNPRINTF(expect, sizeof expect, "\"cmd\":\"scan_list_set\",\"status\":\"queued\",\"request\":%u}",
                 (unsigned)p.request);
    if (strstr(out.data, expect) == NULL) {
        DSD_FPRINTF(stderr, "scan_list_set %s answered %s", params, out.data);
        assert(0);
    }
    dsd_json_buf_free(&out);
    return p;
}

/* scan_list_set (issue #697): a list number or "all", on or off, and the session it names reach the payload, with a
   request id from app-control's counter that the reply echoes; anything else is refused, a client's own request id
   included (it would collide with the ids the terminal and Qt draw from the same counter). */
static void
test_scan_list_set(void) {
    dsd_app_scan_list_set_payload p = scan_list_set_admitted("{\"list\":2,\"enabled\":0,\"session\":4294967295}");
    assert(p.list == 2 && p.enabled == 0U && p.session == UINT32_MAX);
    const uint32_t first = p.request;
    p = scan_list_set_admitted("{\"list\":\"all\",\"enabled\":1,\"session\":3}");
    assert(p.list == DSD_APP_SCAN_LIST_ALL && p.enabled == 1U && p.session == 3U && p.request == first + 1U);
    p = scan_list_set_admitted("{\"list\":8,\"enabled\":1,\"session\":\"7\"}");
    assert(p.list == 8 && p.enabled == 1U && p.session == 7U);

    refused("{\"cmd\":\"scan_list_set\",\"params\":{}}");
    refused("{\"cmd\":\"scan_list_set\"}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":0,\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":9,\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":-1,\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":65538,\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":2.0,\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":\"2\",\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":\"every\",\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":\"All\",\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":\"allx\",\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":[1],\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":null,\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"enabled\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":2,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":-1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":\"1\",\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":true,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"session\":3}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":1}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":1,\"session\":0}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":1,\"session\":-1}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":1,\"session\":4294967296}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":1,\"session\":\"4294967296\"}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":1,\"session\":1.5}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":1,\"session\":true}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":1,\"session\":3,\"request\":77}}");
    refused("{\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":1,\"session\":3,\"request\":null}}");
    /* A refusal takes no request id. */
    assert(g_scan_list_requests == p.request);

    /* The queue refuses a toggle it has no room for (it never evicts one): a synchronous error, nothing pending. */
    g_scan_list_submit_rc = DSD_APP_COMMAND_SUBMIT_REJECTED;
    dsd_json_buf out;
    dsd_json_buf_init(&out);
    const int before = g_submits;
    assert(!run("{\"id\":5,\"cmd\":\"scan_list_set\",\"params\":{\"list\":1,\"enabled\":0,\"session\":3}}", &out));
    assert(g_submits == before);
    assert(strstr(out.data, "{\"id\":5,\"ok\":false,\"cmd\":\"scan_list_set\",\"error\":{\"code\":\"queue_full\"")
           == out.data);
    assert(strstr(out.data, "\"request\"") == NULL);
    dsd_json_buf_free(&out);
    g_scan_list_submit_rc = DSD_APP_COMMAND_SUBMIT_QUEUED;
}

static void
test_config(void) {
    assert(ok("{\"cmd\":\"config_apply\",\"params\":{\"sections\":{\"trunking\":{\"enabled\":true}}}}"));
    assert(g_last_id == DSD_APP_CMD_CONFIG_APPLY && g_last_n == sizeof(dsdneoUserConfig));
    dsdneoUserConfig* cfg = (dsdneoUserConfig*)calloc(1U, sizeof(*cfg));
    assert(cfg != NULL);
    DSD_MEMCPY(cfg, g_last, sizeof(*cfg));
    assert(cfg->has_trunking == 1 && cfg->trunk_enabled == 1);
    /* Keys the request did not name keep the loader's defaults, which are not all zero. */
    assert(cfg->trunk_tune_group_calls == 1 && cfg->trunk_tune_private_calls == 1 && cfg->trunk_tune_enc_calls == 1);
    assert(cfg->has_input == 0);
    free(cfg);
    /* An empty section still counts, as an empty [analog] in a file does: it puts the analog settings back. */
    assert(ok("{\"cmd\":\"config_apply\",\"params\":{\"sections\":{\"analog\":{}}}}"));
    dsdneoUserConfig* analog = (dsdneoUserConfig*)calloc(1U, sizeof(*analog));
    assert(analog != NULL);
    DSD_MEMCPY(analog, g_last, sizeof(*analog));
    assert(analog->has_analog == 1);
    free(analog);
    refused("{\"cmd\":\"config_apply\",\"params\":{\"sections\":{\"trunking\":{\"enabled\":[1]}}}}");
    refused("{\"cmd\":\"config_apply\",\"params\":{\"sections\":{\"trunking\":1}}}");
    refused("{\"cmd\":\"config_apply\",\"params\":{\"sections\":{}}}");
}

static void
test_response_framing(void) {
    dsd_json_buf out;
    dsd_json_buf_init(&out);
    /* A numeric id comes back exactly as sent, however large. */
    (void)run("{\"id\":12345678901234567890,\"cmd\":\"toggle_mute\"}", &out);
    assert(strncmp(out.data, "{\"id\":12345678901234567890,\"ok\":true,", 37) == 0);
    assert(out.data[out.len - 1U] == '\n');
    (void)run("{\"id\":\"abc\",\"cmd\":\"nope\"}", &out);
    assert(strstr(out.data, "{\"id\":\"abc\",\"ok\":false") == out.data);
    assert(strstr(out.data, "\"unknown_command\"") != NULL);
    dsd_json_buf_free(&out);
}

int
main(void) {
    test_scalars();
    test_gain_values();
    test_strings();
    test_talkgroup_edits();
    test_talkgroup_row_tokens();
    test_keys();
    test_receive();
    test_structured();
    test_scan_avoid_remove();
    test_scan_list_set();
    test_config();
    test_response_framing();
    free(g_last);
    printf("api command payload tests passed\n");
    return 0;
}
