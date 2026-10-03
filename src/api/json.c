// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Implementation of the small JSON writer/parser declared in api/json.h.
 */

#include <dsd-neo/api/json.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*============================================================================
 * Growable buffer
 *============================================================================*/

void
dsd_json_buf_init(dsd_json_buf* b) {
    if (b == NULL) {
        return;
    }
    b->data = NULL;
    b->len = 0U;
    b->cap = 0U;
    b->oom = 0;
}

void
dsd_json_buf_free(dsd_json_buf* b) {
    if (b == NULL) {
        return;
    }
    free(b->data);
    dsd_json_buf_init(b);
}

void
dsd_json_buf_reset(dsd_json_buf* b) {
    if (b == NULL) {
        return;
    }
    b->len = 0U;
    if (b->data != NULL) {
        b->data[0] = '\0';
    }
}

int
dsd_json_buf_reserve(dsd_json_buf* b, size_t extra) {
    if (b == NULL) {
        return -1;
    }
    if (b->oom) {
        return -1;
    }
    const size_t need = b->len + extra + 1U; /* +1 for the NUL terminator */
    if (need <= b->cap && b->data != NULL) {
        return 0;
    }
    size_t cap = (b->cap == 0U) ? 256U : b->cap;
    while (cap < need) {
        if (cap > (size_t)-1 / 2U) {
            b->oom = 1;
            return -1;
        }
        cap *= 2U;
    }
    char* grown = (char*)realloc(b->data, cap);
    if (grown == NULL) {
        b->oom = 1;
        return -1;
    }
    b->data = grown;
    b->cap = cap;
    if (b->len == 0U) {
        b->data[0] = '\0';
    }
    return 0;
}

