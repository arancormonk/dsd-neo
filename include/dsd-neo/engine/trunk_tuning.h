// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine-owned trunk tuning policy entry points.
 *
 * These functions implement retune policy and bookkeeping and are installed
 * into the runtime trunk tuning hook table during engine startup.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_TUNING_H_
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_TUNING_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

dsd_trunk_tune_result dsd_engine_trunk_tune_to_freq_request(dsd_opts* opts, dsd_state* state, long int freq,
                                                            int ted_sps, uint64_t request_id);
dsd_trunk_tune_result dsd_engine_trunk_tune_to_cc_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                          uint64_t request_id);
dsd_trunk_tune_result dsd_engine_return_to_cc_request(dsd_opts* opts, dsd_state* state, uint64_t request_id);
/**
 * @brief Whether a protocol state machine may move the tuner on its own initiative now: no manual tune of any kind is
 * pending (<dsd-neo/engine/manual_tune.h>), no correlated tune is in flight or awaiting recovery
 * (dsd_trunk_tuning_pending_request()), trunk scan does not own the tuner and -Y does not scan
 * (<dsd-neo/runtime/tuner_owner.h>). The engine installs it as the tuner_free_for_protocol trunk-tuning hook. Decoder
 * thread.
 */
int dsd_engine_tuner_free_for_protocol(const dsd_opts* opts, const dsd_state* state);
/**
 * @brief Mark (non-zero) or unmark the calling thread as the decoder thread, which owns dsd_state::rx_id.
 *
 * An accepted retune the decoder thread makes drops the received identity's access code at once, under the P25 SM tick
 * guard; one made on any other thread (the P25 watchdog's) only requests the drop, which the decoder thread applies.
 * dsd_engine_run_with_lifecycle() marks the thread it runs on for the run.
 */
void dsd_engine_set_decoder_thread(int is_decoder);
/**
 * @brief A rigctl peer confirmed (RPRT 0) a tune to @p freq_hz, and no RTL leg retunes the stream after it.
 *
 * An RTL retune flushes the stream: the decoder reads nothing of the frequency left, and the RTL readers move
 * dsd_state::symbol_clock_epoch. The audio a rigctl peer sends goes on across its retune, so this is that flush for it
 * (issue #834): unless the peer was on @p freq_hz already (@p sent_before_hz, dsd_opts::rigctl_sent_freq_hz before the
 * tune; 0 when none was sent), the epoch moves, so no span the decoders measure on the symbol clock (a DMR slot's MI
 * chain, a P25 Phase 1 terminator's place, the DMR polarity run) reaches across the tune, and the samples the input
 * held at @p mark are dropped at the decoder thread's next input read (dsd_state::input_drop_pending,
 * dsd_symbol_apply_input_drop()); what arrived after it is kept. @p mark is the input's mark the leg took immediately
 * before it sent the frequency, after any passband request (dsd_input_backlog_mark()): the peer is on the old
 * frequency until it takes the frequency, also while it answers a passband request, and it may move and send the new
 * channel's audio before it answers the frequency, which audio came after the mark. Off the decoder thread (the P25 watchdog) the tune posts the mark and
 * a count, and both happen at the decoder thread's next input read (frame_sync_take_unflushed_retunes()), or at the
 * tune notice's reconcile if no read came first (dsd_frame_sync_apply_off_thread_tunes()).
 * Called by each rigctl leg, once its frequency request was answered RPRT 0, never for a request refused or whose reply
 * was lost: the backend tune (dsd_engine_tune_with_backend()), the legacy -Y step and control-channel return, and
 * app-control's io_control leg.
 */
/** dsd_state::dmr_leave_pending: the calls' decryption goes (dmr_forget_call_decryption()), the FID staying. */
#define DSD_DMR_LEAVE_DECRYPTION 1U
/** dsd_state::dmr_leave_pending: their privacy goes too, the FID included (dmr_forget_call_privacy()). */
#define DSD_DMR_LEAVE_PRIVACY    2U

