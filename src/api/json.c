// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Implementation of the small JSON writer/parser declared in json.h.
 *
 * Built without fast-math (src/api/CMakeLists.txt): the writer's isfinite() and
 * the parser's overflow check must see real infinities and NaNs. A telemetry
 * value comes from fast-math code, so it passes through dsd_fp_opaque_d() first
 * (docs/code_map.md, "Fast-math and non-finite values").
 */

#include "json.h"

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/fp_opaque.h>

#include <errno.h>
#include <math.h>
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
    if (b->data != NULL) {
        /* Request buffers can hold key material; every buffer is wiped before its storage goes back. */
        DSD_SECURE_ZERO(b->data, b->cap);
        free(b->data);
    }
    dsd_json_buf_init(b);
}

void
dsd_json_buf_reset(dsd_json_buf* b) {
    if (b == NULL) {
        return;
    }
    b->len = 0U;
    b->oom = 0;
    if (b->data != NULL) {
        b->data[0] = '\0';
    }
}

void
dsd_json_buf_consume(dsd_json_buf* b, size_t n) {
    if (b == NULL || b->data == NULL || n == 0U) {
        return;
    }
    /* The consumed bytes are wiped, not left behind: a request line can carry key material. */
    if (n >= b->len) {
        DSD_SECURE_ZERO(b->data, b->len);
        b->len = 0U;
        b->data[0] = '\0';
        return;
    }
    const size_t rest = b->len - n;
    DSD_MEMMOVE(b->data, b->data + n, rest);
    DSD_SECURE_ZERO(b->data + rest, n);
    b->len = rest;
    b->data[rest] = '\0';
}

int
dsd_json_buf_reserve(dsd_json_buf* b, size_t extra) {
    if (b == NULL || b->oom) {
        return -1;
    }
    if (extra > ((size_t)-1) - b->len - 1U) {
        b->oom = 1;
        return -1;
    }
    const size_t need = b->len + extra + 1U; /* +1 for the NUL terminator */
    if (need <= b->cap && b->data != NULL) {
        return 0;
    }
    size_t cap = (b->cap == 0U) ? 256U : b->cap;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2U) {
            b->oom = 1;
            return -1;
        }
        cap *= 2U;
    }
    /* Not realloc(): the old block is wiped before it is freed. */
    char* grown = (char*)malloc(cap);
    if (grown == NULL) {
        b->oom = 1;
        return -1;
    }
    if (b->data != NULL) {
        DSD_MEMCPY(grown, b->data, b->len + 1U);
        DSD_SECURE_ZERO(b->data, b->cap);
        free(b->data);
    } else {
        grown[0] = '\0';
    }
    b->data = grown;
    b->cap = cap;
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
    DSD_MEMCPY(b->data + b->len, data, n);
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
 * UTF-8 and escaping
 *============================================================================*/

/* Length of the well-formed UTF-8 sequence at p (RFC 3629: no overlongs, no
 * surrogates, nothing above U+10FFFF), or 0 when the bytes there are not one. */
static size_t
json_utf8_seq_len(const unsigned char* p, const unsigned char* end) {
    if (p >= end) {
        return 0U;
    }
    const unsigned char c = p[0];
    if (c < 0x80U) {
        return 1U;
    }
    size_t seq = 0U;
    unsigned char lo = 0x80U;
    unsigned char hi = 0xBFU;
    if (c >= 0xC2U && c <= 0xDFU) {
        seq = 2U;
    } else if (c == 0xE0U) {
        seq = 3U;
        lo = 0xA0U;
    } else if ((c >= 0xE1U && c <= 0xECU) || c == 0xEEU || c == 0xEFU) {
        seq = 3U;
    } else if (c == 0xEDU) {
        seq = 3U;
        hi = 0x9FU;
    } else if (c == 0xF0U) {
        seq = 4U;
        lo = 0x90U;
    } else if (c >= 0xF1U && c <= 0xF3U) {
        seq = 4U;
    } else if (c == 0xF4U) {
        seq = 4U;
        hi = 0x8FU;
    } else {
        return 0U;
    }
    if ((size_t)(end - p) < seq || p[1] < lo || p[1] > hi) {
        return 0U;
    }
    for (size_t i = 2U; i < seq; i++) {
        if ((p[i] & 0xC0U) != 0x80U) {
            return 0U;
        }
    }
    return seq;
}

