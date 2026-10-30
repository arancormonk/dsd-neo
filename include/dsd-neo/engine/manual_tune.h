// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A manual tune whose RTL wait timed out, followed to its completion (issue #671).
 *
 * io_control_set_freq_tagged() hands back the ID of an RTL tune that timed out: the controller still owns the request
 * and reports its completion later. The decoder notes one such tune at a time, and its loop polls for the completion
 * while one is noted. A late failure puts the cached tuner frequency back on the centre the stream applied, so the
 * decoder never goes on naming a frequency the receiver did not reach. Any newer tune of the receiver, and a stream
 * stop or open, drops the noted tune, so an old completion never overwrites a newer state; the next poll reports the
 * drop, so what the tune was for (an untyped -Y step's row, a resume's) is settled, never left waiting. Decoder thread
 * only.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_MANUAL_TUNE_H
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_MANUAL_TUNE_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a noted manual tune was. */
typedef enum { DSD_MANUAL_TUNE_ENTRY = 1, DSD_MANUAL_TUNE_UNTYPED_STEP = 2 } dsd_manual_tune_kind;

/** dsd_engine_manual_tune_poll() results. */
enum {
    DSD_MANUAL_TUNE_NONE = 0,    /**< Nothing noted, or no completion for it yet. */
    DSD_MANUAL_TUNE_LANDED = 1,  /**< The noted tune landed; the record is gone. */
    DSD_MANUAL_TUNE_FAILED = 2,  /**< The noted tune failed; the record is gone, the cached centre put back. */
    DSD_MANUAL_TUNE_DROPPED = 3, /**< The noted tune was superseded, or its stream session ended; the record is gone. */
};

/**
 * @brief Note the manual tune that timed out with @p id, replacing any tune noted before it.
 *
 * @param state            Decoder state.
 * @param kind             What the tune was.
 * @param id               The ID io_control_set_freq_tagged() handed back; 0 notes nothing (and keeps no older tune).
 * @param requested_hz     The frequency asked for, which a failure names.
 * @param prior_applied_hz The centre in force before the tune, where a failure goes back to when the stream has none.
 * @param row              The scan row the tune enters, -1 for an entry.
 * @param scan_session     The -Y scan session the row belongs to (dsd_state::lcn_scan_session; 0 for an entry).
 * @param map_seq          The channel map revision the row belongs to (dsd_state::trunk_chan_map_seq; 0 for an entry).
 */
void dsd_engine_manual_tune_note_pending(dsd_state* state, dsd_manual_tune_kind kind, uint64_t id,
                                         uint32_t requested_hz, uint32_t prior_applied_hz, int row,
                                         uint32_t scan_session, uint64_t map_seq);

/**
 * @brief Drop the noted tune: another tune of the receiver now decides where the receiver is. The next
 * dsd_engine_manual_tune_poll() reports the drop (DSD_MANUAL_TUNE_DROPPED), unless a newer tune is noted first, which
 * then owns the receiver: an untyped -Y step's drop is still reported then, ahead of the newer tune, since a resume may
 * wait on its row. Nothing when no tune is noted.
 */
void dsd_engine_manual_tune_supersede(dsd_state* state);

/**
 * @brief Take the noted tune's completion, if it has come.
 *
 * Costs a flag read and one atomic load while nothing is noted. A noted record is settled under the P25 SM tick guard
 * (taken here unless the caller holds it), so a late failure never puts back a centre over a newer watchdog tune's. A record whose stream session has ended (rtl_stream_session_generation())
 * is dropped, and so reported, as one superseded is (dsd_engine_manual_tune_supersede()): the cached centre stays, for
 * the next stream to open on or as the newer tune left it. On a failure, @p opts->rtlsdr_center_freq goes back to the
 * centre the stream last applied (or the prior one noted), and for an entry a toast names the frequency that failed.
 * An untyped -Y step's failure is not toasted: the scan retries the row, as after a step that failed in its wait, and
 * the engine's loop settles the row (dsd_engine_scan_untyped_poll()).
 *
 * @return DSD_MANUAL_TUNE_NONE, DSD_MANUAL_TUNE_LANDED, DSD_MANUAL_TUNE_FAILED or DSD_MANUAL_TUNE_DROPPED.
 */
int dsd_engine_manual_tune_poll(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_MANUAL_TUNE_H */
