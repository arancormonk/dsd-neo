// SPDX-License-Identifier: ISC
#ifndef DSD_NEO_CRYPTO_VENDOR_AP_KEY_PARSE_H
#define DSD_NEO_CRYPTO_VENDOR_AP_KEY_PARSE_H

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned char hex[64];
    size_t nhex;
    uint64_t words[4];
} dsd_vendor_ap_key;

enum {
    DSD_VENDOR_AP_KEY_OK = 0,
    DSD_VENDOR_AP_KEY_INVALID = -1,
    DSD_VENDOR_AP_KEY_BAD_LENGTH = -2,
};

static inline int
dsd_vendor_ap_key_nibble(int c) {
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

static inline unsigned char
dsd_vendor_ap_key_hex_char(int nibble) {
    return (unsigned char)(nibble <= 9 ? ('0' + nibble) : ('A' + (nibble - 10)));
}

static inline int
dsd_vendor_ap_key_parse(const char* input, dsd_vendor_ap_key* out) {
    if (input == NULL || out == NULL) {
        return DSD_VENDOR_AP_KEY_INVALID;
    }

    out->nhex = 0U;
    for (size_t i = 0; i < 4U; i++) {
        out->words[i] = 0ULL;
    }

    const unsigned char* p = (const unsigned char*)input;
    while (*p != '\0') {
        while (*p != '\0' && isspace(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            if (*p == '\0' || isspace(*p)) {
                return DSD_VENDOR_AP_KEY_INVALID;
            }
        }

        while (*p != '\0' && !isspace(*p)) {
            const int nibble = dsd_vendor_ap_key_nibble((int)*p);
            if (nibble < 0) {
                return DSD_VENDOR_AP_KEY_INVALID;
            }
            if (out->nhex >= sizeof(out->hex)) {
                return DSD_VENDOR_AP_KEY_BAD_LENGTH;
            }
            out->hex[out->nhex++] = dsd_vendor_ap_key_hex_char(nibble);
            p++;
        }
    }

    if (out->nhex != 32U && out->nhex != 64U) {
        return DSD_VENDOR_AP_KEY_BAD_LENGTH;
    }

    const size_t word_count = out->nhex / 16U;
    for (size_t word = 0; word < word_count; word++) {
        uint64_t value = 0ULL;
        for (size_t i = 0; i < 16U; i++) {
            const int nibble = dsd_vendor_ap_key_nibble((int)out->hex[(word * 16U) + i]);
            if (nibble < 0) {
                return DSD_VENDOR_AP_KEY_INVALID;
            }
            value = (value << 4U) | (uint64_t)nibble;
        }
        out->words[word] = value;
    }

    return DSD_VENDOR_AP_KEY_OK;
}

static inline int
dsd_vendor_ap_key_hex_to_bytes(const unsigned char* hex, size_t nhex, unsigned char* out, size_t out_len) {
    if (hex == NULL || out == NULL || nhex != (out_len * 2U)) {
        return -1;
    }

    for (size_t i = 0; i < out_len; i++) {
        const int hi = dsd_vendor_ap_key_nibble((int)hex[i * 2U]);
        const int lo = dsd_vendor_ap_key_nibble((int)hex[(i * 2U) + 1U]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

#endif
