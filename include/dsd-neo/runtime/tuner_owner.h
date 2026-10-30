// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether a scan owns the tuner now (issue #671): the running predicates of the two scanners.
 *
 * A paused scan keeps its list or coordinator but owns nothing: the operator holds the tuner. What acts for a scan's
 * ownership of the receiver now reads these; what a retained scan forbids still reads the configured flags. They read
 * the option and state fields directly, so this header pulls in the full dsd_opts and dsd_state: it stays apart from
 * the hook tables, and only the readers include it.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TUNER_OWNER_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TUNER_OWNER_H_

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether the --trunk-scan coordinator owns the tuner now: trunk scan is configured
 * (dsd_opts::trunk_scan_enabled) and not paused (dsd_state::trunk_scan_paused, issue #671).
 *
 * The running predicate. A paused scan keeps its coordinator (its targets, snapshots, avoids, session edits and hold),
 * so what a retained scan forbids (a global channel map or band plan, an input switch, a Signal search, -Y) still reads
 * the configured flag; whatever acts for the scan's ownership of the receiver now (its ticks and yields, its tuning
 * ledger, its state machines, its rigctl and receive classing) reads this. The coordinator writes the flag under the
 * P25 SM tick guard, on the decoder thread. A NULL @p state reads as not paused.
 */
static inline int
dsd_trunk_scan_owns_tuner(const dsd_opts* opts, const dsd_state* state) {
    return opts && opts->trunk_scan_enabled == 1 && !(state && state->trunk_scan_paused);
}

/**
 * @brief Whether the -Y scan owns the tuner now: it runs (dsd_opts::scanner_mode), or a resume of it still waits on
 * its row's tune (dsd_state::lcn_scan_resume_pending, issue #671).
 *
 * A resume still pending leaves scanner_mode 0, so no call is named after a row the scan has not reached, yet the row's
 * tune is the scan's: its landing publishes the scan, its failure leaves it paused. Whatever would take the receiver
 * from the scan reads this: a release or a manual tune pauses a pending resume as it pauses a running scan, and the
 * row's commit and its tune's settling treat it as the scan's. A paused scan (dsd_state::lcn_scan_paused alone) owns
 * nothing. Decoder thread. NULL @p opts or @p state read as not owned.
 */
static inline int
dsd_channel_scan_owns_tuner(const dsd_opts* opts, const dsd_state* state) {
    return opts && state && (opts->scanner_mode == 1 || state->lcn_scan_resume_pending);
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TUNER_OWNER_H_ */
