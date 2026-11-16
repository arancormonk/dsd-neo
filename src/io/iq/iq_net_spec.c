// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The `iqtcp:` / `iqudp:` input spec: parse and canonical text, and the wire formats' names and sizes. Socket-free, so
 * it is built into dsd-neo_io_iq for every build; the reader and the converters are in src/io/radio/iq_net_*.c.
 */

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_net_spec.h>
#include <stdint.h>
#include <string.h>

#include "iq_net_spec_internal.h"

enum { IQ_NET_FIELDS_MAX = 6, IQ_NET_FIELD_TEXT_MAX = 512 };

/* ---- Wire-format names ---- */

typedef struct {
    dsd_iq_net_format format;
    const char* name;
    size_t sample_bytes;
} iq_net_format_info;

static const iq_net_format_info k_formats[] = {
    {DSD_IQ_NET_FORMAT_CU8, "cu8", 2U},   {DSD_IQ_NET_FORMAT_CS8, "cs8", 2U},   {DSD_IQ_NET_FORMAT_CS16, "cs16", 4U},
    {DSD_IQ_NET_FORMAT_CS32, "cs32", 8U}, {DSD_IQ_NET_FORMAT_CF32, "cf32", 8U},
};

static const iq_net_format_info*
iq_net_format_lookup(dsd_iq_net_format format) {
    for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++) {
        if (k_formats[i].format == format) {
            return &k_formats[i];
        }
    }
    return NULL;
}

const char*
dsd_iq_net_format_name(dsd_iq_net_format format) {
    const iq_net_format_info* info = iq_net_format_lookup(format);
    return info ? info->name : NULL;
}

dsd_iq_net_format
dsd_iq_net_format_from_name(const char* name) {
    if (!name) {
        return DSD_IQ_NET_FORMAT_NONE;
    }
    for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++) {
        const char* want = k_formats[i].name;
        size_t j = 0;
        while (want[j] != '\0'
               && (name[j] == want[j] || (name[j] >= 'A' && name[j] <= 'Z' && name[j] + 32 == want[j]))) {
            j++;
        }
        if (want[j] == '\0' && name[j] == '\0') {
            return k_formats[i].format;
        }
    }
    return DSD_IQ_NET_FORMAT_NONE;
}

size_t
dsd_iq_net_format_sample_bytes(dsd_iq_net_format format) {
    const iq_net_format_info* info = iq_net_format_lookup(format);
    return info ? info->sample_bytes : 0U;
}

/* ---- Spec ---- */

static const char*
iq_net_kind_prefix(dsd_iq_net_kind kind) {
    return kind == DSD_IQ_NET_KIND_TCP ? "iqtcp" : "iqudp";
}

dsd_iq_net_kind
dsd_iq_net_spec_kind(const char* text) {
    if (!text) {
        return DSD_IQ_NET_KIND_NONE;
    }
    if (strncmp(text, "iqtcp:", 6) == 0) {
        return DSD_IQ_NET_KIND_TCP;
    }
    if (strncmp(text, "iqudp:", 6) == 0) {
        return DSD_IQ_NET_KIND_UDP;
    }
    return DSD_IQ_NET_KIND_NONE;
}

size_t
iq_net_split_fields(char* text, char** fields, size_t max_fields) {
    if (!text || !fields || max_fields == 0) {
        return 0;
    }
    size_t count = 0;
    char* field = text;
    for (;;) {
        if (count == max_fields) {
            return max_fields + 1U;
        }
        fields[count++] = field;
        char* sep = strchr(field, ':');
        if (!sep) {
            return count;
        }
        *sep = '\0';
        field = sep + 1;
    }
}

/* Multiply-add one decimal digit into *value, refusing anything above limit. */
static int
iq_net_push_digit(uint64_t* value, char digit, uint64_t limit) {
    const uint64_t d = (uint64_t)(digit - '0');
    if (*value > (limit - d) / 10U) {
        return -1;
    }
    *value = *value * 10U + d;
    return 0;
}

/* The power of ten a unit suffix stands for: k 3, m 6, g 9 (either case). */
static unsigned
iq_net_suffix_places(char suffix) {
    if (suffix == 'k' || suffix == 'K') {
        return 3U;
    }
    return (suffix == 'm' || suffix == 'M') ? 6U : 9U;
}

