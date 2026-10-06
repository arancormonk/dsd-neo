// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief JSON command table covering every DSD_APP_CMD_* control.
 *
 * Each entry maps a stable snake_case name to a command id and an argument
 * kind. The dispatcher turns the request's "params" object into the exact
 * by-value payload the app-command queue expects, so the JSON boundary stays a
 * thin, auditable layer over dsd_app_command_submit() and friends.
 *
 * Validation here is about the payload, not the policy: a value of the wrong
 * type, out of the field's range, or too long for its field is refused with
 * invalid_params (never cut short or wrapped); whether a well-formed value is
 * acceptable is the decoder's call when the command is applied, as it is for
 * every frontend.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/config.h>

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "api_internal.h"
#include "json.h"

/* Status codes the submitters return besides the queue's own (dsd_app_command_submit_status). */
enum { API_INVALID_PARAMS = -2, API_NO_MEMORY = -3 };

typedef struct api_command_desc api_command_desc;

/* Build one command's payload from params and submit it. Returns a dsd_app_command_submit_status, API_INVALID_PARAMS
   or API_NO_MEMORY; *request_id gets the id of a request whose completion is reported later. */
typedef int (*api_submit_fn)(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id);

struct api_command_desc {
    const char* name;
    int cmd_id;
    api_submit_fn submit;
    int64_t min; /* scalar commands: the accepted range of params.value */
    int64_t max;
    const char* params;
};

/* Every string a command takes is refused at this length or longer, never cut: the decoder's path and name fields
   are 1024 bytes. */
enum { API_STRING_CAP = 1024 };

/*============================================================================
 * Param helpers. Each returns 0 on success, -1 when the field is missing or
 * invalid; the *_opt forms leave @p out untouched and succeed when absent.
 *============================================================================*/

static int
param_i64(const dsd_json_node* params, const char* key, int64_t min, int64_t max, int64_t* out) {
    int64_t v = 0;
    if (dsd_json_as_i64(dsd_json_obj_get(params, key), &v) != 0 || v < min || v > max) {
        return -1;
    }
    *out = v;
    return 0;
}

static int
param_i64_opt(const dsd_json_node* params, const char* key, int64_t min, int64_t max, int64_t* out) {
    return dsd_json_obj_get(params, key) == NULL ? 0 : param_i64(params, key, min, max, out);
}

static int
param_i32(const dsd_json_node* params, const char* key, int32_t* out) {
    int64_t v = 0;
    if (param_i64(params, key, INT32_MIN, INT32_MAX, &v) != 0) {
        return -1;
    }
    *out = (int32_t)v;
    return 0;
}

static int
param_i32_opt(const dsd_json_node* params, const char* key, int32_t* out) {
    return dsd_json_obj_get(params, key) == NULL ? 0 : param_i32(params, key, out);
}

static int
param_u32(const dsd_json_node* params, const char* key, uint32_t* out) {
    uint64_t v = 0U;
    if (dsd_json_as_u64(dsd_json_obj_get(params, key), &v) != 0 || v > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int
param_u64(const dsd_json_node* params, const char* key, uint64_t* out) {
    return dsd_json_as_u64(dsd_json_obj_get(params, key), out);
}

static int
param_u64_opt(const dsd_json_node* params, const char* key, uint64_t* out) {
    return dsd_json_obj_get(params, key) == NULL ? 0 : param_u64(params, key, out);
}

static int
param_bool(const dsd_json_node* params, const char* key, int* out) {
    return dsd_json_as_bool(dsd_json_obj_get(params, key), out);
}

static int
param_bool_opt(const dsd_json_node* params, const char* key, int* out) {
    return dsd_json_obj_get(params, key) == NULL ? 0 : param_bool(params, key, out);
}

/* Copy a string field that must fit @p cap bytes with its terminator; refused when longer, never cut. */
static int
param_str(const dsd_json_node* params, const char* key, char* dst, size_t cap) {
    const char* s = dsd_json_as_str_bounded(dsd_json_obj_get(params, key), cap);
    if (s == NULL) {
        return -1;
    }
    DSD_SNPRINTF(dst, cap, "%s", s);
    return 0;
}

static int
param_str_opt(const dsd_json_node* params, const char* key, char* dst, size_t cap) {
    return dsd_json_obj_get(params, key) == NULL ? 0 : param_str(params, key, dst, cap);
}

/* The talkgroup list version every list edit must quote (status.tg_policy). */
static int
param_policy(const dsd_json_node* params, uint64_t* context, unsigned int* generation) {
    uint32_t g = 0U;
    if (param_u64(params, "policy_context", context) != 0 || param_u32(params, "policy_generation", &g) != 0) {
        return -1;
    }
    *generation = g;
    return 0;
}

static int
key_type_from_name(const char* name, int32_t* out) {
    static const char* const k_names[] = {"basic", "hex", "rc4", "scrambler", "m17_scrambler", "m17_aes"};
    static const int32_t k_types[] = {DSD_APP_KEY_TYPE_BASIC,         DSD_APP_KEY_TYPE_HEX,
                                      DSD_APP_KEY_TYPE_RC4,           DSD_APP_KEY_TYPE_SCRAMBLER,
                                      DSD_APP_KEY_TYPE_M17_SCRAMBLER, DSD_APP_KEY_TYPE_M17_AES};
    if (name == NULL) {
        return -1;
    }
    for (size_t i = 0U; i < sizeof k_names / sizeof k_names[0]; i++) {
        if (strcmp(name, k_names[i]) == 0) {
            *out = k_types[i];
            return 0;
        }
    }
    return -1;
}

/*============================================================================
 * Submitters. Each returns a dsd_app_command_submit_status, API_INVALID_PARAMS
 * or API_NO_MEMORY. Every payload holding secret material is wiped.
 *============================================================================*/

static int
submit_action(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)params;
    (void)request_id;
    return dsd_app_command_action(d->cmd_id);
}

static int
submit_i32(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    int64_t v = 0;
    if (param_i64(params, "value", d->min, d->max, &v) != 0) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_set_i32(d->cmd_id, (int32_t)v);
}

static int
submit_u8(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    int64_t v = 0;
    if (param_i64(params, "value", d->min, d->max, &v) != 0) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_set_u8(d->cmd_id, (uint8_t)v);
}

static int
submit_u32(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    int64_t v = 0;
    if (param_i64(params, "value", d->min, d->max, &v) != 0) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_set_u32(d->cmd_id, (uint32_t)v);
}

static int
submit_u64(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    uint64_t v = 0U;
    if (param_u64(params, "value", &v) != 0) {
        return API_INVALID_PARAMS;
    }
    /* As the frontends submit both u64 commands; the queue's typed u64 setter admits only the RC4/DES key. */
    const int rc = dsd_app_command_submit(d->cmd_id, &v, sizeof v);
    DSD_SECURE_ZERO(&v, sizeof v);
    return rc;
}

static int
submit_double(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    double v = 0.0;
    if (dsd_json_as_double(dsd_json_obj_get(params, "value"), &v) != 0) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_set_double(d->cmd_id, v);
}

static int
submit_float(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    double v = 0.0;
    if (dsd_json_as_double(dsd_json_obj_get(params, "value"), &v) != 0 || v < -(double)FLT_MAX || v > (double)FLT_MAX) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_set_float(d->cmd_id, (float)v);
}

static int
submit_string(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    const char* s = dsd_json_as_str_bounded(dsd_json_obj_get(params, "value"), API_STRING_CAP);
    if (s == NULL) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_set_string(d->cmd_id, s);
}

static int
submit_endpoint(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    dsd_app_endpoint_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t port = 0;
    if (param_str(params, "host", p.host, sizeof p.host) != 0 || p.host[0] == '\0'
        || param_i64(params, "port", 1, 65535, &port) != 0) {
        return API_INVALID_PARAMS;
    }
    p.port = (int32_t)port;
    return dsd_app_command_submit(d->cmd_id, &p, sizeof p);
}

static int
submit_udp_input(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    dsd_app_udp_input_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t port = 0;
    if (param_str_opt(params, "bind", p.bind, sizeof p.bind) != 0 || param_i64(params, "port", 1, 65535, &port) != 0) {
        return API_INVALID_PARAMS;
    }
    p.port = (int32_t)port;
    return dsd_app_command_submit(d->cmd_id, &p, sizeof p);
}

static int
submit_sql_setting(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)request_id;
    dsd_app_squelch_setting_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t mode = 0;
    if (param_i64(params, "mode", 0, 2, &mode) != 0 || param_i32_opt(params, "margin_db", &p.margin_db) != 0) {
        return API_INVALID_PARAMS;
    }
    const dsd_json_node* level = dsd_json_obj_get(params, "level");
    if (level != NULL && dsd_json_as_double(level, &p.level) != 0) {
        return API_INVALID_PARAMS;
    }
    p.mode = (int32_t)mode;
    return dsd_app_command_submit(d->cmd_id, &p, sizeof p);
}

