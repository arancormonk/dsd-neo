// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Core audio API surface for DSD-neo.
 *
 * Exposes device open/close helpers, drain/flush routines, and playback
 * helpers shared across OSS/Pulse/PCM paths. Kept separate from dsd.h so
 * modules that only need audio APIs can avoid pulling in the full core header.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for PulseAudio types used in callbacks.
typedef struct pa_context pa_context;
typedef struct pa_sink_info pa_sink_info;
typedef struct pa_source_info pa_source_info;

// Field list is here: http://0pointer.de/lennart/projects/pulseaudio/doxygen/structpa__sink__info.html
typedef struct pa_devicelist {
    uint8_t initialized;
    char name[512];
    uint32_t index;
    char description[256];
} pa_devicelist_t;

/** @brief Process one block of dPMR voice through the decoder/synth path. */
void processdPMRvoice(dsd_opts* opts, dsd_state* state);
/** @brief Core audio processing entry point (slot 1). */
void processAudio(dsd_opts* opts, dsd_state* state);
/** @brief Core audio processing entry point (slot 2 / right). */
void processAudioR(dsd_opts* opts, dsd_state* state);

/** @brief Open PulseAudio input device based on opts. */
void openPulseInput(dsd_opts* opts);
/** @brief Open PulseAudio output device based on opts. */
void openPulseOutput(dsd_opts* opts);
/** @brief Open OSS output device based on opts. */
void openOSSOutput(dsd_opts* opts);
/** @brief Close PulseAudio input device if open. */
void closePulseInput(dsd_opts* opts);
/** @brief Close PulseAudio output device if open. */
void closePulseOutput(dsd_opts* opts);

/** @brief Best-effort drain of audio output buffers (Pulse/OSS). Safe no-op when disabled. */
void dsd_drain_audio_output(dsd_opts* opts);

/** @brief Write synthesized mono voice samples for slot 1. */
void writeSynthesizedVoice(dsd_opts* opts, dsd_state* state);
/** @brief Write synthesized mono voice samples for slot 2. */
void writeSynthesizedVoiceR(dsd_opts* opts, dsd_state* state);
/** @brief Write synthesized mono mixed voice samples. */
void writeSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state);

/** @brief Play synthesized voice (float stereo mix) for slot 1. */
void playSynthesizedVoiceFS(dsd_opts* opts, dsd_state* state); // float stereo mix
/** @brief Play synthesized voice (float stereo mix 3v2 DMR). */
void playSynthesizedVoiceFS3(dsd_opts* opts, dsd_state* state); // float stereo mix 3v2 DMR
/** @brief Play synthesized voice (float stereo mix 4v2 P25p2). */
void playSynthesizedVoiceFS4(dsd_opts* opts, dsd_state* state); // float stereo mix 4v2 P25p2
/** @brief Play synthesized voice (float mono). */
void playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state); // float mono

/** @brief Play synthesized voice (short mono output slot 1). */
void playSynthesizedVoice(dsd_opts* opts, dsd_state* state); // short mono output slot 1
/** @brief Play synthesized voice (short mono output slot 2). */
void playSynthesizedVoiceR(dsd_opts* opts, dsd_state* state); // short mono output slot 2
/** @brief Play synthesized voice (short mono mix). */
void playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state); // short mono mix
/** @brief Play synthesized voice (short mono mix for OSS 48k I/O). */
void playSynthesizedVoiceMSR(dsd_opts* opts, dsd_state* state); // short mono mix R (OSS 48k input/output)
/** @brief Play synthesized voice (short stereo mix). */
void playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state); // short stereo mix
/** @brief Play synthesized voice (short stereo mix 3v2 DMR). */
void playSynthesizedVoiceSS3(dsd_opts* opts, dsd_state* state); // short stereo mix 3v2 DMR
/** @brief Play synthesized voice (short stereo mix 4v2 P25p2). */
void playSynthesizedVoiceSS4(dsd_opts* opts, dsd_state* state); // short stereo mix 4v2 P25p2
/** @brief Play synthesized voice (short stereo mix 18V superframe). */
void playSynthesizedVoiceSS18(dsd_opts* opts, dsd_state* state); // short stereo mix 18V Superframe

