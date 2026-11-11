// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Module-private parts of the digital voice chain (<dsd-neo/core/voice_chain.h>): its speech level stage
   (voice_level.c), and a test seam that shows what each producer tells the chain about its frames. The seam is in the
   shipped core, as the TETRA codec seam is, because the producers' tests link the core library; unset, it costs one
   pointer test per frame. */

#ifndef DSD_NEO_SRC_CORE_AUDIO_VOICE_CHAIN_INTERNAL_H_
#define DSD_NEO_SRC_CORE_AUDIO_VOICE_CHAIN_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/voice_chain.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The reception a frame belongs to, as the chain reads it from the call state: the slot's transmission
   (dsd_call_snapshot_transmission() of its active call) and the protocol family; 0 for none known. */
typedef struct {
    uint64_t transmission;
    int family;
} dsd_voice_reception;

/* The chain's speech level stage for slot @p slot (0 or 1): follow @p reception and the frame's talkspurt, then apply
   the slot's speech AGC to the frame in place and leave its gain in dB in aout_gain/aout_gainR. */
void dsd_voice_level_run(const dsd_opts* opts, dsd_state* state, int slot, float* frame,
                         const dsd_voice_frame_info* info, const dsd_voice_reception* reception);

/* Called by dsd_voice_chain_run() for every frame it takes, before it touches the frame: the slot, the frame's
   DSD_VOICE_FRAME_SAMPLES samples as the producer made them, and its info (NULL when the producer gave none). */
typedef void (*dsd_voice_chain_observer_fn)(void* ctx, int slot, const float* frame, const dsd_voice_frame_info* info);

/* Install @p fn with @p ctx; NULL removes it. Decoder thread only, like the chain. */
void dsd_voice_chain_set_observer_for_test(dsd_voice_chain_observer_fn fn, void* ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSD_NEO_SRC_CORE_AUDIO_VOICE_CHAIN_INTERNAL_H_ */
