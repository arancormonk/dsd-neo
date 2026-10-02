// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_FINITE_H
#define DSD_NEO_IO_RADIO_RTL_FINITE_H

namespace dsd {
namespace io {
namespace radio {

/*
 * std::isfinite() for the radio sources that keep the global fast-math option. Under fast-math the compiler may
 * assume every value is finite and fold std::isfinite() to true; this one lives in its own translation unit built
 * with IEEE semantics, so the test survives. It is an out-of-line call: use it on per-block, per-retune or
 * per-setting values, never inside a per-sample loop.
 */
bool rtl_is_finite(double value);

} // namespace radio
} // namespace io
} // namespace dsd

#endif /* DSD_NEO_IO_RADIO_RTL_FINITE_H */
