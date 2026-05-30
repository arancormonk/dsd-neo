// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/protocol/edacs/edacs_bch.h>
#include <stddef.h>
#include <stdint.h>

static void
test_bch_preserves_message_bits(void) {
    static const uint32_t messages[] = {0x0000000U, 0x0000001U, 0x0123456U, 0x0ABCDEFU, 0x0FFFFFFFU};

    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); i++) {
        unsigned long long int codeword = edacs_bch(messages[i]);
        assert(((codeword >> 12) & 0x0FFFFFFFULL) == messages[i]);
        assert((codeword & ~0xFFFFFFFFFFULL) == 0ULL);
    }
}

static void
test_bch_repeatability_after_other_messages(void) {
    unsigned long long int first = edacs_bch(0x0123456U);
    (void)edacs_bch(0x0000000U);
    (void)edacs_bch(0x0FFFFFFFU);
    assert(edacs_bch(0x0123456U) == first);
}

static void
test_bch_ignores_bits_above_message_width(void) {
    assert(edacs_bch(0x0123456U) == edacs_bch(0x10123456U));
}

int
main(void) {
    test_bch_preserves_message_bits();
    test_bch_repeatability_after_other_messages();
    test_bch_ignores_bits_above_message_width();
    return 0;
}
