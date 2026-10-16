// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Two-channel WAV I/Q recordings (issue #701). The container rules follow EBU Tech 3306 (RF64) for 64-bit sizes and
 * cover the forms SDR software leaves behind: unfinalized streaming files, odd chunks with and without their pad byte,
 * fmt after data, and trailing chunks after data.
 */

#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_wav.h>
#include <dsd-neo/platform/file_compat.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/platform/posix_compat.h"

static void wav_set_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 3, 4);

static void
wav_set_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) {
    if (!err_buf || err_buf_size == 0 || !fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(err_buf, err_buf_size, fmt, ap);
    va_end(ap);
    err_buf[err_buf_size - 1] = '\0';
}

static uint16_t
le16(const uint8_t* p) {
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t
le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
le64(const uint8_t* p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

int
dsd_iq_wav_sniff(const void* head, size_t len) {
    if (!head || len < DSD_IQ_WAV_SNIFF_BYTES) {
        return DSD_IQ_WAV_SNIFF_NOT_WAV;
    }
    const uint8_t* p = (const uint8_t*)head;
    if (memcmp(p + 8, "WAVE", 4) != 0) {
        return DSD_IQ_WAV_SNIFF_NOT_WAV;
    }
    if (memcmp(p, "RIFF", 4) == 0 || memcmp(p, "RF64", 4) == 0 || memcmp(p, "BW64", 4) == 0) {
        return DSD_IQ_WAV_SNIFF_WAV;
    }
    if (memcmp(p, "RIFX", 4) == 0) {
        return DSD_IQ_WAV_SNIFF_RIFX;
    }
    return DSD_IQ_WAV_SNIFF_NOT_WAV;
}

const char*
dsd_iq_wav_encoding_name(dsd_iq_wav_encoding encoding) {
    switch (encoding) {
        case DSD_IQ_WAV_ENC_U8: return "PCM 8-bit unsigned";
        case DSD_IQ_WAV_ENC_S16: return "PCM 16-bit";
        case DSD_IQ_WAV_ENC_S24: return "PCM 24-bit";
        case DSD_IQ_WAV_ENC_S32: return "PCM 32-bit";
        case DSD_IQ_WAV_ENC_F32: return "IEEE float 32-bit";
        case DSD_IQ_WAV_ENC_F64: return "IEEE float 64-bit";
        default: return "unknown";
    }
}

const char*
dsd_iq_wav_container_name(dsd_iq_wav_container container) {
    switch (container) {
        case DSD_IQ_WAV_CONTAINER_RIFF: return "RIFF";
        case DSD_IQ_WAV_CONTAINER_RF64: return "RF64";
        case DSD_IQ_WAV_CONTAINER_BW64: return "BW64";
        default: return "unknown";
    }
}

const char*
dsd_iq_wav_source_name(dsd_iq_wav_source source) {
    switch (source) {
        case DSD_IQ_WAV_FROM_AUXI: return "auxi chunk";
        case DSD_IQ_WAV_FROM_FILE_NAME: return "file name";
        default: return "none";
    }
}

/* ---- Conversion ------------------------------------------------------------------------------------------------ */

static float
wav_f64_to_f32(const uint8_t* p) {
    const uint64_t bits = le64(p);
    const uint64_t magnitude = bits & 0x7FFFFFFFFFFFFFFFULL;
    const int negative = (bits >> 63) != 0U;
    /* Classified on the bits, before narrowing, so the checks hold under fast-math: exponent all ones is an infinity
       (mantissa 0) or a NaN; positive doubles order as their bit patterns do, so a magnitude above the clamp's bits
       is above the clamp. */
    if ((magnitude >> 52) == 0x7FFU && (magnitude & 0x000FFFFFFFFFFFFFULL) != 0U) {
        return 0.0f;
    }
    double clamp = DSD_IQ_WAV_F64_CLAMP;
    uint64_t clamp_bits = 0;
    DSD_MEMCPY(&clamp_bits, &clamp, sizeof(clamp_bits));
    if (magnitude > clamp_bits) {
        return negative ? (float)-DSD_IQ_WAV_F64_CLAMP : (float)DSD_IQ_WAV_F64_CLAMP;
    }
    double value = 0.0;
    DSD_MEMCPY(&value, &bits, sizeof(value));
    return (float)value;
}

void
dsd_iq_wav_frames_to_cf32(dsd_iq_wav_encoding encoding, const void* src, size_t frames, float* dst) {
    if (!src || !dst || frames == 0U) {
        return;
    }
    const uint8_t* p = (const uint8_t*)src;
    const size_t n = frames * 2U;
    switch (encoding) {
        case DSD_IQ_WAV_ENC_U8: {
            /* The cu8 widen's form (widen_u8_to_f32_bias127()), so a u8 recording carries no DC line. */
            const float inv = 1.0f / 127.5f;
            for (size_t i = 0; i < n; i++) {
                dst[i] = ((float)p[i] - 127.5f) * inv;
            }
            break;
        }
        case DSD_IQ_WAV_ENC_S16: {
            const float inv = 1.0f / 32768.0f;
            for (size_t i = 0; i < n; i++) {
                dst[i] = (float)(int16_t)le16(p + i * 2U) * inv;
            }
            break;
        }
        case DSD_IQ_WAV_ENC_S24: {
            const float inv = 1.0f / 8388608.0f;
            for (size_t i = 0; i < n; i++) {
                const uint8_t* s = p + i * 3U;
                uint32_t u = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16);
                int32_t v = (u & 0x800000U) ? (int32_t)(u | 0xFF000000U) : (int32_t)u;
                dst[i] = (float)v * inv;
            }
            break;
        }
        case DSD_IQ_WAV_ENC_S32: {
            const float inv = 1.0f / 2147483648.0f;
            for (size_t i = 0; i < n; i++) {
                dst[i] = (float)(int32_t)le32(p + i * 4U) * inv;
            }
            break;
        }
        case DSD_IQ_WAV_ENC_F32: {
            for (size_t i = 0; i < n; i++) {
                const uint32_t bits = le32(p + i * 4U);
                DSD_MEMCPY(&dst[i], &bits, sizeof(float));
            }
            break;
        }
        case DSD_IQ_WAV_ENC_F64: {
            for (size_t i = 0; i < n; i++) {
                dst[i] = wav_f64_to_f32(p + i * 8U);
            }
            break;
        }
        default: DSD_MEMSET(dst, 0, n * sizeof(float)); break;
    }
}

/* ---- File-name metadata ---------------------------------------------------------------------------------------- */

static int
ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int
ends_with_ci(const char* s, size_t len, const char* suffix) {
    const size_t n = strlen(suffix);
    if (len < n) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (ascii_lower((unsigned char)s[len - n + i]) != ascii_lower((unsigned char)suffix[i])) {
            return 0;
        }
    }
    return 1;
}

/* The base name of @p path without a ".wav" extension, in @p out. 0 when it does not fit. */
static int
name_stem(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0U) {
        return 0;
    }
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t len = strlen(base);
    if (ends_with_ci(base, len, ".wav")) {
        len -= 4U;
    }
    if (len + 1U > out_size) {
        return 0;
    }
    DSD_MEMCPY(out, base, len);
    out[len] = '\0';
    return 1;
}

