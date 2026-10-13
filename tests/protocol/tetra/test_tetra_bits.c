// SPDX-License-Identifier: GPL-3.0-or-later
/* The bounded TETRA bit reader: MSB-first reads, sticky failure past the end, type-2 presence bits and type-3/4
 * element chains (EN 300 392-2 clauses 14.7 and 18.4). */

#include <assert.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <stddef.h>
#include <stdint.h>

static void
test_reads_and_bounds(void) {
    static const uint8_t bits[] = {1, 0, 1, 1, 0, 0, 1, 0, 1};
    tetra_bit_reader r = tetra_bits_reader(bits, 9, 0);
    assert(tetra_bits_read(&r, 4) == 0xBU);
    assert(tetra_bits_left(&r) == 5);
    assert(tetra_bits_read(&r, 5) == 0x5U);
    assert(tetra_bits_ok(&r) && tetra_bits_left(&r) == 0);
    /* Past the end: 0, no advance, and the reader stays failed. */
    assert(tetra_bits_read(&r, 1) == 0U);
    assert(!tetra_bits_ok(&r));
    assert(tetra_bits_read(&r, 0) == 0U && !tetra_bits_ok(&r));

    tetra_bit_reader w = tetra_bits_reader(bits, 9, 0);
    assert(tetra_bits_read(&w, 33) == 0U && !tetra_bits_ok(&w));
    tetra_bit_reader bad = tetra_bits_reader(bits, 9, 10);
    assert(!tetra_bits_ok(&bad));
    tetra_bit_reader none = tetra_bits_reader(NULL, 9, 0);
    assert(!tetra_bits_ok(&none));
}

static void
test_wide_reads(void) {
    uint8_t bits[64];
    for (int i = 0; i < 64; i++) {
        bits[i] = (uint8_t)(i & 1);
    }
    tetra_bit_reader r = tetra_bits_reader(bits, 64, 0);
    assert(tetra_bits_read64(&r, 64) == 0x5555555555555555ULL);
    assert(tetra_bits_ok(&r));
}

static void
test_type34_chain(void) {
    /* M=1, id 0101, length 3, value 101, M=0 */
    static const uint8_t chain[] = {1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0};
    tetra_bit_reader r = tetra_bits_reader(chain, (int)sizeof chain, 0);
    assert(tetra_bits_skip_type34(&r) == 1);
    assert(tetra_bits_left(&r) == 0);

    /* The closing M-bit is missing. */
    tetra_bit_reader cut = tetra_bits_reader(chain, (int)sizeof chain - 1, 0);
    assert(tetra_bits_skip_type34(&cut) == 0 && !tetra_bits_ok(&cut));

    /* A zero-length element is malformed. */
    static const uint8_t zero_len[] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    tetra_bit_reader z = tetra_bits_reader(zero_len, (int)sizeof zero_len, 0);
    assert(tetra_bits_skip_type34(&z) == 0);

    /* No elements: a single M-bit of 0. */
    static const uint8_t empty[] = {0};
    tetra_bit_reader e = tetra_bits_reader(empty, 1, 0);
    assert(tetra_bits_skip_type34(&e) == 1);
}

static void
test_presence(void) {
    static const uint8_t bits[] = {1, 0};
    tetra_bit_reader r = tetra_bits_reader(bits, 2, 0);
    assert(tetra_bits_present(&r) == 1);
    assert(tetra_bits_present(&r) == 0);
    assert(tetra_bits_present(&r) == 0 && !tetra_bits_ok(&r));
}

int
main(void) {
    test_reads_and_bounds();
    test_wide_reads();
    test_type34_chain();
    test_presence();
    return 0;
}
