// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_MBE_RESULT_CONTEXT_H_
#define DSD_NEO_SRC_CORE_MBE_RESULT_CONTEXT_H_

#include <mbelib-neo/mbelib.h>
#include <stddef.h>

static inline int
dsd_mbe_bits_changed(const char* before, const char* after, size_t len) {
    if (!before || !after) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (before[i] != after[i]) {
            return 1;
        }
    }
    return 0;
}

static inline int
dsd_mbe_strip_ambe_context_if_changed(const char before[49], const char after[49], mbe_process_result* result) {
    if (!result || !dsd_mbe_bits_changed(before, after, 49U)) {
        return 0;
    }

    result->flags &= ~MBE_PROCESS_FLAG_C0_VALID;
    result->c0_errors = 0;
    result->protected_errors = result->total_errors;
    return 1;
}

static inline int
dsd_mbe_strip_imbe_context_if_changed(const char before[88], const char after[88], mbe_process_result* result) {
    if (!result || !dsd_mbe_bits_changed(before, after, 88U)) {
        return 0;
    }

    result->flags &= ~(MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_C4_VALID);
    result->c0_errors = 0;
    result->c4_errors = 0;
    result->protected_errors = result->total_errors;
    return 1;
}

#endif /* DSD_NEO_SRC_CORE_MBE_RESULT_CONTEXT_H_ */
