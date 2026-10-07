// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for the API's JSON writer/parser (src/api/json.c), which reads untrusted network input. */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

static int
parses(const char* doc) {
    dsd_json_node* root = NULL;
    char err[96] = "";
    const int rc = dsd_json_parse(doc, &root, err, sizeof err);
    if (rc == 0) {
        assert(root != NULL);
        dsd_json_free(root);
        return 1;
    }
    assert(root == NULL);
    assert(err[0] != '\0');
    return 0;
}

static dsd_json_node*
parse_ok(const char* doc) {
    dsd_json_node* root = NULL;
    char err[96] = "";
    if (dsd_json_parse(doc, &root, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "unexpected parse failure for %s: %s\n", doc, err);
        assert(0);
    }
    return root;
}

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
    assert(dsd_json_kv_u64_str(&w, "ctx", UINT64_MAX) == 0);
    assert(dsd_json_kv_bool(&w, "on", 1) == 0);
    assert(dsd_json_key(&w, "arr") == 0);
    assert(dsd_json_arr_begin(&w) == 0);
    assert(dsd_json_value_i64(&w, 1) == 0);
    assert(dsd_json_value_i64(&w, INT64_MIN) == 0);
    assert(dsd_json_arr_end(&w) == 0);
    assert(dsd_json_obj_end(&w) == 0);
    assert(dsd_json_obj_end(&w) == 0);
    const char* expected =
        "{\"id\":7,\"cmd\":\"tu\\\"ne\",\"params\":{\"hz\":851375000,\"ctx\":\"18446744073709551615\","
        "\"on\":true,\"arr\":[1,-9223372036854775808]}}";
    assert(strcmp(b.data, expected) == 0);

    dsd_json_node* root = parse_ok(b.data);
    assert(root->type == DSD_JSON_OBJECT);
    uint64_t ctx = 0U;
    assert(dsd_json_as_u64(dsd_json_obj_get(dsd_json_obj_get(root, "params"), "ctx"), &ctx) == 0);
    assert(ctx == UINT64_MAX);
    dsd_json_free(root);
    dsd_json_buf_free(&b);
}

static void
test_writer_misuse_fails(void) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    dsd_json_writer w;

    /* A value in an object needs a key. */
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_obj_begin(&w) == 0);
    assert(dsd_json_value_i64(&w, 1) == -1);
    assert(dsd_json_writer_failed(&w));

    /* Closing an object as an array. */
    dsd_json_buf_reset(&b);
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_obj_begin(&w) == 0);
    assert(dsd_json_arr_end(&w) == -1);

    /* A key with no value. */
    dsd_json_buf_reset(&b);
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_obj_begin(&w) == 0);
    assert(dsd_json_key(&w, "a") == 0);
    assert(dsd_json_key(&w, "b") == -1);

    /* Two top-level values. */
    dsd_json_buf_reset(&b);
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_value_null(&w) == 0);
    assert(dsd_json_value_null(&w) == -1);

    /* Nesting past the limit. */
    dsd_json_buf_reset(&b);
    dsd_json_writer_init(&w, &b);
    int rc = 0;
    for (int i = 0; i < DSD_JSON_MAX_DEPTH + 1 && rc == 0; i++) {
        rc = dsd_json_arr_begin(&w);
    }
    assert(rc == -1);
    dsd_json_buf_free(&b);
}

static void
test_escaping(void) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_value_str(&w, "a\tb\nc\x01\x7F") == 0);
    assert(strcmp(b.data, "\"a\\tb\\nc\\u0001\\u007F\"") == 0);

    /* Bytes that are not UTF-8 -- a stray continuation, an overlong C0, a truncated sequence -- become U+FFFD. */
    dsd_json_buf_reset(&b);
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_value_str(&w, "x\xFFy\xC0\xAFz\xE2\x82") == 0);
    assert(strcmp(b.data, "\"x\xEF\xBF\xBDy\xEF\xBF\xBD\xEF\xBF\xBDz\xEF\xBF\xBD\xEF\xBF\xBD\"") == 0);

    /* Well-formed multibyte text passes through; the JS line separators are escaped. */
    dsd_json_buf_reset(&b);
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_value_str(&w, "\xC3\xA9\xF0\x9F\x98\x80\xE2\x80\xA8\xE2\x80\xA9") == 0);
    assert(strcmp(b.data, "\"\xC3\xA9\xF0\x9F\x98\x80\\u2028\\u2029\"") == 0);
    dsd_json_buf_free(&b);
}

static void
test_bounded_strings(void) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, &b);
    /* A fixed field with no terminator is written up to its size, never past it. */
    const char unterminated[4] = {'a', 'b', 'c', 'd'};
    const char terminated[8] = "x\"y";
    assert(dsd_json_arr_begin(&w) == 0);
    assert(dsd_json_value_strn(&w, unterminated, sizeof unterminated) == 0);
    assert(dsd_json_value_strn(&w, terminated, sizeof terminated) == 0);
    assert(dsd_json_value_strn(&w, NULL, 4U) == 0);
    assert(dsd_json_arr_end(&w) == 0);
    assert(strcmp(b.data, "[\"abcd\",\"x\\\"y\",\"\"]") == 0);
    dsd_json_buf_free(&b);
}