static int
json_append_u_escape(dsd_json_buf* b, unsigned int cp) {
    static const char hex[] = "0123456789ABCDEF";
    char esc[6] = {'\\', 'u', hex[(cp >> 12) & 0xFU], hex[(cp >> 8) & 0xFU], hex[(cp >> 4) & 0xFU], hex[cp & 0xFU]};
    return dsd_json_buf_append(b, esc, sizeof esc);
}

int
dsd_json_buf_append_escaped(dsd_json_buf* b, const char* v) {
    if (b == NULL) {
        return -1;
    }
    if (v == NULL) {
        return 0;
    }
    const unsigned char* p = (const unsigned char*)v;
    const unsigned char* end = p + strlen(v);
    while (p < end) {
        const unsigned char c = *p;
        const char* short_esc = NULL;
        switch (c) {
            case '"': short_esc = "\\\""; break;
            case '\\': short_esc = "\\\\"; break;
            case '\b': short_esc = "\\b"; break;
            case '\f': short_esc = "\\f"; break;
            case '\n': short_esc = "\\n"; break;
            case '\r': short_esc = "\\r"; break;
            case '\t': short_esc = "\\t"; break;
            default: break;
        }
        if (short_esc != NULL) {
            if (dsd_json_buf_append(b, short_esc, 2U) != 0) {
                return -1;
            }
            p++;
            continue;
        }
        if (c < 0x20U || c == 0x7FU) {
            if (json_append_u_escape(b, c) != 0) {
                return -1;
            }
            p++;
            continue;
        }
        if (c < 0x80U) {
            if (dsd_json_buf_putc(b, (char)c) != 0) {
                return -1;
            }
            p++;
            continue;
        }
        const size_t seq = json_utf8_seq_len(p, end);
        if (seq == 0U) {
            /* Not UTF-8 (off-air bytes, a Latin-1 CSV): U+FFFD keeps the line valid. */
            if (dsd_json_buf_append(b, "\xEF\xBF\xBD", 3U) != 0) {
                return -1;
            }
            p++;
            continue;
        }
        if (seq == 3U && p[0] == 0xE2U && p[1] == 0x80U && (p[2] == 0xA8U || p[2] == 0xA9U)) {
            /* U+2028/U+2029 end a line for some JSON readers. */
            if (json_append_u_escape(b, 0x2000U | ((unsigned int)p[2] & 0x3FU)) != 0) {
                return -1;
            }
            p += seq;
            continue;
        }
        if (dsd_json_buf_append(b, (const char*)p, seq) != 0) {
            return -1;
        }
        p += seq;
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
    DSD_MEMSET(w, 0, sizeof(*w));
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

/* Emit the separator due for a value at the current level. */
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
    if (level > 0 && w->is_obj[level]) {
        /* An object member needs its key first. */
        return json_writer_fail(w);
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
    if (level <= 0 || !w->is_obj[level] || w->after_key[level]) {
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
    if (json_writer_puts(w, "\":") != 0) {
        return -1;
    }
    w->after_key[level] = 1U;
    return 0;
}

static int
json_open(dsd_json_writer* w, char brace, int is_obj) {
    if (w == NULL || w->depth + 1 >= DSD_JSON_MAX_DEPTH) {
        return json_writer_fail(w);
    }
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    if (json_writer_putc(w, brace) != 0) {
        return -1;
    }
    w->depth++;
    w->first[w->depth] = 1U;
    w->is_obj[w->depth] = (unsigned char)(is_obj ? 1 : 0);
    w->after_key[w->depth] = 0U;
    return 0;
}

static int
json_close(dsd_json_writer* w, char brace, int is_obj) {
    if (dsd_json_writer_failed(w) || w->depth <= 0) {
        return json_writer_fail(w);
    }
    if (w->after_key[w->depth] || w->is_obj[w->depth] != (unsigned char)(is_obj ? 1 : 0)) {
        /* A key with no value, or a mismatched close, is malformed. */
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
    return json_close(w, '}', 1);
}

int
dsd_json_arr_end(dsd_json_writer* w) {
    return json_close(w, ']', 0);
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
    const int n = DSD_SNPRINTF(tmp, sizeof tmp, "%lld", (long long)v);
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
    const int n = DSD_SNPRINTF(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        return json_writer_fail(w);
    }
    return json_writer_puts(w, tmp);
}

int
dsd_json_value_u64_str(dsd_json_writer* w, uint64_t v) {
    char tmp[32];
    const int n = DSD_SNPRINTF(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        return json_writer_fail(w);
    }
    return dsd_json_value_str(w, tmp);
}

int
dsd_json_value_double(dsd_json_writer* w, double v) {
    if (json_writer_before_value(w) != 0) {
        return -1;
    }
    if (!isfinite(dsd_fp_opaque_d(v))) {
        return json_writer_puts(w, "null");
    }
    char tmp[40];
    /* 15 significant digits is the most a double holds for every decimal value; "%g" never writes a bare "." or a
       leading "+", and LC_NUMERIC stays "C" in this program (only LC_CTYPE is localized). */
    const int n = DSD_SNPRINTF(tmp, sizeof tmp, "%.15g", v);
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
    if (raw == NULL || raw[0] == '\0') {
        return json_writer_fail(w);
    }
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
dsd_json_kv_u64_str(dsd_json_writer* w, const char* key, uint64_t v) {
    return (dsd_json_key(w, key) == 0) ? dsd_json_value_u64_str(w, v) : -1;
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
    int depth;
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
        const int d = dsd_hex_nibble_value((unsigned char)p[i]);
        if (d < 0) {
            return -1;
        }
        v = (v << 4) | (unsigned int)d;
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

/* Read a \uXXXX escape (the "\u" already consumed) into a code point. A lone surrogate becomes U+FFFD. */
static int
json_parse_u_escape(json_parse_ctx* c, unsigned int* out) {
    unsigned int cp = 0U;
    if (c->end - c->p < 4 || json_hex4(c->p, &cp) != 0) {
        json_set_err(c, "bad \\u escape");
        return -1;
    }
    c->p += 4;
    if (cp >= 0xD800U && cp <= 0xDBFFU) {
        unsigned int lo = 0U;
        if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u' && json_hex4(c->p + 2, &lo) == 0 && lo >= 0xDC00U
            && lo <= 0xDFFFU) {
            c->p += 6;
            cp = 0x10000U + ((cp - 0xD800U) << 10) + (lo - 0xDC00U);
        } else {
            cp = 0xFFFDU;
        }
    } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
        cp = 0xFFFDU;
    }
    if (cp == 0U) {
        /* Every consumer copies strings as C strings; a NUL would silently cut one short. */
        json_set_err(c, "NUL in string");
        return -1;
    }
    *out = cp;
    return 0;
}

static char*
json_parse_string_raw(json_parse_ctx* c) {
    if (!(c->p < c->end) || *c->p != '"') {
        json_set_err(c, "expected string");
        return NULL;
    }
    c->p++;
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    if (dsd_json_buf_reserve(&b, 0U) != 0) {
        json_set_err(c, "out of memory");
        return NULL;
    }
    while (c->p < c->end) {
        const unsigned char ch = (unsigned char)*c->p;
        if (ch == '"') {
            c->p++;
            /* Detach the storage; the buffer is always NUL-terminated. */
            char* out = b.data;
            b.data = NULL;
            return out;
        }
        int rc = 0;
        if (ch == '\\') {
            c->p++;
            if (c->p >= c->end) {
                json_set_err(c, "truncated escape");
                break;
            }
            const char esc = *c->p++;
            switch (esc) {
                case '"': rc = dsd_json_buf_putc(&b, '"'); break;
                case '\\': rc = dsd_json_buf_putc(&b, '\\'); break;
                case '/': rc = dsd_json_buf_putc(&b, '/'); break;
                case 'b': rc = dsd_json_buf_putc(&b, '\b'); break;
                case 'f': rc = dsd_json_buf_putc(&b, '\f'); break;
                case 'n': rc = dsd_json_buf_putc(&b, '\n'); break;
                case 'r': rc = dsd_json_buf_putc(&b, '\r'); break;
                case 't': rc = dsd_json_buf_putc(&b, '\t'); break;
                case 'u': {
                    unsigned int cp = 0U;
                    if (json_parse_u_escape(c, &cp) != 0) {
                        dsd_json_buf_free(&b);
                        return NULL;
                    }
                    rc = json_utf8_append(&b, cp);
                    break;
                }
                default:
                    json_set_err(c, "bad escape");
                    dsd_json_buf_free(&b);
                    return NULL;
            }
        } else if (ch < 0x20U) {
            json_set_err(c, "control character in string");
            break;
        } else {
            const size_t seq = json_utf8_seq_len((const unsigned char*)c->p, (const unsigned char*)c->end);
            if (seq == 0U) {
                json_set_err(c, "invalid UTF-8 in string");
                break;
            }
            rc = dsd_json_buf_append(&b, c->p, seq);
            c->p += seq;
        }
        if (rc != 0) {
            json_set_err(c, "out of memory");
            break;
        }
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
    if (node->string != NULL) {
        /* Requests carry keys and tokens as strings or number literals; erase them with the tree. */
        DSD_SECURE_ZERO(node->string, strlen(node->string));
        free(node->string);
    }
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
    }
    container->items[container->count] = value;
    container->count++;
    return 0;
}

static int
json_enter(json_parse_ctx* c) {
    if (c->depth >= DSD_JSON_MAX_DEPTH) {
        json_set_err(c, "nesting too deep");
        return -1;
    }
    c->depth++;
    return 0;
}

static dsd_json_node*
json_parse_object(json_parse_ctx* c) {
    if (json_enter(c) != 0) {
        return NULL;
    }
    dsd_json_node* obj = json_node_new(c, DSD_JSON_OBJECT);
    if (obj == NULL) {
        return NULL;
    }
    c->p++; /* consume '{' */
    json_skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        c->depth--;
        return obj;
    }
    for (;;) {
        json_skip_ws(c);
        char* key = json_parse_string_raw(c);
        if (key == NULL) {
            break;
        }
        json_skip_ws(c);
        if (!(c->p < c->end) || *c->p != ':') {
            free(key);
            json_set_err(c, "expected ':'");
            break;
        }
        c->p++;
        json_skip_ws(c);
        dsd_json_node* value = json_parse_value(c);
        if (value == NULL) {
            free(key);
            break;
        }
        if (json_push_item(obj, key, value) != 0) {
            free(key);
            dsd_json_free(value);
            json_set_err(c, "out of memory");
            break;
        }
        json_skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            c->depth--;
            return obj;
        }
        json_set_err(c, "expected ',' or '}'");
        break;
    }
    dsd_json_free(obj);
    return NULL;
}

static dsd_json_node*
json_parse_array(json_parse_ctx* c) {
    if (json_enter(c) != 0) {
        return NULL;
    }
    dsd_json_node* arr = json_node_new(c, DSD_JSON_ARRAY);
    if (arr == NULL) {
        return NULL;
    }
    c->p++; /* consume '[' */
    json_skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        c->depth--;
        return arr;
    }
    for (;;) {
        json_skip_ws(c);
        dsd_json_node* value = json_parse_value(c);
        if (value == NULL) {
            break;
        }
        if (json_push_item(arr, NULL, value) != 0) {
            dsd_json_free(value);
            json_set_err(c, "out of memory");
            break;
        }
        json_skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            c->depth--;
            return arr;
        }
        json_set_err(c, "expected ',' or ']'");
        break;
    }
    dsd_json_free(arr);
    return NULL;
}