static int
submit_p25p2(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_p25_p2_params_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    if (param_u64(params, "wacn", &p.wacn) != 0 || param_u64(params, "sysid", &p.sysid) != 0
        || param_u64(params, "cc", &p.cc) != 0) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_set_p25_p2_params(&p);
}

static int
param_id_range(const dsd_json_node* params, uint32_t* start, uint32_t* end) {
    if (param_u32(params, "id_start", start) != 0 || param_u32(params, "id_end", end) != 0 || *end < *start) {
        return -1;
    }
    return 0;
}

static int
submit_tg_listen(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_tg_listen_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int listen = 0;
    if (param_id_range(params, &p.id_start, &p.id_end) != 0 || param_bool(params, "listen", &listen) != 0) {
        return API_INVALID_PARAMS;
    }
    p.listen = listen;
    return dsd_app_command_set_tg_listen(&p);
}

static int
submit_tg_listen_all(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_tg_listen_all_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int listen = 0;
    if (param_bool(params, "listen", &listen) != 0 || param_str_opt(params, "tags", p.tags, sizeof p.tags) != 0) {
        return API_INVALID_PARAMS;
    }
    p.listen = listen;
    return dsd_app_command_set_tg_listen_all(&p);
}

static int
submit_tg_row(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_tg_row_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t fields = 0;
    int listen = 0;
    int preempt = 0;
    if (param_id_range(params, &p.id_start, &p.id_end) != 0 || param_i64(params, "fields", 1, 0x1F, &fields) != 0
        || param_policy(params, &p.policy_context, &p.policy_generation) != 0
        || param_bool_opt(params, "listen", &listen) != 0 || param_bool_opt(params, "preempt", &preempt) != 0
        || param_i32_opt(params, "priority", &p.priority) != 0
        || param_str_opt(params, "name", p.name, sizeof p.name) != 0
        || param_str_opt(params, "tags", p.tags, sizeof p.tags) != 0) {
        return API_INVALID_PARAMS;
    }
    p.fields = (uint32_t)fields;
    p.listen = listen;
    p.preempt = preempt;
    return dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &p, sizeof p);
}

static int
submit_tg_range(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_tg_range_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    if (param_id_range(params, &p.id_start, &p.id_end) != 0
        || param_policy(params, &p.policy_context, &p.policy_generation) != 0) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_submit(DSD_APP_CMD_TG_ROW_REMOVE, &p, sizeof p);
}

static int
submit_tg_export(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    uint64_t context = 0U;
    unsigned int generation = 0U;
    const char* path = dsd_json_as_str_bounded(dsd_json_obj_get(params, "path"), API_STRING_CAP);
    if (path == NULL || path[0] == '\0' || param_policy(params, &context, &generation) != 0) {
        return API_INVALID_PARAMS;
    }
    const size_t path_len = strlen(path);
    const size_t total = offsetof(dsd_app_tg_export_payload, path) + path_len + 1U;
    dsd_app_tg_export_payload* p = (dsd_app_tg_export_payload*)calloc(1U, total);
    if (p == NULL) {
        return API_NO_MEMORY;
    }
    p->policy_context = context;
    p->policy_generation = generation;
    DSD_MEMCPY(p->path, path, path_len + 1U);
    const int rc = dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, p, total);
    free(p);
    return rc;
}

static int
submit_tg_selection(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_tg_selection_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    unsigned int generation = 0U;
    int listening = 0;
    if (param_str(params, "path", p.selection_path, sizeof p.selection_path) != 0 || p.selection_path[0] == '\0'
        || param_policy(params, &p.policy_context, &generation) != 0 || param_u32(params, "count", &p.count) != 0
        || param_bool(params, "listening", &listening) != 0) {
        return API_INVALID_PARAMS;
    }
    p.policy_generation = generation;
    p.listening = listening;
    return dsd_app_command_submit(DSD_APP_CMD_TG_SELECTION_SET, &p, sizeof p);
}

