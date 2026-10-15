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

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_H_H

#include <dsd-neo/core/key_material.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/sndfile_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode one dPMR FS2 superframe part: CCH, colour code and both TCH groups.
 *
 * @return How many of the two CCH halves passed their CRC-7 (0, 1 or 2). The caller turns
 *         that into a frame verdict: a passing half is the only thing dPMR can offer the
 *         SPS hunt as proof its profile is the right one (#407).
 */
int processdPMRvoice(dsd_opts* opts, dsd_state* state);
/**
 * @brief Run the voice chain on the left frame (`audio_out_temp_buf`, slot 1's state) and stage it for the short
 * mixers: dsd_voice_chain_run() then dsd_voice_stage_short_left() (<dsd-neo/core/voice_chain.h>).
 *
 * In-tree decoders call those two themselves (the chain on every frame, the staging only where the short output takes
 * it); this stays for out-of-tree callers.
 */
void processAudio(const dsd_opts* opts, dsd_state* state);
/** @brief processAudio() for the right frame (`audio_out_temp_bufR`) on slot 2's state. */
void processAudioR(const dsd_opts* opts, dsd_state* state);

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
/** @brief Reopen local output streams when the active input changes async/sync output policy. */
int dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts);

/**
 * @brief Make sure the analog monitor's raw output sink is open (decoder thread only).
 *
 * For a runtime switch into the analog family: with an unmuted local audio device, opens `audio_raw_out` with the
 * parameters openAudioOutput() uses (unmuting reopens device streams itself); with UDP output (`-o udp`), muted or not,
 * opens the analog socket on port + 2 (`udp_sockfdA`) through the UDP audio hook, since unmuting reopens no socket.
 * Other outputs have no raw sink to open. Idempotent. A failure is logged once and leaves the sink closed, so analog
 * audio stays silent.
 *
 * @return 0 when the sink is open or not needed, -1 when it could not be opened (or @p opts is NULL).
 */
int dsd_audio_ensure_analog_output(dsd_opts* opts);

/**
 * @brief Make sure the digital voice output sink is open (decoder thread only).
 *
 * The digital counterpart of dsd_audio_ensure_analog_output(): opens `audio_out_stream` on a local audio device (UDP
 * output opened its digital socket at session start), plus the raw sink (device stream or UDP analog socket) when
 * ProVoice or the source-audio monitor needs it, exactly as session start would for the current options.
 *
 * @return 0 when the sinks are open or not needed, -1 when one could not be opened (or @p opts is NULL).
 */
int dsd_audio_ensure_digital_output(dsd_opts* opts);

/** @brief Write synthesized mono voice samples for slot 1. */
void writeSynthesizedVoice(const dsd_opts* opts, dsd_state* state);
/** @brief Write synthesized mono voice samples for slot 2. */
void writeSynthesizedVoiceR(dsd_opts* opts, dsd_state* state);
/**
 * @brief Write the mono (left) synthesized frame into a slot's per-call WAV.
 *
 * For a source that stages either timeslot in the mono buffer (X2-TDMA): the samples come from
 * `audio_out_temp_buf` whatever @p slot is, and land in `wav_out_f` (slot 0) or `wav_out_fR` (slot 1). A closed
 * file writes nothing.
 */
void dsd_write_mono_voice_to_slot_wav(dsd_opts* opts, dsd_state* state, int slot);
/** @brief Write synthesized mono mixed voice samples. */
void writeSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state);

/*
 * The voice mixers serve both output formats (issue #652): they stage 16-bit samples after the one gain stage
 * (processAudio()/processAudioR()), and float output (-y) only changes how the raw fd (-o -) and UDP write them (as
 * f32, s16 / 32768). The local audio device takes 16-bit samples in either format.
 */
/** @brief Play synthesized voice (short mono output slot 1). */
void playSynthesizedVoice(dsd_opts* opts, dsd_state* state); // short mono output slot 1
/** @brief Play short mono voice, duplicating samples when the configured output is stereo. */
void playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state); // short mono mix
/** @brief Play synthesized voice (short stereo mix). */
void playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state); // short stereo mix
/** @brief Play synthesized voice (short stereo mix 3v2 DMR). */
void playSynthesizedVoiceSS3(dsd_opts* opts, dsd_state* state); // short stereo mix 3v2 DMR

