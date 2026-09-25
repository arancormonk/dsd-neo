// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Received sub-audible tone and code detection on the analog FM monitor (issues #522, #523).
 *
 * The decoder thread taps the raw monitor audio as the symbol path assembles each unsynced
 * analog block, before the voice filters that would remove everything below 300 Hz, and
 * publishes what it hears in dsd_state::analog_rx. Detection runs whenever the analog FM
 * monitor does (analog-only decoding with input monitoring, on PCM input or on an RTL-family
 * stream that outputs monitor audio), whether or not audio is played and whether or not a tone
 * policy exists: it never gates audio.
 *
 * All calls run on the decoder thread, which owns dsd_state. Frontends read the publication
 * from their snapshot, never the detector.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_RX_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_RX_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How long the carrier may read closed, in sample time, before the tone is forgotten. */
enum { DSD_ANALOG_CARRIER_HANGOVER_MS = 200 };

/**
 * @brief Most input, in sample time, that the tap lets wait before reading it.
 *
 * The symbol path assembles its monitor block sample by sample (960 samples on PCM input, 384 ms
 * at 2500 Hz). The tap reads the block as it fills, this much at a time, so the detectors and
 * the publication keep pace with the input whatever the block's length.
 */
enum { DSD_ANALOG_RX_TAP_READ_MS = 20 };

/**
 * @brief CTCSS timing contract, in sample time (docs/cli.md "Received tone").
 *
 * Lock runs from a tone's onset to the first hop that reports it; loss runs from the moment the
 * tone stops under a live carrier to the first hop that no longer reports it. The contract
 * covers a tone in noise at 0 dB in-band tone-to-noise or better, on its table value or off it
 * by transmitter tone error of up to 0.2 Hz (0.35 Hz at +10 dB):
 *
 * - p95 targets: 95% of onsets lock within DSD_ANALOG_CTCSS_LOCK_P95_MS and 95% of stops are
 *   dropped within DSD_ANALOG_CTCSS_LOSS_P95_MS.
 * - Per-event ceilings, DSD_ANALOG_CTCSS_LOCK_CEILING_MS and DSD_ANALOG_CTCSS_LOSS_CEILING_MS,
 *   above the slowest event of the long-run sweeps. Every onset and stop in DSP_ANALOG_CTCSS is
 *   within them. Over 1,000,000 onsets per condition at 0 dB, on the table value and 0.2 Hz off
 *   it, none locked later than the lock ceiling (the slowest after 651 ms), nor did any of
 *   100,000 per condition at +10 dB (the slowest after 452 ms, 0.35 Hz off); no stop of 800,000
 *   at 0 and +10 dB was dropped later than the loss ceiling (the slowest after 738 ms). Lock time
 *   in noise has no absolute bound: the 250 ms window alone locked about one onset in 125,000 at
 *   0 dB later than the lock ceiling (the slowest after 1,128 ms), and the detector's longer
 *   late acquisition windows are what lock those within it.
 *
 * A carrier that keeps dropping out, each time for less than DSD_ANALOG_CARRIER_HANGOVER_MS,
 * never expires, and a stop under it is dropped later than under a live carrier: past the p95
 * target but within the loss ceiling. Over 11,264 stops under such carriers (openings of 1 to
 * 60 ms between dropouts of 10 to 199 ms), 95% were dropped within 465 ms and the slowest after
 * 693 ms.
 *
 * Speech louder than the tone is outside the contract: under transmitter-filtered speech 10 dB
 * above the tone, 0.16% of 50,000 onsets locked later than the lock ceiling (the slowest after
 * 1,314 ms). docs/testing.md ("Ceiling tail") has the per-condition sweeps.
 *
 * A tone policy that waits for a lock before deciding there is no tone must wait at least
 * DSD_ANALOG_CTCSS_LOCK_CEILING_MS plus 100 ms (two hops), and still meets a late lock under
 * speech at the rate above.
 *
 * A lock can also name the wrong tone for a few hops, and a policy that acts on the first lock
 * meets those too. Near 0 dB in-band an off-table tone can lock a table neighbour for 200-260 ms
 * before the detector drops it: over two hours of continuous carrier at 0 dB, 68.2 Hz read as
 * 67.0 or 69.3 Hz about ten times an hour and 161.0 and 166.7 Hz as a neighbour two or three
 * times an hour, while 150.0 Hz never read as 151.4 Hz and from +3 dB up 68.2 and 161.0 Hz
 * never locked. A voice whose fundamental holds on a table tone can lock that tone (talk-off):
 * under transmitter-filtered speech 10 dB above the tone, twice in 50,000 onsets a voice holding
 * 254.1 Hz locked it for 150-250 ms, once in place of the real tone, and over two hours of
 * seeded speech with no tone the unfiltered speech model locked a tone three times (233.6, 241.8
 * and 254.1 Hz) and the transmitter-filtered one once (254.1 Hz).
 */