int
dsd_json_buf_append(dsd_json_buf* b, const char* data, size_t n) {
    if (b == NULL || (data == NULL && n != 0U)) {
        return -1;
    }
    if (n == 0U) {
        return 0;
    }
    if (dsd_json_buf_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int
dsd_json_buf_puts(dsd_json_buf* b, const char* s) {
    if (s == NULL) {
        return -1;
    }
    return dsd_json_buf_append(b, s, strlen(s));
}

int
dsd_json_buf_putc(dsd_json_buf* b, char c) {
    if (dsd_json_buf_reserve(b, 1U) != 0) {
        return -1;
    }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 0;
}

/*============================================================================
 * Escaping
 *============================================================================*/

static int
json_append_replacement(dsd_json_buf* b) {
    /* U+FFFD, UTF-8 encoded. */
    return dsd_json_buf_append(b, "\xEF\xBF\xBD", 3U);
}

int
dsd_json_buf_append_escaped(dsd_json_buf* b, const char* v) {
    if (b == NULL) {
        return -1;
    }
    if (v == NULL) {
        return dsd_json_buf_puts(b, "");
    }
    const unsigned char* p = (const unsigned char*)v;
    while (*p != 0U) {
        const unsigned char c = *p;
        if (c == '"') {
            if (dsd_json_buf_append(b, "\\\"", 2U) != 0) {
                return -1;
            }
            p++;
        } else if (c == '\\') {
            if (dsd_json_buf_append(b, "\\\\", 2U) != 0) {
                return -1;
            }
            p++;
        } else if (c == '\b') {
            if (dsd_json_buf_append(b, "\\b", 2U) != 0) {
                return -1;
            }
            p++;
        } else if (c == '\f') {
            if (dsd_json_buf_append(b, "\\f", 2U) != 0) {
                return -1;
            }
            p++;
        } else if (c == '\n') {
            if (dsd_json_buf_append(b, "\\n", 2U) != 0) {
                return -1;
            }
            p++;
        } else if (c == '\r') {
            if (dsd_json_buf_append(b, "\\r", 2U) != 0) {
                return -1;
            }
            p++;
        } else if (c == '\t') {
            if (dsd_json_buf_append(b, "\\t", 2U) != 0) {
                return -1;
            }
            p++;
        } else if (c < 0x20U) {
            char esc[8];
            const int n = snprintf(esc, sizeof esc, "\\u%04X", (unsigned)c);
            if (n <= 0 || dsd_json_buf_append(b, esc, (size_t)n) != 0) {
                return -1;
            }
            p++;
        } else if (c < 0x80U) {
            if (dsd_json_buf_putc(b, (char)c) != 0) {
                return -1;
            }
            p++;
        } else {
            /* Validate a UTF-8 sequence; copy it verbatim when well formed,
             * otherwise substitute U+FFFD so output stays valid UTF-8. */
            size_t seq = 0U;
            if ((c & 0xE0U) == 0xC0U) {
                seq = 2U;
            } else if ((c & 0xF0U) == 0xE0U) {
                seq = 3U;
            } else if ((c & 0xF8U) == 0xF0U) {
                seq = 4U;
            }
            int valid = (seq != 0U);
            for (size_t i = 1U; valid && i < seq; i++) {
                if ((p[i] & 0xC0U) != 0x80U) {
                    valid = 0;
                }
            }
            /* Reject overlong/surrogate/out-of-range encodings. */
            if (valid && seq == 3U) {
                if (c == 0xE0U && p[1] < 0xA0U) {
                    valid = 0;
                }
                if (c == 0xEDU && p[1] >= 0xA0U) {
                    valid = 0;
                }
            } else if (valid && seq == 4U) {
                if (c == 0xF0U && p[1] < 0x90U) {
                    valid = 0;
                }
                if (c == 0xF4U && p[1] >= 0x90U) {
                    valid = 0;
                }
                if (c > 0xF4U) {
                    valid = 0;
                }
            }
            if (valid) {
                if (dsd_json_buf_append(b, (const char*)p, seq) != 0) {
                    return -1;
                }
                p += seq;
            } else {
                if (json_append_replacement(b) != 0) {
                    return -1;
                }
                p++;
            }
        }
    }
    return 0;
}

/*============================================================================
 * Writer
 *============================================================================*/

void
dsd_json_writer_init(dsd_json_writer* w, dsd_json_buf* buf) {
    if (w == NULL) {
        return;
    }
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->first[0] = 1U;
}

int
dsd_json_writer_failed(const dsd_json_writer* w) {
    return (w == NULL) || w->failed || w->buf == NULL || w->buf->oom;
}

static int
json_writer_fail(dsd_json_writer* w) {
    if (w != NULL) {
        w->failed = 1;
    }
    return -1;
}

static int
json_writer_putc(dsd_json_writer* w, char c) {
    if (dsd_json_writer_failed(w)) {
        return json_writer_fail(w);
    }
    if (dsd_json_buf_putc(w->buf, c) != 0) {
        return json_writer_fail(w);
    }
    return 0;
}

static int
json_writer_puts(dsd_json_writer* w, const char* s) {
    if (dsd_json_writer_failed(w)) {
        return json_writer_fail(w);
    }
    if (dsd_json_buf_puts(w->buf, s) != 0) {
        return json_writer_fail(w);
    }
    return 0;
}

/* Emit the separator dues for a value at the current level. */
static int
json_writer_before_value(dsd_json_writer* w) {
    if (dsd_json_writer_failed(w)) {
        return json_writer_fail(w);
    }
    const int level = w->depth;
    if (level >= DSD_JSON_MAX_DEPTH) {
        return json_writer_fail(w);
    }
    if (w->after_key[level]) {
        w->after_key[level] = 0U;
        return 0;
    }
    if (w->first[level]) {
        w->first[level] = 0U;
        return 0;
    }
    if (level == 0) {
        /* A second top-level value would not be a single JSON document. */
        return json_writer_fail(w);
    }
    return json_writer_putc(w, ',');
}

int
dsd_json_key(dsd_json_writer* w, const char* key) {
    if (dsd_json_writer_failed(w) || key == NULL) {
        return json_writer_fail(w);
    }
    const int level = w->depth;
    if (level <= 0 || !w->is_obj[level]) {
        return json_writer_fail(w);
    }
    if (!w->first[level]) {
        if (json_writer_putc(w, ',') != 0) {
            return -1;
        }
    }
    w->first[level] = 0U;
    if (json_writer_putc(w, '"') != 0) {
        return -1;
    }
    if (dsd_json_buf_append_escaped(w->buf, key) != 0) {
        return json_writer_fail(w);
    }
    if (json_writer_putc(w, '"') != 0) {
        return -1;
    }
    if (json_writer_putc(w, ':') != 0) {
        return -1;
    }
    w->after_key[level] = 1U;
    return 0;
}

static int
json_open(dsd_json_writer* w, char brace, int is_obj) {
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    if (json_writer_putc(w, brace) != 0) {
        return -1;
    }
    if (w->depth + 1 >= DSD_JSON_MAX_DEPTH) {
        return json_writer_fail(w);
    }
    w->depth++;
    w->first[w->depth] = 1U;
    w->is_obj[w->depth] = (unsigned char)(is_obj ? 1 : 0);
    w->after_key[w->depth] = 0U;
    return 0;
}

static int
json_close(dsd_json_writer* w, char brace) {
    if (dsd_json_writer_failed(w) || w->depth <= 0) {
        return json_writer_fail(w);
    }
    if (w->after_key[w->depth]) {
        /* A key with no value is malformed. */
        return json_writer_fail(w);
    }
    if (json_writer_putc(w, brace) != 0) {
        return -1;
    }
    w->depth--;
    return 0;
}

int
dsd_json_obj_begin(dsd_json_writer* w) {
    return json_open(w, '{', 1);
}

int
dsd_json_arr_begin(dsd_json_writer* w) {
    return json_open(w, '[', 0);
}

int
dsd_json_obj_end(dsd_json_writer* w) {
    return json_close(w, '}');
}

int
dsd_json_arr_end(dsd_json_writer* w) {
    return json_close(w, ']');
}

int
dsd_json_value_str(dsd_json_writer* w, const char* v) {
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    if (json_writer_putc(w, '"') != 0) {
        return -1;
    }
    if (dsd_json_buf_append_escaped(w->buf, v) != 0) {
        return json_writer_fail(w);
    }
    return json_writer_putc(w, '"');
}

int
dsd_json_value_i64(dsd_json_writer* w, int64_t v) {
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    char tmp[32];
    const int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        return json_writer_fail(w);
    }
    return json_writer_puts(w, tmp);
}

int
dsd_json_value_u64(dsd_json_writer* w, uint64_t v) {
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    char tmp[32];
    const int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        return json_writer_fail(w);
    }
    return json_writer_puts(w, tmp);
}

