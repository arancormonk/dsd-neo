// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Core audio API surface for DSD-neo.
 *
 * Exposes device open/close helpers, drain/flush routines, and playback
 * helpers shared across audio backends. Kept separate so
 * modules that only need audio APIs can avoid pulling in the full core header.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/sndfile_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Process one block of dPMR voice through the decoder/synth path. */
void processdPMRvoice(dsd_opts* opts, dsd_state* state);
/** @brief Core audio processing entry point (slot 1). */
void processAudio(dsd_opts* opts, dsd_state* state);
/** @brief Core audio processing entry point (slot 2 / right). */
void processAudioR(dsd_opts* opts, dsd_state* state);

/** @brief Open audio input stream based on opts. Returns 0 on success. */
int openAudioInput(dsd_opts* opts);
/** @brief Open audio output stream based on opts. Returns 0 on success. */
int openAudioOutput(dsd_opts* opts);
/** @brief Close audio input stream if open. */
void closeAudioInput(dsd_opts* opts);
/** @brief Close audio output stream if open. */
void closeAudioOutput(dsd_opts* opts);

/** @brief Best-effort drain of audio output buffers. Safe no-op when disabled. */
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
/** @brief Apply float-domain auto gain control for analog monitor path. */
void agsm_f(dsd_opts* opts, dsd_state* state, float* input, int len); // float gain control for analog
/** @brief Apply manual analog gain to short buffer. */
void analog_gain(dsd_opts* opts, dsd_state* state, short* input, int len); // manual gain for analog paths
/** @brief Apply manual analog gain to float buffer. */
void analog_gain_f(dsd_opts* opts, dsd_state* state, float* input, int len); // float manual gain for analog

/** @brief Multiply float buffer by gain factor in-place. */
void audio_apply_gain_f32(float* buf, size_t n, float gain);
/** @brief Multiply int16 buffer by gain factor in-place. */
void audio_apply_gain_s16(short* buf, size_t n, float gain);

/** @brief Simple legacy 8k→48k upsampler (6:1) with linear interpolation. */
void upsampleS(short invalue, short prev, short outbuf[6]);
/** @brief Float 8k→48k upsampler (6:1) with linear interpolation. */
void upsampleF(float invalue, float prev, float outbuf[6]);
/** @brief Legacy analog monitor 6x upsampler (sample repetition). */
void upsample(dsd_state* state, float invalue);

/**
 * @brief Generate one linear interpolation block that ends on the current sample.
 *
 * This legacy helper is retained for compatibility/tests. Low-rate PCM input
 * staging now uses the FIR/polyphase resampler in `dsd-neo/dsp/resampler.h`.
 *
 * @param previous Previous input sample.
 * @param current Current input sample.
 * @param factor Number of output samples to generate.
 * @param out Destination buffer.
 * @param out_cap Destination capacity in samples.
 * @return Number of samples written, or 0 on invalid arguments.
 */
size_t dsd_audio_linear_upsample_block_f32(float previous, float current, size_t factor, float* out, size_t out_cap);

/**
 * @brief Rescale decoder timing/filter state between two effective PCM rates.
 *
 * The helper preserves the existing symbol-center fraction when possible and
 * rebuilds analog filter coefficients for the new rate.
 *
 * @param state Decoder state to update.
 * @param old_rate_hz Previous effective PCM rate.
 * @param new_rate_hz New effective PCM rate.
 */
void dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz);

/**
 * @brief Apply a new PCM input sample rate and propagate it into decoder state.
 *
 * Updates wav/interpolator options, clears staged upsample history, and
 * rescales timing/filter state against the provided previous effective rate.
 *
 * @param opts Decoder options to update.
 * @param state Decoder state to update.
 * @param old_effective_rate_hz Previous effective PCM rate.
 * @param sample_rate_hz New raw PCM sample rate.
 */
void dsd_audio_apply_input_sample_rate(dsd_opts* opts, dsd_state* state, int old_effective_rate_hz, int sample_rate_hz);

/**
 * @brief Open a mono PCM input file as either a WAV-family container or legacy raw PCM.
 *
 * `.wav` paths are treated as true WAV containers only when the file starts
 * with a supported WAV-family header such as `RIFF`, `RIFX`, or `RF64`
 * followed by `WAVE`. Headerless captures, including legacy discriminator
 * dumps that merely use a `.wav` suffix, fall back to mono 16-bit
 * little-endian raw PCM at the configured sample rate.
 *
 * @param path Input path to open.
 * @param configured_sample_rate_hz Configured raw PCM sample rate.
 * @param out_file [out] Opened libsndfile handle on success.
 * @param out_info [out] Allocated file metadata on success.
 * @param out_sample_rate_hz [out] Active sample rate selected for the input.
 * @param out_opened_as_container [out] Non-zero when opened via WAV container metadata.
 * @return 0 on success; non-zero on failure.
 */
int dsd_audio_open_mono_file_input(const char* path, int configured_sample_rate_hz, SNDFILE** out_file,
                                   SF_INFO** out_info, int* out_sample_rate_hz, int* out_opened_as_container);

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
/** @brief Return 1 when P25p2 decode should queue audio for the slot under decrypt and media policy. */
int dsd_p25p2_decode_audio_allowed(const dsd_opts* opts, const dsd_state* state, int slot, int alg);

/** @brief Flush partially buffered P25p2 SS18 audio on call end/release. */
void dsd_p25p2_flush_partial_audio(dsd_opts* opts, dsd_state* state);

/** @brief Talkgroup/whitelist/TG-hold gating for mono mix (enc flags 0=unmuted,1=muted). */
int dsd_audio_group_gate_mono(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int enc_in, int* enc_out);
/** @brief Talkgroup/whitelist/TG-hold gating for dual/slot mix (enc flags 0=unmuted,1=muted). */
int dsd_audio_group_gate_dual(const dsd_opts* opts, const dsd_state* state, unsigned long tgL, unsigned long tgR,
                              int encL_in, int encR_in, int* encL_out, int* encR_out);
/** @brief Mono per-call WAV gate combining decrypt state with TG/allow-list/TG-hold policy. */
int dsd_audio_record_gate_mono(const dsd_opts* opts, const dsd_state* state, int* allow_out);

/**
 * @brief Return 1 when a DMR/P25-style voice ALGID has sufficient key material to decrypt.
 *
 * This helper intentionally only covers known/implemented families. Unknown ALGIDs
 * return 0 so callers keep audio muted rather than falsely unmuting garble.
 */
int dsd_dmr_voice_alg_can_decrypt(int algid, unsigned long long r_key, int aes_loaded);

/** @brief Legacy UI beeper helper (used by ncurses call-alert and events). */
void beeper(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len);

/** @brief Open output audio device at requested speed. */
void openAudioOutDevice(dsd_opts* opts, int speed);
/** @brief Open input audio device based on opts. Returns 0 on success. */
int openAudioInDevice(dsd_opts* opts, dsd_state* state);

/** @brief Parse audio input device string and update opts. */
void parse_audio_input_string(dsd_opts* opts, char* input);
/** @brief Parse audio output device string and update opts. */
void parse_audio_output_string(dsd_opts* opts, char* input);

/** @brief Print available audio devices to stdout. */
int audio_list_devices(void);

#ifdef __cplusplus
}
#endif