/* Next '_'-separated token of @p s from *@p pos: its start and length. 0 at the end. */
static int
next_token(const char* s, size_t* pos, const char** tok, size_t* tok_len) {
    size_t i = *pos;
    if (s[i] == '\0') {
        return 0;
    }
    const size_t start = i;
    while (s[i] != '\0' && s[i] != '_') {
        i++;
    }
    *tok = s + start;
    *tok_len = i - start;
    *pos = (s[i] == '_') ? i + 1U : i;
    return 1;
}

static int
is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* `<n>[.<f>](Hz|kHz|MHz)` as a whole number of hertz. */
static int
parse_freq_token(const char* tok, size_t len, uint64_t* out_hz) {
    size_t i = 0;
    uint64_t whole = 0;
    size_t whole_digits = 0;
    while (i < len && is_digit(tok[i])) {
        const uint64_t d = (uint64_t)(tok[i] - '0');
        if (whole > (UINT64_MAX - d) / 10U) {
            return 0;
        }
        whole = whole * 10U + d;
        whole_digits++;
        i++;
    }
    if (whole_digits == 0U) {
        return 0;
    }
    const char* frac = NULL;
    size_t frac_len = 0;
    if (i < len && tok[i] == '.') {
        i++;
        frac = tok + i;
        while (i < len && is_digit(tok[i])) {
            frac_len++;
            i++;
        }
        if (frac_len == 0U) {
            return 0;
        }
    }
    const size_t unit_len = len - i;
    unsigned decimals = 0;
    if (unit_len == 2U && ascii_lower((unsigned char)tok[i]) == 'h' && ascii_lower((unsigned char)tok[i + 1U]) == 'z') {
        decimals = 0;
    } else if (unit_len == 3U && ascii_lower((unsigned char)tok[i + 1U]) == 'h'
               && ascii_lower((unsigned char)tok[i + 2U]) == 'z') {
        const int prefix = ascii_lower((unsigned char)tok[i]);
        if (prefix == 'k') {
            decimals = 3;
        } else if (prefix == 'm') {
            decimals = 6;
        } else {
            return 0;
        }
    } else {
        return 0;
    }
    uint64_t scale = 1;
    for (unsigned k = 0; k < decimals; k++) {
        scale *= 10U;
    }
    if (whole > UINT64_MAX / scale) {
        return 0;
    }
    uint64_t hz = whole * scale;
    /* The fraction's digits past the unit's decimals must be 0: the value is a whole number of hertz. */
    uint64_t frac_hz = 0;
    for (size_t k = 0; k < frac_len; k++) {
        const uint64_t d = (uint64_t)(frac[k] - '0');
        if (k < decimals) {
            frac_hz = frac_hz * 10U + d;
        } else if (d != 0U) {
            return 0;
        }
    }
    for (size_t k = frac_len; k < decimals; k++) {
        frac_hz *= 10U;
    }
    if (hz > UINT64_MAX - frac_hz) {
        return 0;
    }
    *out_hz = hz + frac_hz;
    return 1;
}

