// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for the in-tree JSON writer/parser (api/json.c). */

#include <assert.h>
#include <dsd-neo/api/json.h>
#include <stdio.h>
#include <string.h>

static void
test_writer_roundtrip(void) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_obj_begin(&w) == 0);
    assert(dsd_json_kv_i64(&w, "id", 7) == 0);
    assert(dsd_json_kv_str(&w, "cmd", "tu\"ne") == 0);
    assert(dsd_json_key(&w, "params") == 0);
    assert(dsd_json_obj_begin(&w) == 0);
    assert(dsd_json_kv_u64(&w, "hz", 851375000ULL) == 0);
    assert(dsd_json_kv_bool(&w, "on", 1) == 0);
    assert(dsd_json_key(&w, "arr") == 0);
    assert(dsd_json_arr_begin(&w) == 0);
    assert(dsd_json_value_i64(&w, 1) == 0);
    assert(dsd_json_value_i64(&w, 2) == 0);
    assert(dsd_json_arr_end(&w) == 0);
    assert(dsd_json_obj_end(&w) == 0);
    assert(dsd_json_obj_end(&w) == 0);
    const char* expected = "{\"id\":7,\"cmd\":\"tu\\\"ne\",\"params\":{\"hz\":851375000,\"on\":true,\"arr\":[1,2]}}";
    assert(strcmp(b.data, expected) == 0);

    dsd_json_node* root = NULL;
    char err[64] = "";
    assert(dsd_json_parse(b.data, &root, err, sizeof err) == 0);
    assert(root->type == DSD_JSON_OBJECT);
    dsd_json_free(root);
    dsd_json_buf_free(&b);
}

static void
test_control_chars_and_utf8(void) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_value_str(&w, "a\tb\nc\x01") == 0);
    assert(strcmp(b.data, "\"a\\tb\\nc\\u0001\"") == 0);
    dsd_json_buf_free(&b);

    dsd_json_buf_init(&b);
    dsd_json_writer_init(&w, &b);
    /* Invalid UTF-8 byte becomes U+FFFD. */
    assert(dsd_json_value_str(&w, "x\xFFy") == 0);
    assert(strcmp(b.data, "\"x\xEF\xBF\xBDy\"") == 0);
    dsd_json_buf_free(&b);
}

static void
test_parse_errors(void) {
    dsd_json_node* n = NULL;
    char err[64] = "";
    assert(dsd_json_parse("{\"a\":1} trailing", &n, err, sizeof err) == -1);
    assert(dsd_json_parse("{\"a\":}", &n, err, sizeof err) == -1);
    assert(dsd_json_parse("\"unterminated", &n, err, sizeof err) == -1);
    assert(dsd_json_parse("[1,2", &n, err, sizeof err) == -1);
    assert(dsd_json_parse("", &n, err, sizeof err) == -1);
}

static void
test_parse_escapes(void) {
    const char* doc = "{\"s\":\"a\\u00e9\\n\\ud83d\\ude00\"}";
    dsd_json_node* root = NULL;
    char err[64] = "";
    assert(dsd_json_parse(doc, &root, err, sizeof err) == 0);
    const char* s = dsd_json_as_str(dsd_json_obj_get(root, "s"));
    assert(s != NULL);
    assert(strcmp(s, "a\xc3\xa9\n\xf0\x9f\x98\x80") == 0);
    dsd_json_free(root);
}

int
main(void) {
    test_writer_roundtrip();
    test_control_chars_and_utf8();
    test_parse_errors();
    test_parse_escapes();
    printf("api json tests passed\n");
    return 0;
}