/**
 * @brief Decoded media the vocoder staged for a DMR slot since the stereo mix last ran (issue #574).
 *
 * The DMR stereo mix (SS3, in both output formats) runs on a timer of bursts, skipped ones included, and replays each
 * slot's buffers whether or not anything was decoded into them, so it notes the audible-audio stamp
 * (<dsd-neo/core/audio_activity.h>) only for a slot whose gates pass, that a channel it emits carries (its output
 * policy may copy one slot over the other) and that holds media: SHORT when the vocoder ran the short path into
 * `s_l`/`s_r` (never the silence a muted slot stages). The silence a muted slot stages replaces its short frame, so it
 * retires that slot's SHORT flag: the flag then says media was staged after the slot's last silence, which the frames
 * the mix plays still hold. The mix takes both slots' flags; a path that discards the staged audio clears them. Kept
 * only while the stamp is armed. Process-wide, decoder thread only.
 */
enum { DSD_DMR_MIX_MEDIA_SHORT = 1 };

/** @brief The vocoder staged decoded media of @p kind (DSD_DMR_MIX_MEDIA_*) for DMR slot @p slot (0 or 1). */
void dsd_audio_dmr_mix_media_staged(int slot, unsigned int kind);
/** @brief The vocoder staged silence over DMR slot @p slot's (0 or 1) frame of @p kind: retires those flags. */
void dsd_audio_dmr_mix_media_silenced(int slot, unsigned int kind);
/** @brief The staged audio of DMR slot @p slot (0 or 1) was discarded: it holds no fresh media. */
void dsd_audio_dmr_mix_media_discard(int slot);
/** @brief A mix takes DMR slot @p slot's (0 or 1) media: returns its DSD_DMR_MIX_MEDIA_* bits and clears them. */
unsigned int dsd_audio_dmr_mix_media_take(int slot);

/**
 * @brief Play one synthesized voice frame through the mono or stereo mixer for the configured channel count.
 *
 * Picks the short mono (MS) or stereo (SS) mixer from `opts->pulse_digi_out_channels`, in either output format
 * (`opts->floating_point` only changes how the output writes the samples). Unsupported values and null arguments
 * produce no output.
 */
void dsd_play_synthesized_voice(dsd_opts* opts, dsd_state* state);

/**
 * @brief Play decoded 8 kHz mono PCM through the digital voice chain and mixers (issue #658).
 *
 * For a decoder outside the vocoder whose own gates passed the audio (M17's Codec2 voice, with its own CAN and crypto
 * gates and callsign addressing outside talkgroup policy). Each whole 160-sample frame runs the slot 0 high-pass,
 * gain and saturation once, then shared short staging. MS or SS expands it to the stream's channel count and the
 * common sink writes s16 or normalized f32. The slot 1 switch and output mute still apply; talkgroup policy and FDMA
 * crypto rules do not. Per-call and static WAVs consume the same chained samples; the static WAV follows slot 1.
 * A trailing partial frame is dropped, never read past. Nothing plays while an input interruption stands
 * (dsd_state::input_interrupted): the decoder read the frame across a Signal search's cut. Decoder thread only.
 *
 * @param count Samples in @p pcm.
 */
void dsd_audio_play_decoded_pcm16(const dsd_opts* opts, dsd_state* state, const short* pcm, size_t count);

/**
 * @brief dsd_audio_play_decoded_pcm16() for a decoder under talkgroup policy that settled the record policy as well
 * (TETRA, through dsd_audio_record_policy_gate_slot()).
 *
 * With @p record nonzero it is dsd_audio_play_decoded_pcm16(). With @p record 0 the frames play the same way, but
 * neither the per-call nor the static WAV takes them: a call whose talkgroup allows audio but not recording.
 */
void dsd_audio_play_decoded_pcm16_record(const dsd_opts* opts, dsd_state* state, const short* pcm, size_t count,
                                         int record);

/** @brief Apply the manual analog gain (`-n`, 0..100% as 0x..5x) to a short buffer: the M17 encoder's fixed input gain.
 *  The analog monitor's gain stage is dsd_analog_audio_process_f() (<dsd-neo/dsp/analog_audio.h>). */
void analog_gain(const dsd_opts* opts, dsd_state* state, short* input, int len); // manual gain for analog paths