/**
 * @brief Apply what a tune off the decoder thread that left a carrier asked of the DMR calls (dsd_state::
 * dmr_leave_pending): their decryption or privacy goes, and each slot's voice is held (dmr_enc_class_hold()). Decoder
 * thread, under the P25 SM tick guard or where no watchdog tick runs: before each frame, after the synced frame's
 * ticks, at the no-carrier pass's identity step and where a trunk scan snapshots the state.
 *
 * @return 1 when something was pending, 0 otherwise.
 */
int dsd_engine_apply_dmr_leave(dsd_state* state);

void dsd_engine_note_rigctl_retune(const dsd_opts* opts, dsd_state* state, uint32_t sent_before_hz, long int freq_hz,
                                   const dsd_input_cut* mark);
/**
 * @brief Tune a scanner row or target (the -Y channel scan, --trunk-scan).
 *
 * Queues the receive profile the row runs on, bound to @p freq: an analog row (the analog FM monitor in @p opts,
 * issue #526) gets the analog family at its width and no symbol profile; a digital row its symbol profile, and the
 * digital family when the configured mode is digital and the row was timed for that family's landing or the front end
 * runs the analog family or outstanding work lands a family (dsd_engine_scan_retune_attaches_family(), issue #583). A
 * row width the front end refuses at the rate it runs fails the tune before any backend moves.
 */
dsd_trunk_tune_result dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                                   uint64_t* out_request_id);
/** @brief Whether the rigctl leg of the last dsd_engine_scan_tune_to_freq() on this thread moved the radio, whatever
 * that tune returned. With rigctl and an RTL front end in a conventional scan, a tune whose RTL leg then failed, or
 * went on asynchronously (PENDING) and later fails, has the receiver on another carrier though its row never lands.
 * 0 after a tune refused before its rigctl leg moved anything, or one with no rigctl leg. */
int dsd_engine_scan_tune_moved(void);
/** @brief DSP (demodulator) rate of a running RTL-family stream in Hz, which an analog row width must fit: the rate the
 * stream holds an analog request to (rtl_stream_get_request_rate_hz()), published from its start; 0 when no RTL stream
 * runs. */
int dsd_engine_scan_dsp_rate_hz(const dsd_opts* opts, const dsd_state* state);
/** @brief When an RTL-family stream's input came back from its last retune's mute (rtl_stream_input_listening_since_ns()),
 * on the decode clock in seconds: -1 while the input is still muted, 0 when no retune armed a mute or no RTL stream
 * runs. A window timed as listening on a landed frequency (the trunk scan's idle dwell) starts no earlier. */
double dsd_engine_scan_input_listening_since_m(const dsd_opts* opts, const dsd_state* state);
/** @brief The live receive-family requests an RTL-family front end has accepted (rtl_stream_live_family_request_count()),
 * or 0 on any other input. A scanner row's retune queued before this number moved no longer lands the receive family,
 * or the symbol profile, that dsd_engine_scan_tune_to_freq() attached to it (issue #526). */
uint32_t dsd_engine_scan_family_requests(const dsd_opts* opts);
/** @brief The supersedes the stream has counted for retunes queued with no receive family
 * (rtl_stream_familyless_retune_supersedes(): every live analog family request, and every scan leave), or 0 on any
 * other input. A scanner row's retune that attaches no receive family (a typed digital row on an analog session),
 * queued before this number moved, no longer lands the symbol profile dsd_engine_scan_tune_to_freq() queued with it: a
 * live analog request asked for the monitor (issue #582). */
uint32_t dsd_engine_scan_familyless_retune_supersedes(const dsd_opts* opts);
/** @brief Supersede the retunes queued with no receive family so far (rtl_stream_supersede_familyless_retunes()), on an
 * RTL-family input; a no-op on any other. The -Y leave calls it before its own requests (issue #582). */
