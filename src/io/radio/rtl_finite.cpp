// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "rtl_finite.h"

#include <cmath>
#include <dsd-neo/platform/fp_opaque.h>

namespace dsd {
namespace io {
namespace radio {

bool
rtl_is_finite(double value) {
    /* Opaque first: LTO can inline this into a fast-math caller that assumes the value finite. */
    return std::isfinite(dsd_fp_opaque_d(value));
}

} // namespace radio
} // namespace io
} // namespace dsd