static void
test_doubles(void) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, &b);
    assert(dsd_json_arr_begin(&w) == 0);
    assert(dsd_json_value_double(&w, 0.1) == 0);
    assert(dsd_json_value_double(&w, -2.5) == 0);
    assert(dsd_json_value_double(&w, (double)NAN) == 0);
    assert(dsd_json_value_double(&w, (double)INFINITY) == 0);
    assert(dsd_json_value_double(&w, -(double)INFINITY) == 0);
    assert(dsd_json_value_double(&w, 851375000.0) == 0);
    assert(dsd_json_arr_end(&w) == 0);
    assert(strcmp(b.data, "[0.1,-2.5,null,null,null,851375000]") == 0);
    assert(parses(b.data));
    dsd_json_buf_free(&b);
}

static void
test_parse_errors(void) {
    static const char* const k_bad[] = {
        "{\"a\":1} trailing",
        "{\"a\":}",
        "\"unterminated",
        "[1,2",
        "",
        "   ",
        "{\"a\":1,}",
        "[1,]",
        "{'a':1}",
        "nul",
        "tru",
        "+1",
        "01",
        "-01",
        "1.",
        ".5",
        "-",
        "1e",
        "1e+",
        "0x10",
        "1e999",
        "-1e999",
        "\"a\\u0000b\"",
        "\"a\\x\"",
        "\"a\\u12\"",
        "\"tab\there\"",
        "\"\xFF\"",
        "\"\xC0\xAF\"",
        "\"\xED\xA0\x80\"",
        "\"\xF4\x90\x80\x80\"",
        "\"\xE2\x82\"",
        "GET / HTTP/1.1",
        "{\"a\" 1}",
        "[1 2]",
    };
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        if (parses(k_bad[i])) {
            DSD_FPRINTF(stderr, "accepted malformed JSON: %s\n", k_bad[i]);
            assert(0);
        }
    }
    char long_number[DSD_JSON_MAX_NUMBER_LEN + 8];
    DSD_MEMSET(long_number, '1', sizeof long_number);
    long_number[sizeof long_number - 1] = '\0';
    assert(!parses(long_number));
}

static void
test_parse_limits(void) {
    /* Nesting: the limit itself parses, one more level does not -- and nothing recurses without bound. */
    char doc[2 * DSD_JSON_MAX_DEPTH + 8];
    size_t n = 0;
    for (int i = 0; i < DSD_JSON_MAX_DEPTH; i++) {
        doc[n++] = '[';
    }
    for (int i = 0; i < DSD_JSON_MAX_DEPTH; i++) {
        doc[n++] = ']';
    }
    doc[n] = '\0';
    assert(parses(doc));
    char deeper[2 * DSD_JSON_MAX_DEPTH + 8];
    n = 0;
    for (int i = 0; i <= DSD_JSON_MAX_DEPTH; i++) {
        deeper[n++] = '[';
    }
    for (int i = 0; i <= DSD_JSON_MAX_DEPTH; i++) {
        deeper[n++] = ']';
    }
    deeper[n] = '\0';
    assert(!parses(deeper));

    /* A line of 100k opening brackets (well under the line limit) is refused, not a stack overflow. */
    const size_t big = 100000U;
    char* brackets = (char*)malloc(big + 1U);
    assert(brackets != NULL);
    DSD_MEMSET(brackets, '[', big);
    brackets[big] = '\0';
    assert(!parses(brackets));
    free(brackets);

    /* Node count: an array of DSD_JSON_MAX_NODES elements needs one node more than the limit. */
    const size_t elems = (size_t)DSD_JSON_MAX_NODES;
    char* arr = (char*)malloc((elems * 2U) + 4U);
    assert(arr != NULL);
    size_t k = 0;
    arr[k++] = '[';
    for (size_t i = 0; i < elems; i++) {
        arr[k++] = '0';
        arr[k++] = (i + 1U < elems) ? ',' : ']';
    }
    arr[k] = '\0';
    assert(!parses(arr));
    arr[(elems - 1U) * 2U] = ']'; /* one element fewer: within the limit */
    arr[((elems - 1U) * 2U) + 1U] = '\0';
    assert(parses(arr));
    free(arr);
}

static void
test_parse_escapes(void) {
    dsd_json_node* root = parse_ok("{\"s\":\"a\\u00e9\\n\\ud83d\\ude00\\/\",\"lone\":\"\\ud800x\"}");
    const char* s = dsd_json_as_str(dsd_json_obj_get(root, "s"));
    assert(s != NULL);
    assert(strcmp(s, "a\xc3\xa9\n\xf0\x9f\x98\x80/") == 0);
    /* A lone surrogate is not a character: it reads as U+FFFD. */
    assert(strcmp(dsd_json_as_str(dsd_json_obj_get(root, "lone")), "\xEF\xBF\xBDx") == 0);
    dsd_json_free(root);
}

