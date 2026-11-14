// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The analog monitor's audio chain: voice band-pass, the legacy 960 Hz filters, then a fixed gain or the speech
 * AGC (<dsd-neo/dsp/speech_agc.h>, issue #518).
 *
 * Decoder-thread only. Each chain keeps its filter and AGC state in the DSD_STATE_EXT_DSP_ANALOG_AUDIO slot of the
 * state it runs on, allocated on first use; if that allocation fails the chain still applies the fixed gain, unfiltered.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_AUDIO_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_AUDIO_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where a chain's samples come from, which sets the gain that takes the reference signal to the reference level. */
typedef enum {
    /** RTL FM or AM monitor audio after the volume trim (`vol`, default 2): a 1 kHz tone at 3 kHz deviation, or AM at
        50%, reads 0.125 x vol peak, FM less the de-emphasis at 1 kHz, which the chain makes up. EDACS analog voice on
        an RTL input is this too: the stream's FSK voice lane (rtl_stream_read_lanes()) at the monitor's scale, with the
        trim and a de-emphasis the stream's published gain makes up the same way. */
    DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR = 0,
    /** RTL FSK discriminator output, normalised to +/-30000 peak: the -8 source monitor under digital decoding. */
    DSD_ANALOG_AUDIO_SOURCE_RTL_FSK = 1,
    /** PCM input at int16 scale. */
    DSD_ANALOG_AUDIO_SOURCE_PCM16 = 2,
} dsd_analog_audio_source;

/** Which chain: each keeps its own filter and AGC state. */
typedef enum {
    DSD_ANALOG_AUDIO_CHAIN_MONITOR = 0,
    DSD_ANALOG_AUDIO_CHAIN_EDACS = 1,
} dsd_analog_audio_chain;

/** The samples are qualified reception (the squelch open, the tone policy passing, no digital carrier, no retune
    unresolved): the AGC may learn from them, whether or not anything plays or records them. Without it the AGC holds;
    where it drops the reception closed and the AGC undoes what the tail taught (dsd_speech_agc_gate_closed()).
    dsd_analog_audio_process_runs_f() takes it per sample instead. */
#define DSD_ANALOG_AUDIO_LEARN                   0x1U
/** The samples start another reception (a new EDACS call): filters and AGC start over before them. */
#define DSD_ANALOG_AUDIO_RESET                   0x2U
/** The samples are dropped (a block that straddles a retune, partly the old channel's): the chain neither filters nor
    counts them, turns them into silence, and starts over with the samples after them. */
#define DSD_ANALOG_AUDIO_DISCARD                 0x4U

/** The fixed gain's reference level: the reference signal plays at -12 dBFS peak (-15 dBFS RMS) at `-n 50`. */
#define DSD_ANALOG_AUDIO_REFERENCE_PEAK          8231.0

/** The speech AGC's start gains over `-n 50`, dB, per band (issue #518): a nominal talker at rated deviation (FM) or
    modulation (AM) plays at the AGC's speech target from the first word. FM is narrowband up to this channel width
    (`--nfm-bandwidth-hz`, default 16000), wideband above it. */
#define DSD_ANALOG_AUDIO_START_GAIN_FM_NARROW_DB 8.537f
#define DSD_ANALOG_AUDIO_START_GAIN_FM_WIDE_DB   2.518f
#define DSD_ANALOG_AUDIO_START_GAIN_AM_DB        1.447f
#define DSD_ANALOG_AUDIO_FM_NARROW_MAX_WIDTH_HZ  16000

/** A gap at least this long between two samples the AGC learns from starts a new reception: the AGC starts over at
    its start gain (dsd_analog_audio_process_runs_f()). */
#define DSD_ANALOG_AUDIO_NEW_RECEPTION_GAP_NS    1000000000ULL

/**
 * @brief The gain that takes @p source's reference signal to DSD_ANALOG_AUDIO_REFERENCE_PEAK before any de-emphasis:
 * RTL monitor 32924 (0.25 at the default `vol` 2), RTL FSK 8231 / 30000 (0.274: the normalised +/-30000 peak), PCM 1.0.
 *
 * The chain's fixed gain at `-n 50` and the AGC's reference gain are this, times on RTL monitor audio the stream's
 * de-emphasis gain (dsd_rtl_stream_metrics_hook_monitor_deemph_gain()): the FM monitor's de-emphasis takes the 1 kHz
 * reference down (0.87 dB at the default 75 us, 13.65 dB at 750 us), and the chain makes that up (issue #518).
 */
double dsd_analog_audio_source_gain(dsd_analog_audio_source source);

/** The reference signal's peak in RTL monitor audio at the default `vol` 2, before de-emphasis: 1 kHz at 3 kHz
    deviation is 2 pi 3000 / 48000 = 0.3927 rad, 0.125 after the monitor's 1/pi output scale, and the AM detector puts
    50% at 0.25 x 0.5 = 0.125; the trim doubles either. FM de-emphasis then takes the FM tone down by its gain at
    1 kHz, which the audio chain makes up and the -6 raw WAV keeps. */
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
 * @brief Run @p chain over @p n samples of @p buf in place, at @p rate_hz, every sample learning as
 * DSD_ANALOG_AUDIO_LEARN in @p flags says: dsd_analog_audio_process_runs_f() with no per-sample flags.
 *
 * @return 0 on success, -1 on a NULL argument.
 */
int dsd_analog_audio_process_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf,
                               size_t n, dsd_analog_audio_source source, int rate_hz, unsigned int flags);

