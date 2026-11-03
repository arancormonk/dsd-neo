// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_ENGINE_ANALOG_CALL_H
#define DSD_NEO_ENGINE_ANALOG_CALL_H
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#ifdef __cplusplus
extern "C" {
#endif
/** How long a reception runs before it is a call (issue #667 D1): its RECEPTION blocks' spans, and before the first
 * of them a lead-in of PENDING blocks, counted in 48 kHz samples. A shorter one leaves no row and no recording. */
#define DSD_ANALOG_CALL_MIN_MS 500

/** A processed conventional analog monitor block (FM or AM), independent of the speaker sink, and what it is to the
 * call (@p block's input, DSD_ANALOG_CALL_* in <dsd-neo/runtime/frame_sync_hooks.h>): a RECEPTION block is the call's
 * audio, and begins a reception only when it heard the carrier itself (@p block's qualified_carrier, issue #668); a
 * HOLDABLE (or PENDING) block holds the call through a pause up to opts->analog_record_max_gap_ms (issue #756), or ends
 * it with that hold off; a BREAK ends it. A block that carries another receiver boundaries epoch than the last block
 * did, call or not, ends any call and is dropped. Every end is stamped at the last RECEPTION block. A reception becomes
 * a call only once it ran DSD_ANALOG_CALL_MIN_MS; until then it is a candidate nothing else sees, and one that ends
 * first is discarded with its recording (issue #667).
 *
 * With per-call recording on and opts->analog_record_preroll_ms > 0, the allowed audio heard before a reception on
 * the FM monitor (the pre-roll, issue #668: clean HOLDABLE blocks only) opens each new recording, and a held call's
 * resumed recording keeps the audio of its pause. Every call end, BREAK, moved epoch and block whose tone was checked
 * drops it. */
void dsd_engine_analog_call_block(dsd_opts* opts, dsd_state* state, const dsd_analog_call_block* block);
/** End the conventional analog call at its last reception (a candidate unpublished), and drop the pre-roll, with a
 * reception running or not. */
void dsd_engine_analog_call_end(dsd_opts* opts, dsd_state* state);
/** Close and publish the conventional analog call's recording now, as its end would (issue #708), keeping the call and
 * its lifecycle epoch: per-call recording was switched off mid-call. A candidate's recording is discarded instead.
 * Every recording transition that commits calls it (a stop, a start, a folder change), so it also drops the pre-roll,
 * with a recording open or not; a transition that fails must not. */
void dsd_engine_analog_call_stop_recording(const dsd_opts* opts, dsd_state* state);
/** opts->analog_record_preroll_ms was applied (issue #668): drop the pre-roll at once, even when the value is the one
 * it had, so a change and its undo inside one command drain still drop it, and size it for the value. Decoder thread,
 * called synchronously by the apply. */
void dsd_engine_analog_call_preroll_changed(const dsd_opts* opts, const dsd_state* state);
void dsd_engine_analog_call_tick(dsd_opts* opts, dsd_state* state);
/** Whether a call is running: a candidate is not one (issue #667). */
int dsd_engine_analog_call_active(const dsd_state* state);
/** Whether a running call or candidate needs the engine's tick now, which ends it: it is past the received tone's stale
 * deadline, or a reset moved the receiver's boundaries epoch since it began (a TCP reconnect, which clears that
 * deadline). A silent hunt read gives way to it (the frame-sync decoder_ticks_due hook). */
int dsd_engine_analog_call_due(const dsd_state* state);
#ifdef DSD_NEO_TEST_HOOKS
/** Run with a minimum of @p ms instead of DSD_ANALOG_CALL_MIN_MS (-1: that again), so a test of what the minimum does
 * not change (the pre-roll's conversion) begins a call on its first RECEPTION block. */
void dsd_engine_analog_call_test_set_min_ms(int ms);
#endif
/** End every call now and commit it: the analog call through its own lifecycle first, so its per-call recording closes
 * under the row it was heard as, then each slot's call with @p reason at @p ended_m. A slot is committed
 * (dsd_event_sync_slot()) when its call ended, or always when @p sync_all. */
void dsd_engine_end_calls(dsd_opts* opts, dsd_state* state, double ended_m, dsd_call_end_reason reason, int sync_all);
#ifdef __cplusplus
}
#endif
#endif
