// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Response framing shared by the API server's session commands and the command table.
 */

#include <stddef.h>

#include "api_internal.h"
#include "json.h"

void
dsd_api_response_begin(dsd_json_buf* out, dsd_json_writer* w, const dsd_json_node* id, int ok) {
    dsd_json_buf_reset(out);
    dsd_json_writer_init(w, out);
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_key(w, "id");
    if (id != NULL && id->type == DSD_JSON_NUMBER && id->string != NULL) {
        (void)dsd_json_value_raw(w, id->string);
    } else if (id != NULL && id->type == DSD_JSON_STRING) {
        (void)dsd_json_value_str(w, id->string);
    } else {
        (void)dsd_json_value_null(w);
    }
    (void)dsd_json_kv_bool(w, "ok", ok);
}

void
dsd_api_response_error(dsd_json_writer* w, const char* code, const char* message) {
    (void)dsd_json_key(w, "error");
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_kv_str(w, "code", code);
    (void)dsd_json_kv_str(w, "message", message ? message : "");
    (void)dsd_json_obj_end(w);
}

int
dsd_api_response_end(dsd_json_buf* out, dsd_json_writer* w) {
    (void)dsd_json_obj_end(w);
    if (dsd_json_writer_failed(w) || dsd_json_buf_putc(out, '\n') != 0) {
        return -1;
    }
    return 0;
}