static int
submit_tone_filter(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    int64_t mode = 0;
    int keep = 0;
    const dsd_json_node* list_node = dsd_json_obj_get(params, "list");
    const char* list = dsd_json_as_str_bounded(list_node, DSD_APP_TONE_FILTER_LIST_SIZE);
    if (param_i64(params, "mode", 0, 2, &mode) != 0 || param_bool_opt(params, "keep_list", &keep) != 0
        || (list_node != NULL && list == NULL) || (keep && list != NULL && list[0] != '\0')) {
        return API_INVALID_PARAMS;
    }
    return keep ? dsd_app_command_set_tone_filter_mode((int32_t)mode)
                : dsd_app_command_set_tone_filter((int32_t)mode, list ? list : "");
}

static int
submit_scan_row_edit(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_scan_row_edit_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    if (param_u32(params, "session", &p.session) != 0 || param_i32(params, "scanner", &p.scanner) != 0
        || param_i32(params, "row", &p.row) != 0 || param_i32(params, "field", &p.field) != 0
        || param_i32(params, "action", &p.action) != 0 || param_i32_opt(params, "mode", &p.mode) != 0
        || param_i32_opt(params, "squelch_db", &p.squelch_db) != 0
        || param_i32_opt(params, "squelch_mode", &p.squelch_mode) != 0
        || param_i32_opt(params, "squelch_margin_db", &p.squelch_margin_db) != 0
        || param_i32_opt(params, "width_hz", &p.width_hz) != 0 || param_i32_opt(params, "tone_mode", &p.tone_mode) != 0
        || param_i32_opt(params, "gain_db", &p.gain_db) != 0
        || param_str_opt(params, "target_id", p.target_id, sizeof p.target_id) != 0
        || param_str_opt(params, "tone_list", p.tone_list, sizeof p.tone_list) != 0) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_scan_row_edit(&p);
}

static int
submit_airspy(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_airspy_setting_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    if (param_str(params, "key", p.key, sizeof p.key) != 0 || p.key[0] == '\0'
        || param_str(params, "value", p.value, sizeof p.value) != 0) {
        return API_INVALID_PARAMS;
    }
    return dsd_app_command_submit(DSD_APP_CMD_AIRSPY_SET, &p, sizeof p);
}

static int
submit_dsp(const api_command_desc* desc, const dsd_json_node* params, uint64_t* request_id) {
    (void)desc;
    (void)request_id;
    dsd_app_dsp_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int32_t v[5] = {0, 0, 0, 0, 0}; /* op, then the operands a..d, which default to 0 */
    if (param_i32(params, "op", &v[0]) != 0 || param_i32_opt(params, "a", &v[1]) != 0
        || param_i32_opt(params, "b", &v[2]) != 0 || param_i32_opt(params, "c", &v[3]) != 0
        || param_i32_opt(params, "d", &v[4]) != 0) {
        return API_INVALID_PARAMS;
    }
    p.op = v[0];
    p.a = v[1];
    p.b = v[2];
    p.c = v[3];
    p.d = v[4];
    return dsd_app_command_dsp_op(&p);
}

