// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* BPTC(16,2), the single-burst and reverse-channel code (ETSI TS 102 361-1 B.2.2.2): a Hamming(16,11,4) row the
 * decoder cannot correct leaves the output as received, and the call reports the failure.
 *
 * The Hamming decoder leaves its output unwritten when it fails. The extractor used to copy that output anyway, so the
 * 11 data bits came from whatever its stack held: the burst's printed payload and CRC changed from run to run, and the
 * bits were nothing the radio sent. Each case below runs after the stack was filled with ones and again with zeros, so
 * a leftover read shows up as output bits the input does not have. */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_MSC_VER)
#define TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define TEST_NOINLINE __attribute__((noinline))
#else
#define TEST_NOINLINE
#endif

/* Fill a stretch of stack below the caller with @p value, where the extractor's locals will sit next. */
static TEST_NOINLINE void
poison_stack(uint8_t value) {
    volatile uint8_t scratch[4096];
    for (size_t i = 0; i < sizeof scratch; i++) {
        scratch[i] = value;
    }
    (void)scratch[0];
}

static TEST_NOINLINE uint32_t
extract_after_poison(uint8_t value, uint8_t in[32], uint8_t out[32], uint32_t odd) {
    poison_stack(value);
    return BPTC_16x2_Extract_Data(in, out, odd);
}

static int
ones(const uint8_t bits[32]) {
    int n = 0;
    for (int i = 0; i < 32; i++) {
        n += bits[i] & 1;
    }
    return n;
}

/* Whether input bit @p a lands in the Hamming(16,11,4) row: alone, it is an error the row corrects. A data bit is then
 * gone from the output; a row parity bit stays where it was received (the row's parity is not copied back), among the
 * first 16. A bit of the parity row stays there, past them. */
static int
in_hamming_row(int a) {
    uint8_t in[32] = {0};
    uint8_t out[32];
    in[a] = 1;
    (void)BPTC_16x2_Extract_Data(in, out, 1);
    for (int i = 16; i < 32; i++) {
        if (out[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* Two input bits in the Hamming row: two errors, which Hamming(16,11,4) detects and cannot correct. The call reports
 * the failure, and the output is the input deinterleaved, nothing else: exactly two ones, whatever the stack held. */
static void
test_failed_row_keeps_the_received_bits(void) {
    int row_bits = 0;
    int pairs = 0;
    for (int a = 0; a < 32; a++) {
        row_bits += in_hamming_row(a);
    }
    assert(row_bits == 16);
    for (int a = 0; a < 32; a++) {
        for (int b = a + 1; b < 32; b++) {
            if (!in_hamming_row(a) || !in_hamming_row(b)) {
                continue;
            }
            for (int fill = 0; fill < 2; fill++) {
                for (uint32_t odd = 0; odd < 2; odd++) {
                    uint8_t in[32] = {0};
                    uint8_t out[32];
                    in[a] = 1;
                    in[b] = 1;
                    DSD_MEMSET(out, 0xA5, sizeof out);
                    const uint32_t irr = extract_after_poison(fill ? 0xFF : 0x00, in, out, odd);
                    assert(irr != 0U);
                    for (int i = 0; i < 32; i++) {
                        assert(out[i] == 0 || out[i] == 1);
                    }
                    assert(ones(out) == 2);
                }
            }
            pairs++;
        }
    }
    assert(pairs == 16 * 15 / 2);
}

int
main(void) {
    InitAllFecFunction();
    test_failed_row_keeps_the_received_bits();
    printf("FEC BPTC 16x2 tests passed.\n");
    return 0;
}
