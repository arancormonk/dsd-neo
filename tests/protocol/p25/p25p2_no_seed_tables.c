// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test support for frame tests that stand in for the whole Reed-Solomon bridge (src/fec/ez.cpp): scrambling code
 * recovery (issue #804), which processP2() runs, reads the seed tables and decodes with the fixed erasures. Without
 * tables it does nothing, so those tests exercise the dispatch and their own stand-in decoders alone.
 */

#include <dsd-neo/fec/ez.h>
#include <stddef.h>

const ez_p25p2_seed_tables*
ez_p25p2_seed_tables_get(void) {
    return NULL;
}

int
ez_rs28_facch_fixed(int payload[156], int parity[114]) {
    (void)payload;
    (void)parity;
    return -1;
}

int
ez_rs28_sacch_fixed(int payload[180], int parity[132]) {
    (void)payload;
    (void)parity;
    return -1;
}
