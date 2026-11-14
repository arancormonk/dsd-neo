// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The digital voice chain (issue #653): one high-pass and one speech level AGC per slot (issue #518), run once
 * on each decoded 8 kHz voice frame before anything consumes it.
 *
 * A decoder that synthesizes a voice frame (the vocoder, YSF V/D2, MBE file, SDRTrunk JSON playback, M17 Codec2 and
 * TETRA speech) runs dsd_voice_chain_run() on it in place, with what it knows about the frame (dsd_voice_frame_info),
 * whatever the output format, output type or mute say. Every consumer then takes
 * the same samples: the per-call WAV and the shared short staging (dsd_voice_stage_short_left/right()). Mixers route
 * those samples in both output formats; stdout and UDP convert the resulting s16 blocks to f32 under -y. Every
 * consumer starts from one level and one frequency response. The chain is verdict-neutral: crypto, talkgroup and
 * slot gates still decide who receives a frame. Decoder thread only.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_VOICE_CHAIN_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_VOICE_CHAIN_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/speech_agc.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The digital voice high-pass (`-v 0x8`): a 2nd-order Butterworth high-pass at this corner on 8 kHz frames. */
#define DSD_VOICE_HPF_CORNER_HZ       300.0
/** The rate the digital voice frames, and so the high-pass, run at. */
#define DSD_VOICE_HPF_RATE_HZ         8000.0
/** Samples per voice frame. */
#define DSD_VOICE_FRAME_SAMPLES       160
/** The start gain of each slot's speech AGC, dB over the decoder's own level (the `-g 1` reference, which DVSI's decoder
    plays at about -24 dBFS P.56): the gain that plays speech at the reference level at the -20 dBFS target. It is the
    centre of the AGC's range (-12/+30 dB around it), and tone frames play at no more than it. */
#define DSD_VOICE_LEVEL_START_GAIN_DB 4.0f
/** The gain, dB over the `-g 1` level, a reception opens at when its talker is not remembered. Talkers on live traffic
    needed about +10 dB on average, so a reception that opens here plays its first syllables nearer the target than at
    the start gain. */
#define DSD_VOICE_LEVEL_OPEN_GAIN_DB  8.0f

/** What a synthesized voice frame carries, as its decoder knows it. */
typedef enum {
    DSD_VOICE_MEDIA_NONE = 0,   /**< no decoded media: a decode failure's or a P25 tail erasure's silence */
    DSD_VOICE_MEDIA_CONCEALED,  /**< synthesized in place of speech: a repeat, a mute's comfort noise, an erasure,
                                     AMBE silence, an uncorrectable D-STAR C0, a TETRA bad frame */
    DSD_VOICE_MEDIA_SPEECH,     /**< a decoded speech frame */
    DSD_VOICE_MEDIA_TONE,       /**< a tone frame (DTMF, call progress and other AMBE tones), repeated or erased too */
    DSD_VOICE_MEDIA_TX_MONITOR, /**< the M17 encoder's own transmission, decoded back for its monitor: it plays at
                                     unity through a limiter of its own and never touches a slot's speech AGC */
} dsd_voice_media;

/**
 * @brief What the decoder knows about one frame it hands the chain. None of it is an output setting: the sinks, the
 * mutes, `-P`, the slot switches and the talkgroup policy only route the chained frame.
 */
typedef struct {
    uint8_t media;     /**< dsd_voice_media */
    uint8_t decodable; /**< the media is clear or decrypted, judged from its encryption class, never a user mute rule */
    uint8_t admitted;  /**< the receive policy admits the traffic (dsd_receive_current_allows()) */
    uint32_t talkspurt; /**< the producer's own transmission count, for a producer that tells transmissions apart
                             itself (TETRA); 0 for none */
} dsd_voice_frame_info;

/**
 * @brief The media of a frame mbelib-neo synthesized: NONE when @p ret is negative (the frame failed to decode), TONE
 * when @p flags has MBE_PROCESS_FLAG_TONE, CONCEALED when it has ERASURE, REPEAT, MUTE, SILENCE or C0_UNCORRECTABLE,
 * else SPEECH.
 */
dsd_voice_media dsd_voice_media_of_mbe(int ret, unsigned int flags);

/** @brief 1 when @p info is a decodable, admitted speech frame (the frames a level estimate may learn from); 0 for
 * NULL. */
int dsd_voice_frame_may_learn(const dsd_voice_frame_info* info);

