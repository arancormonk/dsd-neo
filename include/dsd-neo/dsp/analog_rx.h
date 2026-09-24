// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Received sub-audible tone detection on the analog FM monitor (issue #522).
 *
 * The decoder thread taps the raw monitor audio once per unsynced analog block, before the
 * voice filters that would remove everything below 300 Hz, and publishes what it hears in
 * dsd_state::analog_rx. Detection runs whenever the analog FM monitor does (analog-only
 * decoding with input monitoring, on PCM input or on an RTL-family stream that outputs monitor
 * audio), whether or not audio is played and whether or not a tone policy exists: it never
 * gates audio.
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
 *   each with its measured exceedance rate. Every onset and stop in DSP_ANALOG_CTCSS is within
 *   them. Lock time in noise has no absolute bound, so the lock ceiling is exceeded at a small
 *   rate that a consumer has to budget for: over 1,000,000 onsets at 0 dB, about one in 125,000
 *   on the table value and one in 40,000 for a tone 0.2 Hz off locked later (the slowest after
 *   1,128 ms), and none of 100,000 per condition at +10 dB did. No stop of 800,000 at 0 and
 *   +10 dB was dropped later than the loss ceiling (the slowest after 738 ms).
 *
 * Speech louder than the tone is outside the contract: under transmitter-filtered speech 10 dB
 * above the tone, 0.21% of 50,000 onsets locked later than the lock ceiling (the slowest after
 * 1,314 ms). docs/testing.md ("Ceiling tail") has the per-condition sweeps.
 *
 * A tone policy that waits for a lock before deciding there is no tone must wait at least
 * DSD_ANALOG_CTCSS_LOCK_CEILING_MS plus 100 ms (two hops), and still meets a late lock at the
 * rates above.
 */
enum {
    DSD_ANALOG_CTCSS_LOCK_P95_MS = 400,
    DSD_ANALOG_CTCSS_LOCK_CEILING_MS = 700,
    DSD_ANALOG_CTCSS_LOSS_P95_MS = 350,
    DSD_ANALOG_CTCSS_LOSS_CEILING_MS = 800,
};

/**
 * @brief Shortest pause of a live stream input (stdin, UDP, TCP) that counts as the carrier
 * dropping.
 *
 * A producer that squelches by sending nothing (rtl_fm without `-E pad`, a UDP sender that
 * stops) delivers no block for the sample-time hangover to count. When the next block arrives
 * later than its own duration plus DSD_ANALOG_CARRIER_HANGOVER_MS after the previous one, and
 * at least this long after it, the pause counts as a carrier drop. The floor keeps scheduling
 * delays of a busy or instrumented decoder from ever reading as one.
 */
enum { DSD_ANALOG_STREAM_PAUSE_MIN_MS = 500 };

/**
 * @brief Feed one raw unsynced analog block to the detectors.
 *
 * Called from the symbol path after the raw WAV write and before the voice filters. Reads
 * the block without modifying it. Resets on its own when the RTL stream generation or the
 * trunk-tuning generation moves (a retune it was not told about), discarding that block, when
 * the input rate changes, and when a live stream input paused for longer than
 * DSD_ANALOG_STREAM_PAUSE_MIN_MS allows (then the block is kept as the start of a new
 * reception).
 */
void dsd_analog_rx_tap(const dsd_opts* opts, dsd_state* state, const float* block, unsigned int count);

/**
 * @brief Forget the received tone: clears the publication and every detector's state.
 *
 * Call on retune, scan row or target change, input switch, decode-mode change and stop, so a
 * new channel never inherits the previous channel's tone. Not for the frequent no-carrier
 * cleanup: that runs every few hundred milliseconds in analog mode and would keep a tone from
 * ever locking. Afterwards the publication reads INACTIVE until the tap processes the next
 * block.
 */
void dsd_analog_rx_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_ANALOG_RX_H_ */