/** @brief Analog monitor 6x sample-repetition upsampler. */
void upsample(dsd_state* state, float invalue);

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
 * @brief Open a mono PCM input file as either a WAV-family container or headerless raw PCM.
 *
 * `.wav` paths are treated as true WAV containers only when the file starts
 * with a supported WAV-family header such as `RIFF`, `RIFX`, or `RF64`
 * followed by `WAVE`. Headerless discriminator captures that merely use a
 * `.wav` suffix fall back to mono 16-bit
 * little-endian raw PCM at the configured sample rate. This fallback remains
 * for persisted captures produced by older deployments; remove the mislabeled
 * `.wav` branch after those captures are migrated or their support window ends.
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

/** @brief Duplicate mono int16 samples into interleaved stereo buffer. */
void audio_mono_to_stereo_s16(const short* in, short* out, size_t n);

/** @brief Mix two int16 channels with per-channel mute flags into interleaved stereo. */
void audio_mix_interleave_stereo_s16(const short* left, const short* right, size_t n, int encL, int encR,
                                     short* stereo_out);
/** @brief Mix two int16 channels with mute flags into mono output (the average when both are on). */
void audio_mix_mono_from_slots_s16(const short* left, const short* right, size_t n, int l_on, int r_on, short* out);

/** @brief Return 1 when P25p2 decode should queue audio for the slot under decrypt and media policy. */
int dsd_p25p2_decode_audio_allowed(const dsd_opts* opts, const dsd_state* state, int slot, int alg);

/**
 * @brief Apply a forced DMR ALGID to encrypted current-slot metadata when no OTA ALG ID is known.
 *
 * Fallback only: a slot whose ALG ID was already learned over the air (PI header, LE single
 * burst) is left untouched, and a known KEY ID is never replaced by the 0xFF sentinel.
 * Returns 1 when applied.
 */
int dsd_dmr_apply_forced_algid(dsd_state* state);

/**
 * @brief The ALG ID a slot's call will be decrypted under, for classification before voice runs.
 *
 * The slot's OTA ALG ID when it has one. Otherwise, when the service options carry the privacy
 * bit (@p so & 0x40) and --dmr-force-algid is set, the forced value dsd_dmr_apply_forced_algid()
 * will install on the first voice frame -- the same rule, read without mutating the slot. The
 * LC path classifies and arms the encryption lockout before any voice frame has run, and trunk
 * tuning zeroes payload_algid on every voice-channel tune, so without this every trunked call's
 * first LC would be judged against "no ALG ID" -- i.e. against whatever key the slot last
 * carried rather than the key the forced ALG (and any --dmr-tg-key-csv row) will actually
 * select. 0 when neither source yields an ALG ID.
 */
int dsd_dmr_classify_algid(const dsd_state* state, int slot, int so);

/** @brief Release hook: play everything the P25 Phase 2 playout holds, then empty it (either output format). */
void dsd_p25p2_flush_partial_audio(dsd_opts* opts, dsd_state* state);
/** @brief A P25 Phase 2 slot's transmission ended: close its playout stream; its queued frames still play. */
void dsd_p25p2_flush_partial_audio_slot(dsd_opts* opts, dsd_state* state, int slot);

/** @brief Talkgroup/whitelist/TG-hold gating for mono mix (enc flags 0=unmuted,1=muted). */
int dsd_audio_group_gate_mono(const dsd_opts* opts, const dsd_state* state, unsigned long tg, int enc_in, int* enc_out);
/** @brief Talkgroup/whitelist/TG-hold gating for dual/slot mix (enc flags 0=unmuted,1=muted). */
int dsd_audio_group_gate_dual(const dsd_opts* opts, const dsd_state* state, unsigned long tgL, unsigned long tgR,
                              int encL_in, int encR_in, int* encL_out, int* encR_out);
/**
 * @brief The talkgroup gate of dsd_audio_group_gate_mono() for a call that is not published: a private call
 * (@p private_call) is judged on both its parties, @p source and @p target, a group call on @p target and its
 * @p source, as the gate judges a published call of that kind with those identities (enc flags 0=unmuted,1=muted).
 *
 * For a decoder that chooses between calls before it publishes one: TETRA's four timeslots share canonical slot 0, and
 * only speech the policy lets play may hold the receiver on its timeslot.
 */
int dsd_audio_group_gate_call(const dsd_opts* opts, const dsd_state* state, int private_call, uint32_t target,
                              uint32_t source, int enc_in, int* enc_out);
/**
 * @brief Mono per-call WAV gate: the vocoder's verdict on the current slot's frame (dsd_state::mbe_slot_muted) and
 * the TG/allow-list/TG-hold policy. Called by the vocoder after it judged the frame.
 */
