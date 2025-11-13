// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Core audio API surface for DSD-neo.
 *
 * Exposes device open/close helpers, drain/flush routines, and playback
 * helpers shared across OSS/Pulse/PCM paths. Kept separate from dsd.h so
 * modules that only need audio APIs can avoid pulling in the full core header.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

// Field list is here: http://0pointer.de/lennart/projects/pulseaudio/doxygen/structpa__sink__info.html
typedef struct pa_devicelist {
    uint8_t initialized;
    char name[512];
    uint32_t index;
    char description[256];
} pa_devicelist_t;

// Basic audio processing pipeline helpers
void processdPMRvoice(dsd_opts* opts, dsd_state* state);
void processAudio(dsd_opts* opts, dsd_state* state);
void processAudioR(dsd_opts* opts, dsd_state* state);

// Device open/close helpers (Pulse/OSS)
void openPulseInput(dsd_opts* opts);
void openPulseOutput(dsd_opts* opts);
void openOSSOutput(dsd_opts* opts);
void closePulseInput(dsd_opts* opts);
void closePulseOutput(dsd_opts* opts);

// Best-effort drain of audio output buffers (Pulse/OSS). Safe no-op when disabled.
void dsd_drain_audio_output(dsd_opts* opts);

// Synthesized voice writers (file/ring level)
void writeSynthesizedVoice(dsd_opts* opts, dsd_state* state);
void writeSynthesizedVoiceR(dsd_opts* opts, dsd_state* state);
void writeSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state);

// Float-path playback helpers
void playSynthesizedVoiceFS(dsd_opts* opts, dsd_state* state);  // float stereo mix
void playSynthesizedVoiceFS3(dsd_opts* opts, dsd_state* state); // float stereo mix 3v2 DMR
void playSynthesizedVoiceFS4(dsd_opts* opts, dsd_state* state); // float stereo mix 4v2 P25p2
void playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state);  // float mono

// Short-path playback helpers
void playSynthesizedVoice(dsd_opts* opts, dsd_state* state);     // short mono output slot 1
void playSynthesizedVoiceR(dsd_opts* opts, dsd_state* state);    // short mono output slot 2
void playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state);   // short mono mix
void playSynthesizedVoiceMSR(dsd_opts* opts, dsd_state* state);  // short mono mix R (OSS 48k input/output)
void playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state);   // short stereo mix
void playSynthesizedVoiceSS3(dsd_opts* opts, dsd_state* state);  // short stereo mix 3v2 DMR
void playSynthesizedVoiceSS4(dsd_opts* opts, dsd_state* state);  // short stereo mix 4v2 P25p2
void playSynthesizedVoiceSS18(dsd_opts* opts, dsd_state* state); // short stereo mix 18V Superframe

// Gain helpers
void agf(dsd_opts* opts, dsd_state* state, float samp[160], int slot);     // float gain control
void agsm(dsd_opts* opts, dsd_state* state, short* input, int len);        // short gain control
void analog_gain(dsd_opts* opts, dsd_state* state, short* input, int len); // manual gain for analog paths

// Simple 8k→48k upsampler (legacy)
void upsampleS(short invalue, short prev, short outbuf[6]);

// Audio device open helpers
void openAudioOutDevice(dsd_opts* opts, int speed);
void openAudioInDevice(dsd_opts* opts);

// Pulse runtime control helpers
void parse_pulse_input_string(dsd_opts* opts, char* input);
void parse_pulse_output_string(dsd_opts* opts, char* input);
void pa_state_cb(pa_context* c, void* userdata);
void pa_sinklist_cb(pa_context* c, const pa_sink_info* l, int eol, void* userdata);
void pa_sourcelist_cb(pa_context* c, const pa_source_info* l, int eol, void* userdata);
int pa_get_devicelist(pa_devicelist_t* input, pa_devicelist_t* output);
int pulse_list(void);
