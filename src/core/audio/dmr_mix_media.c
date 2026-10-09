// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Decoded media staged per DMR slot for the stereo mixes' audible-audio stamp (issue #574): see
   dsd_audio_dmr_mix_media_staged() in <dsd-neo/core/audio.h>. Its own unit, so the reset paths that clear it link
   without the mixers. */

#include <dsd-neo/core/audio.h>

// DSD_DMR_MIX_MEDIA_* bits per slot. Decoder thread only, like the vocoder and the mixes.
static unsigned int s_dmr_mix_media[2];

void
dsd_audio_dmr_mix_media_staged(int slot, unsigned int kind) {
    if (slot == 0 || slot == 1) {
        s_dmr_mix_media[slot] |= kind;
    }
}

void
dsd_audio_dmr_mix_media_silenced(int slot, unsigned int kind) {
    if (slot == 0 || slot == 1) {
        s_dmr_mix_media[slot] &= ~kind;
    }
}

void
dsd_audio_dmr_mix_media_discard(int slot) {
    if (slot == 0 || slot == 1) {
        s_dmr_mix_media[slot] = 0U;
    }
}

unsigned int
dsd_audio_dmr_mix_media_take(int slot) {
    if (slot != 0 && slot != 1) {
        return 0U;
    }
    const unsigned int media = s_dmr_mix_media[slot];
    s_dmr_mix_media[slot] = 0U;
    return media;
}
