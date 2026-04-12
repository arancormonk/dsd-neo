// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/platform/posix_compat.h> // IWYU pragma: keep (MSVC stat/_stat compatibility)

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dsd-neo/io/iq_types.h"

struct dsd_iq_replay_source {
    FILE* fp;
    uint64_t total_bytes;
    uint64_t remaining_bytes;
};

static void
set_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) {
    if (!err_buf || err_buf_size == 0 || !fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    (void)vsnprintf(err_buf, err_buf_size, fmt, ap);
    va_end(ap);
    err_buf[err_buf_size - 1] = '\0';
}

static int
copy_string_checked(const char* src, char* dst, size_t dst_size, char* err_buf, size_t err_buf_size, const char* name) {
    if (!dst || dst_size == 0) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (!src) {
        src = "";
    }
    size_t n = strlen(src);
    if (n + 1 > dst_size) {
        set_error(err_buf, err_buf_size, "%s is too long", (name) ? name : "string");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    memcpy(dst, src, n + 1);
    return DSD_IQ_OK;
}

static int
path_is_absolute(const char* path) {
    if (!path || path[0] == '\0') {
        return 0;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return 1;
    }
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
        return 1;
    }
    return 0;
}

static int
has_suffix(const char* s, const char* suffix) {
    if (!s || !suffix) {
        return 0;
    }
    size_t s_len = strlen(s);
    size_t suf_len = strlen(suffix);
    if (s_len < suf_len) {
        return 0;
    }
    return strcmp(s + (s_len - suf_len), suffix) == 0;
}

static int
file_size_u64(const char* path, uint64_t* out_size) {
    if (!path || !out_size) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return DSD_IQ_ERR_IO;
    }
    if (st.st_size < 0) {
        return DSD_IQ_ERR_IO;
    }
    *out_size = (uint64_t)st.st_size;
    return DSD_IQ_OK;
}

static int
read_file_all(const char* path, char** out_data, size_t* out_size, char* err_buf, size_t err_buf_size) {
    if (!path || !out_data || !out_size) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_size = 0;

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        set_error(err_buf, err_buf_size, "failed to open metadata file '%s': %s", path, strerror(errno));
        return DSD_IQ_ERR_IO;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        set_error(err_buf, err_buf_size, "failed to seek metadata file '%s': %s", path, strerror(errno));
        fclose(fp);
        return DSD_IQ_ERR_IO;
    }
    long end = ftell(fp);
    if (end < 0) {
        set_error(err_buf, err_buf_size, "failed to tell metadata file '%s': %s", path, strerror(errno));
        fclose(fp);
        return DSD_IQ_ERR_IO;
    }
    if ((unsigned long)end > (unsigned long)(SIZE_MAX - 1U)) {
        set_error(err_buf, err_buf_size, "metadata file '%s' is too large", path);
        fclose(fp);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        set_error(err_buf, err_buf_size, "failed to rewind metadata file '%s': %s", path, strerror(errno));
        fclose(fp);
        return DSD_IQ_ERR_IO;
    }

    size_t sz = (size_t)end;
    char* data = (char*)calloc(sz + 1U, sizeof(char));
    if (!data) {
        set_error(err_buf, err_buf_size, "metadata allocation failed");
        fclose(fp);
        return DSD_IQ_ERR_ALLOC;
    }

    size_t n = fread(data, 1, sz, fp);
    if (n != sz) {
        set_error(err_buf, err_buf_size, "failed to read metadata file '%s'", path);
        free(data);
        fclose(fp);
        return DSD_IQ_ERR_IO;
    }
    fclose(fp);

    *out_data = data;
    *out_size = sz;
    return DSD_IQ_OK;
}

