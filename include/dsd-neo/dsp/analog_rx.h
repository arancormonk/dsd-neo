// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Received sub-audible tone detection on the analog FM monitor (issue #522).
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
 * dsd_state::analog_rx.carrier_open says what the tap heard at its last read. When the tap's own generation check
 * would see a boundary since that read -- the trunk-tuning generation, and on RTL input the stream generation or the
 * published analog profile, moved -- the read was of the channel before, so this returns 0 until the tap reads the new
 * one. A scanner that has just retuned therefore never holds the new row on the old row's carrier. Before detection
 * has run there is nothing to compare, and the publication stands as it is. At an input rate the tap's front end
 * cannot use (tone_state UNAVAILABLE) the tap never opens a carrier, so the squelch alone decides: the receiver
 * power (opts->rtl_pwr) above opts->rtl_squelch_level, held to the same boundaries. Read-only.
 */
int dsd_analog_rx_carrier_open_now(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Whether the monitor block now being completed began before a boundary the tap knows of (issue #526).
 *
 * Set when the tap's own generation check found a retune or an applied receive-profile change part-way through the
 * block (see dsd_analog_rx_tap()), and when dsd_analog_rx_reset() set the samples collected before it aside; cleared
 * when the symbol path empties its block (dsd_analog_rx_block_restart()). Part of such a block is the old channel's,
 * so the monitor output drops it (one block at most); the raw WAV keeps it. 0 while detection is not running.
 */
int dsd_analog_rx_block_straddles_boundary(const dsd_state* state);

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
