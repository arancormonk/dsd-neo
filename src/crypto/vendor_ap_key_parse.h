// SPDX-License-Identifier: ISC
#ifndef DSD_NEO_CRYPTO_VENDOR_AP_KEY_PARSE_H
#define DSD_NEO_CRYPTO_VENDOR_AP_KEY_PARSE_H

#include <ctype.h>
#include <dsd-neo/core/parse.h>
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

static inline unsigned char
dsd_vendor_ap_key_hex_char(int nibble) {
    return (unsigned char)(nibble <= 9 ? ('0' + nibble) : ('A' + (nibble - 10)));
}

static inline void
dsd_vendor_ap_key_reset(dsd_vendor_ap_key* out) {
    out->nhex = 0U;
    for (size_t i = 0; i < 4U; i++) {
        out->words[i] = 0ULL;
    }
}

static inline const unsigned char*
dsd_vendor_ap_key_skip_space(const unsigned char* p) {
    while (*p != '\0' && isspace(*p)) {
        p++;
    }
    return p;
}

static inline int
dsd_vendor_ap_key_parse_hex_run(const unsigned char** cursor, dsd_vendor_ap_key* out) {
    const unsigned char* p = *cursor;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (*p == '\0' || isspace(*p)) {
            return DSD_VENDOR_AP_KEY_INVALID;
        }
    }

    while (*p != '\0' && !isspace(*p)) {
        const int nibble = dsd_hex_nibble_value((int)*p);
        if (nibble < 0) {
            return DSD_VENDOR_AP_KEY_INVALID;
        }
        if (out->nhex >= sizeof(out->hex)) {
            return DSD_VENDOR_AP_KEY_BAD_LENGTH;
        }
        out->hex[out->nhex++] = dsd_vendor_ap_key_hex_char(nibble);
        p++;
    }

    *cursor = p;
    return DSD_VENDOR_AP_KEY_OK;
}

static inline int
dsd_vendor_ap_key_parse_words(dsd_vendor_ap_key* out) {
    const size_t word_count = out->nhex / 16U;
    for (size_t word = 0; word < word_count; word++) {
        uint64_t value = 0ULL;
        if (dsd_parse_hex_u64_n((const char*)out->hex + (word * 16U), 16U, &value) != 0) {
            return DSD_VENDOR_AP_KEY_INVALID;
        }
        out->words[word] = value;
    }

    return DSD_VENDOR_AP_KEY_OK;
}

static inline int
dsd_vendor_ap_key_parse(const char* input, dsd_vendor_ap_key* out) {
    if (input == NULL || out == NULL) {
        return DSD_VENDOR_AP_KEY_INVALID;
    }

    dsd_vendor_ap_key_reset(out);

    const unsigned char* p = (const unsigned char*)input;
    while (*p != '\0') {
        p = dsd_vendor_ap_key_skip_space(p);
        if (*p == '\0') {
            break;
        }
        const int run_rc = dsd_vendor_ap_key_parse_hex_run(&p, out);
        if (run_rc != DSD_VENDOR_AP_KEY_OK) {
            return run_rc;
        }
    }

    if (out->nhex != 32U && out->nhex != 64U) {
        return DSD_VENDOR_AP_KEY_BAD_LENGTH;
    }

    return dsd_vendor_ap_key_parse_words(out);
}

#endif
