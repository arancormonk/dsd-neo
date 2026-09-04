// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/fec/dmr_late_entry.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/block_codes.h>
#include <stddef.h>

static uint64_t
dmr_late_entry_pack_fragments(const uint64_t* fragments, unsigned int vc_base) {
    uint64_t packed = 0;
    for (unsigned int col = 0; col < 3U; col++) {
        for (unsigned int row = 0; row < 3U; row++) {
            packed = (packed << 4U) | (fragments[((vc_base + row) * 3U) + col] & 0x0FU);
        }
    }
    return packed;
}

uint8_t
dsd_dmr_crc4(const uint8_t* bits, unsigned int len) {
    static const uint8_t polynomial[5] = {1, 0, 0, 1, 1};
    uint8_t work[256];
    if (!bits || len > sizeof(work) - 4U) {
        return 0;
    }
    DSD_MEMSET(work, 0, sizeof(work));
    DSD_MEMCPY(work, bits, len);

    for (unsigned int i = 0; i < len; i++) {
        if (work[i] == 0U) {
            continue;
        }
        for (unsigned int j = 0; j < 5U; j++) {
            work[i + j] ^= polynomial[j];
        }
    }

    uint8_t crc = 0;
    for (unsigned int i = 0; i < 4U; i++) {
        crc = (uint8_t)((crc << 1U) | work[len + i]);
    }
    return (uint8_t)(crc ^ 0x0FU);
}

bool
dsd_dmr_late_entry_decode(const uint64_t* fragments, dsd_dmr_late_entry_result* result) {
    if (!fragments || !result) {
        return false;
    }

    const uint64_t mi_codewords = dmr_late_entry_pack_fragments(fragments, 1U);
    const uint64_t parity_codewords = dmr_late_entry_pack_fragments(fragments, 4U);
    uint8_t mi_bits[36] = {0};
    bool all_golay_pass = true;

    for (unsigned int triplet = 0; triplet < 3U; triplet++) {
        unsigned char codeword[24] = {0};
        for (unsigned int bit = 0; bit < 12U; bit++) {
            const unsigned int shift = bit + (triplet * 12U);
            codeword[bit] = (unsigned char)(((mi_codewords << shift) & 0x800000000ULL) >> 35U);
            codeword[bit + 12U] = (unsigned char)(((parity_codewords << shift) & 0x800000000ULL) >> 35U);
        }
        if (!Golay_24_12_decode(codeword)) {
            all_golay_pass = false;
        }
        DSD_MEMCPY(&mi_bits[(size_t)triplet * 12U], codeword, 12U);
    }

    uint32_t mi = 0;
    for (unsigned int i = 0; i < 32U; i++) {
        mi = (mi << 1U) | mi_bits[i];
    }
    uint8_t extracted_crc = 0;
    for (unsigned int i = 32U; i < 36U; i++) {
        extracted_crc = (uint8_t)((extracted_crc << 1U) | mi_bits[i]);
    }

    result->mi = mi;
    result->crc_ok = extracted_crc == dsd_dmr_crc4(mi_bits, 32U);
    return all_golay_pass;
}