int
dsd_iq_wav_center_from_name(const char* path, uint64_t* out_hz) {
    char stem[1024];
    if (!out_hz || !name_stem(path, stem, sizeof(stem))) {
        return 0;
    }
    size_t pos = 0;
    const char* tok = NULL;
    size_t tok_len = 0;
    int found = 0;
    uint64_t last = 0;
    while (next_token(stem, &pos, &tok, &tok_len)) {
        uint64_t hz = 0;
        if (parse_freq_token(tok, tok_len, &hz)) {
            last = hz;
            found = 1;
        }
    }
    if (!found || last == 0U || last > UINT32_MAX) {
        return 0;
    }
    *out_hz = last;
    return 1;
}

static int
all_digits(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i])) {
            return 0;
        }
    }
    return 1;
}

/* YYYY-MM-DDTHH:MM:SSZ from its fields, kept only when it is a real UTC time. */
static int
format_utc(char* out, size_t out_size, unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute,
           unsigned second) {
    if (!out || out_size < DSD_IQ_WAV_START_UTC_SIZE || year > 9999U || month > 99U || day > 99U || hour > 99U
        || minute > 99U || second > 99U) {
        return 0;
    }
    char text[DSD_IQ_WAV_START_UTC_SIZE];
    DSD_SNPRINTF(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", year, month, day, hour, minute, second);
    int64_t seconds = 0;
    if (dsd_iq_replay_parse_utc_seconds(text, &seconds) != DSD_IQ_OK) {
        return 0;
    }
    DSD_SNPRINTF(out, out_size, "%s", text);
    return 1;
}

static unsigned
digits_value(const char* s, size_t n) {
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        v = v * 10U + (unsigned)(s[i] - '0');
    }
    return v;
}

int
dsd_iq_wav_start_from_name(const char* path, char* out, size_t out_size) {
    char stem[1024];
    if (!out || out_size < DSD_IQ_WAV_START_UTC_SIZE || !name_stem(path, stem, sizeof(stem))) {
        return 0;
    }
    size_t pos = 0;
    const char* prev = NULL;
    size_t prev_len = 0;
    const char* tok = NULL;
    size_t tok_len = 0;
    while (next_token(stem, &pos, &tok, &tok_len)) {
        if (prev && prev_len == 8U && all_digits(prev, 8U) && tok_len == 7U && all_digits(tok, 6U) && tok[6] == 'Z') {
            if (format_utc(out, out_size, digits_value(prev, 4U), digits_value(prev + 4, 2U),
                           digits_value(prev + 6, 2U), digits_value(tok, 2U), digits_value(tok + 2, 2U),
                           digits_value(tok + 4, 2U))) {
                return 1;
            }
        }
        prev = tok;
        prev_len = tok_len;
    }
    return 0;
}

/* ---- Container ------------------------------------------------------------------------------------------------- */