enum {
    DSD_ANALOG_CTCSS_LOCK_P95_MS = 400,
    DSD_ANALOG_CTCSS_LOCK_CEILING_MS = 700,
    DSD_ANALOG_CTCSS_LOSS_P95_MS = 350,
    DSD_ANALOG_CTCSS_LOSS_CEILING_MS = 800,
};

/**
 * @brief DCS timing contract, in sample time (docs/cli.md "Received code").
 *
 * Lock runs from the onset of a code's word to the first bit that reports it, through the
 * demodulator's DC block and 75 or 750 us de-emphasis at 8 to 78.125 kHz, for a code in noise
 * at an in-band signal-to-noise ratio (0-290 Hz) of:
 *
 * - 10 dB or better: every start within DSD_ANALOG_DCS_LOCK_MS. Over 7,000,000 starts (random
 *   codes in either polarity, the onset anywhere in a word, 75 and 750 us at 8, 44.1, 48 and
 *   78.125 kHz) the slowest took 445 ms.
 * - 3 dB: a p95 target, DSD_ANALOG_DCS_LOCK_P95_MS, and a per-event ceiling,
 *   DSD_ANALOG_DCS_LOCK_CEILING_MS, above the slowest start of the long-run sweeps. Over
 *   8,500,000 starts at 8, 44.1, 48 and 78.125 kHz with both de-emphasis settings, p95 was at
 *   most 415 ms and p99.9 at most 690 ms; 23 took longer than a second, all at 78.125 kHz and 21
 *   of them with 750 us, where the DC block sags the most and the de-emphasis smears a code's
 *   isolated bits, and the slowest took 1,224 ms there. Lock time in noise has no absolute
 *   bound. Every code in both polarities in DSP_ANALOG_DCS locks within 700 ms on its fixed
 *   seeds.
 *
 * A lock needs the code's 23-bit word read twice in a row, 46 bits or 342 ms, so a lock
 * typically comes 345-370 ms after the code starts.
 *
 * A carrier off frequency (the transmitter's error or the receiver's) adds a DC level to the
 * discriminator's output that steps in with the carrier. The balance slicer reads through any
 * level that holds over a word, however large; the droop slicers only once the DC blocking
 * ahead of them has taken it below the code's level. With a step at the code's onset of up to
 * twice the code's deviation, up or down, through the demodulator's DC block at every rate, a
 * DC-coupled PCM input or a 1 Hz coupling, the bounds above hold: at 10 dB every one of
 * 1,200,000 starts locked within DSD_ANALOG_DCS_LOCK_MS (the slowest after 473 ms), and at 3 dB
 * p95 was at most 421 ms and the slowest 882 ms. A step of four times the deviation is past
 * them: 1 of 1,000,000 starts at 10 dB took 526 ms (DC-coupled, 48 kHz, 750 us), and at 3 dB
 * p95 reached 474 ms, the slowest 1,020 ms, within the ceiling.
 *
 * PCM input reaches the detector through the sound card's coupling instead of the demodulator's
 * DC block (a one-pole high-pass at 3.7 Hz at 48 kHz, 6.1 Hz at 78.125 kHz), or through none.
 * A DC-coupled input or a coupling with a corner up to 10 Hz keeps the 10 dB bound: over 800,000
 * starts through a 10 Hz corner at 44.1 and 48 kHz with both de-emphasis settings, the slowest
 * took 477 ms, and with a step of up to four times the deviation at the onset the slowest of
 * 600,000 took 511 ms. At 3 dB a 10 Hz corner is outside the contract, since the steeper sag
 * costs bits: over 1,400,000 such starts p95 was 475-535 ms, p99.9 837-930 ms and the slowest
 * 1,702 ms. A 15 Hz corner misses the 10 dB bound too (5 of 100,000 starts past it, the
 * slowest 649 ms; p95 716 ms at 3 dB).
 *
 * Loss runs from the moment the word stops under a live carrier to the first bit that no longer
 * reports it: 32 bits and the front end's delay, with a p95 target, DSD_ANALOG_DCS_LOSS_P95_MS,
 * and a ceiling, DSD_ANALOG_DCS_LOSS_CEILING_MS. Over 1,000,000 stops at 3 and 10 dB, p95 was
 * at most 328 ms and the slowest 591 ms: in about 9 stops in 100,000 the noise that follows
 * reads as the code once more (within one bit of the word expected next, or exactly at another
 * place in it), which starts the 32 bits over. The 134.4 Hz turn-off tone a transmitter sends as
 * it unkeys ends a lock sooner, once the code has gone with it: p95 target
 * DSD_ANALOG_DCS_TURNOFF_LOSS_P95_MS, ceiling DSD_ANALOG_DCS_TURNOFF_LOSS_CEILING_MS; over
 * 1,000,000 turn-offs at 3 and 10 dB, p95 was at most 134 ms and the slowest 353 ms (78.125 kHz,
 * 750 us, 3 dB; the detector without its balance slicer reads that turn-off the same). A steady
 * component near 134.4 Hz under a code that is still read (an interferer, a voice holding its
 * pitch) ends no lock at 10 dB; at 3 dB, where the noise now and then costs the code a bit, one
 * at the code's power or 3 dB above it ended one or two locks a minute, each locking again.
 *
 * The same code starting over at another place in its word (a radio re-keying inside the carrier
 * hangover without a turn-off tone, another transmitter behind a repeater whose carrier stays up)
 * keeps the lock: at 10 dB or better, of 7,040 such restarts at 8 to 78.125 kHz, on a continuous
 * carrier or after a 120 or 190 ms gap, moved by 1-22 bits and any share of a bit, one showed no
 * code briefly (48 kHz, 10 dB, after a 190 ms gap, as the detector without its balance slicer
 * does too) and none showed another; at 3 dB, where the new place must be read exactly, up to 19
 * of 440 per condition dropped and locked again. The windows read across a dropout hold bits of
 * it for a word, so only windows read wholly with the carrier open count toward the 32 bits, and
 * a lock that has not held for 64 bits (476 ms), carrier open or closed, is lost. Under a carrier
 * that keeps dropping out, each time for less than the hangover, a code that stops is lost within
 * that span and a word of the stop: over 16,000 stops under random flicker (openings of 1-60 ms
 * between dropouts of 10-199 ms) at 3 and 10 dB, p95 was at most 548 ms and the slowest 619 ms,
 * past the live-carrier ceiling. A carrier that comes back from a dropout without the code loses
 * it the span after it last held, less the dropout: p95 368 ms after a 120 ms dropout (the
 * slowest of 16,000 after 591 ms, when the noise read as the code once more).
 *
 * Speech louder than the code is outside the contract. Transmitter-filtered speech 10 and 20 dB
 * above the code never lost a lock in 12 minutes at each level, but unfiltered speech 10 dB
 * above it (a voice fundamental in the band) lost it about 7 times a minute, each time locking
 * again, leaving the code shown 96% of the time. A held code at 3 dB with nothing else in the
 * band drops now and then too (once in 200 minutes with 750 us at 78.125 kHz and once in 200
 * minutes with 75 us at 48 kHz) and locks again.
 *
 * A tone policy that waits for a lock before deciding there is no code must wait at least
 * DSD_ANALOG_DCS_LOCK_CEILING_MS plus 100 ms.
 *
 * A DCS code outranks a CTCSS tone: while both are locked, dsd_state::analog_rx publishes the
 * code, so a voice's talk-off on a coded channel never hides it. A code's own waveform in noise
 * can read as a CTCSS tone before the code locks, and the publication shows that tone until the
 * code does, which a policy that acts on the first lock meets too: twice in 7,000,000 starts at
 * 10 dB of an earlier sweep (D274N as 67.0 Hz at 8 kHz for 161 ms, D122N as 77.0 Hz at
 * 78.125 kHz for 71 ms, which DSP_ANALOG_DCS replays), once in 2,800,000 starts at 10 dB with a
 * DC step at the onset (D225I as 77.0 Hz for 50 ms, through a 10 Hz coupling), never in this
 * sweep's 7,000,000 at 10 dB or 8,500,000 at 3 dB.
 */