static int
resolve_metadata_path(const char* path, char* out_metadata_path, size_t out_metadata_path_size, char* err_buf,
                      size_t err_buf_size) {
    if (!path || !out_metadata_path || out_metadata_path_size == 0) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (has_suffix(path, ".json")) {
        return copy_string_checked(path, out_metadata_path, out_metadata_path_size, err_buf, err_buf_size,
                                   "metadata path");
    }

    size_t n = strlen(path);
    if (n + 6U > out_metadata_path_size) {
        set_error(err_buf, err_buf_size, "metadata path too long");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    snprintf(out_metadata_path, out_metadata_path_size, "%s.json", path);

    struct stat st;
    if (stat(out_metadata_path, &st) != 0) {
        set_error(err_buf, err_buf_size, "metadata sidecar not found for '%s' (expected '%s')", path,
                  out_metadata_path);
        return DSD_IQ_ERR_IO;
    }
    return DSD_IQ_OK;
}

static int
resolve_data_path(const char* metadata_path, const char* data_file, char* out_data_path, size_t out_data_path_size,
                  char* err_buf, size_t err_buf_size) {
    if (!metadata_path || !data_file || !out_data_path || out_data_path_size == 0) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (path_is_absolute(data_file)) {
        return copy_string_checked(data_file, out_data_path, out_data_path_size, err_buf, err_buf_size, "data path");
    }

    const char* slash = strrchr(metadata_path, '/');
    const char* bslash = strrchr(metadata_path, '\\');
    const char* base = slash;
    if (bslash && (!base || bslash > base)) {
        base = bslash;
    }
    if (!base) {
        return copy_string_checked(data_file, out_data_path, out_data_path_size, err_buf, err_buf_size, "data path");
    }

    size_t dir_len = (size_t)(base - metadata_path + 1);
    size_t file_len = strlen(data_file);
    if (dir_len + file_len + 1 > out_data_path_size) {
        set_error(err_buf, err_buf_size, "resolved data path too long");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    memcpy(out_data_path, metadata_path, dir_len);
    memcpy(out_data_path + dir_len, data_file, file_len);
    out_data_path[dir_len + file_len] = '\0';
    return DSD_IQ_OK;
}

typedef enum {
    JTOK_ERROR = -1,
    JTOK_EOF = 0,
    JTOK_LBRACE,
    JTOK_RBRACE,
    JTOK_LBRACKET,
    JTOK_RBRACKET,
    JTOK_COMMA,
    JTOK_COLON,
    JTOK_STRING,
    JTOK_NUMBER_INT,
    JTOK_NUMBER_FLOAT,
    JTOK_TRUE,
    JTOK_FALSE,
    JTOK_NULL,
} json_token_type;

typedef struct {
    const char* src;
    size_t src_len;
    size_t pos;
    char str_buf[4096];
    const char* err_msg;
    size_t err_pos;
} json_tokenizer;

typedef struct {
    json_token_type type;
    size_t offset;
    const char* str;
    size_t str_len;
    const char* num;
    size_t num_len;
} json_token;

static void
tokenizer_set_error(json_tokenizer* tk, const char* msg, size_t pos) {
    if (!tk->err_msg) {
        tk->err_msg = msg;
        tk->err_pos = pos;
    }
}

static int
hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static json_token
make_simple_token(json_token_type type, size_t offset) {
    json_token tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = type;
    tok.offset = offset;
    return tok;
}

static void
skip_ws(json_tokenizer* tk) {
    while (tk->pos < tk->src_len) {
        unsigned char c = (unsigned char)tk->src[tk->pos];
        if (c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D) {
            tk->pos++;
            continue;
        }
        break;
    }
}

static json_token
tokenizer_next(json_tokenizer* tk) {
    json_token tok = make_simple_token(JTOK_ERROR, tk->pos);
    if (tk->err_msg) {
        return tok;
    }

    skip_ws(tk);
    if (tk->pos >= tk->src_len) {
        return make_simple_token(JTOK_EOF, tk->pos);
    }

    size_t start = tk->pos;
    char c = tk->src[tk->pos];
    switch (c) {
        case '{': tk->pos++; return make_simple_token(JTOK_LBRACE, start);
        case '}': tk->pos++; return make_simple_token(JTOK_RBRACE, start);
        case '[': tk->pos++; return make_simple_token(JTOK_LBRACKET, start);
        case ']': tk->pos++; return make_simple_token(JTOK_RBRACKET, start);
        case ',': tk->pos++; return make_simple_token(JTOK_COMMA, start);
        case ':': tk->pos++; return make_simple_token(JTOK_COLON, start);
        default: break;
    }

    if (c == '"') {
        tk->pos++;
        size_t w = 0;
        while (tk->pos < tk->src_len) {
            unsigned char ch = (unsigned char)tk->src[tk->pos++];
            if (ch < 0x20) {
                tokenizer_set_error(tk, "unescaped control char in string", tk->pos - 1);
                return tok;
            }
            if (ch == '"') {
                tk->str_buf[w] = '\0';
                tok.type = JTOK_STRING;
                tok.offset = start;
                tok.str = tk->str_buf;
                tok.str_len = w;
                return tok;
            }
            if (ch == '\\') {
                if (tk->pos >= tk->src_len) {
                    tokenizer_set_error(tk, "truncated escape sequence", tk->pos);
                    return tok;
                }
                unsigned char esc = (unsigned char)tk->src[tk->pos++];
                unsigned char out_ch = 0;
                int have_char = 1;
                switch (esc) {
                    case '"': out_ch = '"'; break;
                    case '\\': out_ch = '\\'; break;
                    case '/': out_ch = '/'; break;
                    case 'b': out_ch = '\b'; break;
                    case 'f': out_ch = '\f'; break;
                    case 'n': out_ch = '\n'; break;
                    case 'r': out_ch = '\r'; break;
                    case 't': out_ch = '\t'; break;
                    case 'u': {
                        if (tk->pos + 4 > tk->src_len) {
                            tokenizer_set_error(tk, "truncated unicode escape", tk->pos);
                            return tok;
                        }
                        int h0 = hex_value(tk->src[tk->pos + 0]);
                        int h1 = hex_value(tk->src[tk->pos + 1]);
                        int h2 = hex_value(tk->src[tk->pos + 2]);
                        int h3 = hex_value(tk->src[tk->pos + 3]);
                        if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                            tokenizer_set_error(tk, "invalid unicode escape", tk->pos);
                            return tok;
                        }
                        unsigned int codepoint = (unsigned int)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                        if (codepoint == 0U) {
                            tokenizer_set_error(tk, "embedded NUL is not allowed", tk->pos);
                            return tok;
                        }
                        if (codepoint > 0x7FU) {
                            tokenizer_set_error(tk, "unicode codepoint > 0x7f is unsupported in metadata v1", tk->pos);
                            return tok;
                        }
                        out_ch = (unsigned char)codepoint;
                        tk->pos += 4;
                        break;
                    }
                    default: tokenizer_set_error(tk, "invalid escape sequence", tk->pos - 1); return tok;
                }
                if (have_char) {
                    if (w + 1 >= sizeof(tk->str_buf)) {
                        tokenizer_set_error(tk, "string exceeds parser buffer", start);
                        return tok;
                    }
                    tk->str_buf[w++] = (char)out_ch;
                }
                continue;
            }
            if (w + 1 >= sizeof(tk->str_buf)) {
                tokenizer_set_error(tk, "string exceeds parser buffer", start);
                return tok;
            }
            tk->str_buf[w++] = (char)ch;
        }
        tokenizer_set_error(tk, "unterminated string", start);
        return tok;
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        int is_float = 0;
        size_t p = tk->pos;
        if (tk->src[p] == '-') {
            p++;
            if (p >= tk->src_len || !(tk->src[p] >= '0' && tk->src[p] <= '9')) {
                tokenizer_set_error(tk, "invalid number", start);
                return tok;
            }
        }
        if (p < tk->src_len && tk->src[p] == '0') {
            p++;
        } else {
            if (p >= tk->src_len || !(tk->src[p] >= '0' && tk->src[p] <= '9')) {
                tokenizer_set_error(tk, "invalid number", start);
                return tok;
            }
            while (p < tk->src_len && tk->src[p] >= '0' && tk->src[p] <= '9') {
                p++;
            }
        }

        if (p < tk->src_len && tk->src[p] == '.') {
            is_float = 1;
            p++;
            if (p >= tk->src_len || !(tk->src[p] >= '0' && tk->src[p] <= '9')) {
                tokenizer_set_error(tk, "invalid fractional number", start);
                return tok;
            }
            while (p < tk->src_len && tk->src[p] >= '0' && tk->src[p] <= '9') {
                p++;
            }
        }
        if (p < tk->src_len && (tk->src[p] == 'e' || tk->src[p] == 'E')) {
            is_float = 1;
            p++;
            if (p < tk->src_len && (tk->src[p] == '+' || tk->src[p] == '-')) {
                p++;
            }
            if (p >= tk->src_len || !(tk->src[p] >= '0' && tk->src[p] <= '9')) {
                tokenizer_set_error(tk, "invalid exponent", start);
                return tok;
            }
            while (p < tk->src_len && tk->src[p] >= '0' && tk->src[p] <= '9') {
                p++;
            }
        }
        tok.type = is_float ? JTOK_NUMBER_FLOAT : JTOK_NUMBER_INT;
        tok.offset = start;
        tok.num = tk->src + start;
        tok.num_len = p - start;
        tk->pos = p;
        return tok;
    }

    if (start + 4 <= tk->src_len && strncmp(tk->src + start, "true", 4) == 0) {
        tk->pos = start + 4;
        return make_simple_token(JTOK_TRUE, start);
    }
    if (start + 5 <= tk->src_len && strncmp(tk->src + start, "false", 5) == 0) {
        tk->pos = start + 5;
        return make_simple_token(JTOK_FALSE, start);
    }
    if (start + 4 <= tk->src_len && strncmp(tk->src + start, "null", 4) == 0) {
        tk->pos = start + 4;
        return make_simple_token(JTOK_NULL, start);
    }

    tokenizer_set_error(tk, "unexpected JSON token", start);
    return tok;
}

static int
copy_token_to_buffer(const json_token* tok, char* out, size_t out_size, char* err_buf, size_t err_buf_size,
                     const char* field_name, int reject_controls) {
    if (!tok || tok->type != JTOK_STRING || !out || out_size == 0) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (tok->str_len + 1 > out_size) {
        set_error(err_buf, err_buf_size, "field '%s' too long", field_name ? field_name : "(string)");
        return DSD_IQ_ERR_INVALID_META;
    }
    if (reject_controls) {
        for (size_t i = 0; i < tok->str_len; i++) {
            unsigned char c = (unsigned char)tok->str[i];
            if (c < 0x20) {
                set_error(err_buf, err_buf_size, "field '%s' contains control byte 0x%02x",
                          field_name ? field_name : "(string)", c);
                return DSD_IQ_ERR_INVALID_META;
            }
        }
    }
    memcpy(out, tok->str, tok->str_len);
    out[tok->str_len] = '\0';
    return DSD_IQ_OK;
}

static int
token_to_u64(const json_token* tok, uint64_t* out, char* err_buf, size_t err_buf_size, const char* field_name) {
    if (!tok || tok->type != JTOK_NUMBER_INT || !out) {
        return DSD_IQ_ERR_INVALID_META;
    }
    char buf[64];
    if (tok->num_len == 0 || tok->num_len >= sizeof(buf)) {
        set_error(err_buf, err_buf_size, "integer for '%s' is invalid", field_name);
        return DSD_IQ_ERR_INVALID_META;
    }
    memcpy(buf, tok->num, tok->num_len);
    buf[tok->num_len] = '\0';
    errno = 0;
    char* end = NULL;
    unsigned long long v = strtoull(buf, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        set_error(err_buf, err_buf_size, "integer for '%s' is invalid", field_name);
        return DSD_IQ_ERR_INVALID_META;
    }
    *out = (uint64_t)v;
    return DSD_IQ_OK;
}

static int
token_to_u32(const json_token* tok, uint32_t* out, char* err_buf, size_t err_buf_size, const char* field_name) {
    uint64_t v = 0;
    int rc = token_to_u64(tok, &v, err_buf, err_buf_size, field_name);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    if (v > UINT32_MAX) {
        set_error(err_buf, err_buf_size, "integer for '%s' overflows uint32", field_name);
        return DSD_IQ_ERR_INVALID_META;
    }
    *out = (uint32_t)v;
    return DSD_IQ_OK;
}

static int
token_to_i32(const json_token* tok, int* out, char* err_buf, size_t err_buf_size, const char* field_name) {
    if (!tok || tok->type != JTOK_NUMBER_INT || !out) {
        return DSD_IQ_ERR_INVALID_META;
    }
    char buf[64];
    if (tok->num_len == 0 || tok->num_len >= sizeof(buf)) {
        set_error(err_buf, err_buf_size, "integer for '%s' is invalid", field_name);
        return DSD_IQ_ERR_INVALID_META;
    }
    memcpy(buf, tok->num, tok->num_len);
    buf[tok->num_len] = '\0';
    errno = 0;
    char* end = NULL;
    long v = strtol(buf, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        set_error(err_buf, err_buf_size, "integer for '%s' is invalid", field_name);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (v < INT_MIN || v > INT_MAX) {
        set_error(err_buf, err_buf_size, "integer for '%s' overflows int", field_name);
        return DSD_IQ_ERR_INVALID_META;
    }
    *out = (int)v;
    return DSD_IQ_OK;
}

static int
token_to_bool(const json_token* tok, int* out, char* err_buf, size_t err_buf_size, const char* field_name) {
    if (!tok || !out) {
        return DSD_IQ_ERR_INVALID_META;
    }
    if (tok->type == JTOK_TRUE) {
        *out = 1;
        return DSD_IQ_OK;
    }
    if (tok->type == JTOK_FALSE) {
        *out = 0;
        return DSD_IQ_OK;
    }
    set_error(err_buf, err_buf_size, "field '%s' expects boolean", field_name);
    return DSD_IQ_ERR_INVALID_META;
}

typedef struct {
    unsigned format                      : 1;
    unsigned version                     : 1;
    unsigned sample_format               : 1;
    unsigned iq_order                    : 1;
    unsigned endianness                  : 1;
    unsigned capture_stage               : 1;
    unsigned sample_rate_hz              : 1;
    unsigned center_frequency_hz         : 1;
    unsigned capture_center_frequency_hz : 1;
    unsigned ppm                         : 1;
    unsigned tuner_gain_tenth_db         : 1;
    unsigned rtl_dsp_bw_khz              : 1;
    unsigned base_decimation             : 1;
    unsigned post_downsample             : 1;
    unsigned demod_rate_hz               : 1;
    unsigned offset_tuning_enabled       : 1;
    unsigned fs4_shift_enabled           : 1;
    unsigned combine_rotate_enabled      : 1;
    unsigned muted_bytes_excluded        : 1;
    unsigned contains_retunes            : 1;
    unsigned capture_retune_count        : 1;
    unsigned source_backend              : 1;
    unsigned source_args                 : 1;
    unsigned capture_started_utc         : 1;
    unsigned data_file                   : 1;
    unsigned data_bytes                  : 1;
    unsigned capture_drops               : 1;
    unsigned capture_drop_blocks         : 1;
    unsigned input_ring_drops            : 1;
    unsigned notes                       : 1;
} replay_seen_fields;

static int
validate_replay_semantics(const dsd_iq_replay_config* cfg, int reject_retunes, char* err_buf, size_t err_buf_size) {
    if (!cfg) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (cfg->format != DSD_IQ_FORMAT_CU8 && cfg->format != DSD_IQ_FORMAT_CF32 && cfg->format != DSD_IQ_FORMAT_CS16) {
        set_error(err_buf, err_buf_size, "unsupported sample_format");
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    if (cfg->sample_rate_hz == 0) {
        set_error(err_buf, err_buf_size, "sample_rate_hz must be > 0");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (cfg->base_decimation == 0 || (cfg->base_decimation & (cfg->base_decimation - 1U)) != 0) {
        set_error(err_buf, err_buf_size, "base_decimation must be a power of two");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (cfg->post_downsample == 0) {
        set_error(err_buf, err_buf_size, "post_downsample must be > 0");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (cfg->demod_rate_hz == 0) {
        set_error(err_buf, err_buf_size, "demod_rate_hz must be > 0");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    uint64_t derived = (uint64_t)cfg->sample_rate_hz / (uint64_t)cfg->base_decimation / (uint64_t)cfg->post_downsample;
    if (derived != cfg->demod_rate_hz) {
        set_error(err_buf, err_buf_size,
                  "demod_rate_hz (%u) inconsistent with sample_rate/base_decimation/post_downsample",
                  cfg->demod_rate_hz);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (strcmp(cfg->capture_stage, "post_mute_pre_widen") != 0
        && strcmp(cfg->capture_stage, "post_driver_cf32_pre_ring") != 0) {
        set_error(err_buf, err_buf_size, "unsupported capture_stage '%s'", cfg->capture_stage);
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    if (reject_retunes && cfg->contains_retunes) {
        set_error(err_buf, err_buf_size, "capture contains retunes; replay requires single-segment capture");
        return DSD_IQ_ERR_RETUNE_REJECT;
    }
    return DSD_IQ_OK;
}

static int
require_field(unsigned seen, const char* field_name, char* err_buf, size_t err_buf_size) {
    if (seen) {
        return DSD_IQ_OK;
    }
    set_error(err_buf, err_buf_size, "missing metadata field '%s'", field_name);
    return DSD_IQ_ERR_INVALID_META;
}

static int
parse_metadata_json(const char* metadata_path, const char* json, size_t json_len, dsd_iq_replay_config* out_cfg,
                    int reject_retunes, char* err_buf, size_t err_buf_size) {
    if (!metadata_path || !json || !out_cfg) {
        return DSD_IQ_ERR_INVALID_ARG;
    }

    json_tokenizer tk;
    memset(&tk, 0, sizeof(tk));
    tk.src = json;
    tk.src_len = json_len;

    dsd_iq_replay_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    replay_seen_fields seen;
    memset(&seen, 0, sizeof(seen));
    int version = 0;
    char format_buf[64] = {0};
    char sample_format_buf[32] = {0};
    char iq_order_buf[16] = {0};
    char endianness_buf[16] = {0};
    char data_file_buf[2048] = {0};

    json_token tok = tokenizer_next(&tk);
    if (tok.type != JTOK_LBRACE) {
        if (tk.err_msg) {
            set_error(err_buf, err_buf_size, "%s at byte %zu", tk.err_msg, tk.err_pos);
        } else {
            set_error(err_buf, err_buf_size, "metadata must start with '{'");
        }
        return DSD_IQ_ERR_INVALID_META;
    }

    for (;;) {
        json_token key_tok = tokenizer_next(&tk);
        if (tk.err_msg) {
            set_error(err_buf, err_buf_size, "%s at byte %zu", tk.err_msg, tk.err_pos);
            return DSD_IQ_ERR_INVALID_META;
        }
        if (key_tok.type == JTOK_RBRACE) {
            break;
        }
        if (key_tok.type != JTOK_STRING) {
            set_error(err_buf, err_buf_size, "expected string key at byte %zu", key_tok.offset);
            return DSD_IQ_ERR_INVALID_META;
        }

        char key_buf[128];
        if (key_tok.str_len + 1 > sizeof(key_buf)) {
            set_error(err_buf, err_buf_size, "metadata key too long at byte %zu", key_tok.offset);
            return DSD_IQ_ERR_INVALID_META;
        }
        memcpy(key_buf, key_tok.str, key_tok.str_len);
        key_buf[key_tok.str_len] = '\0';

        json_token colon_tok = tokenizer_next(&tk);
        if (colon_tok.type != JTOK_COLON) {
            set_error(err_buf, err_buf_size, "expected ':' after key at byte %zu", key_tok.offset);
            return DSD_IQ_ERR_INVALID_META;
        }

        json_token val_tok = tokenizer_next(&tk);
        if (tk.err_msg) {
            set_error(err_buf, err_buf_size, "%s at byte %zu", tk.err_msg, tk.err_pos);
            return DSD_IQ_ERR_INVALID_META;
        }
        if (val_tok.type == JTOK_LBRACE || val_tok.type == JTOK_LBRACKET) {
            set_error(err_buf, err_buf_size, "nested structures are unsupported in metadata v1 (at byte %zu)",
                      val_tok.offset);
            return DSD_IQ_ERR_INVALID_META;
        }

        const char* key = key_buf;
        int rc = DSD_IQ_OK;
        if (strcmp(key, "format") == 0) {
            if (val_tok.type != JTOK_STRING) {
                set_error(err_buf, err_buf_size, "field 'format' expects string");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = copy_token_to_buffer(&val_tok, format_buf, sizeof(format_buf), err_buf, err_buf_size, "format", 1);
            seen.format = 1;
        } else if (strcmp(key, "version") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'version' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_i32(&val_tok, &version, err_buf, err_buf_size, "version");
            seen.version = 1;
        } else if (strcmp(key, "sample_format") == 0) {
            if (val_tok.type != JTOK_STRING) {
                set_error(err_buf, err_buf_size, "field 'sample_format' expects string");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = copy_token_to_buffer(&val_tok, sample_format_buf, sizeof(sample_format_buf), err_buf, err_buf_size,
                                      "sample_format", 1);
            seen.sample_format = 1;
        } else if (strcmp(key, "iq_order") == 0) {
            if (val_tok.type != JTOK_STRING) {
                set_error(err_buf, err_buf_size, "field 'iq_order' expects string");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = copy_token_to_buffer(&val_tok, iq_order_buf, sizeof(iq_order_buf), err_buf, err_buf_size, "iq_order",
                                      1);
            seen.iq_order = 1;
        } else if (strcmp(key, "endianness") == 0) {
            if (val_tok.type != JTOK_STRING) {
                set_error(err_buf, err_buf_size, "field 'endianness' expects string");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = copy_token_to_buffer(&val_tok, endianness_buf, sizeof(endianness_buf), err_buf, err_buf_size,
                                      "endianness", 1);
            seen.endianness = 1;
        } else if (strcmp(key, "capture_stage") == 0) {
            if (val_tok.type != JTOK_STRING) {
                set_error(err_buf, err_buf_size, "field 'capture_stage' expects string");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = copy_token_to_buffer(&val_tok, cfg.capture_stage, sizeof(cfg.capture_stage), err_buf, err_buf_size,
                                      "capture_stage", 1);
            seen.capture_stage = 1;
        } else if (strcmp(key, "sample_rate_hz") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'sample_rate_hz' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u32(&val_tok, &cfg.sample_rate_hz, err_buf, err_buf_size, "sample_rate_hz");
            seen.sample_rate_hz = 1;
        } else if (strcmp(key, "center_frequency_hz") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'center_frequency_hz' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u64(&val_tok, &cfg.center_frequency_hz, err_buf, err_buf_size, "center_frequency_hz");
            seen.center_frequency_hz = 1;
        } else if (strcmp(key, "capture_center_frequency_hz") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'capture_center_frequency_hz' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u64(&val_tok, &cfg.capture_center_frequency_hz, err_buf, err_buf_size,
                              "capture_center_frequency_hz");
            seen.capture_center_frequency_hz = 1;
        } else if (strcmp(key, "ppm") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'ppm' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_i32(&val_tok, &cfg.ppm, err_buf, err_buf_size, "ppm");
            seen.ppm = 1;
        } else if (strcmp(key, "tuner_gain_tenth_db") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'tuner_gain_tenth_db' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_i32(&val_tok, &cfg.tuner_gain_tenth_db, err_buf, err_buf_size, "tuner_gain_tenth_db");
            seen.tuner_gain_tenth_db = 1;
        } else if (strcmp(key, "rtl_dsp_bw_khz") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'rtl_dsp_bw_khz' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_i32(&val_tok, &cfg.rtl_dsp_bw_khz, err_buf, err_buf_size, "rtl_dsp_bw_khz");
            seen.rtl_dsp_bw_khz = 1;
        } else if (strcmp(key, "base_decimation") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'base_decimation' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u32(&val_tok, &cfg.base_decimation, err_buf, err_buf_size, "base_decimation");
            seen.base_decimation = 1;
        } else if (strcmp(key, "post_downsample") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'post_downsample' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u32(&val_tok, &cfg.post_downsample, err_buf, err_buf_size, "post_downsample");
            seen.post_downsample = 1;
        } else if (strcmp(key, "demod_rate_hz") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'demod_rate_hz' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u32(&val_tok, &cfg.demod_rate_hz, err_buf, err_buf_size, "demod_rate_hz");
            seen.demod_rate_hz = 1;
        } else if (strcmp(key, "offset_tuning_enabled") == 0) {
            rc = token_to_bool(&val_tok, &cfg.offset_tuning_enabled, err_buf, err_buf_size, "offset_tuning_enabled");
            seen.offset_tuning_enabled = 1;
        } else if (strcmp(key, "fs4_shift_enabled") == 0) {
            rc = token_to_bool(&val_tok, &cfg.fs4_shift_enabled, err_buf, err_buf_size, "fs4_shift_enabled");
            seen.fs4_shift_enabled = 1;
        } else if (strcmp(key, "combine_rotate_enabled") == 0) {
            rc = token_to_bool(&val_tok, &cfg.combine_rotate_enabled, err_buf, err_buf_size, "combine_rotate_enabled");
            seen.combine_rotate_enabled = 1;
        } else if (strcmp(key, "muted_bytes_excluded") == 0) {
            rc = token_to_bool(&val_tok, &cfg.muted_bytes_excluded, err_buf, err_buf_size, "muted_bytes_excluded");
            seen.muted_bytes_excluded = 1;
        } else if (strcmp(key, "contains_retunes") == 0) {
            rc = token_to_bool(&val_tok, &cfg.contains_retunes, err_buf, err_buf_size, "contains_retunes");
            seen.contains_retunes = 1;
        } else if (strcmp(key, "capture_retune_count") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'capture_retune_count' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u32(&val_tok, &cfg.capture_retune_count, err_buf, err_buf_size, "capture_retune_count");
            seen.capture_retune_count = 1;
        } else if (strcmp(key, "source_backend") == 0) {
            if (val_tok.type != JTOK_STRING) {
                set_error(err_buf, err_buf_size, "field 'source_backend' expects string");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = copy_token_to_buffer(&val_tok, cfg.source_backend, sizeof(cfg.source_backend), err_buf, err_buf_size,
                                      "source_backend", 1);
            seen.source_backend = 1;
        } else if (strcmp(key, "source_args") == 0) {
            if (val_tok.type != JTOK_STRING) {
                set_error(err_buf, err_buf_size, "field 'source_args' expects string");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = copy_token_to_buffer(&val_tok, cfg.source_args, sizeof(cfg.source_args), err_buf, err_buf_size,
                                      "source_args", 1);
            seen.source_args = 1;
        } else if (strcmp(key, "capture_started_utc") == 0) {
            if (val_tok.type != JTOK_STRING) {
                set_error(err_buf, err_buf_size, "field 'capture_started_utc' expects string");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = copy_token_to_buffer(&val_tok, cfg.capture_started_utc, sizeof(cfg.capture_started_utc), err_buf,
                                      err_buf_size, "capture_started_utc", 1);
            seen.capture_started_utc = 1;
        } else if (strcmp(key, "data_file") == 0) {
            if (val_tok.type != JTOK_STRING) {
                set_error(err_buf, err_buf_size, "field 'data_file' expects string");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = copy_token_to_buffer(&val_tok, data_file_buf, sizeof(data_file_buf), err_buf, err_buf_size,
                                      "data_file", 1);
            seen.data_file = 1;
        } else if (strcmp(key, "data_bytes") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'data_bytes' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u64(&val_tok, &cfg.data_bytes, err_buf, err_buf_size, "data_bytes");
            seen.data_bytes = 1;
        } else if (strcmp(key, "capture_drops") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'capture_drops' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u64(&val_tok, &cfg.capture_drops, err_buf, err_buf_size, "capture_drops");
            seen.capture_drops = 1;
        } else if (strcmp(key, "capture_drop_blocks") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'capture_drop_blocks' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u64(&val_tok, &cfg.capture_drop_blocks, err_buf, err_buf_size, "capture_drop_blocks");
            seen.capture_drop_blocks = 1;
        } else if (strcmp(key, "input_ring_drops") == 0) {
            if (val_tok.type != JTOK_NUMBER_INT) {
                set_error(err_buf, err_buf_size, "field 'input_ring_drops' expects integer");
                return DSD_IQ_ERR_INVALID_META;
            }
            rc = token_to_u64(&val_tok, &cfg.input_ring_drops, err_buf, err_buf_size, "input_ring_drops");
            seen.input_ring_drops = 1;
        } else if (strcmp(key, "notes") == 0) {
            if (val_tok.type == JTOK_NULL) {
                cfg.notes[0] = '\0';
                rc = DSD_IQ_OK;
            } else if (val_tok.type == JTOK_STRING) {
                rc = copy_token_to_buffer(&val_tok, cfg.notes, sizeof(cfg.notes), err_buf, err_buf_size, "notes", 0);
            } else {
                set_error(err_buf, err_buf_size, "field 'notes' expects string or null");
                return DSD_IQ_ERR_INVALID_META;
            }
            seen.notes = 1;
        } else {
            /* Unknown fields are ignored for forward compatibility. */
        }

        if (rc != DSD_IQ_OK) {
            return rc;
        }

        json_token delim_tok = tokenizer_next(&tk);
        if (delim_tok.type == JTOK_COMMA) {
            continue;
        }
        if (delim_tok.type == JTOK_RBRACE) {
            break;
        }
        if (tk.err_msg) {
            set_error(err_buf, err_buf_size, "%s at byte %zu", tk.err_msg, tk.err_pos);
        } else {
            set_error(err_buf, err_buf_size, "expected ',' or '}' after field '%s' at byte %zu", key, delim_tok.offset);
        }
        return DSD_IQ_ERR_INVALID_META;
    }

    tok = tokenizer_next(&tk);
    if (tok.type != JTOK_EOF) {
        set_error(err_buf, err_buf_size, "trailing JSON content at byte %zu", tok.offset);
        return DSD_IQ_ERR_INVALID_META;
    }

    if (require_field(seen.format, "format", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.version, "version", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.sample_format, "sample_format", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.iq_order, "iq_order", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.endianness, "endianness", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.capture_stage, "capture_stage", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.sample_rate_hz, "sample_rate_hz", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.center_frequency_hz, "center_frequency_hz", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.capture_center_frequency_hz, "capture_center_frequency_hz", err_buf, err_buf_size)
               != DSD_IQ_OK
        || require_field(seen.ppm, "ppm", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.tuner_gain_tenth_db, "tuner_gain_tenth_db", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.rtl_dsp_bw_khz, "rtl_dsp_bw_khz", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.base_decimation, "base_decimation", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.post_downsample, "post_downsample", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.demod_rate_hz, "demod_rate_hz", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.offset_tuning_enabled, "offset_tuning_enabled", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.fs4_shift_enabled, "fs4_shift_enabled", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.combine_rotate_enabled, "combine_rotate_enabled", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.muted_bytes_excluded, "muted_bytes_excluded", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.contains_retunes, "contains_retunes", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.capture_retune_count, "capture_retune_count", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.source_backend, "source_backend", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.source_args, "source_args", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.capture_started_utc, "capture_started_utc", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.data_file, "data_file", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.data_bytes, "data_bytes", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.capture_drops, "capture_drops", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.capture_drop_blocks, "capture_drop_blocks", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.input_ring_drops, "input_ring_drops", err_buf, err_buf_size) != DSD_IQ_OK
        || require_field(seen.notes, "notes", err_buf, err_buf_size) != DSD_IQ_OK) {
        return DSD_IQ_ERR_INVALID_META;
    }

    if (strcmp(format_buf, "dsd-neo-iq") != 0) {
        set_error(err_buf, err_buf_size, "unsupported format '%s'", format_buf);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (version != 1) {
        set_error(err_buf, err_buf_size, "unsupported metadata version %d", version);
        return DSD_IQ_ERR_UNSUPPORTED_VER;
    }
    if (strcmp(iq_order_buf, "IQ") != 0) {
        set_error(err_buf, err_buf_size, "unsupported iq_order '%s'", iq_order_buf);
        return DSD_IQ_ERR_INVALID_META;
    }

    if (strcmp(sample_format_buf, "cu8") == 0) {
        cfg.format = DSD_IQ_FORMAT_CU8;
        if (strcmp(endianness_buf, "none") != 0) {
            set_error(err_buf, err_buf_size, "cu8 requires endianness 'none'");
            return DSD_IQ_ERR_INVALID_META;
        }
    } else if (strcmp(sample_format_buf, "cf32") == 0) {
        cfg.format = DSD_IQ_FORMAT_CF32;
        if (strcmp(endianness_buf, "little") != 0) {
            set_error(err_buf, err_buf_size, "cf32 requires endianness 'little'");
            return DSD_IQ_ERR_INVALID_META;
        }
    } else if (strcmp(sample_format_buf, "cs16") == 0) {
        cfg.format = DSD_IQ_FORMAT_CS16;
        if (strcmp(endianness_buf, "little") != 0) {
            set_error(err_buf, err_buf_size, "cs16 requires endianness 'little'");
            return DSD_IQ_ERR_INVALID_META;
        }
    } else {
        set_error(err_buf, err_buf_size, "unsupported sample_format '%s'", sample_format_buf);
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }

    int drc =
        resolve_data_path(metadata_path, data_file_buf, cfg.data_path, sizeof(cfg.data_path), err_buf, err_buf_size);
    if (drc != DSD_IQ_OK) {
        return drc;
    }
    int cprc = copy_string_checked(metadata_path, cfg.metadata_path, sizeof(cfg.metadata_path), err_buf, err_buf_size,
                                   "metadata_path");
    if (cprc != DSD_IQ_OK) {
        return cprc;
    }

    int src = validate_replay_semantics(&cfg, reject_retunes, err_buf, err_buf_size);
    if (src != DSD_IQ_OK) {
        return src;
    }

    *out_cfg = cfg;
    return DSD_IQ_OK;
}

int
dsd_iq_replay_compute_effective_bytes(uint64_t data_bytes, uint64_t actual_file_size, dsd_iq_sample_format format,
                                      uint64_t* out_effective, int* out_size_mismatch) {
    if (!out_effective) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    size_t align = dsd_iq_sample_format_alignment_bytes(format);
    if (align == 0) {
        return DSD_IQ_ERR_INVALID_ARG;
    }

    uint64_t raw = actual_file_size;
    int mismatch = 0;
    if (data_bytes > 0) {
        raw = (data_bytes < actual_file_size) ? data_bytes : actual_file_size;
        if (data_bytes != actual_file_size) {
            mismatch = 1;
        }
    }
    *out_effective = raw - (raw % (uint64_t)align);
    if (out_size_mismatch) {
        *out_size_mismatch = mismatch;
    }
    return DSD_IQ_OK;
}

int
dsd_iq_replay_validate_effective_bytes_for_replay(uint64_t effective_bytes, int loop) {
    (void)loop;
    if (effective_bytes == 0) {
        return DSD_IQ_ERR_ALIGNMENT;
    }
    return DSD_IQ_OK;
}

double
dsd_iq_replay_estimate_duration_seconds(uint64_t data_bytes, dsd_iq_sample_format format, uint32_t sample_rate_hz) {
    if (sample_rate_hz == 0) {
        return 0.0;
    }
    size_t align = dsd_iq_sample_format_alignment_bytes(format);
    if (align == 0) {
        return 0.0;
    }
    uint64_t complex_samples = data_bytes / (uint64_t)align;
    return (double)complex_samples / (double)sample_rate_hz;
}

static int
replay_read_metadata_internal(const char* path, dsd_iq_replay_config* out_cfg, int reject_retunes, char* err_buf,
                              size_t err_buf_size) {
    if (!path || !out_cfg) {
        return DSD_IQ_ERR_INVALID_ARG;
    }

    char metadata_path[2048];
    int mrc = resolve_metadata_path(path, metadata_path, sizeof(metadata_path), err_buf, err_buf_size);
    if (mrc != DSD_IQ_OK) {
        return mrc;
    }

    char* json = NULL;
    size_t json_len = 0;
    int rrc = read_file_all(metadata_path, &json, &json_len, err_buf, err_buf_size);
    if (rrc != DSD_IQ_OK) {
        return rrc;
    }

    int prc = parse_metadata_json(metadata_path, json, json_len, out_cfg, reject_retunes, err_buf, err_buf_size);
    free(json);
    return prc;
}

int
dsd_iq_replay_read_metadata(const char* path, dsd_iq_replay_config* out_cfg, char* err_buf, size_t err_buf_size) {
    return replay_read_metadata_internal(path, out_cfg, 0, err_buf, err_buf_size);
}

int
dsd_iq_replay_open(const char* path, dsd_iq_replay_config* out_cfg, dsd_iq_replay_source** out, char* err_buf,
                   size_t err_buf_size) {
    if (!path || !out_cfg) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (out) {
        *out = NULL;
    }

    dsd_iq_replay_config cfg;
    int rc = replay_read_metadata_internal(path, &cfg, 1, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }

    uint64_t actual_size = 0;
    if (file_size_u64(cfg.data_path, &actual_size) != DSD_IQ_OK) {
        set_error(err_buf, err_buf_size, "failed to stat data file '%s': %s", cfg.data_path, strerror(errno));
        return DSD_IQ_ERR_IO;
    }

    uint64_t effective = 0;
    int mismatch = 0;
    rc = dsd_iq_replay_compute_effective_bytes(cfg.data_bytes, actual_size, cfg.format, &effective, &mismatch);
    if (rc != DSD_IQ_OK) {
        set_error(err_buf, err_buf_size, "failed to compute effective replay bytes");
        return rc;
    }
    rc = dsd_iq_replay_validate_effective_bytes_for_replay(effective, cfg.loop);
    if (rc != DSD_IQ_OK) {
        set_error(err_buf, err_buf_size, "no aligned I/Q samples available for replay");
        return rc;
    }

    (void)mismatch;
    *out_cfg = cfg;

    if (!out) {
        return DSD_IQ_OK;
    }

    struct dsd_iq_replay_source* src = (struct dsd_iq_replay_source*)calloc(1, sizeof(*src));
    if (!src) {
        set_error(err_buf, err_buf_size, "replay source allocation failed");
        return DSD_IQ_ERR_ALLOC;
    }
    src->fp = fopen(cfg.data_path, "rb");
    if (!src->fp) {
        set_error(err_buf, err_buf_size, "failed to open replay data '%s': %s", cfg.data_path, strerror(errno));
        free(src);
        return DSD_IQ_ERR_IO;
    }
    src->remaining_bytes = effective;
    src->total_bytes = effective;
    *out = src;
    return DSD_IQ_OK;
}

int
dsd_iq_replay_read(dsd_iq_replay_source* src, void* out, size_t max_bytes, size_t* out_bytes) {
    if (!src || !out || !out_bytes) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    *out_bytes = 0;
    if (!src->fp || max_bytes == 0 || src->remaining_bytes == 0) {
        return DSD_IQ_OK;
    }

    if ((uint64_t)max_bytes > src->remaining_bytes) {
        max_bytes = (size_t)src->remaining_bytes;
    }
    size_t n = fread(out, 1, max_bytes, src->fp);
    if (n == 0 && ferror(src->fp)) {
        return DSD_IQ_ERR_IO;
    }
    src->remaining_bytes -= (uint64_t)n;
    *out_bytes = n;
    return DSD_IQ_OK;
}

int
dsd_iq_replay_rewind(dsd_iq_replay_source* src) {
    if (!src || !src->fp) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (fseek(src->fp, 0, SEEK_SET) != 0) {
        return DSD_IQ_ERR_IO;
    }
    src->remaining_bytes = src->total_bytes;
    return DSD_IQ_OK;
}

void
dsd_iq_replay_close(dsd_iq_replay_source* src) {
    if (!src) {
        return;
    }
    if (src->fp) {
        fclose(src->fp);
    }
    free(src);
}

int
dsd_iq_info_print(const dsd_iq_replay_config* cfg, const char* display_path, uint64_t actual_file_size, FILE* out,
                  FILE* err) {
    if (!cfg || !out) {
        return DSD_IQ_ERR_INVALID_ARG;
    }

    uint64_t effective = 0;
    int mismatch = 0;
    int rc =
        dsd_iq_replay_compute_effective_bytes(cfg->data_bytes, actual_file_size, cfg->format, &effective, &mismatch);
    if (rc != DSD_IQ_OK) {
        return rc;
    }

    double dur = dsd_iq_replay_estimate_duration_seconds(effective, cfg->format, cfg->sample_rate_hz);
    uint64_t complex_samples = 0;
    size_t align = dsd_iq_sample_format_alignment_bytes(cfg->format);
    if (align > 0) {
        complex_samples = effective / (uint64_t)align;
    }

    fprintf(out, "IQ Capture Info: %s\n", display_path ? display_path : cfg->metadata_path);
    fprintf(out, "  Format:              dsd-neo-iq v1\n");
    fprintf(out, "  Sample format:       %s\n", dsd_iq_sample_format_name(cfg->format));
    fprintf(out, "  Sample rate:         %u Hz\n", cfg->sample_rate_hz);
    fprintf(out, "  Center frequency:    %.6f MHz\n", (double)cfg->center_frequency_hz / 1000000.0);
    fprintf(out, "  Capture center:      %.6f MHz\n", (double)cfg->capture_center_frequency_hz / 1000000.0);
    fprintf(out, "  Demod rate:          %u Hz (base_decimation=%u, post_downsample=%u)\n", cfg->demod_rate_hz,
            cfg->base_decimation, cfg->post_downsample);
    fprintf(out, "  Source backend:      %s (%s)\n", cfg->source_backend,
            cfg->source_args[0] ? cfg->source_args : "none");
    fprintf(out, "  Capture stage:       %s\n", cfg->capture_stage);
    fprintf(out, "  FS/4 shift:          %s\n", cfg->fs4_shift_enabled ? "enabled" : "disabled");
    fprintf(out, "  Combine-rotate:      %s\n", cfg->combine_rotate_enabled ? "enabled" : "disabled");
    fprintf(out, "  Offset tuning:       %s\n", cfg->offset_tuning_enabled ? "enabled" : "disabled");
    fprintf(out, "  Tuner gain:          %.1f dB\n", (double)cfg->tuner_gain_tenth_db / 10.0);
    fprintf(out, "  PPM correction:      %d\n", cfg->ppm);
    fprintf(out, "  DSP bandwidth:       %d kHz\n", cfg->rtl_dsp_bw_khz);
    fprintf(out, "  Data file:           %s\n", cfg->data_path);
    fprintf(out, "  Data bytes:          %" PRIu64 " (metadata), %" PRIu64 " (actual)\n", cfg->data_bytes,
            actual_file_size);
    fprintf(out, "  Duration:            ~%.2f s (%" PRIu64 " complex samples)\n", dur, complex_samples);
    fprintf(out, "  Capture drops:       %" PRIu64 " (%" PRIu64 " blocks)\n", cfg->capture_drops,
            cfg->capture_drop_blocks);
    fprintf(out, "  Input ring drops:    %" PRIu64 "\n", cfg->input_ring_drops);
    fprintf(out, "  Contains retunes:    %s (%u retune events)\n", cfg->contains_retunes ? "yes" : "no",
            cfg->capture_retune_count);
    fprintf(out, "  Replay compatible:   %s\n", (!cfg->contains_retunes && effective > 0) ? "yes" : "no");

    if (err) {
        if (mismatch) {
            fprintf(err, "WARNING: metadata data_bytes (%" PRIu64 ") != actual file size (%" PRIu64 ")\n",
                    cfg->data_bytes, actual_file_size);
        }
        if (cfg->data_bytes == 0) {
            fprintf(err, "WARNING: metadata was never finalized (interrupted capture)\n");
        }
        if (effective
            != ((cfg->data_bytes > 0 ? (cfg->data_bytes < actual_file_size ? cfg->data_bytes : actual_file_size)
                                     : actual_file_size))) {
            fprintf(err, "WARNING: data file size not aligned to sample boundary (%s requires %zu-byte alignment)\n",
                    dsd_iq_sample_format_name(cfg->format), align);
        }
        if (cfg->contains_retunes) {
            fprintf(err,
                    "WARNING: capture contains %u retune(s) — not replayable until segment metadata is implemented\n",
                    cfg->capture_retune_count);
        }
    }

    return DSD_IQ_OK;
}
