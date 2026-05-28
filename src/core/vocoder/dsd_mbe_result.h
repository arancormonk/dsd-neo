// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_VOCODER_DSD_MBE_RESULT_H_
#define DSD_NEO_SRC_CORE_VOCODER_DSD_MBE_RESULT_H_

#include <mbelib.h>

static inline int
dsd_mbe_ambe49_changed(const char before[49], const char after[49]) {
    if (!before || !after) {
        return 0;
    }
    for (int i = 0; i < 49; i++) {
        if (before[i] != after[i]) {
            return 1;
        }
    }
    return 0;
}

static inline int
dsd_mbe_strip_c0_context_if_ambe_changed(const char before[49], const char after[49], mbe_process_result* result) {
    if (!result || !dsd_mbe_ambe49_changed(before, after)) {
        return 0;
    }

    result->flags &= ~MBE_PROCESS_FLAG_C0_VALID;
    result->c0_errors = 0;
    result->protected_errors = result->total_errors;
    return 1;
}

#endif /* DSD_NEO_SRC_CORE_VOCODER_DSD_MBE_RESULT_H_ */