typedef struct {
    FILE* fp;
    const char* path;
    uint64_t file_size;
    uint64_t walk_end;
    int rf64;
    int riff_open; /* a RIFF whose size is 0, 0xFFFFFFFF or past the file */
    uint64_t ds64_data_size;
    int have_fmt;
    uint8_t fmt[40];
    uint32_t fmt_size;
    int have_data;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t declared_data_bytes;
    int unfinalized;
    int have_auxi_center;
    uint64_t auxi_center_hz;
    int have_auxi_start;
    char auxi_start[DSD_IQ_WAV_START_UTC_SIZE];
} wav_walk;

static int
read_at(FILE* fp, uint64_t offset, void* buf, size_t n) {
    if (offset > (uint64_t)INT64_MAX || dsd_fseek64(fp, (int64_t)offset, SEEK_SET) != 0) {
        return -1;
    }
    return fread(buf, 1, n, fp) == n ? 0 : -1;
}

static int
id_is(const uint8_t* id, const char* want) {
    return memcmp(id, want, 4) == 0;
}

static int
id_is_ascii(const uint8_t* id) {
    for (int i = 0; i < 4; i++) {
        if (id[i] < 0x20U || id[i] > 0x7EU) {
            return 0;
        }
    }
    return 1;
}

/* Whether a chunk header can start at @p pos: a printable id, and a body inside the walk (a data chunk may run past
 * it: a truncated recording). */
static int
header_valid_at(const wav_walk* w, uint64_t pos) {
    if (pos > w->walk_end || w->walk_end - pos < 8U) {
        return 0;
    }
    uint8_t hdr[8];
    if (read_at(w->fp, pos, hdr, sizeof(hdr)) != 0 || !id_is_ascii(hdr)) {
        return 0;
    }
    if (id_is(hdr, "data")) {
        return 1;
    }
    return (uint64_t)le32(hdr + 4) <= w->walk_end - pos - 8U;
}

static void
wav_read_auxi(wav_walk* w, uint64_t body, uint64_t size) {
    uint8_t aux[36];
    DSD_MEMSET(aux, 0, sizeof(aux));
    const size_t n = size < sizeof(aux) ? (size_t)size : sizeof(aux);
    if (n < 16U || read_at(w->fp, body, aux, n) != 0) {
        return;
    }
    /* SYSTEMTIME StartTime: wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds. Taken as UTC.
       SYSTEMTIME starts at 1601; an earlier year (0 in a writer that left it unset) records no time. */
    if (!w->have_auxi_start && le16(aux) >= 1601U
        && format_utc(w->auxi_start, sizeof(w->auxi_start), le16(aux), le16(aux + 2), le16(aux + 6), le16(aux + 8),
                      le16(aux + 10), le16(aux + 12))) {
        w->have_auxi_start = 1;
    }
    /* After StartTime and StopTime, the centre frequency in Hz. */
    if (n >= 36U && !w->have_auxi_center) {
        const uint32_t hz = le32(aux + 32);
        if (hz != 0U) {
            w->auxi_center_hz = hz;
            w->have_auxi_center = 1;
        }
    }
}

/* The data chunk at @p body with header size @p size32. Sets the size the walk steps over; *@p stop ends the walk. */
static void
wav_take_data(wav_walk* w, uint64_t body, uint32_t size32, uint64_t* step, int* stop) {
    *stop = 0;
    if (w->rf64 && size32 == 0xFFFFFFFFU) {
        w->data_size = w->ds64_data_size;
        w->declared_data_bytes = w->ds64_data_size;
    } else if (!w->rf64 && w->riff_open && (size32 == 0U || size32 == 0xFFFFFFFFU)) {
        /* An unfinalized RIFF, when data is its last chunk: a size of 0 with a chunk header right after it is an empty
           data chunk instead. 0xFFFFFFFF leaves no way to find a later chunk. */
        if (size32 == 0U && header_valid_at(w, body)) {
            w->data_size = 0;
            w->declared_data_bytes = 0;
        } else {
            w->unfinalized = 1;
            w->declared_data_bytes = size32;
            w->data_size = w->file_size > body ? w->file_size - body : 0U;
            *stop = 1;
        }
    } else {
        w->data_size = size32;
        w->declared_data_bytes = size32;
    }
    w->have_data = 1;
    w->data_offset = body;
    *step = w->data_size;
}