int dsd_audio_record_gate_mono(const dsd_opts* opts, const dsd_state* state, int* allow_out);
/**
 * @brief WAV gate for TG/allow-list/TG-hold policy alone, on the call in @p slot.
 *
 * For writers whose decode path settles crypto itself and never sets the DMR
 * slot encryption flags dsd_audio_record_gate_mono() reads: X2-TDMA, D-STAR and
 * SDRTrunk JSON playback. The caller names the slot its protocol publishes on,
 * since currentslot can be left over from another protocol's TDMA decode. A slot
 * with no active call is allowed.
 */
int dsd_audio_record_policy_gate_slot(const dsd_opts* opts, const dsd_state* state, int slot, int* allow_out);

/** @brief How a voice frame stands toward encryption, after the keystream the vocoder applied to it (issue #655). */
typedef enum {
    DSD_VOICE_CRYPTO_CLEAR = 0,     /**< Not encrypted (or no encryption is classified for the protocol). */
    DSD_VOICE_CRYPTO_DECRYPTABLE,   /**< Encrypted, and the key the vocoder applied decrypts it. */
    DSD_VOICE_CRYPTO_UNDECRYPTABLE, /**< Encrypted, and no loaded key decrypts it. */
} dsd_voice_crypto_class;

/**
 * @brief The user's rule on a classified voice frame: 1 when it plays.
 *
 * Reverse mute (-q) plays an encrypted frame, decryptable or not, and mutes a clear one. Otherwise a frame plays
 * unless it is undecryptable while the slot's encrypted-audio mute (dmr_mute_encL/R) is on.
 */
int dsd_voice_crypto_class_audible(const dsd_opts* opts, int slot, dsd_voice_crypto_class cls);

/**
 * @brief The protocol that owns the voice frame being decoded.
 *
 * The protocol processFrame() dispatched (YSF and scrambled dPMR keep theirs while they decode through the NXDN or
 * P25 Phase 1 decoder), else state->synctype.
 */
int dsd_voice_frame_owner(const dsd_state* state);

/**
 * @brief The NXDN-family classification of the current frame: NXDN, and dPMR, whose bridge sets nxdn_cipher_type 1
 * for a scrambled half and 0 for a clear one. Decryptable when the vocoder applies a keystream to it (a scalar key for
 * ciphers 1 and 2 or under forced privacy, an AES key for cipher 3).
 */
dsd_voice_crypto_class dsd_nxdn_voice_crypto_class(const dsd_state* state);

/**
 * @brief The encryption verdict on the voice frame on @p slot, by the protocol that owns it: 1 when it plays.
 *
 * One verdict for every output (issue #655): P25 Phase 1 and Phase 2 by the P25 rule
 * (p25_crypto_audio_output_permitted(), Phase 2 also needs its decode gate); NXDN and dPMR by
 * dsd_nxdn_voice_crypto_class() and dsd_voice_crypto_class_audible() on slot 0; DMR by the vocoder's verdict on the
 * slot's last frame (dsd_state::mbe_slot_muted), since it depends on the keystream that frame got. Protocols with no
 * encryption classified here (YSF, D-STAR, X2-TDMA, ProVoice), no protocol, and SDRTrunk JSON playback, which checks
 * crypto itself, always play. The FDMA mixers call it for the frame they play; the vocoder stores it per slot.
 */
int dsd_voice_crypto_audible(const dsd_opts* opts, const dsd_state* state, int slot);

/**
 * @brief Key material the DMR/P25 voice ALGID @p algid requires.
 *
 * The project's single voice ALGID table. dsd_dmr_voice_alg_can_decrypt() is derived from it, and
 * the --dmr-tg-key-csv resolver gates on it, so the ALG knowledge behind "can this key decrypt?"
 * and behind "may this map row apply?" is one table rather than two that can drift.
 *
 * Unclassified ALGIDs return DSD_KEY_NEED_NONE, which reads as "no keyring material selects this"
 * -- consistent with dsd_dmr_voice_alg_can_decrypt() already reporting 0 for them.
 */
dsd_key_material_need dsd_dmr_alg_key_need(int algid);