enum {
    DSD_ANALOG_DCS_LOCK_MS = 520,
    DSD_ANALOG_DCS_LOCK_P95_MS = 450,
    DSD_ANALOG_DCS_LOCK_CEILING_MS = 1500,
    DSD_ANALOG_DCS_LOSS_P95_MS = 350,
    DSD_ANALOG_DCS_LOSS_CEILING_MS = 600,
    DSD_ANALOG_DCS_TURNOFF_LOSS_P95_MS = 150,
    DSD_ANALOG_DCS_TURNOFF_LOSS_CEILING_MS = 400,
};

/**
 * @brief Shortest gap in an input that may pause that counts as the carrier dropping.
 *
 * Inputs that may pause: stdin, UDP and TCP, and live RTL-family radio streams (not IQ
 * replay). A producer that squelches by sending nothing (rtl_fm without `-E pad`, a UDP sender
 * that stops) or a radio stream whose source stopped (an rtl_tcp server that went away, a
 * stalled device) delivers no samples for the sample-time hangover to count. When the tap's
 * next read arrives later than the previous read's duration plus DSD_ANALOG_CARRIER_HANGOVER_MS
 * after it, and at least this long after it, the pause counts as a carrier drop. The floor keeps
 * scheduling delays of a busy or instrumented decoder from ever reading as one.
 */