static int
json_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

/* RFC 8259 number: -? (0 | [1-9][0-9]*) (\.[0-9]+)? ([eE][+-]?[0-9]+)? */
static dsd_json_node*
json_parse_number(json_parse_ctx* c) {
    const char* start = c->p;
    if (c->p < c->end && *c->p == '-') {
        c->p++;
    }
    if (!(c->p < c->end) || !json_is_digit(*c->p)) {
        json_set_err(c, "invalid number");
        return NULL;
    }
    if (*c->p == '0') {
        c->p++;
        if (c->p < c->end && json_is_digit(*c->p)) {
            json_set_err(c, "invalid number (leading zero)");
            return NULL;
        }
    } else {
        while (c->p < c->end && json_is_digit(*c->p)) {
            c->p++;
        }
    }
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        if (!(c->p < c->end) || !json_is_digit(*c->p)) {
            json_set_err(c, "invalid number (fraction)");
            return NULL;
        }
        while (c->p < c->end && json_is_digit(*c->p)) {
            c->p++;
        }
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '-' || *c->p == '+')) {
            c->p++;
        }
        if (!(c->p < c->end) || !json_is_digit(*c->p)) {
            json_set_err(c, "invalid number (exponent)");
            return NULL;
        }
        while (c->p < c->end && json_is_digit(*c->p)) {
            c->p++;
        }
    }
    const size_t len = (size_t)(c->p - start);
    if (len > (size_t)DSD_JSON_MAX_NUMBER_LEN) {
        json_set_err(c, "number too long");
        return NULL;
    }
    char* literal = (char*)malloc(len + 1U);
    if (literal == NULL) {
        json_set_err(c, "out of memory");
        return NULL;
    }
    DSD_MEMCPY(literal, start, len);
    literal[len] = '\0';
    errno = 0;
    char* end = NULL;
    const double value = strtod(literal, &end);
    if (end != literal + len || !isfinite(value)) {
        free(literal);
        json_set_err(c, "number out of range");
        return NULL;
    }
    dsd_json_node* n = json_node_new(c, DSD_JSON_NUMBER);
    if (n == NULL) {
        free(literal);
        return NULL;
    }
    n->number = value;
    n->string = literal;
    return n;
}