int
dsd_json_value_double(dsd_json_writer* w, double v) {
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    if (!isfinite(v)) {
        return json_writer_puts(w, "null");
    }
    char tmp[40];
    const int n = snprintf(tmp, sizeof tmp, "%.10g", v);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        return json_writer_fail(w);
    }
    return json_writer_puts(w, tmp);
}

int
dsd_json_value_bool(dsd_json_writer* w, int v) {
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    return json_writer_puts(w, v ? "true" : "false");
}

int
dsd_json_value_null(dsd_json_writer* w) {
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    return json_writer_puts(w, "null");
}

int
dsd_json_value_raw(dsd_json_writer* w, const char* raw) {
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    return json_writer_puts(w, raw);
}

int
dsd_json_kv_str(dsd_json_writer* w, const char* key, const char* v) {
    return (dsd_json_key(w, key) == 0) ? dsd_json_value_str(w, v) : -1;
}

int
dsd_json_kv_i64(dsd_json_writer* w, const char* key, int64_t v) {
    return (dsd_json_key(w, key) == 0) ? dsd_json_value_i64(w, v) : -1;
}

int
dsd_json_kv_u64(dsd_json_writer* w, const char* key, uint64_t v) {
    return (dsd_json_key(w, key) == 0) ? dsd_json_value_u64(w, v) : -1;
}

int
dsd_json_kv_double(dsd_json_writer* w, const char* key, double v) {
    return (dsd_json_key(w, key) == 0) ? dsd_json_value_double(w, v) : -1;
}

int
dsd_json_kv_bool(dsd_json_writer* w, const char* key, int v) {
    return (dsd_json_key(w, key) == 0) ? dsd_json_value_bool(w, v) : -1;
}