/**
 * @brief Run the chain on one frame of slot @p slot (0 or 1) in place: the high-pass when `opts->use_hpf_d` is 1, then
 * the slot's speech level AGC (dsd_speech_agc, <dsd-neo/dsp/speech_agc.h>), then saturation to the int16 range.
 *
 * @p slot picks the filter and AGC state, whatever buffer holds @p frame (X2-TDMA synthesizes both timeslots into the
 * left buffer). With `-g 0` (`audio_gain`, slot 2 `audio_gainR`) the AGC, with its acquisition stage on
 * (dsd_speech_agc_set_acquisition()), aims the slot's speech at DSD_SPEECH_AGC_TARGET_DBFS, learning only from frames
 * dsd_voice_frame_may_learn() takes. Each reception opens at DSD_VOICE_LEVEL_OPEN_GAIN_DB over the `-g 1` level (the
 * decoder's own) from its first sample; a tone frame plays at no more than the start gain
 * (DSD_VOICE_LEVEL_START_GAIN_DB); an undecodable frame never teaches, and when the slot's frames turn decodable again
 * its limiter starts over. A `-g` above 0 is a fixed multiplier through the AGC's limiter (DSD_SPEECH_AGC_CEILING_DBFS);
 * switching back to auto keeps what the AGC learned and starts from the fixed gain, within the AGC's range. A setting
 * below 0 (no user surface sets one) passes the frame at unity. Each frame leaves the gain it got, in dB, in `aout_gain`
 * (slot 2 `aout_gainR`) for the readouts.
 *
 * A DSD_VOICE_MEDIA_TX_MONITOR frame (the M17 encoder's monitor) takes neither the slot's AGC nor `-g`: it plays at
 * unity through a fixed-gain speech AGC of its own, which only limits it at DSD_SPEECH_AGC_CEILING_DBFS, so the monitor
 * plays the level the encoder transmits and leaves the slots' speech level as it was.
 *
 * Both run for the slot's reception. The filter starts over when the slot's transmission
 * (dsd_call_snapshot_transmission() of its active call: one nonzero value to another) or the frame's protocol family
 * changes, and while the high-pass is off; a fade the call state heals keeps the transmission, and so the filter. The
 * AGC starts over when the transmission, the family or the producer's talkspurt (one nonzero value to another) changes,
 * and at dsd_voice_level_reset(). Frames before a call is known keep both.
 *
 * @param frame DSD_VOICE_FRAME_SAMPLES int16-scale samples.
 * @param info What the decoder knows about the frame; NULL for a frame nothing may learn from (decodable, no tone).
 */
void dsd_voice_chain_run(const dsd_opts* opts, dsd_state* state, int slot, float* frame,
                         const dsd_voice_frame_info* info);

/**
 * @brief Slot @p slot's speech level starts a new reception (both slots when @p slot is negative): its AGC forgets what
 * it learned and the reception it ran for, and the slot's next frame opens a reception at the opening gain
 * (DSD_VOICE_LEVEL_OPEN_GAIN_DB), which the slot's readout (`aout_gain`, `aout_gainR`) shows until then. For both
 * slots the DMR superframe mixer's one-channel limiter (audio_mix_mono_sum_s16()) goes back to unity too.
 *
 * The receiver left the carrier (a carrier boundary other than a stream gap, a trunk-scan target switch, a manual P25
 * control-channel selection) or a file starts. A stream gap, the no-carrier pass, a crypto purge and a rate change keep
 * it: the next frames' transmission decides. Decoder thread only.
 */
void dsd_voice_level_reset(dsd_state* state, int slot);

/** @brief Slot @p slot's speech AGC, for diagnostics and tests; NULL before the slot's first frame. */
const dsd_speech_agc* dsd_voice_level_agc(const dsd_state* state, int slot);

/**
 * @brief Stage the chained left frame (`audio_out_temp_buf`) for the short mixers: `s_l`, the output ring and, above 8
 * kHz, the upsampled `s_lu`. Runs no gain or filter.
 */
void dsd_voice_stage_short_left(const dsd_opts* opts, dsd_state* state);
/** @brief Stage the chained right frame (`audio_out_temp_bufR`) for the short mixers: `s_r`, the ring, `s_ru`. */
void dsd_voice_stage_short_right(const dsd_opts* opts, dsd_state* state);

/**
 * @brief Clear slot @p slot's high-pass state and the reception it ran for (both slots when @p slot is negative): the
 * next frames start a reception of their own.
 */
void dsd_voice_hpf_reset(dsd_state* state, int slot);

/** @brief High-pass @p n samples of @p buf in place on slot @p slot's state. A non-finite sample comes out as 0. */
void dsd_voice_hpf_process(dsd_state* state, int slot, float* buf, size_t n);

/** @brief The high-pass's designed magnitude response at @p hz, in dB. */
double dsd_voice_hpf_gain_db(double hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_VOICE_CHAIN_H */