void dsd_engine_scan_supersede_familyless_retunes(const dsd_opts* opts);
/** @brief Whether dsd_engine_scan_tune_to_freq(), called with the options now in force and @p ted_sps, attaches a
 * receive family to the row's retune: an analog row's always does, and a digital row's with a symbol clock
 * (@p ted_sps > 0) when the configured mode is digital and the row was timed for the digital family's landing
 * (dsd_scan_mode_timed_digital_family(), issue #583), or the front end runs the analog family, or the retunes and
 * requests already outstanding land a family: the analog one, or the digital one, which a retune carrying it lands on
 * the digital family's prediction (rtl_stream_family_landing_after_pending()). That is the decision the tune's
 * preparation makes, and spends; this only reads it. Such a retune is superseded by any live family request made
 * before it lands (dsd_engine_scan_family_requests(), issue #526). One without a family lands its symbol profile
 * whatever the digital requests; only a live analog request, which asks for the monitor, or a scan leave supersedes it
 * (dsd_engine_scan_familyless_retune_supersedes(), issue #582). 0 on any input but an RTL-family one. */
int dsd_engine_scan_retune_attaches_family(const dsd_opts* opts, const dsd_state* state, int ted_sps);
/** @brief Once a scanner has left its rows, put a rigctl peer back (issue #526): a scan tune asks a peer that
 * demodulates audio input for an am row's AM, or an nfm row's own passband, and nothing else undoes that once the
 * scanner stops tuning. RestoreScanModulation() sends back each passband a row changed, as read from the peer before
 * the change, then asks for what the restored settings run (dsd_rigctl_passband_of()): on the analog monitor of audio
 * input its configured width, the AM default, or -B standing in for an unset NFM width, and on a digital mode of audio
 * input -B, each a passband the peer is heard through whose own-passband reading the restore keeps for the session
 * (issue #621, dsd_rigctl_passband_captures()); otherwise FM at -B, which without -B is the peer's own FM passband,
 * sent only when this client changed the peer. Best-effort; a no-op without a live rigctl peer. */
void dsd_engine_scan_rigctl_restore(const dsd_opts* opts, const dsd_state* state);
/** @brief Ask a rigctl peer that demodulates audio input for the demodulator and passband the scan row on air runs now
 * (the one whose scope is in force, not a -Y tune staged since), as its tune's rigctl leg does (a session width edit
 * applied on air, issue #518), but strictly: the configured width, -B or the peer's own passband a row that goes back
 * to the default asks for is a request the peer must take too, where a tune's -B is best-effort. Returns 1 when the
 * peer took it, 0 when it refused, -1 with no such peer (no live rigctl peer, or an input whose audio it does not
 * demodulate: an RTL-family input, whose I/Q DSD-neo demodulates itself, or a symbol or null input;
 * dsd_opts_rigctl_peer_demodulates()). Off a trunk scan with no scan scope the request is the session's, and so is
 * the reading of the peer's own passband taken for it, unless a typed -Y list is configured, whose first row is tuned
 * before its scope is entered (dsd_channel_modes_rigctl_request_is_session(), RigctlMarkSessionPassband()). */
int dsd_engine_scan_rigctl_apply_modulation(const dsd_opts* opts, const dsd_state* state);
/** @brief Ask a rigctl peer that demodulates audio input for the width the session's analog monitor runs (issue #621):
 * its configured NFM or AM width, or the AM default, as dsd_rigctl_passband_of() gives it with no row, through
 * SetScanRowModulation() so a later return to the peer's own passband sends the passband read; off a trunk scan or a
 * typed -Y list that reading is the session's (RigctlMarkSessionPassband()), an untyped -Y list's included, since that
 * list holds no scan scope and has no leave restore. @p state is the decoder state where the caller has one (the
 * engine start and input fallback, the reconnect service), which tells a typed list from an untyped one; NULL counts
 * any configured -Y list as a scan. Call it once the input is open at start, before the P25 watchdog starts (the
 * engine does), whenever a new rigctl connection replaces the one the peer was asked on, and when the engine's
 * fallback to Pulse brings a peer to demodulate the input (a symbol replay ended), under the P25 SM tick guard once the
 * watchdog runs. Best-effort: a refusal is logged.
 * Nothing is asked for -B, the peer's own passband or a session off the monitor, which a session without an explicit
 * width never asked for at start either; a tune asks for them as it always did. Decoder thread. */