/**
 * @brief Return 1 when a DMR/P25-style voice ALGID has sufficient key material to decrypt.
 *
 * This helper intentionally only covers known/implemented families that can be
 * checked from legacy scalar values and an AES loaded flag. Supplied zero scalars and slot-specific key
 * completeness rules, such as Kirisun 0x36/0x37, require
 * dsd_dmr_voice_slot_can_decrypt() -- or, when the verdict is wanted for a key
 * ID other than the slot's installed one (a --dmr-tg-key-csv override that has
 * not been activated yet), dsd_dmr_voice_kid_can_decrypt(). Unknown ALGIDs
 * return 0 so callers keep audio muted rather than falsely unmuting garble.
 */
int dsd_dmr_voice_alg_can_decrypt(int algid, unsigned long long r_key, int aes_loaded);

/** @brief Return 1 when missing DMR ALG ID can still be decrypted from loaded per-slot key material. */
int dsd_dmr_missing_alg_key_can_decrypt(const dsd_state* state, int slot);

/**
 * @brief Return 1 when the slot carries a complete Kirisun (ALG 0x36/0x37) key.
 *
 * Requires all four AES segments present and strictly non-zero. Exposed so classification can
 * compare the slot's installed key against a prospective one -- see
 * keyring_kid_kirisun_complete(), which predicts this for a key ID that has not been activated.
 */
int dsd_dmr_kirisun_slot_key_complete(const dsd_state* state, int slot);

/**
 * The key material a decryptability verdict is taken against. Built by dsd_dmr_slot_key_material()
 * for a slot, or by hand in tests.
 *
 * @p kirisun_complete carries the Kirisun 0x36/0x37 verdict separately because it cannot be
 * expressed as r_key/aes_loaded: activation overwrites aes_key_segments[]/A1..A4[] for those ALG
 * IDs too, so the slot's current quartet describes the previous key, not the one this call will
 * use. dsd_dmr_kirisun_slot_key_complete() answers it for the slot's own key,
 * keyring_kid_kirisun_complete() for a prospective key ID. Ignored for every other ALG ID.
 */
typedef struct {
    unsigned long long r_key;
    int aes_loaded;
    int kirisun_complete;
    int scalar_present; /* An explicitly supplied scalar may be zero. */
} dsd_dmr_key_material;

/**
 * @brief Decryptability check against key material the caller supplies.
 *
 * Same rules as dsd_dmr_voice_slot_can_decrypt(), but the key material is a parameter rather
 * than per-slot state, so classification can evaluate a key id that has not been activated --
 * which is what --dmr-tg-key-csv requires at LC/PI time, before any voice frame has run.
 */
int dsd_dmr_voice_kid_can_decrypt(const dsd_state* state, int slot, int algid, const dsd_dmr_key_material* key);

/**
 * @brief Key material a DMR slot will actually decrypt with.
 *
 * Reports the slot's own installed material, or -- when @p mapped says a --dmr-tg-key-csv row
 * replaced the signaled key ID -- what @p key_id would install. The mapped case cannot be read off
 * the slot: activation has not run yet at LC/PI time, so R/RR, aes_key_loaded[] and the Kirisun
 * quartet still describe the previous key.
 *
 * One implementation because the callers must not disagree: dmr_flco.c uses it both for the label
 * the operator sees and for the gate that arms the encryption lockout (which forces a P_CLEAR and
 * drops the channel, and cannot self-heal), and dmr_pi.c publishes the same verdict for the same
 * call. An out-of-range slot yields all-zero material, i.e. "cannot decrypt".
 */
dsd_dmr_key_material dsd_dmr_slot_key_material(const dsd_state* state, int slot, int key_id, int mapped);

/**
 * @brief Slot-aware DMR/P25-style decryptability check.
 *
 * Use this when ALGID rules depend on per-slot key metadata. Kirisun 0x36/0x37
 * requires a complete four-segment key, while AES-128/AES-256 families continue
 * to use the broader per-slot AES loaded flag.
 */
int dsd_dmr_voice_slot_can_decrypt(const dsd_state* state, int slot, int algid, unsigned long long r_key);

/** @brief Terminal call-alert and event beeper. */
void beeper(dsd_opts* opts, dsd_state* state, int lr, int id, int ad, int len);

/** @brief Open input audio device based on opts. Returns 0 on success. */
int openAudioInDevice(dsd_opts* opts, dsd_state* state);
/** @brief Close all input resources owned by the active input device. */
void closeAudioInDevice(dsd_opts* opts);

/** @brief Parse audio input device string and update opts. */
void parse_audio_input_string(dsd_opts* opts, char* input);
/** @brief Parse audio output device string and update opts. */
void parse_audio_output_string(dsd_opts* opts, char* input);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_H_H */