/* RF64/BW64: the ds64 chunk that must come first, and the walk's end from its RIFF size. */
static int
wav_read_ds64(wav_walk* w, char* err_buf, size_t err_buf_size) {
    uint8_t hdr[8];
    uint8_t body[24];
    if (read_at(w->fp, 12, hdr, sizeof(hdr)) != 0 || !id_is(hdr, "ds64")) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': an RF64/BW64 file must start with a ds64 chunk", w->path);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (le32(hdr + 4) < 24U || read_at(w->fp, 20, body, sizeof(body)) != 0) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': the ds64 chunk is too short", w->path);
        return DSD_IQ_ERR_INVALID_META;
    }
    const uint64_t riff_size = le64(body);
    w->ds64_data_size = le64(body + 8);
    w->walk_end = (riff_size <= UINT64_MAX - 8U && 8U + riff_size < w->file_size) ? 8U + riff_size : w->file_size;
    return DSD_IQ_OK;
}

/* Where the walk goes after a chunk whose body ends at @p next: the padded position when its size is odd, unless that
 * cannot be a chunk header and the unpadded one can (a writer that left the pad byte out). */
static uint64_t
wav_next_chunk(const wav_walk* w, uint64_t next, uint64_t size) {
    if ((size & 1U) == 0U || next == UINT64_MAX) {
        return next;
    }
    const uint64_t padded = next + 1U;
    if (header_valid_at(w, padded)) {
        return padded;
    }
    if (header_valid_at(w, next)) {
        return next;
    }
    return padded;
}

static int
wav_walk_chunks(wav_walk* w, char* err_buf, size_t err_buf_size) {
    uint64_t pos = 12;
    for (unsigned count = 0; count < DSD_IQ_WAV_MAX_CHUNKS; count++) {
        if (pos > w->walk_end || w->walk_end - pos < 8U) {
            break;
        }
        uint8_t hdr[8];
        if (read_at(w->fp, pos, hdr, sizeof(hdr)) != 0 || !id_is_ascii(hdr)) {
            break;
        }
        const uint32_t size32 = le32(hdr + 4);
        const uint64_t body = pos + 8U;
        uint64_t step = size32;
        int stop = 0;
        if (id_is(hdr, "fmt ")) {
            if (!w->have_fmt) {
                w->fmt_size = size32;
                DSD_MEMSET(w->fmt, 0, sizeof(w->fmt));
                const size_t n = size32 < sizeof(w->fmt) ? (size_t)size32 : sizeof(w->fmt);
                if (n < 16U || read_at(w->fp, body, w->fmt, n) != 0) {
                    wav_set_error(err_buf, err_buf_size, "WAV '%s': the fmt chunk is too short (%u bytes)", w->path,
                                  (unsigned)size32);
                    return DSD_IQ_ERR_INVALID_META;
                }
                w->have_fmt = 1;
            }
        } else if (id_is(hdr, "data")) {
            if (w->have_data) {
                break;
            }
            wav_take_data(w, body, size32, &step, &stop);
        } else if (id_is(hdr, "auxi")) {
            wav_read_auxi(w, body, size32);
        }
        if (stop || step > UINT64_MAX - body) {
            break;
        }
        const uint64_t next = body + step;
        if (next >= w->walk_end) {
            break;
        }
        pos = wav_next_chunk(w, next, step);
    }
    if (!w->have_fmt) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': no fmt chunk", w->path);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (!w->have_data) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': no data chunk", w->path);
        return DSD_IQ_ERR_INVALID_META;
    }
    return DSD_IQ_OK;
}

/* KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT after their first two bytes (the format tag). */
static const uint8_t kSubformatGuidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                               0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