int
dsd_json_kv_null(dsd_json_writer* w, const char* key) {
    return (dsd_json_key(w, key) == 0) ? dsd_json_value_null(w) : -1;
}

/*============================================================================
 * Parser
 *============================================================================*/

typedef struct {
    const char* p;
    const char* end;
    size_t nodes;
    const char* err;
} json_parse_ctx;

static void
json_set_err(json_parse_ctx* c, const char* msg) {
    if (c->err == NULL) {
        c->err = msg;
    }
}

static void
json_skip_ws(json_parse_ctx* c) {
    while (c->p < c->end) {
        const char ch = *c->p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            c->p++;
        } else {
            break;
        }
    }
}

static int
json_hex4(const char* p, unsigned int* out) {
    unsigned int v = 0U;
    for (int i = 0; i < 4; i++) {
        const char ch = p[i];
        unsigned int d;
        if (ch >= '0' && ch <= '9') {
            d = (unsigned int)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            d = (unsigned int)(ch - 'a') + 10U;
        } else if (ch >= 'A' && ch <= 'F') {
            d = (unsigned int)(ch - 'A') + 10U;
        } else {
            return -1;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

static int
json_utf8_append(dsd_json_buf* b, unsigned int cp) {
    char tmp[4];
    size_t n;
    if (cp <= 0x7FU) {
        tmp[0] = (char)cp;
        n = 1U;
    } else if (cp <= 0x7FFU) {
        tmp[0] = (char)(0xC0U | (cp >> 6));
        tmp[1] = (char)(0x80U | (cp & 0x3FU));
        n = 2U;
    } else if (cp <= 0xFFFFU) {
        tmp[0] = (char)(0xE0U | (cp >> 12));
        tmp[1] = (char)(0x80U | ((cp >> 6) & 0x3FU));
        tmp[2] = (char)(0x80U | (cp & 0x3FU));
        n = 3U;
    } else {
        tmp[0] = (char)(0xF0U | (cp >> 18));
        tmp[1] = (char)(0x80U | ((cp >> 12) & 0x3FU));
        tmp[2] = (char)(0x80U | ((cp >> 6) & 0x3FU));
        tmp[3] = (char)(0x80U | (cp & 0x3FU));
        n = 4U;
    }
    return dsd_json_buf_append(b, tmp, n);
}

static char*
json_parse_string_raw(json_parse_ctx* c) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    if (!(c->p < c->end) || *c->p != '"') {
        json_set_err(c, "expected string");
        dsd_json_buf_free(&b);
        return NULL;
    }
    c->p++;
    while (c->p < c->end) {
        const unsigned char ch = (unsigned char)*c->p;
        if (ch == '"') {
            c->p++;
            if (b.data == NULL) {
                /* Return an owned empty string. */
                char* empty = (char*)malloc(1U);
                if (empty == NULL) {
                    json_set_err(c, "out of memory");
                    return NULL;
                }
                empty[0] = '\0';
                return empty;
            }
            /* Detach storage; buffer already NUL-terminated by append. */
            char* out = b.data;
            b.data = NULL;
            dsd_json_buf_free(&b);
            return out;
        }
        if (ch == '\\') {
            c->p++;
            if (c->p >= c->end) {
                json_set_err(c, "truncated escape");
                dsd_json_buf_free(&b);
                return NULL;
            }
            const char esc = *c->p++;
            switch (esc) {
                case '"': (void)dsd_json_buf_putc(&b, '"'); break;
                case '\\': (void)dsd_json_buf_putc(&b, '\\'); break;
                case '/': (void)dsd_json_buf_putc(&b, '/'); break;
                case 'b': (void)dsd_json_buf_putc(&b, '\b'); break;
                case 'f': (void)dsd_json_buf_putc(&b, '\f'); break;
                case 'n': (void)dsd_json_buf_putc(&b, '\n'); break;
                case 'r': (void)dsd_json_buf_putc(&b, '\r'); break;
                case 't': (void)dsd_json_buf_putc(&b, '\t'); break;
                case 'u': {
                    if (c->end - c->p < 4) {
                        json_set_err(c, "truncated \\u escape");
                        dsd_json_buf_free(&b);
                        return NULL;
                    }
                    unsigned int cp = 0U;
                    if (json_hex4(c->p, &cp) != 0) {
                        json_set_err(c, "bad \\u escape");
                        dsd_json_buf_free(&b);
                        return NULL;
                    }
                    c->p += 4;
                    if (cp >= 0xD800U && cp <= 0xDBFFU) {
                        /* High surrogate: require a following low surrogate. */
                        if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
                            unsigned int lo = 0U;
                            if (json_hex4(c->p + 2, &lo) == 0 && lo >= 0xDC00U && lo <= 0xDFFFU) {
                                c->p += 6;
                                cp = 0x10000U + ((cp - 0xD800U) << 10) + (lo - 0xDC00U);
                            } else {
                                cp = 0xFFFDU;
                            }
                        } else {
                            cp = 0xFFFDU;
                        }
                    } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                        cp = 0xFFFDU;
                    }
                    if (json_utf8_append(&b, cp) != 0) {
                        json_set_err(c, "out of memory");
                        dsd_json_buf_free(&b);
                        return NULL;
                    }
                    break;
                }
                default:
                    json_set_err(c, "bad escape");
                    dsd_json_buf_free(&b);
                    return NULL;
            }
            continue;
        }
        if (ch < 0x20U) {
            json_set_err(c, "control character in string");
            dsd_json_buf_free(&b);
            return NULL;
        }
        if (dsd_json_buf_putc(&b, (char)ch) != 0) {
            json_set_err(c, "out of memory");
            dsd_json_buf_free(&b);
            return NULL;
        }
        c->p++;
    }
    json_set_err(c, "unterminated string");
    dsd_json_buf_free(&b);
    return NULL;
}