enum { DSD_ANALOG_STREAM_PAUSE_MIN_MS = 500 };

/**
 * @brief Most input, in sample time, the tap skips after a boundary while a live input drains
 * what it had queued before it (see dsd_analog_rx_reset()).
 *
 * A backlog builds only while the decoder is held up, as it is for the rigctl round trip of a
 * retune; of a longer one, what is left after this much is heard. An input that never runs dry,
 * such as stdin fed from a file as fast as the decoder reads it, is heard again after it.
 */
enum { DSD_ANALOG_RX_BACKLOG_MAX_MS = 2000 };

/**
 * @brief Feed the detectors the rest of a completed raw unsynced analog block.
 *
 * Called from the symbol path after the raw WAV write and before the voice filters, with the
 * whole block: the tap reads the samples dsd_analog_rx_tap_partial() has not already read,
 * without modifying any of them, and starts the next block from its first sample. A caller
 * that never calls dsd_analog_rx_tap_partial() hands over whole blocks.
 *
 * Each read resets on its own when the RTL stream generation or the trunk-tuning generation
 * moved (a retune it was not told about), when the analog receive profile the RTL stream
 * publishes changed (dsd_rtl_stream_metrics_hook_analog_profile(): a width-only change or the
 * channel filter turning on or off, applied to a running monitor, does not move the
 * generation), on an input that may pause when it arrives more than
 * DSD_ANALOG_STREAM_PAUSE_MIN_MS allows after the previous read, and when the input rate
 * changed since the previous read. Each way the samples read may straddle the boundary -- after
 * a rate change, samples taken at the old rate are another signal at the new one -- so they are
 * dropped and the new reception starts with the next read. After a generation move it also skips
 * what the input had queued, as after dsd_analog_rx_reset().
 */
void dsd_analog_rx_tap(const dsd_opts* opts, dsd_state* state, const float* block, unsigned int count);

/**
 * @brief Read the part of the raw unsynced analog block the symbol path has assembled so far.
 *
 * Called once per sample the symbol path adds, with the block and the number of samples it
 * now holds, the sample that completes the block included (before dsd_analog_rx_tap() is handed
 * the whole block). The tap reads what it has not read yet once DSD_ANALOG_RX_TAP_READ_MS of
 * input, at the input's current rate, is waiting, so on an input whose block lasts longer the
 * detectors and the publication still keep pace, also across a change of rate. When detection
 * starts part-way through a block, the tap reads from the sample just added, even when that
 * sample completes the block: the ones before it arrived while nothing listened. Otherwise as
 * dsd_analog_rx_tap().
 */
void dsd_analog_rx_tap_partial(const dsd_opts* opts, dsd_state* state, const float* block, unsigned int filled);

/**
 * @brief Note that the symbol path emptied its monitor block.
 *
 * The symbol path calls it whenever it empties dsd_state::analog_out_f: after each block it
 * completes, and when it drops a part-collected block, as dsd_symbol_analog_block_reset() and a
 * receive-family switch landing on an RTL front end do. The tap's next read then starts at the
 * first sample of the block that follows, not where it had read up to in the dropped one, which
 * could otherwise leave the new block's first samples unread. Only the detector's own state,
 * which the state holds by pointer, changes.
 */
void dsd_analog_rx_block_restart(const dsd_state* state);

