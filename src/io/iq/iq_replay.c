// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/iq_replay_checkpoint.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_wav.h>
#include <dsd-neo/platform/file_compat.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/platform/posix_compat.h"
#include "iq_replay_test_hooks.h"

/* A WAV read converts at most this many frames per pass, through 64 KiB of raw bytes and 64 KiB of cf32. */
#define REPLAY_WAV_SCRATCH_BYTES 65536U
#define REPLAY_WAV_CF32_FRAMES   8192U

struct dsd_iq_replay_source {
    FILE* fp;
    uint64_t total_bytes;     /* capture bytes the open measured (virtual cf32 bytes for a WAV) */
    uint64_t remaining_bytes; /* capture bytes not yet handed to a caller */
    size_t align;             /* capture bytes of one complex sample */
    /* WAV recordings: the reader converts frames to cf32 as it reads them. */
    int wav;
    dsd_iq_wav_encoding wav_encoding;
    size_t wav_block_align;
    uint64_t wav_data_offset;
    uint64_t wav_total_frames;
    uint64_t wav_frames_left;    /* whole frames not yet read from the file */
    uint8_t* wav_raw;            /* REPLAY_WAV_SCRATCH_BYTES */
    float* wav_cf32;             /* 2 x REPLAY_WAV_CF32_FRAMES */
    uint8_t wav_raw_pending[16]; /* the start of a frame a short read cut */
    size_t wav_raw_pending_len;
    uint8_t wav_out_pending[8]; /* the rest of a converted frame a read smaller than 8 bytes split */
    size_t wav_out_pending_off;
    size_t wav_out_pending_len;
};

static void set_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 3, 4);

