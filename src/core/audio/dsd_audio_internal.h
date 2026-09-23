// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO_INTERNAL_H
#define DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO_INTERNAL_H

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <sndfile.h>

void dsd_audio_write_wav_short_block(SNDFILE* file, const short* samples, sf_count_t sample_count, const char* context);

/* Mono output verdict for the active slot 0 call, shared by the short mono path
 * and the legacy short output: the talkgroup gate plus the live P25 Phase 1
 * crypto and reverse-mute rule the float and stereo paths apply. 1 = muted. */
int dsd_audio_mono_output_muted(const dsd_opts* opts, const dsd_state* state);

static inline int
dsd_audio_input_type_uses_async_output(int audio_in_type, int playfiles, const char* audio_in_dev, int m17decoderip) {
    if (playfiles == 1) {
        return 0;
    }

    switch (audio_in_type) {
        case AUDIO_IN_STDIN:
        case AUDIO_IN_WAV:
        case AUDIO_IN_SYMBOL_BIN:
        case AUDIO_IN_SYMBOL_FLT: return 0;
        case AUDIO_IN_NULL: return (m17decoderip == 1 || dsd_opts_audio_in_dev_is_m17udp_spec(audio_in_dev)) ? 1 : 0;
        default: break;
    }

    return 1;
}

#endif /* DSD_NEO_SRC_CORE_AUDIO_DSD_AUDIO_INTERNAL_H */