static void
test_integer_accessors(void) {
    dsd_json_node* root =
        parse_ok("{\"max\":9223372036854775807,\"min\":-9223372036854775808,\"over\":9223372036854775808,"
                 "\"u64\":18446744073709551615,\"u64over\":18446744073709551616,\"neg\":-1,"
                 "\"frac\":1.5,\"whole\":1.0,\"exp\":1e3,\"hex\":\"0xDEADBEEFcafef00d\","
                 "\"dec\":\"18446744073709551615\",\"sp\":\" 1\",\"plus\":\"+1\",\"bare\":\"0x\","
                 "\"big_hex\":\"0x10000000000000000\",\"t\":true,\"f\":false,\"one\":1,\"two\":2,"
                 "\"str\":\"5\",\"nul\":null}");
    int64_t i = 0;
    uint64_t u = 0U;
    double d = 0.0;
    int b = -1;
    assert(dsd_json_as_i64(dsd_json_obj_get(root, "max"), &i) == 0 && i == INT64_MAX);
    assert(dsd_json_as_i64(dsd_json_obj_get(root, "min"), &i) == 0 && i == INT64_MIN);
    assert(dsd_json_as_i64(dsd_json_obj_get(root, "over"), &i) == -1);
    assert(dsd_json_as_i64(dsd_json_obj_get(root, "neg"), &i) == 0 && i == -1);
    /* A fraction or exponent is not an integer, even when its value is whole. */
    assert(dsd_json_as_i64(dsd_json_obj_get(root, "frac"), &i) == -1);
    assert(dsd_json_as_i64(dsd_json_obj_get(root, "whole"), &i) == -1);
    assert(dsd_json_as_i64(dsd_json_obj_get(root, "exp"), &i) == -1);
    assert(dsd_json_as_i64(dsd_json_obj_get(root, "str"), &i) == -1);
    assert(dsd_json_as_i64(dsd_json_obj_get(root, "t"), &i) == -1);
    assert(dsd_json_as_i64(NULL, &i) == -1);

    /* 64-bit values convert exactly, from a number literal or a decimal/hex string. */
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "u64"), &u) == 0 && u == UINT64_MAX);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "u64over"), &u) == -1);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "neg"), &u) == -1);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "hex"), &u) == 0 && u == UINT64_C(0xDEADBEEFCAFEF00D));
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "dec"), &u) == 0 && u == UINT64_MAX);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "str"), &u) == 0 && u == 5U);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "sp"), &u) == -1);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "plus"), &u) == -1);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "bare"), &u) == -1);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "big_hex"), &u) == -1);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "frac"), &u) == -1);
    assert(dsd_json_as_u64(dsd_json_obj_get(root, "t"), &u) == -1);

    assert(dsd_json_as_double(dsd_json_obj_get(root, "frac"), &d) == 0 && fabs(d - 1.5) < 1e-12);
    assert(dsd_json_as_double(dsd_json_obj_get(root, "str"), &d) == -1);

    assert(dsd_json_as_bool(dsd_json_obj_get(root, "t"), &b) == 0 && b == 1);
    assert(dsd_json_as_bool(dsd_json_obj_get(root, "f"), &b) == 0 && b == 0);
    assert(dsd_json_as_bool(dsd_json_obj_get(root, "one"), &b) == 0 && b == 1);
    assert(dsd_json_as_bool(dsd_json_obj_get(root, "two"), &b) == -1);
    assert(dsd_json_as_bool(dsd_json_obj_get(root, "nul"), &b) == -1);

    assert(dsd_json_as_str_bounded(dsd_json_obj_get(root, "str"), 2U) != NULL);
    assert(dsd_json_as_str_bounded(dsd_json_obj_get(root, "str"), 1U) == NULL);
    assert(dsd_json_as_str_bounded(dsd_json_obj_get(root, "one"), 8U) == NULL);
    dsd_json_free(root);
}

static void
test_buf_consume(void) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    assert(dsd_json_buf_puts(&b, "line1\nline2\n") == 0);
    dsd_json_buf_consume(&b, 6U);
    assert(b.len == 6U && strcmp(b.data, "line2\n") == 0);
    /* The vacated bytes are wiped. */
    for (size_t i = b.len + 1U; i < 12U; i++) {
        assert(b.data[i] == '\0');
    }
    dsd_json_buf_consume(&b, 100U);
    assert(b.len == 0U && b.data[0] == '\0');
    dsd_json_buf_free(&b);
    assert(b.data == NULL && b.len == 0U);
}

int
main(void) {
    test_writer_roundtrip();
    test_writer_misuse_fails();
    test_escaping();
    test_bounded_strings();
    test_doubles();
    test_parse_errors();
    test_parse_limits();
    test_parse_escapes();
    test_integer_accessors();
    test_buf_consume();
    printf("api json tests passed\n");
    return 0;
}