static dsd_json_node* json_parse_value(json_parse_ctx* c);

static dsd_json_node*
json_node_new(json_parse_ctx* c, dsd_json_type type) {
    if (c->nodes >= (size_t)DSD_JSON_MAX_NODES) {
        json_set_err(c, "document too large");
        return NULL;
    }
    dsd_json_node* n = (dsd_json_node*)calloc(1U, sizeof(*n));
    if (n == NULL) {
        json_set_err(c, "out of memory");
        return NULL;
    }
    n->type = type;
    c->nodes++;
    return n;
}

static void
json_free_children(dsd_json_node* n) {
    if (n == NULL || n->items == NULL) {
        return;
    }
    for (size_t i = 0U; i < n->count; i++) {
        dsd_json_free(n->items[i]);
        if (n->keys != NULL) {
            free(n->keys[i]);
        }
    }
    free(n->items);
    free(n->keys);
    n->items = NULL;
    n->keys = NULL;
    n->count = 0U;
}

void
dsd_json_free(dsd_json_node* node) {
    if (node == NULL) {
        return;
    }
    json_free_children(node);
    free(node->string);
    free(node);
}

static int
json_push_item(dsd_json_node* container, char* key, dsd_json_node* value) {
    dsd_json_node** items = (dsd_json_node**)realloc(container->items, (container->count + 1U) * sizeof(*items));
    if (items == NULL) {
        return -1;
    }
    container->items = items;
    if (container->type == DSD_JSON_OBJECT) {
        char** keys = (char**)realloc(container->keys, (container->count + 1U) * sizeof(*keys));
        if (keys == NULL) {
            return -1;
        }
        container->keys = keys;
        container->keys[container->count] = key;
    } else {
        free(key);
    }
    container->items[container->count] = value;
    container->count++;
    return 0;
}