static int
json_match_word(json_parse_ctx* c, const char* word) {
    const size_t len = strlen(word);
    if ((size_t)(c->end - c->p) < len || memcmp(c->p, word, len) != 0) {
        return 0;
    }
    c->p += len;
    return 1;
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
        char* s = json_parse_string_raw(c);
        if (s == NULL) {
            return NULL;
        }
        dsd_json_node* n = json_node_new(c, DSD_JSON_STRING);
        if (n == NULL) {
            free(s);
            return NULL;
        }
        n->string = s;
        return n;
    }
    if (json_match_word(c, "true")) {
        dsd_json_node* n = json_node_new(c, DSD_JSON_BOOL);
        if (n != NULL) {
            n->boolean = 1;
        }
        return n;
    }
    if (json_match_word(c, "false")) {
        return json_node_new(c, DSD_JSON_BOOL);
    }
    if (json_match_word(c, "null")) {
        return json_node_new(c, DSD_JSON_NULL);
    }
    if (ch == '-' || json_is_digit(ch)) {
        return json_parse_number(c);
    }
    json_set_err(c, "unexpected character");
    return NULL;
}

int
dsd_json_parse(const char* text, dsd_json_node** out, char* err, size_t errsz) {
    if (out != NULL) {
        *out = NULL;
    }
    if (text == NULL || out == NULL) {
        return -1;
    }
    json_parse_ctx c;
    c.p = text;
    c.end = text + strlen(text);
    c.nodes = 0U;
    c.depth = 0;
    c.err = NULL;
    json_skip_ws(&c);
    dsd_json_node* root = json_parse_value(&c);
    if (root != NULL) {
        json_skip_ws(&c);
        if (c.p != c.end) {
            json_set_err(&c, "trailing content after JSON value");
            dsd_json_free(root);
            root = NULL;
        }
    }
    if (root == NULL) {
        if (err != NULL && errsz > 0U) {
            DSD_SNPRINTF(err, errsz, "%s", c.err ? c.err : "parse error");
        }
        return -1;
    }
    *out = root;
    return 0;
}