static void
set_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) {
    if (!err_buf || err_buf_size == 0 || !fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(err_buf, err_buf_size, fmt, ap);
    va_end(ap);
    err_buf[err_buf_size - 1] = '\0';
}

/* The @p n decimal digits at @p text, or -1 when one of them is not a digit. */
static int
utc_field(const char* text, int n) {
    int value = 0;
    for (int i = 0; i < n; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

/* Days from 1970-01-01 to @p y-@p m-@p d in the proleptic Gregorian calendar (H. Hinnant's days_from_civil). */
static int64_t
utc_days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (int64_t)(m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int
utc_days_in_month(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[month - 1];
}

/* The separators of YYYY-MM-DDTHH:MM:SSZ in place, at its length. */
static int
utc_layout_ok(const char* text) {
    static const char kLayout[] = "####-##-##T##:##:##Z";
    if (strlen(text) != sizeof(kLayout) - 1U) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(kLayout) - 1U; i++) {
        if (kLayout[i] != '#' && text[i] != kLayout[i]) {
            return 0;
        }
    }
    return 1;
}

int
dsd_iq_replay_parse_utc_seconds(const char* text, int64_t* out_s) {
    if (!text || !out_s) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (!utc_layout_ok(text)) {
        return DSD_IQ_ERR_INVALID_META;
    }
    const int year = utc_field(text, 4);
    const int month = utc_field(text + 5, 2);
    const int day = utc_field(text + 8, 2);
    const int hour = utc_field(text + 11, 2);
    const int minute = utc_field(text + 14, 2);
    const int second = utc_field(text + 17, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1 || day > utc_days_in_month(year, month)) {
        return DSD_IQ_ERR_INVALID_META;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return DSD_IQ_ERR_INVALID_META;
    }
    *out_s = utc_days_from_civil(year, month, day) * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
    return DSD_IQ_OK;
}

void
dsd_iq_replay_config_clear(dsd_iq_replay_config* cfg) {
    if (!cfg) {
        return;
    }
    free(cfg->events);
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
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
    DSD_MEMCPY(dst, src, n + 1);
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
    /* Case-insensitive to match Windows, where "capture.iq.JSON" is the same file
       as "capture.iq.json" and must be recognised as the sidecar. */
    return dsd_strcasecmp(s + (s_len - suf_len), suffix) == 0;
}

static int
file_size_u64(const char* path, uint64_t* out_size) {
    if (!path || !out_size) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    dsd_stat_t st;
    if (dsd_stat_path(path, &st) != 0) {
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

    FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
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
    if ((uintmax_t)end > ((uintmax_t)SIZE_MAX - 1U)) {
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
    if (n + 9U > out_metadata_path_size) {
        set_error(err_buf, err_buf_size, "metadata path too long");
        return DSD_IQ_ERR_INVALID_ARG;
    }

    /* Try the sidecar for the path exactly as given first, so captures written before
       the ".iq" default still resolve and a data path like "mycap.iq" is found. Only
       then try the name a bare capture now produces. */
    dsd_stat_t st;
    DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s.json", path);
    if (dsd_stat_path(out_metadata_path, &st) == 0) {
        return DSD_IQ_OK;
    }
    DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s.iq.json", path);
    if (dsd_stat_path(out_metadata_path, &st) == 0) {
        return DSD_IQ_OK;
    }

    set_error(err_buf, err_buf_size, "metadata sidecar not found for '%s' (tried '%s.json' and '%s.iq.json')", path,
              path, path);
    return DSD_IQ_ERR_IO;
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
    DSD_MEMCPY(out_data_path, metadata_path, dir_len);
    DSD_MEMCPY(out_data_path + dir_len, data_file, file_len);
    out_data_path[dir_len + file_len] = '\0';
    return DSD_IQ_OK;
}

typedef enum {
    JTOK_ERROR = -1,
    JTOK_EOF,
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

static json_token
make_simple_token(json_token_type type, size_t offset) {
    json_token tok;
    DSD_MEMSET(&tok, 0, sizeof(tok));
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

static int
tokenizer_append_string_char(json_tokenizer* tk, size_t start, size_t* write_pos, unsigned char ch) {
    if (*write_pos + 1 >= sizeof(tk->str_buf)) {
        tokenizer_set_error(tk, "string exceeds parser buffer", start);
        return 0;
    }
    tk->str_buf[(*write_pos)++] = (char)ch;
    return 1;
}

static int
tokenizer_parse_unicode_escape(json_tokenizer* tk, unsigned char* out_ch) {
    if (tk->pos + 4 > tk->src_len) {
        tokenizer_set_error(tk, "truncated unicode escape", tk->pos);
        return 0;
    }
    uint64_t parsed = 0U;
    if (dsd_parse_hex_u64_n(tk->src + tk->pos, 4U, &parsed) != 0) {
        tokenizer_set_error(tk, "invalid unicode escape", tk->pos);
        return 0;
    }
    const unsigned int codepoint = (unsigned int)parsed;
    if (codepoint == 0U) {
        tokenizer_set_error(tk, "embedded NUL is not allowed", tk->pos);
        return 0;
    }
    if (codepoint > 0x7FU) {
        tokenizer_set_error(tk, "unicode codepoint > 0x7f is unsupported in IQ metadata", tk->pos);
        return 0;
    }
    *out_ch = (unsigned char)codepoint;
    tk->pos += 4;
    return 1;
}

static int
tokenizer_parse_escape_sequence(json_tokenizer* tk, unsigned char* out_ch) {
    if (tk->pos >= tk->src_len) {
        tokenizer_set_error(tk, "truncated escape sequence", tk->pos);
        return 0;
    }
    unsigned char esc = (unsigned char)tk->src[tk->pos++];
    switch (esc) {
        case '"': *out_ch = '"'; return 1;
        case '\\': *out_ch = '\\'; return 1;
        case '/': *out_ch = '/'; return 1;
        case 'b': *out_ch = '\b'; return 1;
        case 'f': *out_ch = '\f'; return 1;
        case 'n': *out_ch = '\n'; return 1;
        case 'r': *out_ch = '\r'; return 1;
        case 't': *out_ch = '\t'; return 1;
        case 'u': return tokenizer_parse_unicode_escape(tk, out_ch);
        default: tokenizer_set_error(tk, "invalid escape sequence", tk->pos - 1); return 0;
    }
}

static int
tokenizer_parse_string_token(json_tokenizer* tk, size_t start, json_token* out_tok) {
    size_t write_pos = 0;
    tk->pos++;
    while (tk->pos < tk->src_len) {
        unsigned char ch = (unsigned char)tk->src[tk->pos++];
        if (ch < 0x20) {
            tokenizer_set_error(tk, "unescaped control char in string", tk->pos - 1);
            return 0;
        }
        if (ch == '"') {
            tk->str_buf[write_pos] = '\0';
            out_tok->type = JTOK_STRING;
            out_tok->offset = start;
            out_tok->str = tk->str_buf;
            out_tok->str_len = write_pos;
            return 1;
        }
        if (ch == '\\') {
            unsigned char out_ch = 0;
            if (!tokenizer_parse_escape_sequence(tk, &out_ch)
                || !tokenizer_append_string_char(tk, start, &write_pos, out_ch)) {
                return 0;
            }
            continue;
        }
        if (!tokenizer_append_string_char(tk, start, &write_pos, ch)) {
            return 0;
        }
    }
    tokenizer_set_error(tk, "unterminated string", start);
    return 0;
}

static int
tokenizer_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int
tokenizer_parse_number_integer_part(json_tokenizer* tk, size_t start, size_t* p) {
    if (tk->src[*p] == '-') {
        (*p)++;
        if (*p >= tk->src_len || !tokenizer_is_digit(tk->src[*p])) {
            tokenizer_set_error(tk, "invalid number", start);
            return 0;
        }
    }
    if (*p < tk->src_len && tk->src[*p] == '0') {
        (*p)++;
        return 1;
    }
    if (*p >= tk->src_len || !tokenizer_is_digit(tk->src[*p])) {
        tokenizer_set_error(tk, "invalid number", start);
        return 0;
    }
    while (*p < tk->src_len && tokenizer_is_digit(tk->src[*p])) {
        (*p)++;
    }
    return 1;
}

static int
tokenizer_parse_number_fraction_part(json_tokenizer* tk, size_t start, size_t* p, int* is_float) {
    if (*p >= tk->src_len || tk->src[*p] != '.') {
        return 1;
    }
    *is_float = 1;
    (*p)++;
    if (*p >= tk->src_len || !tokenizer_is_digit(tk->src[*p])) {
        tokenizer_set_error(tk, "invalid fractional number", start);
        return 0;
    }
    while (*p < tk->src_len && tokenizer_is_digit(tk->src[*p])) {
        (*p)++;
    }
    return 1;
}

static int
tokenizer_parse_number_exponent_part(json_tokenizer* tk, size_t start, size_t* p, int* is_float) {
    if (*p >= tk->src_len || (tk->src[*p] != 'e' && tk->src[*p] != 'E')) {
        return 1;
    }
    *is_float = 1;
    (*p)++;
    if (*p < tk->src_len && (tk->src[*p] == '+' || tk->src[*p] == '-')) {
        (*p)++;
    }
    if (*p >= tk->src_len || !tokenizer_is_digit(tk->src[*p])) {
        tokenizer_set_error(tk, "invalid exponent", start);
        return 0;
    }
    while (*p < tk->src_len && tokenizer_is_digit(tk->src[*p])) {
        (*p)++;
    }
    return 1;
}

static int
tokenizer_parse_number_token(json_tokenizer* tk, size_t start, json_token* out_tok) {
    int is_float = 0;
    size_t p = tk->pos;
    if (!tokenizer_parse_number_integer_part(tk, start, &p)
        || !tokenizer_parse_number_fraction_part(tk, start, &p, &is_float)
        || !tokenizer_parse_number_exponent_part(tk, start, &p, &is_float)) {
        return 0;
    }

    out_tok->type = is_float ? JTOK_NUMBER_FLOAT : JTOK_NUMBER_INT;
    out_tok->offset = start;
    out_tok->num = tk->src + start;
    out_tok->num_len = p - start;
    tk->pos = p;
    return 1;
}

static int
tokenizer_match_keyword(json_tokenizer* tk, size_t start, const char* kw, size_t kw_len, json_token_type type,
                        json_token* out_tok) {
    if (start + kw_len > tk->src_len || strncmp(tk->src + start, kw, kw_len) != 0) {
        return 0;
    }
    tk->pos = start + kw_len;
    *out_tok = make_simple_token(type, start);
    return 1;
}

static int
tokenizer_parse_simple_token(json_tokenizer* tk, char c, size_t start, json_token* out_tok) {
    json_token_type type;
    switch (c) {
        case '{': type = JTOK_LBRACE; break;
        case '}': type = JTOK_RBRACE; break;
        case '[': type = JTOK_LBRACKET; break;
        case ']': type = JTOK_RBRACKET; break;
        case ',': type = JTOK_COMMA; break;
        case ':': type = JTOK_COLON; break;
        default: return 0;
    }
    tk->pos++;
    *out_tok = make_simple_token(type, start);
    return 1;
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
    if (tokenizer_parse_simple_token(tk, c, start, &tok)) {
        return tok;
    }
    if (c == '"') {
        if (!tokenizer_parse_string_token(tk, start, &tok)) {
            return make_simple_token(JTOK_ERROR, start);
        }
        return tok;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (!tokenizer_parse_number_token(tk, start, &tok)) {
            return make_simple_token(JTOK_ERROR, start);
        }
        return tok;
    }
    if (tokenizer_match_keyword(tk, start, "true", 4, JTOK_TRUE, &tok)
        || tokenizer_match_keyword(tk, start, "false", 5, JTOK_FALSE, &tok)
        || tokenizer_match_keyword(tk, start, "null", 4, JTOK_NULL, &tok)) {
        return tok;
    }

    tokenizer_set_error(tk, "unexpected JSON token", start);
    return make_simple_token(JTOK_ERROR, start);
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
    DSD_MEMCPY(out, tok->str, tok->str_len);
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
    if (tok->num[0] == '-') {
        set_error(err_buf, err_buf_size, "integer for '%s' must be non-negative", field_name);
        return DSD_IQ_ERR_INVALID_META;
    }
    DSD_MEMCPY(buf, tok->num, tok->num_len);
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
    DSD_MEMCPY(buf, tok->num, tok->num_len);
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
    unsigned tuner_gain_known            : 1;
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
    unsigned events                      : 1;
} replay_seen_fields;

static int
validate_replay_semantics(const dsd_iq_replay_config* cfg, char* err_buf, size_t err_buf_size) {
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
    /* The half-band cascade keeps one history buffer per pass, so the replay chain must not
       ask for more passes than the demodulator state has room for. */
    if (cfg->base_decimation > DSD_IQ_REPLAY_MAX_BASE_DECIMATION) {
        set_error(err_buf, err_buf_size, "base_decimation (%u) exceeds the maximum of %u", cfg->base_decimation,
                  (unsigned)DSD_IQ_REPLAY_MAX_BASE_DECIMATION);
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
    /* The chain runs from the rate the demodulator takes: the capture's, or the one a rate plan resamples it to. */
    uint64_t derived = (uint64_t)dsd_iq_replay_dsp_input_rate_hz(cfg) / (uint64_t)cfg->base_decimation
                       / (uint64_t)cfg->post_downsample;
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
    return DSD_IQ_OK;
}

/* The sample format and capture stage pairs a replay can convert to the demodulator's float input, as the replay
 * reader's converters do (replay_convert_block_to_f32() in rtl_device.cpp): cu8 at either capture stage, cf32 only
 * as the driver delivered it, before the input ring, and little-endian cs16 only at the cu8 stage, where it stands in
 * for a widened cu8 capture. */
static int
validate_replay_convertible(const dsd_iq_replay_config* cfg, char* err_buf, size_t err_buf_size) {
    if (cfg->format == DSD_IQ_FORMAT_CU8) {
        return DSD_IQ_OK;
    }
    if (cfg->format == DSD_IQ_FORMAT_CS16) {
        if (strcmp(cfg->capture_stage, "post_mute_pre_widen") == 0) {
            return DSD_IQ_OK;
        }
        set_error(err_buf, err_buf_size,
                  "replay cannot convert sample_format 'cs16' at capture_stage '%s' (cs16 replays only from "
                  "'post_mute_pre_widen')",
                  cfg->capture_stage);
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    if (cfg->format == DSD_IQ_FORMAT_CF32) {
        if (strcmp(cfg->capture_stage, "post_driver_cf32_pre_ring") == 0) {
            return DSD_IQ_OK;
        }
        set_error(err_buf, err_buf_size,
                  "replay cannot convert sample_format 'cf32' at capture_stage '%s' (cf32 replays only from "
                  "'post_driver_cf32_pre_ring')",
                  cfg->capture_stage);
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    set_error(err_buf, err_buf_size, "replay cannot convert sample_format '%s' (replay accepts cu8, cf32 and cs16)",
              dsd_iq_sample_format_name(cfg->format));
    return DSD_IQ_ERR_UNSUPPORTED_FMT;
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
event_kind_from_string(const char* s, dsd_iq_event_kind* out) {
    if (!s || !out) {
        return 0;
    }
    if (strcmp(s, "RETUNE") == 0) {
        *out = DSD_IQ_EVENT_RETUNE;
        return 1;
    }
    if (strcmp(s, "MUTE") == 0) {
        *out = DSD_IQ_EVENT_MUTE;
        return 1;
    }
    if (strcmp(s, "RESET") == 0) {
        *out = DSD_IQ_EVENT_RESET;
        return 1;
    }
    return 0;
}

static int
append_parsed_event(dsd_iq_event** events, size_t* count, size_t* cap, const dsd_iq_event* event) {
    if (!events || !count || !cap || !event) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (*count == *cap) {
        size_t new_cap = (*cap == 0U) ? 8U : (*cap * 2U);
        if (new_cap < *cap) {
            return DSD_IQ_ERR_ALLOC;
        }
        dsd_iq_event* next = (dsd_iq_event*)realloc(*events, new_cap * sizeof(*next));
        if (!next) {
            return DSD_IQ_ERR_ALLOC;
        }
        *events = next;
        *cap = new_cap;
    }
    (*events)[(*count)++] = *event;
    return DSD_IQ_OK;
}

typedef struct {
    unsigned kind                        : 1;
    unsigned byte_offset                 : 1;
    unsigned duration_bytes              : 1;
    unsigned center_frequency_hz         : 1;
    unsigned capture_center_frequency_hz : 1;
    unsigned sample_rate_hz              : 1;
    unsigned reason                      : 1;
} event_seen_fields;

static int
parse_event_key_value(json_tokenizer* tk, char* key_buf, size_t key_buf_size, json_token* out_val_tok, int* out_done,
                      char* err_buf, size_t err_buf_size) {
    json_token key_tok = tokenizer_next(tk);
    if (tk->err_msg) {
        set_error(err_buf, err_buf_size, "%s at byte %zu", tk->err_msg, tk->err_pos);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (key_tok.type == JTOK_RBRACE) {
        *out_done = 1;
        return DSD_IQ_OK;
    }
    if (key_tok.type != JTOK_STRING) {
        set_error(err_buf, err_buf_size, "expected event string key at byte %zu", key_tok.offset);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (key_tok.str_len + 1 > key_buf_size) {
        set_error(err_buf, err_buf_size, "event key too long at byte %zu", key_tok.offset);
        return DSD_IQ_ERR_INVALID_META;
    }
    DSD_MEMCPY(key_buf, key_tok.str, key_tok.str_len);
    key_buf[key_tok.str_len] = '\0';

    json_token colon_tok = tokenizer_next(tk);
    if (colon_tok.type != JTOK_COLON) {
        set_error(err_buf, err_buf_size, "expected ':' after event key at byte %zu", key_tok.offset);
        return DSD_IQ_ERR_INVALID_META;
    }

    *out_val_tok = tokenizer_next(tk);
    if (tk->err_msg) {
        set_error(err_buf, err_buf_size, "%s at byte %zu", tk->err_msg, tk->err_pos);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (out_val_tok->type == JTOK_LBRACE || out_val_tok->type == JTOK_LBRACKET) {
        set_error(err_buf, err_buf_size, "nested event fields are unsupported at byte %zu", out_val_tok->offset);
        return DSD_IQ_ERR_INVALID_META;
    }
    *out_done = 0;
    return DSD_IQ_OK;
}

static int
parse_event_field_value_a(const char* key, const json_token* val_tok, dsd_iq_event* ev, event_seen_fields* seen,
                          int* handled, char* err_buf, size_t err_buf_size) {
    int rc = DSD_IQ_OK;
    if (strcmp(key, "kind") == 0) {
        if (val_tok->type != JTOK_STRING) {
            set_error(err_buf, err_buf_size, "event field 'kind' expects string");
            return DSD_IQ_ERR_INVALID_META;
        }
        char kind_buf[32];
        rc = copy_token_to_buffer(val_tok, kind_buf, sizeof(kind_buf), err_buf, err_buf_size, "events.kind", 1);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
        if (!event_kind_from_string(kind_buf, &ev->kind)) {
            set_error(err_buf, err_buf_size, "unsupported IQ event kind '%s'", kind_buf);
            return DSD_IQ_ERR_INVALID_META;
        }
        seen->kind = 1;
        *handled = 1;
    } else if (strcmp(key, "byte_offset") == 0) {
        if (val_tok->type != JTOK_NUMBER_INT) {
            set_error(err_buf, err_buf_size, "event field 'byte_offset' expects integer");
            return DSD_IQ_ERR_INVALID_META;
        }
        rc = token_to_u64(val_tok, &ev->byte_offset, err_buf, err_buf_size, "events.byte_offset");
        seen->byte_offset = 1;
        *handled = 1;
    } else if (strcmp(key, "duration_bytes") == 0) {
        if (val_tok->type != JTOK_NUMBER_INT) {
            set_error(err_buf, err_buf_size, "event field 'duration_bytes' expects integer");
            return DSD_IQ_ERR_INVALID_META;
        }
        rc = token_to_u64(val_tok, &ev->duration_bytes, err_buf, err_buf_size, "events.duration_bytes");
        seen->duration_bytes = 1;
        *handled = 1;
    } else if (strcmp(key, "center_frequency_hz") == 0) {
        if (val_tok->type != JTOK_NUMBER_INT) {
            set_error(err_buf, err_buf_size, "event field 'center_frequency_hz' expects integer");
            return DSD_IQ_ERR_INVALID_META;
        }
        rc = token_to_u64(val_tok, &ev->center_frequency_hz, err_buf, err_buf_size, "events.center_frequency_hz");
        seen->center_frequency_hz = 1;
        *handled = 1;
    }
    return rc;
}

static int
parse_event_field_value_b(const char* key, const json_token* val_tok, dsd_iq_event* ev, event_seen_fields* seen,
                          int* handled, char* err_buf, size_t err_buf_size) {
    int rc = DSD_IQ_OK;
    if (strcmp(key, "capture_center_frequency_hz") == 0) {
        if (val_tok->type != JTOK_NUMBER_INT) {
            set_error(err_buf, err_buf_size, "event field 'capture_center_frequency_hz' expects integer");
            return DSD_IQ_ERR_INVALID_META;
        }
        rc = token_to_u64(val_tok, &ev->capture_center_frequency_hz, err_buf, err_buf_size,
                          "events.capture_center_frequency_hz");
        seen->capture_center_frequency_hz = 1;
        *handled = 1;
    } else if (strcmp(key, "sample_rate_hz") == 0) {
        if (val_tok->type != JTOK_NUMBER_INT) {
            set_error(err_buf, err_buf_size, "event field 'sample_rate_hz' expects integer");
            return DSD_IQ_ERR_INVALID_META;
        }
        rc = token_to_u32(val_tok, &ev->sample_rate_hz, err_buf, err_buf_size, "events.sample_rate_hz");
        seen->sample_rate_hz = 1;
        *handled = 1;
    } else if (strcmp(key, "reason") == 0) {
        if (val_tok->type != JTOK_STRING) {
            set_error(err_buf, err_buf_size, "event field 'reason' expects string");
            return DSD_IQ_ERR_INVALID_META;
        }
        rc = copy_token_to_buffer(val_tok, ev->reason, sizeof(ev->reason), err_buf, err_buf_size, "events.reason", 1);
        seen->reason = 1;
        *handled = 1;
    }
    return rc;
}

static int
parse_event_field_value(const char* key, const json_token* val_tok, dsd_iq_event* ev, event_seen_fields* seen,
                        char* err_buf, size_t err_buf_size) {
    int handled = 0;
    int rc = parse_event_field_value_a(key, val_tok, ev, seen, &handled, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK || handled) {
        return rc;
    }
    return parse_event_field_value_b(key, val_tok, ev, seen, &handled, err_buf, err_buf_size);
}

static int
parse_event_field_delim(json_tokenizer* tk, int* out_done, char* err_buf, size_t err_buf_size) {
    json_token delim_tok = tokenizer_next(tk);
    if (delim_tok.type == JTOK_COMMA) {
        *out_done = 0;
        return DSD_IQ_OK;
    }
    if (delim_tok.type == JTOK_RBRACE) {
        *out_done = 1;
        return DSD_IQ_OK;
    }
    if (tk->err_msg) {
        set_error(err_buf, err_buf_size, "%s at byte %zu", tk->err_msg, tk->err_pos);
    } else {
        set_error(err_buf, err_buf_size, "expected ',' or '}' after event field at byte %zu", delim_tok.offset);
    }
    return DSD_IQ_ERR_INVALID_META;
}

static int
validate_event_object_fields(const dsd_iq_event* ev, const event_seen_fields* seen, char* err_buf,
                             size_t err_buf_size) {
    if (!seen->kind || !seen->byte_offset || !seen->reason) {
        set_error(err_buf, err_buf_size, "event is missing required kind, byte_offset, or reason");
        return DSD_IQ_ERR_INVALID_META;
    }
    if (ev->kind == DSD_IQ_EVENT_MUTE) {
        if (!seen->duration_bytes || ev->duration_bytes == 0) {
            set_error(err_buf, err_buf_size, "MUTE event requires positive duration_bytes");
            return DSD_IQ_ERR_INVALID_META;
        }
        return DSD_IQ_OK;
    }
    if (!seen->center_frequency_hz || !seen->capture_center_frequency_hz || !seen->sample_rate_hz
        || ev->center_frequency_hz == 0 || ev->capture_center_frequency_hz == 0 || ev->sample_rate_hz == 0) {
        set_error(
            err_buf, err_buf_size,
            "RETUNE and RESET events require center_frequency_hz, capture_center_frequency_hz, and sample_rate_hz");
        return DSD_IQ_ERR_INVALID_META;
    }
    return DSD_IQ_OK;
}

static int
parse_event_object(json_tokenizer* tk, dsd_iq_event* out, char* err_buf, size_t err_buf_size) {
    if (!tk || !out) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    event_seen_fields seen;
    DSD_MEMSET(&seen, 0, sizeof(seen));

    for (;;) {
        char key_buf[128];
        json_token val_tok;
        int done = 0;
        int rc = parse_event_key_value(tk, key_buf, sizeof(key_buf), &val_tok, &done, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
        if (done) {
            break;
        }
        rc = parse_event_field_value(key_buf, &val_tok, &ev, &seen, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
        rc = parse_event_field_delim(tk, &done, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
        if (done) {
            break;
        }
    }

    int vrc = validate_event_object_fields(&ev, &seen, err_buf, err_buf_size);
    if (vrc != DSD_IQ_OK) {
        return vrc;
    }

    *out = ev;
    return DSD_IQ_OK;
}

static int
parse_events_array(json_tokenizer* tk, dsd_iq_event** out_events, uint32_t* out_count, char* err_buf,
                   size_t err_buf_size) {
    if (!tk || !out_events || !out_count) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    dsd_iq_event* events = NULL;
    size_t count = 0;
    size_t cap = 0;

    json_token tok = tokenizer_next(tk);
    if (tok.type == JTOK_RBRACKET) {
        *out_events = NULL;
        *out_count = 0;
        return DSD_IQ_OK;
    }

    for (;;) {
        if (tok.type != JTOK_LBRACE) {
            set_error(err_buf, err_buf_size, "events array expects objects at byte %zu", tok.offset);
            free(events);
            return DSD_IQ_ERR_INVALID_META;
        }
        if (count >= UINT32_MAX) {
            set_error(err_buf, err_buf_size, "too many IQ events");
            free(events);
            return DSD_IQ_ERR_INVALID_META;
        }
        dsd_iq_event ev;
        int rc = parse_event_object(tk, &ev, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            free(events);
            return rc;
        }
        rc = append_parsed_event(&events, &count, &cap, &ev);
        if (rc != DSD_IQ_OK) {
            set_error(err_buf, err_buf_size, "event allocation failed");
            free(events);
            return rc;
        }

        tok = tokenizer_next(tk);
        if (tok.type == JTOK_COMMA) {
            tok = tokenizer_next(tk);
            continue;
        }
        if (tok.type == JTOK_RBRACKET) {
            break;
        }
        if (tk->err_msg) {
            set_error(err_buf, err_buf_size, "%s at byte %zu", tk->err_msg, tk->err_pos);
        } else {
            set_error(err_buf, err_buf_size, "expected ',' or ']' after event at byte %zu", tok.offset);
        }
        free(events);
        return DSD_IQ_ERR_INVALID_META;
    }

    *out_events = events;
    *out_count = (uint32_t)count;
    return DSD_IQ_OK;
}

static int
validate_replay_events_metadata_preamble(const dsd_iq_replay_config* cfg, int reject_missing_timeline, char* err_buf,
                                         size_t err_buf_size) {
    if (cfg->event_count > 0U && cfg->metadata_version != 2U) {
        set_error(err_buf, err_buf_size, "events require metadata version 2");
        return DSD_IQ_ERR_UNSUPPORTED_VER;
    }
    if (reject_missing_timeline && (cfg->contains_retunes || cfg->capture_retune_count > 0U)
        && cfg->event_count == 0U) {
        set_error(err_buf, err_buf_size, "capture contains retunes but has no replay event timeline");
        return DSD_IQ_ERR_RETUNE_REJECT;
    }
    return DSD_IQ_OK;
}

typedef struct {
    uint64_t prev_offset;
    int has_frequency_event;
    uint32_t retune_event_count;
    uint32_t completed_retune_reset_count;
    int retune_needs_reset;
} replay_event_validation_state;

static int
validate_replay_event_offset(const dsd_iq_event* ev, uint32_t index, uint64_t max_offset, int check_max_offset,
                             size_t align, replay_event_validation_state* st, char* err_buf, size_t err_buf_size) {
    if (index > 0U && ev->byte_offset < st->prev_offset) {
        set_error(err_buf, err_buf_size, "IQ events are not sorted by byte_offset");
        return DSD_IQ_ERR_INVALID_META;
    }
    st->prev_offset = ev->byte_offset;
    if (check_max_offset && ev->byte_offset > max_offset) {
        set_error(err_buf, err_buf_size, "IQ event byte_offset %" PRIu64 " exceeds replay bytes %" PRIu64,
                  ev->byte_offset, max_offset);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (align > 0U && (ev->byte_offset % (uint64_t)align) != 0ULL) {
        set_error(err_buf, err_buf_size, "IQ event byte_offset is not aligned to sample format");
        return DSD_IQ_ERR_ALIGNMENT;
    }
    return DSD_IQ_OK;
}

static int
validate_replay_mute_event(const dsd_iq_event* ev, size_t align, char* err_buf, size_t err_buf_size) {
    if (ev->duration_bytes == 0) {
        set_error(err_buf, err_buf_size, "MUTE event duration_bytes must be > 0");
        return DSD_IQ_ERR_INVALID_META;
    }
    if (align > 0U && (ev->duration_bytes % (uint64_t)align) != 0ULL) {
        set_error(err_buf, err_buf_size, "MUTE event duration_bytes is not aligned to sample format");
        return DSD_IQ_ERR_ALIGNMENT;
    }
    return DSD_IQ_OK;
}

static int
validate_replay_frequency_event(const dsd_iq_replay_config* cfg, const dsd_iq_event* ev, int reject_missing_timeline,
                                replay_event_validation_state* st, char* err_buf, size_t err_buf_size) {
    st->has_frequency_event = 1;
    if (ev->center_frequency_hz == 0 || ev->capture_center_frequency_hz == 0 || ev->sample_rate_hz == 0) {
        set_error(err_buf, err_buf_size, "RETUNE/RESET event is missing frequency or sample-rate fields");
        return DSD_IQ_ERR_INVALID_META;
    }
    if (ev->sample_rate_hz != cfg->sample_rate_hz) {
        set_error(err_buf, err_buf_size, "event sample_rate_hz changes are not supported for replay");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (ev->kind == DSD_IQ_EVENT_RETUNE) {
        if (reject_missing_timeline && st->retune_needs_reset) {
            set_error(err_buf, err_buf_size, "RETUNE event is missing a following RESET event");
            return DSD_IQ_ERR_RETUNE_REJECT;
        }
        st->retune_event_count++;
        st->retune_needs_reset = 1;
        return DSD_IQ_OK;
    }
    if (st->retune_needs_reset) {
        st->completed_retune_reset_count++;
        st->retune_needs_reset = 0;
    }
    return DSD_IQ_OK;
}

static int
validate_replay_event_entry(const dsd_iq_replay_config* cfg, const dsd_iq_event* ev, uint32_t index,
                            uint64_t max_offset, int check_max_offset, size_t align, int reject_missing_timeline,
                            replay_event_validation_state* st, char* err_buf, size_t err_buf_size) {
    int rc = validate_replay_event_offset(ev, index, max_offset, check_max_offset, align, st, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    if (ev->kind == DSD_IQ_EVENT_MUTE) {
        return validate_replay_mute_event(ev, align, err_buf, err_buf_size);
    }
    if (ev->kind == DSD_IQ_EVENT_RETUNE || ev->kind == DSD_IQ_EVENT_RESET) {
        return validate_replay_frequency_event(cfg, ev, reject_missing_timeline, st, err_buf, err_buf_size);
    }
    set_error(err_buf, err_buf_size, "unsupported IQ event kind %d", (int)ev->kind);
    return DSD_IQ_ERR_INVALID_META;
}

static int
validate_replay_event_timeline(const dsd_iq_replay_config* cfg, int reject_missing_timeline,
                               const replay_event_validation_state* st, char* err_buf, size_t err_buf_size) {
    if (reject_missing_timeline && cfg->contains_retunes && !st->has_frequency_event) {
        set_error(err_buf, err_buf_size, "capture contains retunes but event timeline has no RETUNE/RESET event");
        return DSD_IQ_ERR_RETUNE_REJECT;
    }
    if (!reject_missing_timeline
        || (!cfg->contains_retunes && cfg->capture_retune_count == 0U && st->retune_event_count == 0U)) {
        return DSD_IQ_OK;
    }
    if (st->retune_needs_reset) {
        set_error(err_buf, err_buf_size, "capture retune timeline is missing a RESET event");
        return DSD_IQ_ERR_RETUNE_REJECT;
    }
    if (cfg->capture_retune_count == 0U) {
        set_error(err_buf, err_buf_size, "capture contains retunes but capture_retune_count is zero");
        return DSD_IQ_ERR_RETUNE_REJECT;
    }
    if (st->retune_event_count != cfg->capture_retune_count) {
        set_error(err_buf, err_buf_size, "capture retune count does not match RETUNE events");
        return DSD_IQ_ERR_RETUNE_REJECT;
    }
    if (st->completed_retune_reset_count != cfg->capture_retune_count) {
        set_error(err_buf, err_buf_size, "capture retune timeline is missing a RESET event");
        return DSD_IQ_ERR_RETUNE_REJECT;
    }
    return DSD_IQ_OK;
}

static int
validate_replay_events_metadata(const dsd_iq_replay_config* cfg, uint64_t max_offset, int check_max_offset,
                                int reject_missing_timeline, char* err_buf, size_t err_buf_size) {
    if (!cfg) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    int pre_rc = validate_replay_events_metadata_preamble(cfg, reject_missing_timeline, err_buf, err_buf_size);
    if (pre_rc != DSD_IQ_OK) {
        return pre_rc;
    }

    size_t align = dsd_iq_sample_format_alignment_bytes(cfg->format);
    replay_event_validation_state st;
    DSD_MEMSET(&st, 0, sizeof(st));

    for (uint32_t i = 0; i < cfg->event_count; i++) {
        const dsd_iq_event* ev = &cfg->events[i];
        int rc = validate_replay_event_entry(cfg, ev, i, max_offset, check_max_offset, align, reject_missing_timeline,
                                             &st, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
    }
    return validate_replay_event_timeline(cfg, reject_missing_timeline, &st, err_buf, err_buf_size);
}

typedef struct {
    dsd_iq_replay_config cfg;
    replay_seen_fields seen;
    int version;
    char format_buf[64];
    char sample_format_buf[32];
    char iq_order_buf[16];
    char endianness_buf[16];
    char data_file_buf[2048];
} metadata_parse_state;

static int
metadata_expect_string(const json_token* tok, const char* field_name, char* err_buf, size_t err_buf_size) {
    if (tok->type == JTOK_STRING) {
        return DSD_IQ_OK;
    }
    set_error(err_buf, err_buf_size, "field '%s' expects string", field_name);
    return DSD_IQ_ERR_INVALID_META;
}

static int
metadata_expect_integer(const json_token* tok, const char* field_name, char* err_buf, size_t err_buf_size) {
    if (tok->type == JTOK_NUMBER_INT) {
        return DSD_IQ_OK;
    }
    set_error(err_buf, err_buf_size, "field '%s' expects integer", field_name);
    return DSD_IQ_ERR_INVALID_META;
}

static int
metadata_parse_field_group_a1(metadata_parse_state* st, const char* key, const json_token* val_tok, int* handled,
                              char* err_buf, size_t err_buf_size) {
    int rc = DSD_IQ_OK;
    if (strcmp(key, "format") == 0) {
        rc = metadata_expect_string(val_tok, "format", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = copy_token_to_buffer(val_tok, st->format_buf, sizeof(st->format_buf), err_buf, err_buf_size, "format",
                                      1);
            st->seen.format = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "version") == 0) {
        rc = metadata_expect_integer(val_tok, "version", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_i32(val_tok, &st->version, err_buf, err_buf_size, "version");
            st->seen.version = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "sample_format") == 0) {
        rc = metadata_expect_string(val_tok, "sample_format", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = copy_token_to_buffer(val_tok, st->sample_format_buf, sizeof(st->sample_format_buf), err_buf,
                                      err_buf_size, "sample_format", 1);
            st->seen.sample_format = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "iq_order") == 0) {
        rc = metadata_expect_string(val_tok, "iq_order", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = copy_token_to_buffer(val_tok, st->iq_order_buf, sizeof(st->iq_order_buf), err_buf, err_buf_size,
                                      "iq_order", 1);
            st->seen.iq_order = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "endianness") == 0) {
        rc = metadata_expect_string(val_tok, "endianness", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = copy_token_to_buffer(val_tok, st->endianness_buf, sizeof(st->endianness_buf), err_buf, err_buf_size,
                                      "endianness", 1);
            st->seen.endianness = 1;
        }
        *handled = 1;
    }
    return rc;
}

static int
metadata_parse_field_group_a2(metadata_parse_state* st, const char* key, const json_token* val_tok, int* handled,
                              char* err_buf, size_t err_buf_size) {
    int rc = DSD_IQ_OK;
    if (strcmp(key, "capture_stage") == 0) {
        rc = metadata_expect_string(val_tok, "capture_stage", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = copy_token_to_buffer(val_tok, st->cfg.capture_stage, sizeof(st->cfg.capture_stage), err_buf,
                                      err_buf_size, "capture_stage", 1);
            st->seen.capture_stage = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "sample_rate_hz") == 0) {
        rc = metadata_expect_integer(val_tok, "sample_rate_hz", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u32(val_tok, &st->cfg.sample_rate_hz, err_buf, err_buf_size, "sample_rate_hz");
            st->seen.sample_rate_hz = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "center_frequency_hz") == 0) {
        rc = metadata_expect_integer(val_tok, "center_frequency_hz", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u64(val_tok, &st->cfg.center_frequency_hz, err_buf, err_buf_size, "center_frequency_hz");
            st->seen.center_frequency_hz = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "capture_center_frequency_hz") == 0) {
        rc = metadata_expect_integer(val_tok, "capture_center_frequency_hz", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u64(val_tok, &st->cfg.capture_center_frequency_hz, err_buf, err_buf_size,
                              "capture_center_frequency_hz");
            st->seen.capture_center_frequency_hz = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "ppm") == 0) {
        rc = metadata_expect_integer(val_tok, "ppm", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_i32(val_tok, &st->cfg.ppm, err_buf, err_buf_size, "ppm");
            st->seen.ppm = 1;
        }
        *handled = 1;
    }
    return rc;
}

static int
metadata_parse_field_group_a(metadata_parse_state* st, const char* key, const json_token* val_tok, int* handled,
                             char* err_buf, size_t err_buf_size) {
    int rc = metadata_parse_field_group_a1(st, key, val_tok, handled, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK || *handled) {
        return rc;
    }
    return metadata_parse_field_group_a2(st, key, val_tok, handled, err_buf, err_buf_size);
}

static int
metadata_parse_field_group_b1(metadata_parse_state* st, const char* key, const json_token* val_tok, int* handled,
                              char* err_buf, size_t err_buf_size) {
    int rc = DSD_IQ_OK;
    if (strcmp(key, "tuner_gain_tenth_db") == 0) {
        rc = metadata_expect_integer(val_tok, "tuner_gain_tenth_db", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_i32(val_tok, &st->cfg.tuner_gain_tenth_db, err_buf, err_buf_size, "tuner_gain_tenth_db");
            st->seen.tuner_gain_tenth_db = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "tuner_gain_known") == 0) {
        /* Optional: metadata written before this field existed is still valid (see metadata_finalize()). */
        int known = 1;
        rc = token_to_bool(val_tok, &known, err_buf, err_buf_size, "tuner_gain_known");
        st->cfg.tuner_gain_unknown = known ? 0 : 1;
        st->seen.tuner_gain_known = 1;
        *handled = 1;
    } else if (strcmp(key, "rtl_dsp_bw_khz") == 0) {
        rc = metadata_expect_integer(val_tok, "rtl_dsp_bw_khz", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_i32(val_tok, &st->cfg.rtl_dsp_bw_khz, err_buf, err_buf_size, "rtl_dsp_bw_khz");
            st->seen.rtl_dsp_bw_khz = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "base_decimation") == 0) {
        rc = metadata_expect_integer(val_tok, "base_decimation", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u32(val_tok, &st->cfg.base_decimation, err_buf, err_buf_size, "base_decimation");
            st->seen.base_decimation = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "post_downsample") == 0) {
        rc = metadata_expect_integer(val_tok, "post_downsample", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u32(val_tok, &st->cfg.post_downsample, err_buf, err_buf_size, "post_downsample");
            st->seen.post_downsample = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "demod_rate_hz") == 0) {
        rc = metadata_expect_integer(val_tok, "demod_rate_hz", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u32(val_tok, &st->cfg.demod_rate_hz, err_buf, err_buf_size, "demod_rate_hz");
            st->seen.demod_rate_hz = 1;
        }
        *handled = 1;
    }
    return rc;
}

static int
metadata_parse_field_group_b2(metadata_parse_state* st, const char* key, const json_token* val_tok, int* handled,
                              char* err_buf, size_t err_buf_size) {
    int rc = DSD_IQ_OK;
    if (strcmp(key, "offset_tuning_enabled") == 0) {
        rc = token_to_bool(val_tok, &st->cfg.offset_tuning_enabled, err_buf, err_buf_size, "offset_tuning_enabled");
        st->seen.offset_tuning_enabled = 1;
        *handled = 1;
    } else if (strcmp(key, "fs4_shift_enabled") == 0) {
        rc = token_to_bool(val_tok, &st->cfg.fs4_shift_enabled, err_buf, err_buf_size, "fs4_shift_enabled");
        st->seen.fs4_shift_enabled = 1;
        *handled = 1;
    } else if (strcmp(key, "combine_rotate_enabled") == 0) {
        int combine_rotate = 0;
        rc = token_to_bool(val_tok, &combine_rotate, err_buf, err_buf_size, "combine_rotate_enabled");
        if (rc == DSD_IQ_OK) {
            st->cfg.historical_cu8_two_pass = combine_rotate ? 0 : 1;
        }
        st->seen.combine_rotate_enabled = 1;
        *handled = 1;
    } else if (strcmp(key, "muted_bytes_excluded") == 0) {
        rc = token_to_bool(val_tok, &st->cfg.muted_bytes_excluded, err_buf, err_buf_size, "muted_bytes_excluded");
        st->seen.muted_bytes_excluded = 1;
        *handled = 1;
    } else if (strcmp(key, "contains_retunes") == 0) {
        rc = token_to_bool(val_tok, &st->cfg.contains_retunes, err_buf, err_buf_size, "contains_retunes");
        st->seen.contains_retunes = 1;
        *handled = 1;
    }
    return rc;
}

static int
metadata_parse_field_group_b(metadata_parse_state* st, const char* key, const json_token* val_tok, int* handled,
                             char* err_buf, size_t err_buf_size) {
    int rc = metadata_parse_field_group_b1(st, key, val_tok, handled, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK || *handled) {
        return rc;
    }
    return metadata_parse_field_group_b2(st, key, val_tok, handled, err_buf, err_buf_size);
}

static int
metadata_parse_field_group_c1(metadata_parse_state* st, const char* key, const json_token* val_tok, int* handled,
                              char* err_buf, size_t err_buf_size) {
    int rc = DSD_IQ_OK;
    if (strcmp(key, "capture_retune_count") == 0) {
        rc = metadata_expect_integer(val_tok, "capture_retune_count", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u32(val_tok, &st->cfg.capture_retune_count, err_buf, err_buf_size, "capture_retune_count");
            st->seen.capture_retune_count = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "source_backend") == 0) {
        rc = metadata_expect_string(val_tok, "source_backend", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = copy_token_to_buffer(val_tok, st->cfg.source_backend, sizeof(st->cfg.source_backend), err_buf,
                                      err_buf_size, "source_backend", 1);
            st->seen.source_backend = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "source_args") == 0) {
        rc = metadata_expect_string(val_tok, "source_args", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = copy_token_to_buffer(val_tok, st->cfg.source_args, sizeof(st->cfg.source_args), err_buf, err_buf_size,
                                      "source_args", 1);
            st->seen.source_args = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "capture_started_utc") == 0) {
        rc = metadata_expect_string(val_tok, "capture_started_utc", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = copy_token_to_buffer(val_tok, st->cfg.capture_started_utc, sizeof(st->cfg.capture_started_utc),
                                      err_buf, err_buf_size, "capture_started_utc", 1);
            st->seen.capture_started_utc = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "data_file") == 0) {
        rc = metadata_expect_string(val_tok, "data_file", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = copy_token_to_buffer(val_tok, st->data_file_buf, sizeof(st->data_file_buf), err_buf, err_buf_size,
                                      "data_file", 1);
            st->seen.data_file = 1;
        }
        *handled = 1;
    }
    return rc;
}

static int
metadata_parse_field_group_c2(metadata_parse_state* st, const char* key, const json_token* val_tok, int* handled,
                              char* err_buf, size_t err_buf_size) {
    int rc = DSD_IQ_OK;
    if (strcmp(key, "data_bytes") == 0) {
        rc = metadata_expect_integer(val_tok, "data_bytes", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u64(val_tok, &st->cfg.data_bytes, err_buf, err_buf_size, "data_bytes");
            st->seen.data_bytes = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "capture_drops") == 0) {
        rc = metadata_expect_integer(val_tok, "capture_drops", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u64(val_tok, &st->cfg.capture_drops, err_buf, err_buf_size, "capture_drops");
            st->seen.capture_drops = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "capture_drop_blocks") == 0) {
        rc = metadata_expect_integer(val_tok, "capture_drop_blocks", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u64(val_tok, &st->cfg.capture_drop_blocks, err_buf, err_buf_size, "capture_drop_blocks");
            st->seen.capture_drop_blocks = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "input_ring_drops") == 0) {
        rc = metadata_expect_integer(val_tok, "input_ring_drops", err_buf, err_buf_size);
        if (rc == DSD_IQ_OK) {
            rc = token_to_u64(val_tok, &st->cfg.input_ring_drops, err_buf, err_buf_size, "input_ring_drops");
            st->seen.input_ring_drops = 1;
        }
        *handled = 1;
    } else if (strcmp(key, "size_limit_reached") == 0) {
        /* Optional: metadata written before this field existed is still valid,
         * so it is deliberately absent from the required-field list. */
        rc = token_to_bool(val_tok, &st->cfg.size_limit_reached, err_buf, err_buf_size, "size_limit_reached");
        *handled = 1;
    }
    return rc;
}

static int
metadata_parse_field_group_c3(metadata_parse_state* st, json_tokenizer* tk, const char* key, const json_token* val_tok,
                              int* handled, char* err_buf, size_t err_buf_size) {
    int rc = DSD_IQ_OK;
    if (strcmp(key, "notes") == 0) {
        if (val_tok->type == JTOK_NULL) {
            st->cfg.notes[0] = '\0';
            rc = DSD_IQ_OK;
        } else if (val_tok->type == JTOK_STRING) {
            rc = copy_token_to_buffer(val_tok, st->cfg.notes, sizeof(st->cfg.notes), err_buf, err_buf_size, "notes", 0);
        } else {
            set_error(err_buf, err_buf_size, "field 'notes' expects string or null");
            rc = DSD_IQ_ERR_INVALID_META;
        }
        st->seen.notes = 1;
        *handled = 1;
    } else if (strcmp(key, "events") == 0) {
        if (val_tok->type != JTOK_LBRACKET) {
            set_error(err_buf, err_buf_size, "field 'events' expects array");
            return DSD_IQ_ERR_INVALID_META;
        }
        if (st->cfg.events) {
            free(st->cfg.events);
            st->cfg.events = NULL;
            st->cfg.event_count = 0;
        }
        rc = parse_events_array(tk, &st->cfg.events, &st->cfg.event_count, err_buf, err_buf_size);
        st->seen.events = 1;
        *handled = 1;
    }
    return rc;
}

static int
metadata_parse_field_group_c(metadata_parse_state* st, json_tokenizer* tk, const char* key, const json_token* val_tok,
                             int* handled, char* err_buf, size_t err_buf_size) {
    int rc = metadata_parse_field_group_c1(st, key, val_tok, handled, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK || *handled) {
        return rc;
    }
    rc = metadata_parse_field_group_c2(st, key, val_tok, handled, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK || *handled) {
        return rc;
    }
    return metadata_parse_field_group_c3(st, tk, key, val_tok, handled, err_buf, err_buf_size);
}

static int
metadata_parse_key_value(json_tokenizer* tk, char* key_buf, size_t key_buf_size, json_token* out_val_tok, int* out_done,
                         char* err_buf, size_t err_buf_size) {
    json_token key_tok = tokenizer_next(tk);
    if (tk->err_msg) {
        set_error(err_buf, err_buf_size, "%s at byte %zu", tk->err_msg, tk->err_pos);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (key_tok.type == JTOK_RBRACE) {
        *out_done = 1;
        return DSD_IQ_OK;
    }
    if (key_tok.type != JTOK_STRING) {
        set_error(err_buf, err_buf_size, "expected string key at byte %zu", key_tok.offset);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (key_tok.str_len + 1 > key_buf_size) {
        set_error(err_buf, err_buf_size, "metadata key too long at byte %zu", key_tok.offset);
        return DSD_IQ_ERR_INVALID_META;
    }
    DSD_MEMCPY(key_buf, key_tok.str, key_tok.str_len);
    key_buf[key_tok.str_len] = '\0';

    json_token colon_tok = tokenizer_next(tk);
    if (colon_tok.type != JTOK_COLON) {
        set_error(err_buf, err_buf_size, "expected ':' after key at byte %zu", key_tok.offset);
        return DSD_IQ_ERR_INVALID_META;
    }

    *out_val_tok = tokenizer_next(tk);
    if (tk->err_msg) {
        set_error(err_buf, err_buf_size, "%s at byte %zu", tk->err_msg, tk->err_pos);
        return DSD_IQ_ERR_INVALID_META;
    }
    *out_done = 0;
    return DSD_IQ_OK;
}

static int
metadata_parse_field_value(metadata_parse_state* st, json_tokenizer* tk, const char* key, const json_token* val_tok,
                           char* err_buf, size_t err_buf_size) {
    int handled = 0;
    int rc = metadata_parse_field_group_a(st, key, val_tok, &handled, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK || handled) {
        return rc;
    }
    rc = metadata_parse_field_group_b(st, key, val_tok, &handled, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK || handled) {
        return rc;
    }
    rc = metadata_parse_field_group_c(st, tk, key, val_tok, &handled, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK || handled) {
        return rc;
    }
    if (val_tok->type == JTOK_LBRACE || val_tok->type == JTOK_LBRACKET) {
        set_error(err_buf, err_buf_size, "nested structures are unsupported in metadata (at byte %zu)",
                  val_tok->offset);
        return DSD_IQ_ERR_INVALID_META;
    }
    return DSD_IQ_OK;
}

static int
metadata_parse_field_delim(json_tokenizer* tk, const char* key, int* out_done, char* err_buf, size_t err_buf_size) {
    json_token delim_tok = tokenizer_next(tk);
    if (delim_tok.type == JTOK_COMMA) {
        *out_done = 0;
        return DSD_IQ_OK;
    }
    if (delim_tok.type == JTOK_RBRACE) {
        *out_done = 1;
        return DSD_IQ_OK;
    }
    if (tk->err_msg) {
        set_error(err_buf, err_buf_size, "%s at byte %zu", tk->err_msg, tk->err_pos);
    } else {
        set_error(err_buf, err_buf_size, "expected ',' or '}' after field '%s' at byte %zu", key, delim_tok.offset);
    }
    return DSD_IQ_ERR_INVALID_META;
}

static int
metadata_parse_object_fields(json_tokenizer* tk, metadata_parse_state* st, char* err_buf, size_t err_buf_size) {
    for (;;) {
        char key_buf[128];
        json_token val_tok;
        int done = 0;
        int rc = metadata_parse_key_value(tk, key_buf, sizeof(key_buf), &val_tok, &done, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
        if (done) {
            return DSD_IQ_OK;
        }
        rc = metadata_parse_field_value(st, tk, key_buf, &val_tok, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
        rc = metadata_parse_field_delim(tk, key_buf, &done, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
        if (done) {
            return DSD_IQ_OK;
        }
    }
}

static int
metadata_require_required_fields(const replay_seen_fields* seen, char* err_buf, size_t err_buf_size) {
    struct {
        unsigned value;
        const char* name;
    } required[] = {
        {seen->format, "format"},
        {seen->version, "version"},
        {seen->sample_format, "sample_format"},
        {seen->iq_order, "iq_order"},
        {seen->endianness, "endianness"},
        {seen->capture_stage, "capture_stage"},
        {seen->sample_rate_hz, "sample_rate_hz"},
        {seen->center_frequency_hz, "center_frequency_hz"},
        {seen->capture_center_frequency_hz, "capture_center_frequency_hz"},
        {seen->ppm, "ppm"},
        {seen->tuner_gain_tenth_db, "tuner_gain_tenth_db"},
        {seen->rtl_dsp_bw_khz, "rtl_dsp_bw_khz"},
        {seen->base_decimation, "base_decimation"},
        {seen->post_downsample, "post_downsample"},
        {seen->demod_rate_hz, "demod_rate_hz"},
        {seen->offset_tuning_enabled, "offset_tuning_enabled"},
        {seen->fs4_shift_enabled, "fs4_shift_enabled"},
        {seen->combine_rotate_enabled, "combine_rotate_enabled"},
        {seen->muted_bytes_excluded, "muted_bytes_excluded"},
        {seen->contains_retunes, "contains_retunes"},
        {seen->capture_retune_count, "capture_retune_count"},
        {seen->source_backend, "source_backend"},
        {seen->source_args, "source_args"},
        {seen->capture_started_utc, "capture_started_utc"},
        {seen->data_file, "data_file"},
        {seen->data_bytes, "data_bytes"},
        {seen->capture_drops, "capture_drops"},
        {seen->capture_drop_blocks, "capture_drop_blocks"},
        {seen->input_ring_drops, "input_ring_drops"},
        {seen->notes, "notes"},
    };

    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        int rc = require_field(required[i].value, required[i].name, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
    }
    return DSD_IQ_OK;
}

static int
metadata_apply_sample_format(metadata_parse_state* st, char* err_buf, size_t err_buf_size) {
    if (strcmp(st->sample_format_buf, "cu8") == 0) {
        st->cfg.format = DSD_IQ_FORMAT_CU8;
        if (strcmp(st->endianness_buf, "none") != 0) {
            set_error(err_buf, err_buf_size, "cu8 requires endianness 'none'");
            return DSD_IQ_ERR_INVALID_META;
        }
        return DSD_IQ_OK;
    }
    if (strcmp(st->sample_format_buf, "cf32") == 0) {
        st->cfg.format = DSD_IQ_FORMAT_CF32;
        if (strcmp(st->endianness_buf, "little") != 0) {
            set_error(err_buf, err_buf_size, "cf32 requires endianness 'little'");
            return DSD_IQ_ERR_INVALID_META;
        }
        return DSD_IQ_OK;
    }
    if (strcmp(st->sample_format_buf, "cs16") == 0) {
        st->cfg.format = DSD_IQ_FORMAT_CS16;
        if (strcmp(st->endianness_buf, "little") != 0) {
            set_error(err_buf, err_buf_size, "cs16 requires endianness 'little'");
            return DSD_IQ_ERR_INVALID_META;
        }
        return DSD_IQ_OK;
    }
    set_error(err_buf, err_buf_size, "unsupported sample_format '%s'", st->sample_format_buf);
    return DSD_IQ_ERR_UNSUPPORTED_FMT;
}

static int
metadata_finalize(metadata_parse_state* st, const char* metadata_path, int reject_retunes, char* err_buf,
                  size_t err_buf_size) {
    if (strcmp(st->format_buf, "dsd-neo-iq") != 0) {
        set_error(err_buf, err_buf_size, "unsupported format '%s'", st->format_buf);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (st->version != 1 && st->version != 2) {
        set_error(err_buf, err_buf_size, "unsupported metadata version %d", st->version);
        return DSD_IQ_ERR_UNSUPPORTED_VER;
    }
    st->cfg.metadata_version = (uint32_t)st->version;
    if (st->seen.events && st->version != 2) {
        set_error(err_buf, err_buf_size, "events require metadata version 2");
        return DSD_IQ_ERR_UNSUPPORTED_VER;
    }
    if (strcmp(st->iq_order_buf, "IQ") != 0) {
        set_error(err_buf, err_buf_size, "unsupported iq_order '%s'", st->iq_order_buf);
        return DSD_IQ_ERR_INVALID_META;
    }

    int rc = metadata_apply_sample_format(st, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    rc = resolve_data_path(metadata_path, st->data_file_buf, st->cfg.data_path, sizeof(st->cfg.data_path), err_buf,
                           err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    rc = copy_string_checked(metadata_path, st->cfg.metadata_path, sizeof(st->cfg.metadata_path), err_buf, err_buf_size,
                             "metadata_path");
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    rc = validate_replay_semantics(&st->cfg, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    return validate_replay_events_metadata(&st->cfg, st->cfg.data_bytes, st->cfg.data_bytes > 0, reject_retunes,
                                           err_buf, err_buf_size);
}

static int
parse_metadata_json(const char* metadata_path, const char* json, size_t json_len, dsd_iq_replay_config* out_cfg,
                    int reject_retunes, char* err_buf, size_t err_buf_size) {
    if (!metadata_path || !json || !out_cfg) {
        return DSD_IQ_ERR_INVALID_ARG;
    }

    json_tokenizer tk;
    DSD_MEMSET(&tk, 0, sizeof(tk));
    tk.src = json;
    tk.src_len = json_len;

    metadata_parse_state st;
    DSD_MEMSET(&st, 0, sizeof(st));

    int rc = DSD_IQ_OK;
    json_token tok = tokenizer_next(&tk);
    if (tok.type != JTOK_LBRACE) {
        if (tk.err_msg) {
            set_error(err_buf, err_buf_size, "%s at byte %zu", tk.err_msg, tk.err_pos);
        } else {
            set_error(err_buf, err_buf_size, "metadata must start with '{'");
        }
        rc = DSD_IQ_ERR_INVALID_META;
        goto fail;
    }

    rc = metadata_parse_object_fields(&tk, &st, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        goto fail;
    }

    tok = tokenizer_next(&tk);
    if (tok.type != JTOK_EOF) {
        set_error(err_buf, err_buf_size, "trailing JSON content at byte %zu", tok.offset);
        rc = DSD_IQ_ERR_INVALID_META;
        goto fail;
    }

    rc = metadata_require_required_fields(&st.seen, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        goto fail;
    }
    rc = metadata_finalize(&st, metadata_path, reject_retunes, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        goto fail;
    }
    /* Before "tuner_gain_known" existed, a capture whose tuner reported no gain recorded the read's error code as the
       gain. An Airspy never reports one, so its legacy captures carry no gain. */
    if (!st.seen.tuner_gain_known && strcmp(st.cfg.source_backend, "airspy") == 0) {
        st.cfg.tuner_gain_unknown = 1;
        st.cfg.tuner_gain_tenth_db = 0;
    }

    st.cfg.recorded_dsp_bw_khz = st.cfg.rtl_dsp_bw_khz;
    *out_cfg = st.cfg;
    return DSD_IQ_OK;

fail:
    dsd_iq_replay_config_clear(&st.cfg);
    return rc;
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
dsd_iq_replay_measure(const dsd_iq_replay_config* cfg, uint64_t file_size, uint64_t* out_effective, int* out_mismatch) {
    if (!cfg || !out_effective) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (cfg->container != DSD_IQ_REPLAY_CONTAINER_WAV) {
        return dsd_iq_replay_compute_effective_bytes(cfg->data_bytes, file_size, cfg->format, out_effective,
                                                     out_mismatch);
    }
    uint64_t frames = 0;
    int truncated = 0;
    int rc = dsd_iq_wav_frames_in_file(&cfg->wav, file_size, &frames, &truncated);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    if (frames > UINT64_MAX / 8U) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    *out_effective = frames * 8U;
    if (out_mismatch) {
        *out_mismatch = truncated;
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

size_t
dsd_iq_replay_chunk_bytes(dsd_iq_sample_format format, uint32_t sample_rate_hz) {
    const size_t align = dsd_iq_sample_format_alignment_bytes(format);
    if (align == 0U || sample_rate_hz == 0U) {
        return 0U;
    }
    uint64_t samples =
        ((uint64_t)sample_rate_hz * DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES) / DSD_IQ_REPLAY_CHUNK_REFERENCE_RATE_HZ;
    if (samples > DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES) {
        samples = DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES;
    } else if (samples == 0U) {
        samples = 1U;
    }
    return (size_t)samples * align;
}

size_t
dsd_iq_replay_chunk_bytes_for(dsd_iq_sample_format format, uint32_t capture_hz, uint32_t dsp_in_hz) {
    if (capture_hz == dsp_in_hz) {
        return dsd_iq_replay_chunk_bytes(format, capture_hz);
    }
    const size_t align = dsd_iq_sample_format_alignment_bytes(format);
    if (align == 0U || capture_hz == 0U || dsp_in_hz == 0U) {
        return 0U;
    }
    uint64_t samples = DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES;
    const uint64_t span =
        ((uint64_t)capture_hz * DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES) / DSD_IQ_REPLAY_CHUNK_REFERENCE_RATE_HZ;
    const uint64_t resampled = ((uint64_t)(DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES - 1U) * capture_hz) / dsp_in_hz;
    if (span < samples) {
        samples = span;
    }
    if (resampled < samples) {
        samples = resampled;
    }
    if (samples == 0U) {
        samples = 1U;
    }
    return (size_t)samples * align;
}

uint32_t
dsd_iq_replay_dsp_input_rate_hz(const dsd_iq_replay_config* cfg) {
    if (!cfg) {
        return 0U;
    }
    return cfg->dsp_input_rate_hz != 0U ? cfg->dsp_input_rate_hz : cfg->sample_rate_hz;
}

static int
is_power_of_two_u32(uint32_t v) {
    return v != 0U && (v & (v - 1U)) == 0U;
}

static void
rate_plan_fill(dsd_iq_replay_rate_plan* plan, uint32_t capture_hz, uint32_t dsp_in_hz, uint32_t base, uint32_t post,
               uint32_t target_hz, int kept) {
    DSD_MEMSET(plan, 0, sizeof(*plan));
    plan->capture_rate_hz = capture_hz;
    plan->dsp_input_rate_hz = dsp_in_hz;
    plan->base_decimation = base;
    plan->post_downsample = post;
    plan->demod_rate_hz = dsp_in_hz / base / post;
    plan->target_dsp_hz = kept ? 0U : target_hz;
    plan->kept_recorded = kept ? 1 : 0;
    plan->resample = dsp_in_hz != capture_hz ? 1 : 0;
}

/* KEEP (dsd_iq_replay_plan_rate()): 1 when the sidecar's recorded chain runs as captured, with @p plan filled. A TETRA
 * chain that misses the requirement only through a power-of-two post_downsample is folded into the half-band chain. */
static int
rate_plan_keep(const dsd_iq_replay_config* cfg, uint32_t required_hz, dsd_iq_replay_rate_plan* plan) {
    const uint32_t fs = cfg->sample_rate_hz;
    const uint32_t base = cfg->base_decimation;
    const uint32_t post = cfg->post_downsample;
    if (!is_power_of_two_u32(base) || base > DSD_IQ_REPLAY_MAX_BASE_DECIMATION || post == 0U || cfg->demod_rate_hz == 0U
        || fs / base / post != cfg->demod_rate_hz) {
        return 0;
    }
    /* The CQPSK path runs at fs / base: it skips post_downsample. */
    if (required_hz == 0U || (post == 1U && ((fs / base) % required_hz) == 0U)) {
        rate_plan_fill(plan, fs, fs, base, post, 0U, 1);
        return 1;
    }
    const uint64_t folded = (uint64_t)base * post;
    if (post > 1U && is_power_of_two_u32(post) && folded <= DSD_IQ_REPLAY_MAX_BASE_DECIMATION
        && ((fs / (uint32_t)folded) % required_hz) == 0U) {
        rate_plan_fill(plan, fs, fs, (uint32_t)folded, 1U, (uint32_t)(fs / folded), 0);
        return 1;
    }
    return 0;
}

int
dsd_iq_replay_plan_rate(const dsd_iq_replay_config* cfg, uint32_t target_dsp_hz, uint32_t cqpsk_required_multiple_hz,
                        int explicit_target, dsd_iq_replay_rate_plan* out_plan, char* err_buf, size_t err_buf_size) {
    if (!cfg || !out_plan) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    const uint32_t fs = cfg->sample_rate_hz;
    dsd_iq_replay_rate_plan plan;
    if (!explicit_target && cfg->container == DSD_IQ_REPLAY_CONTAINER_SIDECAR
        && rate_plan_keep(cfg, cqpsk_required_multiple_hz, &plan)) {
        *out_plan = plan;
        return DSD_IQ_OK;
    }
    const uint32_t d = target_dsp_hz;
    if (d == 0U) {
        set_error(err_buf, err_buf_size, "no DSP target rate to plan the replay for");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (fs < DSD_IQ_REPLAY_MIN_CAPTURE_RATE_HZ) {
        set_error(err_buf, err_buf_size, "the capture's rate (%u Hz) is below the %u Hz a replay can resample",
                  (unsigned)fs, (unsigned)DSD_IQ_REPLAY_MIN_CAPTURE_RATE_HZ);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    for (uint32_t k = 0U; k <= DSD_IQ_REPLAY_MAX_PLAN_PASSES; k++) {
        if ((uint64_t)d << k == (uint64_t)fs) {
            rate_plan_fill(&plan, fs, fs, 1U << k, 1U, d, 0);
            *out_plan = plan;
            return DSD_IQ_OK;
        }
    }
    uint64_t r = d;
    uint32_t k = 0U;
    if (fs < d) {
        if ((uint64_t)d > (uint64_t)fs * DSD_IQ_REPLAY_MAX_UPSAMPLE_RATIO) {
            set_error(err_buf, err_buf_size,
                      "the capture's rate (%u Hz) would need more than a %ux upsample to reach the %u Hz DSP rate",
                      (unsigned)fs, (unsigned)DSD_IQ_REPLAY_MAX_UPSAMPLE_RATIO, (unsigned)d);
            return DSD_IQ_ERR_RATE_CHAIN;
        }
    } else {
        while (k < DSD_IQ_REPLAY_MAX_PLAN_PASSES && ((uint64_t)d << (k + 1U)) <= (uint64_t)fs) {
            k++;
        }
        r = (uint64_t)d << k;
        if ((uint64_t)fs >= 2U * r) {
            set_error(err_buf, err_buf_size,
                      "the capture's rate (%u Hz) is too fast to resample to the %u Hz DSP rate (more than a %u x 2 "
                      "decimation)",
                      (unsigned)fs, (unsigned)d, (unsigned)(1U << DSD_IQ_REPLAY_MAX_PLAN_PASSES));
            return DSD_IQ_ERR_RATE_CHAIN;
        }
    }
    rate_plan_fill(&plan, fs, (uint32_t)r, 1U << k, 1U, d, 0);
    *out_plan = plan;
    return DSD_IQ_OK;
}

int
dsd_iq_replay_apply_rate_plan(dsd_iq_replay_config* cfg, const dsd_iq_replay_rate_plan* plan) {
    if (!cfg || !plan || plan->capture_rate_hz == 0U || plan->capture_rate_hz != cfg->sample_rate_hz
        || plan->dsp_input_rate_hz == 0U || plan->base_decimation == 0U || plan->post_downsample == 0U
        || plan->demod_rate_hz == 0U) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    cfg->dsp_input_rate_hz = plan->dsp_input_rate_hz;
    cfg->base_decimation = plan->base_decimation;
    cfg->post_downsample = plan->post_downsample;
    cfg->demod_rate_hz = plan->demod_rate_hz;
    return DSD_IQ_OK;
}

/* Whether checkpoint @p ckpt fits the replay @p cfg: an fs/4 phase of 0 to 3, and an event index that falls where its
   data offset does among the capture's events. */
static int
resume_checkpoint_fits(const dsd_iq_replay_config* cfg, const dsd_iq_replay_checkpoint* ckpt, char* err_buf,
                       size_t err_buf_size) {
    if (ckpt->fs4_phase < 0 || ckpt->fs4_phase > 3) {
        set_error(err_buf, err_buf_size, "the checkpoint's fs/4 phase %d is not 0 to 3", (int)ckpt->fs4_phase);
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (ckpt->event_index > cfg->event_count || (cfg->event_count > 0U && !cfg->events)) {
        set_error(err_buf, err_buf_size, "the checkpoint's event %u is past the capture's %u events",
                  (unsigned)ckpt->event_index, (unsigned)cfg->event_count);
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if ((ckpt->event_index > 0U && cfg->events[ckpt->event_index - 1U].byte_offset > ckpt->data_offset)
        || (ckpt->event_index < cfg->event_count && cfg->events[ckpt->event_index].byte_offset < ckpt->data_offset)) {
        set_error(err_buf, err_buf_size, "the checkpoint at byte %" PRIu64 " does not fall before event %u",
                  ckpt->data_offset, (unsigned)ckpt->event_index);
        return DSD_IQ_ERR_INVALID_ARG;
    }
    return DSD_IQ_OK;
}

int
dsd_iq_replay_resume_check(const dsd_iq_replay_config* cfg, const dsd_iq_replay_checkpoint* ckpt, uint64_t* out_center,
                           uint64_t* out_capture_center, char* err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size > 0U) {
        err_buf[0] = '\0';
    }
    if (!cfg || !ckpt) {
        set_error(err_buf, err_buf_size, "no replay or checkpoint to resume");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    const int fits = resume_checkpoint_fits(cfg, ckpt, err_buf, err_buf_size);
    if (fits != DSD_IQ_OK) {
        return fits;
    }
    uint64_t center = cfg->center_frequency_hz;
    uint64_t capture_center = cfg->capture_center_frequency_hz;
    for (uint32_t i = ckpt->event_index; i > 0U; i--) {
        const dsd_iq_event* ev = &cfg->events[i - 1U];
        if (ev->kind == DSD_IQ_EVENT_RETUNE) {
            center = ev->center_frequency_hz;
            capture_center = ev->capture_center_frequency_hz;
            break;
        }
    }
    if (out_center) {
        *out_center = center;
    }
    if (out_capture_center) {
        *out_capture_center = capture_center;
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

#ifdef DSD_NEO_TEST_HOOKS
static int g_test_sniff_read_fail_after = -1;

void
dsd_iq_replay_test_fail_sniff_read(int after) {
    g_test_sniff_read_fail_after = after;
}
#endif

/* What replay_sniff_file() found at a path. */
typedef struct {
    int sniff;       /* a dsd_iq_wav_sniff_result, or -1 when the first bytes were not read */
    int open_errno;  /* why the path could not be opened as a regular file, or 0 */
    int read_failed; /* the read of the first bytes failed: an I/O error, not a file too short to hold a header */
    int read_errno;  /* that read's errno, when read_failed */
} replay_sniff;

/* Read up to @p len of a file's first bytes; *@p out_failed set, with *@p out_errno, when the read fails rather than
 * reaching the file's end. */
static size_t
replay_sniff_read(uint8_t* head, size_t len, FILE* fp, int* out_failed, int* out_errno) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_test_sniff_read_fail_after == 0) {
        g_test_sniff_read_fail_after = -1;
        *out_failed = 1;
        *out_errno = EIO;
        return 0U;
    }
    if (g_test_sniff_read_fail_after > 0) {
        g_test_sniff_read_fail_after--;
    }
#endif
    errno = 0;
    const size_t n = fread(head, 1, len, fp);
    if (n < len && ferror(fp)) {
        *out_failed = 1;
        *out_errno = errno != 0 ? errno : EIO;
    }
    return n;
}

/* What @p path's first bytes are, or why they could not be read. */
static replay_sniff
replay_sniff_file(const char* path) {
    replay_sniff out = {-1, 0, 0, 0};
    errno = 0;
    FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
    if (!fp) {
        out.open_errno = errno != 0 ? errno : EINVAL;
        return out;
    }
    uint8_t head[DSD_IQ_WAV_SNIFF_BYTES];
    const size_t n = replay_sniff_read(head, sizeof(head), fp, &out.read_failed, &out.read_errno);
    fclose(fp);
    if (!out.read_failed) {
        out.sniff = dsd_iq_wav_sniff(head, n);
    }
    return out;
}

/* Whether @p err is how an open that does not follow a final symbolic link refuses one. */
static int
replay_errno_is_symlink(int err) {
#ifdef ELOOP
    if (err == ELOOP) {
        return 1;
    }
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
    if (err == EMLINK) { /* their O_NOFOLLOW refusal */
        return 1;
    }
#endif
    return 0;
}

/* Why @p path, which replay could not open with @p err, is refused. */
static int
replay_refuse_open(const char* path, int err, char* err_buf, size_t err_buf_size) {
    if (replay_errno_is_symlink(err)) {
        set_error(err_buf, err_buf_size,
                  "'%s' is a symbolic link, or a path the system would not follow; replay opens regular files only",
                  path);
    } else if (err == EINVAL) {
        /* dsd_fopen_existing_regular_file()'s refusal of a directory, FIFO, device or (on Windows) reparse point. */
        set_error(err_buf, err_buf_size, "'%s' is not a regular file; replay opens regular files only", path);
    } else {
        set_error(err_buf, err_buf_size, "cannot open '%s': %s", path, strerror(err));
    }
    return DSD_IQ_ERR_IO;
}

static int
replay_refuse_unreadable(const char* path, const replay_sniff* sniff, char* err_buf, size_t err_buf_size) {
    set_error(err_buf, err_buf_size, "failed to read '%s': %s", path, strerror(sniff->read_errno));
    return DSD_IQ_ERR_IO;
}

/* Whether @p type is a whole JSON value on its own: a string, number, true, false or null. */
static int
json_token_is_scalar(json_token_type type) {
    switch (type) {
        case JTOK_STRING:
        case JTOK_NUMBER_INT:
        case JTOK_NUMBER_FLOAT:
        case JTOK_TRUE:
        case JTOK_FALSE:
        case JTOK_NULL: return 1;
        default: return 0;
    }
}

/* Skip one JSON value whose first token is @p tok. 0, or -1 for malformed JSON. */
static int
prepass_skip_value(json_tokenizer* tk, json_token tok) {
    if (tok.type != JTOK_LBRACE && tok.type != JTOK_LBRACKET) {
        return json_token_is_scalar(tok.type) ? 0 : -1;
    }
    unsigned depth = 1;
    while (depth > 0U) {
        tok = tokenizer_next(tk);
        if (tok.type == JTOK_ERROR || tok.type == JTOK_EOF) {
            return -1;
        }
        if (tok.type == JTOK_LBRACE || tok.type == JTOK_LBRACKET) {
            depth++;
        } else if (tok.type == JTOK_RBRACE || tok.type == JTOK_RBRACKET) {
            depth--;
        }
    }
    return 0;
}

/* The top-level data_file of a sidecar, read before anything else in it is validated. 1 when found. */
static int
prepass_data_file(const char* json, size_t json_len, char* out, size_t out_size) {
    json_tokenizer tk;
    DSD_MEMSET(&tk, 0, sizeof(tk));
    tk.src = json;
    tk.src_len = json_len;
    if (tokenizer_next(&tk).type != JTOK_LBRACE) {
        return 0;
    }
    for (;;) {
        json_token key = tokenizer_next(&tk);
        if (key.type != JTOK_STRING) {
            return 0;
        }
        const int is_data_file = key.str_len == 9U && memcmp(key.str, "data_file", 9U) == 0;
        if (tokenizer_next(&tk).type != JTOK_COLON) {
            return 0;
        }
        json_token val = tokenizer_next(&tk);
        if (is_data_file) {
            return val.type == JTOK_STRING && copy_token_to_buffer(&val, out, out_size, NULL, 0, "data_file", 1) == 0
                       ? 1
                       : 0;
        }
        if (prepass_skip_value(&tk, val) != 0) {
            return 0;
        }
        json_token delim = tokenizer_next(&tk);
        if (delim.type != JTOK_COMMA) {
            return 0;
        }
    }
}

/* The replay config of the WAV recording at @p wav_path, opened by @p opened_path. @p ignored_sidecar names a sidecar
 * the WAV's header overrides, for the open notice, or is NULL. */
static int
replay_wav_config(const char* wav_path, const char* opened_path, const char* ignored_sidecar,
                  dsd_iq_replay_config* out_cfg, char* err_buf, size_t err_buf_size) {
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    int rc = dsd_iq_wav_read_info(wav_path, &cfg.wav, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    rc = copy_string_checked(wav_path, cfg.data_path, sizeof(cfg.data_path), err_buf, err_buf_size, "data path");
    if (rc == DSD_IQ_OK) {
        rc = copy_string_checked(opened_path, cfg.metadata_path, sizeof(cfg.metadata_path), err_buf, err_buf_size,
                                 "metadata path");
    }
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    cfg.container = DSD_IQ_REPLAY_CONTAINER_WAV;
    cfg.metadata_version = 1;
    cfg.format = DSD_IQ_FORMAT_CF32;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_driver_cf32_pre_ring");
    cfg.sample_rate_hz = cfg.wav.sample_rate_hz;
    cfg.center_frequency_hz = cfg.wav.center_frequency_hz;
    cfg.capture_center_frequency_hz = cfg.wav.center_frequency_hz;
    cfg.data_bytes = cfg.wav.frames * 8U;
    cfg.base_decimation = 1;
    cfg.post_downsample = 1;
    cfg.demod_rate_hz = cfg.wav.sample_rate_hz;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "wav");
    DSD_SNPRINTF(cfg.capture_started_utc, sizeof(cfg.capture_started_utc), "%s", cfg.wav.start_utc);
    if (ignored_sidecar) {
        DSD_SNPRINTF(cfg.open_notice, sizeof(cfg.open_notice),
                     "'%s' is a WAV file: its header gives the format and rate (2-channel %s, %u Hz), so the sidecar "
                     "'%s' is ignored",
                     wav_path, dsd_iq_wav_encoding_name(cfg.wav.encoding), cfg.wav.sample_rate_hz, ignored_sidecar);
    }
    rc = validate_replay_semantics(&cfg, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    *out_cfg = cfg;
    return DSD_IQ_OK;
}

static int
replay_refuse_rifx(const char* path, char* err_buf, size_t err_buf_size) {
    set_error(err_buf, err_buf_size, "WAV '%s': big-endian RIFX files are not supported", path);
    return DSD_IQ_ERR_UNSUPPORTED_FMT;
}

static int
replay_read_metadata_internal(const char* path, dsd_iq_replay_config* out_cfg, int reject_retunes, char* err_buf,
                              size_t err_buf_size) {
    if (!path || !out_cfg) {
        return DSD_IQ_ERR_INVALID_ARG;
    }

    /* A WAV recording describes itself (issue #701): it is recognised before any sidecar is looked for, and a sidecar
       beside it is only reported as ignored. */
    replay_sniff sniff = {-1, 0, 0, 0};
    if (!has_suffix(path, ".json")) {
        sniff = replay_sniff_file(path);
        if (sniff.read_failed) {
            return replay_refuse_unreadable(path, &sniff, err_buf, err_buf_size);
        }
        if (sniff.sniff == DSD_IQ_WAV_SNIFF_WAV) {
            char sidecar[2048];
            char sidecar_err[64];
            const int have_sidecar =
                resolve_metadata_path(path, sidecar, sizeof(sidecar), sidecar_err, sizeof(sidecar_err)) == DSD_IQ_OK;
            return replay_wav_config(path, path, have_sidecar ? sidecar : NULL, out_cfg, err_buf, err_buf_size);
        }
        if (sniff.sniff == DSD_IQ_WAV_SNIFF_RIFX) {
            return replay_refuse_rifx(path, err_buf, err_buf_size);
        }
    }

    /* A path that could not be opened may still name a capture through its sidecar (the bare name given to
       --iq-capture, say); only when there is none is the open's reason the answer. */
    char metadata_path[2048];
    int mrc = resolve_metadata_path(path, metadata_path, sizeof(metadata_path), err_buf, err_buf_size);
    if (mrc != DSD_IQ_OK) {
        if (mrc == DSD_IQ_ERR_IO && sniff.open_errno == ENOENT) {
            set_error(err_buf, err_buf_size,
                      "cannot open '%s': %s, and it has no sidecar (tried '%s.json' and '%s.iq.json')", path,
                      strerror(ENOENT), path, path);
            return DSD_IQ_ERR_IO;
        }
        if (mrc == DSD_IQ_ERR_IO && sniff.open_errno != 0) {
            return replay_refuse_open(path, sniff.open_errno, err_buf, err_buf_size);
        }
        if (sniff.sniff == DSD_IQ_WAV_SNIFF_NOT_WAV && has_suffix(path, ".wav")) {
            set_error(err_buf, err_buf_size,
                      "'%s' is not a WAV file (no RIFF, RF64 or BW64 WAVE header) and has no sidecar (tried '%s.json' "
                      "and '%s.iq.json')",
                      path, path, path);
            return DSD_IQ_ERR_INVALID_META;
        }
        return mrc;
    }

    char* json = NULL;
    size_t json_len = 0;
    int rrc = read_file_all(metadata_path, &json, &json_len, err_buf, err_buf_size);
    if (rrc != DSD_IQ_OK) {
        return rrc;
    }

    /* A sidecar whose data_file is a WAV: only that name is read, and the WAV's header describes the capture, however
       wrong the sidecar's other fields are. */
    char data_file[2048];
    char data_path[2048];
    if (prepass_data_file(json, json_len, data_file, sizeof(data_file))
        && resolve_data_path(metadata_path, data_file, data_path, sizeof(data_path), NULL, 0) == DSD_IQ_OK) {
        const replay_sniff data_sniff = replay_sniff_file(data_path);
        if (data_sniff.read_failed) {
            free(json);
            return replay_refuse_unreadable(data_path, &data_sniff, err_buf, err_buf_size);
        }
        /* A missing data file still leaves the sidecar readable (--iq-info); the replay's open reports it. Any other
           reason the data file cannot be opened is reported here, before the sidecar's other fields are judged. */
        if (data_sniff.open_errno != 0 && data_sniff.open_errno != ENOENT) {
            free(json);
            return replay_refuse_open(data_path, data_sniff.open_errno, err_buf, err_buf_size);
        }
        if (data_sniff.sniff == DSD_IQ_WAV_SNIFF_WAV) {
            free(json);
            return replay_wav_config(data_path, metadata_path, metadata_path, out_cfg, err_buf, err_buf_size);
        }
        if (data_sniff.sniff == DSD_IQ_WAV_SNIFF_RIFX) {
            free(json);
            return replay_refuse_rifx(data_path, err_buf, err_buf_size);
        }
    }

    int prc = parse_metadata_json(metadata_path, json, json_len, out_cfg, reject_retunes, err_buf, err_buf_size);
    free(json);
    return prc;
}

int
dsd_iq_replay_read_metadata(const char* path, dsd_iq_replay_config* out_cfg, char* err_buf, size_t err_buf_size) {
    if (!out_cfg) {
        return DSD_IQ_ERR_INVALID_ARG;
    }

    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    int rc = replay_read_metadata_internal(path, &cfg, 0, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }

    *out_cfg = cfg;
    return DSD_IQ_OK;
}

/* The WAV side of a source whose stream is open: scratch buffers and the reader at the first frame. */
static int
replay_source_open_wav(struct dsd_iq_replay_source* src, const dsd_iq_replay_config* cfg, uint64_t effective,
                       char* err_buf, size_t err_buf_size) {
    src->wav = 1;
    src->wav_encoding = cfg->wav.encoding;
    src->wav_block_align = cfg->wav.block_align;
    src->wav_data_offset = cfg->wav.data_offset;
    src->wav_total_frames = effective / 8U;
    src->wav_frames_left = src->wav_total_frames;
    if (src->wav_block_align == 0U || src->wav_block_align > sizeof(src->wav_raw_pending)) {
        set_error(err_buf, err_buf_size, "WAV '%s': unsupported frame size", cfg->data_path);
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    src->wav_raw = (uint8_t*)malloc(REPLAY_WAV_SCRATCH_BYTES);
    src->wav_cf32 = (float*)malloc((size_t)REPLAY_WAV_CF32_FRAMES * 2U * sizeof(float));
    if (!src->wav_raw || !src->wav_cf32) {
        set_error(err_buf, err_buf_size, "replay source allocation failed");
        return DSD_IQ_ERR_ALLOC;
    }
    if (src->wav_data_offset > (uint64_t)INT64_MAX
        || dsd_fseek64(src->fp, (int64_t)src->wav_data_offset, SEEK_SET) != 0) {
        set_error(err_buf, err_buf_size, "failed to seek to the data of '%s': %s", cfg->data_path, strerror(errno));
        return DSD_IQ_ERR_IO;
    }
    return DSD_IQ_OK;
}

/* Convert the next frames of a WAV into @p out, as dsd_iq_replay_read() describes. A short read hands over the whole
 * frames it got (the start of a frame it cut stays pending), and the read after it reports the failure. A read
 * smaller than one 8-byte cf32 frame keeps the rest of that frame for the next read. */
static int
replay_read_wav(dsd_iq_replay_source* src, uint8_t* out, size_t max_bytes, size_t* out_bytes) {
    size_t produced = 0;
    if (src->wav_out_pending_len > 0U) {
        const size_t n = max_bytes < src->wav_out_pending_len ? max_bytes : src->wav_out_pending_len;
        DSD_MEMCPY(out, src->wav_out_pending + src->wav_out_pending_off, n);
        src->wav_out_pending_off += n;
        src->wav_out_pending_len -= n;
        produced += n;
    }
    const size_t frames_per_pass_raw = REPLAY_WAV_SCRATCH_BYTES / src->wav_block_align;
    const size_t frames_per_pass =
        frames_per_pass_raw < REPLAY_WAV_CF32_FRAMES ? frames_per_pass_raw : REPLAY_WAV_CF32_FRAMES;
    while (produced < max_bytes && src->wav_frames_left > 0U) {
        const size_t room = max_bytes - produced;
        size_t want = room / 8U;
        const int split = want == 0U;
        if (split) {
            want = 1U;
        }
        if (want > frames_per_pass) {
            want = frames_per_pass;
        }
        if ((uint64_t)want > src->wav_frames_left) {
            want = (size_t)src->wav_frames_left;
        }
        const size_t need = want * src->wav_block_align;
        const size_t have = src->wav_raw_pending_len;
        DSD_MEMCPY(src->wav_raw, src->wav_raw_pending, have);
        src->wav_raw_pending_len = 0U;
        const size_t got = have + fread(src->wav_raw + have, 1, need - have, src->fp);
        const size_t complete = got / src->wav_block_align;
        const size_t rest = got % src->wav_block_align;
        if (rest > 0U) {
            DSD_MEMCPY(src->wav_raw_pending, src->wav_raw + complete * src->wav_block_align, rest);
            src->wav_raw_pending_len = rest;
        }
        if (complete > 0U) {
            dsd_iq_wav_frames_to_cf32(src->wav_encoding, src->wav_raw, complete, src->wav_cf32);
            src->wav_frames_left -= complete;
            if (split) {
                DSD_MEMCPY(src->wav_out_pending, src->wav_cf32, 8U);
                DSD_MEMCPY(out + produced, src->wav_out_pending, room);
                src->wav_out_pending_off = room;
                src->wav_out_pending_len = 8U - room;
                produced += room;
            } else {
                DSD_MEMCPY(out + produced, src->wav_cf32, complete * 8U);
                produced += complete * 8U;
            }
        }
        if (complete < want) {
            /* Frames the open measured are missing: a failed read, or a file cut short after the open. */
            if (produced == 0U) {
                return DSD_IQ_ERR_IO;
            }
            break;
        }
    }
    src->remaining_bytes -= (uint64_t)produced;
    *out_bytes = produced;
    return DSD_IQ_OK;
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
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    int rc = replay_read_metadata_internal(path, &cfg, 1, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    rc = validate_replay_convertible(&cfg, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        dsd_iq_replay_config_clear(&cfg);
        return rc;
    }

    uint64_t actual_size = 0;
    if (file_size_u64(cfg.data_path, &actual_size) != DSD_IQ_OK) {
        set_error(err_buf, err_buf_size, "failed to stat data file '%s': %s", cfg.data_path, strerror(errno));
        dsd_iq_replay_config_clear(&cfg);
        return DSD_IQ_ERR_IO;
    }

    uint64_t effective = 0;
    int mismatch = 0;
    rc = dsd_iq_replay_measure(&cfg, actual_size, &effective, &mismatch);
    if (rc != DSD_IQ_OK) {
        set_error(err_buf, err_buf_size, "failed to compute effective replay bytes");
        dsd_iq_replay_config_clear(&cfg);
        return rc;
    }
    rc = dsd_iq_replay_validate_effective_bytes_for_replay(effective, cfg.loop);
    if (rc != DSD_IQ_OK) {
        set_error(err_buf, err_buf_size, "no aligned I/Q samples available for replay");
        dsd_iq_replay_config_clear(&cfg);
        return rc;
    }
    rc = validate_replay_events_metadata(&cfg, effective, 1, 1, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        dsd_iq_replay_config_clear(&cfg);
        return rc;
    }

    (void)mismatch;

    if (!out) {
        *out_cfg = cfg;
        return DSD_IQ_OK;
    }

    struct dsd_iq_replay_source* src = (struct dsd_iq_replay_source*)calloc(1, sizeof(*src));
    if (!src) {
        set_error(err_buf, err_buf_size, "replay source allocation failed");
        dsd_iq_replay_config_clear(&cfg);
        return DSD_IQ_ERR_ALLOC;
    }
    errno = 0;
    src->fp = dsd_fopen_existing_regular_file(cfg.data_path, "rb");
    if (!src->fp) {
        (void)replay_refuse_open(cfg.data_path, errno != 0 ? errno : EINVAL, err_buf, err_buf_size);
        free(src);
        dsd_iq_replay_config_clear(&cfg);
        return DSD_IQ_ERR_IO;
    }
    /* Unbuffered, so each read sees the file as it is then: a stdio buffer can hold bytes read ahead of the request
       (musl fills its own on every read), and a file cut short after the open would then still hand over bytes it no
       longer has before the read error. Replay reads are whole transfer blocks (about 5 ms of capture each, at any
       rate), so this costs at most one read() per block. */
    (void)setvbuf(src->fp, NULL, _IONBF, 0);
    src->remaining_bytes = effective;
    src->total_bytes = effective;
    src->align = dsd_iq_sample_format_alignment_bytes(cfg.format);
    if (cfg.container == DSD_IQ_REPLAY_CONTAINER_WAV) {
        rc = replay_source_open_wav(src, &cfg, effective, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            dsd_iq_replay_close(src);
            dsd_iq_replay_config_clear(&cfg);
            return rc;
        }
    }
    *out_cfg = cfg;
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
    if (src->wav) {
        return replay_read_wav(src, (uint8_t*)out, max_bytes, out_bytes);
    }
    size_t n = fread(out, 1, max_bytes, src->fp);
    if (n == 0) {
        /* Capture bytes remain: the open measured them, and remaining_bytes counts what is left. A read that gets none
           failed (ferror()), or found the end of a file cut short after the open, which is a read error too, not the
           capture's end. A capture whose data_bytes is 0 counts the file's size at the open, so a healthy file never
           runs out before it. */
        return DSD_IQ_ERR_IO;
    }
    src->remaining_bytes -= (uint64_t)n;
    *out_bytes = n;
    return DSD_IQ_OK;
}

int
dsd_iq_replay_rewind(dsd_iq_replay_source* src) {
    return dsd_iq_replay_seek(src, 0U);
}

int
dsd_iq_replay_seek(dsd_iq_replay_source* src, uint64_t capture_byte_offset) {
    if (!src || !src->fp || src->align == 0U) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    const uint64_t aligned = capture_byte_offset - capture_byte_offset % (uint64_t)src->align;
    if (aligned > src->total_bytes) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    uint64_t file_offset = aligned;
    if (src->wav) {
        const uint64_t frame = aligned / 8U;
        if (frame > (UINT64_MAX - src->wav_data_offset) / (uint64_t)src->wav_block_align) {
            return DSD_IQ_ERR_INVALID_ARG;
        }
        file_offset = src->wav_data_offset + frame * (uint64_t)src->wav_block_align;
    }
    if (file_offset > (uint64_t)INT64_MAX || dsd_fseek64(src->fp, (int64_t)file_offset, SEEK_SET) != 0) {
        return DSD_IQ_ERR_IO;
    }
    src->remaining_bytes = src->total_bytes - aligned;
    if (src->wav) {
        src->wav_frames_left = src->wav_total_frames - aligned / 8U;
        src->wav_raw_pending_len = 0U;
        src->wav_out_pending_off = 0U;
        src->wav_out_pending_len = 0U;
    }
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
    free(src->wav_raw);
    free(src->wav_cf32);
    free(src);
}

static uint64_t
dsd_iq_info_raw_bytes(const dsd_iq_replay_config* cfg, uint64_t actual_file_size) {
    if (cfg->data_bytes == 0U) {
        return actual_file_size;
    }
    return (cfg->data_bytes < actual_file_size) ? cfg->data_bytes : actual_file_size;
}

static void
dsd_iq_info_print_summary(const dsd_iq_replay_config* cfg, const char* display_path, uint64_t actual_file_size,
                          uint64_t effective, int replay_compatible, FILE* out) {
    size_t align = dsd_iq_sample_format_alignment_bytes(cfg->format);
    double dur = dsd_iq_replay_estimate_duration_seconds(effective, cfg->format, cfg->sample_rate_hz);
    uint64_t complex_samples = (align > 0U) ? (effective / (uint64_t)align) : 0U;

    DSD_FPRINTF(out, "IQ Capture Info: %s\n", display_path ? display_path : cfg->metadata_path);
    DSD_FPRINTF(out, "  Format:              dsd-neo-iq v%u\n", cfg->metadata_version ? cfg->metadata_version : 1U);
    DSD_FPRINTF(out, "  Sample format:       %s\n", dsd_iq_sample_format_name(cfg->format));
    DSD_FPRINTF(out, "  Sample rate:         %u Hz\n", cfg->sample_rate_hz);
    DSD_FPRINTF(out, "  Center frequency:    %.6f MHz\n", (double)cfg->center_frequency_hz / 1000000.0);
    DSD_FPRINTF(out, "  Capture center:      %.6f MHz\n", (double)cfg->capture_center_frequency_hz / 1000000.0);
    DSD_FPRINTF(out, "  Demod rate:          %u Hz (base_decimation=%u, post_downsample=%u)\n", cfg->demod_rate_hz,
                cfg->base_decimation, cfg->post_downsample);
    DSD_FPRINTF(out, "  Source backend:      %s (%s)\n", cfg->source_backend,
                cfg->source_args[0] ? cfg->source_args : "none");
    DSD_FPRINTF(out, "  Capture stage:       %s\n", cfg->capture_stage);
    DSD_FPRINTF(out, "  FS/4 shift:          %s\n", cfg->fs4_shift_enabled ? "enabled" : "disabled");
    DSD_FPRINTF(out, "  CU8 transform:       %s\n", cfg->historical_cu8_two_pass ? "historical two-pass" : "combined");
    DSD_FPRINTF(out, "  Offset tuning:       %s\n", cfg->offset_tuning_enabled ? "enabled" : "disabled");
    if (cfg->tuner_gain_unknown) {
        DSD_FPRINTF(out, "  Tuner gain:          unknown\n");
    } else {
        DSD_FPRINTF(out, "  Tuner gain:          %.1f dB\n", (double)cfg->tuner_gain_tenth_db / 10.0);
    }
    DSD_FPRINTF(out, "  PPM correction:      %d\n", cfg->ppm);
    DSD_FPRINTF(out, "  DSP bandwidth:       %d kHz\n", cfg->rtl_dsp_bw_khz);
    DSD_FPRINTF(out, "  Data file:           %s\n", cfg->data_path);
    DSD_FPRINTF(out, "  Data bytes:          %" PRIu64 " (metadata), %" PRIu64 " (actual)\n", cfg->data_bytes,
                actual_file_size);
    DSD_FPRINTF(out, "  Duration:            ~%.2f s (%" PRIu64 " complex samples)\n", dur, complex_samples);
    DSD_FPRINTF(out, "  Capture drops:       %" PRIu64 " (%" PRIu64 " blocks)\n", cfg->capture_drops,
                cfg->capture_drop_blocks);
    DSD_FPRINTF(out, "  Input ring drops:    %" PRIu64 "\n", cfg->input_ring_drops);
    DSD_FPRINTF(out, "  Size limit reached:  %s\n", cfg->size_limit_reached ? "yes" : "no");
    DSD_FPRINTF(out, "  Contains retunes:    %s (%u retune events)\n", cfg->contains_retunes ? "yes" : "no",
                cfg->capture_retune_count);
    DSD_FPRINTF(out, "  Event timeline:      %u event(s)\n", cfg->event_count);
    DSD_FPRINTF(out, "  Replay compatible:   %s\n", replay_compatible ? "yes" : "no");
}

static void
dsd_iq_info_print_warnings(const dsd_iq_replay_config* cfg, uint64_t actual_file_size, uint64_t effective, int mismatch,
                           int replay_compatible, const char* compat_err, FILE* err) {
    if (!err) {
        return;
    }
    if (mismatch) {
        DSD_FPRINTF(err, "WARNING: metadata data_bytes (%" PRIu64 ") != actual file size (%" PRIu64 ")\n",
                    cfg->data_bytes, actual_file_size);
    }
    if (cfg->data_bytes == 0) {
        DSD_FPRINTF(err, "WARNING: metadata was never finalized (interrupted capture)\n");
    }
    uint64_t raw_bytes = dsd_iq_info_raw_bytes(cfg, actual_file_size);
    if (effective != raw_bytes) {
        size_t align = dsd_iq_sample_format_alignment_bytes(cfg->format);
        DSD_FPRINTF(err, "WARNING: data file size not aligned to sample boundary (%s requires %zu-byte alignment)\n",
                    dsd_iq_sample_format_name(cfg->format), align);
    }
    if (cfg->contains_retunes && cfg->event_count == 0U) {
        DSD_FPRINTF(err, "WARNING: capture contains %u retune(s) but has no replay event timeline\n",
                    cfg->capture_retune_count);
    } else if (!replay_compatible && compat_err[0] != '\0') {
        DSD_FPRINTF(err, "WARNING: replay compatibility check failed: %s\n", compat_err);
    }
}

/* --iq-info for a WAV recording (issue #701). */
/* How a WAV at its rate reaches @p target_hz: "resampled to R Hz, decimated by N", "decimated by N", "as recorded", or
 * why it cannot. */
static int
info_describe_wav_plan(const dsd_iq_replay_config* cfg, uint32_t target_hz, uint32_t required_hz, char* out,
                       size_t out_size) {
    dsd_iq_replay_rate_plan plan;
    char err[160] = {0};
    if (dsd_iq_replay_plan_rate(cfg, target_hz, required_hz, 0, &plan, err, sizeof(err)) != DSD_IQ_OK) {
        DSD_SNPRINTF(out, out_size, "refused (%s)", err);
        return 0;
    }
    if (plan.resample) {
        DSD_SNPRINTF(out, out_size, "resampled to %u Hz, decimated by %u", (unsigned)plan.dsp_input_rate_hz,
                     (unsigned)plan.base_decimation);
    } else if (plan.base_decimation > 1U) {
        DSD_SNPRINTF(out, out_size, "decimated by %u", (unsigned)plan.base_decimation);
    } else {
        DSD_SNPRINTF(out, out_size, "%s", "as recorded");
    }
    return 1;
}

/* The warnings dsd_iq_info_print_wav() writes to @p err about a WAV whose header and file disagree. */
static void
info_print_wav_warnings(const dsd_iq_replay_config* cfg, uint64_t actual_file_size, int truncated, FILE* err) {
    const dsd_iq_wav_info* w = &cfg->wav;
    if (truncated) {
        DSD_FPRINTF(err,
                    "WARNING: the WAV header declares %" PRIu64 " data bytes but the file holds %" PRIu64
                    " (a truncated recording)\n",
                    w->declared_data_bytes, actual_file_size > w->data_offset ? actual_file_size - w->data_offset : 0U);
    }
    if (w->unfinalized) {
        DSD_FPRINTF(err, "WARNING: the WAV header was never finalized (an interrupted recording); the data runs to "
                         "the end of the file\n");
    }
    if (w->container_clamped) {
        DSD_FPRINTF(err,
                    "WARNING: the WAV data chunk declares %" PRIu64 " bytes but its %s container ends %" PRIu64
                    " bytes into it (a writer inconsistency); the data stops there\n",
                    w->declared_data_bytes, dsd_iq_wav_container_name(w->container),
                    w->container_end > w->data_offset ? w->container_end - w->data_offset : 0U);
    }
    const uint64_t raw = dsd_iq_wav_data_bytes_in_file(w, actual_file_size, NULL);
    if (raw % (uint64_t)w->block_align != 0U) {
        DSD_FPRINTF(err, "WARNING: the data ends inside a frame; its last %" PRIu64 " byte(s) are not replayed\n",
                    raw % (uint64_t)w->block_align);
    }
    if (cfg->open_notice[0] != '\0') {
        DSD_FPRINTF(err, "WARNING: %s\n", cfg->open_notice);
    }
}

/* The WAV's centre frequency and start time lines, with where each came from. */
static void
info_print_wav_origin(const dsd_iq_wav_info* w, FILE* out) {
    if (w->center_source != DSD_IQ_WAV_FROM_NONE) {
        DSD_FPRINTF(out, "  Center frequency:    %.6f MHz (%s)\n", (double)w->center_frequency_hz / 1000000.0,
                    dsd_iq_wav_source_name(w->center_source));
    } else {
        DSD_FPRINTF(out, "  Center frequency:    unknown (no auxi chunk, none in the file name)\n");
    }
    if (w->start_source != DSD_IQ_WAV_FROM_NONE) {
        DSD_FPRINTF(out, "  Start time:          %s (%s)\n", w->start_utc, dsd_iq_wav_source_name(w->start_source));
    } else {
        DSD_FPRINTF(out, "  Start time:          unknown (no auxi chunk, none in the file name)\n");
    }
}

static int
dsd_iq_info_print_wav(const dsd_iq_replay_config* cfg, const char* display_path, uint64_t actual_file_size, FILE* out,
                      FILE* err) {
    const dsd_iq_wav_info* w = &cfg->wav;
    uint64_t effective = 0;
    int truncated = 0;
    int rc = dsd_iq_replay_measure(cfg, actual_file_size, &effective, &truncated);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    const uint64_t frames = effective / 8U;
    const double dur = cfg->sample_rate_hz ? (double)frames / (double)cfg->sample_rate_hz : 0.0;

    DSD_FPRINTF(out, "IQ Capture Info: %s\n", display_path ? display_path : cfg->metadata_path);
    DSD_FPRINTF(out, "  Format:              WAV I/Q recording (%s container)\n",
                dsd_iq_wav_container_name(w->container));
    DSD_FPRINTF(out, "  Sample encoding:     %s, 2 channels: I then Q\n", dsd_iq_wav_encoding_name(w->encoding));
    DSD_FPRINTF(out, "  Sample rate:         %u Hz\n", cfg->sample_rate_hz);
    info_print_wav_origin(w, out);
    DSD_FPRINTF(out, "  Data file:           %s\n", cfg->data_path);
    DSD_FPRINTF(out, "  Data offset:         %" PRIu64 " bytes\n", w->data_offset);
    if (w->unfinalized) {
        DSD_FPRINTF(out,
                    "  Data bytes:          unfinalized header, %" PRIu64 " (whole frames to the end of the file)\n",
                    frames * (uint64_t)w->block_align);
    } else {
        DSD_FPRINTF(out, "  Data bytes:          %" PRIu64 " (header), %" PRIu64 " (whole frames in the file)\n",
                    w->declared_data_bytes, frames * (uint64_t)w->block_align);
    }
    DSD_FPRINTF(out, "  Duration:            ~%.2f s (%" PRIu64 " frames)\n", dur, frames);
    /* The chain follows the DSP rate the decode mode runs (dsd_iq_replay_plan_rate()): the default 48 kHz, and TETRA's
       72 kHz, as examples. */
    char at48[96];
    char at72[96];
    /* Replayable when some DSP rate can be planned for it; a rate no plan reaches is refused by --iq-replay. */
    const int plans = info_describe_wav_plan(cfg, 48000U, 0U, at48, sizeof(at48))
                      | info_describe_wav_plan(cfg, 72000U, 18000U, at72, sizeof(at72));
    DSD_FPRINTF(out, "  Rate chain:          follows the DSP rate the mode runs; at 48 kHz %s, at TETRA's 72 kHz %s\n",
                at48, at72);
    DSD_FPRINTF(out, "  Replay compatible:   %s\n", (effective > 0U && plans) ? "yes" : "no");

    if (err) {
        info_print_wav_warnings(cfg, actual_file_size, truncated, err);
    }
    return DSD_IQ_OK;
}

int
dsd_iq_info_print(const dsd_iq_replay_config* cfg, const char* display_path, uint64_t actual_file_size, FILE* out,
                  FILE* err) {
    if (!cfg || !out) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (cfg->container == DSD_IQ_REPLAY_CONTAINER_WAV) {
        return dsd_iq_info_print_wav(cfg, display_path, actual_file_size, out, err);
    }

    uint64_t effective = 0;
    int mismatch = 0;
    int rc =
        dsd_iq_replay_compute_effective_bytes(cfg->data_bytes, actual_file_size, cfg->format, &effective, &mismatch);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    char compat_err[256] = {0};
    int replay_compatible =
        (effective > 0 && validate_replay_convertible(cfg, compat_err, sizeof(compat_err)) == DSD_IQ_OK
         && validate_replay_events_metadata(cfg, effective, 1, 1, compat_err, sizeof(compat_err)) == DSD_IQ_OK);

    dsd_iq_info_print_summary(cfg, display_path, actual_file_size, effective, replay_compatible, out);
    dsd_iq_info_print_warnings(cfg, actual_file_size, effective, mismatch, replay_compatible, compat_err, err);

    return DSD_IQ_OK;
}
