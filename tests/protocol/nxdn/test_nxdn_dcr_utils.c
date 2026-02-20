// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused tests for DCR/NXDN helper utilities in nxdn_deperm.
 */

#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void
write_bits_u8(uint8_t* bits, size_t start, uint8_t value, size_t nbits) {
    for (size_t i = 0U; i < nbits; i++) {
        size_t shift = nbits - 1U - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static int
expect_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    {
        uint8_t trellis_bits[32];
        memset(trellis_bits, 0, sizeof(trellis_bits));

        write_bits_u8(trellis_bits, 25U, 0x55U, 7U);
        rc |= expect_u8("scch-crc7-pattern-55", nxdn_scch_crc7_check_from_trellis(trellis_bits), 0x55U);

        write_bits_u8(trellis_bits, 25U, 0x7FU, 7U);
        rc |= expect_u8("scch-crc7-pattern-7f", nxdn_scch_crc7_check_from_trellis(trellis_bits), 0x7FU);
    }

    {
        uint8_t trellis_bits[96];
        char out[32];
        memset(trellis_bits, 0, sizeof(trellis_bits));
        memset(out, 0, sizeof(out));

        for (size_t i = 0U; i < 9U; i++) {
            write_bits_u8(trellis_bits, i * 4U, (uint8_t)(i + 1U), 4U);
        }
        rc |= expect_int("dcr-csm-decode-ok", nxdn_dcr_decode_csm_alias(trellis_bits, out, sizeof(out)), 1);
        rc |= expect_str("dcr-csm-decode-value", out, "CSM 123456789");
    }

    {
        uint8_t trellis_bits[96];
        char out[8];
        memset(trellis_bits, 0, sizeof(trellis_bits));
        snprintf(out, sizeof(out), "%s", "busy");

        for (size_t i = 0U; i < 9U; i++) {
            write_bits_u8(trellis_bits, i * 4U, (uint8_t)(i + 1U), 4U);
        }
        rc |= expect_int("dcr-csm-decode-small-buffer", nxdn_dcr_decode_csm_alias(trellis_bits, out, sizeof(out)), 0);
        rc |= expect_int("dcr-csm-decode-small-buffer-clears-out", out[0], '\0');
    }

    {
        uint8_t trellis_bits[96];
        char out[32];
        memset(trellis_bits, 0, sizeof(trellis_bits));
        snprintf(out, sizeof(out), "%s", "unchanged");

        write_bits_u8(trellis_bits, 0U, 0xAU, 4U);
        rc |= expect_int("dcr-csm-decode-invalid", nxdn_dcr_decode_csm_alias(trellis_bits, out, sizeof(out)), 0);
        rc |= expect_int("dcr-csm-decode-invalid-clears-out", out[0], '\0');
    }

    if (rc == 0) {
        printf("NXDN_DCR_UTILS: OK\n");
    }
    return rc;
}