const dsd_json_node*
dsd_json_obj_get(const dsd_json_node* obj, const char* key) {
    if (obj == NULL || obj->type != DSD_JSON_OBJECT || key == NULL || obj->keys == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < obj->count; i++) {
        if (obj->keys[i] != NULL && strcmp(obj->keys[i], key) == 0) {
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
    if (s == NULL || cap == 0U || strlen(s) >= cap) {
        return NULL;
    }
    return s;
}

/* The literal of an integer-valued number node: no fraction, no exponent. */
static const char*
json_integer_literal(const dsd_json_node* n) {
    if (n == NULL || n->type != DSD_JSON_NUMBER || n->string == NULL) {
        return NULL;
    }
    return strpbrk(n->string, ".eE") == NULL ? n->string : NULL;
}

static int
json_all_digits(const char* s, int hex) {
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    for (const char* p = s; *p != '\0'; p++) {
        if (hex ? dsd_hex_nibble_value((unsigned char)*p) < 0 : !json_is_digit(*p)) {
            return 0;
        }
    }
    return 1;
}

int
dsd_json_as_i64(const dsd_json_node* n, int64_t* out) {
    const char* lit = json_integer_literal(n);
    if (lit == NULL || out == NULL) {
        return -1;
    }
    errno = 0;
    char* end = NULL;
    const long long v = strtoll(lit, &end, 10);
    if (errno != 0 || end == lit || end == NULL || *end != '\0') {
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

int
dsd_json_as_u64(const dsd_json_node* n, uint64_t* out) {
    if (n == NULL || out == NULL) {
        return -1;
    }
    const char* text = NULL;
    int hex = 0;
    if (n->type == DSD_JSON_NUMBER) {
        text = json_integer_literal(n);
    } else if (n->type == DSD_JSON_STRING && n->string != NULL) {
        text = n->string;
        if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            text += 2;
            hex = 1;
        }
    }
    if (!json_all_digits(text, hex)) {
        return -1;
    }
    uint64_t v = 0U;
    if (hex) {
        if (dsd_parse_hex_u64_n(text, strlen(text), &v) != 0) {
            return -1;
        }
    } else if (dsd_parse_uint64_strict(text, 10, UINT64_MAX, &v) != 0) {
        return -1;
    }
    *out = v;
    return 0;
}

int
dsd_json_as_double(const dsd_json_node* n, double* out) {
    if (n == NULL || out == NULL || n->type != DSD_JSON_NUMBER) {
        return -1;
    }
    *out = n->number; /* finite: the parser refuses anything else */
    return 0;
}

int
dsd_json_as_bool(const dsd_json_node* n, int* out) {
    if (n == NULL || out == NULL) {
        return -1;
    }
    if (n->type == DSD_JSON_BOOL) {
        *out = n->boolean ? 1 : 0;
        return 0;
    }
    int64_t v = 0;
    if (dsd_json_as_i64(n, &v) == 0 && (v == 0 || v == 1)) {
        *out = (int)v;
        return 0;
    }
    return -1;
}