/* A decimal number scaled to tenths of a Hz: its value times 10^(places + 1), exactly. */
typedef struct {
    uint64_t tenths;
    unsigned frac;   /* fraction digits taken into tenths */
    int below_tenth; /* a non-zero digit past the tenth of a Hz */
} iq_net_decimal;

/* Scan text[0..len), `[0-9]*('.'[0-9]*)?` with at least one digit, into d at places. */
static int
iq_net_scan_decimal(const char* text, size_t len, unsigned places, iq_net_decimal* d) {
    const uint64_t limit = (uint64_t)UINT32_MAX * 10U + 9U;
    int digits = 0;
    int dot = 0;
    for (size_t i = 0; i < len; i++) {
        const char c = text[i];
        if (c == '.' && !dot) {
            dot = 1;
            continue;
        }
        if (c < '0' || c > '9') {
            return IQ_NET_HZ_SYNTAX;
        }
        digits++;
        if (dot && d->frac > places) {
            d->below_tenth |= (c != '0');
            continue;
        }
        d->frac += dot ? 1U : 0U;
        if (iq_net_push_digit(&d->tenths, c, limit) != 0) {
            return IQ_NET_HZ_RANGE;
        }
    }
    if (digits == 0) {
        return IQ_NET_HZ_SYNTAX;
    }
    for (; d->frac <= places; d->frac++) {
        if (iq_net_push_digit(&d->tenths, '0', limit) != 0) {
            return IQ_NET_HZ_RANGE;
        }
    }
    return IQ_NET_HZ_OK;
}

int
iq_net_parse_hz(const char* text, const char* suffixes, int whole_only, uint32_t* out_hz) {
    if (!text || !suffixes || !out_hz || text[0] == '\0') {
        return IQ_NET_HZ_SYNTAX;
    }
    size_t len = strlen(text);
    unsigned places = 0;
    if (strchr(suffixes, text[len - 1])) {
        places = iq_net_suffix_places(text[len - 1]);
        len--;
    }
    iq_net_decimal d = {0, 0, 0};
    const int rc = iq_net_scan_decimal(text, len, places, &d);
    if (rc != IQ_NET_HZ_OK) {
        return rc;
    }
    const uint64_t tenth = d.tenths % 10U;
    const uint64_t hz = d.tenths / 10U + (tenth >= 5U ? 1U : 0U);
    if (whole_only && (tenth != 0U || d.below_tenth)) {
        return IQ_NET_HZ_FRACTION;
    }
    if (hz > UINT32_MAX) {
        return IQ_NET_HZ_RANGE;
    }
    *out_hz = (uint32_t)hz;
    return IQ_NET_HZ_OK;
}

typedef struct {
    const char* prefix;
    char* err;
    size_t err_size;
    dsd_iq_net_field* field; /* the field being read, which a failure names; may be NULL */
} iq_net_parse_ctx;

/* The fields that follow are @p field's. */
static void
iq_net_at(const iq_net_parse_ctx* ctx, dsd_iq_net_field field) {
    if (ctx->field) {
        *ctx->field = field;
    }
}

static int
iq_net_fail(const iq_net_parse_ctx* ctx, const char* reason) {
    if (ctx->err && ctx->err_size > 0) {
        DSD_SNPRINTF(ctx->err, ctx->err_size, "%s: %s (expected %s:%s:PORT:RATE:FORMAT[:FREQ])", ctx->prefix, reason,
                     ctx->prefix, strcmp(ctx->prefix, "iqtcp") == 0 ? "HOST" : "BIND");
    }
    return -1;
}

static int
iq_net_parse_host(const iq_net_parse_ctx* ctx, const char* text, const char* name, dsd_iq_net_spec* spec) {
    char reason[96];
    if (text[0] == '\0') {
        DSD_SNPRINTF(reason, sizeof(reason), "%s is empty", name);
        return iq_net_fail(ctx, reason);
    }
    if (strlen(text) >= sizeof(spec->host)) {
        DSD_SNPRINTF(reason, sizeof(reason), "%s is longer than %d characters", name, DSD_IQ_NET_HOST_MAX - 1);
        return iq_net_fail(ctx, reason);
    }
    DSD_SNPRINTF(spec->host, sizeof(spec->host), "%s", text);
    return 0;
}

