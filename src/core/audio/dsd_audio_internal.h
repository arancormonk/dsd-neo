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
#include <stddef.h>

void dsd_audio_write_wav_short_block(SNDFILE* file, const short* samples, sf_count_t sample_count, const char* context);

/* dsd_audio2.c output primitives the mixers share with the P25 Phase 2 playout (p25p2_playout.c, issue #651). */
void dsd_output_float_block(dsd_opts* opts, dsd_state* state, const float* samples, size_t frames, int channels);
void dsd_output_s16_block(dsd_opts* opts, dsd_state* state, const short* samples, size_t frames, int channels);
/* Wrap the vocoder's output buffer pointers before they run past their allocation, as every mixer pass does. */
void dsd_audio_maybe_reset_output_ring_left(dsd_state* state);
void dsd_audio_maybe_reset_output_ring_right(dsd_state* state);
/* Whether an output receives a mix's blocks (issue #574, the audible-audio stamp's output condition). */
int dsd_mix_output_plays(const dsd_opts* opts);
/* Write a 320-sample stereo block to the static WAV with the channels in @p mask (bit 0 left, bit 1 right) silent. */
void dsd_write_masked_stereo_wav_block(const dsd_opts* opts, const short* block, int mask, const char* context);
/* beeper()'s tone, sounded now: the P25 Phase 2 playout sounds an alert it held back with it. */
void dsd_beeper_emit(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len);

/* Mono output verdict for the active slot 0 call, shared by the short mono path
 * and the legacy short output: the talkgroup gate plus the live P25 Phase 1
 * crypto and reverse-mute rule the float and stereo paths apply. 1 = muted. */
int dsd_audio_mono_output_muted(const dsd_opts* opts, const dsd_state* state);

/* Symbol replay pacing, shared by the startup open and the runtime input switch (dsd_audio_input_switch.c): a new
 * input starts unpaced, and a `.bin` symbol capture is replayed at symbol pace with its header read afresh. */
void dsd_audio_reset_symbol_replay_pacing(dsd_state* state);
void dsd_audio_enable_bin_symbol_replay(dsd_state* state);

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