static int
submit_hytera(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_hytera_key_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int rc = API_INVALID_PARAMS;
    if (param_u64(params, "H", &p.H) == 0 && param_u64(params, "K1", &p.K1) == 0 && param_u64(params, "K2", &p.K2) == 0
        && param_u64(params, "K3", &p.K3) == 0 && param_u64(params, "K4", &p.K4) == 0) {
        rc = dsd_app_command_set_hytera_key(&p);
    }
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

static int
submit_aes(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_aes_key_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int rc = API_INVALID_PARAMS;
    if (param_u64(params, "K1", &p.K1) == 0 && param_u64(params, "K2", &p.K2) == 0
        && param_u64(params, "K3", &p.K3) == 0 && param_u64(params, "K4", &p.K4) == 0) {
        rc = dsd_app_command_set_aes_key(&p);
    }
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

static int
param_flag_opt(const dsd_json_node* params, const char* key, uint8_t* out) {
    int v = 0;
    if (dsd_json_obj_get(params, key) == NULL) {
        return 0;
    }
    if (param_bool(params, key, &v) != 0) {
        return -1;
    }
    *out = (uint8_t)v;
    return 0;
}

/* The RadioReference apply flags, all optional booleans. */
static int
rr_apply_flags(const dsd_json_node* params, dsd_app_rr_apply_payload* p) {
    if (param_flag_opt(params, "edacs_ea", &p->edacs_ea) != 0 || param_flag_opt(params, "edacs_esk", &p->edacs_esk) != 0
        || param_flag_opt(params, "simulcast_qpsk", &p->simulcast_qpsk) != 0
        || param_flag_opt(params, "p25_prefer_candidates", &p->p25_prefer_candidates) != 0
        || param_flag_opt(params, "trunking", &p->trunking) != 0
        || param_flag_opt(params, "scanner", &p->scanner) != 0) {
        return -1;
    }
    return (p->trunking && p->scanner) ? -1 : 0; /* trunking and the scanner cannot both own the tuner */
}

static int
submit_rr_apply(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_rr_apply_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t mode = 0;
    if (param_i64(params, "decode_mode", DSDCFG_MODE_AUTO, DSDCFG_MODE_AM, &mode) != 0
        || rr_apply_flags(params, &p) != 0 || param_str_opt(params, "chan_path", p.chan_path, sizeof p.chan_path) != 0
        || param_str_opt(params, "group_path", p.group_path, sizeof p.group_path) != 0
        || (dsd_json_obj_get(params, "tune_hz") != NULL && param_u32(params, "tune_hz", &p.tune_hz) != 0)) {
        return API_INVALID_PARAMS;
    }
    p.decode_mode = (int32_t)mode;
    p.has_chan = (p.chan_path[0] != '\0') ? 1U : 0U;
    p.has_group = (p.group_path[0] != '\0') ? 1U : 0U;
    return dsd_app_command_set_rr_apply(&p);
}

static int
submit_rr_account(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_rr_account_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int rc = API_INVALID_PARAMS;
    if (param_str(params, "username", p.username, sizeof p.username) == 0
        && param_str(params, "app_key", p.app_key, sizeof p.app_key) == 0) {
        rc = dsd_app_command_set_rr_account(&p);
    }
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

static int
submit_key_direct(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_key_direct_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int rc = API_INVALID_PARAMS;
    if (key_type_from_name(dsd_json_as_str(dsd_json_obj_get(params, "key_type")), &p.key_type) == 0
        && param_str(params, "value", p.value, sizeof p.value) == 0) {
        rc = dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &p, sizeof p);
    }
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

static atomic_int g_request_seq = 0;

/* A request id from the API's own range (DSD_API_REQUEST_ID_TAG), unique across sessions. */
static uint64_t
next_request_id(void) {
    const uint32_t n = (uint32_t)atomic_fetch_add(&g_request_seq, 1) + 1U;
    return DSD_API_REQUEST_ID_TAG | (uint64_t)n;
}

/* The decryption request's text fields; each refused when longer than its field. */
static int
decryption_strings(const dsd_json_node* params, dsd_app_decryption_payload* p) {
    return (param_str_opt(params, "target_id", p->target_id, sizeof p->target_id) != 0
            || param_str_opt(params, "profile", p->profile_ref, sizeof p->profile_ref) != 0
            || param_str_opt(params, "value", p->value, sizeof p->value) != 0
            || param_str_opt(params, "hex", p->keys_hex, sizeof p->keys_hex) != 0
            || param_str_opt(params, "dec", p->keys_dec, sizeof p->keys_dec) != 0
            || param_str_opt(params, "map", p->map_file, sizeof p->map_file) != 0)
               ? -1
               : 0;
}

/* The fields a request names when it gives no explicit mask: material, a map and a force setting, as present. */
static uint32_t
decryption_default_fields(const dsd_json_node* params, const dsd_app_decryption_payload* p) {
    uint32_t fields = 0U;
    if (p->value[0] != '\0' || p->keys_hex[0] != '\0' || p->keys_dec[0] != '\0') {
        fields |= (uint32_t)DSD_APP_DECRYPTION_MATERIAL;
    }
    if (p->map_file[0] != '\0') {
        fields |= (uint32_t)DSD_APP_DECRYPTION_MAP;
    }
    if (dsd_json_obj_get(params, "force") != NULL) {
        fields |= (uint32_t)DSD_APP_DECRYPTION_FORCE;
    }
    return fields;
}

static int
fill_decryption(const dsd_json_node* params, dsd_app_decryption_payload* p) {
    int64_t scope = DSD_APP_KEY_SCOPE_DEFAULTS;
    int64_t source = DSD_APP_KEY_SOURCE_DIRECT;
    int64_t fields = -1;
    const dsd_json_node* type = dsd_json_obj_get(params, "type");
    if ((type != NULL && key_type_from_name(dsd_json_as_str(type), &p->key_type) != 0)
        || param_i64_opt(params, "scope", DSD_APP_KEY_SCOPE_DEFAULTS, DSD_APP_KEY_SCOPE_TARGET, &scope) != 0
        || param_i64_opt(params, "source", DSD_APP_KEY_SOURCE_NONE, DSD_APP_KEY_SOURCE_DIRECT_OVERLAY, &source) != 0
        || param_i64_opt(params, "fields", 0, 7, &fields) != 0 || param_i32_opt(params, "force", &p->force) != 0
        || param_u64_opt(params, "tune_generation", &p->tune_generation) != 0
        || param_u64_opt(params, "key_epoch", &p->key_epoch) != 0 || decryption_strings(params, p) != 0) {
        return -1;
    }
    p->scope = (int32_t)scope;
    p->source = (int32_t)source;
    p->fields = (fields >= 0) ? (uint32_t)fields : decryption_default_fields(params, p);
    return 0;
}

static int
submit_decryption(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    dsd_app_decryption_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int rc = API_INVALID_PARAMS;
    if (fill_decryption(params, &p) == 0) {
        p.request_id = next_request_id();
        p.session_generation = dsd_app_command_session_generation();
        *request_id = p.request_id;
        rc = dsd_app_command_submit(DSD_APP_CMD_DECRYPTION_APPLY, &p, sizeof p);
    }
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

/* An INI value as the loader would read it: a string as is, a bool as 1/0, a number as its literal. A line break
   could never come from an INI file (and would break the file a save writes), so it is refused. */
static const char*
config_value_text(const dsd_json_node* val) {
    if (val == NULL) {
        return NULL;
    }
    switch (val->type) {
        case DSD_JSON_STRING:
        case DSD_JSON_NUMBER: return (val->string != NULL && strpbrk(val->string, "\r\n") == NULL) ? val->string : NULL;
        case DSD_JSON_BOOL: return val->boolean ? "1" : "0";
        default: return NULL;
    }
}

static int
submit_config(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    const dsd_json_node* sections = dsd_json_obj_get(params, "sections");
    if (sections == NULL || sections->type != DSD_JSON_OBJECT || sections->count == 0U) {
        return API_INVALID_PARAMS;
    }
    dsdneoUserConfig* cfg = (dsdneoUserConfig*)malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return API_NO_MEMORY;
    }
    dsd_user_config_init(cfg);
    int rc = 0;
    for (size_t i = 0; i < sections->count && rc == 0; i++) {
        const dsd_json_node* kv = sections->items[i];
        if (kv == NULL || kv->type != DSD_JSON_OBJECT) {
            rc = API_INVALID_PARAMS;
            break;
        }
        for (size_t j = 0; j < kv->count; j++) {
            const char* text = config_value_text(kv->items[j]);
            if (text == NULL || !dsd_user_config_apply_key(cfg, sections->keys[i], kv->keys[j], text)) {
                rc = API_INVALID_PARAMS;
                break;
            }
        }
    }
    if (rc == 0) {
        rc = dsd_app_command_apply_config(cfg);
    }
    /* A config can carry account keys and paths; wipe the copy like any other secret payload. */
    DSD_SECURE_ZERO(cfg, sizeof(*cfg));
    free(cfg);
    return rc;
}

static int
submit_config_meta(const api_command_desc* d, const dsd_json_node* params, uint64_t* request_id) {
    (void)d;
    (void)request_id;
    dsd_app_config_metadata_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int enabled = 0;
    if (param_bool(params, "autosave_enabled", &enabled) != 0
        || param_str_opt(params, "path", p.path, sizeof p.path) != 0) {
        return API_INVALID_PARAMS;
    }
    p.autosave_enabled = enabled;
    return dsd_app_command_set_config_metadata(&p);
}

/*============================================================================
 * The table
 *============================================================================*/

#define API_ACTION(name, id)                 {name, id, submit_action, 0, 0, ""}
#define API_I32(name, id, doc)               {name, id, submit_i32, INT32_MIN, INT32_MAX, doc}
#define API_I32_RANGE(name, id, lo, hi, doc) {name, id, submit_i32, lo, hi, doc}
#define API_U8(name, id, lo, hi, doc)        {name, id, submit_u8, lo, hi, doc}
#define API_U32(name, id, doc)               {name, id, submit_u32, 0, UINT32_MAX, doc}
#define API_KIND(name, id, fn, doc)          {name, id, fn, 0, 0, doc}

/* Ordered to follow include/dsd-neo/app_control/commands.h. */
static const api_command_desc k_commands[] = {
    API_ACTION("toggle_mute", DSD_APP_CMD_TOGGLE_MUTE),
    API_ACTION("toggle_compact", DSD_APP_CMD_TOGGLE_COMPACT),
    API_ACTION("history_cycle", DSD_APP_CMD_HISTORY_CYCLE),

    API_ACTION("slot1_toggle", DSD_APP_CMD_SLOT1_TOGGLE),
    API_ACTION("slot2_toggle", DSD_APP_CMD_SLOT2_TOGGLE),
    API_ACTION("slot_pref_cycle", DSD_APP_CMD_SLOT_PREF_CYCLE),

    API_I32("gain_delta", DSD_APP_CMD_GAIN_DELTA, "{value:int}"),
    API_I32("again_delta", DSD_APP_CMD_AGAIN_DELTA, "{value:int}"),

    API_ACTION("trunk_toggle", DSD_APP_CMD_TRUNK_TOGGLE),
    API_ACTION("scanner_toggle", DSD_APP_CMD_SCANNER_TOGGLE),

    API_ACTION("payload_toggle", DSD_APP_CMD_PAYLOAD_TOGGLE),

    API_ACTION("p25_ga_toggle", DSD_APP_CMD_P25_GA_TOGGLE),
    API_U8("tg_hold_toggle", DSD_APP_CMD_TG_HOLD_TOGGLE, 0, 1, "{value:slot 0|1}"),
    API_ACTION("lpf_toggle", DSD_APP_CMD_LPF_TOGGLE),
    API_ACTION("hpf_toggle", DSD_APP_CMD_HPF_TOGGLE),
    API_ACTION("pbf_toggle", DSD_APP_CMD_PBF_TOGGLE),
    API_ACTION("hpf_d_toggle", DSD_APP_CMD_HPF_D_TOGGLE),
    API_ACTION("aggr_sync_toggle", DSD_APP_CMD_AGGR_SYNC_TOGGLE),
    API_ACTION("call_alert_toggle", DSD_APP_CMD_CALL_ALERT_TOGGLE),
    API_U8("call_alert_events_set", DSD_APP_CMD_CALL_ALERT_EVENTS_SET, 0, 255, "{value:event mask 0..255}"),

    API_ACTION("const_toggle", DSD_APP_CMD_CONST_TOGGLE),
    API_ACTION("const_norm_toggle", DSD_APP_CMD_CONST_NORM_TOGGLE),
    API_KIND("const_gate_delta", DSD_APP_CMD_CONST_GATE_DELTA, submit_float, "{value:number}"),
    API_ACTION("eye_toggle", DSD_APP_CMD_EYE_TOGGLE),
    API_ACTION("eye_unicode_toggle", DSD_APP_CMD_EYE_UNICODE_TOGGLE),
    API_ACTION("eye_color_toggle", DSD_APP_CMD_EYE_COLOR_TOGGLE),
    API_ACTION("fsk_hist_toggle", DSD_APP_CMD_FSK_HIST_TOGGLE),
    API_ACTION("spectrum_toggle", DSD_APP_CMD_SPECTRUM_TOGGLE),
    API_I32("spec_size_delta", DSD_APP_CMD_SPEC_SIZE_DELTA, "{value:int}"),
    API_ACTION("input_vol_cycle", DSD_APP_CMD_INPUT_VOL_CYCLE),

    API_ACTION("eh_next", DSD_APP_CMD_EH_NEXT),
    API_ACTION("eh_prev", DSD_APP_CMD_EH_PREV),
    API_ACTION("eh_toggle_slot", DSD_APP_CMD_EH_TOGGLE_SLOT),

    API_I32("ppm_delta", DSD_APP_CMD_PPM_DELTA, "{value:int}"),
    API_ACTION("invert_toggle", DSD_APP_CMD_INVERT_TOGGLE),
    API_ACTION("mod_toggle", DSD_APP_CMD_MOD_TOGGLE),
    API_ACTION("dmr_reset", DSD_APP_CMD_DMR_RESET),
    API_I32("gain_set", DSD_APP_CMD_GAIN_SET, "{value:int 0..50}"),
    API_I32("again_set", DSD_APP_CMD_AGAIN_SET, "{value:int 0=auto,1..100}"),
    API_KIND("input_warn_db_set", DSD_APP_CMD_INPUT_WARN_DB_SET, submit_double, "{value:dB}"),
    API_ACTION("input_monitor_toggle", DSD_APP_CMD_INPUT_MONITOR_TOGGLE),
    API_ACTION("cosine_filter_toggle", DSD_APP_CMD_COSINE_FILTER_TOGGLE),

    API_ACTION("tcp_connect_audio", DSD_APP_CMD_TCP_CONNECT_AUDIO),
    API_ACTION("rigctl_connect", DSD_APP_CMD_RIGCTL_CONNECT),
    API_ACTION("return_cc", DSD_APP_CMD_RETURN_CC),
    API_ACTION("channel_cycle", DSD_APP_CMD_CHANNEL_CYCLE),
    API_ACTION("symcap_save", DSD_APP_CMD_SYMCAP_SAVE),
    API_ACTION("symcap_stop", DSD_APP_CMD_SYMCAP_STOP),
    API_ACTION("replay_last", DSD_APP_CMD_REPLAY_LAST),
    API_ACTION("wav_start", DSD_APP_CMD_WAV_START),
    API_ACTION("wav_stop", DSD_APP_CMD_WAV_STOP),
    API_ACTION("stop_playback", DSD_APP_CMD_STOP_PLAYBACK),

    API_ACTION("trunk_wlist_toggle", DSD_APP_CMD_TRUNK_WLIST_TOGGLE),
    API_ACTION("trunk_priv_toggle", DSD_APP_CMD_TRUNK_PRIV_TOGGLE),
    API_ACTION("trunk_data_toggle", DSD_APP_CMD_TRUNK_DATA_TOGGLE),
    API_ACTION("trunk_enc_toggle", DSD_APP_CMD_TRUNK_ENC_TOGGLE),
    API_ACTION("wav_toggle", DSD_APP_CMD_WAV_TOGGLE),
    API_ACTION("enc_lockout_clear", DSD_APP_CMD_ENC_LOCKOUT_CLEAR),
    API_ACTION("scan_hold_toggle", DSD_APP_CMD_SCAN_HOLD_TOGGLE),
    API_ACTION("scan_avoid", DSD_APP_CMD_SCAN_AVOID),
    API_ACTION("scan_avoid_clear", DSD_APP_CMD_SCAN_AVOID_CLEAR),

    API_ACTION("quit", DSD_APP_CMD_QUIT),
    API_ACTION("force_priv_toggle", DSD_APP_CMD_FORCE_PRIV_TOGGLE),
    API_ACTION("force_rc4_toggle", DSD_APP_CMD_FORCE_RC4_TOGGLE),
    API_ACTION("trunk_group_toggle", DSD_APP_CMD_TRUNK_GROUP_TOGGLE),
    API_ACTION("sim_nocar", DSD_APP_CMD_SIM_NOCAR),
    API_ACTION("mod_p2_toggle", DSD_APP_CMD_MOD_P2_TOGGLE),
    API_U8("lockout_slot", DSD_APP_CMD_LOCKOUT_SLOT, 0, 1, "{value:slot 0|1}"),
    API_ACTION("m17_tx_toggle", DSD_APP_CMD_M17_TX_TOGGLE),
    API_ACTION("provoice_esk_toggle", DSD_APP_CMD_PROVOICE_ESK_TOGGLE),
    API_ACTION("provoice_mode_toggle", DSD_APP_CMD_PROVOICE_MODE_TOGGLE),
    API_U8("skip_slot", DSD_APP_CMD_SKIP_SLOT, 0, 1, "{value:slot 0|1}"),

    API_ACTION("ui_msg_clear", DSD_APP_CMD_UI_MSG_CLEAR),
    API_ACTION("eh_reset", DSD_APP_CMD_EH_RESET),
    API_ACTION("event_log_disable", DSD_APP_CMD_EVENT_LOG_DISABLE),
    API_KIND("event_log_set", DSD_APP_CMD_EVENT_LOG_SET, submit_string, "{value:path}"),

    API_ACTION("lcw_retune_toggle", DSD_APP_CMD_LCW_RETUNE_TOGGLE),
    API_ACTION("p25_cc_cand_toggle", DSD_APP_CMD_P25_CC_CAND_TOGGLE),
    API_ACTION("reverse_mute_toggle", DSD_APP_CMD_REVERSE_MUTE_TOGGLE),
    API_ACTION("dmr_le_toggle", DSD_APP_CMD_DMR_LE_TOGGLE),
    API_ACTION("all_mutes_toggle", DSD_APP_CMD_ALL_MUTES_TOGGLE),
    API_ACTION("inv_x2_toggle", DSD_APP_CMD_INV_X2_TOGGLE),
    API_ACTION("inv_dmr_toggle", DSD_APP_CMD_INV_DMR_TOGGLE),
    API_ACTION("inv_dpmr_toggle", DSD_APP_CMD_INV_DPMR_TOGGLE),
    API_ACTION("inv_m17_toggle", DSD_APP_CMD_INV_M17_TOGGLE),

    API_KIND("wav_static_open", DSD_APP_CMD_WAV_STATIC_OPEN, submit_string, "{value:path}"),
    API_KIND("wav_raw_open", DSD_APP_CMD_WAV_RAW_OPEN, submit_string, "{value:path}"),
    API_KIND("dsp_out_set", DSD_APP_CMD_DSP_OUT_SET, submit_string, "{value:filename}"),
    API_KIND("symcap_open", DSD_APP_CMD_SYMCAP_OPEN, submit_string, "{value:path}"),
    API_KIND("symbol_in_open", DSD_APP_CMD_SYMBOL_IN_OPEN, submit_string, "{value:path}"),
    API_KIND("input_wav_set", DSD_APP_CMD_INPUT_WAV_SET, submit_string, "{value:path}"),
    API_KIND("input_sym_stream_set", DSD_APP_CMD_INPUT_SYM_STREAM_SET, submit_string, "{value:path}"),
    API_ACTION("input_set_pulse", DSD_APP_CMD_INPUT_SET_PULSE),

    API_KIND("udp_out_cfg", DSD_APP_CMD_UDP_OUT_CFG, submit_endpoint, "{host,port}"),
    API_KIND("tcp_connect_audio_cfg", DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, submit_endpoint, "{host,port}"),
    API_KIND("rigctl_connect_cfg", DSD_APP_CMD_RIGCTL_CONNECT_CFG, submit_endpoint, "{host,port}"),
    API_KIND("udp_input_cfg", DSD_APP_CMD_UDP_INPUT_CFG, submit_udp_input, "{bind,port}"),

    API_ACTION("rtl_enable_input", DSD_APP_CMD_RTL_ENABLE_INPUT),
    API_ACTION("rtl_restart", DSD_APP_CMD_RTL_RESTART),
    API_I32_RANGE("rtl_set_dev", DSD_APP_CMD_RTL_SET_DEV, 0, INT32_MAX, "{value:device index}"),
    API_U32("rtl_set_freq", DSD_APP_CMD_RTL_SET_FREQ, "{value:Hz}"),
    API_I32("rtl_set_gain", DSD_APP_CMD_RTL_SET_GAIN, "{value:int}"),
    API_I32("rtl_set_ppm", DSD_APP_CMD_RTL_SET_PPM, "{value:int}"),
    API_I32("rtl_set_bw", DSD_APP_CMD_RTL_SET_BW, "{value:kHz}"),
    API_KIND("rtl_set_sql_db", DSD_APP_CMD_RTL_SET_SQL_DB, submit_double, "{value:dB}"),
    API_I32("rtl_set_vol_mult", DSD_APP_CMD_RTL_SET_VOL_MULT, "{value:int}"),
    API_I32_RANGE("rtl_set_bias_tee", DSD_APP_CMD_RTL_SET_BIAS_TEE, 0, 1, "{value:0|1}"),
    API_I32_RANGE("rtltcp_set_autotune", DSD_APP_CMD_RTLTCP_SET_AUTOTUNE, 0, 1, "{value:0|1}"),
    API_I32_RANGE("rtl_set_auto_ppm", DSD_APP_CMD_RTL_SET_AUTO_PPM, 0, 1, "{value:0|1}"),
    API_U32("manual_tune", DSD_APP_CMD_MANUAL_TUNE, "{value:Hz}"),
    API_ACTION("tuner_release", DSD_APP_CMD_TUNER_RELEASE),
    API_I32_RANGE("mod_set", DSD_APP_CMD_MOD_SET, 0, 2, "{value:0=C4FM|1=QPSK|2=GFSK}"),
    API_I32_RANGE("decode_mode_set", DSD_APP_CMD_DECODE_MODE_SET, DSDCFG_MODE_AUTO, DSDCFG_MODE_AM,
                  "{value:decode mode 1..16}"),
    API_I32_RANGE("trunk_set", DSD_APP_CMD_TRUNK_SET, 0, 1, "{value:0|1}"),
    API_KIND("airspy_set", DSD_APP_CMD_AIRSPY_SET, submit_airspy, "{key,value}"),
    API_ACTION("airspy_enable_input", DSD_APP_CMD_AIRSPY_ENABLE_INPUT),
    API_KIND("rtl_set_sql_setting", DSD_APP_CMD_RTL_SET_SQL_SETTING, submit_sql_setting,
             "{mode:0=level|1=auto|2=noise,level,margin_db}"),

    API_I32("rigctl_set_mod_bw", DSD_APP_CMD_RIGCTL_SET_MOD_BW, "{value:Hz}"),
    API_U32("tg_hold_set", DSD_APP_CMD_TG_HOLD_SET, "{value:talkgroup}"),
    API_KIND("hangtime_set", DSD_APP_CMD_HANGTIME_SET, submit_double, "{value:seconds}"),
    API_I32_RANGE("slot_pref_set", DSD_APP_CMD_SLOT_PREF_SET, 0, 2, "{value:0=slot 1|1=slot 2|2=auto}"),
    API_I32("slots_onoff_set", DSD_APP_CMD_SLOTS_ONOFF_SET, "{value:mask}"),
    API_I32_RANGE("scan_voice_only_set", DSD_APP_CMD_SCAN_VOICE_ONLY_SET, 0, 1, "{value:0|1}"),
    API_I32("scan_voice_qualify_ms_set", DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, "{value:ms 100..600000}"),
    API_I32("scan_voice_hold_ms_set", DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET, "{value:ms 100..600000}"),
    API_I32("nfm_bandwidth_set", DSD_APP_CMD_NFM_BANDWIDTH_SET, "{value:Hz 8000..25000, 0=default}"),
    API_I32("am_bandwidth_set", DSD_APP_CMD_AM_BANDWIDTH_SET, "{value:Hz 5000..20000, 0=default}"),
    API_KIND("tone_filter_set", DSD_APP_CMD_TONE_FILTER_SET, submit_tone_filter, "{mode:0|1|2,list,keep_list}"),
    API_KIND("scan_row_edit", DSD_APP_CMD_SCAN_ROW_EDIT, submit_scan_row_edit,
             "{scanner,session,row,mode,target_id,field,action,squelch_db,squelch_mode,squelch_margin_db,width_hz,"
             "tone_mode,tone_list,gain_db}"),

    API_KIND("pulse_out_set", DSD_APP_CMD_PULSE_OUT_SET, submit_string, "{value:name}"),
    API_KIND("pulse_in_set", DSD_APP_CMD_PULSE_IN_SET, submit_string, "{value:name}"),

    API_I32("input_vol_set", DSD_APP_CMD_INPUT_VOL_SET, "{value:mult 1..16}"),

    API_ACTION("lrrp_set_home", DSD_APP_CMD_LRRP_SET_HOME),
    API_ACTION("lrrp_set_dsdp", DSD_APP_CMD_LRRP_SET_DSDP),
    API_KIND("lrrp_set_custom", DSD_APP_CMD_LRRP_SET_CUSTOM, submit_string, "{value:path}"),
    API_ACTION("lrrp_disable", DSD_APP_CMD_LRRP_DISABLE),

    API_KIND("import_channel_map", DSD_APP_CMD_IMPORT_CHANNEL_MAP, submit_string, "{value:path}"),
    API_KIND("import_group_list", DSD_APP_CMD_IMPORT_GROUP_LIST, submit_string, "{value:path}"),
    API_KIND("import_keys_dec", DSD_APP_CMD_IMPORT_KEYS_DEC, submit_string, "{value:path}"),
    API_KIND("import_keys_hex", DSD_APP_CMD_IMPORT_KEYS_HEX, submit_string, "{value:path}"),
    API_ACTION("import_channel_map_clear", DSD_APP_CMD_IMPORT_CHANNEL_MAP_CLEAR),
    API_ACTION("import_group_list_clear", DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR),
    API_ACTION("import_keys_clear", DSD_APP_CMD_IMPORT_KEYS_CLEAR),
    API_KIND("import_p25_bandplan", DSD_APP_CMD_IMPORT_P25_BANDPLAN, submit_string, "{value:path}"),
    API_KIND("export_p25_bandplan", DSD_APP_CMD_EXPORT_P25_BANDPLAN, submit_string, "{value:path}"),

    API_KIND("rr_apply_import", DSD_APP_CMD_RR_APPLY_IMPORT, submit_rr_apply,
             "{decode_mode,edacs_ea,edacs_esk,simulcast_qpsk,p25_prefer_candidates,trunking,scanner,chan_path,"
             "group_path,tune_hz}"),
    API_KIND("rr_account_set", DSD_APP_CMD_RR_ACCOUNT_SET, submit_rr_account, "{username,app_key}"),

    API_KIND("import_src_list", DSD_APP_CMD_IMPORT_SRC_LIST, submit_string, "{value:path}"),
    API_ACTION("import_src_list_clear", DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR),

    API_KIND("p25_p2_params_set", DSD_APP_CMD_P25_P2_PARAMS_SET, submit_p25p2, "{wacn,sysid,cc}"),

    API_KIND("tg_listen_set", DSD_APP_CMD_TG_LISTEN_SET, submit_tg_listen, "{id_start,id_end,listen}"),
    API_KIND("tg_listen_set_all", DSD_APP_CMD_TG_LISTEN_SET_ALL, submit_tg_listen_all, "{listen,tags}"),
    API_KIND("tg_row_set", DSD_APP_CMD_TG_ROW_SET, submit_tg_row,
             "{id_start,id_end,fields,listen,priority,preempt,name,tags,policy_context,policy_generation}"),
    API_KIND("tg_row_remove", DSD_APP_CMD_TG_ROW_REMOVE, submit_tg_range,
             "{id_start,id_end,policy_context,policy_generation}"),
    API_KIND("tg_list_export", DSD_APP_CMD_TG_LIST_EXPORT, submit_tg_export, "{path,policy_context,policy_generation}"),
    API_KIND("tg_selection_set", DSD_APP_CMD_TG_SELECTION_SET, submit_tg_selection,
             "{path,count,listening,policy_context,policy_generation}"),
    API_I32_RANGE("tg_lockout_persist_set", DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET, 0, 1, "{value:0|1}"),
    API_KIND("tg_session_avoid_clear", DSD_APP_CMD_TG_SESSION_AVOID_CLEAR, submit_u64, "{value:policy context}"),

    API_ACTION("ui_show_dsp_panel_toggle", DSD_APP_CMD_UI_SHOW_DSP_PANEL_TOGGLE),
    API_ACTION("ui_show_p25_metrics_toggle", DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE),
    API_ACTION("ui_show_p25_affil_toggle", DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE),
    API_ACTION("ui_show_p25_neighbors_toggle", DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE),
    API_ACTION("ui_show_p25_iden_toggle", DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE),
    API_ACTION("ui_show_p25_ccc_toggle", DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE),
    API_ACTION("ui_show_channels_toggle", DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE),
    API_ACTION("ui_show_p25_callsign_toggle", DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE),

    API_U32("key_basic_set", DSD_APP_CMD_KEY_BASIC_SET, "{value:uint32}"),
    API_U32("key_scrambler_set", DSD_APP_CMD_KEY_SCRAMBLER_SET, "{value:uint32}"),
    API_KIND("key_rc4des_set", DSD_APP_CMD_KEY_RC4DES_SET, submit_u64, "{value:uint64}"),
    API_KIND("key_hytera_set", DSD_APP_CMD_KEY_HYTERA_SET, submit_hytera, "{H,K1,K2,K3,K4}"),
    API_KIND("key_aes_set", DSD_APP_CMD_KEY_AES_SET, submit_aes, "{K1,K2,K3,K4}"),
    API_KIND("key_tyt_ap_set", DSD_APP_CMD_KEY_TYT_AP_SET, submit_string, "{value:hex}"),
    API_KIND("key_retevis_rc2_set", DSD_APP_CMD_KEY_RETEVIS_RC2_SET, submit_string, "{value:hex}"),
    API_KIND("key_tyt_ep_set", DSD_APP_CMD_KEY_TYT_EP_SET, submit_string, "{value:hex}"),
    API_KIND("key_ken_scr_set", DSD_APP_CMD_KEY_KEN_SCR_SET, submit_string, "{value:decimal}"),
    API_KIND("key_anytone_bp_set", DSD_APP_CMD_KEY_ANYTONE_BP_SET, submit_string, "{value:hex16}"),
    API_KIND("key_xor_set", DSD_APP_CMD_KEY_XOR_SET, submit_string, "{value:len:hex}"),
    API_KIND("m17_user_data_set", DSD_APP_CMD_M17_USER_DATA_SET, submit_string, "{value:string}"),
    API_KIND("key_direct_set", DSD_APP_CMD_KEY_DIRECT_SET, submit_key_direct, "{key_type,value}"),
    API_I32_RANGE("force_key_set", DSD_APP_CMD_FORCE_KEY_SET, 0, 2, "{value:0|1|2}"),
    API_KIND("decryption_apply", DSD_APP_CMD_DECRYPTION_APPLY, submit_decryption,
             "{type,value,hex,dec,map,profile,force,source,scope,fields,target_id,tune_generation,key_epoch}"),

    API_KIND("dsp_op", DSD_APP_CMD_DSP_OP, submit_dsp, "{op,a,b,c,d}"),
    API_KIND("config_apply", DSD_APP_CMD_CONFIG_APPLY, submit_config, "{sections:{section:{key:value}}}"),
    API_KIND("config_metadata_set", DSD_APP_CMD_CONFIG_METADATA_SET, submit_config_meta, "{autosave_enabled,path}"),
};

enum { API_COMMAND_COUNT = (int)(sizeof k_commands / sizeof k_commands[0]) };

static const api_command_desc*
find_command(const char* name) {
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < API_COMMAND_COUNT; i++) {
        if (strcmp(k_commands[i].name, name) == 0) {
            return &k_commands[i];
        }
    }
    return NULL;
}

int
dsd_api_command_count_total(void) {
    return API_COMMAND_COUNT;
}

int
dsd_api_command_execute(const dsd_json_node* request, dsd_json_buf* out) {
    const dsd_json_node* id = dsd_json_obj_get(request, "id");
    const char* name = dsd_json_as_str(dsd_json_obj_get(request, "cmd"));
    const dsd_json_node* params = dsd_json_obj_get(request, "params");
    dsd_json_writer w;
    const api_command_desc* desc = find_command(name);
    if (desc == NULL) {
        dsd_api_response_begin(out, &w, id, 0);
        (void)dsd_json_kv_str(&w, "cmd", name ? name : "");
        dsd_api_response_error(&w, "unknown_command", "no such command");
        return dsd_api_response_end(out, &w);
    }
    uint64_t request_id = 0U;
    const int rc = desc->submit(desc, params, &request_id);
    const int ok = (rc == DSD_APP_COMMAND_SUBMIT_QUEUED || rc == DSD_APP_COMMAND_SUBMIT_COALESCED);
    dsd_api_response_begin(out, &w, id, ok);
    (void)dsd_json_kv_str(&w, "cmd", desc->name);
    if (ok) {
        (void)dsd_json_kv_str(&w, "status", (rc == DSD_APP_COMMAND_SUBMIT_COALESCED) ? "coalesced" : "queued");
        if (request_id != 0U) {
            (void)dsd_json_kv_u64_str(&w, "request_id", request_id);
        }
    } else if (rc == API_INVALID_PARAMS) {
        dsd_api_response_error(&w, "invalid_params", "missing, out-of-range or over-long parameters");
    } else if (rc == API_NO_MEMORY) {
        dsd_api_response_error(&w, "internal_error", "out of memory");
    } else {
        dsd_api_response_error(&w, "rejected", "not admitted: no decoder session is open");
    }
    return dsd_api_response_end(out, &w);
}

int
dsd_api_command_catalog(dsd_json_buf* out) {
    dsd_json_buf_reset(out);
    dsd_json_writer w;
    dsd_json_writer_init(&w, out);
    (void)dsd_json_arr_begin(&w);
    for (int i = 0; i < API_COMMAND_COUNT; i++) {
        (void)dsd_json_obj_begin(&w);
        (void)dsd_json_kv_str(&w, "name", k_commands[i].name);
        (void)dsd_json_kv_i64(&w, "id", k_commands[i].cmd_id);
        (void)dsd_json_kv_str(&w, "params", k_commands[i].params);
        (void)dsd_json_obj_end(&w);
    }
    (void)dsd_json_arr_end(&w);
    return dsd_json_writer_failed(&w) ? -1 : 0;
}