/* Fail naming field, quoting its text (at most 32 characters), and saying what is wrong with it. */
static int
iq_net_fail_value(const iq_net_parse_ctx* ctx, const char* field, const char* text, const char* problem) {
    char reason[160];
    DSD_SNPRINTF(reason, sizeof(reason), "%s '%.32s' %s", field, text, problem);
    return iq_net_fail(ctx, reason);
}

static int
iq_net_parse_port(const iq_net_parse_ctx* ctx, const char* text, dsd_iq_net_spec* spec) {
    if (text[0] == '\0') {
        return iq_net_fail(ctx, "PORT is empty");
    }
    int port = 0;
    if (text[0] < '0' || text[0] > '9' || dsd_parse_int_strict(text, 10, 1, 65535, &port) != 0) {
        return iq_net_fail_value(ctx, "PORT", text, "is not a port number from 1 to 65535");
    }
    spec->port = port;
    return 0;
}

static int
iq_net_parse_rate(const iq_net_parse_ctx* ctx, const char* text, dsd_iq_net_spec* spec) {
    if (text[0] == '\0') {
        return iq_net_fail(ctx, "RATE is empty");
    }
    uint32_t hz = 0;
    const int rc = iq_net_parse_hz(text, "kKmM", 1, &hz);
    if (rc == IQ_NET_HZ_FRACTION) {
        return iq_net_fail_value(ctx, "RATE", text, "is not a whole number of samples per second");
    }
    if (rc == IQ_NET_HZ_RANGE || (rc == IQ_NET_HZ_OK && hz < DSD_IQ_NET_MIN_RATE_HZ)) {
        char problem[64];
        DSD_SNPRINTF(problem, sizeof(problem), "is outside %u to %u samples per second", DSD_IQ_NET_MIN_RATE_HZ,
                     (unsigned)UINT32_MAX);
        return iq_net_fail_value(ctx, "RATE", text, problem);
    }
    if (rc != IQ_NET_HZ_OK) {
        return iq_net_fail_value(ctx, "RATE", text, "is not a sample rate (Hz, or a number with k or M)");
    }
    spec->rate_hz = hz;
    return 0;
}

static int
iq_net_parse_format(const iq_net_parse_ctx* ctx, const char* text, dsd_iq_net_spec* spec) {
    if (text[0] == '\0') {
        return iq_net_fail(ctx, "FORMAT is empty");
    }
    const dsd_iq_net_format format = dsd_iq_net_format_from_name(text);
    if (format == DSD_IQ_NET_FORMAT_NONE) {
        return iq_net_fail_value(ctx, "FORMAT", text, "is not one of cu8, cs8, cs16, cs32, cf32");
    }
    spec->format = format;
    return 0;
}

static int
iq_net_parse_freq(const iq_net_parse_ctx* ctx, const char* text, dsd_iq_net_spec* spec) {
    spec->freq_hz = 0;
    if (!text || text[0] == '\0') {
        return 0;
    }
    uint32_t hz = 0;
    const int rc = iq_net_parse_hz(text, "kKmMgG", 0, &hz);
    if (rc == IQ_NET_HZ_RANGE) {
        return iq_net_fail_value(ctx, "FREQ", text, "is above 4294.967295 MHz");
    }
    if (rc != IQ_NET_HZ_OK) {
        return iq_net_fail_value(ctx, "FREQ", text, "is not a frequency (Hz, or a number with k, M or G)");
    }
    spec->freq_hz = hz;
    return 0;
}

/* Parse the fields after the prefix in order, so the first bad or missing one is the one named. */
static int
iq_net_parse_fields(const iq_net_parse_ctx* ctx, char** fields, size_t n, dsd_iq_net_spec* spec) {
    static const char* const k_missing[] = {"", "", "PORT is missing", "RATE is missing", "FORMAT is missing"};
    const char* host_name = spec->kind == DSD_IQ_NET_KIND_TCP ? "HOST" : "BIND";
    iq_net_at(ctx, DSD_IQ_NET_FIELD_HOST);
    if (n < 2U) {
        return iq_net_fail(ctx, spec->kind == DSD_IQ_NET_KIND_TCP ? "HOST is missing" : "BIND is missing");
    }
    if (iq_net_parse_host(ctx, fields[1], host_name, spec) != 0) {
        return -1;
    }
    iq_net_at(ctx, DSD_IQ_NET_FIELD_PORT);
    if (n < 3U) {
        return iq_net_fail(ctx, k_missing[n]);
    }
    if (iq_net_parse_port(ctx, fields[2], spec) != 0) {
        return -1;
    }
    iq_net_at(ctx, DSD_IQ_NET_FIELD_RATE);
    if (n < 4U) {
        return iq_net_fail(ctx, k_missing[n]);
    }
    if (iq_net_parse_rate(ctx, fields[3], spec) != 0) {
        return -1;
    }
    iq_net_at(ctx, DSD_IQ_NET_FIELD_FORMAT);
    if (n < 5U) {
        return iq_net_fail(ctx, k_missing[n]);
    }
    if (iq_net_parse_format(ctx, fields[4], spec) != 0) {
        return -1;
    }
    iq_net_at(ctx, DSD_IQ_NET_FIELD_FREQ);
    return iq_net_parse_freq(ctx, n > 5U ? fields[5] : NULL, spec);
}

