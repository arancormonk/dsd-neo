// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Test seam of the digital voice chain (<dsd-neo/core/voice_chain.h>): what each producer tells the chain about its
   frames. Module-private. It is in the shipped core, as the TETRA codec seam is, because the producers' tests link the
   core library; unset, it costs one pointer test per frame. */

#ifndef DSD_NEO_SRC_CORE_AUDIO_VOICE_CHAIN_INTERNAL_H_
#define DSD_NEO_SRC_CORE_AUDIO_VOICE_CHAIN_INTERNAL_H_

#include <dsd-neo/core/voice_chain.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by dsd_voice_chain_run() for every frame it takes, before it touches the frame: the slot, the frame's
   DSD_VOICE_FRAME_SAMPLES samples as the producer made them, and its info (NULL when the producer gave none). */
typedef void (*dsd_voice_chain_observer_fn)(void* ctx, int slot, const float* frame, const dsd_voice_frame_info* info);

/* Install @p fn with @p ctx; NULL removes it. Decoder thread only, like the chain. */
void dsd_voice_chain_set_observer_for_test(dsd_voice_chain_observer_fn fn, void* ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSD_NEO_SRC_CORE_AUDIO_VOICE_CHAIN_INTERNAL_H_ */