void dsd_engine_rigctl_ask_session_passband(const dsd_opts* opts, const dsd_state* state);
/** @brief Forget the frequency the legacy rigctl leg last sent (issue #589): the untyped -Y step and the direct
 * control-channel return skip a repeat of it, which holds only for the connection it went out on. Call it whenever
 * opts->rigctl_sockfd becomes a new connection (the rigctl reconnect service does), since the peer of a new connection,
 * another one or the same one restarted, was sent nothing. Engine start empties it too. The leg's -B is not cached
 * here: the client's record of the peer skips a repeat, and RigctlRebindPeer() hands a reconnect that record (issue
 * #621). */
void dsd_engine_rigctl_tune_cache_forget(void);
/** @brief Forget the frequency a scan confirmed it landed the rigctl peer on (issue #667 D9), which an analog call on
 * audio input is heard on: it describes the connection the tune went out on. Call it whenever opts->rigctl_sockfd
 * becomes a new connection or none (the rigctl reconnect service does, on a reconnect, a new peer and a connect that
 * leaves rigctl off). NULL does nothing. */
void dsd_engine_rigctl_landing_forget(dsd_state* state);
/** @brief Forget the frequencies the legacy legs last sent, rigctl's and the RTL stream's (issue #671): the untyped -Y
 * step and the direct control-channel return skip a repeat of them, which holds only while nothing else moved the
 * receiver. A manual tune moves it without writing them, so every accepted one forgets them: the carrier boundary of a
 * tune the user asked for (DSD_CARRIER_BOUNDARY_TUNE) does, and so must a manual tune that runs no boundary (a channel
 * cycle or return under trunking). A -Y resume forgets them too. Decoder thread. */
void dsd_engine_legacy_tune_caches_forget(void);
/**
 * @brief Release the call state a tuned voice channel owns, without tuning.
 *
 * Ends the canonical call rows with DSD_CALL_END_EXPLICIT, clears the VC frequency mirrors and
 * the crypto/audio gating state, and drops trunk_is_tuned; does not tune.
 */
void dsd_engine_release_tuned_call_state(dsd_opts* opts, dsd_state* state);

/** @brief Where the receiver is, the one reading every judge of a move uses (the engine's tune hooks' leave judge and
 *  the judge of an operator's move that reads no tuner): on a rigctl peer, the frequency the peer last confirmed on
 *  this connection (opts->rigctl_sent_freq_hz, which every rigctl tune leg sets on success; 0, unknown, after a connect
 *  or reconnect before anything was sent, which every judge counts as a leave); otherwise the frequency the RTL stream
 *  last applied, else the decoder's tuner frequency (opts->rtlsdr_center_freq). It makes no rigctl round trip. An
 *  outside retune of the peer (its own knob, another client) is not seen. */
uint32_t dsd_engine_receiver_frequency(const dsd_opts* opts, const dsd_state* state);

/**
 * Retire failed tune requests once no tune owner is active (trunking, trunk scan, signal search, a typed -Y list), so
 * a failure nobody will roll back cannot gate frames (issue #703). An asynchronous RTL retune staged the decoder's
 * tuner frequency when it was issued; when the newest unresolved request is a failure, the radio never left the
 * frequency the stream last applied, so the decoder's tuner frequency is reconciled with it first, and a block decoded
 * afterwards is attributed to the frequency the radio is on. A request still in flight is left alone.
 */
void dsd_engine_retire_inactive_tune_failures(dsd_opts* opts, const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_TUNING_H_ */