static int
wav_parse_fmt(const wav_walk* w, dsd_iq_wav_info* info, char* err_buf, size_t err_buf_size) {
    const uint8_t* f = w->fmt;
    uint16_t tag = le16(f);
    const uint16_t channels = le16(f + 2);
    const uint32_t rate = le32(f + 4);
    const uint16_t block_align = le16(f + 12);
    const uint16_t bits = le16(f + 14);
    uint16_t valid_bits = bits;
    info->format_tag = tag;
    if (tag == 0xFFFEU) {
        if (w->fmt_size < 40U || le16(f + 16) < 22U) {
            wav_set_error(err_buf, err_buf_size, "WAV '%s': the EXTENSIBLE fmt chunk is too short", w->path);
            return DSD_IQ_ERR_INVALID_META;
        }
        valid_bits = le16(f + 18);
        const uint16_t sub = le16(f + 24);
        if ((sub != 1U && sub != 3U) || memcmp(f + 26, kSubformatGuidTail, sizeof(kSubformatGuidTail)) != 0) {
            wav_set_error(err_buf, err_buf_size,
                          "WAV '%s': unsupported EXTENSIBLE subformat (replay reads PCM and IEEE float)", w->path);
            return DSD_IQ_ERR_UNSUPPORTED_FMT;
        }
        if (valid_bits == 0U || valid_bits > bits) {
            wav_set_error(err_buf, err_buf_size, "WAV '%s': %u valid bits in a %u-bit sample", w->path,
                          (unsigned)valid_bits, (unsigned)bits);
            return DSD_IQ_ERR_INVALID_META;
        }
        tag = sub;
    }
    if (tag == 1U) {
        switch (bits) {
            case 8: info->encoding = DSD_IQ_WAV_ENC_U8; break;
            case 16: info->encoding = DSD_IQ_WAV_ENC_S16; break;
            case 24: info->encoding = DSD_IQ_WAV_ENC_S24; break;
            case 32: info->encoding = DSD_IQ_WAV_ENC_S32; break;
            default:
                wav_set_error(err_buf, err_buf_size,
                              "WAV '%s': unsupported PCM sample size %u bits (replay reads 8, 16, 24 and 32)", w->path,
                              (unsigned)bits);
                return DSD_IQ_ERR_UNSUPPORTED_FMT;
        }
    } else if (tag == 3U) {
        if (bits == 32U) {
            info->encoding = DSD_IQ_WAV_ENC_F32;
        } else if (bits == 64U) {
            info->encoding = DSD_IQ_WAV_ENC_F64;
        } else {
            wav_set_error(err_buf, err_buf_size,
                          "WAV '%s': unsupported float sample size %u bits (replay reads 32 and 64)", w->path,
                          (unsigned)bits);
            return DSD_IQ_ERR_UNSUPPORTED_FMT;
        }
    } else {
        wav_set_error(err_buf, err_buf_size,
                      "WAV '%s': unsupported format tag 0x%04X (replay reads PCM, IEEE float and EXTENSIBLE)", w->path,
                      (unsigned)tag);
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    if (channels == 1U) {
        wav_set_error(err_buf, err_buf_size,
                      "WAV '%s' has 1 channel: I/Q replay needs 2 (I, then Q); decode an audio WAV with -i '%s'",
                      w->path, w->path);
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    if (channels != 2U) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s' has %u channels: I/Q replay needs exactly 2 (I, then Q)",
                      w->path, (unsigned)channels);
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    if ((unsigned)block_align != 2U * (unsigned)bits / 8U) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': block align %u does not match 2 channels of %u bits", w->path,
                      (unsigned)block_align, (unsigned)bits);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (rate == 0U) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': the sample rate is 0", w->path);
        return DSD_IQ_ERR_INVALID_META;
    }
    info->bits_per_sample = bits;
    info->valid_bits = valid_bits;
    info->block_align = block_align;
    info->sample_rate_hz = rate;
    return DSD_IQ_OK;
}

int
dsd_iq_wav_frames_in_file(const dsd_iq_wav_info* info, uint64_t file_size, uint64_t* out_frames, int* out_truncated) {
    if (!info || !out_frames || info->block_align == 0U) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    const uint64_t avail = file_size > info->data_offset ? file_size - info->data_offset : 0U;
    uint64_t raw = avail;
    int truncated = 0;
    if (!info->unfinalized) {
        raw = info->declared_data_bytes < avail ? info->declared_data_bytes : avail;
        truncated = info->declared_data_bytes > avail;
    }
    *out_frames = raw / info->block_align;
    if (out_truncated) {
        *out_truncated = truncated;
    }
    return DSD_IQ_OK;
}

static int
wav_open_and_size(const char* path, FILE** out_fp, uint64_t* out_size, char* err_buf, size_t err_buf_size) {
    FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
    if (!fp) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': cannot open: %s", path, strerror(errno));
        return DSD_IQ_ERR_IO;
    }
    dsd_stat_t st;
    if (dsd_fstat(dsd_fileno(fp), &st) != 0 || st.st_size < 0) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': cannot read its size: %s", path, strerror(errno));
        fclose(fp);
        return DSD_IQ_ERR_IO;
    }
    *out_fp = fp;
    *out_size = (uint64_t)st.st_size;
    return DSD_IQ_OK;
}