static dsd_json_node*
json_parse_object(json_parse_ctx* c) {
    dsd_json_node* obj = json_node_new(c, DSD_JSON_OBJECT);
    if (obj == NULL) {
        return NULL;
    }
    c->p++; /* consume '{' */
    json_skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return obj;
    }
    for (;;) {
        json_skip_ws(c);
        char* key = json_parse_string_raw(c);
        if (key == NULL) {
            dsd_json_free(obj);
            return NULL;
        }
        json_skip_ws(c);
        if (!(c->p < c->end) || *c->p != ':') {
            free(key);
            json_set_err(c, "expected ':'");
            dsd_json_free(obj);
            return NULL;
        }
        c->p++;
        json_skip_ws(c);
        dsd_json_node* value = json_parse_value(c);
        if (value == NULL) {
            free(key);
            dsd_json_free(obj);
            return NULL;
        }
        if (json_push_item(obj, key, value) != 0) {
            free(key);
            dsd_json_free(value);
            json_set_err(c, "out of memory");
            dsd_json_free(obj);
            return NULL;
        }
        json_skip_ws(c);
        if (!(c->p < c->end)) {
            json_set_err(c, "unterminated object");
            dsd_json_free(obj);
            return NULL;
        }
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == '}') {
            c->p++;
            return obj;
        }
        json_set_err(c, "expected ',' or '}'");
        dsd_json_free(obj);
        return NULL;
    }
}

static dsd_json_node*
json_parse_array(json_parse_ctx* c) {
    dsd_json_node* arr = json_node_new(c, DSD_JSON_ARRAY);
    if (arr == NULL) {
        return NULL;
    }
    c->p++; /* consume '[' */
    json_skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return arr;
    }
    for (;;) {
        json_skip_ws(c);
        dsd_json_node* value = json_parse_value(c);
        if (value == NULL) {
            dsd_json_free(arr);
            return NULL;
        }
        if (json_push_item(arr, NULL, value) != 0) {
            dsd_json_free(value);
            json_set_err(c, "out of memory");
            dsd_json_free(arr);
            return NULL;
        }
        json_skip_ws(c);
        if (!(c->p < c->end)) {
            json_set_err(c, "unterminated array");
            dsd_json_free(arr);
            return NULL;
        }
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == ']') {
            c->p++;
            return arr;
        }
        json_set_err(c, "expected ',' or ']'");
        dsd_json_free(arr);
        return NULL;
    }
}

static dsd_json_node*
json_parse_number(json_parse_ctx* c) {
    const char* start = c->p;
    if (c->p < c->end && (*c->p == '-' || *c->p == '+')) {
        c->p++;
    }
    int digits = 0;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        c->p++;
        digits++;
    }
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            c->p++;
            digits++;
        }
    }
    if (digits == 0) {
        json_set_err(c, "invalid number");
        return NULL;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '-' || *c->p == '+')) {
            c->p++;
        }
        int exp_digits = 0;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            c->p++;
            exp_digits++;
        }
        if (exp_digits == 0) {
            json_set_err(c, "invalid exponent");
            return NULL;
        }
    }
    dsd_json_node* n = json_node_new(c, DSD_JSON_NUMBER);
    if (n == NULL) {
        return NULL;
    }
    char tmp[64];
    const size_t len = (size_t)(c->p - start);
    if (len >= sizeof tmp) {
        json_set_err(c, "number too long");
        dsd_json_free(n);
        return NULL;
    }
    memcpy(tmp, start, len);
    tmp[len] = '\0';
    n->number = strtod(tmp, NULL);
    return n;
}

static dsd_json_node*
json_parse_value(json_parse_ctx* c) {
    if (!(c->p < c->end)) {
        json_set_err(c, "unexpected end of input");
        return NULL;
    }
    const char ch = *c->p;
    if (ch == '{') {
        return json_parse_object(c);
    }
    if (ch == '[') {
        return json_parse_array(c);
    }
    if (ch == '"') {
        dsd_json_node* n = json_node_new(c, DSD_JSON_STRING);
        if (n == NULL) {
            return NULL;
        }
        n->string = json_parse_string_raw(c);
        if (n->string == NULL) {
            dsd_json_free(n);
            return NULL;
        }
        return n;
    }
    if (c->end - c->p >= 4 && memcmp(c->p, "true", 4U) == 0) {
        c->p += 4;
        dsd_json_node* n = json_node_new(c, DSD_JSON_BOOL);
        if (n != NULL) {
            n->boolean = 1;
        }
        return n;
    }
    if (c->end - c->p >= 5 && memcmp(c->p, "false", 5U) == 0) {
        c->p += 5;
        dsd_json_node* n = json_node_new(c, DSD_JSON_BOOL);
        if (n != NULL) {
            n->boolean = 0;
        }
        return n;
    }
    if (c->end - c->p >= 4 && memcmp(c->p, "null", 4U) == 0) {
        c->p += 4;
        return json_node_new(c, DSD_JSON_NULL);
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        return json_parse_number(c);
    }
    json_set_err(c, "unexpected character");
    return NULL;
}

