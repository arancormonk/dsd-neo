// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Hide a floating-point value's origin from the optimizer before testing it for NaN or infinity.
 *
 * A NaN or infinity check that must survive the global fast-math option lives in a translation unit built with IEEE
 * semantics. With link-time optimization, Clang can still inline such a function into a fast-math caller, which marks
 * its arguments "never NaN or infinite", and fold the check away. Passing the argument through dsd_fp_opaque_f() or
 * dsd_fp_opaque_d() first ends that inference: the value makes a round trip through a volatile object, whose load the
 * compiler can neither skip nor know the result of. That costs one store and one load, so use it on per-symbol,
 * per-block or per-setting values, not in a per-sample loop.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_FP_OPAQUE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_FP_OPAQUE_H_

#ifdef __cplusplus
extern "C" {
#endif

static inline float
dsd_fp_opaque_f(float v) {
    volatile float opaque = v;
    return opaque;
}

static inline double
dsd_fp_opaque_d(double v) {
    volatile double opaque = v;
    return opaque;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_FP_OPAQUE_H_ */