static int
wav_read_header(wav_walk* w, dsd_iq_wav_info* info, char* err_buf, size_t err_buf_size) {
    uint8_t head[DSD_IQ_WAV_SNIFF_BYTES];
    if (w->file_size < sizeof(head) || read_at(w->fp, 0, head, sizeof(head)) != 0) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': too short for a WAV header", w->path);
        return DSD_IQ_ERR_INVALID_META;
    }
    const int sniff = dsd_iq_wav_sniff(head, sizeof(head));
    if (sniff == DSD_IQ_WAV_SNIFF_RIFX) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': big-endian RIFX files are not supported", w->path);
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    if (sniff != DSD_IQ_WAV_SNIFF_WAV) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': not a RIFF, RF64 or BW64 WAVE file", w->path);
        return DSD_IQ_ERR_INVALID_META;
    }
    if (memcmp(head, "RIFF", 4) == 0) {
        info->container = DSD_IQ_WAV_CONTAINER_RIFF;
        const uint32_t riff_size = le32(head + 4);
        if (riff_size == 0U || riff_size == 0xFFFFFFFFU || 8U + (uint64_t)riff_size > w->file_size) {
            w->riff_open = 1;
            w->walk_end = w->file_size;
        } else {
            w->walk_end = 8U + (uint64_t)riff_size;
        }
        return DSD_IQ_OK;
    }
    info->container = memcmp(head, "RF64", 4) == 0 ? DSD_IQ_WAV_CONTAINER_RF64 : DSD_IQ_WAV_CONTAINER_BW64;
    w->rf64 = 1;
    return wav_read_ds64(w, err_buf, err_buf_size);
}

static void
wav_apply_metadata(const wav_walk* w, const char* path, dsd_iq_wav_info* info) {
    if (w->have_auxi_center) {
        info->center_frequency_hz = w->auxi_center_hz;
        info->center_source = DSD_IQ_WAV_FROM_AUXI;
    } else if (dsd_iq_wav_center_from_name(path, &info->center_frequency_hz)) {
        info->center_source = DSD_IQ_WAV_FROM_FILE_NAME;
    }
    if (w->have_auxi_start) {
        DSD_SNPRINTF(info->start_utc, sizeof(info->start_utc), "%s", w->auxi_start);
        info->start_source = DSD_IQ_WAV_FROM_AUXI;
    } else if (dsd_iq_wav_start_from_name(path, info->start_utc, sizeof(info->start_utc))) {
        info->start_source = DSD_IQ_WAV_FROM_FILE_NAME;
    }
}

int
dsd_iq_wav_read_info(const char* path, dsd_iq_wav_info* out, char* err_buf, size_t err_buf_size) {
    if (!path || !out) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    wav_walk w;
    DSD_MEMSET(&w, 0, sizeof(w));
    w.path = path;
    int rc = wav_open_and_size(path, &w.fp, &w.file_size, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    dsd_iq_wav_info info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.file_size = w.file_size;
    rc = wav_read_header(&w, &info, err_buf, err_buf_size);
    if (rc == DSD_IQ_OK) {
        rc = wav_walk_chunks(&w, err_buf, err_buf_size);
    }
    fclose(w.fp);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    rc = wav_parse_fmt(&w, &info, err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    if (!w.unfinalized && w.declared_data_bytes == 0U) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': the data chunk is empty (no I/Q frames)", path);
        return DSD_IQ_ERR_ALIGNMENT;
    }
    info.data_offset = w.data_offset;
    info.declared_data_bytes = w.declared_data_bytes;
    info.unfinalized = w.unfinalized;
    (void)dsd_iq_wav_frames_in_file(&info, w.file_size, &info.frames, &info.truncated);
    if (info.frames == 0U) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': the data chunk holds no whole I/Q frame", path);
        return DSD_IQ_ERR_ALIGNMENT;
    }
    if (info.frames > UINT64_MAX / 8U) {
        wav_set_error(err_buf, err_buf_size, "WAV '%s': too many frames", path);
        return DSD_IQ_ERR_INVALID_META;
    }
    info.effective_data_bytes = info.frames * info.block_align;
    wav_apply_metadata(&w, path, &info);
    *out = info;
    return DSD_IQ_OK;
}
