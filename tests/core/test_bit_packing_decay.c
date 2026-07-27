// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Negative compile test: this translation unit must NOT build.
 *
 * The array convenience macros take both capacities from sizeof(), which is only
 * meaningful for an array operand. If a pointer slipped through, sizeof() would
 * silently yield the pointer width and the clamp would be wrong in the dangerous
 * direction, so DSD_MUST_BE_ARRAY() turns that into a compile error. CTest builds
 * this target with WILL_FAIL set; a successful build means the guard regressed.
 */

#include <dsd-neo/core/bit_packing.h>
#include <stddef.h>
#include <stdint.h>

static size_t
dsd_test_decay_must_not_compile(const uint8_t* decayed_source, uint8_t* destination) {
    return DSD_UNPACK_ARRAY_TO_BITS(decayed_source, destination, 4U);
}