/** @brief Apply float-domain gain to 160-sample block for given slot. */
void agf(dsd_opts* opts, dsd_state* state, float samp[160], int slot); // float gain control
/** @brief Apply short-domain gain to buffer of given length. */
void agsm(dsd_opts* opts, dsd_state* state, short* input, int len); // short gain control
/** @brief Apply manual analog gain to short buffer. */
void analog_gain(dsd_opts* opts, dsd_state* state, short* input, int len); // manual gain for analog paths

/** @brief Multiply float buffer by gain factor in-place. */
void audio_apply_gain_f32(float* buf, size_t n, float gain);
/** @brief Multiply int16 buffer by gain factor in-place. */
void audio_apply_gain_s16(short* buf, size_t n, float gain);

/** @brief Simple legacy 8k→48k upsampler (6:1). */
void upsampleS(short invalue, short prev, short outbuf[6]);

/** @brief Convert float samples to int16 with scaling. */
void audio_float_to_s16(const float* in, short* out, size_t n, float scale);
/** @brief Convert int16 samples to float with scaling. */
void audio_s16_to_float(const short* in, float* out, size_t n, float scale);
/** @brief Duplicate mono float samples into interleaved stereo buffer. */
void audio_mono_to_stereo_f32(const float* in, float* out, size_t n);
/** @brief Duplicate mono int16 samples into interleaved stereo buffer. */
void audio_mono_to_stereo_s16(const short* in, short* out, size_t n);

/** @brief Mix two float channels with per-channel mute flags into interleaved stereo. */
void audio_mix_interleave_stereo_f32(const float* left, const float* right, size_t n, int encL, int encR,
                                     float* stereo_out);
/** @brief Mix two int16 channels with per-channel mute flags into interleaved stereo. */
void audio_mix_interleave_stereo_s16(const short* left, const short* right, size_t n, int encL, int encR,
                                     short* stereo_out);
/** @brief Mix two float channels with mute flags into mono output. */
void audio_mix_mono_from_slots_f32(const float* left, const float* right, size_t n, int l_on, int r_on,
                                   float* mono_out);

/** @brief Simple P25 P2 per-slot mixer gate used by tests (maps p25_p2_audio_allowed -> enc flags). */
int dsd_p25p2_mixer_gate(const dsd_state* state, int* encL, int* encR);

/** @brief Talkgroup/whitelist/TG-hold gating for mono mix (enc flags 0=unmuted,1=muted). */
int dsd_audio_group_gate_mono(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int enc_in, int* enc_out);
/** @brief Talkgroup/whitelist/TG-hold gating for dual/slot mix (enc flags 0=unmuted,1=muted). */
int dsd_audio_group_gate_dual(const dsd_opts* opts, const dsd_state* state, unsigned long tgL, unsigned long tgR,
                              int encL_in, int encR_in, int* encL_out, int* encR_out);

/** @brief Open output audio device (Pulse/OSS) at requested speed. */
void openAudioOutDevice(dsd_opts* opts, int speed);
/** @brief Open input audio device (Pulse/OSS) based on opts. */
void openAudioInDevice(dsd_opts* opts);

/** @brief Parse Pulse input device string (after 'pulse:' prefix) and update opts. */
void parse_pulse_input_string(dsd_opts* opts, char* input);
/** @brief Parse Pulse output device string (after 'pulse:' prefix) and update opts. */
void parse_pulse_output_string(dsd_opts* opts, char* input);
/** @brief PulseAudio context state callback used during device discovery. */
void pa_state_cb(pa_context* c, void* userdata);
/** @brief PulseAudio sink enumeration callback. */
void pa_sinklist_cb(pa_context* c, const pa_sink_info* l, int eol, void* userdata);
/** @brief PulseAudio source enumeration callback. */
void pa_sourcelist_cb(pa_context* c, const pa_source_info* l, int eol, void* userdata);
/** @brief Populate input/output Pulse device lists. */
int pa_get_devicelist(pa_devicelist_t* input, pa_devicelist_t* output);
/** @brief Print available Pulse devices to stdout. */
int pulse_list(void);

#ifdef __cplusplus
}
#endif
