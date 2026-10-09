// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The analog monitor's audio chain: voice band-pass, the legacy 960 Hz filters, then a fixed gain or the AGC
 * (issue #518).
 *
 * Decoder-thread only. Each chain keeps its filter and AGC state in the DSD_STATE_EXT_DSP_ANALOG_AUDIO slot of the
 * state it runs on, allocated on first use; if that allocation fails the chain still applies the fixed gain, unfiltered.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_AUDIO_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_AUDIO_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where a chain's samples come from, which sets the gain that takes the reference signal to the reference level. */
typedef enum {
    /** RTL FM or AM monitor audio after the volume trim (`vol`, default 2): a 1 kHz tone at 3 kHz deviation, or AM at
        50%, reads 0.125 x vol peak. */
    DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR = 0,
    /** RTL FSK discriminator output, normalised to +/-30000 peak: the -8 source monitor under digital decoding, and
        EDACS analog voice on an RTL input. */
    DSD_ANALOG_AUDIO_SOURCE_RTL_FSK = 1,
    /** PCM input at int16 scale. */
    DSD_ANALOG_AUDIO_SOURCE_PCM16 = 2,
} dsd_analog_audio_source;

/** Which chain: each keeps its own filter and AGC state. */
typedef enum {
    DSD_ANALOG_AUDIO_CHAIN_MONITOR = 0,
    DSD_ANALOG_AUDIO_CHAIN_EDACS = 1,
} dsd_analog_audio_chain;

/** The samples are heard: the AGC adapts to them. Without it the gain holds, and dropping it rolls the gain back
    (dsd_voice_agc_process()). */
#define DSD_ANALOG_AUDIO_PLAYING        0x1U
/** The samples start another reception (a new EDACS call): filters and AGC start over before them. */
#define DSD_ANALOG_AUDIO_RESET          0x2U
/** The samples are dropped (a block that straddles a retune, partly the old channel's): the chain neither filters nor
    counts them, turns them into silence, and starts over with the samples after them. */
#define DSD_ANALOG_AUDIO_DISCARD        0x4U

/** The fixed gain's reference level: the reference signal plays at -12 dBFS peak (-15 dBFS RMS) at `-n 50`. */
#define DSD_ANALOG_AUDIO_REFERENCE_PEAK 8231.0

/**
 * @brief The gain that takes @p source's reference signal to DSD_ANALOG_AUDIO_REFERENCE_PEAK: the fixed gain at
 * `-n 50`, and the AGC's reference gain. RTL monitor 32924 (0.25 at the default `vol` 2), RTL FSK 8231 / 30000
 * (0.274: the normalised +/-30000 peak), PCM 1.0.
 */
double dsd_analog_audio_source_gain(dsd_analog_audio_source source);

/** The reference signal's peak in RTL monitor audio at the default `vol` 2: 1 kHz at 3 kHz deviation is
    2 pi 3000 / 48000 = 0.3927 rad, 0.125 after the monitor's 1/pi output scale, and the AM detector puts 50% at
    0.25 x 0.5 = 0.125; the trim doubles either. */
#define DSD_ANALOG_AUDIO_RTL_MONITOR_REFERENCE 0.25

/**
 * @brief The scale that puts @p source's samples at int16 scale, where the -6 raw WAV records them (issue #643).
 *
 * RTL monitor audio, FM or AM, runs at the monitor's 1/pi output scale, where speech is about +/-0.25 and would round
 * to 0 or +/-1: it is scaled by DSD_ANALOG_AUDIO_REFERENCE_PEAK / DSD_ANALOG_AUDIO_RTL_MONITOR_REFERENCE (32924), so
 * its reference signal lands at the level a PCM input at the reference has. The RTL FSK discriminator output and PCM
 * input are at int16 scale already: 1.0, which writes them exactly as they are. Unlike the source gain, it ignores
 * `-n` and the AGC: the raw WAV is the input before the monitor's filters and gain stage.
 *
 * Inline, so the tests that replace analog_audio.c with their own copy still get it.
 */
static inline double
dsd_analog_audio_int16_scale(dsd_analog_audio_source source) {
    return source == DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR
               ? DSD_ANALOG_AUDIO_REFERENCE_PEAK / DSD_ANALOG_AUDIO_RTL_MONITOR_REFERENCE
               : 1.0;
}

/** Whether an analog gain setting (`-n`, dsd_opts::audio_gainA) asks for the AGC: 0, or anything not above it. */
static inline int
dsd_analog_gain_is_auto(float gain) {
    return !(gain > 0.0f);
}

/**
 * @brief Run @p chain over @p n samples of @p buf in place, at @p rate_hz.
 *
 * In order: the voice band-pass when `use_pbf` is set (on the monitor chain under AM, the AM one, the FM one otherwise:
 * EDACS and the FSK discriminator output are FM), the legacy 960 Hz low-pass (`use_lpf`) and high-pass (`use_hpf`),
 * then the fixed gain (`-n N`: the source gain x N / 50) or, with the gain on auto, the AGC. Every filter's state is the
 * chain's own. The filters run whether or not the samples are heard; the AGC adapts only to DSD_ANALOG_AUDIO_PLAYING
 * samples. The chain starts over on DSD_ANALOG_AUDIO_RESET, after DSD_ANALOG_AUDIO_DISCARD, and on a change of source,
 * rate or band, and on a new reception: a new RTL stream generation (on an RTL input), trunk-tuning generation or
 * announced boundary (dsd_analog_audio_note_reception()). Samples whose collection began in another reception than
 * the one now (dsd_analog_audio_block_begin()) straddle a boundary and are dropped as DSD_ANALOG_AUDIO_DISCARD drops
 * them; samples collected wholly after one start the chain over. So a retune never hands the next channel the last
 * one's level or filter history, whichever path collected the samples. Publishes the gain applied, in dB over the
 * `-n 50` gain, in dsd_state::aout_gainA. The output is not clipped.
 *
 * @return 0 on success, -1 on a NULL argument.
 */
int dsd_analog_audio_process_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf,
                               size_t n, dsd_analog_audio_source source, int rate_hz, unsigned int flags);

/**
 * @brief Note that @p chain starts collecting the samples it will process next: the reception they begin in.
 *
 * Call it when a block starts filling (EDACS: before its three blocks). Samples processed after a boundary moved since
 * are dropped (dsd_analog_audio_process_f()).
 */
void dsd_analog_audio_block_begin(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain);

/**
 * @brief A new reception starts: whatever dsd_analog_rx_reset() announces (a scan row commit, a retune the stream
 * generation does not show, such as the legacy -Y rigctl one, a reconnect). Samples collected before it and not yet
 * processed are the old channel's.
 */
void dsd_analog_audio_note_reception(const dsd_state* state);

/** dsd_analog_audio_process_f() on int16 samples, saturating the output to int16. */
int dsd_analog_audio_process_s(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, short* buf,
                               size_t n, dsd_analog_audio_source source, int rate_hz, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_AUDIO_H */