int
dsd_json_parse(const char* text, dsd_json_node** out, char* err, size_t errsz) {
    if (text == NULL || out == NULL) {
        return -1;
    }
    *out = NULL;
    json_parse_ctx c;
    c.p = text;
    c.end = text + strlen(text);
    c.nodes = 0U;
    c.err = NULL;
    json_skip_ws(&c);
    dsd_json_node* root = json_parse_value(&c);
    if (root == NULL) {
        if (err != NULL && errsz > 0U) {
            snprintf(err, errsz, "%s", c.err ? c.err : "parse error");
        }
        return -1;
    }
    json_skip_ws(&c);
    if (c.p != c.end) {
        if (err != NULL && errsz > 0U) {
            snprintf(err, errsz, "trailing content after JSON value");
        }
        dsd_json_free(root);
        return -1;
    }
    *out = root;
    return 0;
}

const dsd_json_node*
dsd_json_obj_get(const dsd_json_node* obj, const char* key) {
    if (obj == NULL || obj->type != DSD_JSON_OBJECT || key == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < obj->count; i++) {
        if (obj->keys != NULL && obj->keys[i] != NULL && strcmp(obj->keys[i], key) == 0) {
            return obj->items[i];
        }
    }
    return NULL;
}

const char*
dsd_json_as_str(const dsd_json_node* n) {
    if (n == NULL || n->type != DSD_JSON_STRING) {
        return NULL;
    }
    return n->string;
}

const char*
dsd_json_as_str_bounded(const dsd_json_node* n, size_t cap) {
    const char* s = dsd_json_as_str(n);
    if (s == NULL) {
        return NULL;
    }
    const size_t len = strlen(s);
    if (len >= cap) {
        return NULL;
    }
    for (size_t i = 0U; i < len; i++) {
        if (s[i] == '\0') {
            return NULL;
        }
    }
    return s;
}

int
dsd_json_as_i64(const dsd_json_node* n, int64_t* out) {
    if (n == NULL || out == NULL) {
        return -1;
    }
    if (n->type == DSD_JSON_NUMBER) {
        *out = (int64_t)n->number;
        return 0;
    }
    if (n->type == DSD_JSON_BOOL) {
        *out = n->boolean ? 1 : 0;
        return 0;
    }
    return -1;
}

int
dsd_json_as_u64(const dsd_json_node* n, uint64_t* out) {
    if (n == NULL || out == NULL) {
        return -1;
    }
    if (n->type == DSD_JSON_NUMBER) {
        if (n->number < 0.0) {
            return -1;
        }
        *out = (uint64_t)n->number;
        return 0;
    }
    if (n->type == DSD_JSON_BOOL) {
        *out = n->boolean ? 1U : 0U;
        return 0;
    }
    return -1;
}

int
dsd_json_as_double(const dsd_json_node* n, double* out) {
    if (n == NULL || out == NULL) {
        return -1;
    }
    if (n->type == DSD_JSON_NUMBER) {
        *out = n->number;
        return 0;
    }
    if (n->type == DSD_JSON_BOOL) {
        *out = n->boolean ? 1.0 : 0.0;
        return 0;
    }
    return -1;
}

int
dsd_json_as_bool(const dsd_json_node* n, int* out) {
    if (n == NULL || out == NULL) {
        return -1;
    }
    if (n->type == DSD_JSON_BOOL) {
        *out = n->boolean;
        return 0;
    }
    if (n->type == DSD_JSON_NUMBER) {
        *out = (n->number != 0.0) ? 1 : 0;
        return 0;
    }
    return -1;
}
