// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO_INTERNAL_H
#define DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO_INTERNAL_H

#include <dsd-neo/core/opts.h>

static inline int
dsd_audio_input_type_uses_async_output(int audio_in_type, int playfiles) {
    if (playfiles == 1) {
        return 0;
    }

    switch (audio_in_type) {
        case AUDIO_IN_STDIN:
        case AUDIO_IN_WAV:
        case AUDIO_IN_SYMBOL_BIN:
        case AUDIO_IN_SYMBOL_FLT:
        case AUDIO_IN_NULL: return 0;
        default: break;
    }

    return 1;
}

#endif /* DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO_INTERNAL_H */