/**
 * @brief Run @p chain over @p n samples of @p buf in place, at @p rate_hz, with a learn flag per sample.
 *
 * In order: the voice band-pass when `use_pbf` is set (on the monitor chain under AM, the AM one, the FM one otherwise:
 * EDACS and the FSK discriminator output are FM), the legacy 960 Hz low-pass (`use_lpf`) and high-pass (`use_hpf`),
 * then the source gain (the gain that takes the source's reference signal to the `-n 50` level, on RTL monitor audio
 * times the stream's de-emphasis gain, dsd_rtl_stream_metrics_hook_monitor_deemph_gain()) and the speech AGC over it:
 * fixed at N / 50 for `-n N`, automatic on `-n 0`, its limiter holding -1 dBFS either way. On automatic a reception
 * starts at the band's start gain (DSD_ANALOG_AUDIO_START_GAIN_*_DB; on RTL monitor audio less the `vol` trim over its
 * default 2, so the trim does not move where the AGC starts). The AGC runs at @p rate_hz; at a rate it does not support
 * (dsd_speech_agc_init()) the chain plays the fixed gain (`-n 50` on auto) with the limiter, and on auto logs that once
 * per rate.
 *
 * The filters run whether or not the samples learn. @p learn holds one flag per sample (nonzero: the sample is
 * qualified reception, DSD_ANALOG_AUDIO_LEARN); NULL takes DSD_ANALOG_AUDIO_LEARN in @p flags for every sample. Where
 * the flag drops the reception closed and the AGC rolls back what its tail taught; where it rises one opened
 * (dsd_speech_agc_process_runs()), the flag of the call before carrying over. A sample that learns at least
 * DSD_ANALOG_AUDIO_NEW_RECEPTION_GAP_NS after the last one that learned starts a new reception: the AGC starts over
 * before it, whether samples that did not learn came between or none came at all (a source that stopped sending). The
 * gap is the longer of the samples processed between the two at @p rate_hz and their decode-clock times: the clock is
 * read once per call (dsd_decode_now_mono_ns()) as the time of its last sample, and each earlier sample sits its
 * distance in samples before it, so how a caller splits its samples into calls does not move the times.
 *
 * The chain starts over on DSD_ANALOG_AUDIO_RESET, after DSD_ANALOG_AUDIO_DISCARD, and on a change of source, rate or
 * band, and on a new reception: a new RTL stream generation or de-emphasis gain (on an RTL input), trunk-tuning
 * generation or announced boundary (dsd_analog_audio_note_reception()). Samples whose collection began in another
 * reception than the one now (dsd_analog_audio_block_begin()) straddle a boundary and are dropped as
 * DSD_ANALOG_AUDIO_DISCARD drops them; samples collected wholly after one start the chain over. So a retune never hands
 * the next channel the last one's level or filter history, whichever path collected the samples. Publishes the gain
 * applied (AGC and limiter), in dB over the `-n 50` gain, in dsd_state::aout_gainA.
 *
 * @return 0 on success, -1 on a NULL argument.
 */
int dsd_analog_audio_process_runs_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf,
                                    size_t n, dsd_analog_audio_source source, int rate_hz, unsigned int flags,
                                    const uint8_t* learn);

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

/** dsd_analog_audio_process_f() on int16 samples, saturating the output to int16 (the limiter keeps it under). */
int dsd_analog_audio_process_s(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, short* buf,
                               size_t n, dsd_analog_audio_source source, int rate_hz, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_AUDIO_H */