int
dsd_iq_net_spec_parse(const char* text, dsd_iq_net_spec* out, char* err, size_t err_size) {
    return dsd_iq_net_spec_parse_field(text, out, err, err_size, NULL);
}

int
dsd_iq_net_spec_parse_field(const char* text, dsd_iq_net_spec* out, char* err, size_t err_size,
                            dsd_iq_net_field* bad_field) {
    if (err && err_size > 0) {
        err[0] = '\0';
    }
    if (bad_field) {
        *bad_field = DSD_IQ_NET_FIELD_SPEC;
    }
    const dsd_iq_net_kind kind = dsd_iq_net_spec_kind(text);
    iq_net_parse_ctx ctx = {iq_net_kind_prefix(kind), err, err_size, bad_field};
    if (!out || kind == DSD_IQ_NET_KIND_NONE) {
        if (err && err_size > 0) {
            DSD_SNPRINTF(err, err_size, "not an iqtcp: or iqudp: spec");
        }
        return -1;
    }
    char scratch[IQ_NET_FIELD_TEXT_MAX];
    if (strlen(text) >= sizeof(scratch)) {
        return iq_net_fail(&ctx, "the spec is too long");
    }
    DSD_SNPRINTF(scratch, sizeof(scratch), "%s", text);
    char* fields[IQ_NET_FIELDS_MAX];
    const size_t n = iq_net_split_fields(scratch, fields, IQ_NET_FIELDS_MAX);
    if (n > IQ_NET_FIELDS_MAX) {
        return iq_net_fail(&ctx, "too many fields");
    }
    dsd_iq_net_spec spec;
    DSD_MEMSET(&spec, 0, sizeof(spec));
    spec.kind = kind;
    if (iq_net_parse_fields(&ctx, fields, n, &spec) != 0) {
        return -1;
    }
    iq_net_at(&ctx, DSD_IQ_NET_FIELD_NONE);
    *out = spec;
    return 0;
}

int
iq_net_spec_usable(const dsd_iq_net_spec* spec) {
    if (!spec || (spec->kind != DSD_IQ_NET_KIND_TCP && spec->kind != DSD_IQ_NET_KIND_UDP)) {
        return 0;
    }
    if (dsd_iq_net_format_sample_bytes(spec->format) == 0 || spec->port < 0 || spec->port > 65535) {
        return 0;
    }
    if (spec->kind == DSD_IQ_NET_KIND_TCP && spec->port == 0) {
        return 0;
    }
    return memchr(spec->host, '\0', sizeof(spec->host)) != NULL && spec->host[0] != '\0';
}

int
dsd_iq_net_spec_format(const dsd_iq_net_spec* spec, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (!iq_net_spec_usable(spec) || spec->port == 0 || spec->rate_hz < DSD_IQ_NET_MIN_RATE_HZ
        || strchr(spec->host, ':')) {
        return -1;
    }
    int len = 0;
    if (spec->freq_hz != 0) {
        len = DSD_SNPRINTF(out, out_size, "%s:%s:%d:%u:%s:%u", iq_net_kind_prefix(spec->kind), spec->host, spec->port,
                           (unsigned)spec->rate_hz, dsd_iq_net_format_name(spec->format), (unsigned)spec->freq_hz);
    } else {
        len = DSD_SNPRINTF(out, out_size, "%s:%s:%d:%u:%s", iq_net_kind_prefix(spec->kind), spec->host, spec->port,
                           (unsigned)spec->rate_hz, dsd_iq_net_format_name(spec->format));
    }
    if (len < 0 || (size_t)len >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return len;
}
