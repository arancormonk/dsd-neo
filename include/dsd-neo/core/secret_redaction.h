// SPDX-License-Identifier: ISC

#ifndef DSD_NEO_CORE_SECRET_REDACTION_H
#define DSD_NEO_CORE_SECRET_REDACTION_H

#include <dsd-neo/core/safe_api.h>

#include <stddef.h>
#include <stdint.h>

#define DSD_SECRET_REDACTED "[redacted]"

static inline const char*
dsd_secret_format_decimal(char* out, size_t out_size, int show_keys, unsigned long long value, unsigned width) {
    if (show_keys == 0 || out == NULL || out_size == 0U) {
        return DSD_SECRET_REDACTED;
    }
    if (width > 0U) {
        (void)DSD_SNPRINTF(out, out_size, "%0*llu", (int)width, value);
    } else {
        (void)DSD_SNPRINTF(out, out_size, "%llu", value);
    }
    return out;
}

static inline const char*
dsd_secret_format_hex(char* out, size_t out_size, int show_keys, unsigned long long value, unsigned width,
                      int with_prefix) {
    if (show_keys == 0 || out == NULL || out_size == 0U) {
        return DSD_SECRET_REDACTED;
    }
    if (with_prefix != 0) {
        (void)DSD_SNPRINTF(out, out_size, "0x%0*llX", (int)width, value);
    } else {
        (void)DSD_SNPRINTF(out, out_size, "%0*llX", (int)width, value);
    }
    return out;
}

static inline const char*
dsd_secret_format_u64_segments(char* out, size_t out_size, int show_keys, const unsigned long long* segments,
                               size_t segment_count) {
    if (show_keys == 0 || out == NULL || out_size == 0U || segments == NULL || segment_count == 0U) {
        return DSD_SECRET_REDACTED;
    }

    size_t pos = 0U;
    out[0] = '\0';
    for (size_t i = 0U; i < segment_count; i++) {
        int n = DSD_SNPRINTF(out + pos, out_size - pos, (i == 0U) ? "%016llX" : " %016llX", segments[i]);
        if (n < 0) {
            out[0] = '\0';
            return out;
        }
        if ((size_t)n >= out_size - pos) {
            out[out_size - 1U] = '\0';
            return out;
        }
        pos += (size_t)n;
    }
    return out;
}

static inline const char*
dsd_secret_format_byte_hex(char* out, size_t out_size, int show_keys, const uint8_t* bytes, size_t byte_count) {
    if (show_keys == 0 || out == NULL || out_size == 0U || bytes == NULL) {
        return DSD_SECRET_REDACTED;
    }

    size_t pos = 0U;
    out[0] = '\0';
    for (size_t i = 0U; i < byte_count; i++) {
        int n = DSD_SNPRINTF(out + pos, out_size - pos, "%02X", bytes[i]);
        if (n < 0) {
            out[0] = '\0';
            return out;
        }
        if ((size_t)n >= out_size - pos) {
            out[out_size - 1U] = '\0';
            return out;
        }
        pos += (size_t)n;
    }
    return out;
}

static inline const char*
dsd_secret_format_string(char* out, size_t out_size, int show_keys, const char* value) {
    if (show_keys == 0 || out == NULL || out_size == 0U || value == NULL) {
        return DSD_SECRET_REDACTED;
    }
    (void)DSD_SNPRINTF(out, out_size, "%s", value);
    return out;
}

#endif /* DSD_NEO_CORE_SECRET_REDACTION_H */