/**
 * @brief The published carrier, held to the channel the receiver is on now (issue #526).
 *
 * dsd_state::analog_rx.carrier_open says what the tap heard at its last read, at every input rate: one the tap's front
 * end cannot use (tone_state UNAVAILABLE) keeps the carrier as well, above the level floor and through the hangover.
 * When the tap's own generation check would see a boundary since that read -- the trunk-tuning generation, and on RTL
 * input the stream generation or the published analog profile, moved -- the read was of the channel before, so this
 * returns 0 until the tap reads the new one. So it does while a retune is unresolved
 * (dsd_trunk_tuning_pending_request()): in flight, the front end still delivers the channel being left; failed after
 * the scanner moved on, it delivers a channel other than the one the scanner shows. A scanner therefore never holds a
 * row on another channel's carrier, and one a failed retune left the receiver on lets the row's hangtime run out.
 * Before detection has run there is nothing to compare, and the publication stands as it is. Read-only.
 */
int dsd_analog_rx_carrier_open_now(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Whether the monitor block now being completed began before a boundary the tap knows of (issue #526).
 *
 * Set when the tap's own generation check found a retune or an applied receive-profile change part-way through the
 * block (see dsd_analog_rx_tap()), when dsd_analog_rx_reset() set the samples collected before it aside, and when
 * detection started part-way through it, with no word on the samples before; cleared when the symbol path empties its
 * block (dsd_analog_rx_block_restart()). A boundary the tap has not read past yet, one that landed after its last read
 * of the block, counts too. Part of such a block is the old channel's, so the analog monitor's output drops it (one
 * block at most, or two when a boundary lands at a block's end); the raw WAV keeps it. 0 while detection is not running
 * (dsd_analog_tone_detection_active() on @p opts), whatever a reset set aside: the -8 source monitor under digital
 * decoding plays every block, as it did before the tap, whether or not an earlier analog row left the tap a session.
 */
int dsd_analog_rx_block_straddles_boundary(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Forget the received tone: clears the publication and every detector's state.
 *
 * Call on retune, scan row or target change, input switch, decode-mode change and stop, so a
 * new channel never inherits the previous channel's tone. What the monitor block the symbol
 * path is part-way through assembling (dsd_state::analog_out_f) holds arrived before the
 * boundary too, and what the tap reads next, which opens the new reception, must hold none of
 * it: the tap sets those samples aside and reads on from the next one. The block itself is left
 * alone, so the raw WAV and the monitor output keep every sample, in digital modes as in analog.
 *
 * Nor may the audio a live input still holds: on Pulse, stdin, UDP and TCP input the old
 * channel keeps arriving while a rigctl retune holds the decoder, and the decoder reads that
 * backlog afterwards. So on those inputs the tap skips what it reads after the boundary until a
 * read shows the input ran dry -- DSD_ANALOG_RX_TAP_READ_MS or more of input that took at least
 * half as long to arrive, not counting the time the decoder spent playing monitor audio
 * (dsd_analog_rx_playback_begin()), which a backlog read at the decoder's pace never does -- and
 * that read too, or until it has skipped DSD_ANALOG_RX_BACKLOG_MAX_MS. With nothing queued that
 * costs two reads. The publication reads IDLE meanwhile. Files and RTL-family streams are not
 * skipped: a file queues no other channel, and a stream clears its own output at a retune.
 *
 * Before detection has run there is no detector state to hold either boundary. The tap then
 * starts at the sample detection starts on (dsd_analog_rx_tap_partial()), and when a reset
 * came first (the publication's generation is no longer 0) its first reads skip what the input
 * holds, as after a reset with detection running.
 *
 * Not for the frequent no-carrier cleanup: that runs every few hundred milliseconds in analog
 * mode and would keep a tone from ever locking. Afterwards the publication reads INACTIVE until
 * the tap reads again.
 */
void dsd_analog_rx_reset(dsd_state* state);

/**
 * @brief Mark the start of the monitor playback of a block.
 *
 * The symbol path calls it before it writes a monitor block to the audio output and
 * dsd_analog_rx_playback_end() after. Synchronous playback (stdin and file input) holds the
 * decoder for the block's playing time once its buffer is full, and the decoder then reads an
 * input's backlog at real-time pace, as it reads audio arriving live; the backlog skip after a
 * boundary (dsd_analog_rx_reset()) counts the time between the two calls as playing, not as
 * waiting for input. Both only note the time in the detector's own state, which the state
 * holds by pointer, so neither changes @p state itself.
 */
void dsd_analog_rx_playback_begin(const dsd_state* state);

/** @brief Mark the end of the monitor playback dsd_analog_rx_playback_begin() started. */
void dsd_analog_rx_playback_end(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_RX_H_ */
