// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN data-call CRC32 (MSB-first, init all-ones, no final xor) unit checks.
 */

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Link stubs:
 * nxdn_message_crc32 currently lives in nxdn_deperm.c, so this test only
 * provides minimal symbols required to link that object safely.
 */
uint64_t
ConvertBitIntoBytes(uint8_t* bits, uint32_t n) {
    uint64_t v = 0ULL;
    for (uint32_t i = 0; i < n; i++) {
        v = (v << 1U) | (uint64_t)(bits[i] & 1U);
    }
    return v;
}

uint64_t
convert_bits_into_output(uint8_t* input, int len) {
    if (len <= 0) {
        return 0ULL;
    }
    return ConvertBitIntoBytes(input, (uint32_t)len);
}

void
CNXDNConvolution_start(void) {}

void
CNXDNConvolution_decode(uint8_t s0, uint8_t s1) {
    (void)s0;
    (void)s1;
}

void
CNXDNConvolution_decode_soft(uint8_t s0, uint8_t s1, uint8_t r0, uint8_t r1) {
    (void)s0;
    (void)s1;
    (void)r0;
    (void)r1;
}

void
CNXDNConvolution_chainback(unsigned char* out, unsigned int nBits) {
    (void)nBits;
    if (out != NULL) {
        memset(out, 0, 32U);
    }
}

void
NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, uint8_t CrcCorrect, uint8_t* ElementsContent,
                             size_t elements_bits) {
    (void)opts;
    (void)state;
    (void)CrcCorrect;
    (void)ElementsContent;
    (void)elements_bits;
}

void
NXDN_SACCH_Full_decode(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
NXDN_decode_scch(dsd_opts* opts, dsd_state* state, uint8_t* Message, uint8_t direction) {
    (void)opts;
    (void)state;
    (void)Message;
    (void)direction;
}

void
nxdn_alias_reset(dsd_state* state) {
    (void)state;
}

int
nxdn_dcr_decode_csm_alias(const uint8_t trellis_bits[96], char* out, size_t out_sz) {
    (void)trellis_bits;
    if (out != NULL && out_sz > 0U) {
        out[0] = '\0';
    }
    return 0;
}

uint8_t
nxdn_scch_crc7_check_from_trellis(const uint8_t trellis_bits[32]) {
    (void)trellis_bits;
    return 0U;
}

void
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
trellis_decode(uint8_t result[], const uint8_t source[], int result_len) {
    (void)source;
    if (result != NULL && result_len > 0) {
        memset(result, 0, (size_t)result_len * sizeof(uint8_t));
    }
}

static void
bytes_to_bits(const uint8_t* input, int nbytes, uint8_t* output) {
    memset(output, 0, (size_t)(nbytes * 8) * sizeof(uint8_t));
    for (int i = 0; i < nbytes; i++) {
        uint8_t b = input[i];
        int base = i * 8;
        for (int j = 0; j < 8; j++) {
            output[base + j] = (uint8_t)((b >> (7 - j)) & 1U);
        }
    }
}

static int
expect_u32(const char* tag, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got %08X want %08X\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    {
        static const uint8_t msg[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        uint8_t bits[sizeof(msg) * 8];
        bytes_to_bits(msg, (int)sizeof(msg), bits);
        uint32_t crc = nxdn_message_crc32(bits, (int)(sizeof(msg) * 8));
        rc |= expect_u32("crc-123456789", crc, 0x0376E6E7U);
    }

    {
        static const uint8_t msg[] = {'A', 'R', 'I', 'B', 'T', 'E', 'S', 'T'};
        uint8_t bits[sizeof(msg) * 8];
        bytes_to_bits(msg, (int)sizeof(msg), bits);
        uint32_t crc = nxdn_message_crc32(bits, (int)(sizeof(msg) * 8));
        rc |= expect_u32("crc-aribtest", crc, 0x84201F67U);
    }

    rc |= expect_u32("crc-len0", nxdn_message_crc32((uint8_t*)"ignored", 0), 0xFFFFFFFFU);
    rc |= expect_u32("crc-null", nxdn_message_crc32(NULL, 8), 0xFFFFFFFFU);

    return rc;
}
